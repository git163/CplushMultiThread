#ifndef TASK_RUNNER_STOP_TOKEN_HPP_
#define TASK_RUNNER_STOP_TOKEN_HPP_

// task_runner：协作式停止源与只读视图。
// - stop_control：可 request_stop() 的"停止源"，多个 stop_token 共享同一状态。
// - stop_token   ：发给业务任务的只读视图，只能感知停止、不能触发停止。
// C++17 实现（std::atomic<bool> + mutex + condition_variable），可安全拷贝。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace task_runner {

class stop_control;

// 只读停止视图：业务拿到后只能感知，无法触发停止。
class stop_token {
public:
    stop_token() = default;
    explicit stop_token(std::shared_ptr<const stop_control> ctl) : ctl_(std::move(ctl)) {}

    bool stop_requested() const noexcept;
    bool wait_for(std::chrono::milliseconds timeout) const;

    explicit operator bool() const noexcept {
        return static_cast<bool>(ctl_);
    }

private:
    std::shared_ptr<const stop_control> ctl_;
};

// 停止源：可触发停止；通过 token() 派发只读视图共享同一状态。
class stop_control : public std::enable_shared_from_this<stop_control> {
public:
    // 工厂方法（私有构造 + shared_ptr 管理，enable_shared_from_this 才能工作）。
    static std::shared_ptr<stop_control> create() {
        return std::shared_ptr<stop_control>(new stop_control());
    }

    // 请求停止。幂等：返回 true 表示这是首次触发，false 表示之前已被请求。
    bool request_stop() noexcept {
        bool expected = false;
        if (requested_.compare_exchange_strong(expected, true)) {
            cv_.notify_all();
            return true;
        }
        return false;
    }

    bool stop_requested() const noexcept {
        return requested_.load(std::memory_order_acquire);
    }

    // 等待指定时长；返回 true=已停止 / false=超时。谓词形式规避 spurious wake 与丢唤醒。
    bool wait_for(std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, timeout, [this] { return requested_.load(std::memory_order_acquire); });
        return requested_.load(std::memory_order_acquire);
    }

    // 无停止语义地唤醒所有等待者（用于通知"本阶段已结束"，让周期发布器及时退出）。
    void wake_all() noexcept {
        cv_.notify_all();
    }

    // 派发只读视图。
    stop_token token() const {
        return stop_token(weak_from_this().lock());
    }

private:
    stop_control() = default;

    std::atomic<bool> requested_{false};
    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
};

// stop_control 必须完整定义后才能调用其成员。
inline bool stop_token::stop_requested() const noexcept {
    return ctl_ && ctl_->stop_requested();
}

inline bool stop_token::wait_for(std::chrono::milliseconds timeout) const {
    return ctl_ ? ctl_->wait_for(timeout) : false;
}

}  // namespace task_runner

#endif  // TASK_RUNNER_STOP_TOKEN_HPP_