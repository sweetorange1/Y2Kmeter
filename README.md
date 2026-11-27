<p align="center">
  <img src="assets/logo.png" alt="Y2Kmeter Logo" width="128">
</p>

<h1 align="center">Y2Kmeter</h1>

<p align="center"><strong>Y2K 像素复古音频分析仪</strong> — VST3 / AU / Standalone</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-3.5.0-blue" alt="Version">
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20macOS-lightgrey" alt="Platform">
  <img src="https://img.shields.io/badge/license-GPL--3.0-green" alt="License">
</p>

---

## 概述

**Y2Kmeter** 是一款面向音乐制作人、主播与音频爱好者的开源专业音频分析仪，默认呈现经典的 **Windows 95-XP 像素复古粉色（Pink XP）** 视觉风格，并提供 **6 套深度耦合模块工艺的视觉皮肤**（像素复古 / 千禧金属 / 现代简约 / 玻璃 / 霓虹赛博 / CRT 终端）与**正交背景模式**（正常 / 绿幕抠像 / 真透明）。支持 **VST3、AU 和独立应用（Standalone）** 三种形态，全模块嵌入态与脱离浮动窗口均走统一 OpenGL 渲染管线；Standalone Windows 版原生支持**定向程序音频采集（Application Audio Capture）**。

> 分类：Analyzer / Fx | BundleID：`cn.iisaacbeats.Y2Kmeter` | 开源协议：GPL-3.0

---

## 功能模块

| 模块 | 功能描述 |
|------|---------|
| **响度与电平表** | ITU-R BS.1770-4（LUFS-M / LUFS-S / LUFS-I）+ 立体声 RMS & True Peak（4× 过采样）+ 目标参考线 |
| **相位相关仪** | Goniometer 矢量示波器 / Correlation / Width / Balance，支持分项自由组合 |
| **动态范围** | Peak / RMS / Crest / Short-DR / Integrated-DR + 峰值因数历史曲线 |
| **频谱分析** | 20Hz~20kHz（Log / Mel / Linear），双路 FFT（2048 + 8192），能量幕 / 填充 / 轮廓 / 柱状多样式与峰值保持 |
| **频谱瀑布图 / 3D 语谱图** | 2D 四向滚动像素瀑布图（Classic / Sharp / Sharper 重分配）与 3D 立体山峰曲面语谱图 |
| **示波器 / 波形历史** | Waveform / X-Y / Lissajous（三频段点云染色 + 自动/手动缩放 + 按住慢放）与滚动瀑布波形（音高/响度/齿音着色、双声道、对称切换） |
| **VU 表** | 指针式（4 款复古/演播室表盘，支持单表同轴双针）与横向条形（5 款样式）双家族，含 Mono / Stereo / L / R 四声道模式 |
| **立体声场 / 立体声频段图** | 半圆雷达声像指示（三路独立残影）与逐频段 L/R 声像分布 / 单声道相位抵消检测 |
| **实时节拍与调性** | 后台线程实时估算 BPM、大小调与关系调（Standard / Camelot 记号） |
| **录音机** | 常驻 60 秒内存滚动回溯录音 + 最长 1 小时连续 WAV 录制导出 |
| **电脑状态 / 小工具** | CPU / GPU / 温度 / 内存 / 显存 / 磁盘 / 网络 / 电池监控；计算器 / 屏幕取色器 / 秒表 / 倒计时 / 音频换算 / 时钟 / 便签 |
| **VirtuPet / 拼豆像素画** | 随音频信号互动的像素电子宠物状态机，支持拖入图片生成桌面拼豆贴画 |
| **Milkdrop 可视化** 🔥 | 基于 **libprojectM 4** 原生 OpenGL 渲染，内置精选 `.milk` 预设、收藏库与 38 种后处理特效 |

---

## 视觉风格、背景模式与布局预设

- **6 套全模块耦合视觉皮肤**：像素复古（Pixel Retro）、千禧金属（Millennium Chrome）、现代简约（Modern Studio）、玻璃（Frosted Glass）、霓虹赛博（Neon Cyber）、CRT 终端（CRT Terminal），配合 12 套预设配色与 OKLCH 调色工作室（Palette Studio）。
- **正交背景模式（SKIN 二级菜单）**：正常（Normal）/ 绿幕抠像（Green Screen）/ 真透明（Transparent，Windows 采用 DWM 硬件透明合成），切入绿幕或透明时自动继承当前皮肤的全部仪表质感。
- **场景化布局预设**：涵盖「日常与桌面」「混音与音频分析」「视觉与直播」「全屏指挥台」共 14 套预设 + 4 套 Stick 驻留吸附条；任意模块均可脱离为独立浮动窗口，并与主工作区共享统一 OpenGL 渲染与外观。
- **Standalone 音频源**：支持麦克风输入、整机系统输出回环（Windows WASAPI Loopback / macOS ScreenCaptureKit），以及 **Windows 10 2004+ 定向程序音频采集**（仅采集指定应用及其子进程声音）。

---

## Milkdrop 模块

Y2Kmeter v2.1.0 起内置完整 **Milkdrop 可视化引擎**：

- 基于 **libprojectM 4**（OpenGL Core Profile 4.1）：通过 `LoadLibrary` 动态加载 `projectM-4.dll`，零编译期依赖
- 1114 个精选 `.milk` 预设（来自 [Cream of the Crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)），扁平化存储，0 重复
- 66 个基础纹理（来自 projectM 官方 texture pack）
- 预设切换软渐变（soft-cut 1.0s），支持上一个/下一个/随机/指定
- 分辨率缩放（1:1 / 1:2 / 1:4）平衡画质与性能
- 音频实时联动（立体声 PCM 推流 → `bass`/`mid`/`treb` 变量调制视觉效果）
- 预设索引持久化（关闭软件后重新打开自动恢复）
- 100% 本地运行，无需网络，无需 WebView/WebGL

---

## 技术栈

| 项目 | 版本 |
|------|------|
| 语言 | C++17 |
| 框架 | [JUCE](https://juce.com) 8.0.12 |
| OpenGL | Core Profile 4.1（libprojectM 4） |
| FFT | `juce::dsp::FFT` |
| 构建 | CMake ≥ 3.22 |
| 安装器 | Inno Setup（Windows） |

---

## 构建

```bash
# 克隆仓库
git clone https://github.com/iisaacbeats/Y2Kmeter.git
cd Y2Kmeter

# CMake 配置 & 构建（Release）
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --config Release
``` 

**Windows 用户**：构建前请将 `third_party/projectm/bin/projectM-4.dll` 和 `glew32.dll` 放置到可执行文件同目录（CMake Post-build 脚本会自动处理）。

---

## 代码贡献者（Contributors）

衷心感谢以下社区开发者为 Y2Kmeter 提交 PR 与高质量代码贡献 ❤️

- **[@StageGuard](https://github.com/StageGuard)** — [PR #8](https://github.com/sweetorange1/Y2Kmeter/pull/8)：实现 Standalone Windows 定向程序音频采集（Application Audio Capture，进程树级 WASAPI 回环、`AppAudioTargets` 枚举解析、高采样率静音追赶修复、采集线程原子状态与中文本地化及文档同步）。
- **[@Chillizu](https://github.com/Chillizu)** — [PR #6](https://github.com/sweetorange1/Y2Kmeter/pull/6) & [PR #7](https://github.com/sweetorange1/Y2Kmeter/pull/7)：探索并贡献 Linux（Arch / Hyprland + PipeWire + Milkdrop）适配思路，以及示波器 `Auto zoom` 关闭时重置周期峰值统计状态的改进。

---

## 鸣谢

本项目站在众多开源项目的肩膀上，在此致以诚挚感谢 🙏

### libprojectM & Milkdrop 生态

- **[projectM](https://github.com/projectM-visualizer/projectm)** — 将 Milkdrop 可视化引擎带出 Winamp，移植到跨平台 OpenGL 的杰出项目。Y2Kmeter 的 Milkdrop 模块完全基于 libprojectM 4 构建。
- **[ProjectM Team](https://github.com/projectM-visualizer)** — 感谢所有 projectM 维护者和贡献者，让 Milkdrop 生态得以在现代系统中延续。
- **[Ryan Geiss](https://www.geisswerks.com/milkdrop/)** — 致敬 Milkdrop 原始作者，创造了一个影响深远的可视化传奇。
- **[presets-cream-of-the-crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)** — 精选 Milkdrop 预设集，Y2Kmeter 内 100 个预设来源。
- **所有 Milkdrop 预设作者** — 感谢每一位 `.milk` 创作者的想象力与艺术贡献，包括但不限于：suksma、flexi、shifter、martin、rova、fiShbRaiN、EvilTwin、orb、geiss、unChained、Zylot 等。

### 框架与工具

- **[JUCE](https://juce.com)** — 强大的跨平台音频应用框架，提供插件格式、OpenGL 上下文、DSP 等一切基础设施。
- **[Inno Setup](https://jrsoftware.org/isinfo.php)** — Windows 安装包制作工具。
- **[Silkscreen](https://kottke.org/plus/type/silkscreen/)** — 经典的像素英文字体，作为 Y2Kmeter 的默认 UI 字体。
- **视觉风格字体**（均来自 [Google Fonts](https://github.com/google/fonts) / [Noto CJK](https://github.com/notofonts/noto-cjk)）— Inter（现代简约）、Nunito（玻璃）、Orbitron（霓虹赛博）、Exo 2（千禧金属）、VT323（CRT 终端）、Noto Sans SC 思源黑体（非像素风格的中文界面）。

### 音频标准参考

- **ITU-R BS.1770-4** — 响度计量算法标准
- **EBU R 128** — 广播响度规范

---

## 许可

Y2Kmeter 本体以 **GPL-3.0** 开源。

第三方组件许可：

| 组件 | 许可 |
|------|------|
| libprojectM 4 | LGPL-2.1-or-later（动态链接，可替换 DLL） |
| Milkdrop 预设 / 纹理 | LGPL-2.1-or-later |
| JUCE | GPL-3.0 / 商业双授权 |
| Silkscreen 字体 | SIL Open Font License |
| Inter / Nunito / Orbitron / Exo 2 / VT323 / Noto Sans SC 字体 | SIL Open Font License 1.1（原文见 `ttf/skin/licenses/`） |

> 详细许可文本见根目录 `LICENSE` 文件。

## 赞助支持 / Support

全系插件永久免费开源，无付费墙、无功能阉割。如果它帮到了你的音乐制作，欢迎自愿支持开发：
All plugins are 100% free and open source. If this tool helped your music, voluntary support is always appreciated:

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20on%20Ko--fi-FF5E5B?style=flat-square&logo=ko-fi&logoColor=white)](https://ko-fi.com/iisaacbeats)
[![Website](https://img.shields.io/badge/Donate-WeChat%20%2F%20Alipay%20%2F%20TRC20-111111?style=flat-square)](https://iisaacbeats.cn/about.html)

---

<p align="center">
  <em>Made with ❤️ and pixels. Powered by projectM.</em><br>
  &copy; 2024-2026 iisaacbeats
</p>