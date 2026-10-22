// ==========================================================
// LinuxWindowSupport.h
//   Linux 桌面窗口集成（当前聚焦 Hyprland/Wayland + XWayland 场景）。
//
//   背景：
//     Y2Kmeter 的窗口是无边框浮动条形态。Windows 下靠 AppBar
//     （SHAppBarMessage）实现"驻留条 + 工作区让位"；Linux 没有对应 API，
//     且 Hyprland 等平铺式合成器默认会把普通窗口塞进平铺布局（表现为窗口
//     被拉满屏幕、无法拖动）。这里通过 hyprctl IPC 把本进程窗口显式设为
//     浮动 / 置顶 / 贴边。
//
//   注意：
//     · 只在 Hyprland 下生效（环境变量 HYPRLAND_INSTANCE_SIGNATURE 存在）。
//       其它 Wayland 合成器/平铺 WM 需要用户在配置里加窗口规则（见 docs）。
//     · Hyprland 0.55 起配置与 dispatch 改为 Lua（hl.dsp.*），更早版本是
//       hyprlang 语法；两者都做了适配。
//     · 本头文件刻意不引用任何 JUCE 类型：Y2KStandaloneApp.cpp 会包含
//       juce_audio_plugin_client 的 juce_IncludeModuleHeaders.h，它把
//       `Component` 宏定义成 `juce::Component`，在那些 TU 里写
//       `juce::Component` 会被展开坏掉。窗口定位一律按 pid 进行，
//       不需要 Component 指针。
//     · 所有函数在非 Linux 平台都退化为返回 false 的空实现，方便跨平台编译。
// ==========================================================

#pragma once

namespace y2k
{
namespace linuxwm
{

// 当前会话是否是 Hyprland（只看环境变量，不启动子进程）
bool isHyprland();

// 把 pid 对应的窗口设为浮动（floating=true）或恢复平铺。成功返回 true。
//   pid 传 0 表示"当前进程"。内部通过 `hyprctl -j clients` 按 pid 定位窗口
//   address，因此不要求窗口处于焦点态。
bool ensureFloating (int pid, bool floating);

// 置顶/取消置顶（Hyprland 的 pin 语义 = 在所有工作区可见 + 保持最前）。
bool setPinned (int pid, bool pinned);

// 把窗口提到最上层（不改变 pinned 状态）。
bool raiseToTop (int pid);

// X11 像素坐标系下的显示器区域（即 JUCE Display::totalArea 的原始值）。
//   用于把"X11 物理像素坐标"换算成 Hyprland 的逻辑坐标。
struct X11DisplayArea { int x = 0; int y = 0; int w = 0; int h = 0; };

// 把 pid 窗口移动到指定的 X11 像素坐标（xPixels/yPixels）。
//   · Hyprland 对 X11 浮动窗口的 ConfigureRequest **只采纳尺寸、不采纳位置**
//     （窗口会在映射时被居中），所以位置必须走 hyprctl dispatch 显式搬。
//   · 内部按窗口所在显示器的 scale 把 X11 像素换算成 Hyprland 逻辑坐标。
bool moveWindowToX11Pixels (int pid, int xPixels, int yPixels, X11DisplayArea display);

// 启动/重启时的一次性"收养"：确保当前进程主窗浮动 + 提到最前，
// 避免被合成器平铺拉满屏幕。非 Hyprland 返回 false（调用方按无所谓处理）。
bool adoptMainWindow();

} // namespace linuxwm
} // namespace y2k
