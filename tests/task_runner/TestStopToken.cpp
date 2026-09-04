// tests/task_runner/TestStopToken.cpp
// 覆盖矩阵 SC-01/02/03：停止源的初始状态 / 请求唤醒 / 幂等 / 并发读写。

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "task_runner/stop_token.hpp"

using namespace std::chrono_literals;

using task_runner::stop_control;
using task_runner::stop_token;

TEST(StopToken, InitialNotRequested) {
    auto ctl = stop_control::create();
    auto tok = ctl->token();
    EXPECT_FALSE(tok.stop_requested());
    EXPECT_FALSE(ctl->stop_requested());
}

TEST(StopToken, WaitTimeout) {
    auto ctl = stop_control::create();
    auto tok = ctl->token();
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(tok.wait_for(60ms));
    auto dur = std::chrono::steady_clock::now() - start;
    EXPECT_GE(dur, 50ms);  // 确实等待了约一个周期
}

TEST(StopToken, RequestWakeWaiters) {
    auto ctl = stop_control::create();
    auto tok = ctl->token();
    std::atomic<bool> woke_last{false};
    std::thread w([&] { woke_last = tok.wait_for(2s); });
    std::this_thread::sleep_for(50ms);
    ctl->request_stop();
    w.join();
    EXPECT_TRUE(woke_last);
    EXPECT_TRUE(tok.stop_requested());
}

TEST(StopToken, RequestIdempotent) {
    auto ctl = stop_control::create();
    EXPECT_TRUE(ctl->request_stop());
    EXPECT_FALSE(ctl->request_stop());  // 第二次幂等，非首次
    EXPECT_TRUE(ctl->stop_requested());
}

TEST(StopToken, TokenCopiesShareState) {
    auto ctl = stop_control::create();
    stop_token a = ctl->token();
    stop_token b = a;  // 拷贝共享同一状态
    ctl->request_stop();
    EXPECT_TRUE(a.stop_requested());
    EXPECT_TRUE(b.stop_requested());
}

TEST(StopToken, ConcurrentReadWrite) {
    auto ctl = stop_control::create();
    auto tok = ctl->token();
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (int k = 0; k < 200; ++k) {
                (void)tok.stop_requested();
                (void)tok.wait_for(1ms);
            }
        });
    }
    threads.emplace_back([&] {
        for (int k = 0; k < 20; ++k) {
            (void)ctl->request_stop();
        }
    });
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_TRUE(ctl->stop_requested());
}