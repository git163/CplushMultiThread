// examples/sync_task.cpp
// 场景：界面「执行」/「停止」两个按钮，任务是一段可中断的同步耗时循环。
// 对应《场景使用指南》§1 最小可用、§6 可中断写法、§12 错误码处理。

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

#include "demo_util.hpp"
#include "task_runner/task_controller.hpp"

using namespace std::chrono_literals;

using demo::to_str;
using task_runner::error_code;
using task_runner::runnable_task;
using task_runner::run_result;
using task_runner::stop_token;
using task_runner::task_controller;
using task_runner::task_mode;

// 业务任务：只填耗时内容；按自己的粒度检查停止标记。
class compress_job : public runnable_task {
public:
    void run(stop_token token) override {
        for (int i = 0; i < files_; ++i) {
            if (token.stop_requested()) {  // 检查点：停止响应延迟 ≈ 一步的耗时
                std::cout << "  [sync] stop requested at file " << i << "/" << files_ << std::endl;
                break;
            }
            compress_one();
            token.wait_for(100ms);  // 睡眠 + 可被停止立刻唤醒（不要用 sleep_for）
            done_ = i + 1;
        }
    }

    // 每 tick_period（这里 1s）在独立线程被调用：推当前进度。
    void on_tick() override {
        std::cout << "  [sync] tick: " << done_.load() << "/" << files_ << std::endl;
    }

    // 任务结束（三种结果都触发，含自然完成）。
    void on_finished(run_result r, std::exception_ptr e) override {
        std::cout << "  [sync] on_finished: " << to_str(r) << (e ? " (exception)" : "") << std::endl;
    }

private:
    static void compress_one() { std::this_thread::sleep_for(150ms); }

    static constexpr int files_ = 20;
    std::atomic<int> done_{0};  // 跨线程共享（run 写 / on_tick 读）→ 必须原子
};

int main() {
    std::cout << "== example: sync task (run / stop buttons) ==" << std::endl;

    // 装配一次，长期持有（放局部作用域里，函数返回时析构会立刻打断任务）。
    auto job = std::make_shared<compress_job>();
    task_controller ctl(job, task_mode::sync, 1s);

    // ---- 「执行」按钮 ----
    std::cout << "start(): " << to_str(ctl.start()) << std::endl;
    std::cout << "start() again: " << to_str(ctl.start()) << "  <- 防重入" << std::endl;

    std::this_thread::sleep_for(1200ms);  // 让状态发布跑几轮

    // ---- 「停止」按钮 ----
    // stop() 是阻塞调用（要等任务真正停下来），放到独立线程避免卡住界面。
    std::thread stopper([&ctl] {
        error_code r = ctl.stop([] { std::cout << "  [sync] on_stopped: post-processing" << std::endl; });
        std::cout << "stop(): " << to_str(r) << std::endl;
    });
    stopper.join();

    std::cout << "last_run_result: " << to_str(ctl.last_run_result()) << std::endl;

    // 任务已结束，再 stop() 是正常情况（不是错误）：去查 last_run_result()。
    std::cout << "stop() when idle: " << to_str(ctl.stop()) << "  <- 任务已结束" << std::endl;
    return 0;
}
