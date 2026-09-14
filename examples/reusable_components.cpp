// examples/reusable_components.cpp
// 场景：不要 task_controller，只想用里面的通用工具。
// 对应《场景使用指南》§10 组件独立使用。

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "demo_util.hpp"
#include "task_runner/periodic_publisher.hpp"
#include "task_runner/stop_token.hpp"

using namespace std::chrono_literals;

using demo::to_str;
using task_runner::periodic_publisher;
using task_runner::stop_control;
using task_runner::stop_token;

// ---- ① periodic_publisher：固定周期回调（如每 30s 上报心跳）----
static void heartbeat_example() {
    std::cout << "[part 1] periodic_publisher (heartbeat)" << std::endl;

    auto ctrl = stop_control::create();                    // 独立停止源
    std::atomic<int> beats{0};
    periodic_publisher pub(300ms, [&beats] {               // 每 300ms 调一次（示例用短周期）
        std::cout << "  [publisher] heartbeat #" << (++beats) << std::endl;
    });

    std::cout << "start: " << to_str(pub.start(ctrl->token())) << std::endl;  // 返回 ok / already_running
    std::this_thread::sleep_for(1s);

    ctrl->request_stop();   // 或 pub.stop()，两者任一都能停
    std::cout << "stop:  " << to_str(pub.stop()) << "  <- 幂等" << std::endl;
    std::cout << "beats = " << beats.load() << std::endl;
}

// ---- ② stop_control / stop_token：跨线程的协作式取消标记 ----
// 业务自己的线程/循环要能被外部通知"该停了"时，比传裸 atomic<bool> 更安全：
// stop_control 可触发停止，stop_token 只能感知（做成类型区分）。
static void cancel_token_example() {
    std::cout << "\n[part 2] stop_control / stop_token (cooperative cancellation)" << std::endl;

    auto ctrl = stop_control::create();
    auto tok = ctrl->token();                              // 只读视图，发给工作线程

    std::atomic<int> steps{0};
    std::thread worker([tok, &steps] {
        while (!tok.stop_requested()) {                     // 感知停止
            if (tok.wait_for(200ms)) {                      // 睡眠 + 可被立刻唤醒
                break;
            }
            ++steps;
        }
    });

    std::this_thread::sleep_for(700ms);
    std::cout << "request_stop(): " << (ctrl->request_stop() ? "first" : "already") << std::endl;
    std::cout << "request_stop() again: " << (ctrl->request_stop() ? "first" : "already")
              << "  <- 幂等" << std::endl;
    worker.join();
    std::cout << "worker stopped after " << steps.load() << " steps" << std::endl;
}

int main() {
    std::cout << "== example: reusable components (without task_controller) ==" << std::endl;
    heartbeat_example();
    cancel_token_example();
    return 0;
}
