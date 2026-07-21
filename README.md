# Y2Kmeter 项目文件整理说明

## 项目概述
这是一个基于JUCE框架的音频插件项目，包含VST3插件和独立应用程序版本。

## 目录结构说明

### 根目录文件
- `CMakeLists.txt` - CMake构建配置文件
- `PluginEditor.cpp/h` - 插件编辑器主类
- `PluginProcessor.cpp/h` - 插件处理器主类
- `Y2Kmeter_installer.iss` - Inno Setup安装器脚本
- `build_installer.bat` - 构建安装器的批处理脚本
- `.gitignore` - Git忽略规则文件

### 源代码目录 (`source/`)
- `analysis/` - 音频分析模块
  - `AnalyserHub.cpp/h` - 分析器管理中心
  - `DynamicRangeMeter.cpp` - 动态范围计量器
  - `LoudnessMeter.cpp/h` - 响度计量器
  - `PhaseCorrelator.cpp` - 相位相关器
- `standalone/` - 独立应用程序模块
  - `WasapiLoopbackCapture.cpp/h` - WASAPI音频捕获
  - `Y2KStandaloneApp.cpp` - 独立应用程序主类
- `ui/` - 用户界面模块
  - `ModulePanel.cpp/h` - 模块面板
  - `ModuleWorkspace.cpp/h` - 模块工作区
  - `PinkXPStyle.cpp/h` - 界面样式定义
  - `modules/` - 具体功能模块
    - 动态、均衡器、频谱、波形等各功能模块

### 资源文件目录
- `assets/` - 应用程序资源
  - `app_icon.rc` - 图标资源定义
  - `icon.ico` - 应用程序图标
  - `logo.png` - 应用程序Logo
  - `milkdrop_presets/` - Milkdrop 模块使用的 100 个精选 `.milk` 预设
    （来自 [Cream of the Crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)，与 projectM 一同以 LGPL-2.1-or-later 分发）
  - `milkdrop_textures/` - Milkdrop 模块使用的 66 个基础纹理
    （来自 projectM 官方 Milkdrop texture pack）
  - `Tamagotchi/` - 电子宠物模块动画资源
- `third_party/projectm/` - **libprojectM 4** Windows 预编译库（DLL + 头文件）
  - 详见 [`third_party/projectm/README.md`](third_party/projectm/README.md)
  - 以 **LGPL-2.1-or-later** 授权，运行时通过 `LoadLibrary` 动态加载
- `ttf/` - 字体文件
  - `Silkscreen-Regular.ttf` - Silkscreen字体
  - 其他中文字体文件

## 文件筛选指南

### 保留的核心文件类型
- **源代码文件**: `.cpp`, `.h`, `.hpp`
- **构建配置文件**: `CMakeLists.txt`
- **资源文件**: `.rc`, `.ico`, `.png`, `.ttf`
- **安装脚本**: `.iss`, `.bat`
- **文档文件**: `.md`, `.txt`

### 排除的文件类型
- **编译产物**: 所有构建目录 (`cmake-build-*`, `out`, `dist`)
- **IDE配置**: `.vs/`, `.idea/` 目录
- **临时文件**: `_filterstate.*`, `color.txt`
- **测试文件**: `pngtest/` 目录
- **大文件**: `grid_plugin.zip`
- **系统文件**: `desktop.ini`, `.DS_Store`, `Thumbs.db`

### Git仓库使用说明
1. 将整理好的 `D:/y2kmetergit` 目录初始化为新的Git仓库
2. 确保 `.gitignore` 文件已正确配置
3. 提交所有保留的文件到仓库
4. 后续开发时，编译产物会自动被忽略

## 项目构建说明
使用CMake进行项目构建，支持VST3插件和独立应用程序的生成。

## 开源协议
本项目采用 **GNU General Public License v3.0 (GPL-3.0)** 开源。

- 完整协议文本见根目录文件：`LICENSE`
- 你可以自由使用、修改和分发本项目，但分发衍生作品时需遵循 GPL-3.0 的同等开源要求。

## 第三方组件许可

Y2Kmeter 在运行时/发布物中包含以下第三方作品：

### libprojectM 4  (LGPL-2.1-or-later) — Milkdrop 模块的核心可视化引擎

- 上游仓库：<https://github.com/projectM-visualizer/projectm>
- 我们仅以**动态链接**（`LoadLibrary()` + `GetProcAddress()`）的方式使用官方
  预编译的 `projectM-4.dll`（以及其依赖 `glew32.dll`），并**未修改**其源码。
- LGPL-2.1 要求：
  1. 用户可以**替换或升级** `projectM-4.dll` 而无需重新编译 Y2Kmeter ——
     我们的加载策略（从 exe 目录读取 DLL）已满足该要求；
  2. 完整源码见上游仓库；
  3. 完整 LGPL-2.1 协议文本见
     <https://github.com/projectM-visualizer/projectm/blob/master/LICENSE.txt>。

**Powered by projectM** — 感谢 projectM 团队让 Milkdrop 生态得以在原生
应用中复用。

### Milkdrop 预设 / 纹理  (LGPL-2.1-or-later)

- `assets/milkdrop_presets/`：来自
  [presets-cream-of-the-crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)
  的 100 个精选 `.milk` 预设。
- `assets/milkdrop_textures/`：来自 projectM 官方 Milkdrop texture pack 的 66 个基础纹理。
- 与 libprojectM 同以 LGPL-2.1-or-later 授权。

### JUCE  (GPL v3 / 商业双授权)

框架本身；Y2Kmeter 采用 GPL v3 授权，因此我们使用 JUCE 的 GPL v3 版本。