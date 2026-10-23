// ==========================================================
// PaletteGenerator.h —— 主题生成器（纯 C++，不依赖 JUCE UI）
//
//   ThemeSpec（意图参数）→ Recipe（§5.2 配方）→ Guardrails（§5.3 护栏）
//   → GeneratedTheme（19 个既有槽位 + 扩展字段）
//
//   对应需求案 §5、§6 S1。
// ==========================================================
#pragma once

#include "OkLchUtils.h"
#include "ThemeSpec.h"
#include <array>
#include <string>
#include <vector>

namespace y2kpal
{
    struct GeneratedTheme
    {
        // ---- 19 个既有 PinkXP 槽位（语义不变，S3）----
        std::array<Rgb8, 8> pink;             // pink50..pink700（图表阶梯，= chartRamp）
        Rgb8 hl, face, shdw, dark;            // Win95 边框四色
        Rgb8 ink, sel, selInk;
        Rgb8 desktop, desktop2;
        Rgb8 content, btnFace;

        // ---- 新增扩展字段（S3）----
        Rgb8 accent;                          // 主强调色（= sel）
        Rgb8 accent2;                         // 次强调色（和谐方式派生）
        std::array<Rgb8, 8> chartRamp;        // 图表渐变 8 阶（OKLab 均匀）
        Rgb8 danger, warn, ok;                // 语义状态色（可选）

        // 各角色是否被用户手动覆盖（Q7 提示用）
        std::array<bool, kRoleCount> overridden {};

        // 护栏提示（R16：被自动修正时给出轻提示）
        std::vector<std::string> notes;
    };

    // 由 ThemeSpec 生成完整主题（Recipe + Guardrails）
    GeneratedTheme generate(const ThemeSpec& spec);

    // 从主题的两个关键色反推 ThemeSpec（"基于预设新建" / 旧 custom 迁移用）：
    //   accent     → 色相 + vibe
    //   background → 明暗 + 背景染色
    bool fitSpecFromThemeColors(const Rgb8& accent, const Rgb8& background, ThemeSpec& out);

    // 精选随机配方库（A7：≥ 20 组合法组合，带人工评审通过标记）
    struct RandomRecipe
    {
        float      hue;
        Brightness brightness;
        float      vibe;
        Harmony    harmony;
    };
    const std::vector<RandomRecipe>& getCuratedRecipes();
}
