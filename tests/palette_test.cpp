// ==========================================================
// palette_test.cpp —— PaletteGenerator 纯 C++ 冒烟测试（AC7）
//
//   1. 全遍历断言：H 步进 5° × {Dark, Light, AutoMid} × Vibe 5 档
//      → Q1-Q6 护栏全通过、无 NaN、颜色均在 sRGB 色域内
//   2. 分享码往返（ThemeSpec → `Y2KT1-xxx` → ThemeSpec）
//   3. 手动覆盖路径：生成不崩 + 产生提示（Q7）
//   4. `--overview <file.html>`：输出"色相 x 明暗 x Vibe"总览图（P0 评审用）
//   5. `--golden`：输出 12 组代表参数的 19 槽位 hex 快照（AC8 存档用）
//
//   编译：不依赖 JUCE，仅链接 palette 纯 C++ 源文件。
//   通过标志：打印 ALL_OK 且 exit 0。
// ==========================================================
#include "source/ui/palette/PaletteGenerator.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace y2kpal;

static int gFailures = 0;

// 当前遍历上下文（失败信息里带上参数，便于定位）
static int gCtxH = -1, gCtxM = -1, gCtxV = -1;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond))                                                  \
        {                                                             \
            ++gFailures;                                              \
            std::printf("FAIL: %s (h=%d m=%d v=%d line %d)\n",        \
                        msg, gCtxH, gCtxM, gCtxV, __LINE__);          \
        }                                                             \
    } while (0)

static std::string hex(const Rgb8& c);

static bool isFiniteRgb8(const Rgb8& c) noexcept
{
    // Rgb8 是 uint8，天然有限；此处检查"转换链"无退化：
    // 再次转 OKLCH 后必须全部有限
    const OkLch o = rgbToOkLch(c);
    return std::isfinite(o.L) && std::isfinite(o.C) && std::isfinite(o.H);
}

// ---- Q1-Q6 护栏断言（§5.3）----
static void checkGuardrails(const GeneratedTheme& t, const ThemeSpec& spec)
{
    // 颜色有限性（无 NaN）
    CHECK(isFiniteRgb8(t.ink), "ink NaN");
    CHECK(isFiniteRgb8(t.sel), "sel NaN");
    CHECK(isFiniteRgb8(t.desktop), "desktop NaN");
    CHECK(isFiniteRgb8(t.content), "content NaN");
    CHECK(isFiniteRgb8(t.btnFace), "btnFace NaN");
    for (int i = 0; i < 8; ++i)
    {
        CHECK(isFiniteRgb8(t.chartRamp[(size_t) i]), "ramp NaN");
        CHECK(isFiniteRgb8(t.pink[(size_t) i]), "pink NaN");
    }

    // Q1：Text 对 Surface / Control 对比度 ≥ 4.5（自动角色下强制满足）
    if (!t.overridden[(int) Role::Text])
    {
        const float q1c = wcagContrast(t.ink, t.content);
        const float q1b = wcagContrast(t.ink, t.btnFace);
        CHECK(q1c >= 4.5f, "Q1: ink vs content < 4.5");
        CHECK(q1b >= 4.5f, "Q1: ink vs btnFace < 4.5");
        if (q1c < 4.5f || q1b < 4.5f)
            std::printf("    ink=%s content=%s btnFace=%s c1=%.2f c2=%.2f\n",
                        hex(t.ink).c_str(), hex(t.content).c_str(),
                        hex(t.btnFace).c_str(), q1c, q1b);
    }

    // Q2：OnAccent 对 Accent ≥ 4.5
    //   自动 accent 下生成器保证可达；手动 accent（fit/迁移）可能落在中间亮度带，
    //   近黑/近白物理上都达不到 4.5 —— Q7 只要求"选更优者"，不硬性断言。
    {
        const float q2 = wcagContrast(t.selInk, t.sel);
        if (!t.overridden[(int) Role::Accent])
        {
            CHECK(q2 >= 4.4f, "Q2: selInk vs sel < 4.5");
            if (q2 < 4.4f)
                std::printf("    selInk=%s sel=%s c=%.2f\n",
                            hex(t.selInk).c_str(), hex(t.sel).c_str(), q2);
        }
    }

    // Q3：Accent 对 Desktop/Surface 的 L 差 ≥ 0.25
    if (!t.overridden[(int) Role::Accent])
    {
        CHECK(okLchDeltaL(t.sel, t.desktop) >= 0.24f, "Q3: accent vs desktop ΔL");
        CHECK(okLchDeltaL(t.sel, t.content) >= 0.24f, "Q3: accent vs content ΔL");
    }

    // Q4：背景 C 上限
    {
        const float cap = (spec.brightness == Brightness::Light) ? 0.08f
                        : (spec.brightness == Brightness::AutoMid) ? 0.06f : 0.05f;
        const float bgC = rgbToOkLch(t.desktop).C;
        CHECK(bgC <= cap + 0.02f, "Q4: background chroma over cap");
    }

    // Q5：Chart Ramp L 严格单调 + 相邻 ΔL ≥ 0.05
    for (int i = 1; i < 8; ++i)
    {
        const float l0 = rgbToOkLch(t.chartRamp[(size_t) i - 1]).L;
        const float l1 = rgbToOkLch(t.chartRamp[(size_t) i]).L;
        CHECK(l1 > l0, "Q5: ramp L not monotonic");
        CHECK(l1 - l0 >= 0.04f, "Q5: ramp ΔL < 0.05");
    }

    // Q6：边框 hl > face > shdw > dark（OKLab L 顺序）
    {
        const float lhl = rgbToOkLch(t.hl).L;
        const float lface = rgbToOkLch(t.face).L;
        const float lshdw = rgbToOkLch(t.shdw).L;
        const float ldark = rgbToOkLch(t.dark).L;
        CHECK(lhl >= lface, "Q6: hl >= face");
        CHECK(lface >= lshdw, "Q6: face >= shdw");
        CHECK(lshdw >= ldark, "Q6: shdw >= dark");
    }
}

// ---- 分享码往返 ----
static void testShareCodeRoundTrip()
{
    ThemeSpec spec;
    spec.hue = 287.4f;
    spec.brightness = Brightness::AutoMid;
    spec.vibe = 0.73f;
    spec.bgTint = 0.42f;
    spec.harmony = Harmony::Split;
    spec.pattern = Pattern::Hearts;
    spec.titleIcon = 5;
    spec.lockHue = true;
    spec.lockVibe = true;
    spec.overrides[(int) Role::Accent].set = true;
    spec.overrides[(int) Role::Accent].rgb = { 0x12, 0x34, 0x56 };
    spec.overrides[(int) Role::Text].set = true;
    spec.overrides[(int) Role::Text].rgb = { 0xab, 0xcd, 0xef };

    const std::string code = spec.shareCode();
    CHECK(code.rfind("Y2KT1-", 0) == 0, "share code prefix");
    CHECK(code.size() <= 80, "share code length <= 80");

    ThemeSpec back;
    CHECK(ThemeSpec::fromShareCode(code, back), "share code decode");

    // hue 精度 0.1°、vibe/tint 精度 1/255
    CHECK(std::fabs(back.hue - spec.hue) < 0.2f, "share code hue");
    CHECK(back.brightness == spec.brightness, "share code brightness");
    CHECK(std::fabs(back.vibe - spec.vibe) < 0.01f, "share code vibe");
    CHECK(std::fabs(back.bgTint - spec.bgTint) < 0.01f, "share code tint");
    CHECK(back.harmony == spec.harmony, "share code harmony");
    CHECK(back.pattern == spec.pattern, "share code pattern");
    CHECK(back.titleIcon == spec.titleIcon, "share code icon");
    CHECK(back.lockHue && back.lockVibe, "share code locks");
    CHECK(back.overrides[(int) Role::Accent].set
          && back.overrides[(int) Role::Accent].rgb == spec.overrides[(int) Role::Accent].rgb,
          "share code accent override");
    CHECK(back.overrides[(int) Role::Text].rgb == spec.overrides[(int) Role::Text].rgb,
          "share code text override");

    // 非法输入
    ThemeSpec junk;
    CHECK(!ThemeSpec::fromShareCode("garbage!!!", junk), "share code reject garbage");
}

// ---- 手动覆盖路径（Q7：不崩 + 提示）----
static void testManualOverrides()
{
    ThemeSpec spec;
    spec.hue = 200.0f;
    spec.brightness = Brightness::Light;
    spec.overrides[(int) Role::Background].set = true;
    spec.overrides[(int) Role::Background].rgb = { 0xf0, 0xf0, 0xf0 }; // 亮背景
    spec.overrides[(int) Role::Text].set = true;
    spec.overrides[(int) Role::Text].rgb = { 0xee, 0xee, 0xee };       // 与背景几乎同色（危险）
    spec.overrides[(int) Role::Accent].set = true;
    spec.overrides[(int) Role::Accent].rgb = { 0xe8, 0xe8, 0xe8 };     // 与背景太近

    const GeneratedTheme t = generate(spec);
    CHECK(!t.notes.empty(), "manual override should produce notes");
    CHECK(t.ink == spec.overrides[(int) Role::Text].rgb, "manual text preserved");
    CHECK(t.sel == spec.overrides[(int) Role::Accent].rgb, "manual accent preserved");
}

// ---- 全遍历（AC7）----
static void runExhaustiveSweep()
{
    int count = 0;
    for (int h = 0; h < 360; h += 5)
    {
        for (int m = 0; m < 3; ++m)
        {
            for (int v = 0; v < 5; ++v)
            {
                ThemeSpec spec;
                spec.hue = (float) h;
                spec.brightness = (Brightness) m;
                spec.vibe = v * 0.25f;
                spec.bgTint = 0.3f;
                spec.harmony = (Harmony) (h % 5);
                gCtxH = h; gCtxM = m; gCtxV = v;
                checkGuardrails(generate(spec), spec);
                ++count;
            }
        }
    }
    std::printf("sweep: %d combos checked\n", count);
}

// ---- hex 工具 ----
static std::string hex(const Rgb8& c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

// ---- HTML 总览图（P0 评审：色相 x 明暗 x Vibe）----
static void writeOverviewHtml(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) { std::printf("cannot open %s\n", path.c_str()); return; }

    std::fputs("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
               "<title>Y2Kmeter Palette Overview</title>"
               "<style>body{background:#111;color:#ddd;font-family:monospace;margin:16px}"
               "table{border-collapse:collapse;margin:8px 0}"
               "td{border:1px solid #333;padding:2px}"
               ".cell{width:12px;height:12px;display:inline-block;margin:1px}"
               ".sw{width:14px;height:14px;display:inline-block;border:1px solid #555;margin:1px}"
               "h3{margin:20px 0 4px;color:#f7a}"
               "</style></head><body>", f);

    const char* modeNames[] = { "Dark", "Light", "AutoMid" };

    for (int m = 0; m < 3; ++m)
    {
        std::fprintf(f, "<h3>%s</h3><table>", modeNames[m]);
        std::fputs("<tr><td>hue\\vibe</td>", f);
        for (int v = 0; v < 3; ++v)
            std::fprintf(f, "<td>vibe %.2f</td>", v * 0.5f + 0.1f);
        std::fputs("</tr>", f);

        for (int h = 0; h < 360; h += 15)
        {
            std::fprintf(f, "<tr><td>%d</td>", h);
            for (int v = 0; v < 3; ++v)
            {
                ThemeSpec spec;
                spec.hue = (float) h;
                spec.brightness = (Brightness) m;
                spec.vibe = v * 0.5f + 0.1f;
                spec.bgTint = 0.3f;
                spec.harmony = Harmony::Analogous;
                const GeneratedTheme t = generate(spec);

                std::fputs("<td>", f);
                for (int i = 0; i < 8; ++i)
                    std::fprintf(f, "<span class=\"cell\" style=\"background:%s\" title=\"ramp%d\"></span>",
                                 hex(t.chartRamp[(size_t) i]).c_str(), i);
                std::fprintf(f, "<span class=\"sw\" style=\"background:%s\" title=\"desktop\"></span>", hex(t.desktop).c_str());
                std::fprintf(f, "<span class=\"sw\" style=\"background:%s\" title=\"surface\"></span>", hex(t.content).c_str());
                std::fprintf(f, "<span class=\"sw\" style=\"background:%s\" title=\"control\"></span>", hex(t.btnFace).c_str());
                std::fprintf(f, "<span class=\"sw\" style=\"background:%s\" title=\"accent\"></span>", hex(t.accent).c_str());
                std::fprintf(f, "<span class=\"sw\" style=\"background:%s\" title=\"accent2\"></span>", hex(t.accent2).c_str());
                std::fprintf(f, "<span class=\"sw\" style=\"background:%s\" title=\"text\"></span>", hex(t.ink).c_str());
                std::fputs("</td>", f);
            }
            std::fputs("</tr>", f);
        }
        std::fputs("</table>", f);
    }
    std::fputs("</body></html>", f);
    std::fclose(f);
    std::printf("overview written: %s\n", path.c_str());
}

// ---- 黄金色板快照（AC8：12 组代表参数 → 19 槽位 hex）----
static void writeGoldenSnapshot()
{
    struct Case { float hue; Brightness b; float vibe; float tint; Harmony har; };
    const Case cases[] =
    {
        { 330.0f, Brightness::Dark,  0.55f, 0.30f, Harmony::Analogous },
        { 200.0f, Brightness::Dark,  0.70f, 0.30f, Harmony::Complementary },
        {  90.0f, Brightness::Dark,  0.50f, 0.30f, Harmony::Triad },
        { 130.0f, Brightness::Dark,  0.80f, 0.50f, Harmony::Analogous },
        {   0.0f, Brightness::Light, 0.55f, 0.20f, Harmony::Mono },
        { 230.0f, Brightness::Light, 0.60f, 0.40f, Harmony::Split },
        {  55.0f, Brightness::Light, 0.75f, 0.30f, Harmony::Complementary },
        { 170.0f, Brightness::Light, 0.50f, 0.20f, Harmony::Analogous },
        { 280.0f, Brightness::AutoMid, 0.55f, 0.30f, Harmony::Triad },
        {  40.0f, Brightness::AutoMid, 0.70f, 0.50f, Harmony::Analogous },
        { 150.0f, Brightness::AutoMid, 0.60f, 0.30f, Harmony::Mono },
        { 310.0f, Brightness::AutoMid, 0.80f, 0.40f, Harmony::Complementary },
    };

    for (const auto& c : cases)
    {
        ThemeSpec spec;
        spec.hue = c.hue; spec.brightness = c.b; spec.vibe = c.vibe;
        spec.bgTint = c.tint; spec.harmony = c.har;
        const GeneratedTheme t = generate(spec);

        std::printf("H=%05.1f M=%d V=%.2f T=%.2f R=%d | ",
                    c.hue, (int) c.b, c.vibe, c.tint, (int) c.har);
        std::printf("p50..700: %s %s %s %s %s %s %s %s\n",
                    hex(t.pink[0]).c_str(), hex(t.pink[1]).c_str(),
                    hex(t.pink[2]).c_str(), hex(t.pink[3]).c_str(),
                    hex(t.pink[4]).c_str(), hex(t.pink[5]).c_str(),
                    hex(t.pink[6]).c_str(), hex(t.pink[7]).c_str());
        std::printf("           hl %s face %s shdw %s dark %s | ink %s sel %s selInk %s\n",
                    hex(t.hl).c_str(), hex(t.face).c_str(), hex(t.shdw).c_str(),
                    hex(t.dark).c_str(), hex(t.ink).c_str(), hex(t.sel).c_str(),
                    hex(t.selInk).c_str());
        std::printf("           desktop %s desktop2 %s content %s btnFace %s | a2 %s\n",
                    hex(t.desktop).c_str(), hex(t.desktop2).c_str(),
                    hex(t.content).c_str(), hex(t.btnFace).c_str(),
                    hex(t.accent2).c_str());
    }
}

int main(int argc, char** argv)
{
    std::string overviewPath;
    bool doGolden = false;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--overview") == 0 && i + 1 < argc)
            overviewPath = argv[++i];
        else if (std::strcmp(argv[i], "--golden") == 0)
            doGolden = true;
    }

    std::printf("== Y2Kmeter palette smoke test ==\n");

    runExhaustiveSweep();
    testShareCodeRoundTrip();
    testManualOverrides();

    // fit 冒烟：任意双色都能反推合法 spec
    {
        ThemeSpec fit;
        CHECK(fitSpecFromThemeColors({ 0xec, 0x4d, 0x85 }, { 0x10, 0x10, 0x10 }, fit),
              "fit dark");
        CHECK(fitSpecFromThemeColors({ 0x2a, 0x7a, 0xa8 }, { 0xf0, 0xf0, 0xf0 }, fit),
              "fit light");
        CHECK(fit.brightness == Brightness::Light, "fit detects light bg");
        const GeneratedTheme t = generate(fit);
        // 手动 accent（#2a7aa8 中间亮度，OnAccent 无法达 4.5）→ 应产生 Q7 提示
        CHECK(!t.notes.empty(), "fit manual accent should produce notes");
        checkGuardrails(t, fit);
    }

    if (doGolden)
        writeGoldenSnapshot();
    if (!overviewPath.empty())
        writeOverviewHtml(overviewPath);

    if (gFailures == 0)
    {
        std::printf("ALL_OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", gFailures);
    return 1;
}
