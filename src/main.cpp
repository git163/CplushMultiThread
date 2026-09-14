// src/main.cpp — 程序入口
// task_runner 框架演示：
//   1) 同步任务：业务内容 + 协作式停止 + 每秒状态发布
//   2) 异步任务：内部线程跑同步内容 + async_completion() 完成句柄
//   3) 自然完成：不调 stop()，任务跑完 on_finished 依然触发
//   4) 防重入：执行/停止各自触发一次错误码分支

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

#include "task_runner/task_controller.hpp"

using namespace std::chrono_literals;

using task_runner::error_code;
using task_runner::runnable_task;
using task_runner::run_result;
using task_runner::stop_token;
using task_runner::task_controller;
using task_runner::task_mode;

static const char* to_str(error_code e) {
    switch (e) {
        case error_code::ok:              return "ok";
        case error_code::already_running:  return "already_running";
        case error_code::already_stopping: return "already_stopping";
        case error_code::not_running:      return "not_running";
        case error_code::self_stop_denied: return "self_stop_denied";
        case error_code::resource_error:   return "resource_error";
    }
    return "?";
}

static const char* to_str(run_result r) {
    switch (r) {
        case run_result::none:      return "none";
        case run_result::completed: return "completed";
        case run_result::stopped:   return "stopped";
        case run_result::failed:    return "failed";
    }
    return "?";
}

// ===== 演示 1：同步任务 =====
class compress_job : public runnable_task {
public:
    void run(stop_token token) override {
        for (int i = 0; i < total_; ++i) {
            if (token.stop_requested()) {  // 业务自行感知停止
                std::cout << "  [sync] stop requested, break at " << i << "/" << total_ << std::endl;
                break;
            }
            std::this_thread::sleep_for(50ms);  // 模拟耗时业务
            done_ = i + 1;
        }
    }

    void on_tick() override {
        std::cout << "  [sync] tick: progress " << done_.load() << "/" << total_ << std::endl;
    }

    // 任务结束回调：三种结束都会触发
    void on_finished(run_result r, std::exception_ptr e) override {
        std::cout << "  [sync] on_finished: result=" << to_str(r)
                  << (e ? " exception" : "") << std::endl;
    }

private:
    static constexpr int total_ = 100;  // 100 * 50ms ≈ 5s，确保 stop 时仍在执行中被中途打断
    std::atomic<int> done_{0};
};

// ===== 演示 2：异步任务（内部线程跑同步内容 + 完成句柄）=====
class download_job : public runnable_task {
public:
    void run(stop_token token) override {
        // 把"同步内容"发到业务自己的内部线程；run 快速返回
        fut_ = std::async(std::launch::async, [this, token] {
            for (int i = 0; i < total_; ++i) {
                if (token.stop_requested()) {
                    std::cout << "  [async] stop requested, break at " << i << "/" << total_ << std::endl;
                    break;
                }
                std::this_thread::sleep_for(50ms);
                done_ = i + 1;
            }
        }).share();
    }

    std::shared_future<void> async_completion() const override { return fut_; }

    void on_tick() override {
        std::cout << "  [async] tick: progress " << done_.load() << "/" << total_ << std::endl;
    }

    void on_finished(run_result r, std::exception_ptr e) override {
        std::cout << "  [async] on_finished: result=" << to_str(r)
                  << (e ? " exception" : "") << std::endl;
    }

private:
    static constexpr int total_ = 80;
    std::shared_future<void> fut_;
    std::atomic<int> done_{0};
};

// ===== 演示 3：自然完成（不调用 stop()，on_finished 依然触发）=====
class quick_job : public runnable_task {
public:
    void run(stop_token) override { std::this_thread::sleep_for(300ms); }

    void on_finished(run_result r, std::exception_ptr) override {
        std::cout << "  [quick] on_finished: result=" << to_str(r)
                  << "  <- 无人调 stop()，任务跑完自动回调" << std::endl;
    }
};

int main() {
    std::cout << "== task_runner demo (sync + async) ==" << std::endl;
    std::cout << std::endl;

    // ---- 同步任务 ----
    std::cout << "[scenario 1] sync task" << std::endl;
    task_controller sync_ctl(std::make_shared<compress_job>(), task_mode::sync, 1s);

    std::cout << "  start():            " << to_str(sync_ctl.start()) << std::endl;
    std::cout << "  start() (re-enter): " << to_str(sync_ctl.start())
              << "  <- 已在运行，防重入" << std::endl;

    std::this_thread::sleep_for(2200ms);  // 让状态发布跑几轮

    std::thread sync_stop([&] {
        auto r = sync_ctl.stop([] {
            std::cout << "  [sync] on_stopped: post-processing done" << std::endl;
        });
        std::cout << "  stop():             " << to_str(r) << std::endl;
    });
    sync_stop.join();

    std::cout << "  last_run_result:    " << to_str(sync_ctl.last_run_result()) << std::endl;
    std::cout << "  stop() after idle:  " << to_str(sync_ctl.stop())
              << "  <- 未在运行" << std::endl;
    std::cout << std::endl;

    // ---- 异步任务 ----
    std::cout << "[scenario 2] async task (internal thread + completion handle)" << std::endl;
    task_controller async_ctl(std::make_shared<download_job>(), task_mode::async, 1s);

    std::cout << "  start():            " << to_str(async_ctl.start()) << std::endl;

    std::this_thread::sleep_for(2200ms);

    std::thread async_stop([&] {
        auto r = async_ctl.stop([] {
            std::cout << "  [async] on_stopped: post-processing done" << std::endl;
        });
        std::cout << "  stop():             " << to_str(r) << std::endl;
    });
    async_stop.join();

    std::cout << "  last_run_result:    " << to_str(async_ctl.last_run_result()) << std::endl;
    std::cout << std::endl;

    // ---- 自然完成 ----
    std::cout << "[scenario 3] natural completion (no stop() call)" << std::endl;
    task_controller quick_ctl(std::make_shared<quick_job>(), task_mode::sync, 1s);

    std::cout << "  start():            " << to_str(quick_ctl.start()) << std::endl;
    for (int i = 0; i < 60 && quick_ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);  // 只等，不调 stop()
    }
    std::cout << "  running():          " << (quick_ctl.running() ? "true" : "false") << std::endl;
    std::cout << "  last_run_result:    " << to_str(quick_ctl.last_run_result()) << std::endl;
    std::cout << "  stop() after idle:  " << to_str(quick_ctl.stop())
              << "  <- 自然完成后无需再 stop" << std::endl;
    std::cout << std::endl;

    std::cout << "== done ==" << std::endl;
    return 0;
}