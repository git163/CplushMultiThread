#ifndef TASK_RUNNER_PERIODIC_PUBLISHER_HPP_
#define TASK_RUNNER_PERIODIC_PUBLISHER_HPP_

// task_runner：通用周期回调执行器（独立可复用）。
// 以固定周期在自身线程上调用 tick；停止由外部 stop_token 驱动（可与任务共用同一
// 停止源，一次 request_stop 同时停住所有组件）。
// 线程所有权归自身（RAII 析构退出并 join）；tick 抛出的异常被捕获，
// 首个异常记录到 tick_exception()，循环继续不崩溃。
//
// 独立用法示例（不经过 task_controller）：
//   auto ctrl   = stop_control::create();
//   periodic_publisher pub(1s, [this]{ ping(); });
//   pub.start(ctrl->token());
//   ...
//   ctrl->request_stop();   // 或 pub.stop();
//   pub.stop();

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

#include "task_runner/runnable_task.hpp"
#include "task_runner/stop_token.hpp"

namespace task_runner {

class periodic_publisher {
public:
    using tick_fn = std::function<void()>;

    explicit periodic_publisher(std::chrono::milliseconds period, tick_fn tick)
        : period_(period), tick_(std::move(tick)) {}

    // 析构时若仍在运行，退出并 join（不抛异常）。
    ~periodic_publisher() { (void)stop(); }

    periodic_publisher(const periodic_publisher&) = delete;
    periodic_publisher& operator=(const periodic_publisher&) = delete;
    periodic_publisher(periodic_publisher&&) = delete;
    periodic_publisher& operator=(periodic_publisher&&) = delete;

    // 启动周期线程。返回 ok / already_running / resource_error。
    error_code start(stop_token token) {
        bool expected = false;
        if (!active_.compare_exchange_strong(expected, true)) {
            return error_code::already_running;
        }
        try {
            std::lock_guard<std::mutex> lk(join_mtx_);
            thread_ = std::thread(&periodic_publisher::loop, this, std::move(token));
        } catch (...) {
            active_.store(false);
            return error_code::resource_error;
        }
        return error_code::ok;
    }

    // 退出并 join（幂等）。未在运行返回 not_running。
    // 外部 token 已停止时立即返回；否则至多阻塞当前这一个周期。
    error_code stop() noexcept {
        std::lock_guard<std::mutex> lk(join_mtx_);
        if (!thread_.joinable()) {
            return error_code::not_running;
        }
        active_.store(false);
        thread_.join();
        return error_code::ok;
    }

    bool running() const noexcept {
        return active_.load();
    }

    // 周期线程 id（用于 self-stop 防护等）。
    std::thread::id thread_id() const noexcept {
        return thread_.get_id();
    }

    // 首个 tick 异常（无则返回空）。
    std::exception_ptr tick_exception() const noexcept {
        std::lock_guard<std::mutex> lk(tick_mtx_);
        return tick_exc_;
    }

private:
    void loop(stop_token token) noexcept {
        while (active_.load() && !token.stop_requested()) {
            if (token.wait_for(period_)) {
                break;  // 停止到达
            }
            if (!active_.load()) {
                break;
            }
            try {
                tick_();
            } catch (...) {
                std::lock_guard<std::mutex> lk(tick_mtx_);
                if (!tick_exc_) {
                    tick_exc_ = std::current_exception();
                }
            }
        }
    }

    std::chrono::milliseconds period_;
    tick_fn tick_;
    std::atomic<bool> active_{false};
    mutable std::mutex tick_mtx_;
    std::exception_ptr tick_exc_;
    std::mutex join_mtx_;
    std::thread thread_;
};

}  // namespace task_runner

#endif  // TASK_RUNNER_PERIODIC_PUBLISHER_HPP_