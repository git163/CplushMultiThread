#ifndef TASK_RUNNER_RUNNABLE_TASK_HPP_
#define TASK_RUNNER_RUNNABLE_TASK_HPP_

// task_runner：业务接口与通用枚举。
// 业务只需填充 run() 的耗时内容；同步/异步由 task_mode 传参决定驱动方式。
// - sync ：run() 返回即任务完成。
// - async：run() 把同段内容发到业务自己的内部线程/执行器跑并快速返回，
//          业务返回 std::shared_future<void> 完成句柄，框架 wait() 判定真正完成。
// 可选钩子：on_tick() 周期发布状态；on_finished() 任务结束时收尾（三种结束都触发）。

#include <exception>
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
    self_stop_denied, // 在控制器自身线程（worker/发布器/结束回调）内调 start/stop，避免 join 自身
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

    // 任务结束钩子：本任务真正结束时被框架调用，每轮 start() 恰好一次。
    // - 触发时机：三种结束都触发 —— 正常完成 completed / 收到停止后结束 stopped /
    //   run() 抛异常 failed（含 async 路径等完成句柄 ready 之后）。
    // - 执行线程：框架 worker 线程（在 task_controller::finalize() 内）。
    //   因 stop() 会 join worker，故 stop() 返回时本回调保证已执行完毕；
    //   任务**自然完成**（无人调 stop()）时同样会触发，析构触发的停止也会触发。
    //   框架在回调返回后才置回 idle，故 controller.running()==false 蕴含本回调已返回。
    // - result 与 task_controller::last_run_result() 一致；
    //   error 即 task_controller::run_exception()（无异常则空）。
    // - 抛出的异常被框架捕获，记入 task_controller::finish_exception()，不影响 result。
    // - 约束：不得在回调内调用控制器 API（见 task_runner/README.md「生命周期与约束」）。
    virtual void on_finished(run_result result, std::exception_ptr error) {
        (void)result;
        (void)error;
    }
};

}  // namespace task_runner

#endif  // TASK_RUNNER_RUNNABLE_TASK_HPP_