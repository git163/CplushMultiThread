# docs/ppt — 项目汇报 PPT

`task_runner_overview.pptx`：22 页的项目汇报（16:9，可直接编辑），内容为**模块设计 + 框架图 + 项目优势 + 使用用例**。
由 `build_deck.py` 生成，**改内容改脚本、不改 pptx**。

## 重新生成

```bash
python3 -m venv .venv
.venv/bin/pip install python-pptx
.venv/bin/python docs/ppt/build_deck.py
# → docs/ppt/task_runner_overview.pptx
```

## 怎么改内容

所有文字都在 `build_deck.py` 里，按页一个函数：

| 函数 | 页码与内容 |
|---|---|
| `s01_cover` | 1. 封面 |
| `s02_value` | 2. 一句话价值 + 3 个关键数字 |
| `s03_pain` | 3. 痛点：手写要处理的 6 件事 |
| `s04_arch` | 4. 三层结构 |
| `s05_components` | 5. 四个组件一览 |
| `s06_module_files` | 6. **模块设计① 头文件与依赖图** |
| `s07_class_diagram` | 7. **模块设计② 核心类图（框架图）** |
| `s08_state_machine` | 8. **模块设计③ 状态机与关键时序** |
| `s09_advantage1` | 9. 优势① 业务只填 `run()` |
| `s10_result` | 10. 优势② 三态结果 + 错误码 |
| `s11_callbacks` | 11. 优势③ 两个回调（on_tick / on_finished） |
| `s12_thread_model` | 12. 优势④ 线程模型（run_phase 生命周期） |
| `s13_safety` | 13. 优势⑤ 异常不逃逸 + 防重入 |
| `s14_portable` | 14. 优势⑥ 仅头文件 / 零依赖 / 可移植 |
| `s15_case1` | 15. 用例① 执行 / 停止按钮 |
| `s16_case2` | 16. 用例② 异步任务两种完成句柄来源 |
| `s17_case3` | 17. 用例③ 进度发布 + 结束收尾 |
| `s18_case4` | 18. 用例④ 多任务并发 / 全局互斥 |
| `s19_case5` | 19. 用例⑤ 组件独立使用 |
| `s20_quality` | 20. 工程质量（测试 / TSan / 文档） |
| `s21_start` | 21. 上手三步 |
| `s22_summary` | 22. 总结 |

版式（颜色、字体、页边距、代码块/表格/指标卡样式）由文件顶部的**设计系统常量**和各 `helper`
统一控制，改一处即全局生效。新增页：加一个 `sNN_xxx(prs)` 函数，并加进 `main()` 的 `builders` 列表。

### 画图用的图元（模块设计三页在用）

| 图元 | 作用 |
|---|---|
| `arrow(slide, x1, y1, x2, y2, color, dash)` | 带箭头的直线连接符（`dash=True` 画虚线） |
| `arrow_label(slide, text, left, top)` | 箭头旁的说明文字 |
| `uml_class(slide, left, top, width, name, attrs, methods, accent, stereotype)` | UML 类框（类名 / 字段 / 方法三分栏），**返回底边 y** 方便接箭头 |
| `diamond(slide, cx, cy, filled)` | UML 菱形：`filled=True` 复合（■）、`False` 聚合（□） |
| `state_box(slide, left, top, w, h, text, accent)` | 状态机里的状态框 |
| `node(...)`（在 `s06` 内） | 依赖图里的模块框 |

`left` / `top` / `width` 都接受 `Inches(...)`；**EMU 相减后是普通 int**（`Inches(a) - Inches(b)` 不再有
`.inches`），要换算时用 `Emu(int(...))` 或先除 `914400`。

### 几个改动时的注意点

- **正文里的 `\n` 是换行**（`note()` / 步骤卡会自动拆段落）；但 **OOXML 的代码块里 `\n` 表示换行本身**，`code_box()` 按行拆。
- **代码块高度要够**：按 `行数 × 字号 × 1.2 × 0.95 / 72`（英寸）+ 0.24 估算，留 5% 余量。
- 字体用的是 `Calibri`（拉丁）+ `Microsoft YaHei`（中文）+ `Consolas`（代码）—— 三者都随
  Office 安装（Mac / Windows 都有）。若目标机器用别的字体，改顶部 `FONT_LATIN` / `FONT_EA` / `FONT_CODE`。

## 自查版式

改完跑一次溢出 / 越界检查（按字号估算文字渲染高度，比对容器高度与页面下边界）：

```bash
.venv/bin/python docs/ppt/check_layout.py docs/ppt/task_runner_overview.pptx
# → 共 22 页，问题 0 处
```

没有 LibreOffice 时无法真正渲染，用 macOS 的 Quick Look 生成缩略图肉眼复核：

```bash
qlmanage -t -s 1300 -o /tmp/out docs/ppt/task_runner_overview.pptx
```

## 数据来源

PPT 里的数字都取自仓库当前状态，改动代码后请同步核对：

| 数字 | 来源 |
|---|---|
| 652 行 / 4 个 .hpp | `wc -l src/include/task_runner/*.hpp` |
| 52 / 52 测试（+2 项默认禁用的激进压测） | `ctest --test-dir build` |
| 30 / 11 / 6 / 6 / 1 各套件用例数 | `./build/tests/unit_tests --gtest_list_tests` |
| 0 TSan 报告 | `TSAN_OPTIONS=suppressions=$PWD/tsan.supp ctest --test-dir build-tsan` |
| 5 个示例 | `ls examples/*.cpp` |
