#pragma once

// ==========================================================
// 翻译入口的轻量头（不依赖 JuceHeader.h）
//
//   背景：Localisation.h 为了字体 / File / 语言包 IO 需要完整的
//   <JuceHeader.h>，而该文件由 JUCE 自动生成，末尾含一句全局的
//   `using namespace juce;`（受 DONT_SET_USING_JUCE_NAMESPACE 控制）。
//
//   Objective-C++ 文件（如 source/standalone/MacDesktopAudioCapture.mm）
//   会在之后 import Foundation / AppKit / AudioToolbox，这些头经
//   CoreServices → CarbonCore 间接引入系统 MacTypes.h；此时其中的 C 类型
//   Point / Component 就会与 juce::Point / juce::Component 撞名，报
//   "reference to 'Point' is ambiguous" 并连带 Finder.h / Components.h 大批报错。
//   反过来把系统头全放在前面又会撞 JUCE 的 "gltypes.h included before
//   juce_gl.h" 规则。
//
//   因此把 y2k::tr / y2k::trf 这两个只需 juce_core 的入口单独拆到本文件：
//   ObjC++ 源文件只 include 本文件，就能在任意位置安全地 import 系统 framework。
//
//   注：本文件的实现仍统一在 Localisation.cpp，无新增依赖。
// ==========================================================

#include <juce_core/juce_core.h>
#include <utility>

namespace y2k::i18n
{
    // ---- 翻译（实现见 Localisation.cpp）----
    juce::String translate (const juce::String& source);

    // 模板翻译："Saved: {0}"（模板本身是翻译键）→ 译文模板 → 依次替换 {0} {1} ...
    juce::String translateFormat (const juce::String& templ, const juce::StringArray& args);
}

namespace y2k
{
    // 便捷入口：y2k::tr ("Hide")
    inline juce::String tr (const juce::String& source) { return i18n::translate (source); }
    inline juce::String tr (const char* source)         { return i18n::translate (juce::String (source)); }
    // StringRef 同时可隐式转换到 const char* 与 juce::String，直接传会二义
    inline juce::String tr (juce::StringRef source)     { return i18n::translate (juce::String (source.text)); }

    // 模板入口：y2k::trf ("Saved: {0}", fileName)、y2k::trf ("Wave {0} of {1}", a, b)
    template <typename... Args>
    juce::String trf (const juce::String& templ, Args&&... args)
    {
        juce::StringArray a;
        (a.add (juce::String (std::forward<Args> (args))), ...);
        return i18n::translateFormat (templ, a);
    }
}
