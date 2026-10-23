// ==========================================================
// ThemeSpec.h —— 主题意图参数 + 手动覆盖（纯 C++，可序列化）
//
//   三层结构的"用户意图层"：
//     色相 / 明暗 / 氛围(Vibe) / 背景染色 / 和谐方式 / 随机锁定
//     + 7 个语义角色的手动覆盖（自动/手动开关）
//     + 桌面纹理 / 标题图标
//
//   可序列化为：
//     · 二进制 blob（toBinary / fromBinary，28 字节定长）
//     · 分享码（shareCode / fromShareCode：`Y2KT1-` + Base64url，≤ 80 字符）
//
//   对应需求案 §3、§4.3（R7/R8）、§4.4（R12）。
// ==========================================================
#pragma once

#include "OkLchUtils.h"
#include <cstdint>
#include <string>
#include <array>

namespace y2kpal
{
    // 明暗策略（§5.2 配方表的三种模式）
    enum class Brightness : uint8_t
    {
        Dark    = 0,   // 暗色
        Light   = 1,   // 亮色
        AutoMid = 2    // 中间调（彩色桌面风格）
    };

    // 和谐方式：决定 accent2 相对主色相的偏移（§5.2 Accent2 行）
    enum class Harmony : uint8_t
    {
        Mono          = 0,   // 同色相（仅 L 偏移 + 15° 小漂移）
        Analogous     = 1,   // 邻近 ±30°
        Complementary = 2,   // 互补 +180°
        Split         = 3,   // 分裂互补 +150°
        Triad         = 4    // 三合 +120°
    };

    // 桌面纹理（对应 PinkXP::DesktopPattern 的 7 种）
    enum class Pattern : uint8_t
    {
        Checker     = 0,
        PixelStars  = 1,
        ScanGrid    = 2,
        BigDots     = 3,
        Bubbles     = 4,
        DiagStripes = 5,
        Hearts      = 6,
        Zigzag      = 7,     // 锯齿波纹
        CrossHatch  = 8,     // 交叉斜线网
        PixelFlower = 9      // 像素小花（中心+四花瓣）
    };

    // 语义角色（§4.3 R7，约 8-10 个，这里取 7 个可直接编辑的角色）
    enum class Role : uint8_t
    {
        Accent     = 0,   // 主强调色 → sel / swatch / pink400
        Accent2    = 1,   // 次强调色 → 图表渐变终点
        Background = 2,   // 桌面背景 → desktop / desktop2
        Surface    = 3,   // 模块画布 → content
        Control    = 4,   // 控件面 → btnFace / face / hl / shdw / dark
        Text       = 5,   // 正文文字 → ink
        OnAccent   = 6,   // 强调色上的文字 → selInk
        COUNT      = 7
    };

    constexpr int kRoleCount = (int) Role::COUNT;

    struct ThemeSpec
    {
        float      hue        = 330.0f;             // 主色相 [0, 360)
        Brightness brightness = Brightness::Dark;
        float      vibe       = 0.55f;              // 氛围 [0,1]：Soft → Vivid
        float      bgTint     = 0.30f;              // 背景染色 [0,1]：Neutral → Tinted
        Harmony    harmony    = Harmony::Analogous;
        Pattern    pattern    = Pattern::Checker;   // 桌面纹理
        uint8_t    titleIcon  = 0;                  // 标题图标索引（0..7）

        // 随机锁定（🎲 时保持不变）
        bool lockHue        = false;
        bool lockBrightness = false;
        bool lockVibe       = false;
        bool lockHarmony    = false;
        bool lockBgTint     = false;

        // 角色手动覆盖（set=true 表示用户手动锁定该角色）
        struct Override
        {
            bool  set = false;
            Rgb8  rgb {};
        };
        std::array<Override, kRoleCount> overrides {};

        // ---- 二进制序列化（28 字节定长）----
        std::string toBinary() const;
        static bool fromBinary(const std::string& bin, ThemeSpec& out);

        // ---- 分享码（R12）：`Y2KT1-` + Base64url ----
        std::string shareCode() const;
        static bool fromShareCode(const std::string& code, ThemeSpec& out);
    };

    // 角色显示名（UI 用，纯 ASCII 便于单元测试环境）
    const char* roleName(Role r);
}
