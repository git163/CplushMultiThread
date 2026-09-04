// tests/task_runner/TestStress.cpp
// 压测用例：在默认 ctest 中保持有界（每个用例 ≤ ~2.5s）；更激进的重压用例用
// DISABLED_ 前缀，显式运行方式：
//   ./build/tests/unit_tests --gtest_also_run_disabled_tests --gtest_filter=Stress.DISABLED_*
//
// 关注点：并发 start/stop 竞态、同时多线程 stop 只成功一次、长周期复用（线程/阶段回收）、
// 控制器高频创建销毁、start 与 stop 紧邻竞态、高频 tick、多异步任务并发、多等待者唤醒。

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "task_runner/periodic_publisher.hpp"
#include "task_runner/stop_token.hpp"
#include "task_runner/task_controller.hpp"

using namespace std::chrono_literals;

using task_runner::error_code;
using task_runner::periodic_publisher;
using task_runner::runnable_task;
using task_runner::stop_control;
using task_runner::stop_token;
using task_runner::task_controller;
using task_runner::task_mode;

// 快速协作式任务：每 1ms 检查一次停止。
class fast_job : public runnable_task {
public:
    void run(stop_token token) override {
        int n = 0;
        while (!token.stop_requested() && n < 1000000) {
            std::this_thread::sleep_for(1ms);
            ++n;
            ++iterations_;
        }
    }

    std::atomic<long> iterations_{0};
};

// 高频 on_tick 任务。
class tick_count_job : public runnable_task {
public:
    void run(stop_token token) override {
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(1ms);
        }
    }
    void on_tick() override { ++ticks_; }

    std::atomic<long> ticks_{0};
};

// 异步任务（内部线程跑同步内容直至停止）。
class async_burst_job : public runnable_task {
public:
    void run(stop_token token) override {
        fut_ = std::async(std::launch::async, [this, token] {
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(1ms);
            }
        }).share();
    }
    std::shared_future<void> async_completion() const override { return fut_; }

private:
    std::shared_future<void> fut_;
};

// ========================= 有界压测（默认 ctest 运行） =========================

TEST(Stress, HammerStartStop) {
    // 8 个线程同时锤同一个控制器：反复 start/stop + 抢占失败路径。
    auto job = std::make_shared<fast_job>();
    task_controller ctl(job, task_mode::sync, 5ms);
    std::atomic<long> ok_start{0};
    std::atomic<int> bad_foreign_code{0};
    constexpr int kThreads = 8;
    constexpr int kLoops = 150;

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kLoops; ++i) {
                auto r = ctl.start();
                if (r == error_code::ok) {
                    ++ok_start;
                    (void)ctl.stop();  // 立即停止，制造竞态窗口
                } else if (r != error_code::already_running &&
                           r != error_code::already_stopping) {
                    ++bad_foreign_code;  // 正常负载下不应出现其他错误码
                }
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }

    EXPECT_EQ(bad_foreign_code.load(), 0);
    EXPECT_GE(ok_start.load(), 1);
    EXPECT_FALSE(ctl.running());  // 结束后应收敛到 idle

    // 收敛后仍可正常使用
    EXPECT_EQ(ctl.start(), error_code::ok);
    EXPECT_TRUE(ctl.running());
    EXPECT_EQ(ctl.stop(), error_code::ok);
    EXPECT_FALSE(ctl.running());
}

TEST(Stress, ConcurrentStopWins) {
    // 8 个线程同时 stop：恰好一个成功，on_stopped 只执行一次。
    auto job = std::make_shared<fast_job>();
    task_controller ctl(job, task_mode::sync, 10ms);
    ASSERT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(20ms);

    std::atomic<int> on_stopped_count{0};
    std::atomic<int> ok_count{0};
    std::atomic<int> early_count{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&] {
            auto r = ctl.stop([&] { ++on_stopped_count; });
            if (r == error_code::ok) {
                ++ok_count;
            } else {
                ++early_count;  // already_stopping / not_running 都属"让位"
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }
    EXPECT_EQ(on_stopped_count.load(), 1);
    EXPECT_EQ(ok_count.load(), 1);
    EXPECT_EQ(early_count.load(), 7);
}

TEST(Stress, ManyCyclesReuse) {
    // 300 轮 start/stop：验证每轮独立 run_phase、无累积泄漏/挂起。
    auto job = std::make_shared<fast_job>();
    task_controller ctl(job, task_mode::sync, 5ms);
    for (int i = 0; i < 300; ++i) {
        ASSERT_EQ(ctl.start(), error_code::ok);
        ASSERT_TRUE(ctl.running());
        ASSERT_EQ(ctl.stop(), error_code::ok);
        ASSERT_FALSE(ctl.running());
    }
    // 依然可用
    EXPECT_EQ(ctl.start(), error_code::ok);
    EXPECT_EQ(ctl.stop(), error_code::ok);
}

TEST(Stress, ChurnControllers) {
    // 4 线程 × 100 个控制器：高频创建/启动/停止/销毁，锤析构与 phase 生命周期。
    std::atomic<int> fails{0};
    constexpr int kThreads = 4;
    constexpr int kEach = 100;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kEach; ++i) {
                auto job = std::make_shared<fast_job>();
                task_controller ctl(job, task_mode::sync, 2ms);
                if (ctl.start() != error_code::ok) {
                    ++fails;
                }
                if (ctl.stop() != error_code::ok) {
                    ++fails;
                }
                // 析构接管（此时已停止，应无操作）
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }
    EXPECT_EQ(fails.load(), 0);
}

TEST(Stress, RapidStartStopRace) {
    // 生产者不停 start，消费者不停 stop，锤 start/stop 紧邻竞态窗口。
    auto job = std::make_shared<fast_job>();
    task_controller ctl(job, task_mode::sync, 5ms);
    std::atomic<int> ok_start{0};
    std::atomic<int> ok_stop{0};
    std::atomic<bool> start_done{false};

    std::thread producer([&] {
        for (int i = 0; i < 200; ++i) {
            if (ctl.start() == error_code::ok) {
                ++ok_start;
            }
        }
        start_done = true;
    });
    std::thread consumer([&] {
        for (int guard = 0; guard < 200000 && (!start_done.load() || ctl.running()); ++guard) {
            if (ctl.stop() == error_code::ok) {
                ++ok_stop;
            }
            std::this_thread::sleep_for(200us);
        }
    });
    producer.join();
    consumer.join();

    EXPECT_GE(ok_start.load(), 1);
    EXPECT_GE(ok_stop.load(), 1);
    EXPECT_FALSE(ctl.running());
    EXPECT_EQ(ctl.start(), error_code::ok);
    EXPECT_EQ(ctl.stop(), error_code::ok);
}

TEST(Stress, HighRateTicks) {
    // 1ms 周期的状态发布：run 不被高频 tick 干扰，stop 依然及时。
    auto job = std::make_shared<tick_count_job>();
    task_controller ctl(job, task_mode::sync, 1ms);
    EXPECT_EQ(ctl.start(), error_code::ok);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(ctl.stop(), error_code::ok);
    EXPECT_FALSE(ctl.running());
    EXPECT_GT(job->ticks_.load(), 150);  // 标称 ~300，留 2 倍余量
}

TEST(Stress, StressPublisherHighRate) {
    // 独立 periodic_publisher：1ms 高频 tick + 停止。
    auto ctl = stop_control::create();
    std::atomic<long> ticks{0};
    periodic_publisher pub(1ms, [&] { ++ticks; });
    EXPECT_EQ(pub.start(ctl->token()), error_code::ok);
    std::this_thread::sleep_for(200ms);
    ctl->request_stop();
    EXPECT_EQ(pub.stop(), error_code::ok);
    EXPECT_GT(ticks.load(), 100);  // 标称 ~200，留 2 倍余量
    EXPECT_FALSE(pub.running());
}

TEST(Stress, AsyncBurst) {
    // 8 个异步控制器并发：每个含内部线程 + 完成句柄。
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&] {
            auto job = std::make_shared<async_burst_job>();
            task_controller ctl(job, task_mode::async, 2ms);
            if (ctl.start() != error_code::ok) {
                ++bad;
            }
            std::this_thread::sleep_for(30ms);
            if (ctl.stop() != error_code::ok) {
                ++bad;
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }
    EXPECT_EQ(bad.load(), 0);
}

TEST(Stress, ManyWaitersWake) {
    // 16 个并发等待者被一次 request_stop 全部唤醒。
    auto ctl = stop_control::create();
    auto tok = ctl->token();
    std::atomic<int> woken{0};
    std::vector<std::thread> ws;
    for (int i = 0; i < 16; ++i) {
        ws.emplace_back([&] {
            if (tok.wait_for(5s)) {
                ++woken;
            }
        });
    }
    std::this_thread::sleep_for(30ms);
    ctl->request_stop();
    for (auto& w : ws) {
        w.join();
    }
    EXPECT_EQ(woken.load(), 16);
}

// ========================= 激进重压（默认禁用，显式开启） =========================

// 数千轮循环：更完整地暴露线程/阶段回收、竞态、偶发挂起。
TEST(Stress, DISABLED_HeavyCycles) {
    auto job = std::make_shared<fast_job>();
    task_controller ctl(job, task_mode::sync, 2ms);
    constexpr int kCycles = 5000;
    for (int i = 0; i < kCycles; ++i) {
        ASSERT_EQ(ctl.start(), error_code::ok);
        ASSERT_EQ(ctl.stop(), error_code::ok);
    }
    SUCCEED();
}

// 并发 hammer + 更长时间，用于长时间稳定性验证。
TEST(Stress, DISABLED_HeavyHammer) {
    auto job = std::make_shared<fast_job>();
    task_controller ctl(job, task_mode::sync, 2ms);
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 16; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) {
                if (ctl.start() == error_code::ok) {
                    if (ctl.stop() != error_code::ok) {
                        ++bad;
                    }
                }
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }
    EXPECT_EQ(bad.load(), 0);
    EXPECT_FALSE(ctl.running());
}