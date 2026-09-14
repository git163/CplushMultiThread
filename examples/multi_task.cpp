// examples/multi_task.cpp
// 场景：同时跑多个任务 / 全局只允许跑一个。
// 对应《场景使用指南》§8 多任务并发、§9 全局互斥。

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
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

// 一份可并行的任务逻辑（状态都在各自对象里）。
class download_job : public runnable_task {
public:
    explicit download_job(std::string name, int steps) : name_(std::move(name)), steps_(steps) {}

    void run(stop_token token) override {
        for (int i = 0; i < steps_; ++i) {
            if (token.stop_requested()) {
                break;
            }
            std::this_thread::sleep_for(80ms);
            done_ = i + 1;
        }
    }

    void on_finished(run_result r, std::exception_ptr e) override {
        std::cout << "  [" << name_ << "] on_finished: " << to_str(r)
                  << (e ? " (exception)" : "") << "  done=" << done_.load() << "/" << steps_ << std::endl;
    }

private:
    std::string name_;
    int steps_;
    std::atomic<int> done_{0};
};

int main() {
    std::cout << "== example: multiple tasks vs global mutual exclusion ==" << std::endl;

    // ---- ① 并发跑多个任务：多个控制器 + 各自的 task 实例 ----
    std::cout << "[part 1] concurrent tasks (one controller per instance)" << std::endl;
    {
        // 同一个类，不同对象 → 逻辑共享、状态不共享，天然安全。
        auto job_a = std::make_shared<download_job>("A", 10);
        auto job_b = std::make_shared<download_job>("B", 6);

        task_controller ctl_a(job_a, task_mode::sync, 1s);
        task_controller ctl_b(job_b, task_mode::sync, 1s);
        std::cout << "start A: " << to_str(ctl_a.start()) << ", start B: " << to_str(ctl_b.start())
                  << "  <- 互不影响" << std::endl;

        // 等 B 自然跑完，再停 A。
        while (ctl_b.running()) {
            std::this_thread::sleep_for(50ms);
        }
        std::cout << "B last_run_result: " << to_str(ctl_b.last_run_result()) << std::endl;
        std::cout << "stop A: " << to_str(ctl_a.stop()) << std::endl;
        std::cout << "A last_run_result: " << to_str(ctl_a.last_run_result()) << std::endl;
    }

    // ---- ② 全局互斥：只用一个控制器，多轮复用 ----
    std::cout << "\n[part 2] global mutual exclusion (single controller, reused)" << std::endl;
    {
        auto job = std::make_shared<download_job>("only", 1000);  // 长任务，需要 stop 才结束
        task_controller ctl(job, task_mode::sync, 1s);

        for (int round = 0; round < 3; ++round) {
            // 「执行」按钮：返回值本身就是互斥的答案，不需要额外的锁。
            error_code e = ctl.start();
            std::cout << "round " << round << " start(): " << to_str(e);
            if (e == error_code::already_running) {
                std::cout << "  <- 已在运行";
            }
            std::cout << std::endl;

            std::this_thread::sleep_for(150ms);

            // 「停止」按钮
            std::cout << "round " << round << " stop():  " << to_str(ctl.stop()) << std::endl;
        }
    }

    // 注意：把同一个 task 对象交给两个控制器是**业务侧数据竞争**，
    // 框架的防重入是控制器粒度的，拦不住。分型与对策见
    // docs/task-runner-reentrancy.md。
    return 0;
}
