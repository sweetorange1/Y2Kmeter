<p align="center">
  <img src="assets/logo.png" alt="Y2Kmeter Logo" width="128">
</p>

<h1 align="center">Y2Kmeter</h1>

<p align="center"><strong>Y2K 像素复古音频分析仪</strong> — VST3 / AU / Standalone</p>

<p align="center">
<img src="https://img.shields.io/badge/version-2.2.4-blue" alt="Version">
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20macOS-lightgrey" alt="Platform">
  <img src="https://img.shields.io/badge/license-GPL--3.0-green" alt="License">
</p>

---

## 概述

**Y2Kmeter** 是一款面向音乐制作人的专业音频分析插件，带有强烈的 **Windows 95-XP 像素复古粉色（Pink XP）** 视觉主题。支持 VST3、AU 和独立应用（Standalone）三种形态。

> 分类：Analyzer / Fx | BundleID：`cn.iisaacbeats.Y2Kmeter` | 开源协议：GPL-3.0

---

## 功能模块

| 模块 | 功能描述 |
|------|---------|
| **电平表** | 立体声 RMS L/R + True Peak L/R，含数值溢出计数 |
| **响度计** | ITU-R BS.1770-4（LUFS-M / LUFS-S / LUFS-I），含 rang 计算 |
| **相位相关仪** | Correlation / Width / Balance / Goniometer 矢量示波器 |
| **动态范围** | Peak / RMS / Crest / Short-DR / Integrated-DR |
| **频谱分析** | 对数轴 20Hz~20kHz，双路 FFT（2048 + 8192），dBFS 纵轴，峰值保持 |
| **频谱瀑布图** | 像素方格风格 Spectrogram，时间×频率 滚动热力图 |
| **示波器 / 波形** | X-Y / Lissajous / 持续滚动瀑布波形 |
| **VU 表** | 模拟指针式 VU 表，带峰值灯 |
| **EQ 频谱** | Y2K 主题的 6 段 EQ 频谱可视化（仅显示，不处理音频） |
| **Tamagotchi 电子宠物** | 音频信号驱动的像素小怪，含孵化/觅食/睡眠/生病/死亡完整状态机 |
| **拼豆像素画** | 用户拖入图片生成拼豆像素画，可贴到桌面背景 |
| **Milkdrop 可视化** 🔥 | 基于 **libprojectM 4** 原生 OpenGL 渲染，本地打包 1114 个真实 Milkdrop 预设 |

---

## Milkdrop 模块

Y2Kmeter v2.1.0 起内置完整 **Milkdrop 可视化引擎**：

- 基于 **libprojectM 4**（OpenGL Core Profile 4.1）：通过 `LoadLibrary` 动态加载 `projectM-4.dll`，零编译期依赖
- 1114 个精选 `.milk` 预设（来自 [Cream of the Crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)），扁平化存储，0 重复
- 66 个基础纹理（来自 projectM 官方 texture pack）
- 预设切换软渐变（soft-cut 1.0s），支持上一个/下一个/随机/指定
- 分辨率缩放（1:1 / 1:2 / 1:4）平衡画质与性能
- 音频实时联动（立体声 PCM 推流 → `bass`/`mid`/`treb` 变量驱动视觉效果）
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

> 详细许可文本见根目录 `LICENSE` 文件。

---

<p align="center">
  <em>Made with ❤️ and pixels. Powered by projectM.</em><br>
  &copy; 2024-2026 iisaacbeats
</p>