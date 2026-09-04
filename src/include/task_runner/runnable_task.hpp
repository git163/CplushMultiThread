#ifndef TASK_RUNNER_RUNNABLE_TASK_HPP_
#define TASK_RUNNER_RUNNABLE_TASK_HPP_

// task_runner：业务接口与通用枚举。
// 业务只需填充 run() 的耗时内容；同步/异步由 task_mode 传参决定驱动方式。
// - sync ：run() 返回即任务完成。
// - async：run() 把同段内容发到业务自己的内部线程/执行器跑并快速返回，
//          业务返回 std::shared_future<void> 完成句柄，框架 wait() 判定真正完成。

#include <future>
#include <memory>

#include "task_runner/stop_token.hpp"

namespace task_runner {

// 任务模式（决定框架如何驱动 run()）。
enum class task_mode { sync, async };

// 控制器公共错误码。
enum class error_code {
    ok,               // 成功
    already_running,  // 正在执行，禁止重入
    already_stopping, // 正在停止，禁止重入
    not_running,      // 未在运行（如对空闲任务调用 stop）
    self_stop_denied, // 在任务/tick 线程内调用 stop，避免 join 自身
    resource_error,   // 线程创建等资源失败
};

// 一次运行的结果（first-wins，只写一次）。
enum class run_result {
    none,      // 尚无结果
    completed, // 正常完成（未收到停止请求）
    stopped,   // 运行期间收到停止请求后结束
    failed,    // run()/内部任务抛出异常
};

// 业务接口。
// 停止为协作式：run()/异步分块里自行查 token.stop_requested() 决定是否提前退出；
// 业务不感知则框架一直等业务内容执行结束才返回。
class runnable_task {
public:
    virtual ~runnable_task() = default;

    // 业务主体（耗时内容）。同步/异步写法完全一致：同步块 + token 感知停止。
    virtual void run(stop_token token) = 0;

    // 异步完成句柄。async 模式：返回代表后台"内部任务"完成的 shared_future，
    // 框架 wait 它（即时、非轮询）。sync 模式用默认空 future（run() 返回即完成）。
    // 业务典型实现：
    //   fut_ = std::async(std::launch::async, [this, token]{ /*同步内容*/ }).share();
    virtual std::shared_future<void> async_completion() const {
        return {};
    }

    // 状态发布钩子：每 tick_period（默认 1s）被框架调用一次，用于推当前状态。
    // 注意：与 run()/业务异步并发执行，共享状态需自行线程安全（如 std::atomic）。
    virtual void on_tick() {}
};

}  // namespace task_runner

#endif  // TASK_RUNNER_RUNNABLE_TASK_HPP_