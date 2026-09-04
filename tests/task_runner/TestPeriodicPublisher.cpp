// tests/task_runner/TestPeriodicPublisher.cpp
// 覆盖矩阵 SC-04~08：周期 tick / 重入保护 / token 停止 / join / tick 异常。

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "task_runner/periodic_publisher.hpp"
#include "task_runner/stop_token.hpp"

using namespace std::chrono_literals;

using task_runner::error_code;
using task_runner::periodic_publisher;
using task_runner::stop_control;

TEST(Publisher, TicksAtPeriod) {
    auto ctl = stop_control::create();
    std::atomic<int> n{0};
    periodic_publisher pub(40ms, [&n] { ++n; });
    EXPECT_EQ(pub.start(ctl->token()), error_code::ok);
    std::this_thread::sleep_for(270ms);
    ctl->request_stop();
    pub.stop();
    // ~6 个周期 → 至少 4 次（留余量防抖动）
    EXPECT_GE(n.load(), 4);
    EXPECT_FALSE(pub.running());
}

TEST(Publisher, StartTwiceRejected) {
    auto ctl = stop_control::create();
    periodic_publisher pub(40ms, [] {});
    EXPECT_EQ(pub.start(ctl->token()), error_code::ok);
    EXPECT_EQ(pub.start(ctl->token()), error_code::already_running);
    ctl->request_stop();
    pub.stop();
}

TEST(Publisher, FollowsTokenStop) {
    auto ctl = stop_control::create();
    std::atomic<int> n{0};
    periodic_publisher pub(40ms, [&n] { ++n; });
    pub.start(ctl->token());
    std::this_thread::sleep_for(120ms);
    ctl->request_stop();  // token 停止 → tick 循环退出
    pub.stop();
    int after = n.load();
    std::this_thread::sleep_for(120ms);
    EXPECT_EQ(n.load(), after);  // 不再 tick
    EXPECT_FALSE(pub.running());
}

TEST(Publisher, StopJoinsAndCeases) {
    auto ctl = stop_control::create();
    std::atomic<int> n{0};
    periodic_publisher pub(30ms, [&n] { ++n; });
    pub.start(ctl->token());
    std::this_thread::sleep_for(100ms);
    pub.stop();
    int after = n.load();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(n.load(), after);
    EXPECT_FALSE(pub.running());
}

TEST(Publisher, StopWhenIdleSafe) {
    periodic_publisher pub(30ms, [] {});
    EXPECT_EQ(pub.stop(), error_code::not_running);  // 未启动，安全返回
}

TEST(Publisher, TickExceptionRecorded) {
    auto ctl = stop_control::create();
    std::atomic<int> good{0};
    periodic_publisher pub(30ms, [&good] {
        if (good++ % 2 == 0) {
            throw std::runtime_error("tick boom");
        }
    });
    pub.start(ctl->token());
    std::this_thread::sleep_for(180ms);
    ctl->request_stop();
    pub.stop();
    EXPECT_NE(pub.tick_exception(), nullptr);  // 首个异常被记录
    EXPECT_GE(good.load(), 3);                 // 异常后循环继续
}