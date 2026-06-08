
# Y2Kmeter v1.8 外网版本更新公告

> 发布日期：2026-05-07  
> 版本号：**v1.5.0 → v1.8.1**  
> 对比基线：`3724f5f7740f33d7be363328fc064f1eee7489be`（上一外网版本）

本次版本跨越 v1.6 / v1.7 / v1.8 三个版本节点的全部改动累计更新，总计 **25 个提交**，涉及 **52 个文件**、新增约 **9648 行** / 删除约 **528 行** 代码。以下按用户可感知维度进行分类整理。

---

## 🎉 一、全新功能

### 1. 前置分析增益（Analysis Input Gain）
- 新增独立于音频输出的 **分析前置增益**，范围 **-10 dB ~ +36 dB**。
- 该增益仅作用于分析链路，不影响实际输出音频，方便用户在小信号或需要观测响度细节时手动提升显示动态范围。
- 修改增益后，**LUFS-I（积分响度）自动归零重算**，避免旧数据污染。
- 增益值随插件状态一并保存/恢复（`analysisInputGainDb`）。

### 2. macOS 平台完整适配（首次支持）
- 新增 macOS 构建体系：
  - `build_macos_installer.sh` 一键打包脚本
  - `scripts/macos_dmg_background.m` / `macos_iconize.m` / `macos_iconize.swift` DMG 图标与背景资源生成脚本
  - `CMakeLists.txt` 增加 macOS 工具链、代码签名、Universal Binary、notarization 配置
- 新增 macOS 独立桌面音频采集后端：
  - `source/standalone/MacDesktopAudioCapture.h/.mm`（约 1500 行）
  - 基于 ScreenCaptureKit / CoreAudio 的系统音频环回采集
- 新增 `AudioDumpRecorder`：支持独立应用的音频转储录制功能
- 修复 macOS 上字体渲染异常与跨平台 UI 样式差异（`PinkXPStyle`）
- 修复 macOS 独立运行时的卡顿问题

### 3. 性能计数系统（PerformanceCounterSystem）
- 新增完整的性能计数系统（`source/perf/PerformanceCounterSystem.cpp/.h`，约 1200 行）
- 支持按模块统计 UI 帧率、绘制耗时、音频线程负载、占用率等
- 插件端可通过 `exportPerfCountersNow()` 导出性能报表
- 正式版关闭 occupancy 日志输出，避免对外网用户造成干扰

---

## ⚡ 二、性能优化（重点）

### 1. 编辑器背景与绘制
- **背景不再每帧实时绘制**：改为缓存复用，仅在必要时重绘背景，大幅降低 UI 线程占用。
- **桌面缓存范围收窄到 workspace**，并把 logo 预合成进缓存，减少每帧绘制开销。
- **自适应 FPS 策略改进**：不再因一次性掉帧就长期降档，可从当前档位逐步回升；修复了长时间运行后帧率稳定卡在 30FPS 不回升的问题。
- 新增统一 `UiFrameClock` UI 帧时钟，用于各模块统一节拍调度。

### 2. 频谱 / 瀑布图模块（SpectrumModule / SpectrogramModule）
- 引入 **vector / cache 优化**，将大量临时分配替换为复用缓冲。
- **缩短/替换 specLock**，快照处理移到锁外，显著降低锁争用。
- 渲染路径算力进一步压缩。

### 3. 电子宠物模块（Tamagotchi）
- 重绘改为 **变化驱动**：仅在位置 / 帧 / 运动模式 / HUD 可见数值变化时才触发重绘。
- 引入 **局部脏区重绘**：以旧/新边界并集替代高频全量 repaint。
- 动态刷新率：
  - 活跃态 **20 Hz**
  - 中间态 **10 Hz**
  - 静止态 **5 Hz**
  - 状态切换时及时恢复
- 需求演进改为按真实时间步进，降频后行为速度不再异常。
- 信号链路按需计算：**只有存在 Tamagotchi 模块时才计算并分发 signal01**，未启用时零开销。

### 4. 编译与链接层优化
- 引入 **PGO（Profile-Guided Optimization）** 流程，Release 构建进一步提速。
- 开启多线程编译、Release 额外优化选项。
- 适配 clangd 插件，改善开发体验（不影响终端用户）。

### 5. 其他模块性能
- Oscilloscope（示波器）、Phase（相位）、Loudness（响度）、Waveform（波形）、FineSplit（分屏）等模块均加入按帧节流、脏区/缓存绘制、减少重复计算等改动。
- 算法层优化（P4-P6）——针对低分辨率显示档位大幅降低算力占用。

---

## 🛠 三、体验与显示规则优化

### 1. 响度 / True Peak 显示规则更新
- **True Peak** 改为按块统计：每次快照发布后清零，下一块重新累计，避免长时间"钉"在历史最大值。
- **TP-L / TP-R 引入短保持 + 自动回落**：
  - 保持时长约 **2 秒**（60 帧 @30fps）
  - 回落速率约 **8.4 dB/s**
  - 读数更贴近当前真实响度。

### 2. Hide 按钮算法优化
- 重写 Hide 状态下窗口尺寸/坐标的反算逻辑，使显示/收起切换在任何初始位置都稳定可靠。
- 保证重启后能正确恢复"show 态"布局。

### 3. 首次启动默认窗口定位
- 首次启动独立应用时，窗口**默认贴屏幕顶部**（与默认 horizontal bar 预设语义一致），不再居中，减少新用户的手动调整成本。

### 4. 图片/皮肤模块优化
- 精简并优化图片加载与绘制流程，降低内存占用与首次加载耗时。

---

## 🐞 四、问题修复

| # | 问题 | 修复内容 |
|---|---|---|
| 1 | 长时间运行后 FPS 卡在 ~30 FPS 不回升 | 调整自适应 FPS 策略，允许逐步回档 |
| 2 | macOS 字体渲染异常 | 修复 `PinkXPStyle` 字体兼容性 |
| 3 | macOS 独立运行卡顿 | 优化音频/渲染调度 |
| 4 | CMakePresets 会覆盖用户原有预设 | 修复 preset 合并逻辑 |
| 5 | 前置增益调整后 LUFS-I 不重置 | 增益变化时音频线程内安全 reset |
| 6 | 首次启动窗口位置不合理 | 默认贴屏幕 userArea 顶部 |

---

## 📦 五、工程与构建变更

- `CMakeLists.txt` 大规模重构：多平台支持、PGO、Release 优化、JUCE 适配。
- 新增 `CMakePresets.json`：提供标准化构建预设。
- 新增 `.clangd` / `.gitignore` 更新，改善跨平台开发体验。
- `Y2Kmeter_installer.iss`：Windows 安装包版本号同步到 **v1.8.0**。

---

## 📋 六、完整提交记录（按时间倒序）

| 日期 | 作者 | 说明 |
|---|---|---|
| 2026-05-07 | jy | mac 打包优化 |
| 2026-05-07 | sweetorange1 | **v1.8 正式版** |
| 2026-05-07 | sweetorange1 | 优化图片模块和默认打开的状态 |
| 2026-05-07 | sweetorange1 | 优化算法提升性能 P4-P6 |
| 2026-05-07 | sweetorange1 | 优化算法提升性能 |
| 2026-05-07 | sweetorange1 | 修复 mac 卡顿 |
| 2026-05-06 | jy | CMake 适配 mac 改动 |
| 2026-05-06 | sweetorange1 | 修复 mac 字体 bug |
| 2026-05-06 | sweetorange1 | 跨平台合并：保留 Windows 主干优化并合入 macOS 适配 |
| 2026-05-06 | sweetorange1 | **v1.7 正式版**：前置增益、Hide 算法、响度显示规则 |
| 2026-05-04 | sweetorange1 | 合并 background_render 优化 |
| 2026-05-03 | SunWater | 修复 Editor 背景缓存帧率掉到 30FPS 且不回升 |
| 2026-05-03 | SunWater | 加入 PGO 优化 |
| 2026-05-03 | SunWater | 缩短/替换 specLock，快照锁外处理 |
| 2026-05-03 | SunWater | SpectrumModule vector/cache 优化 |
| 2026-05-03 | SunWater | 修复 CMakePresets 覆盖问题 |
| 2026-05-03 | SunWater | 不再实时绘制背景，使用缓存 |
| 2026-05-03 | SunWater | 加入性能计数部分 |
| 2026-05-03 | SunWater | 适配 clangd、多线程编译、Release 优化 |
| 2026-04-30 | sweetorange1 | **v1.6 正式版** |
| 2026-04-29 | sweetorange1 | Tamagotchi 优化算力占用并保持动画流畅 |
| 2026-04-29 | sweetorange1 | Release 关闭 occupancy 日志，bump to v1.6 |

---

## ✅ 升级建议

- **推荐所有用户升级**：本次版本在 UI 帧率稳定性、算力占用、响度显示准确度等方面均有显著改善。
- macOS 用户：v1.8 为首个正式支持版本，欢迎体验并反馈。
- 自定义布局的用户：状态迁移已兼容处理，`analysisInputGainDb` 字段会被读取并恢复。

