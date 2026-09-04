# CplushMultiThread

C++17 多线程相关功能示例与封装项目（占位描述，待补充）。

当前包含框架模块：**task_runner** — 可执行/可停止任务框架（详见 `src/include/task_runner/README.md`）。

## 环境要求

- CMake ≥ 3.20
- 支持 C++17 的编译器（GCC ≥ 7、Clang ≥ 5、MSVC ≥ 19.14）
- 可选：GDB 或 LLDB 用于调试；VSCode + 推荐的扩展可获得最佳体验。

## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

默认构建类型为 `RelWithDebInfo` — 有优化又保留调试符号，崩溃时仍能拿到可用堆栈。真正的发布构建：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

## 运行

```bash
./build/CplushMultiThread
```

运行 task_runner 的 sync + async 演示：每秒状态发布、协作式停止、on_stopped 后处理与错误码分支。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

测试文件位于 `tests/` 下，命名 `Test*.cpp` — `tests/CMakeLists.txt` 通过 `gtest_discover_tests` 自动发现，新增测试只需在 `tests/` 里加一个 `TestXxx.cpp`。

## 调试

在 VSCode 打开本项目（`code .`）。项目自带调试配置（`.vscode/launch.json`）：

- **(gdb) Launch CplushMultiThread** / **(lldb) Launch CplushMultiThread** — 构建并调试主程序。Linux 选 gdb，macOS 选 lldb。
- **(gdb) Run unit_tests** / **(lldb) Run unit_tests** — 构建并调试测试程序。可在任一 `Test*.cpp` 中打断点后按 F5。

启动前任务（preLaunchTask）为 `cmake build`，调试会话开始时二进制总是最新的。

不使用 VSCode 时，可从命令行附加：

```bash
gdb --args ./build/CplushMultiThread     # Linux
lldb ./build/CplushMultiThread           # macOS
```

## 项目结构

- `docs/` — 设计文档与 plan（使用 `docs/plan-template.md`）
- `src/` — 实现（`.cpp`）
- `src/include/` — 公共头文件（Google C++ 风格）
- `src/include/task_runner/` — 可执行/可停止任务框架（仅头文件），使用文档见 `src/include/task_runner/README.md`
- `tests/` — GTest 单元测试（`Test*.cpp`）

## 约定

见项目根目录 `CLAUDE.md`。