# 新增任务结束回调 on_finished（task_runner）

- 日期: 2026-09-14
- 作者: Claude
- 状态: 已实施（实现 + 测试 + demo + 文档均完成，52/52 用例通过）
- 关联: 框架 `src/include/task_runner/`；主计划 `docs/plan-task-runner-framework.md`

## 背景

`task_runner` 框架原先**只在停止路径上有回调**：`stop(on_stopped)` 的 `on_stopped` 仅在显式调用 `stop()` 时触发。而**任务自然完成**（`run()` 自己返回、没人调 `stop()`）时，`finalize()` 只写结果、置 `idle`、停发布器，**不执行任何用户代码**。

调用方只能靠轮询 `running()` + `last_run_result()` 感知结束——有延迟、要占一个外部线程，且拿不到"结束"这一刻做后处理（上传结果 / 清理资源 / 通知界面）。

## 目标

- `runnable_task` 新增 `virtual void on_finished(run_result result, std::exception_ptr error) {}`（与 `on_tick()` 对称）。
- 每轮 `start()` 到任务结束，`on_finished` **恰好触发一次**，覆盖三种结束（completed / stopped / failed），sync + async 均适用。
- 结果与 `last_run_result()` **严格一致**；`run_exception()` 非空时同步传入 `error`。
- `on_finished` 抛异常被捕获记录，不影响 `run_result`，不崩溃，控制器仍可复用。
- 把"任务级防重入"的需求分型与业务侧实现沉淀为独立文档 `docs/task-runner-reentrancy.md`。

**非目标**

- 不改 `on_stopped` 现有语义；不做超时杀任务；不做任务重启；不加新的"取消"语义。
- **不提供任务级（任务对象粒度）防重入机制，也不新增 `error_code` 枚举值**（见"替代方案"）。

## 方案

### 接口

```cpp
// runnable_task.hpp
virtual void on_finished(run_result result, std::exception_ptr error) { ... }  // 默认空实现

// task_controller.hpp
std::exception_ptr finish_exception() const noexcept;   // on_finished 抛出的首个异常
```

### finalize() 的执行顺序（关键）

```
worker: run()/等完成句柄
   │
   ├─ ① 锁内 first-wins 写 result / run_exc，锁内取出最终值 (final_r, run_e)
   ├─ ② request_stop() + wake_all()          // 幂等，通知发布器与业务 token
   ├─ ③ publisher->stop()                    // join tick 线程：此后无 on_tick 并发
   ├─ ④ task->on_finished(final_r, run_e)    // ★ 新增：worker 线程，每轮恰好一次
   │      └ 抛异常 → 捕获、记入 finish_exc、stderr 日志
   └─ ⑤ 锁内置回 idle                        // ★ 收尾最后一步
```

**为什么"置 idle"放在 `on_finished` 之后**：这样不变式

> `running() == false` ⟹ 任务已彻底结束（`on_finished` 已返回、发布器已 join）

在两种结束路径上都成立。若先置 idle，轮询 `running()` 判结束的调用方会与 `on_finished` **竞争**（可能后处理跑在回调之前）。这是实现过程中发现并修正的一处时序问题。

**其余设计点**

- `on_finished` 放在 `publisher->stop()` 之后 → 回调期间不再有 `on_tick` 并发，回调是最后一段用户代码。
- 回调**不持锁**调用 → 避免回调内误调控制器 API 时与 `mutex_` 互锁。
- 回调异常**不影响** `run_result` —— `run_result` 只反映 `run()`/异步完成情况，回调属后处理。
- 析构路径（`~task_controller` 的 request_stop + join）也会走 `finalize()`，故 **`on_finished` 也会触发**（任务确实结束了），与 `on_stopped`「析构不触发」不同。

### start() 补自调用防护

`on_finished` 跑在 worker 线程上，且自然完成路径下此时 `state_` 已是 `idle`。若业务在回调里写 `ctl.start()`（想"重跑"），`start()` 会走到"回收上一轮 phase"那步**对自己的 worker 线程 `join()`** → 抛 `std::system_error`。这是新回调引入的新的可触达误用路径，故在 `start()` 里补上与 `stop()` 同款的自调用防护：

```cpp
if (phase_) {
    const auto tid = std::this_thread::get_id();
    if (phase_->worker.joinable() && phase_->worker.get_id() == tid) return error_code::self_stop_denied;
    if (phase_->publisher && phase_->publisher->thread_id() == tid)     return error_code::self_stop_denied;
    ...
}
```

复用 `error_code::self_stop_denied` 而不新增枚举值（新增值会牵动 `main.cpp`/README 的 switch 与文档）。代价是该名字对 `start()` 场景不够贴切，通过更新 README 中该错误码的说明来弥合语义：**"在控制器自身线程（任务 / tick / 结束回调）内调用 `start()`/`stop()`，避免 join 自身"**。

## 影响范围

| 文件 | 改动 |
|---|---|
| `src/include/task_runner/runnable_task.hpp` | 新增 `on_finished` 虚函数 + `<exception>` 头 + 注释；`self_stop_denied` 说明更新 |
| `src/include/task_runner/task_controller.hpp` | `run_phase::finish_exc`、`finish_exception()`、`finalize()` 调用回调并调整时序、`start()` 自调用防护、`running()`/`stop()`/析构/类头注释 |
| `src/main.cpp` | 两个任务类加 `on_finished` 演示；新增"场景 3：自然完成" |
| `src/include/task_runner/README.md` | 特性 / 核心概念 / 新增「任务结束 on_finished」小节 / 错误码 / 生命周期约束 / 常见坑 / 测试 / 延伸阅读 |
| `tests/task_runner/TestTaskController.cpp` | 新增 SC-30～SC-38；测试任务类加回调记录 |
| `docs/task-runner-reentrancy.md` | 新增：任务级防重入分型与业务侧实现 |

**兼容性**：纯新增虚函数（有默认空实现）+ 纯新增访问器 → 现有业务代码零改动。`start()` 新增的拒绝分支原先属未定义误用，不视为行为破坏。
**性能**：每轮 `start()` 多一次虚调用 + 一次 `try`，无感知。

## 风险与对策

| 风险 | 对策 |
|---|---|
| 结果与 `last_run_result()` 不一致 | 锁内读 `ph->result` 传出；`stop()` 兜底只在 `result==none` 时写，先后由 `join` 保证 |
| 回调与 `on_tick` 并发访问业务状态 | 先 `publisher->stop()` 再回调，回调期间不再有 tick；文档仍要求业务共享状态线程安全 |
| 回调抛异常导致 worker 崩溃 | 全部 try/catch 兜底（含 `catch (...)`），记入 `finish_exc` + stderr |
| 回调内调控制器 API | `stop()` 被状态机拒绝；`start()` 由自调用防护拒绝；文档明令禁止 |
| 回调耗时过长阻塞 worker 退出 | 与 `on_stopped` 同理：会推迟 `stop()` 返回；文档写明"回调应短小" |
| 轮询 `running()` 与回调竞争 | 把"置 idle"移到 `on_finished` 之后，给出 `running()==false ⟹ 回调已返回` 的不变式 |
| async「future 迟迟不完成」（原 SC-27）下回调语义 | `finalize()` 在 `wait()` 返回后才跑，回调必然在完成句柄 ready 后触发一次，不矛盾 |
| `start()` 回滚路径（`resource_error`）可能触发回调 | worker 已启动并跑过 `run()` 时会以 `stopped` 触发；属极罕见的诚实上报，不特殊屏蔽 |
| 多轮复用时重复/漏触发 | 每轮 `start()` 建全新 `run_phase`，`finalize()` 每轮只跑一次 |

## 替代方案

- **`on_finished` 挂在 `task_controller` 上的 `std::function`**：已否决。停止路径之外的装配处要各自重复设置，且与 `on_tick()` 一虚一 `std::function` 风格割裂。
- **同时在 `runnable_task` 与 controller 上都提供**：已否决。API 面翻倍，还需定义优先级。
- **仅在 `stop()` 调用者线程、`stop()` 返回前执行**：已否决。自然完成路径没有 `stop()` 调用者，该路径下无法触发，退化成轮询——正是本次要消除的痛点。
- **`on_finished` 取代 `on_stopped`**：已否决。二者语义正交（"任务结束" vs "停止后处理"），删除会破坏现有用法。
- **本次顺带做任务级（任务对象粒度）防重入**：已否决。A（多份并发）/B（全局互斥）用现有机制零代码即可满足，只有 C（防同一对象被误用）需要写代码，而当前无此场景；改为沉淀成 `docs/task-runner-reentrancy.md`。

## 实施步骤

- [x] 1. `runnable_task.hpp`：`<exception>` + `on_finished` 虚函数 + 注释
- [x] 2. `task_controller.hpp`：`finish_exc`、`finish_exception()`、`finalize()` 调用回调、`start()` 自调用防护、注释
- [x] 3. `src/main.cpp`：回调演示 + 场景 3 自然完成
- [x] 4. `tests/task_runner/TestTaskController.cpp`：SC-30～SC-38
- [x] 5. 更新 `README.md`
- [x] 6. `docs/task-runner-reentrancy.md`
- [x] 7. 构建 + `ctest`（52/52 通过）
- [x] 8. 运行 demo 目视验证

## 测试计划

新增测试任务类：`sync_loop_job` / `throwing_job` / `async_loop_job` / `async_blocked_job` 加回调记录（结果 / 异常 / 线程 id / 计数），新增 `finish_throw_job`（回调内抛异常）。

| # | 用例名 | 断言 |
|---|---|---|
| SC-30 | `FinishFiredOnNaturalComplete` | 自然完成后回调 1 次、`completed`、无异常；`running()==false` 时回调已返回 |
| SC-31 | `FinishFiredOnCoopStop` | 停止路径回调 1 次、`stopped`；**先于 `on_stopped`**、`stop()` 返回前已执行完 |
| SC-32 | `FinishFiredOnFailure` | `throwing_job` → 1 次、`failed`、`error != nullptr` |
| SC-33 | `FinishRunsOnWorkerThread` | 回调线程 == `run()` 线程，且 != 调用方线程 |
| SC-34 | `FinishFiredOnAsyncPath` | async 自然完成 1 次 `completed`；阻塞完成句柄释放前 0 次、释放后 1 次 `stopped` |
| SC-35 | `FinishPerCycleExactlyOnce` | 3 轮 start/stop → 计数 == 3 |
| SC-36 | `FinishNotFiredWhenStartRejectedOrIdleStop` | 空闲 stop + 被拒的 start → 不触发 |
| SC-37 | `FinishExceptionRecordedAndIsolated` | 回调异常 → `finish_exception()` 非空；`run_result` 不受影响；控制器可复用 |
| SC-38 | `FinishFiredOnDestructor` | 运行中析构 → 1 次、`stopped` |

## 验证

1. `cmake -S . -B build && cmake --build build -j` → 编译通过。
2. `ctest --test-dir build --output-on-failure` → **52/52 通过**（原 43 项不回归 + 新增 9 项）。
3. `./build/CplushMultiThread` → 观察：
   - 场景 1/2：`on_finished: result=stopped` 打印在 `on_stopped` **之前**；
   - 场景 3：不调 `stop()`，`on_finished: result=completed` 自动触发，`running()` 变 `false`。
4. TSan 巡检（可选，见 `docs/data_race_detection.md` §2.2）。

## 引用

- 模板：`docs/plan-template.md`
- 框架原设计：`docs/plan-task-runner-framework.md`
- 任务级防重入：`docs/task-runner-reentrancy.md`
- 竞争检测：`docs/data_race_detection.md`
- 实现：`src/include/task_runner/task_controller.hpp`、`runnable_task.hpp`
- 测试：`tests/task_runner/TestTaskController.cpp`
