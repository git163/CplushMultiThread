// examples/progress_and_finish.cpp
// 场景：界面要显示进度，并在任务结束时做收尾（上传结果 / 通知界面 / 报错）。
// 对应《场景使用指南》§4 状态发布、§5 结束收尾。

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
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

// 模拟"投递到 UI 线程"（Qt/Android/Web 各有自己的 API，这里只打印）。
static void ui_post(std::function<void()> fn) { fn(); }

// 正常任务：跑完自然结束，期间发布进度，结束后收尾。
class report_job : public runnable_task {
public:
    void run(stop_token token) override {
        for (int i = 0; i < total_; ++i) {
            if (token.stop_requested()) {
                break;
            }
            std::this_thread::sleep_for(60ms);
            done_ = i + 1;
        }
    }

    void on_tick() override {
        const int d = done_.load();  // 原子读
        // total_ 是 static constexpr，lambda 内无需捕获即可使用
        ui_post([d] { std::cout << "  [report] progress " << d << "/" << total_ << std::endl; });
    }

    // 三种结果都走这里。注意：它跑在 worker 线程上，不能直接碰控件 → 用 ui_post。
    void on_finished(run_result r, std::exception_ptr e) override {
        ++finish_count_;
        ui_post([r, e] {
            switch (r) {
                case run_result::completed: std::cout << "  [report] finished: completed" << std::endl; break;
                case run_result::stopped:   std::cout << "  [report] finished: cancelled" << std::endl; break;
                case run_result::failed:
                    std::cout << "  [report] finished: failed";
                    try {
                        if (e) { std::rethrow_exception(e); }
                    } catch (std::exception const& ex) {
                        std::cout << " (" << ex.what() << ")";
                    }
                    std::cout << std::endl;
                    break;
                case run_result::none: break;
            }
        });
    }

    int finish_count() const { return finish_count_.load(); }

private:
    static constexpr int total_ = 40;  // 40 * 60ms = 2.4s 自然跑完
    std::atomic<int> done_{0};
    std::atomic<int> finish_count_{0};
};

// 失败任务：run() 抛异常。
class failing_job : public runnable_task {
public:
    void run(stop_token) override {
        std::this_thread::sleep_for(100ms);
        throw std::runtime_error("disk full");
    }
};

int main() {
    std::cout << "== example: progress reporting + finish callback ==" << std::endl;

    // ---- ① 自然完成：没人调 stop()，on_finished 依然触发 ----
    std::cout << "[part 1] natural completion" << std::endl;
    auto job = std::make_shared<report_job>();
    task_controller ctl(job, task_mode::sync, 500ms);

    std::cout << "start(): " << to_str(ctl.start()) << std::endl;
    for (int i = 0; i < 100 && ctl.running(); ++i) {
        std::this_thread::sleep_for(50ms);  // 只等，不调 stop()
    }
    // 不变式：running() == false 蕴含 on_finished 已返回
    std::cout << "running()==false, on_finished count = " << job->finish_count() << std::endl;
    std::cout << "last_run_result: " << to_str(ctl.last_run_result()) << std::endl;

    // ---- ② 失败：run() 抛异常 ----
    std::cout << "\n[part 2] run() throws" << std::endl;
    auto bad = std::make_shared<failing_job>();
    task_controller bad_ctl(bad);
    std::cout << "start(): " << to_str(bad_ctl.start()) << std::endl;
    for (int i = 0; i < 100 && bad_ctl.running(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    std::cout << "last_run_result: " << to_str(bad_ctl.last_run_result()) << std::endl;
    try {
        if (auto e = bad_ctl.run_exception()) {
            std::rethrow_exception(e);
        }
    } catch (std::exception const& ex) {
        std::cout << "run_exception(): " << ex.what() << std::endl;
    }

    return 0;
}
