# examples — 按场景的可运行示例

每个 `.cpp` 编译成一个可独立运行的程序，聚焦**一个**使用场景。是
`docs/task-runner-usage-guide.md`（按场景索引的使用指南）的可执行版本。

## 构建与运行

```bash
cmake -S . -B build
cmake --build build -j

./build/examples/sync_task
./build/examples/async_task
./build/examples/progress_and_finish
./build/examples/multi_task
./build/examples/reusable_components
```

不需要示例时：`cmake -S . -B build -DBUILD_EXAMPLES=OFF`。

## 示例清单

| 文件 | 场景 | 覆盖的指南章节 | 看什么 |
|---|---|---|---|
| `sync_task.cpp` | 界面「执行」/「停止」按钮，任务是可中断的同步耗时循环 | §1 最小可用、§6 可中断写法、§12 错误码 | `on_finished` 打印在 `on_stopped` **之前**；防重入返回 `already_running`；任务结束后 `stop()` 返回 `not_running` |
| `async_task.cpp` | 异步任务的两种完成句柄来源 | §3 异步任务 | 来源① `std::async`；来源② 事件链 + `std::promise`（含"future 为什么必须存下来"的说明） |
| `progress_and_finish.cpp` | 界面显示进度 + 任务结束收尾 | §4 状态发布、§5 结束收尾 | 自然完成也触发 `on_finished(completed)`；`running()==false` 时回调计数已是 1；`run()` 抛异常 → `failed` + `run_exception()` |
| `multi_task.cpp` | 多任务并发 vs 全局互斥 | §8 多任务并发、§9 全局互斥 | 两个控制器 + 两个实例并行；单控制器多轮复用天然互斥 |
| `reusable_components.cpp` | 不要控制器，只用通用工具 | §10 组件独立使用 | `periodic_publisher` 做周期回调；`stop_control`/`stop_token` 做跨线程协作式取消 |

`demo_util.hpp` 是两个 `to_str()` 打印助手，仅供示例使用，不属于库本身。

## 新增示例

1. 在 `examples/` 下加一个 `xxx.cpp`（`#include "demo_util.hpp"` 与 `task_runner/...`）。
2. 在 `examples/CMakeLists.txt` 末尾追加 `add_example(xxx)`。

## 相关文档

- 使用指南（按场景）：`docs/task-runner-usage-guide.md`
- API 参考（按组件）：`src/include/task_runner/README.md`
- 任务级防重入：`docs/task-runner-reentrancy.md`
