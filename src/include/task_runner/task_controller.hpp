#ifndef TASK_RUNNER_TASK_CONTROLLER_HPP_
#define TASK_RUNNER_TASK_CONTROLLER_HPP_

// task_runner：任务执行/停止编排器（状态机 + 线程 + 周期状态发布）。
//
// 用法：
//   auto ctl = task_controller(std::make_shared<my_job>(), task_mode::sync, 1s);
//   error_code r = ctl.start();                          // 启动 worker + 周期发布器
//   r = ctl.stop([] { /* 停止后处理 */ });               // 阻塞：等真正停止 → join → 后处理
//
// 设计要点：
//   1. 每次 start() 创建一个全新 run_phase（新停止源/worker/发布器），旧 phase 整体替换，
//      任何 join 都发生在 phase 内部，杜绝跨轮次双重 join 与成员写竞态。
//   2. 停止为协作式：置 stop 标记 + 唤醒 + 等待真正停止。业务不感知则框架一直等其结束。
//   3. 防重入：running 中 start→already_running；stopping 中 start/stop→already_stopping。
//   4. 约束（务必遵守）：析构不得与 start()/stop() 并发；回调内不得再调控制器 API。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "task_runner/periodic_publisher.hpp"
#include "task_runner/runnable_task.hpp"
#include "task_runner/stop_token.hpp"

namespace task_runner {

class task_controller {
public:
    // tick_period：状态发布周期（默认 1s）。
    explicit task_controller(std::shared_ptr<runnable_task> task,
                             task_mode mode = task_mode::sync,
                             std::chrono::milliseconds tick_period = std::chrono::seconds(1))
        : task_(std::move(task)), mode_(mode), tick_period_(tick_period) {
        if (!task_) {
            throw std::invalid_argument("task_controller: task must not be null");
        }
    }

    // 强制停止并 join；不触发 on_stopped 回调；不抛异常。
    ~task_controller() {
        std::shared_ptr<run_phase> ph;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            ph = phase_;
        }
        if (!ph) {
            return;
        }
        ph->control->request_stop();
        ph->control->wake_all();
        if (ph->worker.joinable()) {
            ph->worker.join();
        }
        if (ph->publisher) {
            (void)ph->publisher->stop();
        }
    }

    task_controller(const task_controller&) = delete;
    task_controller& operator=(const task_controller&) = delete;
    task_controller(task_controller&&) = delete;
    task_controller& operator=(task_controller&&) = delete;

    // 启动任务：创建新 run_phase 并启动 worker 与周期发布器（均不阻塞调用者）。
    error_code start() {
        std::shared_ptr<run_phase> ph;
        bool spawn_ok = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (state_ == controller_state::running) {
                return error_code::already_running;
            }
            if (state_ == controller_state::stopping) {
                return error_code::already_stopping;
            }
            // 回收上一轮自然结束的 phase（已结束，join 瞬时）。
            if (phase_) {
                if (phase_->worker.joinable()) {
                    phase_->worker.join();
                }
                if (phase_->publisher) {
                    (void)phase_->publisher->stop();
                }
            }
            ph = std::make_shared<run_phase>(task_, mode_);
            phase_ = ph;
            state_ = controller_state::running;
            // 锁内启动线程，使 start 与 stop 原子互斥（短临界区）。
            try {
                ph->worker = std::thread(&task_controller::run_body, this, ph);
                std::weak_ptr<run_phase> weak_ph = ph;
                ph->publisher = std::make_unique<periodic_publisher>(
                    tick_period_, [this, weak_ph] { tick_body(weak_ph); });
                if (ph->publisher->start(ph->control->token()) != error_code::ok) {
                    throw std::runtime_error("start periodic_publisher failed");
                }
                spawn_ok = true;
            } catch (std::exception const&) {
                spawn_ok = false;
            }
        }
        if (!spawn_ok) {
            // 出锁后回滚：请求停止，join 已启动的 worker，恢复 idle。
            std::fprintf(stderr, "[task_runner] start() failed, rolling back\n");
            ph->control->request_stop();
            ph->control->wake_all();
            if (ph->worker.joinable()) {
                ph->worker.join();
            }
            if (ph->publisher) {
                (void)ph->publisher->stop();
            }
            std::lock_guard<std::mutex> lk(mutex_);
            if (phase_ == ph) {
                state_ = controller_state::idle;
                phase_.reset();
            }
            return error_code::resource_error;
        }
        return error_code::ok;
    }

    // 停止任务（阻塞）：请求停止 → 等待任务真正停止（join worker）→ 执行 on_stopped。
    // on_stopped 运行在调用 stop() 的线程上。回调抛异常被捕获，不影响返回值。
    error_code stop(std::function<void()> on_stopped = {}) {
        std::shared_ptr<run_phase> ph;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (state_ == controller_state::stopping) {
                return error_code::already_stopping;
            }
            if (state_ != controller_state::running) {
                return error_code::not_running;
            }
            // 自停止防护：禁止在 worker/tick 线程内调用 stop()（join 自身 → 死锁）。
            const auto tid = std::this_thread::get_id();
            if (phase_->worker.joinable() && phase_->worker.get_id() == tid) {
                return error_code::self_stop_denied;
            }
            if (phase_->publisher && phase_->publisher->thread_id() == tid) {
                return error_code::self_stop_denied;
            }
            state_ = controller_state::stopping;
            ph = phase_;
        }

        ph->control->request_stop();
        ph->control->wake_all();
        if (ph->worker.joinable()) {
            ph->worker.join();  // sync=等业务内容返回；async=等 async_completion() 完成
        }
        if (ph->publisher) {
            (void)ph->publisher->stop();  // worker 已退出，join 瞬时
        }

        {
            std::lock_guard<std::mutex> lk(mutex_);
            state_ = controller_state::idle;
            if (ph->result == run_result::none) {
                ph->result = run_result::stopped;  // 异常兜底：正常由 worker 收尾写入
            }
        }
        try {
            if (on_stopped) {
                on_stopped();
            }
        } catch (std::exception const& e) {
            std::fprintf(stderr, "[task_runner] on_stopped callback threw: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[task_runner] on_stopped callback threw unknown\n");
        }
        return error_code::ok;
    }

    bool running() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return state_ == controller_state::running;
    }

    // 最近一次运行的结果（first-wins）。无运行/未完成时返回 none。
    run_result last_run_result() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return phase_ ? phase_->result : run_result::none;
    }

    // run()/内部任务抛出的首个异常（无则空）。
    std::exception_ptr run_exception() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return phase_ ? phase_->run_exc : nullptr;
    }

    // on_tick 抛出的首个异常（无则空）。
    std::exception_ptr tick_exception() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return phase_ ? phase_->tick_exc : nullptr;
    }

private:
    enum class controller_state { idle, running, stopping };

    // 一次运行的所有资源与结果：单一所有者，避免跨轮次冲突。
    struct run_phase {
        run_phase(std::shared_ptr<runnable_task> t, task_mode m)
            : control(stop_control::create()), task(std::move(t)), mode(m) {}

        std::shared_ptr<stop_control> control;
        std::shared_ptr<runnable_task> task;
        task_mode mode;
        std::thread worker;
        std::unique_ptr<periodic_publisher> publisher;
        run_result result{run_result::none};
        std::exception_ptr run_exc;
        std::exception_ptr tick_exc;
    };

    // worker 线程入口。
    void run_body(std::shared_ptr<run_phase> const& ph) {
        std::exception_ptr exc;
        auto token = ph->control->token();
        try {
            ph->task->run(token);
            if (ph->mode == task_mode::async) {
                // 等业务后台"内部任务"真正完成（协作式：即使收到停止也等它结束）。
                auto fut = ph->task->async_completion();
                if (fut.valid()) {
                    fut.wait();
                }
            }
        } catch (std::exception const& e) {
            std::fprintf(stderr, "[task_runner] task run failed: %s\n", e.what());
            exc = std::current_exception();
        } catch (...) {
            std::fprintf(stderr, "[task_runner] task run failed: unknown\n");
            exc = std::current_exception();
        }
        finalize(ph, std::move(exc));
    }

    // 收尾：写结果（first-wins）→ 自然完成时置 idle → 通知并 join 发布器。
    void finalize(std::shared_ptr<run_phase> const& ph, std::exception_ptr exc) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!ph->run_exc && exc) {
                ph->run_exc = exc;
            }
            const run_result r = ph->run_exc ? run_result::failed
                                 : ph->control->stop_requested() ? run_result::stopped
                                                                 : run_result::completed;
            if (state_ == controller_state::running) {
                state_ = controller_state::idle;  // 自然完成；stopping 时由 stop() 收尾
            }
            if (ph->result == run_result::none) {
                ph->result = r;  // first-wins
            }
        }
        ph->control->request_stop();  // 幂等：通知发布器（及业务 token）本阶段结束
        ph->control->wake_all();
        if (ph->publisher) {
            (void)ph->publisher->stop();  // 由 worker 负责 join 发布器
        }
    }

    // 周期发布器 tick 入口。
    void tick_body(std::weak_ptr<run_phase> const& weak_ph) {
        auto ph = weak_ph.lock();
        if (!ph) {
            return;
        }
        try {
            ph->task->on_tick();
        } catch (std::exception const& e) {
            std::fprintf(stderr, "[task_runner] on_tick threw: %s\n", e.what());
            std::lock_guard<std::mutex> lk(mutex_);
            if (!ph->tick_exc) {
                ph->tick_exc = std::current_exception();
            }
        } catch (...) {
            std::fprintf(stderr, "[task_runner] on_tick threw unknown\n");
            std::lock_guard<std::mutex> lk(mutex_);
            if (!ph->tick_exc) {
                ph->tick_exc = std::current_exception();
            }
        }
    }

    mutable std::mutex mutex_;
    std::shared_ptr<runnable_task> task_;
    task_mode mode_;
    std::chrono::milliseconds tick_period_;
    std::shared_ptr<run_phase> phase_;
    controller_state state_{controller_state::idle};
};

}  // namespace task_runner

#endif  // TASK_RUNNER_TASK_CONTROLLER_HPP_