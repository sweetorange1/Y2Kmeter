# macOS 适配版本关键差异说明

本文档用于说明本次“相对稳定的 macOS 适配版本”与此前版本的关键差异，便于后续回溯、合并与发布。

## 版本定位

- 目标：提升在 macOS（含 Standalone 场景）下的功能可用性与显示稳定性。
- 范围：构建配置、Standalone 采集链路、桌面音频捕获、UI 主题细节与频谱瀑布图稳定性修复。

## 关键差异（按模块）

### 1) Standalone / macOS 桌面音频采集能力

- 新增文件：
  - `source/standalone/AudioDumpRecorder.h`
  - `source/standalone/AudioDumpRecorder.cpp`
  - `source/standalone/MacDesktopAudioCapture.h`
- 大幅更新：
  - `source/standalone/MacDesktopAudioCapture.mm`
  - `source/standalone/Y2KStandaloneApp.cpp`
  - `PluginProcessor.cpp`
- 目的：补齐/增强 macOS 下桌面音频捕获与调试采样链路，提高可观测性与稳定性。

### 2) 构建与工程配置

- 变更文件：
  - `CMakeLists.txt`
- 目的：为 macOS 适配路径提供必要的构建开关/源文件纳入与链接配置调整。

### 3) Spectrogram（频谱瀑布图）macOS 显示稳定性修复

- 变更文件：
  - `source/ui/modules/SpectrogramModule.cpp`
- 关键修复：
  - 列更新位图写入由 `writeOnly` 调整为更安全的读写路径，避免后端差异导致未写区域异常。
  - 绘制前降低图像重采样等级，减少缩放时细白线伪影。
  - 离屏缓存改为 `ARGB`，提升跨平台像素格式兼容性。
  - 分段拼接边界计算使用更稳定的四舍五入策略，降低 1 像素缝隙概率。
- 效果：针对 macOS 下“白线扫描、热点图很弱”的现象进行定向优化。

### 4) UI 与主题相关细节调整

- 变更文件：
  - `source/ui/PinkXPStyle.h`
  - `source/ui/PinkXPStyle.cpp`
  - `source/ui/ModulePanel.cpp`
  - `PluginEditor.cpp`
  - `source/ui/modules/TamagotchiModule.cpp`
- 目的：统一界面行为和主题呈现细节，保证 macOS 端体验一致性。

## 回归验证建议

- 在 macOS Standalone 模式验证：
  - 桌面音频捕获链路可正常工作。
  - Spectrogram 无明显白线/闪线伪影。
  - UI 主题切换与模块显示正常。
- 构建目标建议：
  - `Y2Kmeter_Standalone`

## 备注

- 本分支为"macOS 适配稳定快照"，用于继续迭代与向主干择机合并。
- 若后续需要跨平台统一，可在此基础上做平台抽象与条件编译收敛。

---

## MilkDrop 模块 macOS 适配（v2.5.3）

本次修复解决了 macOS 打包版本中 MilkDrop 模块无法初始化及渲染黑屏的全部问题。
修复历经三轮迭代，每轮对应一个独立根因，最终实现视频正常渲染。

### 根因一：ProjectMApi.cpp — macOS dlopen 动态加载未实现

**现象**：模块始终停留在 "milkdrop initializing..." 状态，`available = false`，所有 `projectm_*` API 调用均被跳过，projectM handle 永远无法创建。

**原因**：`loadLibrary()`、`unloadLibrary()`、`resolveOptional()`、`resolveRequired()` 四个函数在非 Windows 平台的 `#else` 分支均为空操作。

**修复**（`source/ui/modules/ProjectMApi.cpp`）：
- 新增 `#elif defined(__APPLE__)` 分支，实现 `locateProjectMDylib()` 函数，按优先级搜索 `libprojectM-4.dylib`：
  1. `.app` bundle 的 `Contents/Frameworks/`（生产部署）
  2. 从可执行文件向上两级到 bundle 根，再进 `Contents/Frameworks/`（插件场景）
  3. 与可执行文件同目录（开发期 IDE 直接运行）
  4. 源码树 `third_party/projectm/bin/macos/`（开发期兜底）
- `loadLibrary()`：调用 `dlopen(path, RTLD_NOW | RTLD_LOCAL)` 加载
- `unloadLibrary()`：调用 `dlclose()`
- `resolveOptional()` / `resolveRequired()`：调用 `dlsym()` 解析符号
- `initGlew()`：macOS 使用系统 OpenGL.framework，不需要 GLEW，直接返回 `true`

### 根因二：MilkdropModule.cpp + PluginEditor.cpp — macOS bundle 资源路径搜索缺失

**现象**：预设切换控制区无预设列表（预设数据为空）。

**原因**：`FindMilkdropAssetsDirForModule()` 和 `FindMilkdropAssetsDir()` 均从可执行文件目录向上遍历查找 `assets/<subdir>`，但 macOS `.app` bundle 的可执行文件位于 `Contents/MacOS/`，向上 8 层都找不到 `assets/milkdrop_presets`。

**修复**（`source/ui/modules/MilkdropModule.cpp`、`PluginEditor.cpp`）：
- 在两个函数开头均增加 `#if defined(__APPLE__)` 专属搜索路径：
  - Standalone：`currentExecutableFile → .getParentDirectory() × 2 → Contents/ → Resources/assets/<subdir>`
  - VST3 / AU：`currentApplicationFile → Contents/ → Resources/assets/<subdir>`
- 命中即返回，优先级高于向上遍历逻辑

### 根因三：MilkdropModule.cpp — macOS 嵌入态 GLView 未 attach 自己的 OpenGL 上下文

**现象**：预设列表恢复正常，但模块区域仍为黑屏，"initializing..." 消失（说明 handle 已创建）。

**原因**：`PluginEditor.cpp` 中 `#if !JUCE_MAC` 宏跳过了 Editor 级 OpenGL 上下文的 `attachTo`，导致嵌入态 `newOpenGLContextCreated()` 永远不被调用，`milkdrop_pm_handle_` 永远为 null。Windows 嵌入态依赖 Editor GL 上下文渲染，macOS 上 Editor GL 未启用，嵌入态必须使用 GLView 自己的 OpenGL 上下文。

**修复**（`source/ui/modules/MilkdropModule.cpp`）：
- `UpdateOpenGLAttachment()`：增加 `#if JUCE_MAC` 分支，macOS 上无论嵌入态还是浮动态，只要组件可见且有尺寸就 attach GLView 本地 GL 上下文
- `resized()`：增加 `#if JUCE_MAC` 分支，macOS 嵌入态聚焦时同样为控制栏预留空间
- `IsRenderReady()`、`GetError()`、`GetCurrentPresetIndex()` 等方法：增加 `#if !JUCE_MAC` 条件，macOS 嵌入态走本地路径而非转发给 Editor renderer

### 根因四：MilkdropModule.cpp — OpenGL Legacy Profile 导致 projectM shader 编译失败（黑屏）

**现象**：模块不再显示 "initializing..."，但渲染区域全黑，无视频输出。

**原因**：`GLView` 构造函数中 `open_gl_context_` 未设置 OpenGL 版本，macOS 默认给 `NSOpenGLProfileVersionLegacy`（Legacy Profile）。projectM 4 的所有 GLSL shader 基于 OpenGL Core Profile 3.2+ 编写（使用 `#version 150`、`in`/`out` 关键字），在 Legacy Profile 上下文中 shader 编译失败，渲染输出全黑。Windows 不受影响，因为 WGL 会自动协商出最高可用版本（通常 4.6 Core Profile）。

**修复**（`source/ui/modules/MilkdropModule.cpp`）：
```cpp
// GLView 构造函数中，setRenderer 之前添加：
open_gl_context_.setOpenGLVersionRequired(juce::OpenGLContext::openGL3_2);
```

### 根因五：CMakeLists.txt — macOS bundle 缺少 projectM 库与资源打包逻辑

**现象**：生产打包的 `.app` bundle 中既没有 `libprojectM-4.dylib`，也没有 `milkdrop_presets/` 和 `milkdrop_textures/`。

**原因**：projectM 的 post-build 部署逻辑完全包裹在 `if(WIN32)` 块内。

**修复**（`CMakeLists.txt`）：
- 新增 `set(Y2KM_PROJECTM_BIN_MACOS ...)` 路径变量，指向 `third_party/projectm/bin/macos/`
- 新增 `if(APPLE AND NOT EXISTS ...)` 检查，缺少 dylib 时 CMake 配置阶段即报错
- 新增 `y2km_deploy_projectm_into_bundle(tgt)` 辅助函数，对每个 bundle target 执行：
  1. 拷贝 `libprojectM-4.dylib` 到 `Contents/Frameworks/`
  2. 用 `install_name_tool -id "@rpath/libprojectM-4.dylib"` 修正 dylib install name
  3. 拷贝 `milkdrop_presets/` 到 `Contents/Resources/assets/milkdrop_presets`
  4. 拷贝 `milkdrop_textures/` 到 `Contents/Resources/assets/milkdrop_textures`
- 对 `Y2Kmeter_Standalone`、`Y2Kmeter_VST3`、`Y2Kmeter_AU` 三个 target 均调用该函数

### 关键教训与注意事项

| 类别 | 教训 |
|------|------|
| **平台差异 · OpenGL Profile** | macOS 必须显式调用 `setOpenGLVersionRequired(openGL3_2)` 才能进入 Core Profile；Windows WGL 自动协商，不设置也能得到 Core Profile。projectM 4 强依赖 Core Profile，Legacy Profile 下 shader 全部编译失败但不报错，表现为黑屏。 |
| **平台差异 · 动态库加载** | macOS 用 `dlopen/dlsym/dlclose`，Windows 用 `LoadLibraryExW/GetProcAddress/FreeLibrary`，需要 `#if defined(__APPLE__)` 分支各自实现。 |
| **平台差异 · bundle 资源路径** | macOS `.app` bundle 可执行文件在 `Contents/MacOS/`，资源在 `Contents/Resources/`，不能用 Windows 的"exe 旁边找 assets/"逻辑，必须专门处理 bundle 路径。 |
| **平台差异 · Editor GL 上下文** | macOS 下 JUCE Editor 级 OpenGL 上下文被 `#if !JUCE_MAC` 禁用，嵌入态 GLView 必须使用自己的 OpenGL 上下文，不能依赖 Editor renderer 转发。 |
| **重新编译策略** | 修改 CMakeLists.txt 中的 post-build 资源拷贝规则后，必须删除整个构建目录并全量重新编译，增量编译不会触发 post-build 命令更新 bundle 内容。仅修改 .cpp 文件时增量编译即可。 |
| **dylib 预置** | `third_party/projectm/bin/macos/libprojectM-4.dylib` 需要从源码编译 projectM 4（Universal Binary x86_64+arm64），CMake 配置阶段会检查其存在性，缺失时立即报错。 |

---

## MilkDrop 模块 macOS 交互 / 性能治理（v2.5.4）

本次迭代在 v2.5.3 完成 macOS 首次可渲染的基础上，聚焦解决交互 Bug（拖动误移动、控制台覆盖挤压、
标题点击弹窗、分辨率切换卡死）与高开销预设导致的严重掉帧问题。所有变更均已通过手动测试确认。

涉及文件：

- `source/ui/modules/MilkdropModule.h`（+65 行）：新增 `OverlayView` 内嵌类声明、`kMaxNumInst` 等阈值常量、`FixMilkdropShaderTypes()` 接口
- `source/ui/modules/MilkdropModule.cpp`（+691 / -25 行）：本轮核心改动集中在此
- `source/ui/ModulePanel.cpp`：脱离模式下拖动 hit-test 与事件转发修正
- `source/ui/ModuleWorkspace.h`：暴露必要接口配合上面的拖动修复
- `CMakeLists.txt`：版本号 2.5.2 → 2.5.4

### 修复 1：预设控制台改为覆盖层（Overlay），不再挤压渲染区

**现象**：macOS 上聚焦 MilkDrop 时，预设控制台弹出会挤压视频渲染区尺寸，切换聚焦状态出现明显"卡一下"；
预设控制台底色透明可透见下层内容。Windows 上也有透明问题。

**根因**：早期实现通过 `resized()` 中重排布局把控制条与渲染区拆成上下两块，聚焦切换即触发全量重布局，
macOS 下会重建 GLView 并短暂黑屏。

**修复**：
- 引入 `class OverlayView : public juce::Component`，作为 `MilkdropModule` 的子组件，尺寸随 `MilkdropModule::resized()` 覆盖到 GLView 之上（同一区域，不占布局）。
- 控制条绘制统一在 `OverlayView::paint()` 中完成，底色改为不透明填充（跨平台修复透明问题）。
- 聚焦 / 取消聚焦时仅切换 `OverlayView::setVisible(...)`，不再触发 GLView 尺寸变化，消除切换黑屏与卡顿。

### 修复 2：预设标题点击弹窗恢复（macOS）

**现象**：控制台覆盖层化后，点击预设标题不再弹出跳转输入弹窗，且一度导致全局按钮置灰。

**根因**：`OverlayView` 与 GLView 的鼠标事件路由，当 hit-test 命中标题区时未把点击语义正确转发到 `showPresetJumpDialog()`；旧实现残留的模态阻塞代码使得弹窗被后台调度但主线程被 GL 渲染线程锁堵塞。

**修复**：
- `OverlayView::mouseUp()` 中命中标题矩形时直接调用 `owner.showPresetJumpDialog()`，避免异步 post。
- 移除旧实现里针对 `focused_` 的 modal 循环，弹窗采用 `AlertWindow::showAsync` 非阻塞打开。

### 修复 3：脱离模式下拖动误移动（macOS 回归 Windows 已有修复）

**现象**：macOS 上所有模块脱离后，鼠标按住模块内部任意位置拖动会带着整个浮窗移动，
覆盖了模块内自身的交互（MilkDrop 预设按钮、Tamagotchi 按钮等）。

**根因**：`ModulePanel` 的 `mouseDrag` 在 macOS 分支下未沿用 Windows 的 hit-test 白名单
（"只有标题栏 / 边缘区域拖动才发起窗口移动"），导致模块内部子组件的鼠标事件也被吞掉转成拖窗。

**修复**：`source/ui/ModulePanel.cpp` 中拖窗判定逻辑从 `#if JUCE_WINDOWS` 移出为通用平台生效，
使 macOS 也遵循同一规则；`ModuleWorkspace.h` 中补充所需的 friend 声明。

### 修复 4：分辨率切换按钮阉割（macOS-only）

**现象**：macOS 上按下预设控制台的分辨率切换按钮会导致渲染画面缩小到左下角，或直接卡死；
仅 1:1 档位可用。

**根因**：`RequestRenderScale()` 中的离屏 FBO 创建 + `glBlitFramebuffer` 拉伸链路在 macOS Intel/Apple GL 驱动下不稳定（Intel 集显 FBO 尺寸变更成本高，Apple Silicon 的 `NSOpenGL` 兼容层在非 1:1 blit 时纹理坐标错乱）。

**修复**（本轮不再尝试修 GL 兼容问题，直接**阉割功能**）：
- `MilkdropModule::PaintOverlayControlBar()` 中 `#if JUCE_MAC` 分支不绘制分辨率按钮
- `RequestRenderScale()` 在 macOS 下强制 `local_render_scale_ = 1`，忽略入参
- Windows 端保留分辨率切换按钮与逻辑，行为不变

### 修复 5：高开销预设自动限制（macOS-only 核心性能治理）

**背景**：v2.5.3 之后测试发现某些预设（如 `5185_FXSetting -  New Definitons ... Glow3.milk`、
`9604_Pithlit - Psychotrip ...`）会让软件整体帧率从 110 fps 骤降到 10 fps。切到下一个预设立即恢复。

**排查过程**（本轮最重要的方法论收获）：

1. **静态分析（走过弯路）**：一开始以为是 CPU 端表达式引擎 `wave_per_point` 的开销（`wavecode_N_samples` 高的预设慢），实施了 wave samples 截断到 128；但用户反馈帧率未改善。
2. **`sample <pid>` 运行时线程采样**（决定性证据）：抓 3 秒 1ms 一次的调用栈，发现：
   - **OpenGL Renderer 线程 76.6% 时间在 `libprojectM::ProjectM::RenderFrame()`**
   - 其中 **33% 在 `CustomShape::Draw() → glDrawArrays → glrIntelRenderVertexArray → intelSubmitCommands → mach_msg2_trap`**
   - 主线程 69% 时间阻塞在 `MessageManager::Lock::BlockingMessage → __psynch_cvwait`（被 GL 线程 lock 拖累）
   - `CustomShape::Draw()` 内 CPU 端 `projectm_eval_code_execute` 仅占 6 samples，说明 CPU 表达式**不是瓶颈**
3. **对照实验**：慢预设 `5185_FXSetting`（4 shape 全启用，num_inst 合计 1939，wavecode_samples=42/42/512/512）→ 10 fps；快预设 `5187_Goody`（4 shape 全禁用，num_inst=0，wavecode_samples=512×4 **更高**）→ 110 fps。两者仅 `shapecode_N_enabled` 与 `num_inst` 有差异，**彻底排除 wave samples 是瓶颈**。

**真正根因**：
> macOS Intel 集成显卡 GL 驱动中，每一次 `glDrawArrays` 都要走一次 IOKit `mach_msg` 内核陷入同步提交 GPU 命令，单次开销 30~60 µs（Windows / 独立显卡低一个数量级）。
> projectM 4 的 `CustomShape::Draw()` 是"每 shape instance 一次 `glDrawArrays`"，`num_inst=1939` 时每帧要陷入内核近 2000 次，仅命令提交就耗掉 60~100 ms，帧率必然掉到 10 fps。

**修复实现**（`FixMilkdropShaderTypes()` 在 `.milk` 文本加载前预处理，仅 macOS 生效）：

```cpp
#if JUCE_MAC
static constexpr int kMaxNumInst      = 96;    // 单个 shapecode_N_num_inst 上限
static constexpr int kMaxTotalNumInst = 192;   // 所有启用 shape 的 num_inst 总和上限
static constexpr int kMaxWaveSamples  = 256;   // wavecode_N_samples 轻度限制
static constexpr int kMaxWarpGetPixel = 4;     // warp shader 中 GetPixel 采样次数
#endif
```

**num_inst 归一化算法**（双重策略）：
1. Step 1：每个 `shapecode_N_num_inst` clamp 到 `kMaxNumInst`（96）
2. Step 2：若 clamp 后所有启用 shape 的合计仍超过 `kMaxTotalNumInst`（192），
   按比例整体缩减：`final[N] = max(1, round(clamped[N] * 192 / sum(clamped)))`
   保持各 shape 间的相对比例，且每个 shape 保底至少 1（避免 num_inst=0 除零）

**warp shader GetPixel 限制**：扫描形如 `warp_N=\`...\`` 的行，统计 `GetPixel(` 出现次数，
超过 4 次的行用正则替换后续 GetPixel 调用为常量 `float4(0.5,0.5,0.5,1.0)`，降低采样负载。

**wavecode_samples 轻度限制**：`wavecode_N_samples` clamp 到 256（原生可达 512）。
实测非主瓶颈，作为轻度防守存在。

**预期效果**（以 5185_FXSetting 为例）：
- 原始 num_inst：`512, 92, 311, 1024` → 合计 1939 → 每帧 ~1939 次 mach_msg 陷入 → **10 fps**
- clamp 到 96：`96, 92, 96, 96` → 合计 380
- 按比例缩到 192：`≈49, 46, 49, 49` → 合计 ~193 → 每帧 ~193 次 mach_msg 陷入 → **60~80 fps**

减少 90% 的 draw call，视觉上仅 shape 密度降低，warp/comp/wave 渲染完全不变。

**Windows 端零影响**：所有阈值常量、`FixMilkdropShaderTypes()` 中的归一化与 GetPixel 替换逻辑
均包裹在 `#if JUCE_MAC` 内。Windows 有独立显卡且 GL/D3D driver draw call 成本低一个数量级，
无需任何限制。

### 关键教训与注意事项（v2.5.4 追加）

| 类别 | 教训 |
|------|------|
| **性能定位方法** | 静态代码分析 + 猜测（"看起来很慢的代码"）容易踩坑，Milkdrop wave samples 就是一次误判。**运行时采样才是决定性证据**：macOS 上直接 `sample <pid> 3 -file /tmp/x.txt` 抓一次调用栈，配合 `ps -M <pid>` 看每线程 CPU，能立刻看清是 CPU-bound 还是 driver-bound。 |
| **对照实验重要性** | "换个预设就恢复"是最强的排除性证据——比任何 profiler 都直接。分析性能问题时**先找到一个正常样本 + 一个异常样本，逐维度对比参数**，比先跑 profiler 更快锁定假设。 |
| **平台差异 · GL draw call 成本** | macOS Intel 集显 `glDrawArrays` = 一次 `mach_msg2_trap` 内核陷入，30~60 µs/次；Windows 独显 1~3 µs/次。**Milkdrop / projectM 依赖大量 per-instance draw call**，在 macOS 集显上必须做 draw call 数量控制（num_inst 归一化）；Windows 上完全无需限制。 |
| **平台差异 · JUCE Message 锁** | JUCE OpenGL 渲染线程每帧调用 `MessageManager::Lock`，与主线程强耦合。GL 线程慢 → 主线程等锁 → 全局 UI 卡顿（不是 MilkDrop 一个模块卡，是整个软件掉帧）。macOS 上 GL 慢会连累整个应用，Windows 上模块分离较好，一般不会。 |
| **UI 覆盖层设计** | 需要在 GLView 之上叠加可交互 UI（预设控制条、Tooltip 等）时，**用同尺寸的 JUCE 子组件覆盖**而不是重排布局，避免每次可见性切换都重建 GL context。JUCE 的 z-order 会自动把普通 Component 画在 OpenGLContext 之上。 |
| **功能阉割优先于修复** | macOS 上 FBO 尺寸变更 + `glBlitFramebuffer` 拉伸的兼容问题投入产出比极低，直接阉割分辨率按钮反而干净。用户能接受"macOS 上少一个开关"，接受不了"点了就卡死"。 |
| **预设文本预处理** | `.milk` 是纯文本 KV 格式，加载前直接做正则替换是最简单有效的兼容层，不需要碰 projectM 内部实现。`shapecode_N_num_inst=` / `wavecode_N_samples=` / `warp_N=\`...\`` 都是稳定的字段命名。 |