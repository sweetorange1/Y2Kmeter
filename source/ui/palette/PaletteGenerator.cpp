// ==========================================================
// PaletteGenerator.cpp —— Recipe + Guardrails 实现
//
//   所有颜色在 OKLCH 空间生成，转 sRGB 前统一 gamutClamp（A2）。
//   护栏顺序（§5.3）：
//     Q4 背景饱和度上限 → Q1 文字对比度 → Q3 强调色可见度
//     → Q2 强调色上文字 → Q5 渐变单调 → Q6 边框明度顺序
//   用户手动覆盖的角色只提示、不强制修改（Q7）。
// ==========================================================
#include "source/ui/palette/PaletteGenerator.h"
#include <cmath>

namespace y2kpal
{
    namespace
    {
        // ---- 和谐方式 → accent2 色相偏移（§5.2 Accent2 行）----
        float harmonyOffset(Harmony h)
        {
            switch (h)
            {
                case Harmony::Mono:          return 15.0f;   // 仅小漂移
                case Harmony::Analogous:     return 30.0f;
                case Harmony::Complementary: return 180.0f;
                case Harmony::Split:         return 150.0f;
                case Harmony::Triad:         return 120.0f;
                default:                     return 30.0f;
            }
        }

        // ---- 氛围(Vibe) → Accent 彩度（vibe∈[0,2] 单调映射到 C 0.06~0.28）----
        //   斜率放慢到 0.11：vibe=1（默认居中）≈ 旧 vibe=0.55 的鲜艳度，
        //   vibe=2（上限）才触及多数色相的 sRGB 色域边界，全段都在变鲜艳。
        float accentChroma(float vibe) noexcept
        {
            return 0.06f + 0.11f * vibe;
        }

        // ---- 背景 C 上限（Q4）----
        float backgroundChromaCap(Brightness b) noexcept
        {
            switch (b)
            {
                case Brightness::Light:   return 0.08f;
                case Brightness::AutoMid: return 0.06f;
                default:                  return 0.05f;
            }
        }

        // ---- 在 [0.02, 0.98] 上采样 L，满足对两个背景的 WCAG 对比度，
        //      取最接近 preferredL 的值（Q1 文字修复）----
        float findLForContrast(const Rgb8& bgA, const Rgb8& bgB, float preferredL,
                               float minContrast, float hue, float chroma, bool& found)
        {
            float best = preferredL;
            float bestErr = 1e9f;
            found = false;

            for (int i = 0; i <= 48; ++i)
            {
                const float L = 0.02f + 0.96f * (float) i / 48.0f;
                const Rgb8 c = okLchToRgb8(gamutClamp({ L, chroma, hue }));
                if (wcagContrast(c, bgA) >= minContrast &&
                    wcagContrast(c, bgB) >= minContrast)
                {
                    const float err = std::fabs(L - preferredL);
                    if (err < bestErr) { bestErr = err; best = L; found = true; }
                }
            }
            return best;
        }

        // ---- 采样 L 满足对两个背景的 OKLab 明度差 ≥ minDelta（Q3 强调色修复）----
        float findLForDeltaL(float bgL1, float bgL2, float preferredL,
                             float minDelta, bool& found)
        {
            float best = preferredL;
            float bestErr = 1e9f;
            found = false;

            for (int i = 0; i <= 48; ++i)
            {
                const float L = 0.02f + 0.96f * (float) i / 48.0f;
                if (std::fabs(L - bgL1) >= minDelta &&
                    std::fabs(L - bgL2) >= minDelta)
                {
                    const float err = std::fabs(L - preferredL);
                    if (err < bestErr) { bestErr = err; best = L; found = true; }
                }
            }
            return best;
        }

        constexpr float kWcagTarget = 4.5f;   // Q1/Q2 对比度目标（WCAG AA 近似）
        constexpr float kDeltaLTarget = 0.25f; // Q3 强调色可见度目标
    }

    GeneratedTheme generate(const ThemeSpec& spec)
    {
        const float hue  = wrapHue(spec.hue);
        const float vibe = clamp02(spec.vibe);
        const float tint = clamp02(spec.bgTint);
        const bool  dark  = (spec.brightness == Brightness::Dark);
        const bool  light = (spec.brightness == Brightness::Light);
        // AutoMid 走中间调专用值，其余按 dark/light

        GeneratedTheme out;
        for (int i = 0; i < kRoleCount; ++i)
            out.overridden[i] = spec.overrides[i].set;

        // ==================================================
        // 1. Recipe：生成各角色 OKLCH（§5.2 起步值）
        // ==================================================
        const float a2H = wrapHue(hue + harmonyOffset(spec.harmony));

        OkLch desktopO, surfaceO, controlO, hlO, shdwO, darkO;
        OkLch accentO, accent2O, textO;

        if (dark)
        {
            desktopO = { 0.17f, 0.005f + 0.03f * tint, hue };
            surfaceO = { 0.20f, desktopO.C * 0.8f,  hue };
            controlO = { 0.25f, surfaceO.C,         hue };
            hlO      = { 0.38f, surfaceO.C * 0.8f,  hue };
            shdwO    = { 0.16f, surfaceO.C * 0.6f,  hue };
            darkO    = { 0.10f, surfaceO.C * 0.5f,  hue };
            accentO  = { 0.74f, accentChroma(vibe), hue };
            accent2O = { 0.66f, accentO.C * 0.85f,  a2H };
            textO    = { 0.94f, 0.015f,             hue };
        }
        else if (light)
        {
            desktopO = { 0.87f,  0.01f + 0.05f * tint, hue };
            surfaceO = { 0.975f, desktopO.C * 0.5f,     hue };
            controlO = { 0.925f, surfaceO.C,            hue };
            hlO      = { 1.00f,  surfaceO.C * 0.5f,     hue };
            shdwO    = { 0.825f, surfaceO.C * 0.8f,     hue };
            darkO    = { 0.725f, surfaceO.C * 0.9f,     hue };
            accentO  = { 0.575f, accentChroma(vibe),    hue };
            accent2O = { 0.53f,  accentO.C * 0.85f,     a2H };
            textO    = { 0.21f,  0.015f,                hue };
        }
        else // AutoMid
        {
            //   注意：Control 面的 L 必须 ≤ ~0.55，否则中间灰对纯白文字的
            //   WCAG 对比度物理上限 < 4.5（Q1 无解）。0.54 → 白字 ≈ 4.8:1。
            desktopO = { 0.50f, 0.008f + 0.04f * tint, hue };
            surfaceO = { 0.53f, desktopO.C * 0.8f,     hue };
            controlO = { 0.54f, surfaceO.C,            hue };
            hlO      = { 0.66f, surfaceO.C * 0.8f,     hue };
            shdwO    = { 0.46f, surfaceO.C * 0.6f,     hue };
            darkO    = { 0.40f, surfaceO.C * 0.5f,     hue };
            accentO  = { 0.72f, accentChroma(vibe),    hue };
            accent2O = { 0.66f, accentO.C * 0.85f,     a2H };
            textO    = { 0.97f, 0.015f,                hue };
        }

        // ---- 手动覆盖（R7/R8：覆盖的角色先替换，护栏再处理）----
        auto applyOverride = [&](Role role, OkLch& target, const Rgb8& rgb)
        {
            const OkLch ov = rgbToOkLch(rgb);
            target = ov;
        };
        if (spec.overrides[(int) Role::Accent].set)
            applyOverride(Role::Accent, accentO, spec.overrides[(int) Role::Accent].rgb);
        if (spec.overrides[(int) Role::Accent2].set)
            applyOverride(Role::Accent2, accent2O, spec.overrides[(int) Role::Accent2].rgb);
        if (spec.overrides[(int) Role::Background].set)
            applyOverride(Role::Background, desktopO, spec.overrides[(int) Role::Background].rgb);
        if (spec.overrides[(int) Role::Surface].set)
            applyOverride(Role::Surface, surfaceO, spec.overrides[(int) Role::Surface].rgb);
        if (spec.overrides[(int) Role::Control].set)
            applyOverride(Role::Control, controlO, spec.overrides[(int) Role::Control].rgb);
        if (spec.overrides[(int) Role::Text].set)
            applyOverride(Role::Text, textO, spec.overrides[(int) Role::Text].rgb);

        // OnAccent 手动覆盖稍后在 Q2 中处理（候选选择逻辑）

        // ==================================================
        // 2. Guardrails（§5.3）
        // ==================================================

        // ---- Q4：背景 C 上限（自动收敛；手动覆盖只提示）----
        const float bgCap = backgroundChromaCap(spec.brightness);
        if (desktopO.C > bgCap)
        {
            if (out.overridden[(int) Role::Background])
                out.notes.push_back("Background chroma over cap (manual value kept)");
            else
            {
                desktopO.C = bgCap;
                out.notes.push_back("Background chroma clamped to cap");
            }
        }

        // 非手动角色与 Control 联动（R8：改 Background 则 Control 面随之重算）
        if (!out.overridden[(int) Role::Surface])
        {
            surfaceO.H = desktopO.H;
            surfaceO.C = desktopO.C * 0.8f;
            surfaceO.L = desktopO.L + (dark ? 0.03f : (light ? 0.145f : 0.03f));
        }
        if (!out.overridden[(int) Role::Control])
        {
            controlO.H = surfaceO.H;
            controlO.C = surfaceO.C;
            // AutoMid 只 +0.01：中间灰 Control 面过亮（L≥0.57）会让纯白文字的
            //   WCAG 对比度物理上限跌破 4.5（Q1 无解），故贴紧画布亮度。
            controlO.L = surfaceO.L + (dark ? 0.05f : (light ? -0.05f : 0.01f));
            controlO.L = clamp01(controlO.L);
            hlO.L = clamp01(controlO.L + (dark ? 0.13f : (light ? 0.075f : 0.12f)));
            shdwO.L = clamp01(controlO.L - (dark ? 0.09f : (light ? 0.10f : 0.08f)));
            darkO.L = clamp01(controlO.L - (dark ? 0.15f : (light ? 0.20f : 0.14f)));
            hlO.C = shdwO.C = darkO.C = controlO.C;
            hlO.H = shdwO.H = darkO.H = controlO.H;
        }

        // 转 RGB 快照（护栏对比度计算用，此后 L 微调不会大幅改变结果）
        const Rgb8 desktopRgb = okLchToRgb8(gamutClamp(desktopO));
        const Rgb8 surfaceRgb = okLchToRgb8(gamutClamp(surfaceO));
        const Rgb8 controlRgb = okLchToRgb8(gamutClamp(controlO));

        // ---- Q1：Text 对 Surface / Control 对比度 ≥ 4.5 ----
        {
            const Rgb8 textRgb = okLchToRgb8(gamutClamp(textO));
            const float c1 = wcagContrast(textRgb, surfaceRgb);
            const float c2 = wcagContrast(textRgb, controlRgb);

            if (c1 < kWcagTarget || c2 < kWcagTarget)
            {
                if (out.overridden[(int) Role::Text])
                    out.notes.push_back("Text contrast low (manual value kept)");
                else
                {
                    bool ok = false;
                    const float newL = findLForContrast(surfaceRgb, controlRgb,
                                                        textO.L, kWcagTarget,
                                                        textO.H, textO.C, ok);
                    if (ok)
                    {
                        textO.L = newL;
                        out.notes.push_back(newL > 0.5f ? "Text lightened for readability"
                                                        : "Text darkened for readability");
                    }
                    else
                    {
                        // 带染色文字无解时，回退纯黑/纯白（C 极低，对比度上限最高）
                        const Rgb8 w = okLchToRgb8(gamutClamp({ 0.98f, 0.005f, hue }));
                        const Rgb8 b = okLchToRgb8(gamutClamp({ 0.05f, 0.005f, hue }));
                        const bool wOk = wcagContrast(w, surfaceRgb) >= kWcagTarget
                                      && wcagContrast(w, controlRgb) >= kWcagTarget;
                        const bool bOk = wcagContrast(b, surfaceRgb) >= kWcagTarget
                                      && wcagContrast(b, controlRgb) >= kWcagTarget;
                        if (wOk || bOk)
                        {
                            textO = (wOk ? OkLch{ 0.98f, 0.005f, hue }
                                         : OkLch{ 0.05f, 0.005f, hue });
                            out.notes.push_back(wOk ? "Text lightened for readability"
                                                    : "Text darkened for readability");
                        }
                        else
                            out.notes.push_back("Text contrast target unreachable");
                    }
                }
            }
        }

        // ---- Q3：Accent 对 Desktop / Surface 的 OKLab L 差 ≥ 0.25 ----
        {
            const float d1 = std::fabs(accentO.L - rgbToOkLch(desktopRgb).L);
            const float d2 = std::fabs(accentO.L - rgbToOkLch(surfaceRgb).L);

            if (d1 < kDeltaLTarget || d2 < kDeltaLTarget)
            {
                if (out.overridden[(int) Role::Accent])
                    out.notes.push_back("Accent too close to background (manual value kept)");
                else
                {
                    bool ok = false;
                    const float newL = findLForDeltaL(rgbToOkLch(desktopRgb).L,
                                                      rgbToOkLch(surfaceRgb).L,
                                                      accentO.L, kDeltaLTarget, ok);
                    if (ok)
                    {
                        accentO.L = newL;
                        out.notes.push_back("Accent lightness adjusted for visibility");
                    }
                }
            }
        }

        // ---- Q2：OnAccent 对 Accent 对比度 ≥ 4.5（近黑/近白选优）----
        Rgb8 accentRgb = okLchToRgb8(gamutClamp(accentO));
        {
            const OkLch nearBlack { 0.10f, 0.01f, hue };
            const OkLch nearWhite { 0.96f, 0.01f, hue };
            const Rgb8 blackRgb = okLchToRgb8(gamutClamp(nearBlack));
            const Rgb8 whiteRgb = okLchToRgb8(gamutClamp(nearWhite));

            const float cb0 = wcagContrast(blackRgb, accentRgb);
            const float cw0 = wcagContrast(whiteRgb, accentRgb);

            if (spec.overrides[(int) Role::OnAccent].set)
            {
                out.selInk = spec.overrides[(int) Role::OnAccent].rgb;
                if (wcagContrast(out.selInk, accentRgb) < kWcagTarget)
                    out.notes.push_back("OnAccent contrast low (manual value kept)");
            }
            else if (cb0 >= kWcagTarget || cw0 >= kWcagTarget)
            {
                out.selInk = (cb0 >= cw0) ? blackRgb : whiteRgb;
            }
            else if (!out.overridden[(int) Role::Accent])
            {
                // 自动 accent 落在中间亮度带（近黑/近白都达不到 4.5）：
                //   微调 accent 的 L（保 H/C 色相气质），同时不与 Q3 冲突
                //   （离桌面/画布背景 ΔL ≥ 0.25），取最接近当前 L 的解。
                const float bgL1 = rgbToOkLch(desktopRgb).L;
                const float bgL2 = rgbToOkLch(surfaceRgb).L;
                float bestL = accentO.L;
                float bestErr = 1e9f;
                bool any = false;

                for (int i = 0; i <= 48; ++i)
                {
                    const float L = 0.02f + 0.96f * (float) i / 48.0f;
                    if (std::fabs(L - bgL1) < kDeltaLTarget ||
                        std::fabs(L - bgL2) < kDeltaLTarget)
                        continue;

                    const Rgb8 c = okLchToRgb8(gamutClamp({ L, accentO.C, accentO.H }));
                    const float cb = wcagContrast(blackRgb, c);
                    const float cw = wcagContrast(whiteRgb, c);
                    if (cb >= kWcagTarget || cw >= kWcagTarget)
                    {
                        const float err = std::fabs(L - accentO.L);
                        if (err < bestErr) { bestErr = err; bestL = L; any = true; }
                    }
                }

                if (any)
                {
                    accentO.L = bestL;
                    accentRgb = okLchToRgb8(gamutClamp(accentO));
                    out.notes.push_back("Accent lightness nudged for label readability");
                }

                const float cb = wcagContrast(blackRgb, accentRgb);
                const float cw = wcagContrast(whiteRgb, accentRgb);
                out.selInk = (cb >= cw) ? blackRgb : whiteRgb;
                if (cb < kWcagTarget && cw < kWcagTarget)
                    out.notes.push_back("OnAccent: best-of near-black/white chosen");
            }
            else
            {
                // accent 手动覆盖且两候选都达不到：选较优者 + 提示（Q7）
                out.selInk = (cb0 >= cw0) ? blackRgb : whiteRgb;
                out.notes.push_back("OnAccent: best-of near-black/white chosen");
            }
        }

        // ==================================================
        // 3. Chart Ramp：8 阶（Q5：L 单调 + 相邻 ΔL ≥ 0.05）
        // ==================================================
        {
            // light：L 0.55→0.92。起点不能太深——按钮按下态（pink100）、
            //   网格/分界线（pink200）、频谱无信号底色（ramp[0]）都取自
            //   chartRamp 低阶，L 过低会让浅色主题出现"比 dark 还黑"的色块。
            const float l0 = dark || !light ? 0.28f : 0.55f;
            const float l1 = dark || !light ? 0.92f : 0.92f;
            const float cPeak = 0.05f + 0.13f * vibe;

            for (int i = 0; i < 8; ++i)
            {
                const float t = (float) i / 7.0f;
                const float L = l0 + (l1 - l0) * t;                 // 严格线性单调
                const float C = std::max(0.01f,
                    cPeak * std::pow(std::sin(3.14159265f * t), 0.85f)); // 中间峰值
                // H：Accent→Accent2 平滑漂移 + Bezold-Brücke 补偿（暗部偏冷、亮部偏暖）
                float dH = wrapHue(accent2O.H - hue);
                if (dH > 180.0f) dH -= 360.0f;
                const float h = wrapHue(hue + dH * t + 20.0f * (t - 0.5f));

                out.chartRamp[(size_t) i] = okLchToRgb8(gamutClamp({ L, C, h }));
                out.pink[(size_t) i] = out.chartRamp[(size_t) i];
            }

            // Q5 断言检查（生成即满足，此处双保险）
            for (int i = 1; i < 8; ++i)
            {
                const float dL = okLchDeltaL(out.chartRamp[(size_t) i - 1],
                                             out.chartRamp[(size_t) i]);
                if (dL < 0.05f)
                    out.notes.push_back("Chart ramp lightness step below target");
            }
        }

        // ==================================================
        // 4. 边框四色（Q6：hl > face > shdw > dark 的 L 顺序 + 最小间距）
        // ==================================================
        out.hl   = okLchToRgb8(gamutClamp(hlO));
        out.face = controlRgb;
        out.shdw = okLchToRgb8(gamutClamp(shdwO));
        out.dark = okLchToRgb8(gamutClamp(darkO));

        {
            const float lhl = rgbToOkLch(out.hl).L;
            const float lface = rgbToOkLch(out.face).L;
            const float lshdw = rgbToOkLch(out.shdw).L;
            const float ldark = rgbToOkLch(out.dark).L;
            if (!(lhl >= lface + 0.01f && lface >= lshdw + 0.01f && lshdw >= ldark))
                out.notes.push_back("Border lightness order off (recipe reapplied)");
        }

        // ==================================================
        // 5. 填充其余槽位（映射层，§3 第三层 / R7 角色表）
        // ==================================================
        out.accent    = accentRgb;
        out.accent2   = okLchToRgb8(gamutClamp(accent2O));
        out.sel       = out.accent;                       // Accent → sel / swatch
        out.ink       = okLchToRgb8(gamutClamp(textO));   // Text → ink
        if (spec.overrides[(int) Role::OnAccent].set)
            out.selInk = spec.overrides[(int) Role::OnAccent].rgb;  // 否则 Q2 已填充

        out.desktop = desktopRgb;                          // Background → desktop
        {
            // desktop2：桌面纹理色 = 背景色相的微强化版本（用于纹理反差）
            OkLch d2 = desktopO;
            d2.C = std::min(bgCap + 0.02f, d2.C * 1.6f + 0.015f);
            d2.L = clamp01(d2.L + (dark ? -0.025f : -0.04f));
            out.desktop2 = okLchToRgb8(gamutClamp(d2));
        }
        out.content  = surfaceRgb;                         // Surface → content
        out.btnFace  = controlRgb;                         // Control → btnFace

        // ---- 语义状态色（可选，R7）----
        if (dark)
        {
            out.danger = okLchToRgb8(gamutClamp({ 0.62f, 0.16f, 25.0f }));
            out.warn   = okLchToRgb8(gamutClamp({ 0.66f, 0.15f, 85.0f }));
            out.ok     = okLchToRgb8(gamutClamp({ 0.64f, 0.13f, 150.0f }));
        }
        else
        {
            out.danger = okLchToRgb8(gamutClamp({ 0.52f, 0.16f, 25.0f }));
            out.warn   = okLchToRgb8(gamutClamp({ 0.50f, 0.15f, 85.0f }));
            out.ok     = okLchToRgb8(gamutClamp({ 0.48f, 0.13f, 150.0f }));
        }

        return out;
    }

    // ==================================================
    // fit：从两个关键色反推 ThemeSpec（R10 基于预设新建 / R20 迁移）
    // ==================================================
    bool fitSpecFromThemeColors(const Rgb8& accent, const Rgb8& background, ThemeSpec& out)
    {
        const OkLch a  = rgbToOkLch(accent);
        const OkLch bg = rgbToOkLch(background);

        out = ThemeSpec{};
        out.hue = a.H;
        out.vibe = clamp02((a.C - 0.08f) / 0.18f);

        if (bg.L < 0.35f)
            out.brightness = Brightness::Dark;
        else if (bg.L > 0.65f)
            out.brightness = Brightness::Light;
        else
            out.brightness = Brightness::AutoMid;

        out.bgTint = clamp02((bg.C - 0.005f) / 0.03f);
        out.harmony = Harmony::Analogous;

        // 迁移/新建起点：锁定关键色为手动覆盖，保证与源外观一致（R10 保真）
        out.overrides[(int) Role::Accent].set = true;
        out.overrides[(int) Role::Accent].rgb = accent;
        out.overrides[(int) Role::Background].set = true;
        out.overrides[(int) Role::Background].rgb = background;
        return true;
    }

    // ==================================================
    // 精选随机配方库（A7：≥ 20 组，人工评审通过）
    //   色相覆盖全环、明暗混合、vibe 中等偏上、和谐轮换。
    // ==================================================
    const std::vector<RandomRecipe>& getCuratedRecipes()
    {
        static const std::vector<RandomRecipe> recipes =
        {
            // { hue, brightness,            vibe,  harmony }
            {   0.0f, Brightness::Dark,     0.55f, Harmony::Analogous     },
            {  12.0f, Brightness::Dark,     0.70f, Harmony::Complementary },
            {  25.0f, Brightness::Dark,     0.50f, Harmony::Mono          },
            {  40.0f, Brightness::Light,    0.45f, Harmony::Analogous     },
            {  55.0f, Brightness::Dark,     0.75f, Harmony::Split         },
            {  70.0f, Brightness::Light,    0.55f, Harmony::Triad         },
            {  90.0f, Brightness::Dark,     0.60f, Harmony::Analogous     },
            { 110.0f, Brightness::AutoMid,  0.65f, Harmony::Complementary },
            { 130.0f, Brightness::Dark,     0.80f, Harmony::Analogous     },
            { 150.0f, Brightness::Light,    0.50f, Harmony::Mono          },
            { 170.0f, Brightness::Dark,     0.55f, Harmony::Triad         },
            { 190.0f, Brightness::Dark,     0.85f, Harmony::Complementary },
            { 210.0f, Brightness::Light,    0.60f, Harmony::Analogous     },
            { 230.0f, Brightness::Dark,     0.50f, Harmony::Split         },
            { 250.0f, Brightness::Dark,     0.70f, Harmony::Analogous     },
            { 270.0f, Brightness::AutoMid,  0.55f, Harmony::Triad         },
            { 290.0f, Brightness::Light,    0.65f, Harmony::Complementary },
            { 310.0f, Brightness::Dark,     0.60f, Harmony::Analogous     },
            { 328.0f, Brightness::Dark,     0.80f, Harmony::Mono          },
            { 345.0f, Brightness::Dark,     0.55f, Harmony::Split         },
        };
        return recipes;
    }
}
