# task_runner — 可执行/可停止任务框架

> C++17 · 仅头文件 · 零第三方依赖（标准库 + 线程）
> 面向"执行 + 停止"按钮类场景：业务只填耗时内容，线程编排/状态机/防重入/周期状态发布全部由框架接管。

## 目录

1. [特性](#特性)
2. [核心概念](#核心概念)
3. [快速开始（同步任务）](#快速开始同步任务)
4. [业务任务写法](#业务任务写法)
   - [同步任务](#同步任务sync)
   - [异步任务（完成句柄两种来源）](#异步任务async)
   - [状态发布 on_tick](#状态发布-on_tick)
   - [任务结束 on_finished](#任务结束-on_finished)
5. [错误码与结果](#错误码与结果)
6. [生命周期与约束（必读）](#生命周期与约束必读)
7. [组件独立使用](#组件独立使用)
8. [构建与集成](#构建与集成)
9. [测试](#测试)
10. [常见坑](#常见坑)

---

## 特性

| 能力 | 说明 |
|---|---|
| 执行/停止编排 | `start()` 启动 worker 线程 + 周期发布器；`stop()` 阻塞等到任务真正停止，再跑停止后处理回调 |
| 防重入 | 运行中再 `start()` → `already_running`；停止中再 `start()`/`stop()` → `already_stopping` |
| 协作式停止 | 框架只置停止标记；业务自行感知（读 token）；不感知则框架一直等业务跑完 |
| 同步/异步任务 | `task_mode::sync` / `task_mode::async` 传参区分驱动方式，业务接口统一 |
| 周期状态发布 | 每 `tick_period`（默认 1s）在独立线程调 `on_tick()`，可推当前状态 |
| 任务结束回调 | `on_finished(result, exception)`：三种结束（completed/stopped/failed）都触发，每轮恰好一次 |
| 错误码/结果 | `error_code` 区分各失败场景；`run_result` 区分 completed / stopped / failed |
| 通用工具 | `periodic_publisher`、`stop_control`/`stop_token` 可脱离控制器单独复用 |
| 异常安全 | `run()`/`on_tick()`/`on_finished()`/停止回调抛异常均被捕获记录，不崩溃 |
| 仅头文件 | 拷贝 `task_runner/` 目录即可移植到其他项目 |

## 核心概念

| 组件 | 头文件 | 职责 |
|---|---|---|
| `stop_control` / `stop_token` | `stop_token.hpp` | 协作式停止源与只读视图：`stop_control` 可触发停止，`stop_token` 只许感知 |
| `periodic_publisher` | `periodic_publisher.hpp` | 通用周期回调执行器（独立线程，可复用） |
| `runnable_task` | `runnable_task.hpp` | 业务接口：`run()`（耗时内容）+ 可选 `on_tick()` / `on_finished()` / `async_completion()` |
| `task_controller` | `task_controller.hpp` | 编排器：状态机 + 线程 + 周期发布 + 防重入 + 错误码 |

线程模型：`start()` 每次创建一个**全新的 `run_phase`**（新停止源 + worker 线程 + 发布器线程），旧阶段整体替换，杜绝跨轮次的 join 竞态。`stop()` 与 `start()` 可来自不同线程。

## 快速开始（同步任务）

```cpp
#include "task_runner/task_controller.hpp"

using namespace std::chrono_literals;
using namespace task_runner;

// ① 业务任务：只填耗时内容（run），可选每秒状态发布（on_tick）
class compress_job : public runnable_task {
public:
    explicit compress_job(std::function<void(int, int)> publish) : publish_(std::move(publish)) {}

    void run(stop_token token) override {
        for (int i = 0; i < total_; ++i) {
            if (token.stop_requested()) {   // ② 感知停止（可选；不感知就跑完全部）
                break;
            }
            compress_one(i);                // ★ 业务内容
            token.wait_for(10ms);           // 睡眠+轮询：让出 CPU，停止时也能及时被唤醒
            done_ = i + 1;
        }
    }

    // ③ 状态发布：每 1s 在独立线程被调用
    void on_tick() override { publish_(done_.load(), total_); }

    // ④ 任务结束：三种结束都触发（自然完成也会触发）
    void on_finished(run_result r, std::exception_ptr e) override {
        report_finished(r, e);              // ★ 收尾：上传结果/清理/通知 UI
    }

private:
    static constexpr int total_ = 100;
    std::atomic<int> done_{0};              // 跨线程共享，必须原子/加锁
    std::function<void(int, int)> publish_;
};

// ② 装配 + 启动
auto job = std::make_shared<compress_job>([](int d, int t) { report_progress(d, t); });
task_controller ctl(job, task_mode::sync, 1s);

error_code e = ctl.start();                 // 启动 worker + 周期发布器，不阻塞
if (e != error_code::ok) { /* already_running / already_stopping */ }

// ③ 另一线程（停止按钮）：
std::thread stop_thread([&] {
    ctl.stop([] { /* 停止后处理 */ });      // 阻塞直到真正停止 → 执行回调
});
stop_thread.join();
```

## 业务任务写法

### 同步任务（sync）

- 任务内容在框架的 worker 线程上执行。
- `run()` 返回 = 任务完成；期间收到停止，业务可自行提前退出。
- 若 `run()` 不感知停止，`stop()` 会一直等它跑完（协作式语义，见[常见坑](#常见坑)）。

```cpp
class my_sync_job : public runnable_task {
    void run(stop_token token) override {
        // 你的同步耗时逻辑；需要可中断就在关键点查 token
    }
};
task_controller ctl(std::make_shared<my_sync_job>(), task_mode::sync, 1s);
```

### 异步任务（async）

- `run()` 只**发起**异步、快速返回，**不阻塞**。
- 框架随后 wait 业务返回的完成句柄 `async_completion()` 判定真正完成（即时、非轮询）。
- 期间 `on_tick()` 照常发布状态；收到停止仍会等完成句柄变 ready。

**完成句柄来源① —— 内部线程跑同步块**（业务内容是一段阻塞序列时）：

```cpp
class download_job : public runnable_task {
public:
    void run(stop_token token) override {
        // 把"同步内容"发到业务自己的内部线程；run 快速返回
        fut_ = std::async(std::launch::async, [this, token] {
            while (!token.stop_requested() && chunk_ < n_) {
                fetch_chunk(chunk_++);      // 与同步写法一致，分块查停止
            }
        }).share();
    }
    std::shared_future<void> async_completion() const override { return fut_; }
    void on_tick() override { publish_status(chunk_.load(), n_); }
private:
    std::shared_future<void> fut_;
    std::atomic<int> chunk_{0};
};
task_controller ctl(std::make_shared<download_job>(), task_mode::async, 1s);
```

**完成句柄来源② —— `std::promise` 由业务自己的异步完成回调置值**（业务本身就是事件/回调驱动时，不需要线程、不需要循环）：

```cpp
class event_job : public runnable_task {
public:
    void run(stop_token token) override {
        stopper_ = token;
        start_event_chain(/*完成回调*/ [this] { done_promise_.set_value(); });  // 业务自有异步链
    }
    std::shared_future<void> async_completion() const override {
        return done_promise_.get_future().share();
    }
private:
    stop_token stopper_;
    mutable std::promise<void> done_promise_;   // 只 set 一次；mutable 因 async_completion() 是 const
};
```

> 停止注意（来源②）：事件驱动业务需在自己异步链里感知 `stopper_`，确保停止请求后**仍有完成回调最终触发**（或提前 set_value），否则框架 wait 会一直等（协作式语义）。

### 状态发布 `on_tick`

- 由周期发布器在**独立线程**每 `tick_period` 调用一次（构造 `task_controller` 第 3 个参数，默认 1s）。
- 与 `run()`/业务异步**并发**执行 → 共享的进度等状态必须线程安全（`std::atomic` 或加锁）。
- `on_tick()` 抛异常被捕获，记入 `tick_exception()`，不中断 `run()`。

### 任务结束 `on_finished`

任务**真正结束**时被框架调用一次（每轮 `start()` 恰好一次），用来做收尾：上传结果、清理资源、通知界面。

```cpp
void on_finished(run_result result, std::exception_ptr error) override {
    if (result == run_result::completed) { save_result(); }
    else if (result == run_result::failed) { report_error(error); }
    else { /* stopped：被停止打断 */ }
}
```

- **触发范围**：三种结束都触发 —— `completed`（正常跑完）/ `stopped`（收到停止后结束）/ `failed`（`run()` 抛异常）。**任务自然完成（无人调 `stop()`）时同样触发**，这正是它与 `on_stopped` 的核心差别。
- **执行线程**：框架 **worker 线程**（不是调 `stop()` 的线程）。
- **时序保证**：
  - `stop()` 返回时 `on_finished` **保证已执行完毕**，且**先于** `on_stopped`；
  - `running()` 返回 `false` 蕴含 `on_finished` 已返回（框架在回调返回后才置回 idle）→ 可以放心用 `running()` 轮询等结束；
  - 析构 `task_controller` 触发的停止**也会**触发（任务确实结束了，与 `on_stopped` 不同）。
- **结果一致性**：`result` 与 `last_run_result()` 一致；`error` 即 `run_exception()`（无异常则空）。
- **异常隔离**：`on_finished()` 抛出的异常被框架捕获，记入 `finish_exception()`，**不影响** `run_result()`。
- **约束**：回调内不得再调控制器 API（`stop()` 会被状态机拒绝，`start()` 返回 `self_stop_denied`）；回调应短小，它跑在 worker 线程上，会推迟 `stop()` 返回。

## 错误码与结果

```cpp
enum class error_code {
    ok,               // 成功
    already_running,  // 正在执行，禁止重入
    already_stopping, // 正在停止，禁止重入
    not_running,      // 未在运行（对空闲任务调 stop）
    self_stop_denied, // 在控制器自身线程（任务/tick/结束回调）内调 start/stop（避免 join 自身）
    resource_error,   // 线程创建失败
};
```

| 调用 | 可能返回 |
|---|---|
| `start()` | `ok` / `already_running` / `already_stopping` / `self_stop_denied` / `resource_error` |
| `stop()` | `ok` / `already_stopping` / `not_running` / `self_stop_denied` |

```cpp
enum class run_result { none, completed, stopped, failed };
```

- `last_run_result()`：最近一次运行的结果（自然完成 / 收到停止后结束 / 抛异常）。first-wins，只写一次。
- `run_exception()`：`run()`/内部异步抛出的首个异常（无则空）。
- `tick_exception()`：`on_tick()` 抛出的首个异常（无则空）。
- `finish_exception()`：`on_finished()` 抛出的首个异常（无则空）。不影响 `last_run_result()`。

典型判断业务成败：

```cpp
if (ctl.last_run_result() == run_result::completed) { /* 成功 */ }
else if (ctl.last_run_result() == run_result::failed) { /* 失败, 可 rethrow run_exception() */ }
else { /* stopped 或 none */ }
```

## 生命周期与约束（必读）

1. **`stop()` 是阻塞调用**：请求停止 → 等待任务真正停止（sync=等 `run()` 返回；async=等完成句柄 ready）→ 执行 `on_stopped` 回调。回调运行在**调用 `stop()` 的线程**上。worker 收尾时会先在 worker 线程上执行 `on_finished()`，故 **`on_finished` 先于 `on_stopped`**，且 `stop()` 返回时 `on_finished` 必已执行完毕。
2. **析构自动停止**：析构时若在运行，自动请求停止并 join；**不会触发 `on_stopped`**；不抛异常。但 worker 收尾的 **`on_finished()` 仍会执行**（任务确实结束了）。
3. **析构不得与 `start()`/`stop()` 并发**（未定义行为）。外部需保证串行（如界面线程不可同时销毁）。
4. **回调内不得再调控制器 API**：`on_tick()`/`on_finished()`/`on_stopped()` 内调 `start()`/`stop()` 会被自调用/防重入防护拒绝（返回 `self_stop_denied` 或 `already_stopping`/`not_running`），但这是误用，请避免。
5. **`run()` 与 `on_tick()` 在不同线程并发**：共享状态自行线程安全。
6. **协作式停止**：业务不感知 token，`stop()` 会一直等业务内容执行结束返回。绝大多数情况请用 `token.wait_for()` 做"睡眠+轮询"。
7. **防重入是"控制器"粒度的**：同一控制器同一时刻只有一个运行实例；需要并发跑多个任务请用多个控制器。**框架不阻止两个控制器共享同一个 `task` 对象**——那属于业务侧数据竞争，分型与对策见 `docs/task-runner-reentrancy.md`。

## 组件独立使用

不经过 `task_controller`，也可单独用两个通用工具。

**周期执行器**（如每分钟上报心跳）：

```cpp
auto ctrl = stop_control::create();
periodic_publisher pub(1s, [this] { ping(); });
pub.start(ctrl->token());

// 需要停止时：
ctrl->request_stop();   // 或 pub.stop()（最多阻塞当前一个周期）
pub.stop();
```

**停止源**（任务/线程间协作式取消标记）：

```cpp
auto ctrl = stop_control::create();
auto tok  = ctrl->token();

std::thread worker([tok] {
    while (!tok.stop_requested()) {   // 感知停止
        tok.wait_for(100ms);
        do_step();
    }
});
// ...
ctrl->request_stop();
worker.join();
```

## 构建与集成

- 本仓库已配置：`add_library(task_runner INTERFACE)`（含 pthread），主程序与测试已链接。
- 移植到其他项目：**拷贝 `task_runner/` 目录**，`target_link_libraries(your_target PRIVATE task_runner)`（或等价地加 include 路径与 `-pthread`）。无其他依赖，C++17 即可。
- 业务代码只需包含：`#include "task_runner/task_controller.hpp"`。若只用停止源/发布器，可只包含 `stop_token.hpp` / `periodic_publisher.hpp`。

## 测试

- 单测：`tests/task_runner/` — `TestStopToken`、`TestPeriodicPublisher`、`TestTaskController`（场景矩阵逐行覆盖 SC-09~28 + 任务结束回调 SC-30~38）。
- 压测：`tests/task_runner/TestStress.cpp` — 并发 hammer、同时多线程 stop、多轮复用、高频 tick 等 9 项有界用例；另有 `DISABLED_HeavyCycles`（5000 轮）/`DISABLED_HeavyHammer` 激进用例：
  ```bash
  ctest --test-dir build --output-on-failure
  ./build/tests/unit_tests --gtest_also_run_disabled_tests --gtest_filter=Stress.DISABLED_*
  ```
- 运行演示：`./build/CplushMultiThread`（场景 1 同步 / 场景 2 异步 / 场景 3 自然完成）。

## 延伸阅读

- `docs/task-runner-usage-guide.md`：**按场景索引**的使用指南（执行/停止按钮、进度、多任务、排查清单…）。
- `examples/`：上述场景的**可运行程序**（清单见 `examples/README.md`）。
- `docs/plan-task-runner-framework.md`：框架整体设计与场景矩阵（SC-01~29）。
- `docs/plan-on-finished-callback.md`：任务结束回调 `on_finished` 的设计（SC-30~38）。
- `docs/task-runner-reentrancy.md`：任务级「防重入」的需求分型与业务侧实现。
- `docs/data_race_detection.md`：多线程共享变量竞争检测（TSan / Clang 注解）。

## 常见坑

| 坑 | 后果 | 对策 |
|---|---|---|
| `run()` 里不查 token / 不用 `wait_for()` | `stop()` 永久阻塞 | 长循环包一层 `token.wait_for(10~200ms)` |
| `on_tick()` 直接读写 `run()` 的变量 | 数据竞争 / 未定义行为 | 进度等共享状态用 `std::atomic` 或加锁 |
| `on_tick()`/`on_finished()`/停止回调里调 `ctl.start()/stop()` | 语义错误（虽被防护） | 回调只做发布/后处理，不碰控制器 API；要"重跑"请在外部重新 `start()` |
| 异步任务忘记实现/返回完成句柄 | `task_mode::async` 无完成信号，`stop()` 阻塞 | 来源①用 `std::async(...).share()`；来源②用 promise+完成回调 |
| 事件驱动异步不感知停止 | 停止请求后完成回调不触发，框架一直等 | 在异步链中订阅 token，确保最终 set_value |
| 两个控制器共享同一个 `task` 对象 | 两个 worker 并发进同一对象 → 数据竞争 | 每个控制器配各自的 task 实例；或按 `docs/task-runner-reentrancy.md` 加独占防护 |
| `on_finished()` 里做耗时工作 | 推迟 worker 退出 → `stop()` 被拖慢 | 回调保持短小，耗时收尾交给业务自己的线程 |
| 与 `start()/stop()` 并发析构 | 未定义行为 | 外部保证串行销毁 |