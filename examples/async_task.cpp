// examples/async_task.cpp
// 场景：任务内容不由你顺序执行 —— 要么是阻塞序列想跑在业务自己的线程，要么是第三方
//       完成后回调你。对应《场景使用指南》§3 异步任务（两种完成句柄来源）。

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

#include "demo_util.hpp"
#include "task_runner/task_controller.hpp"

using namespace std::chrono_literals;

using demo::to_str;
using task_runner::runnable_task;
using task_runner::run_result;
using task_runner::stop_token;
using task_runner::task_controller;
using task_runner::task_mode;

// ===== 来源①：一段阻塞序列，发到业务自己的线程跑 =====
// run() 只发起、快速返回；框架 wait 完成句柄来判定真正完成。
class threaded_job : public runnable_task {
public:
    void run(stop_token token) override {
        fut_ = std::async(std::launch::async, [this, token] {
            for (int i = 0; i < chunks_; ++i) {
                if (token.stop_requested()) {
                    break;
                }
                fetch();
                token.wait_for(120ms);
                chunk_ = i + 1;
            }
        }).share();
    }

    std::shared_future<void> async_completion() const override { return fut_; }

    void on_tick() override {
        std::cout << "  [src-1] tick: " << chunk_.load() << "/" << chunks_ << std::endl;
    }
    void on_finished(run_result r, std::exception_ptr) override {
        std::cout << "  [src-1] on_finished: " << to_str(r) << std::endl;
    }

private:
    static void fetch() { std::this_thread::sleep_for(150ms); }

    static constexpr int chunks_ = 15;
    std::shared_future<void> fut_;
    std::atomic<int> chunk_{0};
};

// ===== 来源②：真·事件驱动（业务自己的异步链，完成回调里置 promise）=====
class event_job : public runnable_task {
public:
    void run(stop_token token) override {
        stopper_ = token;  // 保存 token：停止请求要靠业务自己感知
        // 必须把 future 存下来！若让它作为局部变量在 run() 末尾析构，
        // std::async 的共享状态析构会 join 线程 → run() 反而变成阻塞调用。
        chain_ = std::async(std::launch::async, [this] {
            for (int i = 0; i < steps_; ++i) {
                if (stopper_.stop_requested()) {
                    break;
                }
                std::this_thread::sleep_for(150ms);
                ++step_;
            }
            done_promise_.set_value();  // 业务链的完成回调（只 set 一次）
        });
    }

    std::shared_future<void> async_completion() const override {
        return done_promise_.get_future().share();
    }

    void on_tick() override {
        std::cout << "  [src-2] tick: " << step_.load() << "/" << steps_ << std::endl;
    }
    void on_finished(run_result r, std::exception_ptr) override {
        std::cout << "  [src-2] on_finished: " << to_str(r) << std::endl;
    }

private:
    static constexpr int steps_ = 12;
    stop_token stopper_;
    std::shared_future<void> chain_;              // 持有业务链，见 run() 里的说明
    mutable std::promise<void> done_promise_;     // mutable：async_completion() 是 const
    std::atomic<int> step_{0};
};

int main() {
    std::cout << "== example: async task (two completion-handle sources) ==" << std::endl;

    // ---- 来源①：内部线程跑阻塞内容 ----
    {
        std::cout << "[source 1] std::async + shared_future" << std::endl;
        auto job = std::make_shared<threaded_job>();
        task_controller ctl(job, task_mode::async, 1s);
        std::cout << "start(): " << to_str(ctl.start()) << std::endl;
        std::this_thread::sleep_for(1200ms);
        std::cout << "stop(): " << to_str(ctl.stop()) << std::endl;
        std::cout << "last_run_result: " << to_str(ctl.last_run_result()) << std::endl;
    }

    // ---- 来源②：事件驱动 + promise ----
    {
        std::cout << "\n[source 2] event chain + std::promise" << std::endl;
        auto job = std::make_shared<event_job>();
        task_controller ctl(job, task_mode::async, 1s);
        std::cout << "start(): " << to_str(ctl.start()) << std::endl;
        std::this_thread::sleep_for(1200ms);
        std::cout << "stop(): " << to_str(ctl.stop()) << std::endl;
        std::cout << "last_run_result: " << to_str(ctl.last_run_result()) << std::endl;
    }

    return 0;
}
