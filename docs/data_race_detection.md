# 多线程共享变量竞争检测指南

> 目的：快速定位"被多线程改写的变量"是否存在数据竞争（data race），以及如何系统性防止。
> 适用范围：本项目（C++17，mac/linux，task_runner 框架）以及后续移植的多线程代码。

两种方法不是"二选一"，而是互补：

- **静态护栏（Clang Thread-Safety 注解）**：编译期的纪律，防止"将来新写的无锁访问"溜进代码。
- **动态证人（ThreadSanitizer / TSan）**：运行时的验证，抓真实发生的竞争，定位到两边的调用栈。

推荐打法：**先上 TSan 兜底验证，后补注解做纪律约束**。具体见文末「推荐策略」。

---

## 1. 概览对比

| 维度 | Clang Thread-Safety 注解 | ThreadSanitizer (TSan) |
|---|---|---|
| 检测时机 | 编译时，扫源码 | 运行时，观察真实执行的访问序列 |
| 是否需要提前标注 | **要**。不标注的变量它完全看不见 | 不用。任何共享内存访问都在监控下 |
| 漏报 | 高（没标注、标错的都漏） | 低（只漏"本次运行没撞上"的冷路径） |
| 误报 | 低，但对无锁/原子场景会假报 | 极低 |
| 能查什么 | 只查"访问是否持锁"这一类纪律问题 | 真实数据竞争，含双方调用栈、`文件:行` |
| 查不到什么 | 原子/无锁结构、锁顺序、死锁、容器内部竞争 | 冷路径竞争；非 race 的逻辑 bug |
| 运行时开销 | 零 | 慢 2~5 倍，需独立构建目录 |
| 防回归能力 | 强——新增无锁访问编译直接报错 | 弱——要重新跑到才可能发现 |

**关键差异一句话**：
- 注解管的"快"是**改完立刻知道**——有人不守纪律时，编译器当场拒绝；
- TSan 管的"快"是**一报就高置信度**——真竞争直接给出两个线程的完整栈。但前提是这次运行真的执行到了冲突路径。

---

## 2. ThreadSanitizer（TSan）— 运行时检测

### 2.1 原理

TSan 在编译期对所有共享内存访问**插桩**，运行时记录每个访问的线程与地址，利用 C++ 内存模型的 happens-before 关系推断是否存在"无同步的读写竞争"。它**不是**靠人标注，全程自动；且能正确识别 `std::atomic`、无锁结构的 happens-before，这类代码它照样查。

### 2.2 用法

> TSan 必须**重新编译全部代码**才会生效，不能只在已有 `build/` 里临时加 flag。用独立构建目录。

**方式 A：命令行直接指定**（不依赖项目改动，通用）

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

**方式 B：给 CMake 加 `ENABLE_TSAN` 选项**（推荐固化到项目，之后一键开关）

在 `CMakeLists.txt` 的 `add_compile_options(-g)` 之后加：

```cmake
# 多线程竞争检测：cmake -S . -B build-tsan -DENABLE_TSAN=ON
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -O1)
    add_link_options(-fsanitize=thread)
endif()
```

之后使用：

```bash
cmake -S . -B build-tsan -DENABLE_TSAN=ON
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

注意：用 `add_compile_options()` 全局生效，`tests/` 子目录的目标（通过 `add_subdirectory`）会同样带上 flag，无需单独处理。

### 2.3 输出解读

有竞争时，TSan 报 `WARNING: ThreadSanitizer: data race`，并给出双方调用栈，直接定位到 `worker` / `main` 等线程：

```
WARNING: ThreadSanitizer: data race
  Write of size 8 at 0x... by thread T2:
    #0 task_controller::tick_body(...)  src/.../task_controller.hpp:275
  Previous read of size 8 at 0x... by main thread:
    #1 task_controller::stop(...)       src/.../task_controller.hpp:154
  Location is global 'counter'
```

### 2.4 注意事项

- **冷路径漏报**：TSan 只抓"这次运行确实执行的访问"。没覆盖到的时序/低频分支，跑十次也未必命中。所以测试要**多次、压测**多线程路径，而不是跑一遍就完。
- **mac 兼容性**：Linux + clang 上最稳；mac 上 AppleClang 也支持 `-fsanitize=thread`，但较新系统存在个别兼容限制（shadow 内存映射类报错）。若在 mac 上遇到 TSan 自身崩溃类问题，改用 Linux 环境验证。
- **独立构建**：TSan 染色后的产物只用于检测，绝不进发布版本（运行时开销 2~5 倍）。
- 常用环境变量：`TSAN_OPTIONS=halt_on_error=1`（一报即停）、`exitcode=0`（让测试不因 race 失败，仅打印）。

---

## 3. Clang Thread-Safety 注解 — 编译期检测

### 3.1 原理

给共享变量打上"由哪把锁保护"的注解，用 `-Wthread-safety` 让 **clang 在编译时静态检查**每次访问是否合法地持有那把锁。核心是把"别忘了加锁"从"靠人记得"变成"编译器拒绝"——**新增的无锁访问直接编译失败**。

三要素缺一不可：
1. 变量声明标注 `GUARDED_BY(mutex)` 或 `PT_GUARDED_BY`；
2. 编译开启 `-Wthread-safety`；
3. 访问时确实通过 `std::lock_guard` / `scoped_lock` / `unique_lock` 持锁。

### 3.2 开启方式

mac 上的 AppleClang 与 linux 的 clang 都支持。**GCC 目前不实现这些注解**，需用 clang 编译。

在 `CMakeLists.txt` 中按需开启（建议做成独立 option）：

```cmake
option(ENABLE_THREAD_SAFETY "Enable Clang thread-safety annotations" OFF)
if(ENABLE_THREAD_SAFETY)
    add_compile_options(-Wthread-safety)  # clang 专用
endif()
```

### 3.3 代码示例（贴合本项目的 task_controller）

本项目 [task_controller.hpp](src/include/task_runner/task_controller.hpp) 里 `mutex_` 保护 `phase_` 与 `state_`，正是注解的典型场景：

```cpp
class task_controller {
    mutable std::mutex mutex_;
    std::shared_ptr<run_phase> phase_ GUARDED_BY(mutex_);  // 声明：读写 phase_ 必须先持 mutex_
    controller_state state_ GUARDED_BY(mutex_);

public:
    bool running() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);  // 持锁，编译器认可，通过
        return state_ == controller_state::running;
    }

    controller_state peek_state() const {        // 编译报错：读取 state_ 未持锁
        return state_;
    }
};
```

注解宏定义放在类前一处（各编译器通用写法），或直接在本项目公共头文件里提供：

```cpp
#if defined(__clang__)
#define GUARDED_BY(x) __attribute__((guarded_by(x)))
#define PT_GUARDED_BY(x) __attribute__((pt_guarded_by(x)))
#define REQUIRES(x) __attribute__((requires_capability(x)))
#define ACQUIRE(x) __attribute__((acquire_capability(x)))
#else
#define GUARDED_BY(x)
#define PT_GUARDED_BY(x)
#define REQUIRES(x)
#define ACQUIRE(x)
#endif
```

若共享的是裸指针本身（如跨线程传的成员指针），用 `PT_GUARDED_BY`；普通值类型用 `GUARDED_BY`。

### 3.4 局限性（务必了解，否则会误用）

- **必须主动标注**：不标注的变量它完全不管。标错锁（注释说是 A 锁却写着 B 锁）也会静默失效。
- **无锁/原子场景误报**：`std::atomic`、无锁队列、拷贝后在锁外初始化等**合法且无锁**的模式，注解表达不了，要么标不了、要么假报。本项目 `stop_token` 这类原子标志就不适合用注解。
- **管不到容器内部**：容器（如 `std::vector`）被"编译期不可见的"别的线程成员函数改时，注解不能代查。
- **不查锁顺序/死锁**：这是另一类问题（锁顺序违反），TSan 里的 lock-order 检查或静态分析另做。

### 3.5 误报（false positive）与规避

会，但要区分两类：

**（1）契约不一致导致的"误报"（最常见，其实是标错了）**
`GUARDED_BY` 是你向 clang 声明的契约。契约写错，clang 就按错的检查：
- 把 **`std::atomic` / 无锁结构**成员也标上锁 → 编译器要求本不需要的锁，报假错。原子本身就是同步原语，**不标锁**即可消除。
- 把**构造后只读 / 初始化后再不修改**的成员标了锁 → 只读路径无须加锁，却被要求持锁。
  → 对策：契约要与实际同步策略完全一致；不满足"持锁才可访问"的变量就不要标注。

**（2）分析边界导致的真误报（clang 看不到持锁上下文）**
clang 的 `-Wthread-safety` 以"编译单元内可见 + 内联"为准，跨边界时它无法验证你确实持锁：
- **回调 / 函数指针**：本项目里 `periodic_publisher` 回调 `tick_body()` 这类入口，锁在类内其实已正确上，但经过回调边界分析丢失 → 报假错。
- **跨 TU / `const&` 传参到函数内不加锁访问**。
  → 对策：
  - `no_thread_safety_analysis`：对**已人工确认安全**的函数加注解，跳过该函数分析：
    ```cpp
    void tick_body(...) __attribute__((no_thread_safety_analysis));
    ```
  - 结构上让持锁范围与函数体对齐——尽量在函数内 `scoped_lock` 后访问，别把"锁在 A 函数加、在 B 函数访问"拆开。
  - 若确实需要按调用方传锁、由被调函数访问的常规设计，用 `REQUIRES(mutex_)` 声明"调用本函数须已持有 mutex_"：
    ```cpp
    void do_locked_job() REQUIRES(mutex_);  // 调用者必须在持锁时调用
    ```

> 小结：注解的价值前提是"契约真实"。它会把"你标注的契约 vs 访问是否遵守"管得死死的；若契约本身撒了谎（原子当有锁、锁在外层函数没传进来），报出来的"误报"往往是在提醒你同步设计名不副实。

---

## 4. 不依赖工具的快速自查清单

扫代码时，命中以下特征的共享变量即为"高危"，即使还没报竞争也要警惕：

1. **全局变量 / 函数内 `static` 局部变量**——唯一无常的跨线程共享状态，默认按有风险处理。
2. **类的非 const、非原子成员**，被两个及以上线程使用（读+写，或写+写）——亦是本框架成员变量的主要风险面。
3. **跨线程传递的裸指针 / `shared_ptr`**——共享所有权但无锁同步。
4. **容器被一个线程改、另一个线程读**——如 `clear_` / `reset_` / `emplace_` 这类原地改动出现在多线程路径上。

---

## 5. 推荐策略

1. **现在**：先给 CMake 落地 `ENABLE_TSAN`（见 2.2 方式 B），对现有测试跑多轮 TSan，清掉已存在的竞争。
2. **代码长起来后**：挑 2~3 个「被多线程读写最频繁」的类成员加 `GUARDED_BY` 注解做样板（如 `task_controller` 的 `phase_`/`state_`），并开启 `-Wthread-safety`。
3. **CI/习惯**：注解当门禁（编译期拦截新错误），TSan 当巡检（每次测试都过一遍）。