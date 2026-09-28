#pragma once

#include <JuceHeader.h>

#include "source/ui/modules/MilkdropVisualState.h"
#include "source/ui/modules/MilkdropWaveState.h"
#include "source/ui/modules/MilkdropEffect.h"

// ==========================================================
// MilkdropParamIds —— Milkdrop-only 插件变体的宿主自动化参数定义
//
// 将 Milkdrop 模块控制区的所有可参数化功能暴露为宿主可见参数
// （VST3/AU 参数 + 可被宿主 MIDI CC / automation 驱动）。
//   · 仅 Y2KMETER_MILKDROP_ONLY 编译目标使用，完整版 Y2Kmeter 不包含。
//   · 参数 ID 统一命名，供 PluginProcessor 创建布局与 PluginEditor 双向同步。
//
// 参数分组：
//   · color   : tint_r/g/b + brightness
//   · fx_*    : 38 个后处理开关（注册表驱动）
//   · wave_*  : 简单波形样式覆盖
//   · tweak_* : 后处理 uv 几何畸变
//   · auto_*  : 自动轮播
//   · preset_next/prev/random : 预设切换开关（宿主 off→on 触发一次）
// ==========================================================

namespace MilkdropParams
{
    // ---- color ----
    inline constexpr const char* kTintR      = "md_tint_r";
    inline constexpr const char* kTintG      = "md_tint_g";
    inline constexpr const char* kTintB      = "md_tint_b";
    inline constexpr const char* kBrightness = "md_brightness";

    // ---- effects（前缀 md_fx_ + MilkdropEffectDef::display_name）----
    inline constexpr const char* kFxPrefix = "md_fx_";

    // ---- wave ----
    inline constexpr const char* kWaveEnabled  = "md_wave_enabled";
    inline constexpr const char* kWaveMode     = "md_wave_mode";
    inline constexpr const char* kWaveX        = "md_wave_x";
    inline constexpr const char* kWaveY        = "md_wave_y";
    inline constexpr const char* kWaveR        = "md_wave_r";
    inline constexpr const char* kWaveG        = "md_wave_g";
    inline constexpr const char* kWaveB        = "md_wave_b";
    inline constexpr const char* kWaveA        = "md_wave_a";
    inline constexpr const char* kWaveMystery  = "md_wave_mystery";
    inline constexpr const char* kWaveDots     = "md_wave_dots";
    inline constexpr const char* kWaveThick    = "md_wave_thick";
    inline constexpr const char* kWaveAdditive = "md_wave_additive";
    inline constexpr const char* kWaveBrighten = "md_wave_brighten";

    // ---- tweak ----
    inline constexpr const char* kTweakZoom   = "md_tweak_zoom";
    inline constexpr const char* kTweakRot    = "md_tweak_rot";
    inline constexpr const char* kTweakWarp   = "md_tweak_warp";
    inline constexpr const char* kTweakDx     = "md_tweak_dx";
    inline constexpr const char* kTweakDy     = "md_tweak_dy";
    inline constexpr const char* kTweakSx     = "md_tweak_sx";
    inline constexpr const char* kTweakSy     = "md_tweak_sy";
    inline constexpr const char* kTweakKaleido = "md_tweak_kaleido";
    inline constexpr const char* kTweakFoldX   = "md_tweak_fold_x";
    inline constexpr const char* kTweakFoldY   = "md_tweak_fold_y";

    // ---- auto ----
    inline constexpr const char* kAutoMode     = "md_auto_mode";
    inline constexpr const char* kAutoInterval = "md_auto_interval";

    // ---- 预设切换开关（宿主 off→on 触发一次：下一个/上一个/随机）----
    inline constexpr const char* kPresetNext   = "md_preset_next";
    inline constexpr const char* kPresetPrev   = "md_preset_prev";
    inline constexpr const char* kPresetRandom = "md_preset_random";

    // 效果参数的 ID（"md_fx_" + display_name）
    inline juce::String fxId (const MilkdropEffectDef& def)
    {
        return juce::String (kFxPrefix) + def.display_name;
    }

    // 从参数 ID 反查效果 ID（供 pull/push 使用；找不到返回 nullptr）。
    inline const MilkdropEffectDef* fxDefFromId (const juce::String& id)
    {
        if (! id.startsWith (kFxPrefix))
            return nullptr;
        const auto name = id.substring (juce::String (kFxPrefix).length());
        for (const auto& def : GetMilkdropEffectDefs())
            if (def.implemented && juce::String (def.display_name) == name)
                return &def;
        return nullptr;
    }

    // 创建完整参数布局（color + effects + wave + tweak + auto + 预设切换开关）。
    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        // ---- color ----
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTintR,      "Tint R",      0.0f, 2.0f, 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTintG,      "Tint G",      0.0f, 2.0f, 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTintB,      "Tint B",      0.0f, 2.0f, 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kBrightness, "Brightness",  0.0f, 8.0f, 1.0f));

        // ---- effects（注册表驱动）----
        for (const auto& def : GetMilkdropEffectDefs())
            if (def.implemented)
                layout.add (std::make_unique<juce::AudioParameterBool> (fxId (def), def.display_name, false));

        // ---- wave ----
        layout.add (std::make_unique<juce::AudioParameterBool>  (kWaveEnabled,  "Wave Enabled",  false));
        layout.add (std::make_unique<juce::AudioParameterInt>   (kWaveMode,     "Wave Mode",     0, 15, 6));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kWaveX,        "Wave X",        0.0f, 1.0f, 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kWaveY,        "Wave Y",        0.0f, 1.0f, 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kWaveR,        "Wave R",        0.0f, 1.0f, 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kWaveG,        "Wave G",        0.0f, 1.0f, 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kWaveB,        "Wave B",        0.0f, 1.0f, 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kWaveA,        "Wave Alpha",    0.0f, 1.0f, 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kWaveMystery,  "Wave Mystery", -1.0f, 1.0f, 0.0f));
        layout.add (std::make_unique<juce::AudioParameterBool>  (kWaveDots,     "Wave Dots",     false));
        layout.add (std::make_unique<juce::AudioParameterBool>  (kWaveThick,    "Wave Thick",    false));
        layout.add (std::make_unique<juce::AudioParameterBool>  (kWaveAdditive, "Wave Additive", false));
        layout.add (std::make_unique<juce::AudioParameterBool>  (kWaveBrighten, "Wave Brighten", false));

        // ---- tweak ----
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTweakZoom,   "Tweak Zoom",  -0.3f, 1.0f, 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTweakRot,    "Tweak Rot",   -1.0f, 1.0f, 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTweakWarp,   "Tweak Warp",  -1.0f, 1.0f, 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTweakDx,     "Tweak DX",    -1.0f, 1.0f, 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTweakDy,     "Tweak DY",    -1.0f, 1.0f, 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTweakSx,     "Tweak SX",    -1.0f, 1.0f, 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kTweakSy,     "Tweak SY",    -1.0f, 1.0f, 0.0f));
        layout.add (std::make_unique<juce::AudioParameterInt>   (kTweakKaleido, "Tweak Kaleido", 0, 16, 0));
        layout.add (std::make_unique<juce::AudioParameterInt>   (kTweakFoldX,   "Tweak Fold X",  0, 16, 0));
        layout.add (std::make_unique<juce::AudioParameterInt>   (kTweakFoldY,   "Tweak Fold Y",  0, 16, 0));

        // ---- auto ----
        layout.add (std::make_unique<juce::AudioParameterBool>  (kAutoMode,     "Auto Mode",     false));
        layout.add (std::make_unique<juce::AudioParameterFloat> (kAutoInterval, "Auto Interval", 1.0f, 60.0f, 10.0f));

        // ---- 预设切换开关（宿主 off→on 触发一次）----
        layout.add (std::make_unique<juce::AudioParameterBool> (kPresetNext,   "Preset Next",   false));
        layout.add (std::make_unique<juce::AudioParameterBool> (kPresetPrev,   "Preset Prev",   false));
        layout.add (std::make_unique<juce::AudioParameterBool> (kPresetRandom, "Preset Random", false));

        return layout;
    }
}
