# macOS 适配版本关键差异说明

本文档用于说明本次“相对稳定的 macOS 适配版本”与此前版本的关键差异，便于后续回溯、合并与发布。

---

## 适配范围（当前打包版本 v2.6.5）

> 以下信息基于对 `Y2Kmeter.app/Contents/MacOS/Y2Kmeter` 的 `file` / `otool -l` 检查得出，反映的是 **当前 DMG 分发包的真实兼容性边界**，而非源码理论支持范围。

### 0. 版本演进摘要（重要）

| 状态 | 版本 | 主二进制架构 | 最低系统 | 说明 |
|:---:|:---:|:---:|:---:|---|
| ~~旧~~ | ≤ v2.5.6 | 仅 x86_64（单架构） | macOS **15.0** | 未显式设置部署目标 → 被动继承本机 SDK；Apple Silicon 用户被迫走 Rosetta 2 |
| ✅ 新 | ≥ v2.5.7 | **x86_64 + arm64（Universal）** | macOS **12.3** | 在 `CMakeLists.txt` 顶部显式设置 `CMAKE_OSX_ARCHITECTURES` 与 `CMAKE_OSX_DEPLOYMENT_TARGET`，产出 Universal Binary 并把最低系统下探到 ScreenCaptureKit 的底线 |

以下小节描述的是 **新策略（v2.5.7+）** 下的兼容性边界。

### 1. CPU 架构支持

| 架构 | 主可执行文件 | 内嵌 projectM 动态库 | 用户端表现 |
|------|:---:|:---:|------|
| **Intel（x86_64）** | ✅ 原生 | ✅ | 原生运行 |
| **Apple Silicon（arm64，M1/M2/M3/M4）** | ✅ **原生** | ✅ | **原生运行，不再需要 Rosetta 2** |

- 主二进制现在是 **Universal Binary**（x86_64 + arm64 两个切片）
- 第三方 `libprojectM-4*.dylib` 本身已经是 Universal（x86_64 + arm64），加载时会自动匹配架构
- 验证命令：
  ```bash
  file Y2Kmeter.app/Contents/MacOS/Y2Kmeter
  # 期望输出：Mach-O universal binary with 2 architectures: [x86_64] [arm64]
  ```

### 2. 最低系统版本要求

- **CMake 显式设置**：`CMAKE_OSX_DEPLOYMENT_TARGET = 12.3`
- **LC_BUILD_VERSION 预期内嵌值**：`platform=macOS(1), minos=12.3`
- 即最低支持 **macOS 12.3 Monterey**
- 低于 12.3 的系统（macOS 12.0~12.2 / 11 Big Sur / 10.15 Catalina 等）**无法启动**，会在 dyld 加载阶段被系统拒绝
- 取 12.3 的原因：
  - ScreenCaptureKit（本项目桌面音频捕获依赖）自 macOS 12.3 引入
  - 更低版本 ScreenCaptureKit API 不存在，链接期就无法通过

### 2.1 各 macOS 版本的功能可用矩阵

| macOS 版本 | 主程序启动 | 麦克风采集 | Milkdrop 可视化 | **桌面（系统）音频采集** |
|:---:|:---:|:---:|:---:|:---:|
| 15 Sequoia | ✅ | ✅ | ✅ | ✅ |
| 14 Sonoma | ✅ | ✅ | ✅ | ✅ |
| 13 Ventura | ✅ | ✅ | ✅ | ✅ |
| 12.3 ~ 12.7 Monterey | ✅ | ✅ | ✅ | ❌ 明确提示"需要 macOS 13.0+" |
| ≤ 12.2 或更早 | ❌ 系统拒绝启动 | — | — | — |

- 桌面音频采集在 12.3~12.7 不可用的原因：`SCStreamOutputTypeAudio` 系于 macOS 13.0 才对外开放；代码里已有 `@available(macOS 13.0, *)` runtime gate，会返回明确的提示信息 `"Desktop audio capture requires macOS 13.0 or newer."`，而不是崩溃

### 3. 代码签名与公证状态

- **签名类型**：ad-hoc 临时签名（`codesign -s -`），无 Apple Developer ID
- **公证状态**：未公证（未申请 Apple Developer 账号）
- **用户端表现**：首次打开会被 Gatekeeper 拦截，提示"无法打开，来自身份不明的开发者"
- **绕过方式**：右键 → 打开 → 确认；或系统设置 → 隐私与安全性 → "仍要打开"
- **hardened runtime**：**未启用**（`--options runtime` 被主动去除），原因见 v2.5.6 章节 "Milkdrop 纯黑屏"教训——启用后会拦截 ad-hoc 签名的 dylib 加载

### 4. 关键系统能力依赖

| 依赖 | 系统 API 最低版本 | 用途 |
|------|:---:|------|
| **CoreAudio** | 全版本 | 麦克风采集、Audio Unit 宿主 |
| **ScreenCaptureKit（框架加载）** | macOS 12.3+ | 桌面音频链路必需 |
| **`SCStreamOutputTypeAudio`** | macOS 13.0+ | 桌面音频真正取样点，runtime 检测 |
| **CoreMIDI** | 全版本 | MIDI 输入（当前未用但已链接） |
| **OpenGL 4.1 Core Profile** | macOS 10.9+（Apple 已 deprecated 但仍可用） | Milkdrop 可视化渲染 |
| **Metal**（间接） | macOS 10.11+ | JUCE UI 合成层 |

### 5. 权限声明（Info.plist）

打包的 `Y2Kmeter.app/Contents/Info.plist` 中含以下 usage description：

- `NSMicrophoneUsageDescription`：麦克风采集提示
- `NSScreenCaptureUsageDescription` / TCC 屏幕录制：系统音频捕获提示
- 首次触发对应功能时，系统会弹出授权请求；用户拒绝或撤销后需通过 `系统设置 → 隐私与安全性` 手动恢复（详见 v2.5.6 章节"授权授权引导弹窗"）

### 6. 分发格式

- **格式**：DMG 磁盘镜像（`build_macos_installer.sh` 产出）
- **内容**：`Y2Kmeter.app`（Standalone） + `Y2Kmeter.vst3` + `Y2Kmeter.component`（AU），DMG 内三条拖拽引导
- Universal 化后 DMG 体积会比 v2.5.6 大约 **+40%~50%**（多了 arm64 切片），属正常

### 7. 兼容性摘要（一句话版）

> **支持 macOS 12.3 Monterey 及以上 / Intel & Apple Silicon 均原生 / 12.3~12.7 用户无桌面音频功能 / ad-hoc 签名需用户绕过 Gatekeeper**

### 8. 打包 Universal Binary 的完整流程

1. **清理旧构建产物**（架构从单架构切到 Universal 后必须清）：
   ```bash
   rm -rf cmake-build-release
   ```
2. **重新 configure（CMake 会读到顶部新加的 arch/deployment 配置）**：
   ```bash
   cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
   ```
   configure 阶段应该能看到日志：
   ```
   [macOS] Deployment target: 12.3
   [macOS] Architectures    : x86_64;arm64
   ```
3. **构建三个 target**（CLion IDE 里跑 Release 也行）：
   ```bash
   cmake --build cmake-build-release --target Y2Kmeter_Standalone Y2Kmeter_VST3 Y2Kmeter_AU -j
   ```
4. **验证 Universal**：
   ```bash
   file cmake-build-release/Y2Kmeter_artefacts/Release/Standalone/Y2Kmeter.app/Contents/MacOS/Y2Kmeter
   # 期望：Mach-O universal binary with 2 architectures: [x86_64] [arm64]

   otool -l cmake-build-release/Y2Kmeter_artefacts/Release/Standalone/Y2Kmeter.app/Contents/MacOS/Y2Kmeter | grep -A1 LC_BUILD_VERSION | head -6
   # 期望：minos 12.3
   ```
5. **打 DMG**：
   ```bash
   ./build_macos_installer.sh
   # 产出：dist/Y2Kmeter-2.5.x-macOS.dmg
   ```

---

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

---

## macOS 更新检查 & Overlay z-order 修复 + 按钮微调（v2.5.5）

本次迭代在 v2.5.4 完成 Milkdrop 性能治理的基础上，聚焦解决 macOS 更新弹窗不可见、
Milkdrop 控制台 z-order 过高（盖住其他应用）、以及模块标题栏按钮对齐等问题。

涉及文件：

- `source/network/UpdateChecker.cpp`：更新检查链路修复与调试日志
- `source/ui/UpdateDialog.cpp`：弹窗 z-order 与调试日志
- `source/standalone/Y2KStandaloneApp.cpp`：调试日志（版本号临时测试后已还原）
- `source/ui/modules/MilkdropModule.h`：OverlayView 设计注释更新
- `source/ui/modules/MilkdropModule.cpp`：子窗口绑定替代 setAlwaysOnTop；按钮右移
- **新增** `source/ui/modules/MilkdropModule_mac.h`：ObjC 子窗口绑定 C 桥接声明
- **新增** `source/ui/modules/MilkdropModule_mac.mm`：`addChildWindow:ordered:NSWindowAbove` 实现
- `source/ui/ModulePanel.cpp`：标题栏按钮图案右移 1px
- `CMakeLists.txt`：添加 .mm 源文件 + ATS SSL 例外注入；版本号 2.5.4 → 2.5.5

### 修复 1：macOS 更新检查被遥测开关拦截

**现象**：macOS Standalone 启动后不弹出更新弹窗，但 Windows 侧正常。

**根因**：`CheckForUpdatesAsync()` 在发起 HTTP 请求前检查 `TelemetryClient::GetInstance().IsEnabled()`，
而 macOS 上 `LoadFromRegistry()` 因无 Windows 注册表直接 `SetEnabled(false)`，
导致更新检查被整个跳过，连 HTTP 请求都没发出。

**修复**（`source/network/UpdateChecker.cpp`）：
- 删除 `if (!TelemetryClient::GetInstance().IsEnabled()) { return; }` 拦截逻辑。
  更新检查和匿 名遥测是两个独立功能，不应捆绑授权。

### 修复 2：版本比较 lambda 未捕获 current_version

**现象**：`CheckForUpdatesAsync` 中 `std::thread` lambda 未捕获 `current_version` 导致编译错误，
后续修复后又因使用 `JucePlugin_VersionString`（2.5.4）而非传入参数（测试期 1.0.0）导致本地校验拦截服务端返回的更新。

**修复**（`source/network/UpdateChecker.cpp`）：
- lambda 捕获列表加入 `current_version`（按值捕获，因线程被 detach）
- `CompareVersionStrings` 改为使用 `current_version` 参数而非 `JucePlugin_VersionString`

### 修复 3：NativeMessageBox 按钮索引错误（0-based vs 1-based）

**现象**：VST/AU 插件模式下走 `NativeMessageBox` 回退路径，点击 "Remind Me Later" 也会打开浏览器。

**根因**：JUCE `NativeMessageBox::showAsync` 使用 `plainIndex` 模式（0-based），
macOS `NSAlertFirstButtonReturn → 0`（Download），`NSAlertSecondButtonReturn → 1`（Remind Me Later）。
代码判断 `if (result == 1)` 实际匹配的是**第二个按钮** Remind Me Later，而非 Download。

**修复**（`source/network/UpdateChecker.cpp`）：
```cpp
// 修改前：if (result == 1) → 匹配 Remind Me Later
// 修改后：if (result == 0) → 匹配 Download
if (result == 0) {
    juce::URL(info.download_url).launchInDefaultBrowser();
}
```

### 修复 4：UpdateDialog z-order 被 Milkdrop OverlayView 覆盖

**现象**：更新弹窗弹出后被 Milkdrop 预设控制台覆盖，必须点击弹窗聚焦才能显示。

**根因**：`OverlayView::UpdateOverlayViewPlacement()` 每帧调用 `toFront(false)`，持续将 overlay
推到同层级最前，覆盖刚弹出的 UpdateDialog。

**修复**（`source/ui/UpdateDialog.cpp` + `source/ui/modules/MilkdropModule.cpp`）：
- UpdateDialog：`addToDesktop` 后新增 `toFront(true)`（`orderFrontRegardless` 强推到最前）
- OverlayView：`toFront(false)` 移入 `if (!isOnDesktop())` 首次创建块，不再每帧调用
- OverlayView：`setVisible(true)` 改为条件调用 `if (!isVisible())`，避免每帧触发 NSWindow `orderFront:`

### 修复 5：Milkdrop OverlayView z-order 过高（盖住其他应用）

**现象**：OverlayView 使用 `setAlwaysOnTop(true)`，将 overlay NSWindow 推到 `NSFloatingWindowLevel`（层级 3），
高于所有普通应用窗口（`NSNormalWindowLevel`=0），导致系统其他软件的窗口也被 overlay 盖住。

**根因**：`NSFloatingWindowLevel` 是系统级浮动层，所有 `setAlwaysOnTop` 窗口都在此层。

**修复方案**：用 macOS 原生**子窗口**（`addChildWindow:ordered:NSWindowAbove`）替代 `setAlwaysOnTop`：

- 子窗口永悬浮于父窗口上方（覆盖 GL NSOpenGLView），但跟随父窗口层级
- 父窗口在前台时 overlay 在最上；父窗口被其他应用盖住时 overlay 也被盖住

**实现**（新增 `MilkdropModule_mac.h/.mm`）：
```objc
// 获取 overlay 和 parent 的 NSWindow
[parentWin addChildWindow:overlayWin ordered:NSWindowAbove];
```

**编译问题解决**（`MilkdropModule_mac.mm`）：
- **JUCE OpenGL 头文件顺序**：`MilkdropModule_mac.h` → `<JuceHeader.h>` → `juce_opengl.h` 必须在 `<AppKit/AppKit.h>`（会间接引入 `<OpenGL/gl.h>`）之前，否则触发 JUCE 的 `"gltypes.h included before juce_gl.h"` static_assert
- **Carbon 类型冲突**：`<AppKit>` 伞形头文件间接引入 `MacTypes.h` 的 `Point` 和 `Components.h` 的 `Component`，与 JUCE 的 `juce::Point` / `juce::Component` 冲突。解决方案是 `#define Point JUCE_CARBON_Point` / `#define Component JUCE_CARBON_Component` 在 `#import <AppKit>` 前后暂存/还原

**CMakeLists.txt**：`target_sources` 的 `if(APPLE)` 分支加入新文件。

### 修复 6：模块标题栏按钮图案右移 1px

**改动**（`source/ui/ModulePanel.cpp`、`source/ui/modules/MilkdropModule.cpp`）：
```cpp
// 修改前：translate(-1, -1) → 左上偏移
// 修改后：translate(0, -1)  → 仅垂直偏移，向右移 1px
```
关闭按钮（X）和脱离/停靠按钮（-/=）均生效，所有模块统一。

### 修复 7：macOS ATS SSL 例外注入

**背景**：测试期间发现 `iisaacbeats.cn` 的 HTTPS 连接被 macOS NSURLSession 拦截（`errSSLPeerAuthCompleted` -9836），
尤其在 VPN 环境下证书验证失败导致更新检查 HTTP 请求失败。

**修复**（`CMakeLists.txt`）：
- 在 `Y2Kmeter_Standalone` 的 `POST_BUILD` 中用 `PlistBuddy` 注入 `NSAppTransportSecurity` 例外，
  仅针对 `iisaacbeats.cn` 域名放行，不影响其他 HTTPS 连接。

### 调试工具改进

全局将 `DBG()` 替换为 `juce::Logger::writeToLog()`，因为 `DBG()` 在 Release 构建下展开为空宏，
导致调试日志无法输出。`juce::Logger::writeToLog()` 不受构建类型影响，始终写入 stderr。

### 关键教训与注意事项（v2.5.5 追加）

| 类别 | 教训 |
|------|------|
| **平台差异 · 遥测与更新检查解耦** | macOS 上遥测默认禁用（无注册表 = `LoadFromRegistry()` 返回 false），但更新检查不应依赖遥测开关。两个功能应当独立授权，否则 macOS 用户永远收不到更新通知。 |
| **JUCE NativeMessageBox 按钮索引** | `NativeMessageBox::showAsync` 使用 `plainIndex` 模式（0-based），macOS `NSAlertFirstButtonReturn→0`，Windows `TaskDialogIndirect` 返回的 buttonIndex 也是 0-based。`if (result == 1)` 在很多场景下匹配的是第二个按钮，不是第一个。 |
| **平台差异 · NSWindow z-order** | `setAlwaysOnTop` → `NSFloatingWindowLevel` 高于所有普通窗口，适用场景极少（如全局浮动工具条）。需要"在父窗口上方但不盖住其他应用"的语义时，应使用 `addChildWindow:ordered:NSWindowAbove`。子窗口的 z-order 跟随父窗口，这是 macOS 唯一正确的方案。 |
| **ObjC++ 编译 · Carbon 类型冲突** | `.mm` 文件中同时使用 `<AppKit>` 和 JUCE 时，Carbon 的全局 `Point` / `Component` 类型会与 `juce::Point` / `juce::Component` 产生命名歧义。标准解法是用 `#define` 暂存/还原，因为 AppKit 自身不依赖这些 Carbon C 类型。 |
| **ObjC++ 编译 · OpenGL 头文件顺序** | JUCE 的 `juce_opengl.h` 必须严格在系统 `<OpenGL/gl.h>` 之前加载，否则 `static_assert("gltypes.h included before juce_gl.h")`。`.mm` 文件中应先 `#include` JUCE 头，再 `#import <AppKit>`。 |
| **CMake 增量编译不完整** | 修改调用方代码后，有时即使 touch 文件，cmake 也不会检测到变更。最可靠的排查手段是在怀疑未被重编译的函数内部**直接硬编码关键参数**作为应急,同时在调用方添加 `juce::Logger::writeToLog` 签名日志确认执行路径。 |
| **DBG vs Logger** | `DBG()` 在 Release 构建下被编译器干掉了。调试生产环境问题时必须使用 `juce::Logger::writeToLog()`，它在所有构建类型下都输出到 stderr。 |

---

## macOS DMG 打包稳定性 + TCC 授权流水线 + CJK 弹窗字体（v2.5.6）

本次迭代解决 macOS DMG 安装后一系列"IDE 直跑一切正常、装完启动台打开就
崩/黑/无声"的隐性差异，覆盖资源查找、动态库加载、TCC 授权、弹窗中文渲染
四大板块。

涉及文件：

- `build_macos_installer.sh`：去除 `--options runtime`（打破 Milkdrop dylib 黑屏）
- `CMakeLists.txt`：`MICROPHONE_PERMISSION_ENABLED` + `SKIP_PRESETS` 选项
- `PluginProcessor.cpp`：Telemetry 存储路径挂到 `Application Support`
- `PluginEditor.cpp`：`FindMilkdropAssetsDir` 空目录判空 + AppData Seed 分支
- `source/ui/modules/MilkdropModule.cpp`：`FindMilkdropAssetsDirForModule` 同步策略 + GL error 清空 + 首帧黑屏 + 预扫盘
- `source/ui/modules/MilkdropModule.h`：`ScanPresetFiles` 提升为 public
- `source/ui/modules/ProjectMApi.cpp`：dlopen 诊断日志
- `source/ui/modules/TamagotchiModule.cpp`：新增 `findBundleResourcesBaseDir()` + 4 个资源查找函数追加 bundle 分支 + `randomAnimFrom/beginPatrolCycle` 空表兜底
- `source/ui/ModuleWorkspace.cpp`：非脱离态 Milkdrop 模块整体强制置顶
- `source/standalone/MacDesktopAudioCapture.mm`：屏幕录制权限失败文案改为"移除 + 重新添加"分步引导
- `source/standalone/Y2KStandaloneApp.cpp`：TCC reset 版本自检 + `showSystemFontAlertAsync` 系统字体弹窗

### 修复 1：安装后启动崩溃（Tamagotchi 资源查找漏 bundle 分支）

**现象**：DMG 安装后从启动台打开 Y2Kmeter，`TamagotchiModule::randomAnimFrom(std::initializer_list<int>) const + 375` 处 EXC_BAD_ACCESS 空指针崩溃。IDE 直跑正常。

**根因**：`findTamagotchiAssetsRoot()` 等 4 个资源查找函数仅遍历 CWD 与可执行文件父目录链，而启动台启动时 CWD=`/`，可执行文件在 `.app/Contents/MacOS/` 向上遍历也到不了 `Contents/Resources/assets/`。资源找不到导致 `availableAnimIds` 为空，随后 `randomAnimFrom` 越界访问空数组。

**修复**（`TamagotchiModule.cpp`）：
- 新增 `findBundleResourcesBaseDir()` 静态函数：从 `currentApplicationFile`（.app bundle 根）向下取 `Contents/Resources/`，兼容 IDE 直跑（`currentApplicationFile` 指向可执行文件）时向上两级的场景。
- **★ 关键陷阱**：函数返回 `Contents/Resources/`（**不含 `assets/` 层级**），因为下游 `tryFromBase(base)` lambda 内部会再拼一次 `.getChildFile("assets")`；若这里就返回 `.../Resources/assets/`，就会变成 `.../Resources/assets/assets/Tamagotchi/...` 双层 assets 命中不了。
- 4 个查找函数（`findTamagotchiAssetsRoot`、`findTamagotchiMirrorAssetsRoot`、`findTamagotchiRolePngDir`、`findTamagotchiEggAssetsDir`）在 `__APPLE__` 分支加入 `tryFromBase (findBundleResourcesBaseDir())` 兜底。
- `randomAnimFrom()` 追加空表兜底 `if (availableAnimIds.isEmpty()) return 1;`。
- `beginPatrolCycle()` 入口空表提前返回。

### 修复 2：安装后 Milkdrop 纯黑（hardened runtime 拦截 ad-hoc dylib）

**现象**：`cmake-build-release/.../Standalone/Y2Kmeter.app` 直接跑 Milkdrop 完全正常，DMG 打出来装完后打开 Milkdrop 模块只有预设控制台、其他区域纯黑无渲染。

**根因**：`build_macos_installer.sh` 的 `sign_bundle` 使用了 `codesign --options runtime`，启用 hardened runtime。hardened runtime 会强制校验主进程与它 `dlopen` 的 dylib 拥有一致的 Team ID。ad-hoc 签名的 `libprojectM-4.dylib` 会被 dyld 算出一个"隐式 Team ID"（基于内容哈希，如 `A060AF26-12D3-3E02-...`）与主 binary 不匹配，dyld 拒绝加载，`Api::isAvailable()` 返回 false，`renderOpenGL()` 只 clear 成黑色就 return。

vmmap 关键证据：修复前 dylib 未出现在进程内存映射中，flags = `0x10002 (adhoc,runtime)`；修复后 flags = `0x2 (adhoc)`，dylib 正常加载。

**修复**（`build_macos_installer.sh`）：
- `sign_bundle` 内 `codesign` 命令去掉 `--options runtime`
- 保留 `--force --deep --sign - --timestamp=none` 组合
- 添加详细注释说明"只有走 Apple Developer ID + notarization 的正式签名时才应配合 hardened runtime；ad-hoc 分发场景必须禁用"

### 修复 3：麦克风授权失效（缺 NSMicrophoneUsageDescription）

**现象**：安装完新版本 → 手动在系统设置里把麦克风开关切到"允许"→ 打开软件依然抓不到音频；只有先用系统设置里的 `-` 按钮把 Y2Kmeter 条目彻底删除、然后重新开软件让 macOS 弹出授权对话框、点"允许"后才能正常工作。

**根因**：主 binary 的 Info.plist 缺失 `NSMicrophoneUsageDescription` 键。macOS 15+ 的 TCC 会在运行时严格校验此键——即便 TCC.db 里状态是 allowed，缺键的 app 走 AVCaptureDevice 请求时依然返回 denied，且 csreq 记录不会随 csreq 变化更新。用户"删除后重新添加"能成功，是因为那次删除强制走了一遍完整鉴权流程。

**修复**（`CMakeLists.txt`）：
- `juce_add_plugin` 加入
  ```
  MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT "Y2Kmeter 需要访问麦克风或音频输入设备..."
  ```
- JUCE 的 juceaide 会在生成 Info.plist 时自动注入 `NSMicrophoneUsageDescription` 键
- 验证：`plutil -p Y2Kmeter.app/Contents/Info.plist | grep Microphone` 应有输出

### 修复 4：系统音频（ScreenCapture）授权反复失效 + 弹窗引导

**现象**：每次装新版本后，用户必须在系统设置中把旧的 Y2Kmeter 条目彻底删除再重新添加才能授权屏幕录制。TCC 数据库对 `ScreenCapture` 的清理有意做得比 `Microphone` 更保守（无 UsageDescription 键，仅依赖签名 csreq），单纯 `tccutil reset ScreenCapture` 不一定生效。

**修复方案**（双管齐下）：

1) **启动时版本自检 + tccutil reset**（`Y2KStandaloneApp.cpp`）：
   - `initialise()` 里读取 `Application Support/Y2Kmeter/last_launched_version.txt`
   - 若与当前 `JucePlugin_VersionString` 不同：
     ```cpp
     std::system("tccutil reset Microphone cn.iisaacbeats.Y2Kmeter");
     std::system("tccutil reset ScreenCapture cn.iisaacbeats.Y2Kmeter");
     // 各执行两次覆盖并发写入边缘情况
     std::system("tccutil reset Microphone cn.iisaacbeats.Y2Kmeter");
     std::system("tccutil reset ScreenCapture cn.iisaacbeats.Y2Kmeter");
     ```
   - `usleep(150 * 1000)` 落盘等待
   - 写入当前版本号，避免同一版本重复 reset

2) **权限失败弹窗改为分步引导**（`MacDesktopAudioCapture.mm` + `Y2KStandaloneApp.cpp`）：
   - `buildPermissionHelpText()` 从"打开设置授权"改为明确的 4 步中英双语指引："先按 `-` 移除条目 → 完全退出 → 重新打开 → 允许"
   - `startMacDesktopAudioCapture` 失败路径改为 `showSystemFontAlertAsync` 三按钮弹窗
   - 按钮策略：`Open System Settings`（跳 `x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture` 并**保持弹窗打开**）、`Reveal App in Finder`（在 Finder 定位 `/Applications/Y2Kmeter.app` 并**保持弹窗打开**）、`Close`（关闭弹窗）
   - 关键实现：`showSystemFontAlertAsync` 内对非最后一个按钮覆盖 `Button::onClick` 回调 → 只触发用户回调、不调用 `exitModalState`

### 修复 5：macOS CJK 弹窗字体乱码（PinkXPLookAndFeel 全局污染）

**现象**：`showSystemFontAlertAsync` 里传入的中文标题、正文、按钮全部显示为像素方块，样式类似位图字体。

**根因链条**（三层污染，需一并解决）：
1. `PinkXPLookAndFeel::PinkXPLookAndFeel()` 调用 `LookAndFeel::setDefaultSansSerifTypeface(gTypeface)`，把全局默认无衬线字体替换为 ASCII-only 位图字体
2. `PinkXPLookAndFeel::getTypefaceForFont()` 无条件返回 `PinkXP::gTypeface`，忽略传入 `Font` 的 typeface 名称
3. JUCE `AlertWindow` 的大标题走 `getAlertWindowTitleFont()`、按钮走 `getTextButtonFont()` 内部以 `Font(height, bold)` 构造，直接取全局默认 typeface，**根本不调用** `getTypefaceForFont()`

**修复**（`Y2KStandaloneApp.cpp`：新增 `SystemLookAndFeel` 内嵌类）：
```cpp
struct SystemLookAndFeel : juce::LookAndFeel_V4 {
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override { return nullptr; }
    juce::Font getAlertWindowTitleFont() override    { return Font(FontOptions("PingFang SC", 18, bold)); }
    juce::Font getTextButtonFont (TextButton&, int h) override { return Font(FontOptions("PingFang SC", jmin(15, h*0.6), plain)); }
    juce::Font getAlertWindowFont() override         { return Font(FontOptions("PingFang SC", 12, plain)); }
    void drawButtonText (Graphics& g, TextButton& b, bool, bool down) override { /* 显式 setFont 再绘制 */ }
};
```
- `getTypefaceForFont → nullptr` 阻断 LNF 层字体替换
- 三个字体方法返回显式携带 `PingFang SC` typefaceName 的 `FontOptions`（Windows: `Microsoft YaHei`，Linux: `Noto Sans CJK SC`）
- `drawButtonText` 显式 `setFont` 再绘制，防止基类内部字体重解析
- TextEditor 正文 `applyFontToAllText` 直设字体，绕过 LNF 链路

**用户反馈迭代**（v2.5.6 尾声）：即便上述修复完成，用户测试后发现原生 `AlertWindow` 的标题栏文字（顶层 NSWindow 自绘）仍无法被 LNF 影响，最终采用**回避策略**：
- 传入 title 用空字符串 `juce::String()`，AlertWindow 使用固定 `"Y2Kmeter"` 纯 ASCII 标题
- 中文内容仅保留在 TextEditor 正文（`applyFontToAllText` 直接指定 PingFang SC，路径最短最可靠）
- 按钮文本改为纯英文（`Open System Settings` / `Reveal App in Finder` / `Close`）
- 正文严格避免同一行"英文标题 + 中文段落"的混排；仅整行中文 + 整行英文两段

### 修复 6：Milkdrop 模块在 macOS 下无法被其他模块盖住 + 首帧乱码

**现象 A**：GLView 通过 `attachTo` 创建的原生 `NSOpenGLView` 是顶层 NSView 的子视图，其合成层级永远高于同一窗口内 CoreGraphics 绘制的其他模块，用户无法通过 JUCE `toFront()` 让其他模块盖住 Milkdrop 的视频区。为视觉一致性，其他模块 z-order 变化时希望 Milkdrop 模块整体（含 CG 绘制的边框、抬头）也置顶。

**现象 B**：`newOpenGLContextCreated` 中 `projectm_create()` / `initGlew()` 阻塞 GL 线程数百毫秒，期间 AppKit 用 NSOpenGLView 的未初始化 back buffer 合成窗口 → 用户看到花花绿绿的显存乱码。

**现象 C**（Debug 构建独有）：`LoadCurrentPreset()` 中 projectM 内部 HLSL→GLSL 转译 + `glCompileShader / glLinkProgram` 会在 GL error 队列累积项。JUCE 的 `checkGLError()` 在 peer 未 valid 时会无限 `continue`，导致死循环卡帧。

**修复**（`MilkdropModule.cpp` / `ModuleWorkspace.cpp`）：
- `ModuleWorkspace::hookPanel/addModule/dockModule` 都在 `#if JUCE_MAC` 分支下追加：遍历 modules，对非脱离态且类型是 `milkdrop` 的模块调用 `toFront(false)`，保持整体置顶
- `newOpenGLContextCreated()` 入口先 `while (glGetError() != GL_NO_ERROR) {}` 清空错误队列 + `OpenGLHelpers::clear(Colours::black)` 立即黑帧，把窗口 back buffer 清干净后再 `projectm_create()`
- `LoadCurrentPreset()` 前后各追加一次 GL error clear，防止 shader 编译遗留错误踩到 checkGLError 死循环

### 修复 7：AppData 空目录屏蔽 bundle 内合法资源

**现象**：老版本残留在 `~/Library/Application Support/Y2Kmeter/milkdrop_presets/` 的空目录（或不完整目录）会被新版本的 `FindMilkdropAssetsDir` 采纳并 return，导致 bundle 内的合法预设永远命中不到。

**修复**（`PluginEditor.cpp` + `MilkdropModule.cpp`）：
- 新增 `isValidAssetsDir()` lambda：`milkdrop_presets` 至少含 1 个 `.milk`；`milkdrop_textures` 至少含 1 个子文件
- 所有 4 个候选路径（AppData / Standalone bundle / VST3/AU bundle / CWD 遍历）都改用 `isValidAssetsDir` 而非 `exists() && isDirectory()`
- macOS 追加 `seedToAppDataIfNeeded` lambda：命中 Standalone bundle 内合法 `milkdrop_presets` 时同步 `copyDirectoryTo(appDataDir)` 到 AppData，让 VST3/AU 后续能从共享 AppData 读取

### 修复 8：预设三端共享 + DMG 瘦身

**背景**：Milkdrop 预设约 200 MB，Standalone / VST3 / AU 三端 bundle 各自内置一份 → DMG 体积 ~700 MB。

**方案**（`CMakeLists.txt`）：
- `y2km_deploy_projectm_into_bundle` 函数支持 `SKIP_PRESETS` 参数
- Standalone target 传默认参数（内置全部预设作为 seed 源）
- VST3 / AU target 传 `SKIP_PRESETS`（跳过预设拷贝，只保留 textures 副本）
- 运行时依赖上文"AppData Seed"机制：Standalone 首次启动即把预设一次性复制到 `~/Library/Application Support/Y2Kmeter/milkdrop_presets/`，VST3/AU 直接读共享 AppData
- **副作用**：只装 VST3/AU 不装 Standalone 时预设不会自动 seed；DMG 内 README 已注明用户可手动放置预设目录

### 修复 9：Telemetry 存储路径合规

**修复**（`PluginProcessor.cpp`）：
- `PropertiesFile::Options` 追加 `opts.osxLibrarySubFolder = "Application Support";`
- 原来 Telemetry 落在 `~/Library/` 根目录，不符合 macOS 规范；改后落在 `~/Library/Application Support/Y2Kmeter/`

### 修复 10：Milkdrop 预设重命名 + 磁盘扫描前移

**背景**：本轮同时对 9600+ `.milk` 预设文件做了统一重命名（去除特殊字符、规整编号）。旧文件 9612 个删除、新文件 9607 个新增。VST3/AU 走 AppData 共享，Standalone bundle 内置作为 seed 源。

**磁盘扫描前移**（`MilkdropModule.cpp` / `MilkdropModule.h`）：
- `ScanPresetFiles()` 从 private 提升为 public
- `MilkdropModule` 构造函数在主线程调用 `glView->ScanPresetFiles()`，把 9000+ `.milk` 文件的遍历从 GL 线程关键路径中移出
- `newOpenGLContextCreated` 里若 `local_preset_paths_` 已非空则跳过扫盘
- 效果：模块创建瞬间的卡顿感明显缓解

### 关键教训与注意事项（v2.5.6 追加）

| 类别 | 教训 |
|------|------|
| **codesign · ad-hoc vs hardened runtime** | `--options runtime` 会强制主进程与其 dlopen 的所有 dylib 拥有一致的 Team ID。ad-hoc 签名的 dylib 没有真实 Team ID（隐式 Team ID = 内容哈希），必然与主 binary 不一致 → dyld 拒绝加载 → 功能表现为"库存在但用不上"（vmmap 看不到映射）。只有走 Apple Developer ID + notarization 的正式签名才应启用 hardened runtime。 |
| **TCC · UsageDescription 与 csreq 的耦合** | macOS 15+ 对 `Microphone` 等需要 UsageDescription 键的权限做了更严的运行时校验：即便 TCC.db 里状态是 allowed，缺 UsageDescription 键的 app 走对应 API 时依然会被拒。且缺键情况下 TCC 记录不会随 csreq 变化更新，用户"切开关"无效，只有强制删除条目才行。→ **任何需要授权的 macOS app 必须显式声明所有 UsageDescription 键**，即便 JUCE 帮你默认写了空字符串。 |
| **TCC · ScreenCapture 与 tccutil reset 的差异** | `tccutil reset ScreenCapture <bundleID>` 不一定能像 `Microphone` 那样彻底清理 —— ScreenCapture 是 macOS 15 才独立成 category 的年轻权限，Apple 内部策略保守。稳妥做法是"tccutil reset 双写 + 弹窗引导用户手动 `-` 移除"，不要依赖单一手段。 |
| **macOS Bundle 资源路径 · currentApplicationFile 语义歧义** | JUCE `File::currentApplicationFile` 在 .app 里指向 bundle 根目录（如 `Y2Kmeter.app`），在 IDE 直跑时可能指向可执行文件本身（`Y2Kmeter.app/Contents/MacOS/Y2Kmeter`）。资源查找函数必须两种情况都处理：先试 `<appFile>/Contents/Resources/`，再试 `<appFile>.parent.parent/Resources/`。 |
| **macOS Bundle 资源路径 · assets 层级** | 若辅助函数返回 `.../Resources/`，下游 `tryFromBase` lambda 可能会拼一次 `assets/`；若辅助函数已经返回 `.../Resources/assets/`，就会变成双层 assets 命中不了。**辅助函数与调用方 assets 拼接层级要严格对齐**，且**层级差异必须在代码注释里写死**，否则半年后再看谁改谁踩。 |
| **JUCE AlertWindow · 标题栏字体不受 LNF 控制** | AlertWindow 的原生标题栏文字由顶层窗口系统绘制（macOS 是 NSWindow 的 titleBar，Windows 是 WM_NCPAINT），JUCE 的 LookAndFeel 根本影响不到。传给 AlertWindow 构造函数的 title 若含 CJK，在 CJK 字体缺失的构建下会显示乱码。**唯一稳妥做法**：title 传空或纯 ASCII，中文内容全放在 TextEditor 正文，用 `applyFontToAllText` 直接指定 CJK 字体。 |
| **JUCE LookAndFeel · setDefaultSansSerifTypeface 的全局副作用** | 该静态方法会替换整个进程的默认无衬线字体。若某个 LNF（如 PinkXPLookAndFeel）在构造时调用它设置了 ASCII-only 位图字体，即便后续手工挂另一个 LNF 到某个 AlertWindow，AlertWindow 内部走 `Font(height, style)` 构造的字体（大标题、按钮字号计算）仍会取全局默认 typeface → CJK 乱码。要么不用全局设置，要么在每个字体方法里显式指定 `FontOptions(typefaceName, ...)`。 |
| **JUCE LookAndFeel · AlertWindow 按钮点击强制 exitModal** | JUCE `AlertWindow::addButton` 内部为每个按钮设置了 `onClick` 触发 `exitModalState`。若想实现"点按钮 A 只跳转设置、弹窗保持打开"，必须在 `addButton` 之后通过 `getButton(name)->onClick = ...` 覆盖回调（不调用 `exitModalState`）。最后一个按钮保留原生行为作为"关闭"入口。 |
| **NSOpenGLView 合成层级永远高于 CG 绘制** | 同一 NSWindow 里 NSOpenGLView 是硬件合成层，永远盖在 CoreGraphics 绘制的兄弟视图之上；无法通过 JUCE `toFront()` 更改（`toFront` 只影响 JUCE Component 的 childList 顺序，与 AppKit 视图层级不同）。要么接受"GL 视图总在最上"这一物理事实、把其他相关模块也 `toFront` 保持视觉一致，要么把 GL 视图放到独立 NSWindow（脱离态）走窗口级 z-order。 |
| **macOS OpenGL over Metal · GL error 队列残留** | 刚 attach 完 OpenGLContext 时 GL error 队列里常有历史项；projectM 加载 `.milk` 预设的 shader 编译 / link 也会产生 error。JUCE Debug 构建的 `checkGLError()` 在 peer 未 valid 时无限 continue 不消费错误，会造成死循环。**每次 attach 后、每次 shader 加载前后**都要 `while (glGetError() != GL_NO_ERROR) {}` 清空一次。 |
| **AppData 空目录 vs bundle 内合法资源** | "存在即采纳"的资源查找策略在遇到旧版本残留空目录时会屏蔽 bundle 内的合法资源。资源查找应该基于**内容有效性**（至少 N 个子文件 / 特定后缀）而非**存在性**判空。 |
| **DMG 瘦身 · 三端 bundle 共享大资源** | Milkdrop 预设约 200 MB × 3 (Standalone/VST3/AU) = 600 MB 冗余。改为"Standalone 内置作 seed 源，首次运行时 copy 到 AppData，插件读共享 AppData"后 DMG 直接减 400+ MB。副作用：只装插件不装 Standalone 的用户需手动放预设，README 需说明。 |
| **macOS Application Support 规范** | `~/Library/` 根目录不应直接放 app 私有数据，应走 `~/Library/Application Support/<AppName>/`。JUCE `PropertiesFile::Options` 里通过 `osxLibrarySubFolder = "Application Support"` 一行搞定。 |

---

## 全屏行为跨平台适配（v2.6.0 后续修复，不升级版本号）

### 背景与根因

v2.5.8 为修复 Windows 无边框窗口（`setUsingNativeTitleBar(false)`）调用 `setFullScreen` 走 `SW_SHOWMAXIMIZED` 时覆盖任务栏、且 `alwaysOnTop` 主窗口压住 PopupMenu 的问题，把"双击标题栏全屏"与"MV 布局预设全屏"统一改为**伪最大化**：`setBounds(display->userArea)`。

`Displays::Display::userArea` 在 Windows 上等于"扣除任务栏的工作区"，但在 macOS 上等于"扣除顶部菜单栏与 Dock 的安全区"。该改动未做平台隔离，导致 macOS 端出现两个回归：
1. 双击标题栏只能铺满 userArea，留出系统菜单栏/Dock 空间，无法完全全屏；
2. MV 预设（`applyLayoutPreset` case 5）同样只铺满 userArea，未调用系统原生全屏接口，菜单栏/Dock 仍显示。

### 修复内容（仅 [PluginEditor.cpp](/Users/jy/CLionProjects/Y2Kmeter/PluginEditor.cpp)，macOS 分支）

| 位置 | macOS 行为 | Windows 行为 |
|------|-----------|-------------|
| `toggleFakeFullScreen()` | 顶部 `#if JUCE_MAC` 分支直接 `rw->setFullScreen(!rw->isFullScreen())`，进入系统原生全屏（`NSWindow toggleFullScreen` → 独立 Space，隐藏菜单栏/Dock），与双击标题栏历史行为一致 | 保持 v2.5.8 伪最大化：`setBounds(userArea)` + 放宽/恢复 resizeLimits |
| `applyLayoutPreset()` case 5（MV） | 布局区域用 `display->totalArea`（完整显示器尺寸），布局完成后 `rw->setFullScreen(true)` 进入系统原生全屏 | 继续 `display->userArea` 伪最大化，不调用原生全屏 |

### 关键实现细节

- macOS MV 预设顺序：先按 `totalArea` 铺满并完成模块布局（上方 7 模块条 + 下方 Milkdrop），再 `setFullScreen(true)` 进入原生全屏；原生全屏后的窗口尺寸即 totalArea，二者不冲突。
- `setFullScreen(true)` 幂等：若窗口已处于全屏，JUCE 内部判断 `shouldBeFullScreen == isFullScreen()` 后不重复 toggle。
- macOS 原生全屏仅在 `juce::JUCEApplicationBase::isStandaloneApp()` 为真时触发，插件模式（AU/VST3）窗口由宿主管理，不接管全屏。
- 两处均用 `#if JUCE_MAC` 包裹，Windows 路径完全不进入，保证跨平台隔离。

### 教训

| 类别 | 教训 |
|------|------|
| **`Displays::Display::userArea` 平台语义差异** | `userArea` 在 Windows = 扣除任务栏的工作区，在 macOS = 扣除菜单栏/Dock 的安全区。任何"伪全屏/铺满"逻辑若跨平台共用 `userArea`，macOS 上会留出系统栏空间。macOS 需要"真正全屏"时必须显式走原生 `ResizableWindow::setFullScreen`（`NSWindow toggleFullScreen`），仅 setBounds 到 `totalArea` 无法隐藏菜单栏/Dock（它们以更高窗口层级显示）。 |