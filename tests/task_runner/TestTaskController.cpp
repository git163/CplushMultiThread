// tests/task_runner/TestTaskController.cpp
// 覆盖矩阵 SC-09~28：状态机 / 防重入 / 协作停止 / 自然完成 / 异常 / 周期发布 / 析构 / async 完成句柄 / 复用。
// 覆盖矩阵 SC-30~38：任务结束回调 on_finished（触发范围 / 线程 / 结果一致性 / 异常隔离）。

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "task_runner/task_controller.hpp"

using namespace std::chrono_literals;

using task_runner::error_code;
using task_runner::runnable_task;
using task_runner::run_result;
using task_runner::stop_token;
using task_runner::task_controller;
using task_runner::task_mode;

// ===================== 测试用任务 =====================

// 可协作停止的同步任务：循环 steps 次，每步 sleep，检查 token。
class sync_loop_job : public runnable_task {
public:
    sync_loop_job(int steps, std::chrono::milliseconds step) : steps_(steps), step_(step) {}

    void run(stop_token token) override {
        run_tid_ = std::this_thread::get_id();
        for (int i = 0; i < steps_; ++i) {
            if (token.stop_requested()) {
                stopped_at_ = i;
                break;
            }
            std::this_thread::sleep_for(step_);
            ++progress_;
        }
        run_returned_ = true;
    }

    void on_tick() override { ++ticks_; }

    // 记录 on_finished 触发情况（结果 / 异常 / 线程 id），供 SC-30~38 断言。
    void on_finished(run_result r, std::exception_ptr e) override {
        finish_result_ = r;
        finish_has_error_ = (e != nullptr);
        finish_tid_ = std::this_thread::get_id();
        finish_count_.fetch_add(1);
    }

    std::atomic<bool> run_returned_{false};
    std::atomic<int> progress_{0};
    std::atomic<int> stopped_at_{-1};
    std::atomic<int> ticks_{0};

    std::atomic<int> finish_count_{0};
    std::atomic<run_result> finish_result_{run_result::none};
    std::atomic<bool> finish_has_error_{false};
    std::atomic<std::thread::id> run_tid_{};
    std::atomic<std::thread::id> finish_tid_{};

private:
    int steps_;
    std::chrono::milliseconds step_;
};

// 不感知停止：run 固定睡 sleep_ms，stop 只能等它自然返回。
class sleep_job : public runnable_task {
public:
    explicit sleep_job(std::chrono::milliseconds ms) : ms_(ms) {}

    void run(stop_token) override { std::this_thread::sleep_for(ms_); }

private:
    std::chrono::milliseconds ms_;
};

// run 直接抛异常。
class throwing_job : public runnable_task {
public:
    void run(stop_token) override { throw std::runtime_error("boom"); }

    void on_finished(run_result r, std::exception_ptr e) override {
        finish_result_ = r;
        finish_has_error_ = (e != nullptr);
        finish_count_.fetch_add(1);
    }

    std::atomic<int> finish_count_{0};
    std::atomic<run_result> finish_result_{run_result::none};
    std::atomic<bool> finish_has_error_{false};
};

// on_finished 里抛异常（验证被捕获记录、不影响 run_result、控制器仍可复用）。
class finish_throw_job : public runnable_task {
public:
    void run(stop_token) override { std::this_thread::sleep_for(30ms); }

    void on_finished(run_result, std::exception_ptr) override {
        finish_count_.fetch_add(1);
        throw std::runtime_error("on_finished boom");
    }

    std::atomic<int> finish_count_{0};
};

// 收到停止后才抛异常（验证异常优先于 stopped）。
class stop_then_throw_job : public runnable_task {
public:
    void run(stop_token token) override {
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(2ms);
        }
        throw std::runtime_error("boom-after-stop");
    }
};

// on_tick 抛异常（run 不应中断）。
class tick_throw_job : public runnable_task {
public:
    void run(stop_token token) override {
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(2ms);
        }
    }
    void on_tick() override { throw std::runtime_error("tick boom"); }
};

// 任务内主动调 stop()（验证 self_stop_denied）。
class self_stop_job : public runnable_task {
public:
    std::function<void()> on_start;

    void run(stop_token token) override {
        if (on_start) {
            on_start();
        }
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(2ms);
        }
    }
};

// on_tick 内主动调 stop()（验证 self_stop_denied）。
class self_stop_tick_job : public runnable_task {
public:
    std::function<void()> on_tick_fn;

    void run(stop_token token) override {
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(2ms);
        }
    }
    void on_tick() override {
        if (on_tick_fn) {
            on_tick_fn();
        }
    }
};

// 异步任务：内部线程跑同步内容 + shared_future 完成句柄。
class async_loop_job : public runnable_task {
public:
    async_loop_job(int steps, std::chrono::milliseconds step) : steps_(steps), step_(step) {}

    void run(stop_token token) override {
        fut_ = std::async(std::launch::async, [this, token] {
            for (int i = 0; i < steps_; ++i) {
                if (token.stop_requested()) {
                    stopped_at_ = i;
                    break;
                }
                std::this_thread::sleep_for(step_);
                ++progress_;
            }
        }).share();
    }

    std::shared_future<void> async_completion() const override { return fut_; }

    void on_finished(run_result r, std::exception_ptr e) override {
        finish_result_ = r;
        finish_has_error_ = (e != nullptr);
        finish_count_.fetch_add(1);
    }

    std::atomic<int> progress_{0};
    std::atomic<int> stopped_at_{-1};
    std::atomic<int> finish_count_{0};
    std::atomic<run_result> finish_result_{run_result::none};
    std::atomic<bool> finish_has_error_{false};

private:
    int steps_;
    std::chrono::milliseconds step_;
    std::shared_future<void> fut_;
};

// 异步任务：完成信号来自外部 promise（模拟要外部释放才能完成）。
class async_blocked_job : public runnable_task {
public:
    void run(stop_token token) override {
        stopper_ = token;  // 保存但不主动查询
    }

    std::shared_future<void> async_completion() const override {
        return done_promise_.get_future().share();
    }

    void on_finished(run_result r, std::exception_ptr e) override {
        finish_result_ = r;
        finish_has_error_ = (e != nullptr);
        finish_count_.fetch_add(1);
    }

    mutable std::promise<void> done_promise_;
    stop_token stopper_;
    std::atomic<int> finish_count_{0};
    std::atomic<run_result> finish_result_{run_result::none};
    std::atomic<bool> finish_has_error_{false};
};

// 记住上一轮 token，验证旧 token 不影响新一轮。
class token_keep_job : public runnable_task {
public:
    void run(stop_token token) override {
        kept_ = token;
        int n = 0;
        while (!token.stop_requested() && n < 100000) {
            std::this_thread::sleep_for(5ms);
            ++n;
        }
    }

    stop_token kept_;
};

// ===================== 测试 =====================

TEST(TaskController, StartOk) {  // SC-09
    auto job = std::make_shared<sync_loop_job>(5, 10ms);
    task_controller ctl(job, task_mode::sync, 40ms);
    EXPECT_EQ(ctl.start(), error_code::ok);
    EXPECT_TRUE(ctl.running());
    EXPECT_EQ(ctl.stop(), error_code::ok);
    EXPECT_FALSE(ctl.running());
}

TEST(TaskController, StartWhileRunningRejected) {  // SC-10
    auto job = std::make_shared<sync_loop_job>(1000, 20ms);
    task_controller ctl(job);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(ctl.start(), error_code::already_running);
    EXPECT_TRUE(ctl.running());
    EXPECT_EQ(ctl.stop(), error_code::ok);
}

TEST(TaskController, StartWhileStoppingRejected) {  // SC-11
    auto job = std::make_shared<sleep_job>(400ms);
    task_controller ctl(job);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(40ms);
    std::thread st([&] { (void)ctl.stop(); });
    std::this_thread::sleep_for(30ms);  // st 已进入 stopping（stop 在阻塞中等 run 返回）
    EXPECT_EQ(ctl.start(), error_code::already_stopping);
    st.join();
}

TEST(TaskController, SyncCoopStop) {  // SC-12
    auto job = std::make_shared<sync_loop_job>(1000, 20ms);
    std::atomic<bool> on_stopped_called{false};
    std::atomic<bool> saw_run_returned{false};
    task_controller ctl(job, task_mode::sync, 50ms);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(60ms);

    auto r = ctl.stop([&] {
        on_stopped_called = true;
        saw_run_returned = job->run_returned_.load();
    });
    EXPECT_EQ(r, error_code::ok);
    EXPECT_TRUE(on_stopped_called);
    EXPECT_TRUE(saw_run_returned);  // on_stopped 在 run 返回后执行
    EXPECT_FALSE(ctl.running());
    EXPECT_EQ(ctl.last_run_result(), run_result::stopped);
    EXPECT_NE(job->stopped_at_.load(), -1);  // 业务感知停止提前退出
}

TEST(TaskController, SecondStopRejected) {  // SC-13
    auto job = std::make_shared<sleep_job>(400ms);
    std::atomic<int> on_stopped_count{0};
    task_controller ctl(job);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(40ms);

    std::atomic<error_code> first{error_code::ok};
    std::atomic<error_code> second{error_code::ok};
    std::thread t1([&] { first = ctl.stop([&] { ++on_stopped_count; }); });
    std::this_thread::sleep_for(60ms);  // t1 正在 stopping 中阻塞
    second = ctl.stop([&] { ++on_stopped_count; });
    t1.join();

    EXPECT_EQ(first, error_code::ok);
    EXPECT_EQ(second, error_code::already_stopping);
    EXPECT_EQ(on_stopped_count.load(), 1);  // 仅首次 stop 执行回调
}

TEST(TaskController, StopWhenIdle) {  // SC-14
    auto job = std::make_shared<sync_loop_job>(1, 5ms);
    task_controller ctl(job);
    std::atomic<bool> called{false};
    EXPECT_EQ(ctl.stop([&] { called = true; }), error_code::not_running);
    EXPECT_FALSE(called);
}

TEST(TaskController, SelfStopFromTaskRejected) {  // SC-15
    auto job = std::make_shared<self_stop_job>();
    task_controller ctl(job);
    std::atomic<error_code> inner{error_code::ok};
    job->on_start = [&] { inner = ctl.stop(); };
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(ctl.stop(), error_code::ok);
    EXPECT_EQ(inner, error_code::self_stop_denied);  // 任务内 stop 被拒绝，未 join 自身
}

TEST(TaskController, SelfStopFromTickRejected) {  // SC-16
    auto job = std::make_shared<self_stop_tick_job>();
    task_controller ctl(job, task_mode::sync, 10ms);
    std::atomic<error_code> inner{error_code::ok};
    std::atomic<int> denied{0};
    job->on_tick_fn = [&] {
        inner = ctl.stop();
        if (inner == error_code::self_stop_denied) {
            ++denied;
        }
    };
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(80ms);  // 若干 tick 发生

    EXPECT_GE(denied.load(), 1);
    EXPECT_EQ(ctl.stop(), error_code::ok);
    EXPECT_EQ(inner, error_code::self_stop_denied);
}

TEST(TaskController, SyncNaturalComplete) {  // SC-17
    auto job = std::make_shared<sync_loop_job>(5, 15ms);  // ~75ms 自然完成
    task_controller ctl(job, task_mode::sync, 30ms);
    EXPECT_EQ(ctl.start(), error_code::ok);

    for (int i = 0; i < 50 && ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_FALSE(ctl.running());
    EXPECT_EQ(ctl.last_run_result(), run_result::completed);

    std::atomic<bool> called{false};
    EXPECT_EQ(ctl.stop([&] { called = true; }), error_code::not_running);
    EXPECT_FALSE(called);
}

TEST(TaskController, SyncIgnoreStopWaits) {  // SC-18 业务不感知停止，框架一直等
    auto job = std::make_shared<sleep_job>(300ms);
    std::atomic<int> called{0};
    task_controller ctl(job);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(40ms);

    auto t0 = std::chrono::steady_clock::now();
    auto r = ctl.stop([&] { ++called; });
    auto dur = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(r, error_code::ok);
    EXPECT_GE(dur, 250ms);  // stop 一直等到业务内容跑完
    EXPECT_EQ(called.load(), 1);
    EXPECT_EQ(ctl.last_run_result(), run_result::stopped);  // 收到过停止请求
}

TEST(TaskController, RunExceptionFailed) {  // SC-19
    auto job = std::make_shared<throwing_job>();
    task_controller ctl(job);
    EXPECT_EQ(ctl.start(), error_code::ok);

    for (int i = 0; i < 50 && ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_FALSE(ctl.running());
    EXPECT_EQ(ctl.last_run_result(), run_result::failed);
    EXPECT_NE(ctl.run_exception(), nullptr);
    EXPECT_EQ(ctl.stop(), error_code::not_running);  // 已自然结束（异常）
}

TEST(TaskController, RunExceptionWinsOverStop) {  // SC-20
    auto job = std::make_shared<stop_then_throw_job>();
    task_controller ctl(job);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(40ms);

    EXPECT_EQ(ctl.stop(), error_code::ok);
    EXPECT_EQ(ctl.last_run_result(), run_result::failed);  // 异常优先于 stopped
    EXPECT_NE(ctl.run_exception(), nullptr);
}

TEST(TaskController, TickAtPeriod) {  // SC-21
    auto job = std::make_shared<sync_loop_job>(500, 10ms);
    task_controller ctl(job, task_mode::sync, 30ms);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(250ms);

    EXPECT_GE(job->ticks_.load(), 5);  // 30ms 周期 → 期望 ~8 次，留余量
    EXPECT_EQ(ctl.stop(), error_code::ok);
}

TEST(TaskController, TickExceptionKeepsRun) {  // SC-22
    auto job = std::make_shared<tick_throw_job>();
    task_controller ctl(job, task_mode::sync, 20ms);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(100ms);

    EXPECT_TRUE(ctl.running());  // on_tick 异常不中断 run
    EXPECT_NE(ctl.tick_exception(), nullptr);
    EXPECT_EQ(ctl.stop(), error_code::ok);
}

TEST(TaskController, DestructorAutoStop) {  // SC-23 析构强制停止，不触发 on_stopped
    auto job = std::make_shared<sync_loop_job>(100000, 5ms);
    {
        task_controller ctl(job);
        EXPECT_EQ(ctl.start(), error_code::ok);
        std::this_thread::sleep_for(40ms);
        // 析构：自动 request_stop + join
    }
    EXPECT_NE(job->stopped_at_.load(), -1);     // 任务感知 token 提前退出
    EXPECT_LT(job->progress_.load(), 100000);   // 未跑完全部步骤
}

TEST(TaskController, DestructorIdleSafe) {  // SC-24
    auto job = std::make_shared<sync_loop_job>(1, 5ms);
    {
        task_controller ctl(job);  // 未 start，析构安全
    }
    SUCCEED();
}

TEST(TaskController, AsyncCompletionFuture) {  // SC-25
    auto job = std::make_shared<async_loop_job>(5, 15ms);  // ~75ms 自然完成
    task_controller ctl(job, task_mode::async, 30ms);
    EXPECT_EQ(ctl.start(), error_code::ok);

    for (int i = 0; i < 50 && ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_FALSE(ctl.running());
    EXPECT_EQ(ctl.last_run_result(), run_result::completed);
}

TEST(TaskController, AsyncStopWaitsFuture) {  // SC-26 内部线程感知停止，框架 wait 完成句柄
    auto job = std::make_shared<async_loop_job>(1000, 30ms);
    task_controller ctl(job, task_mode::async, 50ms);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(60ms);

    std::atomic<bool> called{false};
    auto r = ctl.stop([&] { called = true; });
    EXPECT_EQ(r, error_code::ok);
    EXPECT_TRUE(called);
    EXPECT_EQ(ctl.last_run_result(), run_result::stopped);
    EXPECT_NE(job->stopped_at_.load(), -1);  // 内部线程感知停止提前结束
}

TEST(TaskController, AsyncUncooperativeKeepsWaiting) {  // SC-27 业务不配合：框架持续等待完成信号
    auto job = std::make_shared<async_blocked_job>();
    task_controller ctl(job, task_mode::async, 30ms);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(40ms);

    std::atomic<bool> stop_returned{false};
    std::atomic<error_code> stop_r{error_code::ok};
    std::thread st([&] {
        stop_r = ctl.stop();
        stop_returned = true;
    });
    std::this_thread::sleep_for(120ms);  // 停止已请求，但未来完成 → 不应提前结束
    EXPECT_FALSE(stop_returned.load());
    EXPECT_EQ(ctl.last_run_result(), run_result::none);  // 未提前置结果

    job->done_promise_.set_value();  // 释放完成信号
    st.join();
    EXPECT_TRUE(stop_returned.load());
    EXPECT_EQ(stop_r, error_code::ok);
    EXPECT_EQ(ctl.last_run_result(), run_result::stopped);
}

TEST(TaskController, MultiCycleReuse) {  // SC-28 多轮 start/stop 复用
    auto job = std::make_shared<sync_loop_job>(1000, 10ms);
    task_controller ctl(job);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(ctl.start(), error_code::ok);
        EXPECT_TRUE(ctl.running());
        std::this_thread::sleep_for(30ms);
        EXPECT_EQ(ctl.stop(), error_code::ok);
        EXPECT_FALSE(ctl.running());
    }
}

TEST(TaskController, StaleTokenDoesNotAffectNext) {  // SC-28 上一轮过期 token 不影响新一轮
    auto job = std::make_shared<token_keep_job>();
    task_controller ctl(job);

    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(ctl.stop(), error_code::ok);
    EXPECT_TRUE(job->kept_.stop_requested());  // 第一轮 token 已停止

    // 第二轮：新 run_phase + 新停止源，旧 token 失效不影响
    EXPECT_EQ(ctl.start(), error_code::ok);
    EXPECT_TRUE(ctl.running());
    std::this_thread::sleep_for(40ms);
    EXPECT_TRUE(ctl.running());  // 新 token 未停止，仍在运行
    EXPECT_EQ(ctl.stop(), error_code::ok);
}

// ===================== 任务结束回调 on_finished（SC-30~38）=====================

TEST(TaskController, FinishFiredOnNaturalComplete) {  // SC-30 自然完成也触发
    auto job = std::make_shared<sync_loop_job>(5, 15ms);  // ~75ms 自然完成
    task_controller ctl(job, task_mode::sync, 30ms);
    EXPECT_EQ(ctl.start(), error_code::ok);

    for (int i = 0; i < 50 && ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    // running()==false 蕴含 on_finished 已返回（置 idle 是 worker 收尾的最后一步）
    EXPECT_FALSE(ctl.running());
    EXPECT_EQ(job->finish_count_.load(), 1);
    EXPECT_EQ(job->finish_result_.load(), run_result::completed);
    EXPECT_FALSE(job->finish_has_error_.load());
    EXPECT_EQ(ctl.last_run_result(), run_result::completed);
}

TEST(TaskController, FinishFiredOnCoopStop) {  // SC-31 停止路径触发，且在 on_stopped 之前
    auto job = std::make_shared<sync_loop_job>(1000, 20ms);
    task_controller ctl(job, task_mode::sync, 50ms);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(60ms);

    std::atomic<bool> finish_done_before_on_stopped{false};
    auto r = ctl.stop([&] {
        // stop() 返回前 worker 已收尾 → 此刻回调必已执行完
        finish_done_before_on_stopped = (job->finish_count_.load() == 1);
    });
    EXPECT_EQ(r, error_code::ok);
    EXPECT_TRUE(finish_done_before_on_stopped);
    EXPECT_EQ(job->finish_count_.load(), 1);
    EXPECT_EQ(job->finish_result_.load(), run_result::stopped);
    EXPECT_EQ(ctl.last_run_result(), run_result::stopped);  // 与回调看到的一致
}

TEST(TaskController, FinishFiredOnFailure) {  // SC-32 抛异常 → failed + 异常传给回调
    auto job = std::make_shared<throwing_job>();
    task_controller ctl(job);
    EXPECT_EQ(ctl.start(), error_code::ok);

    for (int i = 0; i < 50 && ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(job->finish_count_.load(), 1);
    EXPECT_EQ(job->finish_result_.load(), run_result::failed);
    EXPECT_TRUE(job->finish_has_error_.load());  // error 非空
    EXPECT_EQ(ctl.last_run_result(), run_result::failed);
    EXPECT_NE(ctl.run_exception(), nullptr);
}

TEST(TaskController, FinishRunsOnWorkerThread) {  // SC-33 跑在 worker 线程，非调用方线程
    auto job = std::make_shared<sync_loop_job>(5, 10ms);
    task_controller ctl(job);
    EXPECT_EQ(ctl.start(), error_code::ok);

    for (int i = 0; i < 50 && ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(job->finish_count_.load(), 1);
    EXPECT_EQ(job->finish_tid_.load(), job->run_tid_.load());  // 与 run() 同一线程
    EXPECT_NE(job->finish_tid_.load(), std::this_thread::get_id());
}

TEST(TaskController, FinishFiredOnAsyncPath) {  // SC-34 async 路径同样触发
    // ① 完成句柄自然 ready
    auto job = std::make_shared<async_loop_job>(5, 15ms);
    task_controller ctl(job, task_mode::async, 30ms);
    EXPECT_EQ(ctl.start(), error_code::ok);
    for (int i = 0; i < 50 && ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(job->finish_count_.load(), 1);
    EXPECT_EQ(job->finish_result_.load(), run_result::completed);

    // ② 完成句柄迟迟不 ready：释放前不触发，释放后恰好触发一次
    auto bjob = std::make_shared<async_blocked_job>();
    task_controller bctl(bjob, task_mode::async, 30ms);
    EXPECT_EQ(bctl.start(), error_code::ok);
    std::this_thread::sleep_for(40ms);

    std::thread st([&] { (void)bctl.stop(); });
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(bjob->finish_count_.load(), 0);  // future 未完成 → 不能提前触发
    bjob->done_promise_.set_value();
    st.join();
    EXPECT_EQ(bjob->finish_count_.load(), 1);
    EXPECT_EQ(bjob->finish_result_.load(), run_result::stopped);
}

TEST(TaskController, FinishPerCycleExactlyOnce) {  // SC-35 多轮复用：每轮恰好一次
    auto job = std::make_shared<sync_loop_job>(1000, 10ms);
    task_controller ctl(job);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(ctl.start(), error_code::ok);
        std::this_thread::sleep_for(30ms);
        EXPECT_EQ(ctl.stop(), error_code::ok);
        EXPECT_EQ(job->finish_count_.load(), i + 1);  // 每轮 +1，不多不少
    }
}

TEST(TaskController, FinishNotFiredWhenStartRejectedOrIdleStop) {  // SC-36 未真正运行 → 不触发
    auto job = std::make_shared<sync_loop_job>(1000, 20ms);
    task_controller ctl(job);

    EXPECT_EQ(ctl.stop(), error_code::not_running);  // 空闲 stop
    EXPECT_EQ(job->finish_count_.load(), 0);

    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(ctl.start(), error_code::already_running);  // 被拒的 start
    EXPECT_EQ(job->finish_count_.load(), 0);              // 运行中，尚未结束

    EXPECT_EQ(ctl.stop(), error_code::ok);
    EXPECT_EQ(job->finish_count_.load(), 1);
}

TEST(TaskController, FinishExceptionRecordedAndIsolated) {  // SC-37 回调抛异常被隔离
    auto job = std::make_shared<finish_throw_job>();
    task_controller ctl(job);
    EXPECT_EQ(ctl.start(), error_code::ok);

    for (int i = 0; i < 50 && ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(job->finish_count_.load(), 1);
    EXPECT_EQ(ctl.last_run_result(), run_result::completed);  // 回调异常不影响结果
    EXPECT_EQ(ctl.run_exception(), nullptr);
    EXPECT_NE(ctl.finish_exception(), nullptr);  // 记入 finish_exception

    // 控制器仍可复用（回调再抛一次也不影响）
    EXPECT_EQ(ctl.start(), error_code::ok);
    for (int i = 0; i < 50 && ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(job->finish_count_.load(), 2);
    EXPECT_NE(ctl.finish_exception(), nullptr);
}

TEST(TaskController, FinishFiredOnDestructor) {  // SC-38 析构触发的停止也回调
    auto job = std::make_shared<sync_loop_job>(100000, 5ms);
    {
        task_controller ctl(job);
        EXPECT_EQ(ctl.start(), error_code::ok);
        std::this_thread::sleep_for(40ms);
        // 析构：request_stop + join（join 保证 on_finished 已执行完）
    }
    EXPECT_EQ(job->finish_count_.load(), 1);
    EXPECT_EQ(job->finish_result_.load(), run_result::stopped);
}