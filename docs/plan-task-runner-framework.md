# 可执行/可停止任务框架（task_runner）实现计划

- 日期: 2026-09-04
- 作者: Claude
- 状态: 已实施（实现与测试均完成，34/34 用例通过）
- 关联: CplushMultiThread 框架模块 `src/include/task_runner/`

## 背景

业务场景：界面有「执行」与「停止」两个按钮，分别在不同线程触发。
- 执行是耗时任务；停止是耗时操作，且必须**先通知执行中的任务停止 → 等待其真正停止 → 再执行停止后处理**。
- 需要防重入：执行中再执行 → 返回"已在运行"错误码；停止中再停止 → 返回"已在停止"错误码。
- 执行过程中，每 1s 需要一个独立回调发布当前状态。

目标是把整套能力做成**通用、可复用、业务集成简单、方便扩展**的 C++17 框架，业务只填充自己的耗时内容，线程编排/状态机/重入保护/错误码全部收敛在框架内。

## 目标

- `task_runner` 框架，**仅头文件（.hpp）**，业务拷贝 `src/include/task_runner/` 即可集成。
- 两个独立可复用基础工具：`stop_token`/`stop_control`（协作式停止源）与 `periodic_publisher`（任意周期回调执行器）。
- `runnable_task` 接口：**业务只填耗时内容**，通过 **`task_mode` 传参**区分同步/异步。
- 同步任务：worker 线程执行业务内容，内容返回 = 任务完成。
- 异步任务：`run()` 把内容放到业务自有的执行形态（内部线程跑同步块，或事件驱动的 promise 完成回调）后快速返回（不阻塞）；业务返回 `std::shared_future<void>` 完成句柄，框架 `wait()` 判定真正完成（即时等待，非轮询，不强迫业务写轮询循环）。
- 停止为协作式：框架只置标记；**业务自行感知**（读 token）决定是否继续；业务不感知则框架一直等业务内容执行结束才返回。
- 非目标：不做强制停止/超时杀任务；不内置任务重启；不做跨控制器通信。

## 方案

### 组件与公开 API（命名空间 `task_runner`，头文件于 `src/include/task_runner/`）

**`stop_token.hpp`** — 停止源与只读视图（C++17：`atomic<bool>` + `mutex` + `condition_variable`）。

```cpp
class stop_token {                       // 只读视图，业务只能感知、不能触发
    bool stop_requested() const noexcept;
    bool wait_for(std::chrono::milliseconds) const;  // true=停止已到；false=超时
};

class stop_control {                     // 可 request_stop 的源；多个 token 共享同一状态
    static std::shared_ptr<stop_control> create();
    bool request_stop() noexcept;                     // 幂等
    bool stop_requested() const noexcept;
    bool wait_for(std::chrono::milliseconds) const;
    void wake_all() noexcept;
    stop_token token() const;
};
```

**`periodic_publisher.hpp`** — 通用周期回调执行器（独立可复用）。`std::function<void()>` tick，不绑定任何状态类型；线程所有权归自身（RAII 析构 join）。

```cpp
class periodic_publisher {
    explicit periodic_publisher(std::chrono::milliseconds period, tick_fn tick);
    error_code start(stop_token token);               // 重入保护
    error_code stop() noexcept;                       // 退出并 join（幂等）
    bool running() const noexcept;
    std::thread::id thread_id() const noexcept;
    std::exception_ptr tick_exception() const noexcept;
};
```

**`runnable_task.hpp`** — 业务接口 + 通用枚举。

```cpp
enum class task_mode { sync, async };
enum class error_code { ok, already_running, already_stopping, not_running, self_stop_denied, resource_error };
enum class run_result { none, completed, stopped, failed };

class runnable_task {
    virtual void run(stop_token token) = 0;                                   // 业务主体
    virtual std::shared_future<void> async_completion() const { return {}; }  // 异步完成句柄
    virtual void on_tick() {}                                                 // 每秒状态发布
};
```

**`task_controller.hpp`** — 编排器 + 状态机。

```cpp
class task_controller {
public:
    explicit task_controller(std::shared_ptr<runnable_task> task,
                             task_mode mode = task_mode::sync,
                             std::chrono::milliseconds tick_period = std::chrono::seconds(1));
    ~task_controller();                          // 强制停止并 join；不触发 on_stopped
    error_code start();                          // 启动 worker + periodic_publisher
    error_code stop(std::function<void()> on_stopped = {});  // 阻塞：等真正停止 → join → on_stopped
    bool running() const noexcept;
    run_result last_run_result() const noexcept; // first-wins
    std::exception_ptr run_exception() const noexcept;
    std::exception_ptr tick_exception() const noexcept;
};
```

### 线程模型与状态机

- 每次 `start()` 创建**全新 `run_phase`**（新 `stop_control`、新 worker、新 publisher），旧 phase 整体替换；过期 token 因持有 shared_ptr 永远安全。任何 join 都在 run_phase 内部 → 无跨轮次双重 join、无成员写竞态。
- `start()`：锁内校验（running→already_running，stopping→already_stopping），回收上一自然结束的 phase（join 瞬时），置 running，**锁内**创建并启动 worker/发布器（短临界区，使 start 与 stop 原子互斥）。启动异常 → 出锁后回滚恢复 idle，返回 `resource_error`。
- worker 线程 `run_body`（整体 try/catch）：
  - **sync**：`task->run(token)` 返回即完成。
  - **async**：`task->run(token)` 快速返回 → `async_completion()` 的 future `wait()`。**即使收到停止仍继续等待 future 完成**（协作式：内部同步块的循环感知 token 自行结束；不感知则框架一直等它跑完）。future 完成即任务完成，无轮询延迟。
  - 收尾 `finalize`：锁内 first-wins 写 `result`（run_exc→failed，否则 stop_requested→stopped，否则 completed）；若 `state_==running` 置 idle（stop 抢到则保持 stopping，由 stop() 收尾）；随后请求停止并 join 发布器。
- `stop(on_stopped)`：锁内校验（stopping→already_stopping；非 running→not_running）；**自停止防护**（worker/publisher 线程内调用 → `self_stop_denied`，避免 join 自身）；置 stopping、捕获 phase；出锁后 `request_stop()` → `worker.join()` → `publisher.stop()` → 锁内置 idle → 执行 `on_stopped`（try/catch 兜底）。result 由 worker finalize 以 first-wins 写入，stop() 不覆盖。
- publisher(tick)：`while(active && !stop){ wait_for(period); on_tick(); }`，tick 异常入 tick_exc 不中断。on_tick 与 run/业务异步并发 → 共享状态须自行线程安全。
- 析构：request_stop → join → 不触发 on_stopped。**禁止与 start()/stop() 并发销毁**。

### 业务侧适配：业务开发者需要做什么

| # | 事项 | 说明 |
|---|---|---|
| B1 | 实现 `run(stop_token)` | 耗时内容写在这里。sync：返回=完成；async：只发起异步、快速返回 |
| B2 | 选 `task_mode`（sync/async） | 构造 `task_controller` 时传参 |
| B3 | 异步任务返回完成句柄 `async_completion()` | 返回 `std::shared_future<void>`，框架 wait 判完成（sync 用默认空 future） |
| B4 | 可选 `on_tick()` | 每 `tick_period`(默认 1s) 发布当前状态 |
| B5 | 感知停止 | `run()`/异步分块里查 `token.stop_requested()` 或 `wait_for()`。不感知则框架等你跑完 |
| B6 | 处理错误码 | `start()`：ok/already_running/already_stopping；`stop()`：ok/already_stopping/not_running/self_stop_denied |
| B7 | 共享状态线程安全 | `run`/`on_tick`/业务异步 并发读写 → 用 `std::atomic` 或加锁 |
| B8 | 停止后处理 | 以回调传给 `stop()`，框架保证在其正停止后执行 |
| B9 | 业务异步资源生命周期 | 业务自建的 `std::async`/线程池要自管：内部线程结束后才让 future 变为 ready，避免任务对象先于后台销毁 |

**sync 任务示例**：

```cpp
class compress_job : public runnable_task {
public:
    void run(stop_token token) override {
        for (int i = 0; i < total_; ++i) {
            if (token.stop_requested()) break;         // 业务自己感知停止
            compress_one(i);                           // 业务内容
            token.wait_for(10ms);
        }
    }
    void on_tick() override { publish(progress_); }
};
auto ctl = task_controller(std::make_shared<compress_job>(), task_mode::sync, 1s);
ctl.start();
ctl.stop([]{ do_post_processing(); });
```

**async 任务示例**（完成句柄两种来源）：

```cpp
// 来源①：内部线程跑同步块 —— 业务内容是一段阻塞序列
void run(stop_token token) override {
    fut_ = std::async(std::launch::async, [this, token] {
        while (!token.stop_requested() && chunk_ < n_) fetch_chunk(chunk_++);
    }).share();
}
std::shared_future<void> async_completion() const override { return fut_; }

// 来源②：std::promise 由业务自己的异步完成回调 set_value —— 真·事件驱动业务
void run(stop_token token) override {
    start_event_chain(/*完成回调*/ [this] { done_promise_.set_value(); });
}
std::shared_future<void> async_completion() const override {
    return done_promise_.get_future().share();
}
// 停止注意（来源②）：事件驱动业务需在自己异步链里感知 token，
// 确保停止请求后仍有完成回调最终触发，否则框架 wait 一直等待（协作式语义）。
```

### 运行场景 × 状态矩阵（测试用例=逐行派生）

状态：**I**=idle，**R**=running，**S**=stopping。模式：任意=sync/async 均适用。

| # | 前置 | 动作/事件 | 模式 | 预期结果 | 对应测试 |
|---|---|---|---|---|---|
| SC-01 | — | 初始 | 任意 | `stop_requested()==false`；未请求时 `wait_for` 超时 false | `StopToken.InitialNotRequested`/`WaitTimeout` |
| SC-02 | — | `request_stop()` | 任意 | `stop_requested()==true`；等待者被唤醒；重复 request 幂等 | `StopToken.RequestWakeWaiters`/`RequestIdempotent` |
| SC-03 | — | 多 token 拷贝/并发读写 | 任意 | 共享同一状态；无数据竞争 | `StopToken.TokenCopiesShareState`/`ConcurrentReadWrite` |
| SC-04 | 停止源就绪 | 发布器 `start(token)` | 独立 | ok；周期 tick（近似次数） | `Publisher.TicksAtPeriod` |
| SC-05 | 发布器 R | 再 `start()` | 独立 | `already_running` | `Publisher.StartTwiceRejected` |
| SC-06 | 发布器 R | token `request_stop()` | 独立 | 循环退出；join 快速 | `Publisher.FollowsTokenStop` |
| SC-07 | 发布器 R | `stop()` | 独立 | join（≤一个周期）；不再 tick | `Publisher.StopJoinsAndCeases`/`StopWhenIdleSafe` |
| SC-08 | 发布器 R | tick 抛异常 | 独立 | 不崩；`tick_exception()` 有值；循环继续 | `Publisher.TickExceptionRecorded` |
| SC-09 | I | `start()` | 任意 | ok；`running()==true` | `TaskController.StartOk` |
| SC-10 | R | `start()` | 任意 | `already_running`；状态不变 | `TaskController.StartWhileRunningRejected` |
| SC-11 | S | `start()` | 任意 | `already_stopping` | `TaskController.StartWhileStoppingRejected` |
| SC-12 | R | `stop(回调)`（sync 协作） | sync | ok；stopping→idle；`stopped`；on_stopped 在 run 返回后执行 | `TaskController.SyncCoopStop` |
| SC-13 | S | 并发第二次 `stop()` | 任意 | `already_stopping`；on_stopped 只一次 | `TaskController.SecondStopRejected` |
| SC-14 | I | `stop()` | 任意 | `not_running`；回调不执行 | `TaskController.StopWhenIdle` |
| SC-15 | R | worker 线程内调 `stop()` | 任意 | `self_stop_denied`；不 join 自身 | `TaskController.SelfStopFromTaskRejected` |
| SC-16 | R | tick 线程内调 `stop()` | 任意 | `self_stop_denied` | `TaskController.SelfStopFromTickRejected` |
| SC-17 | R | run 自然返回（无停止） | sync | →idle；`completed`；随后 stop→`not_running` | `TaskController.SyncNaturalComplete` |
| SC-18 | R | run 自然返回但收到停止（业务不感知） | sync | 框架等 run 返回；`stopped`；stop() 返回 ok | `TaskController.SyncIgnoreStopWaits` |
| SC-19 | R | run() 抛异常（无停止） | 任意 | →idle；`failed`；`run_exception()` 非空 | `TaskController.RunExceptionFailed` |
| SC-20 | R | run() 抛异常 + 收到停止 | 任意 | `failed`（异常优先，first-wins） | `TaskController.RunExceptionWinsOverStop` |
| SC-21 | R | on_tick 正常 | 任意 | 每 `tick_period` 调用一次 | `TaskController.TickAtPeriod` |
| SC-22 | R | on_tick 抛异常 | 任意 | 捕获；`tick_exception()` 非空；run 不中断 | `TaskController.TickExceptionKeepsRun` |
| SC-23 | R | 析构 controller | 任意 | 自动停止+join；不触发 on_stopped | `TaskController.DestructorAutoStop` |
| SC-24 | I | 析构 controller | 任意 | 安全无操作 | `TaskController.DestructorIdleSafe` |
| SC-25 | R | async：future 完成（无停止） | async | →idle；`completed` | `TaskController.AsyncCompletionFuture` |
| SC-26 | R | async + 停止 + future 延迟完成（业务配合） | async | 继续 wait future；`stopped`；on_stopped 执行 | `TaskController.AsyncStopWaitsFuture` |
| SC-27 | R | async + 停止 + future 长时间不完成（不配合） | async | 持续等待；不提前置 stopped；stop() 阻塞 | `TaskController.AsyncUncooperativeKeepsWaiting` |
| SC-28 | I/R | 多轮 start/stop；旧 token 不影响新一轮 | 任意 | 每轮独立 run_phase，均可复用 | `TaskController.MultiCycleReuse`/`StaleTokenDoesNotAffectNext` |
| SC-29 | I | start 期间线程创建失败 | 任意 | 回滚→idle；`resource_error` | 不单测（依赖系统资源），代码评审覆盖 |

## 影响范围

- 新建 `src/include/task_runner/`：`stop_token.hpp`、`periodic_publisher.hpp`、`runnable_task.hpp`、`task_controller.hpp`。
- 修改 `src/main.cpp`：sync + async 演示。
- 修改 `CMakeLists.txt`：新增 `task_runner` INTERFACE 库（include 目录 + `Threads::Threads`），主目标与 `unit_tests` 链接。
- 修改 `tests/CMakeLists.txt`：`unit_tests` 链接 `task_runner`。
- 新建测试 `tests/task_runner/`：`TestStopToken.cpp`、`TestPeriodicPublisher.cpp`、`TestTaskController.cpp`。
- 无兼容性/性能影响（仅头文件，新模块）。

## 风险与对策

- **stop() 会阻塞直到任务真正停止**：协作式停止，sync 不读 token / async future 不完成时 stop() 长期等待。对策：文档写明业务需用 `token.wait_for()` 感知/推进。
- **async_completion() 与 run 并发**：框架只在 worker 线程调 run()/async_completion()，无自身竞态；业务异步回调与业务状态共享需自行保护。
- **on_tick 与 run 并发访问业务状态**：文档 + demo 用 `std::atomic` 演示。
- **回调里调控制器 API**：on_stopped/on_tick 内调 start/stop → self_stop_denied/already_running 防护；文档禁止。
- **与析构并发**：未定义行为；文档严格声明析构不得与 start/stop 并发。
- **first-wins 结果**：worker finalize 统一写入，stop()/下一次 start() 不覆盖。

## 替代方案

- **迭代式 step()（框架循环业务步骤）**：已否决。业务是"一个整体耗时内容"，停止由业务自行感知，不需要框架以 step 粒度介入。
- **async_done() 布尔轮询**：已否决。业务写法两套心智、有轮询延迟；改用 future 完成句柄（即时等待），业务只学一种写法。
- **平铺 worker_/ticker_ 成员**：已否决（双重 join、成员写竞态）。
- **worker join ticker**：已否决（on_tick 调 stop() 时互等死锁）。
- **C++20 jthread/stop_token**：项目约束 C++17，不用。

## 实施步骤

- [x] 1. `src/include/task_runner/stop_token.hpp`
- [x] 2. `src/include/task_runner/periodic_publisher.hpp`
- [x] 3. `src/include/task_runner/runnable_task.hpp`（含 task_mode/error_code/run_result）
- [x] 4. `src/include/task_runner/task_controller.hpp`（run_phase + sync/async run_body + 状态机）
- [x] 5. 修改 `CMakeLists.txt` 与 `tests/CMakeLists.txt`
- [x] 6. 修改 `src/main.cpp` 演示
- [x] 7. 按矩阵写测试三个 `Test*.cpp`
- [x] 8. 构建 + `ctest`（43/43 通过，含压测 9 项；激进压测默认禁用）
- [x] 9. 压测 `TestStress.cpp`（含两个 `DISABLED_` 激进用例）
- [x] 10. 本计划文档

## 测试计划

- 单测：`TestStopToken.cpp`（SC-01~03）、`TestPeriodicPublisher.cpp`（SC-04~08）、`TestTaskController.cpp`（SC-09~28），矩阵逐行覆盖，全部通过。
- 压测：`TestStress.cpp`（默认 ctest 内 9 项有界用例）：并发 start/stop hammer、同时多线程 stop 只成功一次、300 轮复用、控制器高频创建销毁、start/stop 紧邻竞态、1ms 高频 tick、多异步任务并发、多等待者唤醒；另有 `DISABLED_HeavyCycles`（5000 轮）/`DISABLED_HeavyHammer` 两个激进用例，显式运行：
  `./build/tests/unit_tests --gtest_also_run_disabled_tests --gtest_filter=Stress.DISABLED_*`
- 集成：`./build/CplushMultiThread` 演示 sync/async 全流程（状态发布、协作停止、on_stopped 后处理、错误码分支）。

## 验证

1. `cmake -S . -B build` + `cmake --build build -j` 编译通过。
2. `ctest --test-dir build --output-on-failure` → 100% passed（34 tests）。
3. `./build/CplushMultiThread`：观察每 1s 状态发布、触发停止 → on_stopped 后处理、执行/停止各自触发错误码分支。

## 引用

- 模板：`docs/plan-template.md`
- 框架实现：`src/include/task_runner/`
- 测试：`tests/task_runner/`
- 演示：`src/main.cpp`