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

- 本分支为“macOS 适配稳定快照”，用于继续迭代与向主干择机合并。
- 若后续需要跨平台统一，可在此基础上做平台抽象与条件编译收敛。
