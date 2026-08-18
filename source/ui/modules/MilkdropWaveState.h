#pragma once

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// ==========================================================
// MilkdropWaveState —— Milkdrop 简单波形（simple waveform）样式覆盖状态
//
// MilkDrop 预设文件（.milk）里通过 wave_* 变量控制内置简单波形的绘制样式。
// libprojectM 4 的 C API 没有运行时修改这些参数的接口，因此本模块采用
// "加载时注入"方案：在 projectm_load_preset_data 之前，把本结构体里被
// 启用的覆盖值写入 .milk 文本，再交给 projectM 编译渲染。
//
// 字段语义（对齐 MilkDrop 2 官方 preset authoring 文档）：
//   注意：以下字段是"语义参数"，注入 .milk 时映射到预设块 [preset00] 的
//   对应字段（右侧括号内），而非 per_frame 运行时变量名。
//   · mode      —— 波形类型（预设块 nWaveMode，0~7 为 MilkDrop 2 经典 8 种，
//                 8~15 为 MilkDrop 3 扩展，projectM 4.1 可能忽略后者）。
//   · x/y       —— 波形中心位置（wave_x/wave_y，0=左/底，1=右/顶）。
//   · r/g/b     —— 波形颜色（wave_r/wave_g/wave_b，0..1）。
//   · a         —— 波形不透明度（预设块 fWaveAlpha，0=透明，越大越亮）。
//   · mystery   —— 波形参数（预设块 fWaveParam，-1..1），含义随 mode 变化。
//   · usedots   —— 用点而非线绘制（预设块 bWaveDots）。
//   · thick     —— 加粗绘制（预设块 bWaveThick）。
//   · additive  —— 加性绘制，颜色叠加更亮（预设块 bAdditiveWaves）。
//   · brighten  —— 颜色等比放大到至少一通道为 1.0（预设块 bMaximizeWaveColor）。
//
// 全局共享（与 MilkdropVisualState 同构）：所有 Milkdrop 模块同一状态，
// 由 Editor 持有并持久化到 Processor host state；模块内的 wave 面板通过
// sync/apply 读写 Editor 全局状态。
// ==========================================================
struct MilkdropWaveState
{
    // enabled=false 表示"不覆盖"，预设使用自身 .milk 里的 wave 样式。
    // 用户第一次调整任一参数时自动置 true；Reset 恢复 false。
    bool  enabled = false;

    int   mode     = 6;    // → nWaveMode（默认 6 = Line 单线）
    float x        = 0.5f; // → wave_x
    float y        = 0.5f; // → wave_y
    float r        = 1.0f; // → wave_r
    float g        = 1.0f; // → wave_g
    float b        = 1.0f; // → wave_b
    float a        = 1.0f; // → fWaveAlpha
    float mystery  = 0.0f; // → fWaveParam
    bool  usedots  = false; // → bWaveDots
    bool  thick    = false; // → bWaveThick
    bool  additive = false; // → bAdditiveWaves
    bool  brighten = false; // → bMaximizeWaveColor

    // 未启用覆盖时返回 true，加载预设时直接透传原文本（零开销）。
    bool isNeutral() const noexcept { return !enabled; }

    // 恢复默认（关闭覆盖 + 参数回中性），供 Reset 按钮调用。
    void reset() noexcept
    {
        enabled  = false;
        mode     = 6;
        x        = 0.5f;
        y        = 0.5f;
        r        = 1.0f;
        g        = 1.0f;
        b        = 1.0f;
        a        = 1.0f;
        mystery  = 0.0f;
        usedots  = false;
        thick    = false;
        additive = false;
        brighten = false;
    }
};

// wave_mode 的 16 种类型显示名（前 8 种为 MilkDrop 2 经典类型，名称来自
// projectM touch API 的 projectm_touch_type 枚举；后 8 种为 MilkDrop 3 扩展，
// projectM 4.1 可能忽略，故用中性占位名）。
inline const char* GetWaveModeName(int mode)
{
    static const char* kNames[16] = {
        "Circle",        // 0
        "Radial Blob",   // 1
        "Blob 2",        // 2
        "Blob 3",        // 3
        "Derivative",    // 4
        "Blob 5",        // 5
        "Line",          // 6
        "Double Line",   // 7
        "Wave 8",        // 8
        "Wave 9",        // 9
        "Wave 10",       // 10
        "Wave 11",       // 11
        "Wave 12",       // 12
        "Wave 13",       // 13
        "Wave 14",       // 14
        "Wave 15",       // 15
    };
    if (mode < 0)      mode = 0;
    if (mode > 15)     mode = 15;
    return kNames[mode];
}

// 在一行文本中，把某个 wave 键（如 "wave_mode"）后跟的赋值（支持
// "key=value" 与 "key = value" 两种写法）替换为目标值。循环查找以覆盖
// 同一行出现多次、以及短键作为长键前缀（wave_a 是 wave_additive 前缀）的情况。
inline bool ReplaceWaveKeyValue(std::string& line,
                                const std::string& key,
                                const std::string& value)
{
    bool replaced = false;
    std::size_t search_from = 0;
    while (true)
    {
        const std::size_t pos = line.find(key, search_from);
        if (pos == std::string::npos)
            break;

        std::size_t eq = pos + key.size();
        while (eq < line.size() && (line[eq] == ' ' || line[eq] == '\t'))
            ++eq;
        if (eq < line.size() && line[eq] == '=')
        {
            const std::size_t val_start = eq + 1;
            std::size_t val_end = line.find(';', val_start);
            if (val_end == std::string::npos)
                val_end = line.size();
            line.replace(val_start, val_end - val_start, value);
            replaced = true;
            search_from = val_start + value.size();
        }
        else
        {
            search_from = eq + 1;
        }
    }
    return replaced;
}

// 把 wave 样式覆盖注入到 .milk 预设文本中：
//   · 逐行替换已有的 wave_* 键值（静态键与 per_frame 代码内的赋值都处理）；
//   · 文件中缺失的键追加到文本末尾（作为静态初始值）。
// enabled=false 时原样返回（零开销）。
inline std::string ApplyWaveParamsToPresetText(const std::string& data,
                                               const MilkdropWaveState& wave)
{
    if (wave.isNeutral())
        return data;

    char buf[64];
    auto fstr = [&](float v)
    {
        std::snprintf(buf, sizeof(buf), "%.5f", static_cast<double>(v));
        return std::string(buf);
    };
    auto istr = [](int v) { return std::to_string(v); };
    auto bstr = [](bool v) { return std::string(v ? "1" : "0"); };

    // 顺序按 key 长度降序，减少短键作为长键前缀时的误判（虽已有循环查找兜底）。
    // 注意：这里必须使用 .milk 预设块 [preset00] 里的字段名，而不是 per_frame
    // 运行时变量名。simple waveform 的静态样式由以下预设块字段决定：
    //   nWaveMode(模式) / fWaveAlpha(alpha) / fWaveParam(mystery)
    //   bWaveDots / bWaveThick / bAdditiveWaves / bMaximizeWaveColor
    //   wave_x / wave_y / wave_r / wave_g / wave_b（这几个与运行时变量同名）
    const std::vector<std::pair<std::string, std::string>> kv = {
        {"bAdditiveWaves",     bstr(wave.additive)},
        {"bMaximizeWaveColor", bstr(wave.brighten)},
        {"bWaveDots",          bstr(wave.usedots)},
        {"bWaveThick",         bstr(wave.thick)},
        {"fWaveAlpha",         fstr(wave.a)},
        {"fWaveParam",         fstr(wave.mystery)},
        {"nWaveMode",          istr(wave.mode)},
        {"wave_x",             fstr(wave.x)},
        {"wave_y",             fstr(wave.y)},
        {"wave_r",             fstr(wave.r)},
        {"wave_g",             fstr(wave.g)},
        {"wave_b",             fstr(wave.b)},
    };

    std::vector<bool> found(kv.size(), false);

    std::string result;
    result.reserve(data.size() + 256);

    std::istringstream in(data);
    std::string line;
    while (std::getline(in, line))
    {
        for (std::size_t i = 0; i < kv.size(); ++i)
        {
            if (ReplaceWaveKeyValue(line, kv[i].first, kv[i].second))
                found[i] = true;
        }
        result += line;
        result += '\n';
    }

    // 缺失的键追加到末尾，作为静态初始值。
    for (std::size_t i = 0; i < kv.size(); ++i)
    {
        if (!found[i])
        {
            result += kv[i].first;
            result += '=';
            result += kv[i].second;
            result += '\n';
        }
    }

    return result;
}
