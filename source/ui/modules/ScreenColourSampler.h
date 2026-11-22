#pragma once

// ==========================================================
// ScreenColourSampler —— 屏幕取色（小工具模块的取色器用）
//
//   本头文件刻意不依赖 JUCE：macOS 实现是 Objective-C++（.mm），需要在
//   不引入 JuceHeader.h 的前提下 import AppKit（见 I18nTranslate.h 的说明）。
//
//   · Windows：读取光标周围的屏幕像素（GDI BitBlt，公开 API）。
//     取色器"按住吸管拖到屏幕任意位置、松手取色"，拖动过程中实时放大预览。
//   · macOS：直接读屏需要"屏幕录制"授权；这里改用系统取色放大镜
//     NSColorSampler（macOS 10.15+，无需任何授权），点一下吸管即弹出。
// ==========================================================

#include <cstdint>
#include <functional>

namespace y2k::screencolour
{
    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
    };

    // 是否支持"按住拖动、实时读取光标下像素"（Windows）
    bool supportsLiveSampling() noexcept;

    // 读取光标周围 (2*radius+1)² 个像素，行优先、左上起；out 至少要有这么多元素
    bool sampleAroundCursor (int radius, Rgb* out);

    // 是否支持系统取色放大镜（macOS 10.15+）
    bool supportsSystemSampler() noexcept;

    // 弹出系统取色放大镜；用户选中或取消后在消息线程回调（取消时 ok = false）
    void pickWithSystemSampler (std::function<void (bool ok, Rgb colour)> callback);
}
