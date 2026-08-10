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
