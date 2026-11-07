# Y2Kmeter 对 JUCE 的本地补丁

项目把 JUCE 8.0.12 直接放在 `third_party/JUCE/` 里提交（vendored，不是 submodule），并在源码上打了下列补丁。补丁已经提交在 `third_party/JUCE/` 中，**正常构建什么都不用做**。本目录额外留一份按编号排列的 diff，用于：

- 升级 / 重新拉取 JUCE 时把补丁重新打上；
- 本地没有 `third_party/JUCE` 时，CMake 会通过 FetchContent 拉原版 8.0.12，并用 `PATCH_COMMAND` 自动打补丁；
- 配置期校验（`Y2KJucePatches.cmake`）：实际使用的 JUCE 缺少任何补丁标记就报错，防止优化静默丢失。确需用原版 JUCE 构建时传 `-DY2K_ALLOW_UNPATCHED_JUCE=ON`，此时只给警告。

补丁基线是原版 JUCE 8.0.12，即提交 `e4bc0b78` 时的 `third_party/JUCE`。在源码中搜索 `Y2Kmeter patch` / `Y2K patch` 可以找到所有改动点。

| 编号 | 文件 | 内容 | 引入版本 |
| --- | --- | --- | --- |
| 0001 | `juce_PopupMenu.h/.cpp` | `PopupMenu::Options::withParentAreaOverride`：菜单可以超出 userArea（AppBar 驻留条上弹出菜单的定位） | v2.8.6 |
| 0002 | `juce_Displays.cpp`、`juce_Windowing_windows.cpp` | 混合 DPI 多屏：显示器表与窗口几何按物理像素判定 | v2.9.9 |
| 0003 | `juce_Font.cpp`、`juce_FontOptions.cpp` | Font 比较快速路径：同一共享实例直接判等；`FontOptions::tie()` 改为返回引用 tuple，不再拷贝 String / vector | v3.0.2（mac 性能） |
| 0004 | `juce_OpenGLGraphicsContext.cpp` | mac 下 VBO orphaning（先 `glBufferData(nullptr)` 再 `glBufferSubData`），避免驱动隐式同步卡顿 | v3.0.2（mac 性能） |
| 0005 | `juce_OpenGLContext.h/.cpp` | 新增 `OpenGLContext::setComponentRepaintsPacedToDisplay()`（仅 mac）：组件 repaint 合并到下一个 CVDisplayLink 刷新再出帧。显示器刷新信号超过 100ms 没来时自动退回立即渲染 | v3.0.2（mac 性能） |
| 0006 | `juce_OpenGLContext.h/.cpp`、`juce_OpenGLRenderer.h` | `OpenGLContext::takeRenderStats()`：渲染线程分段耗时（sleep / 锁等待 / renderOpenGL / paintComponent / 合成 / swap / 重绘面积）；`OpenGLRenderer::renderOpenGLPreLock()`（仅 Windows 调用）：在拿消息锁之前执行，Editor 用它在锁外渲染 projectM，让消息线程与 GL 并行 | Win 性能优化 |

## 升级 JUCE 时

1. 用新版 JUCE 替换 `third_party/JUCE/`。
2. 在新 JUCE 根目录执行（重复执行是安全的，已打过的补丁会跳过）：
   ```sh
   cd third_party/JUCE
   cmake -DPATCH_DIR=../juce_patches -P ../juce_patches/ApplyJucePatches.cmake
   ```
3. 有补丁打不上时，手动移植改动，然后重新导出该补丁（见下文）。
4. 重新配置 CMake，看到 `Y2K: JUCE 本地补丁校验通过` 即可。

## 新增 / 修改 JUCE 补丁时

1. 直接改 `third_party/JUCE/` 下的源码，并在改动处加 `[Y2Kmeter patch]` 注释。
2. 导出 diff，路径要相对 JUCE 根目录：
   ```sh
   git diff --relative=third_party/JUCE -- third_party/JUCE/modules/<改动文件> \
       > third_party/juce_patches/00NN-<简述>.patch
   ```
3. 在 `Y2KJucePatches.cmake` 的 `Y2K_JUCE_PATCH_MARKERS` 中补充“文件|标记”。
4. 更新上表。
5. 自检：把基线导出到临时目录，依次打全部补丁，结果应与 `third_party/JUCE/modules` 一致：
   ```sh
   mkdir -p /tmp/jb && git archive e4bc0b78 third_party/JUCE | tar -x -C /tmp/jb
   cd /tmp/jb/third_party/JUCE && cmake -DPATCH_DIR=<repo>/third_party/juce_patches -P <repo>/third_party/juce_patches/ApplyJucePatches.cmake
   diff -rq /tmp/jb/third_party/JUCE/modules <repo>/third_party/JUCE/modules
   ```
