# task_runner 场景使用指南

- 日期: 2026-09-14
- 作者: Claude
- 状态: 参考文档
- 关联: `src/include/task_runner/README.md`（组件与 API 参考）、`docs/plan-task-runner-framework.md`

> 这份文档按**"我要做什么"**索引；`README.md` 按**"这个组件是什么"**索引。两者互补。
> 每节的代码都有对应的**可运行程序**在 `examples/`（清单见 `examples/README.md`）。

## 目录

- [0. 选型索引](#0-选型索引)
- [1. 最小可用：执行/停止按钮](#1-最小可用执行停止按钮)
- [2. 同步任务：一段阻塞循环](#2-同步任务一段阻塞循环)
- [3. 异步任务：业务自带线程或事件链](#3-异步任务业务自带线程或事件链)
- [4. 状态发布：显示进度](#4-状态发布显示进度)
- [5. 结束收尾：on_finished](#5-结束收尾on_finished)
- [6. 让停止"及时"：可中断写法](#6-让停止及时可中断写法)
- [7. 停不下来：不可中断的取舍](#7-停不下来不可中断的取舍)
- [8. 多任务并发](#8-多任务并发)
- [9. 全局互斥：同一时刻只跑一个](#9-全局互斥同一时刻只跑一个)
- [10. 不要控制器：组件独立使用](#10-不要控制器组件独立使用)
- [11. 移植到其他项目](#11-移植到其他项目)
- [12. 错误码 → 该怎么做](#12-错误码--该怎么做)
- [13. 排查清单](#13-排查清单)

---

## 0. 选型索引

```
任务内容是一段自己的耗时代码（循环 / 阻塞调用）？
├─ 是，且能拆成"分块 + 检查停止" ──────────→ §2 同步任务
└─ 否，业务自己有线程池 / 事件回调链 ───────→ §3 异步任务

需要停止按钮吗？
├─ 需要，且希望尽快停 ─────────────────────→ §6 可中断写法
└─ 业务无法中断，只能等它跑完 ─────────────→ §7 不可中断的取舍

要显示进度吗？
├─ 要 ────────────────────────────────────→ §4 状态发布
└─ 不要 ──────────────────────────────────→ tick_period 调大，少唤醒（见 §4 末尾）

任务结束后要收尾（上传/清理/通知 UI）吗？ ───→ §5 on_finished

要同时跑多个任务？ ────────────────────────→ §8 多任务并发
全局只能有一个在跑？ ──────────────────────→ §9 全局互斥
两者都不是，只是要个周期回调 / 取消标记？ ──→ §10 组件独立使用
```

| 你的处境 | 看哪节 |
|---|---|
| 第一次接入，写个能跑能停的骨架 | §1 |
| 想搞清 sync/async 怎么选 | §2 / §3 |
| 停止要"按下去就停" | §6 |
| 停止按钮点了半天没反应 | §6 / §7 / §13 |
| 界面要显示百分比 | §4 |
| 想在任务结束时做后处理 | §5 |
| `start()` 返回了错误码，不知道怎么处理 | §12 |
| 出 bug 了 | §13 |

---

## 1. 最小可用：执行/停止按钮

UI 上两个按钮、任务耗时几十秒、能中断——这是框架的典型场景。

```cpp
#include "task_runner/task_controller.hpp"
using namespace std::chrono_literals;

class my_job : public task_runner::runnable_task {
public:
    void run(task_runner::stop_token token) override {
        for (int i = 0; i < 100; ++i) {
            if (token.stop_requested()) break;   // ① 感知停止
            do_step(i);                          // ② 业务内容
            token.wait_for(10ms);                // ③ 睡眠 + 可被停止唤醒
            done_ = i + 1;
        }
    }
    std::atomic<int> done_{0};
};

// ④ 装配一次，长期持有（不要放在按钮回调的局部变量里！）
auto job = std::make_shared<my_job>();
task_runner::task_controller ctl(job, task_runner::task_mode::sync, 1s);

// 执行按钮
if (ctl.start() != task_runner::error_code::ok) {
    show_message("任务已在运行");                 // 见 §12
}

// 停止按钮（阻塞，建议放独立线程，见下）
ctl.stop([] { show_message("已停止"); });
```

三个容易踩的点：

1. **`task_controller` 要长期持有**。放在按钮回调的局部作用域里，函数一返回就析构 → 自动停止 + join，任务立刻被打断。
2. **`stop()` 是阻塞调用**：它会等任务真正停下来。在 UI 线程直接调会卡界面。放到独立线程：
   ```cpp
   std::thread([&ctl] { ctl.stop([]{ notify_ui_stopped(); }); }).detach();
   ```
   `on_stopped` 跑在这个线程上，**不是** UI 线程——里面不能直接碰控件。
3. **析构必须与 `start()`/`stop()` 串行**，别在任务还在跑的时候从另一个线程销毁控制器（未定义行为）。

---

## 2. 同步任务：一段阻塞循环

**什么时候用**：任务内容就是一段自己的代码（循环、计算、阻塞式 IO）。

**框架怎么驱动**：`start()` 起一个 worker 线程执行 `run()`；**`run()` 返回 = 任务完成**。

```cpp
class compress_job : public task_runner::runnable_task {
public:
    void run(task_runner::stop_token token) override {
        for (auto& file : files_) {
            if (token.stop_requested()) break;   // 分块检查点
            compress_one(file);
            token.wait_for(1ms);                 // 让出 CPU + 及时响应停止
        }
    }
};
task_runner::task_controller ctl(std::make_shared<compress_job>(), task_runner::task_mode::sync, 1s);
```

- `run()` 里的代码跑在 worker 线程上，**不要**在里面直接操作 UI。
- 不查 token 也能跑，但停止会失效——见 §6 / §7。

---

## 3. 异步任务：业务自带线程或事件链

**什么时候用**：业务内容不由你"顺序执行"，而是交给别的东西——线程池、第三方 SDK 的回调、事件循环。

**框架怎么驱动**：`run()` 只**发起**、快速返回；业务给一个 `std::shared_future<void>` 完成句柄，框架 `wait()` 它来判定真正完成。

### 来源①：一段阻塞序列，只是不想占用框架的 worker

```cpp
class download_job : public task_runner::runnable_task {
public:
    void run(task_runner::stop_token token) override {
        fut_ = std::async(std::launch::async, [this, token] {
            while (!token.stop_requested() && chunk_ < n_) {
                fetch_chunk(chunk_++);           // 阻塞内容，分块查停止
            }
        }).share();
    }
    std::shared_future<void> async_completion() const override { return fut_; }
    void on_tick() override { publish(chunk_.load(), n_); }
private:
    std::shared_future<void> fut_;
    std::atomic<int> chunk_{0};
};
task_runner::task_controller ctl(std::make_shared<download_job>(),
                                 task_runner::task_mode::async, 1s);
```

### 来源②：真·事件驱动（不需要线程、不需要循环）

```cpp
class event_job : public task_runner::runnable_task {
public:
    void run(task_runner::stop_token token) override {
        stopper_ = token;
        start_sdk_chain(/* 完成回调 */ [this] { done_promise_.set_value(); });
    }
    std::shared_future<void> async_completion() const override {
        return done_promise_.get_future().share();
    }
private:
    task_runner::stop_token stopper_;
    mutable std::promise<void> done_promise_;    // 只 set 一次；mutable 因 async_completion() 是 const
};
```

**怎么选**

| 你的业务 | 用哪个 |
|---|---|
| 就是一段阻塞代码，只是想跑在别处 | 来源① |
| 已经有自己的线程池 / 执行器 | 来源①（把内容 post 过去，用 promise 桥接） |
| 第三方 SDK 完成后回调你 | 来源② |

> ⚠️ **最重要的一条**：完成句柄必须**最终会 ready**。如果停止请求发生后业务链不再触发完成回调，`stop()` 会**永远阻塞**。事件驱动的业务要在自己的链里订阅 token，确保无论如何最终 `set_value()`（或提前 set）。

---

## 4. 状态发布：显示进度

`on_tick()` 每 `tick_period`（构造第 3 个参数，默认 1s）在**独立线程**被调用一次。

```cpp
task_runner::task_controller ctl(job, task_runner::task_mode::sync, 1s);  // 1s 一次
//                                                              ↑ 周期
```

```cpp
class my_job : public task_runner::runnable_task {
public:
    void run(task_runner::stop_token token) override {
        for (int i = 0; i < total_; ++i) {
            if (token.stop_requested()) break;
            do_step(i);
            done_ = i + 1;                        // ① 原子写
            token.wait_for(10ms);
        }
    }
    void on_tick() override {
        const int d = done_.load();               // ② 原子读
        ui_post([d] { progress_bar->set(d); });   // ③ 投递到 UI 线程执行
    }
private:
    static constexpr int total_ = 100;
    std::atomic<int> done_{0};                    // ④ 跨线程共享必须原子/加锁
};
```

**三个必须注意的点**

1. `on_tick()` 与 `run()`（或业务异步）**并发执行**。共享的进度变量必须 `std::atomic` 或加锁——否则是数据竞争。
2. `on_tick()` **不在 UI 线程**。控件只能在 UI 线程碰，所以要把值投递过去（Qt 的 `QMetaObject::invokeMethod`、Android 的 `Handler`、Web 的 `postMessage` 等）。
3. `on_tick()` 抛异常会被框架捕获记入 `tick_exception()`，不会中断 `run()`。

**不需要状态发布怎么办**：框架每次 `start()` 都会起一个发布器线程，所以把 `tick_period` 设大（如 `std::chrono::hours(1)`）能减少空转唤醒。当前版本没有"不起发布器"的开关。

---

## 5. 结束收尾：on_finished

任务**真正结束**时调用一次，用来上传结果、清理资源、通知界面。三种结束都触发——**包括"没人调 `stop()`、任务自己跑完"**。

```cpp
void on_finished(task_runner::run_result result, std::exception_ptr error) override {
    switch (result) {
        case task_runner::run_result::completed: notify_ui_success();      break;
        case task_runner::run_result::stopped:   notify_ui_cancelled();    break;
        case task_runner::run_result::failed:    report_ui_error(error);   break;
        case task_runner::run_result::none:      break;   // 不会出现
    }
}
```

**时序保证（可以直接依赖）**

| 保证 | 说明 |
|---|---|
| 每轮 `start()` 恰好一次 | 含 sync / async 路径 |
| 跑在 worker 线程 | 不是调 `stop()` 的线程，不能直接碰 UI |
| `stop()` 返回时已执行完 | 且**先于** `on_stopped` 回调 |
| `running() == false` ⟹ 已执行完 | 框架在回调返回后才置回 idle，所以轮询 `running()` 等结束是安全的 |
| 析构触发的停止也会执行 | 与 `on_stopped` 不同（后者析构时不触发） |

**注意**：回调抛异常会被捕获记入 `finish_exception()`，**不影响** `last_run_result()`。回调要保持短小——它跑在 worker 线程上，做太久会推迟 `stop()` 返回。

---

## 6. 让停止"及时"：可中断写法

框架是**协作式停止**：它只置一个标记，业务自己决定何时退出。想让"停止"按下去就生效，就得在业务代码里放**检查点**。

### ✅ 睡眠用 `token.wait_for()`

```cpp
// 好：可被停止立刻唤醒
if (token.wait_for(100ms)) break;     // 返回 true = 已请求停止
```

```cpp
// 差：这 100ms 内停止请求只能干等
std::this_thread::sleep_for(100ms);
if (token.stop_requested()) break;
```

### ✅ 分块 IO：每块之前查一次

```cpp
while (!token.stop_requested() && (chunk = next_chunk())) {
    transfer(chunk);                  // 单块耗时可控
}
```

### ✅ CPU 密集：也要插检查点

```cpp
for (int i = 0; i < n; ++i) {
    if ((i & 0xFF) == 0 && token.stop_requested()) break;   // 每 256 次查一次，开销可忽略
    heavy_compute(i);
}
```

### ❌ 反模式

```cpp
void run(task_runner::stop_token) override {
    std::this_thread::sleep_for(10s);     // 10 秒内完全无法响应停止
    blocking_read();                       // 一次不可切分的阻塞调用
}
```

**检查点的粒度 = 停止响应的延迟。** 选一个你能接受的延迟（通常 10~200ms），按这个粒度放检查点。

---

## 7. 停不下来：不可中断的取舍

如果业务**本质上无法中断**（调用了没有超时的第三方阻塞函数、不能拆分的原子操作）：

1. **`stop()` 会一直等到它跑完**——这是协作式停止的定义，框架不会强杀。
2. **`on_finished` 仍然会触发**（以 `stopped` 结果），`on_stopped` 也会。
3. **别在 UI 线程等它**：`stop()` 放到独立线程，UI 立刻返回"正在停止…"的状态。

```cpp
// 停止按钮：不阻塞 UI
std::thread([&] {
    ctl.stop([]{ notify_ui_stopped(); });     // 可能等很久
}).detach();
```

4. 如果连"等它跑完"都不可接受，那要在**业务层**解决：给阻塞调用加超时、换成支持取消的 API，或干脆让它跑在**独立的子进程**里（框架管不到进程级强杀）。

---

## 8. 多任务并发

**需要并发跑多个任务 → 多个控制器，每个配各自的 task 实例。**

```cpp
auto job_a = std::make_shared<compress_job>("a.zip");
auto job_b = std::make_shared<compress_job>("b.zip");

task_runner::task_controller ctl_a(job_a, task_runner::task_mode::sync, 1s);
task_runner::task_controller ctl_b(job_b, task_runner::task_mode::sync, 1s);

ctl_a.start();     // 互不影响
ctl_b.start();
```

同一个类、**不同对象**（逻辑共享、状态不共享）→ 天然安全。

> ⚠️ **不要**把同一个 task 对象交给两个控制器。框架的防重入是**控制器**粒度的，它拦不住这件事——两个 worker 会并发进同一个对象，那是数据竞争。分型与对策见 **`docs/task-runner-reentrancy.md`**。

每个控制器会起 **2 个线程**（worker + 周期发布器）。要跑 10 个并发任务就是 20 个线程，注意系统开销。

---

## 9. 全局互斥：同一时刻只跑一个

**要的是"全局同一时刻至多一个任务在跑"** → 全程序只用一个控制器，多轮 `start()`/`stop()` 复用同一个 task。

```cpp
task_runner::task_controller ctl(std::make_shared<compress_job>(),
                                 task_runner::task_mode::sync, 1s);

// 执行按钮
if (ctl.start() != task_runner::error_code::ok) {
    show_message("任务已在运行");        // already_running / already_stopping
}

// 停止按钮
ctl.stop([]{ /* 后处理 */ });

// 任务自然跑完后还能再 start() —— 每次 start() 都是全新的一轮，可反复复用
```

`start()` 的返回值本身就是"互斥"的答案，**不需要额外的锁**。

---

## 10. 不要控制器：组件独立使用

框架里有两个通用工具可以脱开 `task_controller` 单独用。

### 周期回调执行器（如每 30s 上报心跳）

```cpp
#include "task_runner/periodic_publisher.hpp"

auto ctrl = task_runner::stop_control::create();
task_runner::periodic_publisher pub(std::chrono::seconds(30), [] { send_heartbeat(); });
pub.start(ctrl->token());

// 需要停时
ctrl->request_stop();     // 或 pub.stop()
pub.stop();               // 退出并 join（幂等）
```

tick 抛异常被捕获记入 `tick_exception()`，循环继续不崩溃。

### 协作式取消标记（跨线程通知"该停了"）

```cpp
#include "task_runner/stop_token.hpp"

auto ctrl = task_runner::stop_control::create();
auto tok  = ctrl->token();                 // 只读视图，发给工作线程

std::thread worker([tok] {
    while (!tok.stop_requested()) {
        tok.wait_for(100ms);               // 睡眠 + 可被唤醒
        do_step();
    }
});

// 任意线程
ctrl->request_stop();                      // 幂等
worker.join();
```

`stop_control` 可触发停止，`stop_token` 只能感知——把这个区分做进类型系统，比传裸 `atomic<bool>` 安全。

---

## 11. 移植到其他项目

框架是**仅头文件**的，零第三方依赖（标准库 + 线程），C++17 即可。

1. 拷贝 `src/include/task_runner/` 目录到你的项目。
2. 加 include 路径 + 链接 pthread：

```cmake
# 方式 A：照搬本项目的 INTERFACE 库
add_library(task_runner INTERFACE)
target_include_directories(task_runner INTERFACE path/to/include)
target_link_libraries(task_runner INTERFACE Threads::Threads)
target_link_libraries(your_target PRIVATE task_runner)

# 方式 B：不用 CMake 目标，直接加路径
target_include_directories(your_target PRIVATE path/to/include)
target_link_libraries(your_target PRIVATE pthread)
```

3. 业务代码只需 `#include "task_runner/task_controller.hpp"`；只用停止源/发布器则可只包含对应头文件。

**平台**：mac / linux 均已验证（本仓）。编译器需支持 C++17（`std::optional` / `shared_future` / 结构化绑定等）。

---

## 12. 错误码 → 该怎么做

```cpp
enum class error_code {
    ok, already_running, already_stopping, not_running, self_stop_denied, resource_error,
};
```

| 调用 | 错误码 | 含义 | 你该怎么做 |
|---|---|---|---|
| `start()` | `ok` | 已启动 | — |
| | `already_running` | 已经在跑 | 正常：提示"任务已在运行"，**忽略即可** |
| | `already_stopping` | 正在停止中 | 提示"正在停止，请稍候"；等 `stop()` 返回后再 `start()` |
| | `self_stop_denied` | **在控制器自身线程里调了 `start()`** | 误用：多半是在 `on_tick`/`on_finished`/`run` 里调了。改到外部线程调 |
| | `resource_error` | 线程创建失败 | 系统资源不足：提示用户重试，或降低并发任务数 |
| `stop()` | `ok` | 已停止 | — |
| | `not_running` | 任务已经结束了 | **正常**：任务可能已自然完成，去查 `last_run_result()` |
| | `already_stopping` | 已有停止在进行 | 忽略（`on_stopped` 只会执行一次） |
| | `self_stop_denied` | **在控制器自身线程里调了 `stop()`** | 误用：同上 |

**常见组合**

```cpp
// 执行按钮：只关心"能不能启动"
if (ctl.start() != task_runner::error_code::ok) show("任务已在运行");

// 停止按钮：not_running 不是错误
auto r = ctl.stop([]{ /* 后处理 */ });
if (r == task_runner::error_code::not_running) {
    // 任务已经自己跑完了 —— 用 on_finished 做收尾，或直接刷新界面
}
```

**结果查询**（判断业务成败）：

```cpp
using task_runner::run_result;
switch (ctl.last_run_result()) {
    case run_result::completed: /* 成功 */ break;
    case run_result::failed:    /* 失败，ctl.run_exception() 取异常 */ break;
    case run_result::stopped:   /* 被停止打断 */ break;
    case run_result::none:      /* 没跑过 / 还没结束 */ break;
}
```

---

## 13. 排查清单

| 现象 | 最可能的原因 | 去哪看 |
|---|---|---|
| 点了停止，任务还在跑 | `run()` 里没有检查点，或检查点粒度太粗 | §6、§7 |
| `stop()` 卡住不返回 | 异步任务的完成句柄永远不 ready | §3 的 ⚠️ |
| 进度条不动 / 数值乱跳 | `on_tick` 直接读写 `run()` 的变量（数据竞争） | §4 |
| 界面偶尔崩溃 | 在 `on_tick`/`on_finished` 里直接操作了控件 | §4、§5 |
| `start()` 返回 `self_stop_denied` | 在 `on_tick`/`on_finished`/`run` 里调了控制器 API | §12 |
| 任务莫名被取消 | 控制器是局部变量，作用域结束被析构 | §1 的 ① |
| `stop()` 在界面线程里调用，界面卡死 | `stop()` 是阻塞调用 | §1、§7 |
| 两个任务跑同一个对象，结果错乱 | 两个控制器共享了同一个 task 对象 | §8、`docs/task-runner-reentrancy.md` |
| 析构时崩溃 | 析构与 `start()`/`stop()` 并发 | §1 的 ③ |
| 怀疑有数据竞争 | — | `docs/data_race_detection.md`（TSan） |

更细的坑表见 `src/include/task_runner/README.md#常见坑`。

## 引用

- 组件与 API 参考：`src/include/task_runner/README.md`
- 框架设计：`docs/plan-task-runner-framework.md`
- 任务结束回调设计：`docs/plan-on-finished-callback.md`
- 任务级防重入：`docs/task-runner-reentrancy.md`
- 竞争检测：`docs/data_race_detection.md`
