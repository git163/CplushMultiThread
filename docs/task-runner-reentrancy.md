# task_runner 任务级「防重入」指南

- 日期: 2026-09-14
- 作者: Claude
- 状态: 参考文档（无配套代码改动）
- 关联: `src/include/task_runner/`、`docs/plan-task-runner-framework.md`、`docs/plan-on-finished-callback.md`

## 背景：框架的防重入是"控制器"粒度的

`task_controller` 的防重入只保护**它自己这一个运行实例**：

| 状态 | `start()` | `stop()` |
|---|---|---|
| running | `already_running` | `ok`（阻塞收尾） |
| stopping | `already_stopping` | `already_stopping` |
| idle | `ok` | `not_running` |

因为校验用的是每个控制器**自己的** `mutex_` / `state_` / `phase_`，而 `runnable_task` 只是一个**纯接口**——没有任何锁、原子量或运行状态。

推论：**两个 `task_controller` 可以同时跑同一个 `runnable_task` 对象，框架不会拦。**

```cpp
auto shared_job = std::make_shared<compress_job>();

task_controller a(shared_job, task_mode::sync, 1s);
task_controller b(shared_job, task_mode::sync, 1s);

a.start();   // ok
b.start();   // 也 ok —— 框架不认为这是"重入"
// 两个 worker 线程并发进入同一个 compress_job 对象的 run()/on_tick()
```

最直观的崩法是 async 模式：`fut_` 是任务对象的成员，两边 `run()` 会互相覆盖对方的 `shared_future`，`async_completion()` 返回谁的都是错的。**这是业务侧数据竞争，框架兜不住。**

所以看到"要防重入"这个需求时，先分清下面三种型——**多数情况根本不需要写防护代码。**

## 三种需求分型

| 型 | 真实诉求 | 推荐做法 | 防护代码 |
|---|---|---|---|
| **A** | 同一份任务**逻辑**要跑多份 | 多控制器 + **各自独立的 task 实例** | **0 行** |
| **B** | 全局**互斥**：同一时刻至多一个在跑 | **只用一个控制器**多轮 `start()`/`stop()` 复用同一 task | **0 行** |
| **C** | 防误用：同一个 task 对象不许被两个控制器拿 | 任务对象内加独占标记 | ~15 行 |

关键区别是**"逻辑共享"还是"状态共享"**，以及**互斥的范围**是"单个控制器"还是"整个程序/某个资源"。

## A：同一份逻辑跑多份

逻辑共享（同一个类），状态不共享（不同对象）。

```cpp
class compress_job : public task_runner::runnable_task {
    void run(task_runner::stop_token) override { /* 状态都在 this 里 */ }
};

// 两份并发，各自一个控制器、各自一个实例 —— 天然安全
auto job1 = std::make_shared<compress_job>();
auto job2 = std::make_shared<compress_job>();
task_runner::task_controller ctl1(job1, task_runner::task_mode::sync, 1s);
task_runner::task_controller ctl2(job2, task_runner::task_mode::sync, 1s);

ctl1.start();
ctl2.start();   // 互不影响
```

这才是 `README` 里"需要并发跑多个任务请用多个控制器"的本意：**每个控制器配各自的 task 对象**。

## B：全局互斥（独占设备/文件等）

要的是"同一时刻至多一个任务在跑"，而 `task_controller` 本身就已经提供了这个语义——**一个控制器同一时刻只有一个运行实例**。所以最省事的做法是全程序只用一个控制器，多轮 `start()`/`stop()` 复用同一个 task：

```cpp
task_runner::task_controller ctl(std::make_shared<compress_job>(), task_runner::task_mode::sync, 1s);

// 执行按钮
if (ctl.start() != task_runner::error_code::ok) {
    show_message("任务已在运行");        // already_running / already_stopping
}

// 停止按钮（另一线程）
ctl.stop([]{ do_post_processing(); });
```

`start()` 的返回值就是"互斥"的答案：`already_running` 意味着已经在跑了。**不需要任何额外的锁或标记。**

> 只有当互斥范围**跨多个控制器**时才需要 C 的做法。

## C：同一个 task 对象不许被两个控制器拿

### C-1 在 `run()` 内自防护（推荐，无泄漏风险）

```cpp
#include <atomic>
#include <stdexcept>

// 业务侧基类：给任务对象加"独占运行"防护。
// 注意：run() 声明 final，子类改为实现 do_run()。
class exclusive_runnable_task : public task_runner::runnable_task {
public:
    void run(task_runner::stop_token token) final {
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // 已被另一个控制器占用：抛异常，让上层能从 run_exception() 看到原因
            throw std::runtime_error("exclusive_runnable_task: already running elsewhere");
        }
        struct busy_guard {                        // RAII：正常返回 / 抛异常都释放
            std::atomic<bool>* f;
            ~busy_guard() { f->store(false, std::memory_order_release); }
        } guard{&busy_};

        do_run(token);                             // 子类把原来的 run() 内容挪到这里
    }

protected:
    virtual void do_run(task_runner::stop_token token) = 0;

private:
    std::atomic<bool> busy_{false};
};

class compress_job : public exclusive_runnable_task {
protected:
    void do_run(task_runner::stop_token token) override {
        /* 原 compress_job::run 的内容 */
    }
};
```

**必须知道的代价**：第二个控制器的 `start()` **仍然返回 `ok`**（框架层拦不住），随后该任务立刻以 `failed` 结束。所以判断成败要看 `last_run_result()` / `run_exception()`，**不能只看 `start()` 的返回值**：

```cpp
b.start();                                        // 返回 ok
// ...
b.last_run_result() == run_result::failed        // 实际结果
b.run_exception()                                 // "already running elsewhere"
```

`on_finished(failed, ...)` 也会在第二个控制器上触发一次——如果你的收尾逻辑假设"失败 = 业务出错"，需要额外区分。

### C-2 在调用侧抢锁（能提前知道，但多一个漏放风险点）

把"门"放在控制器外部，UI 线程先抢锁再 `start()`，并用 `on_finished` 作为释放点。

```cpp
std::atomic<bool> g_running{false};               // 跨控制器共享的"门"
task_runner::task_controller ctl(...);

void on_run_clicked() {                           // UI 线程
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) {
        show_message("任务已在运行");              // 直接拦下，连 start() 都不调
        return;
    }
    if (ctl.start() != task_runner::error_code::ok) {
        g_running.store(false);                   // 被拒/回滚不会触发 on_finished → 必须手动放锁
    }
}

// 任务类：
void on_finished(task_runner::run_result, std::exception_ptr) override {
    g_running.store(false);                       // 每轮恰好一次（含析构路径）→ 释放有保证
}
```

**好处**：能在 `start()` 之前就拦下并给出提示，不像 C-1 那样"先返回 ok 再失败"。

**代价**：`start()` 返回 `already_running` / `already_stopping` / `resource_error` 三条分支下都要手动放锁，比 C-1 多一个漏放锁的风险点。

> 释放点选 `on_finished` 而不是 `stop()` 的回调，是因为 `on_finished` 保证**每轮恰好一次、三种结束都触发、析构路径也触发**——不会漏放。

## 与停止 / 析构的交互（两种做法都要注意）

- **协作式停止**：框架只置停止标记，业务自己感知。`stop()` 会一直等到 `run()` 返回（sync）或完成句柄 ready（async）。若你的独占锁在 `run()` 内部等别的锁，可能让 `stop()` 长时间阻塞。
- **析构**：`~task_controller` 会自动 `request_stop` + join，**也会触发 `on_finished`**。所以 C-2 的释放点即使在"忘了调 `stop()`"的情况下也不会泄漏。
- **回调内不要调控制器 API**：`stop()` 会被状态机拒绝，`start()` 返回 `self_stop_denied`。
- **`running()` 与回调的时序**：框架在 `on_finished` 返回后才置回 idle，故 `running() == false` 蕴含回调已执行完毕。

## 常见坑

| 坑 | 后果 | 对策 |
|---|---|---|
| 两个控制器共享同一个 task 对象 | 两个 worker 并发进同一对象 → 数据竞争 / 未定义行为 | 每个控制器配各自实例（A）；或加独占防护（C） |
| 用 `static` 变量当互斥标记 | 忘记在异常路径释放 → 永久锁死 | 用 RAII（C-1 的 `busy_guard`）或用 `on_finished` 释放（C-2） |
| 在 `run()` 里 `throw` 表达"忙" | `run_result` 变成 `failed`，与真业务失败混淆 | 用 C-2 提前拦下，或在 `on_finished` 里区分错误文案 |
| 以为"一个控制器能并发跑多个任务" | 做不到（同一时刻只有一个运行实例） | 用多个控制器 + 多个实例 |
| 共享 `shared_ptr` 却以为框架会保护 | 框架只保护控制器自身状态 | 见本文开头"背景" |

## 引用

- 框架设计：`docs/plan-task-runner-framework.md`
- 任务结束回调：`docs/plan-on-finished-callback.md`
- 竞争检测：`docs/data_race_detection.md`
- 框架使用说明：`src/include/task_runner/README.md`
