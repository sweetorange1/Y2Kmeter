#include "source/ui/PinkXPStyle.h"
#include "BinaryData.h"
#include "source/i18n/Localisation.h"
#include "source/perf/PerformanceCounterSystem.h"
#include "source/ui/palette/PaletteGenerator.h"
#include "source/Y2KLogging.h"
#include "source/diag/Y2KDiagLog.h"

// 全屏菜单宿主判定需要识别"哪一层是 OpenGL 渲染表面宿主"
//   （Editor 在构造末尾 openGLContext.attachTo(*this)）。
#include <juce_opengl/juce_opengl.h>

#include <unordered_map>
#include <map>
#include <tuple>
#include <atomic>
#include <cmath>

// ==========================================================
// PinkXPStyle.cpp —— 像素风配色/绘制/LookAndFeel 的实现
// ==========================================================

// 前向声明：syncPinkXPColours 的完整定义在文件下方的匿名 namespace 里，
//   但 applyTheme 在它之前就需要调用它，这里先给一个局部声明供链接。
namespace
{
    void syncPinkXPColours (juce::LookAndFeel& lf);
}

// ==========================================================
// 字体配置：英文默认 Silkscreen-Regular.ttf；其它界面语言按语言包 @font 切换
// ==========================================================
namespace PinkXP
{
    static juce::Typeface::Ptr gTypeface;

    // 当前语言字号换算（initCustomTypeface 时从 y2k::i18n 读取）
    static float gFontScale     = 1.0f;
    static float gMinFontHeight = 0.0f;
    static bool  gFontMetricsLoaded = false;
    static std::atomic<juce::uint32> gFontGeneration { 1 };

    // 英文 UI 字体（Silkscreen）的 Typeface —— 装饰符号的渲染基底
    //   （SymbolGlyphs 用；releaseTypefaceCache 里与语言字体缓存一起释放）
    static juce::Typeface::Ptr gSymbolTypeface;

    // loadActiveTypeface 缓存：fontId → Typeface（同一字体只解析一次；
    //   中文像素字体 12 MB，重复 createSystemTypefaceFor 代价很高）
    static std::vector<std::pair<juce::String, juce::Typeface::Ptr>> gTypefaceCache;

    static void refreshFontMetricsFromLanguage()
    {
        const auto& lang = y2k::i18n::getCurrentLanguage();
        gFontScale     = lang.fontScale > 0.0f ? lang.fontScale : 1.0f;
        gMinFontHeight = lang.minFontHeight;
        gFontMetricsLoaded = true;
    }

    void initCustomTypeface(juce::Typeface::Ptr ptr)
    {
        gTypeface = ptr;
        refreshFontMetricsFromLanguage();
        ++gFontGeneration;

        // 全平台字体一致性注入：
        //   让所有走 JUCE 默认字体路径的控件（未显式 setFont 的 Label/Menu 等）
        //   也统一使用自定义 Typeface。macOS 下若不注入会回退系统字体；Windows
        //   下同样收益（彻底避免 JUCE 8 新字体栈在某些控件上回退到系统字体）。
        juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypeface (ptr);

        // 主界面实际使用的是 PinkXPLookAndFeel，需同步注入同一 Typeface；
        // 否则会出现"弹窗字体已变更，但主界面仍回退系统字体"的不一致。
        getPinkXPLookAndFeel().setDefaultSansSerifTypeface (ptr);
    }

    juce::Typeface::Ptr loadActiveTypeface()
    {
        const void*  data = nullptr;
        size_t       size = 0;
        juce::String fontId;

        if (! y2k::i18n::getCurrentFontData (data, size, fontId))
        {
            data   = BinaryData::SilkscreenRegular_ttf;
            size   = (size_t) BinaryData::SilkscreenRegular_ttfSize;
            fontId = "builtin:Silkscreen";
        }

        for (auto& kv : gTypefaceCache)
            if (kv.first == fontId)
                return kv.second;

        auto tf = juce::Typeface::createSystemTypefaceFor (data, size);
        if (tf != nullptr)
            gTypefaceCache.emplace_back (fontId, tf);
        return tf;
    }

    // 定义见下方（Font 实例缓存 / 静态文本位图缓存）
    static void releaseFontInstanceCache();
    static void releaseStaticTextCache();

    void releaseTypefaceCache()
    {
        // 先释放持有 Font / 位图的缓存，再释放 Typeface：缓存若拖到进程 exit 的
        //   静态析构阶段才释放，CoreTextTypeface 析构时 JUCE 内部互斥量已销毁，
        //   会以 "mutex lock failed" 终止进程。
        releaseFontInstanceCache();
        releaseStaticTextCache();
        gTypefaceCache.clear();
        gSymbolTypeface = nullptr;
    }

    juce::uint32 getFontGeneration() noexcept
    {
        return gFontGeneration.load();
    }

    // ==========================================================
    // UI 密度缩放（低分辨率适配）
    // ==========================================================
    static float gUiScale = 1.0f;

    // stick 驻留态（Windows AppBar）：供 LnF 调整 PopupMenu / ComboBox 的弹出锚点
    static bool gStickDockActive = false;
    static bool gStickDockBottom = false;

    void setStickDockState (bool active, bool bottom) noexcept
    {
        gStickDockActive = active;
        gStickDockBottom = active && bottom;
    }

    bool isStickDockActive() noexcept { return gStickDockActive; }
    bool isStickDockBottom() noexcept { return gStickDockBottom; }

    // ==========================================================
    // 全屏菜单宿主（Windows 专项）—— 详见头文件说明
    //
    //   判定完全基于"打开菜单这一刻的真实几何/组件结构"，不维护任何全局状态，
    //   因此不存在"进了全屏忘了复位"的残留风险：
    //     · 窗口不铺满整块显示器 → 返回 nullptr → 菜单仍是独立顶层窗口；
    //     · 退出伪全屏（setBounds 回原尺寸）后，几何条件自动失效，行为立刻复原。
    // ==========================================================
    juce::Component* getInWindowMenuHost (juce::Component* targetComponent)
    {
       #if JUCE_WINDOWS
        if (targetComponent == nullptr)
            return nullptr;

        // VST3/AU 下顶层窗口归宿主所有：往宿主窗口里塞子组件会污染宿主界面
        if (! juce::JUCEApplicationBase::isStandaloneApp())
            return nullptr;

        // 从起点一路向上，取【最外层】的 GL 渲染表面宿主。
        // 命中项就是 Editor（openGLContext.attachTo(*this) 的挂载点）；
        // 取最外层是为了跳过浮动 Milkdrop 等嵌套 GL 上下文。
        auto findGlHost = [] (juce::Component* start) -> juce::Component*
        {
            juce::Component* host = nullptr;

            for (auto* c = start; c != nullptr; c = c->getParentComponent())
                if (juce::OpenGLContext::getContextAttachedTo (*c) != nullptr)
                    host = c;

            return host;
        };

        auto* host = findGlHost (targetComponent);

        // 兜底：调用方若把 targetComponent 传成了顶层窗口本身
        //（历史实现如此），就从窗口内容组件重新向下找一次。
        if (host == nullptr)
            if (auto* rw = dynamic_cast<juce::ResizableWindow*> (targetComponent->getTopLevelComponent()))
                host = findGlHost (rw->getContentComponent());

        if (host == nullptr)
            return nullptr;

        auto* top = host->getTopLevelComponent();
        if (top == nullptr || top == host)
            return nullptr;

        // 关键闸门：只有当主窗口铺满【整块显示器】时才启用。
        //   · 任务栏可见时窗口 = userArea，比 totalArea 少一条 → 条件不成立，
        //     菜单照旧走独立顶层窗口（可自由伸出窗口外），行为零变化；
        //   · 任务栏隐藏后 userArea == totalArea，伪全屏窗口正好铺满整屏 →
        //     这正是 Windows 把主窗口当全屏窗口、独立弹窗被压住的前提。
        const auto windowBounds = top->getScreenBounds();
        if (windowBounds.isEmpty())
            return nullptr;

        for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
            if (d.totalArea.getIntersection (windowBounds) == d.totalArea)
                return host;

        return nullptr;
       #else
        juce::ignoreUnused (targetComponent);
        return nullptr;
       #endif
    }

    float uiScale() noexcept { return gUiScale; }

    void setUiScale (float s) noexcept
    {
        gUiScale = juce::jlimit (0.75f, 1.3f, s);
    }

    int ui (int px) noexcept
    {
        return juce::jmax (1, juce::roundToInt ((float) px * uiScale()));
    }

    float uiScaleForDisplay (const juce::Rectangle<int>& displayArea) noexcept
    {
        const int w = displayArea.getWidth();
        const int h = displayArea.getHeight();
        const int minDim = juce::jmin (w, h);

        // 按"显示屏逻辑分辨率"分级（传 totalArea —— 不要传 userArea：
        //   后者会被任务栏 / 自己的 AppBar 驻留压缩，stick 条拉高时会误判为矮屏）。
        // JUCE 的显示器尺寸已是 DPI 缩放后的逻辑像素：
        //   · ≤720 / ≤768 矮屏：1280×720、1024×768 等 → 0.75 紧凑
        //   · ≤800：1366×768、1280×800 等 → 0.8
        //   · ≤900：1440×900、1600×900 等 → 0.9
        //   · 更高分辨率 → 1.0 原始
        if (h <= 720 || minDim <= 768)  return 0.75f;
        if (h <= 800)                   return 0.80f;
        if (h <= 900)                   return 0.90f;
        return 1.0f;
    }

    // ==========================================================
    // 系统回退族：自定义字体缺字时的兜底
    //   · 界面字体（Silkscreen / ChillBitmap）→ CJK 回退族。
    //     mac 上必须显式避开 PingFang SC —— JUCE 8.0.12 + CoreText 下
    //     PingFang SC 的 CJK 字形描不出来（实测整段空白），而系统默认回退
    //     恰好会挑它。
    //   · 符号字体（Silkscreen）→ 符号族 + CJK 回退族：即使某处漏拆 run，
    //     CJK 也会落到能渲染的字体，而不是空白。
    //   列表里不存在的字族 JUCE 会自动跳过，因此可以安全地多写几档。
    // ==========================================================
    static const std::vector<juce::String>& uiFallbackFamilies()
    {
        // 进程内只构造一次（getFont 在绘制热路径上；列表以 const 引用返回，
        //   withFallbacks 内部会做一次轻量拷贝）
        static const std::vector<juce::String> families
        {
           #if JUCE_MAC
            "Hiragino Sans GB", "Arial Unicode MS", "Heiti SC"
           #elif JUCE_WINDOWS
            "Microsoft YaHei UI", "Microsoft YaHei", "SimSun"
           #else
            "Noto Sans CJK SC", "DejaVu Sans"
           #endif
        };
        return families;
    }

    static const std::vector<juce::String>& symbolFallbackFamilies()
    {
        static const std::vector<juce::String> families
        {
           #if JUCE_MAC
            // Apple Symbols 覆盖 ♥★◆●▲▸↑↓… 等大部分装饰符号；
            // 它缺的 ✓✎✦✿❀❤ 由 Arial Unicode MS 补；Hiragino Sans GB 兜底 CJK
            "Apple Symbols", "Arial Unicode MS", "Hiragino Sans GB", "Lucida Grande"
           #elif JUCE_WINDOWS
            "Segoe UI Symbol", "Segoe UI Emoji", "Microsoft YaHei UI", "Arial Unicode MS"
           #else
            "Noto Sans Symbols2", "DejaVu Sans"
           #endif
        };
        return families;
    }

    // 内部工厂：按原始 height 生成字体
    //
    // 关键说明（JUCE 8 + macOS 字体回退问题修复）：
    //   旧的 juce::Font(typeface) + setBold/setItalic 路径，在 JUCE 8 的
    //   新字体栈下会按"family name + bold/italic"再去系统里查字型变体。
    //   Silkscreen 根本没有 bold/italic 变体，于是 macOS 下就回退到系统字体，
    //   只有 plain 样式（如抬头 iisaacbeats.cn 那一行）仍能命中自定义 Typeface，
    //   其余 bold/italic 文本全部变成系统字体——这正是本次字体异常的根因。
    //
    //   修复：改用 juce::FontOptions(gTypeface).withHeight(height)，
    //   直接锁定 Typeface 实例，不走 family name 再查询的回退路径；
    //   并忽略 styleFlags（Silkscreen 只有一种字形，bold/italic 仅靠
    //   调用方做伪粗/伪斜即可，不应触发 Typeface 替换）。
    //
    // 踩坑记录（Debug 崩溃）：
    //   最初写法 juce::FontOptions(height).withTypeface(gTypeface) 在 Debug 下会
    //   触发 juce_FontOptions.h:126 附近的 jassert：
    //       jassert (x == nullptr || style.isEmpty());
    //   原因是 FontOptions(float) 会级联调用 (String&, String&, float) 版本，
    //   把 style 字段先塞成 "Regular"（Font::plain 转出来的字体样式名），此时
    //   再 withTypeface(非空指针) 就会命中 assert。Release 版 jassert 是空操作，
    //   看似正常；Debug 下 int3 直接卡死在启动阶段（软件窗口都没弹出来）。
    //   正确写法：使用 FontOptions(Typeface::Ptr) 构造函数，让 name/style
    //   从一开始就和 typeface 保持一致（withTypeface 内部同样是这样做的），
    //   然后 withHeight() 覆盖字号即可。
    // ----------------------------------------------------------
    // Font 实例缓存（mac GL 性能）
    //   JUCE 的 GlyphCache / drawText 排版缓存都以 Font 为 std::map 键，每个 glyph
    //   查找都要比较多次 Font。每次 getFont 都新建 Font 时，比较需逐字段进行
    //   （含 fallback 字体名 vector 逐项字符串比较）；复用同一实例后，命中时
    //   Font::compare 走"同一共享实例"快速路径（见 JUCE juce_Font.cpp 补丁）。
    //   Font 写时复制（setX 会先 dupe），共享实例对调用方无副作用。
    //   失效：字体代次 / 语言字号换算 / 最小字号 / UI 密度任一变化即整体清空。
    // ----------------------------------------------------------
    namespace
    {
        struct FontCacheState
        {
            juce::SpinLock lock;
            std::map<std::tuple<int, float, int>, juce::Font> fonts;
            juce::uint32 generation = 0;
            float fontScale = 0.0f, minFontHeight = -1.0f, uiScaleValue = 0.0f;
        };

        FontCacheState& fontCacheState()
        {
            // 有意不析构（进程退出的静态析构阶段 JUCE 字体系统已拆除）；
            //   正常释放走 releaseTypefaceCache → releaseFontInstanceCache
            static auto& s = *new FontCacheState();
            return s;
        }

        template <typename Make>
        juce::Font cachedFont (int kind, float height, int styleFlags, Make&& make)
        {
            auto& s = fontCacheState();
            const juce::SpinLock::ScopedLockType sl (s.lock);

            const auto gen = gFontGeneration.load();
            if (s.generation != gen || s.fontScale != gFontScale
                || s.minFontHeight != gMinFontHeight || s.uiScaleValue != gUiScale)
            {
                s.fonts.clear();
                s.generation    = gen;
                s.fontScale     = gFontScale;
                s.minFontHeight = gMinFontHeight;
                s.uiScaleValue  = gUiScale;
            }

            const auto key = std::make_tuple (kind, height, styleFlags);
            if (const auto it = s.fonts.find (key); it != s.fonts.end())
                return it->second;

            if (s.fonts.size() > 256)
                s.fonts.clear();

            return s.fonts.emplace (key, make()).first->second;
        }
    }

    static void releaseFontInstanceCache()
    {
        auto& s = fontCacheState();
        const juce::SpinLock::ScopedLockType sl (s.lock);
        s.fonts.clear();
    }

    static juce::Font makeFontRawUncached(float height, int styleFlags)
    {
        // 界面语言字号换算：不同字体的 em 框 / 字面比例差异很大
        //   （Silkscreen 大写仅占字高 ~55%，中文像素字体方块字占满整个字高），
        //   由语言包 @fontScale 统一校正，保证同一 height 下视觉大小接近。
        height *= gFontScale;

        if (gTypeface != nullptr)
        {
            // 单一自定义 Typeface 兜底所有样式，避免 bold/italic 场景回退系统字体。
            // 显式回退族：自定义字体缺字（尤其 CJK）时不去问系统默认（mac 会挑到
            // 描不出来的 PingFang SC）。
            juce::ignoreUnused (styleFlags);
            return juce::Font (juce::FontOptions (gTypeface)
                                   .withFallbacks (uiFallbackFamilies())
                                   .withHeight (height));
        }
        return juce::Font (juce::FontOptions (height, styleFlags)
                               .withFallbacks (uiFallbackFamilies()));
    }

    static juce::Font makeFontRaw(float height, int styleFlags)
    {
        if (! gFontMetricsLoaded)
            refreshFontMetricsFromLanguage();

        return cachedFont (0, height, styleFlags,
                           [&] { return makeFontRawUncached (height, styleFlags); });
    }

    // 通用字体：高度 >= 8 时做 1.5x 放大（UI 主字号），极小字 (<8) 保持原样
    // 坐标轴/刻度数字请显式使用 getAxisFont 以不放大
    juce::Font getFont(float height, int styleFlags)
    {
        if (! gFontMetricsLoaded)
            refreshFontMetricsFromLanguage();

        return cachedFont (1, height, styleFlags, [&]
        {
            const float scaled = (height >= 8.0f) ? height * 1.5f : height;
            auto f = makeFontRawUncached(scaled * uiScale(), styleFlags);

            // 语言包最小字号（CJK 像素字过小会糊成一团；坐标轴字体不受此限制）
            if (gMinFontHeight > 0.0f && f.getHeight() < gMinFontHeight)
                f = f.withHeight (gMinFontHeight);
            return f;
        });
    }

    // ==========================================================
    // SymbolGlyphs —— 装饰符号字形集中管理（见头文件说明）
    // ==========================================================
    namespace
    {
        // 中文像素字体（ChillBitmap）中"cmap 有条目但渲染为空"的装饰符号。
        // 实测空白：✓ ✔ ✎ ♡ ♥ ★ ☆ ⌁ ● ◎ ○ ◆ ■ ✦ ▲ ▼ ▸ ▾ ◀ ▶ ↑ ↓ ← → ↗ ↙ ∞ ⚠ · × …
        // 以及常见补充（► ◄ ✖ ✕ ★…）。刻意**不收录** ASCII 字母数字与基本标点
        // （+ - % / : ( ) 等在中文字体里有正常字形），也**不收录**中文标点。
        const juce::String kBrokenSymbolGlyphs = juce::CharPointer_UTF8 (
            "\xe2\x9c\x93" "\xe2\x9c\x94" "\xe2\x9c\x96" "\xe2\x9c\x95"  // ✓ ✔ ✖ ✕
            "\xe2\x9c\x8e"                                                // ✎
            "\xe2\x99\xa1" "\xe2\x99\xa5" "\xe2\x98\x85" "\xe2\x98\x86"  // ♡ ♥ ★ ☆
            "\xe2\x8c\x81"                                                // ⌁
            "\xe2\x97\x8f" "\xe2\x97\x8e" "\xe2\x97\x8b" "\xe2\x97\x89"  // ● ◎ ○ ◉
            "\xe2\x97\x86" "\xe2\x97\x87" "\xe2\x96\xa0" "\xe2\x96\xa1"  // ◆ ◇ ■ □
            "\xe2\x9c\xa6" "\xe2\x9c\xa7"                                // ✦ ✧
            "\xe2\x96\xb2" "\xe2\x96\xbc" "\xe2\x96\xb8" "\xe2\x96\xbe"  // ▲ ▼ ▸ ▾
            "\xe2\x97\x80" "\xe2\x96\xb6" "\xe2\x97\x84" "\xe2\x96\xba"  // ◀ ▶ ◄ ►
            "\xe2\x86\x91" "\xe2\x86\x93" "\xe2\x86\x90" "\xe2\x86\x92"  // ↑ ↓ ← →
            "\xe2\x86\x97" "\xe2\x86\x99" "\xe2\x86\x96" "\xe2\x86\x98"  // ↗ ↙ ↖ ↘
            "\xe2\x88\x9e"                                                // ∞
            "\xe2\x9a\xa0"                                                // ⚠
            "\xe2\x98\xb0"                                                // ☰
            "\xc2\xb7" "\xc3\x97" "\xe2\x80\xa2" "\xe2\x80\xa6"          // · × • …
            "\xe2\x9c\xbf" "\xe2\x9d\x80" "\xe2\x9d\xa4"                // ✿ ❀ ❤
            );
        // 刻意**不收录**：— – “ ” ‘ ’ ‹ ›（中英文字体都有正常字形，
        //   收进来只会让中文句子在每个破折号 / 引号处被切开）。

        // 是否宽字符（CJK / 全角 / emoji 等）：这些字符**绝不能**交给
        //   英文 UI 字体整串渲染，必须留在当前语言字体里。
        bool hasWideChar (const juce::String& text) noexcept
        {
            for (auto p = text.getCharPointer(); ! p.isEmpty();)
                if (p.getAndAdvance() >= 0x2E80)
                    return true;
            return false;
        }
    }

    juce::Typeface::Ptr SymbolGlyphs::getSymbolTypeface()
    {
        if (gSymbolTypeface != nullptr)
            return gSymbolTypeface;

        // 复用 loadActiveTypeface 的缓存：英文界面下与 gTypeface 是同一个
        // Typeface 对象（指针相等 → needsSymbolFont 直接 no-op，零额外字体）
        for (auto& kv : gTypefaceCache)
            if (kv.first == "builtin:Silkscreen")
            {
                gSymbolTypeface = kv.second;
                return gSymbolTypeface;
            }

        gSymbolTypeface = juce::Typeface::createSystemTypefaceFor (
            BinaryData::SilkscreenRegular_ttf, (size_t) BinaryData::SilkscreenRegular_ttfSize);
        if (gSymbolTypeface != nullptr)
            gTypefaceCache.emplace_back ("builtin:Silkscreen", gSymbolTypeface);
        return gSymbolTypeface;
    }

    juce::Font SymbolGlyphs::makeSymbolFont (float height)
    {
        // 英文 UI 字体 + 显式回退族：Silkscreen 本身没有装饰符号字形，
        //   实际字形由 Apple Symbols / Segoe UI Symbol 等提供；
        //   漏拆 run 混入的 CJK 也能落到可渲染的字体，而不是空白。
        if (auto sym = getSymbolTypeface())
            return juce::Font (juce::FontOptions (sym)
                                   .withFallbacks (symbolFallbackFamilies())
                                   .withHeight (height));

        return juce::Font (juce::FontOptions (height, juce::Font::plain)
                               .withFallbacks (symbolFallbackFamilies()));
    }

    bool SymbolGlyphs::needsSymbolFont (const juce::String& text)
    {
        if (text.isEmpty())
            return false;

        // 当前语言字体尚未就绪 / 与英文 UI 字体相同（英文界面）→ 无需切换
        if (gTypeface == nullptr)
            return false;
        const auto sym = getSymbolTypeface();
        if (sym == nullptr || sym == gTypeface)
            return false;

        // 混排（含 CJK）：不能用整串换字体的方式（Silkscreen 无 CJK 字形，
        //   mac 的系统回退又不靠谱）—— 交给 splitRuns / drawText 按 run 绘制。
        if (hasWideChar (text))
            return false;

        return text.containsAnyOf (kBrokenSymbolGlyphs);
    }

    bool SymbolGlyphs::isSymbolsOnly (const juce::String& text) noexcept
    {
        if (text.isEmpty())
            return false;

        for (auto p = text.getCharPointer(); ! p.isEmpty();)
        {
            const juce_wchar c = p.getAndAdvance();
            if (juce::CharacterFunctions::isLetterOrDigit (c)) return false; // 拉丁字母/数字
            if (c >= 0x2E80) return false;                                   // CJK 及宽字符块
            if (c == ' ')     return false;
        }
        return true;
    }

    bool SymbolGlyphs::cjkFontActive()
    {
        if (gTypeface == nullptr)
            return false;
        const auto sym = getSymbolTypeface();
        return sym != nullptr && sym != gTypeface;
    }

    juce::Font SymbolGlyphs::getFont (const juce::String& text, float height, int styleFlags)
    {
        const auto base = PinkXP::getFont (height, styleFlags);
        if (! needsSymbolFont (text))
            return base;
        return makeSymbolFont (base.getHeight());
    }

    juce::Font SymbolGlyphs::rebase (const juce::Font& base, const juce::String& text)
    {
        if (! needsSymbolFont (text))
            return base;
        return makeSymbolFont (base.getHeight());
    }

    juce::Font SymbolGlyphs::buttonFont (const juce::String& text, float height, int styleFlags)
    {
        // CJK 方块字撑满字面框，按钮里视觉比英文大 —— 在语言包 @fontScale
        //   的全局校正之外，按钮文字再收紧 10%（英文界面 0.9 系数不生效）。
        return getFont (text, cjkFontActive() ? height * 0.9f : height, styleFlags);
    }

    juce::Font SymbolGlyphs::iconFont (float height, int styleFlags)
    {
        // 图标字符在任何语言下都必须与英文界面完全一致（字形 + 尺寸 + 位置）。
        //   因此高度只走"语言无关"的换算：1.5x 主字号放大 + uiScale 密度缩放，
        //   **不**应用语言包 @fontScale / @minFontHeight（否则中文模式下图标会
        //   比英文模式缩小、居中位置跟着微移）。
        //   英文界面下 gFontScale=1 / 无 minFontHeight，本路径与 getFont 结果相同。
        const float scaled = (height >= 8.0f) ? height * 1.5f : height;
        const float h = scaled * uiScale();

        // 英文 UI 字体不可用时 makeSymbolFont 会退回同族系统字体，
        //   不会得到"当前语言字体"（否则中英文界面图标风格会不一致）。
        juce::ignoreUnused (styleFlags);
        return makeSymbolFont (h);
    }

    juce::Font SymbolGlyphs::infinityFont (float height, int styleFlags)
    {
        // 字号口径与 iconFont 完全一致：语言无关（1.5x + uiScale），
        //   这样中英文界面里 ∞ 的大小与位置逐像素相同。
        const float scaled = (height >= 8.0f) ? height * 1.5f : height;
        const float h = scaled * uiScale();

        // 显式指定主字体：Arial Unicode MS 的 ∞ 字面高约为 0.32 字高，
        //   比 symbolFallbackFamilies() 首选的 Apple Symbols（约 0.19）饱满得多。
        //   忽略 styleFlags：Arial Unicode MS 没有 bold 字型，带 bold 去查会被
        //   系统替换成别的字体（字形 / 字宽都会变），∞ 只需要一个固定字形。
        juce::ignoreUnused (styleFlags);
        return juce::Font (juce::FontOptions ("Arial Unicode MS", h, juce::Font::plain)
                               .withFallbacks (symbolFallbackFamilies()));
    }

    // ==========================================================
    // 混排文本 run 化
    //   Silkscreen 没有任何装饰符号字形（靠系统回退），ChillBitmap 没有任何
    //   拉丁字形；两者都**不能**整串绘制混排文本。这里按字符类别切分：
    //     · 装饰符号 → 英文 UI 字体（+ 符号回退族）
    //     · 其余（CJK / 拉丁 / 标点）→ 当前语言字体
    //   英文界面下两字体相同，splitRuns 直接返回单 run（零开销）。
    // ==========================================================
    std::vector<SymbolGlyphs::Run> SymbolGlyphs::splitRuns (const juce::String& text)
    {
        std::vector<Run> runs;
        if (text.isEmpty())
            return runs;

        // 英文界面 / 字体尚未就绪：单 run
        if (! cjkFontActive())
        {
            runs.push_back ({ text, false });
            return runs;
        }

        // 纯英文 / 纯符号：保持既有"整串选字体"行为，避免无谓拆分
        if (! hasWideChar (text))
        {
            runs.push_back ({ text, needsSymbolFont (text) });
            return runs;
        }

        // 混排：按下标切分（避免逐字符构造 String，本函数在绘制热路径上）。
        //   用 String::operator[] 按 UTF-16 单元遍历，与 substring 的下标口径
        //   一致（CharPointer 的 getAndAdvance 会把代理对合并成一个码点，
        //   下标就会和 substring 错位 —— emoji 命名主题会踩到）。
        int chunkStart = 0;
        bool chunkIsSymbol = false;
        bool started = false;

        for (int i = 0; i < text.length(); ++i)
        {
            const juce_wchar c = text[i];
            const bool isSymbol = kBrokenSymbolGlyphs.containsChar (c);

            if (! started)
            {
                chunkIsSymbol = isSymbol;
                started = true;
            }
            else if (isSymbol != chunkIsSymbol)
            {
                runs.push_back ({ text.substring (chunkStart, i), chunkIsSymbol });
                chunkStart = i;
                chunkIsSymbol = isSymbol;
            }
        }

        if (chunkStart < text.length())
            runs.push_back ({ text.substring (chunkStart), chunkIsSymbol });

        return runs;
    }

    float SymbolGlyphs::getTextWidth (const juce::String& text, const juce::Font& baseFont)
    {
        float width = 0.0f;

        for (const auto& run : splitRuns (text))
        {
            const auto& font = run.symbol ? makeSymbolFont (baseFont.getHeight()) : baseFont;
            width += juce::GlyphArrangement::getStringWidth (font, run.text);
        }

        return width;
    }

    void SymbolGlyphs::drawText (juce::Graphics& g,
                                 const juce::String& text,
                                 juce::Rectangle<int> area,
                                 juce::Justification justification,
                                 const juce::Font& baseFont)
    {
        if (text.isEmpty() || area.isEmpty())
            return;

        // 多行：退回整串绘制（需要换行的文本极少包含装饰符号）
        if (text.containsChar ('\n'))
        {
            g.setFont (rebase (baseFont, text));
            g.drawFittedText (text, area, justification,
                              juce::jmax (1, (int) ((float) area.getHeight() / juce::jmax (1.0f, baseFont.getHeight()))),
                              1.0f);
            return;
        }

        const auto runs = splitRuns (text);

        // 单 run 且不是符号：走原生 drawText（保留省略号 / 压缩等行为）
        if (runs.size() == 1 && ! runs[0].symbol)
        {
            g.setFont (baseFont);
            g.drawText (text, area, justification, false);
            return;
        }

        std::vector<std::pair<juce::Font, float>> runFonts;
        float totalWidth = 0.0f;

        for (const auto& run : runs)
        {
            const auto font = run.symbol ? makeSymbolFont (baseFont.getHeight()) : baseFont;
            const float w = juce::GlyphArrangement::getStringWidth (font, run.text);
            runFonts.emplace_back (font, w);
            totalWidth += w;
        }

        float x = (float) area.getX();
        const int hFlags = justification.getOnlyHorizontalFlags();
        if ((hFlags & juce::Justification::horizontallyCentred) != 0)
            x += ((float) area.getWidth() - totalWidth) * 0.5f;
        else if ((hFlags & juce::Justification::right) != 0)
            x += (float) area.getWidth() - totalWidth;

        float top = (float) area.getY();
        const int vFlags = justification.getOnlyVerticalFlags();
        if ((vFlags & juce::Justification::verticallyCentred) != 0)
            top += ((float) area.getHeight() - baseFont.getHeight()) * 0.5f;
        else if ((vFlags & juce::Justification::bottom) != 0)
            top += (float) area.getHeight() - baseFont.getHeight();

        const float baseline = top + baseFont.getAscent();

        // 用 addLineOfText（无 maxWidth 截断）而不是 drawText：
        //   Graphics::drawText 的 curtailment 会按 Silkscreen 的 advance 判断
        //   "放不下"，而实际回退字形（如 ♥ 整 em 宽）更宽 —— 16px 图标框里
        //   爱心/五角星会被整颗丢弃，正是 mac 上调色板图标与主题图标空白的原因。
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area);

        juce::GlyphArrangement arrangement;
        for (size_t i = 0; i < runs.size(); ++i)
        {
            arrangement.addLineOfText (runFonts[i].first, runs[i].text, x, baseline);
            x += runFonts[i].second;
        }
        arrangement.draw (g);
    }

    void SymbolGlyphs::appendRuns (juce::AttributedString& as,
                                   const juce::String& text,
                                   const juce::Font& baseFont,
                                   juce::Colour colour)
    {
        for (const auto& run : splitRuns (text))
        {
            const auto& font = run.symbol ? makeSymbolFont (baseFont.getHeight()) : baseFont;
            as.append (run.text, font, colour);
        }
    }

    // ======================================================
    // 静态文本位图缓存（v2.9.6 性能）
    //   mac 上 Editor GL（CPE）渲染下，drawText 每帧触发 HarfBuzz 塑形 +
    //   逐 glyph 的 GL 调用，NSOpenGL 驱动（Metal 转译层）下开销是
    //   Windows 的约 10 倍。标题栏、轴标签等「内容不变但组件每帧全量
    //   重绘」的静态文本是 GL 线程最大热点。
    //   这里把静态文本一次性 CPU 光栅化成 ARGB Image，之后每帧只需
    //   一次 drawImageAt（单纹理 quad），热点降一个数量级。
    //   缓存 key 含文本/字号/颜色/缩放，主题切换后 key 变化自然失效；
    //   容量上限保护避免极端主题组合下内存失控。
    //
    //   v3.x（mac 性能）：位图按"设备像素缩放"光栅化（StaticText）。Retina 下
    //   Graphics 变换含 2x 缩放，1x 位图既会被放大发虚，又走不了 JUCE 的整数
    //   平移 blit 快速路径（退化成 clipToPath → EdgeTable → 变换采样）。
    //   设备分辨率位图配合 drawAt 的设备像素对齐，绘制只剩一次纹理 blit。
    // ======================================================
    namespace
    {
        struct StaticTextKey
        {
            juce::String shown;
            float fontHeight;
            int styleFlags;
            juce::uint32 colour, shadow;
            float uiScaleValue;
            int fontKind;
            bool padded;
            juce::uint32 generation;
            float scale;

            bool operator== (const StaticTextKey& o) const noexcept
            {
                return fontHeight == o.fontHeight && styleFlags == o.styleFlags
                    && colour == o.colour && shadow == o.shadow
                    && uiScaleValue == o.uiScaleValue && fontKind == o.fontKind
                    && padded == o.padded
                    && generation == o.generation && scale == o.scale
                    && shown == o.shown;
            }
        };

        struct StaticTextKeyHash
        {
            size_t operator() (const StaticTextKey& k) const noexcept
            {
                size_t h = (size_t) k.shown.hash();
                auto mix = [&h] (size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
                mix (std::hash<float>{} (k.fontHeight));
                mix ((size_t) k.styleFlags);
                mix ((size_t) k.colour);
                mix ((size_t) k.shadow);
                mix (std::hash<float>{} (k.uiScaleValue));
                mix ((size_t) k.fontKind);
                mix ((size_t) (k.padded ? 1 : 0));
                mix ((size_t) k.generation);
                mix (std::hash<float>{} (k.scale));
                return h;
            }
        };

        using StaticTextCache = std::unordered_map<StaticTextKey, StaticText, StaticTextKeyHash>;

        // 有意不析构（同 fontCacheState）；正常释放走 releaseTypefaceCache
        StaticTextCache& staticTextCache()
        {
            static auto& cache = *new StaticTextCache();
            return cache;
        }

        // 设备像素缩放：量化到 0.25 档，限制缓存键数量（窗口跨屏时才会变化）
        float quantiseDeviceScale (float s) noexcept
        {
            if (! std::isfinite (s) || s <= 0.0f)
                return 1.0f;
            return juce::jlimit (1.0f, 4.0f, std::round (s * 4.0f) / 4.0f);
        }

        // padForSymbols：含符号 run 时位图四周留白、文字不贴边（回退字形常比
        //   主字体的 ascent/advance 更大）；仅 drawCachedSymbolText 使用，
        //   其余调用方的位图尺寸 / 排版保持不变。
        StaticText buildStaticText (const juce::String& text,
                                    float fontHeight,
                                    int styleFlags,
                                    juce::Colour colour,
                                    juce::Colour shadowColour,
                                    StaticTextFont fontKind,
                                    float scale,
                                    bool translate = true,
                                    bool padForSymbols = false);

        juce::Font makeStaticTextFont (StaticTextFont kind, const juce::String& shown,
                                       float fontHeight, int styleFlags)
        {
            switch (kind)
            {
                case StaticTextFont::axis:       return getAxisFont (fontHeight, styleFlags);
                case StaticTextFont::symbolAuto: return SymbolGlyphs::getFont (shown, fontHeight, styleFlags);
                case StaticTextFont::button:     return SymbolGlyphs::buttonFont (shown, fontHeight, styleFlags);
                case StaticTextFont::icon:       return SymbolGlyphs::iconFont (fontHeight, styleFlags);
                case StaticTextFont::ui:
                default:                         return getFont (fontHeight, styleFlags);
            }
        }

        StaticTextFont kindFor (bool axisFont) noexcept
        {
            return axisFont ? StaticTextFont::axis : StaticTextFont::ui;
        }

        juce::Point<float> layoutStaticText (const StaticText& t,
                                             juce::Rectangle<float> area,
                                             juce::Justification justification);
    }

    static void releaseStaticTextCache()
    {
        staticTextCache().clear();
    }

    void StaticText::drawAt (juce::Graphics& g, float x, float y) const
    {
        if (! image.isValid())
            return;

        if (scale == 1.0f)
        {
            g.drawImageAt (image, juce::roundToInt (x), juce::roundToInt (y), false);
            return;
        }

        // 对齐到设备像素：最终变换只剩整数平移 → JUCE renderImage 走无变换 blit
        const float sx = std::round (x * scale) / scale;
        const float sy = std::round (y * scale) / scale;
        g.drawImageTransformed (image,
                                juce::AffineTransform::scale (1.0f / scale).translated (sx, sy),
                                false);
    }

    StaticText cachedStaticText (juce::Graphics& g,
                                 const juce::String& text,
                                 float fontHeight,
                                 int styleFlags,
                                 juce::Colour colour,
                                 juce::Colour shadowColour,
                                 bool axisFont)
    {
        const float scale = quantiseDeviceScale (g.getInternalContext().getPhysicalPixelScaleFactor());
        return buildStaticText (text, fontHeight, styleFlags, colour, shadowColour, kindFor (axisFont), scale);
    }

    void drawCachedText (juce::Graphics& g,
                         const juce::String& text,
                         juce::Rectangle<int> area,
                         juce::Justification justification,
                         float fontHeight,
                         int styleFlags,
                         juce::Colour colour,
                         bool axisFont)
    {
        drawCachedText (g, text, area.toFloat(), justification, fontHeight, styleFlags, colour, axisFont);
    }

    void drawCachedText (juce::Graphics& g,
                         const juce::String& text,
                         juce::Rectangle<float> area,
                         juce::Justification justification,
                         float fontHeight,
                         int styleFlags,
                         juce::Colour colour,
                         bool axisFont)
    {
        if (text.isEmpty() || area.isEmpty())
            return;

        // 位图烘焙不透明色，绘制时用当前填充色的 alpha 调制（renderImage 语义）
        //   → 与 drawText 的"字形覆盖率 × 颜色 alpha"等价，且不同 alpha 共享同一位图。
        g.setColour (colour);
        const auto t = cachedStaticText (g, text, fontHeight, styleFlags,
                                         colour.withAlpha (1.0f),
                                         juce::Colours::transparentBlack, axisFont);

        // 放不下：drawText 会整颗丢弃溢出的 glyph —— 退回原路径保持完全一致
        if (! t.isValid() || t.textWidth > area.getWidth())
        {
            const auto previousFont = g.getCurrentFont();
            const juce::ScopeGuard restoreFont { [&] { g.setFont (previousFont); } };
            g.setFont (axisFont ? getAxisFont (fontHeight, styleFlags)
                                : getFont (fontHeight, styleFlags));
            g.drawText (y2k::tr (text), area, justification, false);
            return;
        }

        const auto pos = layoutStaticText (t, area, justification);
        t.drawAt (g, pos.x, pos.y);
    }

    void drawCachedSymbolText (juce::Graphics& g,
                               const juce::String& text,
                               juce::Rectangle<int> area,
                               juce::Justification justification,
                               float fontHeight,
                               int styleFlags,
                               juce::Colour colour,
                               StaticTextFont fontKind,
                               juce::Colour shadowColour,
                               bool translate)
    {
        if (text.isEmpty() || area.isEmpty())
            return;

        // 无阴影：位图烘焙不透明色、按填充色 alpha 调制（不同 alpha 共享位图）；
        //   有阴影：两层颜色原样烘焙，以不透明填充色绘制。
        const bool hasShadow = ! shadowColour.isTransparent();
        g.setColour (hasShadow ? colour.withAlpha (1.0f) : colour);

        const float scale = quantiseDeviceScale (g.getInternalContext().getPhysicalPixelScaleFactor());
        const auto t = buildStaticText (text, fontHeight, styleFlags,
                                        hasShadow ? colour : colour.withAlpha (1.0f),
                                        shadowColour, fontKind, scale, translate, true);

        // 缓存失败 / 横向放不下（单 run 普通文本在原路径会丢 glyph）：走原路径保持一致
        if (! t.isValid() || t.textWidth > (float) area.getWidth())
        {
            const auto shown = translate ? y2k::tr (text) : text;
            const auto font = makeStaticTextFont (fontKind, shown, fontHeight, styleFlags);
            if (hasShadow)
            {
                g.setColour (shadowColour);
                SymbolGlyphs::drawText (g, shown, area.translated (1, 1), justification, font);
            }
            g.setColour (colour);
            SymbolGlyphs::drawText (g, shown, area, justification, font);
            return;
        }

        // SymbolGlyphs::drawText 语义：超出部分按 area 裁剪（不丢 glyph）
        const auto pos = layoutStaticText (t, area.toFloat(), justification);
        const bool fits = pos.x >= (float) area.getX()
                       && pos.y >= (float) area.getY()
                       && pos.x + (float) t.getWidth()  <= (float) area.getRight()
                       && pos.y + (float) t.getHeight() <= (float) area.getBottom();
        if (fits)
        {
            t.drawAt (g, pos.x, pos.y);
            return;
        }

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area);
        t.drawAt (g, pos.x, pos.y);
    }

    namespace
    {
    juce::Point<float> layoutStaticText (const StaticText& t,
                                         juce::Rectangle<float> area,
                                         juce::Justification justification)
    {
        // 与 Graphics::drawText 相同的排版：文本行（宽 textWidth、高 fontH）按
        //   justification 放入 area；位图内文字左缘在 originX、行顶在 textTop。
        float x = area.getX() - t.originX;
        const int hFlags = justification.getOnlyHorizontalFlags();
        if ((hFlags & juce::Justification::horizontallyCentred) != 0)
            x += (area.getWidth() - t.textWidth) * 0.5f;
        else if ((hFlags & juce::Justification::right) != 0)
            x += area.getWidth() - t.textWidth;

        // 垂直：位图内文字垂直居中排在 [boxTop, boxTop + boxHeight) 的框里。居中时
        //   直接对齐两个框的中心（与行高的具体口径无关；仅有 ≤ 半个设备像素的对齐取整）。
        float y = area.getY();
        const int vFlags = justification.getOnlyVerticalFlags();
        if ((vFlags & juce::Justification::verticallyCentred) != 0)
            y += (area.getHeight() - t.boxHeight) * 0.5f - t.boxTop;
        else if ((vFlags & juce::Justification::bottom) != 0)
            y += area.getHeight() - t.lineHeight - t.textTop;
        else
            y -= t.textTop;

        return { x, y };
    }
    } // namespace

    juce::Image cachedStaticTextImage (const juce::String& text,
                                       float fontHeight,
                                       int styleFlags,
                                       juce::Colour colour,
                                       juce::Colour shadowColour,
                                       bool axisFont)
    {
        return buildStaticText (text, fontHeight, styleFlags, colour, shadowColour, kindFor (axisFont), 1.0f).image;
    }

    namespace
    {
    StaticText buildStaticText (const juce::String& text,
                                float fontHeight,
                                int styleFlags,
                                juce::Colour colour,
                                juce::Colour shadowColour,
                                StaticTextFont fontKind,
                                float scale,
                                bool translate,
                                bool padForSymbols)
    {
        if (text.isEmpty())
            return {};

        // 界面语言：位图里画的是译文（键里也是译文 + 字体代次，切语言 / 换字体自然失效）
        const juce::String shown = translate ? y2k::tr (text) : text;

        // drawCachedSymbolText：多行文本走 drawFittedText，不做位图缓存（调用方回退原路径）
        if (shown.isEmpty() || (padForSymbols && shown.containsChar ('\n')))
            return {};

        StaticTextKey key { shown, fontHeight, styleFlags,
                            colour.getARGB(), shadowColour.getARGB(),
                            uiScale(), (int) fontKind, padForSymbols,
                            getFontGeneration(), scale };

        // 仅在消息线程 / 持 MessageManager 锁的 GL 渲染线程上绘制调用，天然串行
        auto& cache = staticTextCache();

        const auto it = cache.find (key);
        if (it != cache.end())
            return it->second;

        // 基准字体（不含"整串换符号字体"）：混排文本由 drawText 按 run 绘制，
        //   符号片段自动换英文 UI 字体，CJK 仍用当前语言字体。
        const juce::Font font = makeStaticTextFont (fontKind, shown, fontHeight, styleFlags);

        // 含符号 run 时（SymbolGlyphs::drawText 走按 run 绘制 + 裁剪到排版框）四周留白：
        //   回退字形（♥ ★ ∞ 等）的字面常超出主字体的 ascent/advance，贴边会被裁掉。
        //   排版框放大到整张位图并水平居中：按 run 绘制的居中口径正是
        //   (框宽 - getTextWidth) / 2，故 originX 可精确求得。
        bool hasSymbolRun = false;
        if (padForSymbols)
            for (const auto& run : SymbolGlyphs::splitRuns (shown))
                hasSymbolRun = hasSymbolRun || run.symbol;

        const int padX = hasSymbolRun ? (int) std::ceil (font.getHeight() * 0.15f) + 1 : 0;
        const int padY = hasSymbolRun ? (int) std::ceil (font.getHeight() * 0.35f) : 0;

        // 位图尺寸必须"向上取整 + 留余量"，不能四舍五入：
        //   Graphics::drawText 内部走 GlyphArrangement::addCurtailedLineOfText，
        //   文本一旦超出给定宽度，**放不下的整颗 glyph 会被直接丢弃**（既不挤压
        //   也不补省略号）。旧实现 w - 2 == round(测量宽度)，只要测量宽度被抹掉
        //   零点几像素，末位字符就会整颗消失（"8.4" 只剩 "8"、"M"/"I"/"L" 这种
        //   单字符标签甚至会整块空白），而且同一字符串永远复现。
        //   现在 w - 2 >= ceil(测量宽度) + 2，且右侧再留 2px 给阴影偏移。
        //   宽度必须走 getTextWidth（GlyphArrangement）：Font::getStringWidthFloat
        //   按主字体的 notdef advance 测量，回退字形更宽时会把末位字符切掉。
        const float textW = SymbolGlyphs::getTextWidth (shown, font);
        if ((int) std::ceil (textW) <= 0 || (int) std::ceil (font.getHeight()) <= 0)
            return {};
        const int w = (int) std::ceil (textW) + 4 + 2 * padX;
        const int h = (int) std::ceil (font.getHeight()) + 3 + 2 * padY;

        // 逻辑尺寸 w×h 不变（调用方按 getWidth/getHeight 布局），像素按设备缩放
        const int pw = juce::jmax (1, (int) std::ceil ((float) w * scale));
        const int ph = juce::jmax (1, (int) std::ceil ((float) h * scale));
        juce::Image img (juce::Image::ARGB, pw, ph, true, juce::SoftwareImageType());
        {
            juce::Graphics g2 (img);
            if (scale != 1.0f)
                g2.addTransform (juce::AffineTransform::scale (scale));

            const auto just = hasSymbolRun ? juce::Justification::centred
                                           : juce::Justification::centredLeft;
            if (! shadowColour.isTransparent())
            {
                g2.setColour (shadowColour);
                SymbolGlyphs::drawText (g2, shown, { 1, 1, w - 2, h - 2 }, just, font);
            }
            g2.setColour (colour);
            SymbolGlyphs::drawText (g2, shown, { 0, 0, w - 2, h - 2 }, just, font);
        }

        StaticText t;
        t.image      = img;
        t.scale      = scale;
        t.width      = w;
        t.height     = h;
        t.textWidth  = textW;
        t.boxTop     = (float) padY;
        t.boxHeight  = (float) (h - 2 - 2 * padY);
        t.lineHeight = font.getHeight();
        t.textTop    = ((float) (h - 2) - t.lineHeight) * 0.5f;
        t.originX    = hasSymbolRun ? ((float) (w - 2) - textW) * 0.5f : 0.0f;

        // 容量保护：超限整体清空（静态文本数量有限，正常远达不到；
        //   按钮上的 FPS 等少量动态文本也走这里，故留足余量）
        if (cache.size() > 1024)
            cache.clear();
        cache.emplace (std::move (key), t);
        return t;
    }
    } // namespace

    // 坐标轴/刻度专用字体：不放大，保持原始尺寸
    juce::Font getAxisFont(float height, int styleFlags)
    {
        return makeFontRaw(height, styleFlags);
    }

    juce::String formatFreqHz(float hz)
    {
        if (hz >= 1000.0f)
            return juce::String(hz / 1000.0f, 1) + " kHz";
        return juce::String((int) std::lround(hz)) + " Hz";
    }

    juce::String noteNameForHz(float hz)
    {
        if (! std::isfinite(hz) || hz < 16.0f || hz > 20000.0f)
            return {};

        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B" };
        const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
        if (! std::isfinite(midi) || midi < 12.0f || midi > 132.0f)
            return {};

        const int n   = juce::roundToInt(midi);
        const int pc  = ((n % 12) + 12) % 12;
        const int oct = n / 12 - 1;
        return juce::String(names[pc]) + juce::String(oct);
    }

    // ==========================================================
    // 前景色可读性兜底（WCAG 相对亮度 / 对比度）
    // ==========================================================
    namespace
    {
        float srgbToLinear (juce::uint8 v) noexcept
        {
            const float s = (float) v / 255.0f;
            return s <= 0.04045f ? s / 12.92f
                                 : std::pow ((s + 0.055f) / 1.055f, 2.4f);
        }

        float relativeLuminance (juce::Colour c) noexcept
        {
            return 0.2126f * srgbToLinear (c.getRed())
                 + 0.7152f * srgbToLinear (c.getGreen())
                 + 0.0722f * srgbToLinear (c.getBlue());
        }

        float contrastRatio (juce::Colour a, juce::Colour b) noexcept
        {
            const float la = relativeLuminance (a);
            const float lb = relativeLuminance (b);
            return (juce::jmax (la, lb) + 0.05f) / (juce::jmin (la, lb) + 0.05f);
        }
    }

    juce::Colour readableInkOn (juce::Colour fill, float minContrast)
    {
        const auto bg = fill.withAlpha (1.0f);

        // 主题 ink 优先 —— 其余 10 套预设主题都在 11:1 以上，保持原有配色观感
        if (contrastRatio (ink, bg) >= minContrast)
            return ink;

        // 兜底：Starlight(1.48:1) / Cyber Lilac(1.04:1) 这类"face 与 ink 同为深色"，
        //   以及用户把 Control 调得与 Text 接近的自定义主题。挑对比度最高的候选，
        //   保证文字至少可读（这两套主题会落到纯白）。
        const juce::Colour candidates[] = { ink, selInk,
                                            juce::Colours::black, juce::Colours::white };
        juce::Colour best = juce::Colours::white;
        float bestRatio = 0.0f;
        for (const auto c : candidates)
        {
            const auto r = contrastRatio (c, bg);
            if (r > bestRatio) { bestRatio = r; best = c; }
        }
        return best;
    }

    void drawHoverRuler(juce::Graphics& g, juce::Rectangle<int> canvas,
                        juce::Point<int> pos, const juce::String& readoutSource)
    {
        if (readoutSource.isEmpty())
            return;

        // 读数里的文字部分按界面语言翻译（数字走模板：如 "PAN L{0}"）
        const juce::String readout = y2k::tr (readoutSource);

        const int x = pos.x;
        const int y = pos.y;

        // 十字线（竖线 + 横线），半透明墨色，覆盖整个仪表区域
        g.setColour(ink.withAlpha(0.55f));
        g.drawVerticalLine(x, (float) canvas.getY(), (float) canvas.getBottom());
        g.drawHorizontalLine(y, (float) canvas.getX(), (float) canvas.getRight());

        // 读数框：绘制在鼠标右上方，尺寸按文本估算。
        //   反馈修复（外网 #5）：旧版硬编码 9px 太小；改为带 1.5x UI 放大的
        //   getFont，框高随字体实际高度自适应（10 号再微调收敛）。
        const int   fontH = 10;
        const int   padX  = 6;
        const int   padY  = 4;
        const auto  readoutFont = getFont ((float) fontH, juce::Font::plain);
        const int   textH = juce::jmax (16, (int) std::ceil (readoutFont.getHeight()));
        const int   boxH  = textH + padY * 2;
        const int   textW = juce::jmax(40, (int) std::ceil(readoutFont.getStringWidth(readout)) + padX * 2);

        int bx = x + 12;
        int by = y - boxH - 8;
        // 避免读数框超出仪表区域：右侧/顶部越界时回退到鼠标左/下侧
        if (bx + textW > canvas.getRight())
            bx = x - textW - 12;
        if (by < canvas.getY())
            by = y + 8;
        bx = juce::jlimit(canvas.getX(), canvas.getRight() - textW, bx);
        by = juce::jlimit(canvas.getY(), canvas.getBottom() - boxH, by);

        g.setColour(ink.withAlpha(0.85f));
        g.fillRect(bx, by, textW, boxH);
        g.setColour(hl);
        g.setFont(readoutFont);
        g.drawText(readout, bx, by, textW, boxH, juce::Justification::centred, false);
    }

    void drawRaised(juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill)
    {
        g.setColour(fill);
        g.fillRect(r);
        drawRaisedFrame (g, r);
    }

    void drawRaisedFrame(juce::Graphics& g, juce::Rectangle<int> r)
    {
        // 外层 2px 硬边框：左+上 白高光，右+下 深色
        g.setColour(hl);
        g.fillRect(r.getX(), r.getY(), r.getWidth(), 2);
        g.fillRect(r.getX(), r.getY(), 2, r.getHeight());
        g.setColour(dark);
        g.fillRect(r.getX(), r.getBottom() - 2, r.getWidth(), 2);
        g.fillRect(r.getRight() - 2, r.getY(), 2, r.getHeight());

        // 内层 1px 高光/阴影
        g.setColour(juce::Colour(0xfffff4f9));
        g.fillRect(r.getX() + 2, r.getY() + 2, r.getWidth() - 4, 1);
        g.fillRect(r.getX() + 2, r.getY() + 2, 1, r.getHeight() - 4);
        g.setColour(shdw);
        g.fillRect(r.getX() + 2, r.getBottom() - 3, r.getWidth() - 4, 1);
        g.fillRect(r.getRight() - 3, r.getY() + 2, 1, r.getHeight() - 4);
    }

    void drawSunken(juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill)
    {
        g.setColour(fill);
        g.fillRect(r);

        g.setColour(dark);
        g.fillRect(r.getX(), r.getY(), r.getWidth(), 2);
        g.fillRect(r.getX(), r.getY(), 2, r.getHeight());
        g.setColour(hl);
        g.fillRect(r.getX(), r.getBottom() - 2, r.getWidth(), 2);
        g.fillRect(r.getRight() - 2, r.getY(), 2, r.getHeight());

        g.setColour(shdw);
        g.fillRect(r.getX() + 2, r.getY() + 2, r.getWidth() - 4, 1);
        g.fillRect(r.getX() + 2, r.getY() + 2, 1, r.getHeight() - 4);
        g.setColour(juce::Colours::white);
        g.fillRect(r.getX() + 2, r.getBottom() - 3, r.getWidth() - 4, 1);
        g.fillRect(r.getRight() - 3, r.getY() + 2, 1, r.getHeight() - 4);
    }

    void drawPressed(juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill)
    {
        g.setColour(fill);
        g.fillRect(r);

        g.setColour(dark);
        g.fillRect(r.getX(), r.getY(), r.getWidth(), 2);
        g.fillRect(r.getX(), r.getY(), 2, r.getHeight());
        g.setColour(hl);
        g.fillRect(r.getX(), r.getBottom() - 2, r.getWidth(), 2);
        g.fillRect(r.getRight() - 2, r.getY(), 2, r.getHeight());

        g.setColour(shdw);
        g.fillRect(r.getX() + 2, r.getY() + 2, r.getWidth() - 4, 1);
        g.fillRect(r.getX() + 2, r.getY() + 2, 1, r.getHeight() - 4);
    }

    void drawHardShadow(juce::Graphics& g, juce::Rectangle<int> r, int offset)
    {
        g.setColour(dark);
        g.fillRect(r.getRight(), r.getY() + offset, offset, r.getHeight());
        g.fillRect(r.getX() + offset, r.getBottom(), r.getWidth(), offset);
    }

    // ==========================================================
    // 反馈修复（外网 #3 第二轮）：主题自带的 desktop2 在极端明暗主题下
    // 与 desktop 的明度差过小（voidGrey 黑对黑 / paperGrey 白对白 /
    // crimsonNoir 近黑对近黑），纹理完全不可见。
    //
    // 这里不再直接使用 desktop2，而是在 OKLab 空间重算纹理色：
    //   · 明度：从 desktop 的 L 向反方向推离 kMinDeltaL —— 深色桌面提亮、
    //     浅色桌面压暗，保证感知对比；
    //   · 色相：优先沿用 desktop2；desktop2 接近中性灰时借用强调色 sel，
    //     让纹理始终带主题气质而非隐形灰。
    // ==========================================================
    juce::Colour makeDesktopTextureColour()
    {
        using namespace y2kpal;

        const Rgb8 bg8 { desktop.getRed(),  desktop.getGreen(),  desktop.getBlue()  };
        const Rgb8 tn8 { desktop2.getRed(), desktop2.getGreen(), desktop2.getBlue() };

        const OkLab bgLab = linearToOkLab (rgbToLinear (fromRgb8 (bg8)));
        const OkLab tnLab = linearToOkLab (rgbToLinear (fromRgb8 (tn8)));

        // 1) 目标明度：远离背景明度（kMinDeltaL 的 OKLab 明度差肉眼可辨）
        constexpr float kMinDeltaL = 0.24f;
        const float targetL = clamp01 (bgLab.L >= 0.5f
                                        ? bgLab.L - kMinDeltaL
                                        : bgLab.L + kMinDeltaL);

        // 2) 色相/色度：desktop2 有色相则沿用，近中性则借 sel
        const float tnC = std::sqrt (tnLab.a * tnLab.a + tnLab.b * tnLab.b);
        float C, H;
        if (tnC >= 0.03f)
        {
            C = tnC;
            H = wrapHue (std::atan2 (tnLab.b, tnLab.a) * (180.0f / 3.14159265358979323846f));
        }
        else
        {
            const OkLab acLab = linearToOkLab (rgbToLinear (fromRgb8 ({ sel.getRed(),
                                                                       sel.getGreen(),
                                                                       sel.getBlue() })));
            C = std::sqrt (acLab.a * acLab.a + acLab.b * acLab.b);
            H = wrapHue (std::atan2 (acLab.b, acLab.a) * (180.0f / 3.14159265358979323846f));
        }

        OkLch out { targetL, C, H };
        out = gamutClamp (out);
        const Rgb8 rgb = okLchToRgb8 (out);
        return juce::Colour ((juce::uint32) 0xff000000u
                           | ((juce::uint32) rgb.r << 16)
                           | ((juce::uint32) rgb.g << 8)
                           |  (juce::uint32) rgb.b);
    }

    void drawDesktop(juce::Graphics& g, juce::Rectangle<int> r)
    {
        // y2k::perf::ScopedPerfTimer desktopTimer(y2k::perf::FunctionId::uiDrawDesktop,
        //                                         y2k::perf::Partition::uiRendering,
        //                                         y2k::perf::ThreadRole::ui);

        // 1) 底色
        g.setColour(desktop);
        g.fillRect(r);

        // 2) 纹理色（反馈 #3：按背景明度保证对比）+ 高光点缀色
        const juce::Colour tex = makeDesktopTextureColour();
        const bool bgIsDark = desktop.getBrightness() < 0.5f;
        const juce::Colour texHi = bgIsDark ? juce::Colours::white : juce::Colours::black;

        // 3) 按主题纹理绘制
        const auto& th = getCurrentTheme();
        switch (th.desktopPattern)
        {
            case DesktopPattern::checker:
            default:
            {
                // 棋盘格（经典 XP 风，v2.10 增大色块让纹理更醒目）
                g.setColour(tex);
                for (int y = r.getY(); y < r.getBottom(); y += 8)
                    for (int x = r.getX() + ((y / 8) % 2) * 8; x < r.getRight(); x += 16)
                        g.fillRect(x, y, 4, 4);
                break;
            }

            case DesktopPattern::pixelStars:
            {
                // 像素星星（"+" 字 + 对角小臂，v2.10 更大更密）
                const int step = 18;
                for (int y = r.getY() + 8; y < r.getBottom(); y += step)
                {
                    for (int x = r.getX() + ((y / step) % 2) * (step / 2); x < r.getRight(); x += step)
                    {
                        g.setColour(tex);
                        g.fillRect(x,     y - 2, 1, 5);          // 竖臂
                        g.fillRect(x - 2, y,     5, 1);          // 横臂
                        g.fillRect(x - 1, y - 1, 1, 1);          // 对角
                        g.fillRect(x + 1, y + 1, 1, 1);
                        g.fillRect(x + 1, y - 1, 1, 1);
                        g.fillRect(x - 1, y + 1, 1, 1);
                        // 次级小点
                        g.setColour(tex.withAlpha(0.7f));
                        g.fillRect(x + step / 3, y + step / 3, 1, 1);
                        g.fillRect(x - step / 3, y + step / 3, 1, 1);
                    }
                }
                break;
            }

            case DesktopPattern::scanGrid:
            {
                // 网格扫描线：水平淡线 + 竖直淡线 + 每 N 行一条亮线
                g.setColour(tex.withAlpha(0.85f));
                for (int y = r.getY(); y < r.getBottom(); y += 6)
                    g.fillRect(r.getX(), y, r.getWidth(), 1);
                g.setColour(tex.withAlpha(0.55f));
                for (int x = r.getX(); x < r.getRight(); x += 6)
                    g.fillRect(x, r.getY(), 1, r.getHeight());
                // 扫描高亮条（每 48px）
                g.setColour(texHi.withAlpha(0.9f));
                for (int y = r.getY(); y < r.getBottom(); y += 48)
                    g.fillRect(r.getX(), y, r.getWidth(), 1);
                break;
            }

            case DesktopPattern::bigDots:
            {
                // 大圆点（2003 MSN / 复古 iPod 感，v2.10 圆点更大 + 高光点）
                const int step = 30;
                const int radius = 7;
                for (int y = r.getY() + step / 2; y < r.getBottom(); y += step)
                {
                    for (int x = r.getX() + ((y / step) % 2) * (step / 2); x < r.getRight(); x += step)
                    {
                        g.setColour(tex);
                        g.fillEllipse((float)(x - radius), (float)(y - radius),
                                      (float)(radius * 2), (float)(radius * 2));
                        // 左上高光小点，增加立体感
                        g.setColour(texHi.withAlpha(0.45f));
                        g.fillEllipse((float)(x - radius + 3), (float)(y - radius + 3),
                                      3.0f, 3.0f);
                    }
                }
                break;
            }

            case DesktopPattern::bubbles:
            {
                // 横条泡泡：每 20px 一条细横带 + 带上随机小泡泡（基于位置哈希稳定）
                const int stripeStep = 20;
                for (int y = r.getY() + stripeStep / 2; y < r.getBottom(); y += stripeStep)
                {
                    g.setColour(tex.withAlpha(0.8f));
                    g.fillRect(r.getX(), y, r.getWidth(), 2);

                    // 同一横条上稀疏的珍珠泡泡
                    for (int x = r.getX() + 8; x < r.getRight(); x += 28)
                    {
                        const int h = (x * 2654435761u + y * 40503u) & 0xff;
                        if (h < 140)
                        {
                            const int rad = 2 + (h & 3);
                            g.setColour(texHi.withAlpha(0.65f));
                            g.fillEllipse((float)(x - rad), (float)(y - rad),
                                          (float)(rad * 2), (float)(rad * 2));
                        }
                    }
                }
                break;
            }

            case DesktopPattern::diagStripes:
            {
                // 斜条纹（45°，v2.10 加粗 + 双色深浅交替）
                const int step = 16;
                for (int k = -r.getHeight(); k < r.getWidth(); k += step)
                {
                    const juce::Line<float> line(
                        (float)(r.getX() + k),                (float) r.getY(),
                        (float)(r.getX() + k + r.getHeight()), (float) r.getBottom());
                    g.setColour(tex);
                    g.drawLine(line, 4.0f);
                    const juce::Line<float> line2(
                        (float)(r.getX() + k + 2),                (float) r.getY(),
                        (float)(r.getX() + k + 2 + r.getHeight()), (float) r.getBottom());
                    g.setColour(texHi.withAlpha(0.35f));
                    g.drawLine(line2, 1.0f);
                }
                break;
            }

            case DesktopPattern::hearts:
            {
                // 像素爱心：7x7 小桃心均匀平铺
                //    shape (每个 X 为一个 1px 填充点)：
                //      . X . . . X .
                //      X X X X X X X
                //      X X X X X X X
                //      . X X X X X .
                //      . . X X X . .
                //      . . . X . . .
                constexpr int kStep = 18;
                auto drawHeart = [&](int cx, int cy) {
                  // Row 0（top）
                  g.fillRect(cx - 2, cy - 3, 1, 1);
                  g.fillRect(cx + 2, cy - 3, 1, 1);
                  // Row 1
                  for (int i = -3; i <= 3; ++i) g.fillRect(cx + i, cy - 2, 1, 1);
                  // Row 2
                  for (int i = -3; i <= 3; ++i) g.fillRect(cx + i, cy - 1, 1, 1);
                  // Row 3
                  for (int i = -2; i <= 2; ++i) g.fillRect(cx + i, cy,     1, 1);
                  // Row 4
                  for (int i = -1; i <= 1; ++i) g.fillRect(cx + i, cy + 1, 1, 1);
                  // Row 5（bottom tip）
                  g.fillRect(cx, cy + 2, 1, 1);
                };
                g.setColour(tex);
                const int offset = kStep / 2;
                for (int y = r.getY() + 6; y < r.getBottom(); y += kStep) {
                  const int xPhase = ((y / kStep) % 2) * offset;
                  for (int x = r.getX() + 6 + xPhase; x < r.getRight(); x += kStep) {
                    drawHeart(x, y);
                  }
                }
                break;
            }

            case DesktopPattern::zigzag:
            {
                // 锯齿波纹：水平折线，每行错相位，形成波浪感
                const int period = 48;
                const int amp = 8;
                for (int y = r.getY() + 4; y < r.getBottom(); y += 26)
                {
                    g.setColour(tex.withAlpha(0.9f));
                    for (int x = r.getX(); x < r.getRight(); x += 1)
                    {
                        const int phase = (x / period) % 2;
                        const int zy = (phase == 0)
                            ? y + ((x % period) * amp / (period / 2))
                            : y + amp - (((x % period) - period / 2) * amp / (period / 2));
                        if (zy >= r.getY() && zy < r.getBottom())
                            g.fillRect(x, zy, 1, 2);
                    }
                }
                break;
            }

            case DesktopPattern::crossHatch:
            {
                // 交叉斜线网：两组 45° 线交织，形成菱形格网
                const int step = 22;
                for (int k = -r.getHeight(); k < r.getWidth(); k += step)
                {
                    g.setColour(tex.withAlpha(0.75f));
                    const juce::Line<float> a(
                        (float)(r.getX() + k),                (float) r.getY(),
                        (float)(r.getX() + k + r.getHeight()), (float) r.getBottom());
                    g.drawLine(a, 2.0f);
                    const juce::Line<float> b(
                        (float)(r.getX() + k),                (float) r.getBottom(),
                        (float)(r.getX() + k + r.getHeight()), (float) r.getY());
                    g.drawLine(b, 2.0f);
                }
                break;
            }

            case DesktopPattern::pixelFlower:
            {
                // 像素小花：中心点 + 上下左右四花瓣（5x5 十字花），错位平铺
                const int step = 26;
                for (int y = r.getY() + 8; y < r.getBottom(); y += step)
                {
                    for (int x = r.getX() + 8 + ((y / step) % 2) * (step / 2); x < r.getRight(); x += step)
                    {
                        g.setColour(tex);
                        g.fillRect(x - 1, y - 3, 3, 2);      // 上花瓣
                        g.fillRect(x - 1, y + 2, 3, 2);      // 下花瓣
                        g.fillRect(x - 3, y - 1, 2, 3);      // 左花瓣
                        g.fillRect(x + 2, y - 1, 2, 3);      // 右花瓣
                        g.setColour(texHi.withAlpha(0.8f));
                        g.fillRect(x, y, 1, 1);              // 花蕊
                    }
                }
                break;
            }
        }
    }

    // ------------------------------------------------------
    // 跨实例共享桌面纹理缓存（P6）
    //   · key：当前主题 id + (width, height)
    //   · 最多保留少量尺寸的条目（常见情况是用户开 N 个相同尺寸的 Editor
    //     时，所有实例都命中同一个共享 Image）
    //   · 不使用 JUCE ImageCache：那是一个进程级 static，插件 DLL 卸载时
    //     容易发生像素内存跨 DLL 悬垂；这里改为本 .cpp TU 内部 static，
    //     生命周期与 PinkXPStyle 所在的模块一致。
    // ------------------------------------------------------
    namespace
    {
        struct DesktopTextureEntry
        {
            ThemeId       theme;
            int           width;
            int           height;
            int           patternVersion;
            juce::Image   image;
        };

        // 底纹绘制算法/参数版本：改动 drawDesktop 的视觉参数时 +1，
        // 旧版本条目视为未命中，强制重新烘焙（避免升级后仍显示旧纹理）。
        static constexpr int kDesktopPatternVersion = 3;

        // 采用普通 vector + 简单 LRU 淘汰；N 不会很大（通常 1~3 种尺寸）
        static std::vector<DesktopTextureEntry>& getDesktopTextureCache()
        {
            static std::vector<DesktopTextureEntry> cache;
            return cache;
        }

        static constexpr size_t kMaxDesktopTextureEntries = 4;
    }

    juce::Image getSharedDesktopTexture(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return {};

        auto& cache = getDesktopTextureCache();
        const auto currentTheme = getCurrentThemeId();

        // 查找既存条目
        for (size_t i = 0; i < cache.size(); ++i)
        {
            auto& e = cache[i];
            if (e.theme == currentTheme
                && e.width == width
                && e.height == height
                && e.patternVersion == kDesktopPatternVersion
                && e.image.isValid())
            {
                // 命中：把条目移到末尾（简单 LRU）
                if (i + 1 != cache.size())
                {
                    DesktopTextureEntry hit = std::move(e);
                    cache.erase(cache.begin() + (std::ptrdiff_t) i);
                    cache.push_back(std::move(hit));
                }
                return cache.back().image;
            }
        }

        // 未命中：烘焙一张新的（左上角原点 = (0,0)，尺寸 = width × height）
        Y2K_LOG ("[palette] bake desktop texture: theme=" + juce::String ((int) currentTheme)
                 + " desktopGlobal=" + desktop.toString()
                 + " cacheEntries=" + juce::String ((int) cache.size()));
        juce::Image img(juce::Image::RGB, width, height, true);
        {
            juce::Graphics gg(img);
            drawDesktop(gg, juce::Rectangle<int>(0, 0, width, height));
        }

        // LRU 容量控制：超出时弹出最旧一条
        if (cache.size() >= kMaxDesktopTextureEntries)
            cache.erase(cache.begin());

        cache.push_back({ currentTheme, width, height, kDesktopPatternVersion, img });
        return cache.back().image;
    }

    void invalidateDesktopTextureCache()
    {
        getDesktopTextureCache().clear();
    }

    // ------------------------------------------------------
    // drawLogo —— 在 area 中央绘制插件 logo 图片（由调用方提供 Image）
    //   半透明渲染；Image 由 Editor 成员缓存（避免 ImageCache 跨 DLL 悬垂）
    // ------------------------------------------------------
    void drawLogo(juce::Graphics& g, juce::Rectangle<int> area, const juce::Image& logo)
    {
        if (area.getWidth() < 80 || area.getHeight() < 80) return;
        if (logo.isNull()) return;

        // 目标尺寸：取 area 较小边的 80%，保持图片原始宽高比
        // （从 0.40 → 0.80，视觉面积变为原来的 ~4 倍，但边长刚好 2 倍，符合"放大 2 倍"的需求）
        const float targetSide = juce::jmin((float) area.getWidth(),
                                             (float) area.getHeight()) * 0.80f;

        const float imgAspect = (float) logo.getWidth() / (float) logo.getHeight();
        float drawW, drawH;
        if (imgAspect >= 1.0f) {  // 宽 >= 高
            drawW = targetSide;
            drawH = targetSide / imgAspect;
        } else {
            drawH = targetSide;
            drawW = targetSide * imgAspect;
        }

        const float cx = (float) area.getCentreX();
        const float cy = (float) area.getCentreY();
        juce::Rectangle<float> dst(cx - drawW * 0.5f, cy - drawH * 0.5f, drawW, drawH);

        // 40% 透明度绘制（避免喧宾夺主）
        g.setOpacity(0.40f);
        g.drawImage(logo, dst,
                    juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                    false);
        g.setOpacity(1.0f);
    }

    void drawTitleIconText(juce::Graphics& g,
                           juce::Rectangle<int> iconArea,
                           const juce::String& iconText,
                           juce::Colour colour,
                           juce::Colour shadowColour,
                           float fontHeight)
    {
        if (iconArea.getWidth() <= 2 || iconArea.getHeight() <= 2)
            return;

        // 图标框只有 16×14，而回退字形（♥/★ 的 advance / 字面高 ≈ 字号）
        //   在 uiScale=1 时约 16~19px，会被裁掉边上一条甚至整颗丢弃。
        //   绘制区放宽到 20×20（仍完整落在 22px 高的标题栏内；标题文字从
        //   +25 才开始，也不会与图标重叠）。
        const auto drawArea = iconArea.expanded (2, 3);

        // 静态位图缓存（mac 性能：标题栏随内容区逐帧重绘时不再逐帧塑形）；
        //   字体 = SymbolGlyphs::getFont(iconText, fontHeight, bold)，阴影 (1,1) 偏移
        drawCachedSymbolText (g, iconText, drawArea, juce::Justification::centred,
                              fontHeight, juce::Font::bold, colour,
                              StaticTextFont::symbolAuto, shadowColour.withAlpha (0.55f),
                              false);
    }

    juce::Rectangle<int> drawPinkTitleBar(juce::Graphics& g,
                                          juce::Rectangle<int> bounds,
                                          const juce::String& title,
                                          float fontHeight)
    {
        // 主题底色
        g.setColour(sel);
        g.fillRect(bounds);

        // 顶部 1px 高光 + 底部 1px 暗线 —— 旧版本这里写死了 0xffff9dbf / pink700，
        //   在非粉色主题（Starlight 蓝、Tangerine 橙、Aqua 蓝、Matcha 绿、CyberLilac 紫）
        //   下就会出现"一道粉色横线"的 bug。改为基于当前主题的 `sel` 色做亮/暗推演，
        //   保证任何主题下顶/底线都与标题栏底色同色系。
        g.setColour(sel.brighter(0.35f));
        g.fillRect(bounds.getX(), bounds.getY(), bounds.getWidth(), 1);
        g.setColour(sel.darker(0.45f));
        g.fillRect(bounds.getX(), bounds.getBottom() - 1, bounds.getWidth(), 1);

        const auto& th = getCurrentTheme();

        // 左侧主题文本图标
        auto iconRect = juce::Rectangle<int>(bounds.getX() + 5,
                                             bounds.getY() + (bounds.getHeight() - 14) / 2,
                                             16, 14);
        drawTitleIconText(g, iconRect, juce::String::fromUTF8(th.titleIconText), selInk, sel.darker(0.50f), fontHeight + 0.6f);

        // 标题文字（跟随主题 selInk）—— 静态文本位图缓存（v2.9.6 性能：
        //   组件每帧全量重绘时标题不再逐帧塑形/逐 glyph 上传，单次 quad 绘制）
        auto textRect = bounds.reduced(6, 0);
        textRect.removeFromLeft(19); // 给左侧图标留空间

        const auto titleImg = cachedStaticText (
            g, title, fontHeight, juce::Font::bold, selInk, sel.darker(0.50f).withAlpha(0.55f));

        // CJK 方块字紧贴 em 顶部、descent 很小，按字体 metrics 垂直居中后视觉偏上
        //   —— 中文界面下标题统一向下微移 1px（英文界面为 0，无变化）
        const int titleVNudge = SymbolGlyphs::cjkFontActive() ? 1 : 0;

        if (titleImg.isValid())
        {
            titleImg.drawAt (g,
                             (float) textRect.getX() - titleImg.originX,
                             (float) (textRect.getY() + (textRect.getHeight() - titleImg.getHeight()) / 2 + titleVNudge));
        }
        else
        {
            // 兜底（缓存失败）：保持旧绘制路径
            const auto shownTitle = y2k::tr (title);
            g.setFont(getFont(fontHeight, juce::Font::bold));
            g.setColour(sel.darker(0.50f).withAlpha(0.55f));
            g.drawText(shownTitle, textRect.translated(1, 1 + titleVNudge), juce::Justification::centredLeft, false);
            g.setColour(selInk);
            g.drawText(shownTitle, textRect.translated(0, titleVNudge), juce::Justification::centredLeft, false);
        }

        return bounds;
    }

    // ======================================================
    // Y2K / 像素装饰工具
    // ======================================================
    void drawScanlines(juce::Graphics& g, juce::Rectangle<int> area,
                       juce::Colour colour, float alpha)
    {
        if (area.isEmpty() || alpha <= 0.0f) return;
        g.setColour(colour.withAlpha(alpha));
        for (int y = area.getY(); y < area.getBottom(); y += 2)
            g.fillRect(area.getX(), y, area.getWidth(), 1);
    }

    void drawPixelCorners(juce::Graphics& g, juce::Rectangle<int> area,
                          juce::Colour colour, int armLen, int thickness)
    {
        if (area.getWidth() < armLen * 2 || area.getHeight() < armLen * 2) return;
        g.setColour(colour);
        const int L = armLen, T = thickness;

        // 左上 L
        g.fillRect(area.getX(), area.getY(), L, T);
        g.fillRect(area.getX(), area.getY(), T, L);
        // 右上 L
        g.fillRect(area.getRight() - L, area.getY(), L, T);
        g.fillRect(area.getRight() - T, area.getY(), T, L);
        // 左下 L
        g.fillRect(area.getX(), area.getBottom() - T, L, T);
        g.fillRect(area.getX(), area.getBottom() - L, T, L);
        // 右下 L
        g.fillRect(area.getRight() - L, area.getBottom() - T, L, T);
        g.fillRect(area.getRight() - T, area.getBottom() - L, T, L);
    }

    void drawCheckerBorder(juce::Graphics& g, juce::Rectangle<int> area,
                           juce::Colour a, juce::Colour b, int cellSize)
    {
        if (area.isEmpty() || cellSize <= 0) return;

        const int x0 = area.getX(), x1 = area.getRight();
        const int y0 = area.getY(), y1 = area.getBottom();

        auto drawRow = [&](int y, bool flip)
        {
            int i = 0;
            for (int x = x0; x < x1; x += cellSize, ++i)
            {
                const bool useA = ((i % 2) == 0) ^ flip;
                g.setColour(useA ? a : b);
                g.fillRect(x, y,
                           juce::jmin(cellSize, x1 - x),
                           juce::jmin(cellSize, y1 - y));
            }
        };
        auto drawCol = [&](int x, bool flip)
        {
            int i = 0;
            for (int y = y0; y < y1; y += cellSize, ++i)
            {
                const bool useA = ((i % 2) == 0) ^ flip;
                g.setColour(useA ? a : b);
                g.fillRect(x, y,
                           juce::jmin(cellSize, x1 - x),
                           juce::jmin(cellSize, y1 - y));
            }
        };

        drawRow(y0,             false);
        drawRow(y1 - cellSize,  true);
        drawCol(x0,             false);
        drawCol(x1 - cellSize,  true);
    }

    void drawPixelStars(juce::Graphics& g, juce::Rectangle<int> area,
                        int phase, int count, juce::Colour colour)
    {
        if (area.getWidth() < 8 || area.getHeight() < 8) return;
        // 简单 LCG 伪随机（不依赖 <random>，保证每帧可复现）
        unsigned int seed = (unsigned int)(phase * 2654435761u);
        auto nextU = [&seed]() -> unsigned int
        {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };
        for (int i = 0; i < count; ++i)
        {
            const int x = area.getX() + (int)(nextU() % (unsigned int) area.getWidth());
            const int y = area.getY() + (int)(nextU() % (unsigned int) area.getHeight());
            const int brightness = (int)(nextU() % 3u); // 0/1/2 三档亮度
            const float alpha = 0.35f + 0.25f * (float) brightness;

            g.setColour(colour.withAlpha(alpha));
            // Y2K 典型 "+" 字像素星
            g.fillRect(x,     y - 1, 1, 3);
            g.fillRect(x - 1, y,     3, 1);
        }
    }

    void drawDashedBorder(juce::Graphics& g, juce::Rectangle<int> area,
                          juce::Colour colour, int dash)
    {
        if (area.isEmpty() || dash <= 0) return;
        g.setColour(colour);
        const int x0 = area.getX(), x1 = area.getRight() - 1;
        const int y0 = area.getY(), y1 = area.getBottom() - 1;

        // 顶/底
        for (int x = x0; x < x1; x += dash * 2)
            g.fillRect(x, y0, juce::jmin(dash, x1 - x), 1);
        for (int x = x0 + dash; x < x1; x += dash * 2)
            g.fillRect(x, y1, juce::jmin(dash, x1 - x), 1);
        // 左/右
        for (int y = y0; y < y1; y += dash * 2)
            g.fillRect(x0, y, 1, juce::jmin(dash, y1 - y));
        for (int y = y0 + dash; y < y1; y += dash * 2)
            g.fillRect(x1, y, 1, juce::jmin(dash, y1 - y));
    }

    void drawY2KGradient(juce::Graphics& g, juce::Rectangle<int> area,
                         juce::Colour top, juce::Colour bottom, int bands)
    {
        if (area.isEmpty() || bands <= 0) return;
        const int h = area.getHeight();
        for (int i = 0; i < bands; ++i)
        {
            const float t = (float) i / (float) juce::jmax(1, bands - 1);
            const int y1 = area.getY() + (i * h) / bands;
            const int y2 = area.getY() + ((i + 1) * h) / bands;
            g.setColour(top.interpolatedWith(bottom, t));
            g.fillRect(area.getX(), y1, area.getWidth(), juce::jmax(1, y2 - y1));
        }
    }

    // ======================================================
    // 主题系统实现
    // ======================================================
    static const std::vector<Theme>& buildThemes()
    {
        static const std::vector<Theme> themes = {
            // ---- 🌸 Bubblegum（糖果粉，当前默认主题）----
            {
                ThemeId::bubblegum,
                "Bubblegum",
                "Sweet / Girly / Painter",
                juce::Colour(0xfffff3f9), juce::Colour(0xffffe6f1),
                juce::Colour(0xffffd1e6), juce::Colour(0xffffb8d8),
                juce::Colour(0xffff9cc9), juce::Colour(0xffff7bb3),
                juce::Colour(0xffe65e96), juce::Colour(0xffbf3f72),
                juce::Colour(0xffffffff), juce::Colour(0xffffe6f1),
                juce::Colour(0xffe65e96), juce::Colour(0xff5a1a35),
                juce::Colour(0xff3a0f24), juce::Colour(0xffff7bb3),
                juce::Colour(0xffffffff),
                juce::Colour(0xffd977ac), juce::Colour(0xffbf5a91),
                juce::Colour(0xfffffbfe),   // content (更亮粉白)
                juce::Colour(0xffffe6f1),   // btnFace (更浅奶粉)
                juce::Colour(0xffff7bb3),
                DesktopPattern::checker,
                "♡"
            },
            // ---- 🌌 Starlight（星空深蓝 + 黄小星）----
            {
                ThemeId::starlight,
                "Starlight",
                "Midnight / Wish / Stars",
                juce::Colour(0xffe7ecff), juce::Colour(0xffc8d2ff),
                juce::Colour(0xff9fb0f5), juce::Colour(0xff7083e0),
                juce::Colour(0xff4658b5), juce::Colour(0xfff8d74a), // 霓虹黄作为"强调"
                juce::Colour(0xff1c2450), juce::Colour(0xff10163a),
                juce::Colour(0xffffffff), juce::Colour(0xff2a3465),
                juce::Colour(0xff10163a), juce::Colour(0xff060824),
                juce::Colour(0xff10163a), juce::Colour(0xfff8d74a),
                juce::Colour(0xff10163a),
                juce::Colour(0xff10163a), juce::Colour(0xfff8d74a),
                juce::Colour(0xffe7ecff),   // content (淡蓝白)
                juce::Colour(0xffc8d2ff),   // btnFace (淡蓝白，深主题下按钮面)
                juce::Colour(0xff3f51b5),
                DesktopPattern::pixelStars,
                "★"
            },
            // ---- 🪩 Cyber Lilac（赛博紫 + 青）----
            {
                ThemeId::cyberLilac,
                "Cyber Lilac",
                "Cyber / e-Pet / DVD menu",
                juce::Colour(0xfff1e7ff), juce::Colour(0xffd8c2ff),
                juce::Colour(0xffb89aff), juce::Colour(0xff9870ff),
                juce::Colour(0xff8151ff), juce::Colour(0xffff45c6), // 霓虹粉
                juce::Colour(0xff5432b5), juce::Colour(0xff2e1873),
                juce::Colour(0xffffffff), juce::Colour(0xff2a1e5a),
                juce::Colour(0xff6a3cdb), juce::Colour(0xff1a0f44),
                juce::Colour(0xff2e1873), juce::Colour(0xffff45c6),
                juce::Colour(0xff1a0f44),   // selInk (\u6df1\u7d2b\uff1a\u7edf\u4e00\u6df1\u8272\u6587\u5b57\u903b\u8f91)
                juce::Colour(0xff1a0f44), juce::Colour(0xff6affd5), // \u8584\u8377\u9752\u7f51\u683c
                juce::Colour(0xfff1e7ff),   // content (淡紫白)
                juce::Colour(0xffd8c2ff),   // btnFace (淡紫白，深主题下按钮面)
                juce::Colour(0xff8151ff),
                DesktopPattern::scanGrid,
                "⌁"
            },
            // ---- 🍊 Tangerine Pop（橘色波普 + 奶油）----
            {
                ThemeId::tangerinePop,
                "Tangerine Pop",
                "2003 MSN / retro iPod",
                juce::Colour(0xfffff6e8), juce::Colour(0xffffe4bc),
                juce::Colour(0xffffc48a), juce::Colour(0xffffa85d),
                juce::Colour(0xffff8b33), juce::Colour(0xffff6a1f),
                juce::Colour(0xffc04e15), juce::Colour(0xff7a2f08),
                juce::Colour(0xffffffff), juce::Colour(0xfffff2d6),
                juce::Colour(0xffc04e15), juce::Colour(0xff4a1b05),
                juce::Colour(0xff2b1003), juce::Colour(0xffff6a1f),
                juce::Colour(0xffffffff),
                juce::Colour(0xffffb558), juce::Colour(0xfffff2d6),
                juce::Colour(0xfffff6e8),   // content (奶油白)
                juce::Colour(0xffffe4bc),   // btnFace (浅奶油按钮面)
                juce::Colour(0xffff6a1f),
                DesktopPattern::bigDots,
                "●"
            },
            // ---- 🪞 Aqua Pearl（水蓝珠光）----
            {
                ThemeId::aquaPearl,
                "Aqua Pearl",
                "Ocean / early Mac / water fae",
                juce::Colour(0xffe9f6ff), juce::Colour(0xffcae8ff),
                juce::Colour(0xff9cd1f8), juce::Colour(0xff6fb6ea),
                juce::Colour(0xff4a98d0), juce::Colour(0xff2a7aa8),
                juce::Colour(0xff1b5480), juce::Colour(0xff0d3050),
                juce::Colour(0xffffffff), juce::Colour(0xffd7eeff),
                juce::Colour(0xff1b5480), juce::Colour(0xff0a2742),
                juce::Colour(0xff061a2e), juce::Colour(0xff2a7aa8),
                juce::Colour(0xffffffff),
                juce::Colour(0xff4a98d0), juce::Colour(0xffc0d8ea), // 珍珠白
                juce::Colour(0xffe9f6ff),   // content (水白)
                juce::Colour(0xffcae8ff),   // btnFace (浅水蓝按钮面)
                juce::Colour(0xff6fb6ea),
                DesktopPattern::bubbles,
                "◎"
            },
            // ---- 🫧 Matcha Soda（苏打绿 + 粉）----
            {
                ThemeId::matchaSoda,
                "Matcha Soda",
                "Fresh / K-Y2K / stickers",
                juce::Colour(0xfff0fbe6), juce::Colour(0xffd5f1b8),
                juce::Colour(0xffb4e38a), juce::Colour(0xff8fd45c),
                juce::Colour(0xff69bf36), juce::Colour(0xffff8fb5), // 桃粉强调
                juce::Colour(0xff3c8f2c), juce::Colour(0xff245a1a),
                juce::Colour(0xffffffff), juce::Colour(0xffe6f7d4),
                juce::Colour(0xff3c8f2c), juce::Colour(0xff123e0a),
                juce::Colour(0xff0a2405), juce::Colour(0xffff8fb5),
                juce::Colour(0xffffffff),
                juce::Colour(0xff8fd45c), juce::Colour(0xfffff6e8), // 米白斜条
                juce::Colour(0xfff0fbe6),   // content (嫩绿白)
                juce::Colour(0xffd5f1b8),   // btnFace (浅嫩绿按钮面)
                juce::Colour(0xff8fd45c),
                DesktopPattern::diagStripes,
                "\\"
            },
            // ---- 🌿 Jungle（荧光绿丛林）----
            //   · 荧光霓虹绿：暗黑丛林底 + 高饱和亮绿阶 → 经典荧光绿 #39FF14
            //   · 桌面 pixelStars 模拟暗夜雨林中的萤火虫/孢子光点
            //   · btnFace 暗绿底 + 亮绿 ink，确保深色主题下按钮文字清晰可读
            {
                ThemeId::jungle,
                "Jungle",
                "Neon / Rainforest / Fluorescent",
                juce::Colour(0xff081002), juce::Colour(0xff102006), // pink50/100 近黑→深暗绿
                juce::Colour(0xff1d3810), juce::Colour(0xff2a5218), // pink200/300 暗绿→暗翠绿
                juce::Colour(0xff39ff14), juce::Colour(0xff60ff40), // pink400/500 ★荧光绿→亮柠绿
                juce::Colour(0xff80ff6a), juce::Colour(0xffa0ff90), // pink600/700 亮霓虹→浅霓虹
                juce::Colour(0xff204a0e),                           // hl（暗绿高光）
                juce::Colour(0xff0a1a04),                           // face（深暗绿按钮面）
                juce::Colour(0xff040c01),                           // shdw
                juce::Colour(0xff010301),                           // dark（几乎纯黑）
                juce::Colour(0xffc0ffa0),                           // ink（亮浅绿文字，暗底高对比）
                juce::Colour(0xff39ff14),                           // sel（荧光绿标题栏）
                juce::Colour(0xff050d02),                           // selInk（暗绿字 on 荧光绿标题）
                juce::Colour(0xff040901),                           // desktop（暗黑丛林底）
                juce::Colour(0xff39ff14),                           // desktop2（荧光绿纹理点，萤火虫感）
                juce::Colour(0xff060e02),                           // content（暗黑绿画布）
                juce::Colour(0xff122808),                           // btnFace（暗绿按钮底，与亮 ink 对比强）
                juce::Colour(0xff39ff14),                           // swatch（荧光绿色票）
                DesktopPattern::pixelStars,
                "\xe2\x9c\xa6" // ✦（荧光星芒）
            },
            // ---- 🪟 Windows XP Luna（经典 Bliss 蓝天绿草）----
            //   pink50..pink700 在此主题下被"重新解读"为一组 Luna 蓝色阶
            //   （从极浅 sky → 经典 Luna 深蓝），所有使用 pink* 色的 UI
            //   控件（EQ 格子、Meter、曲线等）自动获得 Luna 蓝配色。
            //   桌面纹理用 bigDots 模拟 XP 经典 Bliss 天空蓝的波点。
            {
                ThemeId::winXP,
                "Win XP Luna",
                "Bliss / taskbar / Clearlooks",
                juce::Colour(0xffeaf4ff), juce::Colour(0xffcfe4ff), // pink50/100 淡天蓝
                juce::Colour(0xffa8cdf0), juce::Colour(0xff7db4e3),
                juce::Colour(0xff4a8ed4), juce::Colour(0xff2d6fcd), // pink400/500 Luna 蓝
                juce::Colour(0xff17549c), juce::Colour(0xff0d3b7a),
                juce::Colour(0xffffffff),                           // hl
                juce::Colour(0xffe6effa),                           // face (XP 任务栏浅蓝灰)
                juce::Colour(0xff486fad),                           // shdw
                juce::Colour(0xff0a2a5e),                           // dark
                juce::Colour(0xff0a1e3a),                           // ink
                juce::Colour(0xff0a66b0),                           // sel (XP 标题栏深 Luna 蓝)
                juce::Colour(0xffffffff),                           // selInk
                juce::Colour(0xff3a649c),                           // desktop (天空蓝)
                juce::Colour(0xff6e97c8),                           // desktop2 (淡天空蓝)
                juce::Colour(0xfff7fbff),                           // content (near-white 画布)
                juce::Colour(0xffe6effa),                           // btnFace (XP 按钮灰蓝)
                juce::Colour(0xff2d6fcd),                           // swatch (色票 Luna 蓝)
                DesktopPattern::bigDots,
                "\xe2\x96\xa0" // ■（实心方块作为"开始按钮"风图标）
            },
            // ---- 🩸 Crimson Noir（红黑暗夜）----
            //   · pink50..pink700 重新解读为"带淡红调的亮灰阶"（暗→亮），
            //     让仪表图线条（oscilloscope/spectrum 等常用 pink300/400/500）
            //     在黑底上清晰可见；红色气质改由 sel/desktop 承担
            //   · desktopPattern 选 diagStripes：黑底斜纹，经典暗夜感
            {
                ThemeId::crimsonNoir,
                "Crimson Noir",
                "Gothic / Vampire / Late-night mix",
                juce::Colour(0xff3a2a2a), juce::Colour(0xff5a4040), // pink50/100  深灰带红
                juce::Colour(0xff7f6060), juce::Colour(0xffa68888), // pink200/300 中灰带红
                juce::Colour(0xffc9b2b2), juce::Colour(0xffe4d4d4), // pink400/500 亮灰（仪表主线）
                juce::Colour(0xfff2e8e8), juce::Colour(0xfffaf5f5), // pink600/700 近白
                juce::Colour(0xff6d2b2b),                           // hl（高光：暗红）
                juce::Colour(0xff2a0f0f),                           // face（按钮面：近黑深红）
                juce::Colour(0xff1a0606),                           // shdw
                juce::Colour(0xff000000),                           // dark（最外层：黑）
                juce::Colour(0xfff7dada),                           // ink（浅血红文字，在暗底上可读）
                juce::Colour(0xff8a1a1a),                           // sel（标题栏深血红）
                juce::Colour(0xfffff0f0),                           // selInk
                juce::Colour(0xff0a0303),                           // desktop（近纯黑）
                juce::Colour(0xff2a0808),                           // desktop2（暗红纹理）
                juce::Colour(0xff120505),                           // content（画布：深黑红）
                juce::Colour(0xff2a0f0f),                           // btnFace（按钮底）
                juce::Colour(0xff8a1a1a),                           // swatch（色票：血红）
                DesktopPattern::diagStripes,
                "\xe2\x97\x86" // ◆（深色菱形）
            },
            // ---- ⬛ Void Grey（纯黑灰）----
            //   · OLED 友好的极简暗色：纯黑背景 + 中性灰阶 + 微弱白色点缀
            //   · 适合长时间监听 / 深夜工作；不带任何强调色
            //   · pink 色阶按"暗→亮"排列，仪表图线用 pink300/400/500 在黑底上如同白线
            {
                ThemeId::voidGrey,
                "Void Grey",
                "Pure dark / OLED / minimal",
                juce::Colour(0xff2a2a2a), juce::Colour(0xff4a4a4a), // pink50/100  深灰
                juce::Colour(0xff707070), juce::Colour(0xff9a9a9a), // pink200/300 中灰→中亮灰
                juce::Colour(0xffc2c2c2), juce::Colour(0xffe0e0e0), // pink400/500 亮灰（仪表主线）
                juce::Colour(0xfff0f0f0), juce::Colour(0xfffafafa), // pink600/700 近白
                juce::Colour(0xff505050),                           // hl
                juce::Colour(0xff2a2a2a),                           // face
                juce::Colour(0xff1a1a1a),                           // shdw
                juce::Colour(0xff000000),                           // dark（纯黑）
                juce::Colour(0xffe6e6e6),                           // ink（浅灰文字）
                juce::Colour(0xff3d3d3d),                           // sel（标题栏中灰）
                juce::Colour(0xffffffff),                           // selInk
                juce::Colour(0xff000000),                           // desktop（纯黑桌面）
                juce::Colour(0xff1a1a1a),                           // desktop2
                juce::Colour(0xff181818),                           // content（画布：接近纯黑）
                juce::Colour(0xff2a2a2a),                           // btnFace
                juce::Colour(0xff5a5a5a),                           // swatch（色票：中灰）
                DesktopPattern::scanGrid,
                "\xe2\x97\x8f" // ●（黑色实心圆）
            },
            // ---- ⬜ Paper Grey（纯白灰）----
            //   · 纸面极简：纯白背景 + 浅灰阶 + 深灰文字
            //   · 亮色模式、打印机友好、清晰易读
            {
                ThemeId::paperGrey,
                "Paper Grey",
                "Pure light / paper / minimal",
                juce::Colour(0xfffafafa), juce::Colour(0xfff0f0f0), // pink50/100 近白
                juce::Colour(0xffe0e0e0), juce::Colour(0xffc8c8c8),
                juce::Colour(0xffa8a8a8), juce::Colour(0xff7d7d7d), // pink400/500 中灰
                juce::Colour(0xff5a5a5a), juce::Colour(0xff333333),
                juce::Colour(0xffffffff),                           // hl
                juce::Colour(0xffeeeeee),                           // face（浅灰按钮面）
                juce::Colour(0xffbdbdbd),                           // shdw
                juce::Colour(0xff6a6a6a),                           // dark
                juce::Colour(0xff1a1a1a),                           // ink（深灰文字）
                juce::Colour(0xff7d7d7d),                           // sel（中灰标题栏）
                juce::Colour(0xffffffff),                           // selInk
                juce::Colour(0xffe8e8e8),                           // desktop（浅灰桌面）
                juce::Colour(0xfff5f5f5),                           // desktop2
                juce::Colour(0xffffffff),                           // content（画布：纯白）
                juce::Colour(0xffeeeeee),                           // btnFace
                juce::Colour(0xff7d7d7d),                           // swatch（色票：中灰）
                DesktopPattern::checker,
                "\xe2\x97\x8b" // ○（空心圆）
            },
            // ---- 🖤💗 Black Pink（黑粉 Y2K）----
            //   · K-pop / 千禧年复古：纯黑主基调 + 亮粉强调色
            //   · desktop/content/face 全部使用暗黑色系，pink50→300 为暗粉→中粉过渡，
            //     pink400→700 为鲜艳粉→浅粉，供图表线条逐级高亮。
            //   · sel（标题栏）用签名亮粉 #ec4d85，搭配白色标题文字，黑底高对比。
            //   · ink 用浅粉白 #ffe6f0，在深色画布上清晰可读且保持粉色气质。
            //   · btnFace 用暗灰 #1e1e1e，保证按钮/控件在黑底上可见但不刺眼。
            //   · desktopPattern 用 checker 经典棋盘格，保持千禧年 XP 感。
            {
                ThemeId::blackPink,
                "Black Pink",
                "K-pop / Y2K / Dark + Neon pink",
                juce::Colour(0xff220a14), juce::Colour(0xff3a1428), // pink50/100  近黑深粉（提亮）
                juce::Colour(0xff5a203a), juce::Colour(0xff8a3058), // pink200/300 暗粉→玫瑰粉（提亮）
                juce::Colour(0xfff0508a), juce::Colour(0xffff78aa), // pink400/500 亮粉（Y2K 签名色，更亮）
                juce::Colour(0xffff96c0), juce::Colour(0xffffbddd), // pink600/700 浅粉→极浅粉（提亮）
                juce::Colour(0xff2e2e2e),                           // hl（暗灰高光，克制）
                juce::Colour(0xff1a1a1a),                           // face（深黑灰按钮面）
                juce::Colour(0xff0a0a0a),                           // shdw（近黑阴影）
                juce::Colour(0xff000000),                           // dark（纯黑边框）
                juce::Colour(0xffffe6f0),                           // ink（浅粉白文字，暗底高对比）
                juce::Colour(0xfff0508a),                           // sel（标题栏：亮粉，与 pink400 同步提亮）
                juce::Colour(0xff050d02),                           // selInk（深色文字 on 亮粉标题栏，复刻 Jungle 黑字风格）
                juce::Colour(0xff0e0e0e),                           // desktop（暗黑底，较之前调亮以显示纹理）
                juce::Colour(0xff4d1b2f),                           // desktop2（更深的爱心颜色，与底色形成微弱反差）
                juce::Colour(0xff121212),                           // content（深灰黑画布）
                juce::Colour(0xff1e1e1e),                           // btnFace（暗灰按钮面）
                juce::Colour(0xffec4d85),                           // swatch（色票：亮粉）
                DesktopPattern::hearts,
                "\xe2\x99\xa5" // ♥（黑桃心，千禧年符号）
            },
        };
        return themes;
    }

    const std::vector<Theme>& getAllThemes()
    {
        return buildThemes();
    }

    // 当前主题状态
    static ThemeId gCurrentThemeId = ThemeId::blackPink;

    // 主题变更回调表
    struct ThemeSub { int token; ThemeChangedCallback cb; };
    static std::vector<ThemeSub> gThemeSubs;
    static int gNextThemeSubToken = 1;

    // 自定义主题状态
    static juce::Colour gCustomPrimary { 0xffec4d85 };     // 默认粉色
    static juce::Colour gCustomSecondary { 0xffffffff };    // 默认白色
    // 调色板重构：生成器主题的原始 ThemeSpec + 动态显示名
    static y2kpal::ThemeSpec gCustomSpec;
    static juce::String gCustomThemeName;
    static Theme gCustomTheme;                              // 运行时动态构建的完整Theme

    juce::Colour getCustomPrimary()  { return gCustomPrimary; }
    juce::Colour getCustomSecondary() { return gCustomSecondary; }
    bool isCustomThemeActive() { return gCurrentThemeId == ThemeId::custom; }

    ThemeId getCurrentThemeId() { return gCurrentThemeId; }

    const Theme& getCurrentTheme()
    {
        if (gCurrentThemeId == ThemeId::custom)
            return gCustomTheme;
        const auto& all = getAllThemes();
        for (const auto& t : all)
            if (t.id == gCurrentThemeId)
                return t;
        return all.front();
    }

    // 把 Theme 的全部槽位写入全局调色板并通知订阅者
    //   （applyTheme / applyThemeFromSpec 共用，保证写入 + 通知流程一致）
    void commitTheme(const Theme& t)
    {
        pink50 = t.pink50;   pink100 = t.pink100; pink200 = t.pink200; pink300 = t.pink300;
        pink400 = t.pink400; pink500 = t.pink500; pink600 = t.pink600; pink700 = t.pink700;

        hl  = t.hl;   face = t.face; shdw = t.shdw; dark = t.dark;
        ink = t.ink;  sel  = t.sel;  selInk = t.selInk;
        desktop  = t.desktop;
        desktop2 = t.desktop2;
        content  = t.content;
        btnFace  = t.btnFace;

        // 调色板重构扩展槽位（S3）：预设主题未填时回退到 pink 阶梯/占位色
        accent2 = t.accent2.isTransparent() ? t.swatch : t.accent2;
        danger  = t.danger.isTransparent()  ? sel : t.danger;
        warn    = t.warn.isTransparent()    ? pink500 : t.warn;
        ok      = t.ok.isTransparent()      ? pink200 : t.ok;

        // 预设主题无 chartRamp 数据（默认透明）→ 回退 pink50..pink700 阶梯
        if (t.chartRamp[0].isTransparent())
        {
            chartRamp[0] = t.pink50;  chartRamp[1] = t.pink100;
            chartRamp[2] = t.pink200; chartRamp[3] = t.pink300;
            chartRamp[4] = t.pink400; chartRamp[5] = t.pink500;
            chartRamp[6] = t.pink600; chartRamp[7] = t.pink700;
        }
        else
        {
            for (int i = 0; i < 8; ++i)
                chartRamp[i] = t.chartRamp[(size_t) i];
        }

        // 刷新 LookAndFeel 内部 ColourScheme 缓存 —— 让 ComboBox 的
        //   arrowColourId/textColourId 等通过 findColour() 读取的控件颜色
        //   立刻跟随新主题（否则会保留构造时的旧配色）。
        //   注：ComboBox 内嵌 Label 的显式颜色残留由 PalettePanel 在
        //   应用主题后单独刷新（全局 sendLookAndFeelChange 会递归整棵
        //   组件树，拖动滑条高频触发时是明显的卡顿源，不能放在这里）。
        syncPinkXPColours (getPinkXPLookAndFeel());

        // P6: 主题切换时让"跨实例共享桌面纹理"全部失效，下一帧各 Editor
        //     paint 会拿到 key=(newThemeId,size) 的全新共享 Image。
        invalidateDesktopTextureCache();

        notifyThemeSubscribers();
    }

    void notifyThemeSubscribers()
    {
        // 通知所有订阅者。每个回调单独 try/catch 隔离：任一订阅者抛异常
        //   都不能中断后续订阅者（Editor 的桌面缓存失效回调排在队列末尾，
        //   若被前面模块的异常截断，桌面纹理就会永远画旧缓存图）。
        //   按 token 逐个回查再调用：回调里若有订阅者被退订（模块析构），
        //   不会再调到已销毁对象的回调。
        std::vector<int> tokens;
        tokens.reserve (gThemeSubs.size());
        for (auto& s : gThemeSubs)
            tokens.push_back (s.token);

        for (const int token : tokens)
        {
            ThemeChangedCallback cb;
            for (auto& live : gThemeSubs)
                if (live.token == token) { cb = live.cb; break; }

            ThemeSub s { token, std::move (cb) };
            if (!s.cb)
                continue;
            try
            {
                s.cb();
            }
            catch (const std::exception& e)
            {
                Y2K_LOG ("[palette] theme sub callback #" + juce::String (s.token)
                         + " threw: " + e.what());
            }
            catch (...)
            {
                Y2K_LOG ("[palette] theme sub callback #" + juce::String (s.token)
                         + " threw (unknown)");
            }
        }
    }

    void applyTheme(ThemeId id)
    {
        gCurrentThemeId = id;
        commitTheme(getCurrentTheme());
    }

    int subscribeThemeChanged(ThemeChangedCallback cb)
    {
        const int token = gNextThemeSubToken++;
        gThemeSubs.push_back({ token, std::move(cb) });
        return token;
    }

    void unsubscribeThemeChanged(int token)
    {
        for (auto it = gThemeSubs.begin(); it != gThemeSubs.end(); ++it)
        {
            if (it->token == token) { gThemeSubs.erase(it); return; }
        }
    }

    // DesktopPattern ↔ y2kpal::Pattern（ThemeSpec 存储的是生成器枚举）
    DesktopPattern patternFromSpec(y2kpal::Pattern p)
    {
        switch (p)
        {
            case y2kpal::Pattern::PixelStars:  return DesktopPattern::pixelStars;
            case y2kpal::Pattern::ScanGrid:    return DesktopPattern::scanGrid;
            case y2kpal::Pattern::BigDots:     return DesktopPattern::bigDots;
            case y2kpal::Pattern::Bubbles:     return DesktopPattern::bubbles;
            case y2kpal::Pattern::DiagStripes: return DesktopPattern::diagStripes;
            case y2kpal::Pattern::Hearts:      return DesktopPattern::hearts;
            case y2kpal::Pattern::Zigzag:      return DesktopPattern::zigzag;
            case y2kpal::Pattern::CrossHatch:  return DesktopPattern::crossHatch;
            case y2kpal::Pattern::PixelFlower: return DesktopPattern::pixelFlower;
            case y2kpal::Pattern::Checker:
            default:                           return DesktopPattern::checker;
        }
    }

    y2kpal::Pattern patternFromSpecInv(DesktopPattern p)
    {
        switch (p)
        {
            case DesktopPattern::pixelStars:  return y2kpal::Pattern::PixelStars;
            case DesktopPattern::scanGrid:    return y2kpal::Pattern::ScanGrid;
            case DesktopPattern::bigDots:     return y2kpal::Pattern::BigDots;
            case DesktopPattern::bubbles:     return y2kpal::Pattern::Bubbles;
            case DesktopPattern::diagStripes: return y2kpal::Pattern::DiagStripes;
            case DesktopPattern::hearts:      return y2kpal::Pattern::Hearts;
            case DesktopPattern::zigzag:      return y2kpal::Pattern::Zigzag;
            case DesktopPattern::crossHatch:  return y2kpal::Pattern::CrossHatch;
            case DesktopPattern::pixelFlower: return y2kpal::Pattern::PixelFlower;
            case DesktopPattern::checker:
            default:                          return y2kpal::Pattern::Checker;
        }
    }

    void applyCustomTheme(juce::Colour primary, juce::Colour secondary)
    {
        gCustomPrimary   = primary;
        gCustomSecondary = secondary;

        // v2.10 调色板重构（R20 迁移）：双色 fit 成 ThemeSpec → OKLCH 生成器。
        //   accent/background 锁定为旧双色（视觉接近旧外观），其余角色由
        //   配方生成并受护栏保护（允许因质量提升略有差异）。
        y2kpal::ThemeSpec spec;
        y2kpal::fitSpecFromThemeColors(
            { primary.getRed(),   primary.getGreen(),   primary.getBlue()   },
            { secondary.getRed(), secondary.getGreen(), secondary.getBlue() },
            spec);
        applyThemeFromSpec(spec, "Custom (migrated)");
    }

    // ==================================================
    // 调色板重构（S2）：ThemeSpec → PaletteGenerator → Theme
    // ==================================================
    void applyThemeFromSpec(const y2kpal::ThemeSpec& spec, const juce::String& displayName)
    {
        gCustomSpec = spec;
        gCustomThemeName = displayName.isNotEmpty() ? displayName : juce::String("Custom");

        const y2kpal::GeneratedTheme gen = y2kpal::generate(spec);

        // 诊断日志（RelWithDebInfo）：桌面底色不随明暗切换的问题定位。
        //   bg/surface/control 的角色锁定会让 desktop/content/btnFace 槽位
        //   恒等于锁定色 —— mode 切换自然"看起来无效"。
        Y2K_LOG ("[palette] applyThemeFromSpec: bright=" + juce::String ((int) spec.brightness)
                 + " hue=" + juce::String (spec.hue, 1)
                 + " vibe=" + juce::String (spec.vibe, 2)
                 + " bgLock=" + (spec.overrides[(int) y2kpal::Role::Background].set ? "YES" : "no")
                 + " sfLock=" + (spec.overrides[(int) y2kpal::Role::Surface].set ? "YES" : "no")
                 + " ctlLock=" + (spec.overrides[(int) y2kpal::Role::Control].set ? "YES" : "no")
                 + " desktop=" + [&gen]() {
                       auto byte = [](uint8_t v) {
                           const juce::String s = juce::String::toHexString ((int) v);
                           return (s.length() < 2 ? "0" : "") + s;
                       };
                       return "#" + byte (gen.desktop.r) + byte (gen.desktop.g)
                            + byte (gen.desktop.b);
                   }());
        const auto toColour = [](y2kpal::Rgb8 c)
        {
            return juce::Colour(c.r, c.g, c.b);
        };

        // 标题图标候选（R14：5-8 个，与预设图标风格一致）
        static const char* const kTitleIcons[] = { "♡", "★", "◆", "●", "✦", "⌁", "♥", "▲" };
        const char* icon = kTitleIcons[spec.titleIcon % 8];

        // 逐字段赋值（MSVC 对含 std::array 成员的嵌套聚合初始化较挑剔）
        Theme t;
        t.id          = ThemeId::custom;
        t.displayName = gCustomThemeName;
        t.keywordHint = "Custom palette";
        t.pink50 = toColour(gen.pink[0]); t.pink100 = toColour(gen.pink[1]);
        t.pink200 = toColour(gen.pink[2]); t.pink300 = toColour(gen.pink[3]);
        t.pink400 = toColour(gen.pink[4]); t.pink500 = toColour(gen.pink[5]);
        t.pink600 = toColour(gen.pink[6]); t.pink700 = toColour(gen.pink[7]);
        t.hl = toColour(gen.hl);        t.face = toColour(gen.face);
        t.shdw = toColour(gen.shdw);    t.dark = toColour(gen.dark);
        t.ink = toColour(gen.ink);      t.sel = toColour(gen.sel);
        t.selInk = toColour(gen.selInk);
        t.desktop = toColour(gen.desktop); t.desktop2 = toColour(gen.desktop2);
        t.content = toColour(gen.content); t.btnFace = toColour(gen.btnFace);
        t.swatch = toColour(gen.accent);
        t.desktopPattern = patternFromSpec(spec.pattern);
        t.titleIconText = icon;
        t.accent2 = toColour(gen.accent2);
        t.danger = toColour(gen.danger);
        t.warn   = toColour(gen.warn);
        t.ok     = toColour(gen.ok);
        for (int i = 0; i < 8; ++i)
            t.chartRamp[(size_t) i] = toColour(gen.chartRamp[(size_t) i]);

        gCustomTheme = t;

        // 预览方块/色票条使用的双色快照
        gCustomPrimary   = gCustomTheme.sel;
        gCustomSecondary = gCustomTheme.desktop;

        gCurrentThemeId = ThemeId::custom;
        commitTheme(gCustomTheme);
    }

    y2kpal::ThemeSpec getCurrentThemeSpec()
    {
        if (gCurrentThemeId == ThemeId::custom)
            return gCustomSpec;

        // 预设主题：从 Accent(swatch)/Background(desktop) 反推意图参数
        //   （R10"基于预设新建"入口）。清除 fit 产生的覆盖锁定，
        //   保证色相环/氛围滑条等意图控件立即可用。
        const Theme& t = getCurrentTheme();
        y2kpal::ThemeSpec spec;
        y2kpal::fitSpecFromThemeColors(
            { t.swatch.getRed(),   t.swatch.getGreen(),   t.swatch.getBlue()   },
            { t.desktop.getRed(),  t.desktop.getGreen(),  t.desktop.getBlue()  },
            spec);
        for (auto& ov : spec.overrides)
            ov.set = false;
        spec.pattern = patternFromSpecInv(t.desktopPattern);
        // 氛围/染色从中间值起步（fit 反推会把它们算到接近 0，用户期望默认居中）
        spec.vibe   = 1.0f;
        spec.bgTint = 1.0f;
        return spec;
    }

    void applyUserTheme(const y2kpal::ThemeSpec& spec, const juce::String& name)
    {
        applyThemeFromSpec(spec, name);
    }

    juce::String getCustomThemeName()
    {
        return gCustomThemeName;
    }

    // ==================================================
    // 图表渐变采样（A3/S4）：OKLab 空间插值替代 sRGB 线性插值
    // ==================================================
    juce::Colour sampleChartRamp(float t)
    {
        t = juce::jlimit(0.0f, 1.0f, t);
        const float pos = t * 7.0f;
        const int   i = juce::jmin(6, (int) pos);
        const float u = pos - (float) i;
        return interpolateOkLab(chartRamp[i], chartRamp[i + 1], u);
    }

    juce::Colour interpolateOkLab(juce::Colour a, juce::Colour b, float t)
    {
        t = juce::jlimit(0.0f, 1.0f, t);
        const y2kpal::Rgb8 c = y2kpal::interpolateOkLab(
            { a.getRed(), a.getGreen(), a.getBlue() },
            { b.getRed(), b.getGreen(), b.getBlue() },
            t);
        return juce::Colour(c.r, c.g, c.b);
    }
}

// ==========================================================
// PinkXPLookAndFeel
// ==========================================================
namespace
{
    // 将当前 PinkXP 全局调色板（pink*/ink/btnFace/sel/selInk 等）同步写入
    // 指定 LookAndFeel 的 ColourScheme 缓存里。
    //   · 构造时调用一次：建立初始配色
    //   · applyTheme 切主题后再调一次：更新缓存，让通过 findColour() 取色的
    //     控件（ComboBox 的文字/箭头、PopupMenu、Slider textbox 等）立刻跟随
    //     新主题，无需各处重新 setColour。
    void syncPinkXPColours (juce::LookAndFeel& lf)
    {
        lf.setColour (juce::Slider::backgroundColourId,        PinkXP::btnFace);
        lf.setColour (juce::Slider::trackColourId,             PinkXP::sel);
        lf.setColour (juce::Slider::thumbColourId,             PinkXP::btnFace);
        lf.setColour (juce::Slider::textBoxTextColourId,       PinkXP::ink);
        lf.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
        lf.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);

        lf.setColour (juce::Label::textColourId,               PinkXP::ink);
        lf.setColour (juce::Label::backgroundColourId,         juce::Colours::transparentBlack);

        lf.setColour (juce::TextButton::buttonColourId,        PinkXP::btnFace);
        lf.setColour (juce::TextButton::buttonOnColourId,      PinkXP::pink200);
        lf.setColour (juce::TextButton::textColourOffId,       PinkXP::ink);
        lf.setColour (juce::TextButton::textColourOnId,        PinkXP::ink);

        lf.setColour (juce::PopupMenu::backgroundColourId,            PinkXP::btnFace);
        lf.setColour (juce::PopupMenu::textColourId,                  PinkXP::ink);
        lf.setColour (juce::PopupMenu::highlightedBackgroundColourId, PinkXP::sel);
        lf.setColour (juce::PopupMenu::highlightedTextColourId,       PinkXP::selInk);

        // ComboBox —— 跟随 Pink XP 主题（消除默认的 LookAndFeel_V4 浅蓝配色）
        lf.setColour (juce::ComboBox::backgroundColourId,     PinkXP::btnFace);
        lf.setColour (juce::ComboBox::textColourId,           PinkXP::ink);
        lf.setColour (juce::ComboBox::outlineColourId,        juce::Colours::transparentBlack);
        lf.setColour (juce::ComboBox::buttonColourId,         PinkXP::btnFace);
        lf.setColour (juce::ComboBox::arrowColourId,          PinkXP::ink);
        lf.setColour (juce::ComboBox::focusedOutlineColourId, juce::Colours::transparentBlack);
    }
}

PinkXPLookAndFeel::PinkXPLookAndFeel()
{
    syncPinkXPColours (*this);
}

void PinkXPLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                             const juce::Colour&, bool, bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds();
    const bool toggled = button.getToggleState();
    const bool pressed = shouldDrawButtonAsDown || toggled;

    if (pressed)
    {
        PinkXP::drawPressed(g, bounds, PinkXP::pink100);
        g.setColour(PinkXP::pink200);
        for (int py = bounds.getY() + 3; py < bounds.getBottom() - 2; py += 4)
            for (int px = bounds.getX() + 3 + ((py / 4) % 2) * 2; px < bounds.getRight() - 2; px += 4)
                g.fillRect(px, py, 2, 2);
    }
    else
    {
        PinkXP::drawRaised(g, bounds, PinkXP::btnFace);
    }
}

void PinkXPLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                       bool, bool shouldDrawButtonAsDown)
{
    // 组件显式覆盖的文字色优先（RoleRow 的 Auto 开关等需要自适应对比）；
    // 未覆盖时回退 PinkXP::ink（与历史行为一致）。TextButton 的文字色 id
    // 是 textColourOffId / textColourOnId（按下/选中态用 On）。
    const bool toggled = shouldDrawButtonAsDown || button.getToggleState();
    const juce::Colour tc = button.findColour(toggled ? juce::TextButton::textColourOnId
                                                      : juce::TextButton::textColourOffId);
    const juce::Colour textColour = tc.isOpaque() ? tc : PinkXP::ink;

    const juce::String shown = y2k::tr (button.getButtonText());

    // 纯符号文本（如关闭 ×）恒用英文 UI 字体 iconFont：字号不受 CJK 界面
    //   的 0.9 收紧影响，中英文界面字形/大小完全一致。
    //   混排文本（如「高级 ▸」）由 drawText 拆 run：CJK 留中文字体，
    //   ▸ 用英文字体 —— 整串换字体会让中文消失。
    const bool symbolsOnly = PinkXP::SymbolGlyphs::isSymbolsOnly (shown);

    auto area = button.getLocalBounds();
    if (shouldDrawButtonAsDown || button.getToggleState())
        area.translate(1, 1);

    // 静态位图缓存（mac 性能）：与 SymbolGlyphs::drawText(shown, area, centred, baseFont) 等价
    PinkXP::drawCachedSymbolText (g, shown, area, juce::Justification::centred,
                                  symbolsOnly ? 14.0f : 12.0f, juce::Font::bold, textColour,
                                  symbolsOnly ? PinkXP::StaticTextFont::icon
                                              : PinkXP::StaticTextFont::button,
                                  juce::Colours::transparentBlack, false);
}

juce::Font PinkXPLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return PinkXP::getFont(juce::jmin(14.0f, buttonHeight * 0.55f), juce::Font::bold);
}

int PinkXPLookAndFeel::getTextButtonWidthToFitText (juce::TextButton& b, int buttonHeight)
{
    // 测量与 drawButtonText 用同一套"按文本选字体 + run 化"，避免混排宽度不一致
    const juce::String shown = y2k::tr (b.getButtonText());
    const juce::Font font = PinkXP::SymbolGlyphs::isSymbolsOnly (shown)
        ? PinkXP::SymbolGlyphs::iconFont (14.0f, juce::Font::bold)
        : PinkXP::SymbolGlyphs::buttonFont (shown,
                                            juce::jmin (14.0f, buttonHeight * 0.55f),
                                            juce::Font::bold);
    return (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (shown, font)) + buttonHeight;
}

void PinkXPLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h,
                                         float sliderPos, float minSliderPos, float maxSliderPos,
                                         const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    juce::ignoreUnused(slider);

    const bool isVertical = (style == juce::Slider::LinearVertical
                             || style == juce::Slider::TwoValueVertical);

    juce::Rectangle<int> track;
    if (isVertical)
    {
        const int tw = 8;
        track = juce::Rectangle<int>(x + (w - tw) / 2, y, tw, h);
    }
    else
    {
        const int th = 8;
        track = juce::Rectangle<int>(x, y + (h - th) / 2, w, th);
    }
    PinkXP::drawSunken(g, track, juce::Colours::white);

    const int thumbW = isVertical ? juce::jmin(w - 4, 18) : 12;
    const int thumbH = isVertical ? 12 : juce::jmin(h - 4, 18);

    const bool isTwoValue = (style == juce::Slider::TwoValueHorizontal
                             || style == juce::Slider::TwoValueVertical);

    if (isTwoValue)
    {
        // 双滑块：填充 min↔max 之间的选中区，并绘制两个 thumb。
        //   JUCE 对 TwoValue 只调用一次本函数，两个 thumb 位置分别由
        //   minSliderPos / maxSliderPos 传入（sliderPos 仅是当前被拖动的 thumb）。
        if (isVertical)
        {
            // 垂直方向：value 越大 thumb 越靠上，故 max 在上、min 在下
            const int top    = juce::jlimit(track.getY() + 2, track.getBottom() - 2, (int) maxSliderPos);
            const int bottom = juce::jlimit(track.getY() + 2, track.getBottom() - 2, (int) minSliderPos);
            g.setColour(PinkXP::sel);
            g.fillRect(track.getX() + 2, top, track.getWidth() - 4, juce::jmax(0, bottom - top));
        }
        else
        {
            const int left  = juce::jlimit(track.getX() + 2, track.getRight() - 2, (int) minSliderPos);
            const int right = juce::jlimit(track.getX() + 2, track.getRight() - 2, (int) maxSliderPos);
            g.setColour(PinkXP::sel);
            g.fillRect(left, track.getY() + 2, juce::jmax(0, right - left), track.getHeight() - 4);
        }

        auto drawThumb = [&](float pos)
        {
            juce::Rectangle<int> thumb;
            if (isVertical)
                thumb = juce::Rectangle<int>(x + (w - thumbW) / 2, (int) pos - thumbH / 2, thumbW, thumbH);
            else
                thumb = juce::Rectangle<int>((int) pos - thumbW / 2, y + (h - thumbH) / 2, thumbW, thumbH);
            PinkXP::drawRaised(g, thumb, PinkXP::btnFace);
        };
        drawThumb(minSliderPos);
        drawThumb(maxSliderPos);
        return;
    }

    // ---- 单值滑块（保持原逻辑）----
    if (isVertical)
    {
        int fillTop = juce::jlimit(track.getY() + 2, track.getBottom() - 2, (int) sliderPos);
        g.setColour(PinkXP::sel);
        g.fillRect(track.getX() + 2, fillTop, track.getWidth() - 4, track.getBottom() - 2 - fillTop);
    }
    else
    {
        int fillRight = juce::jlimit(track.getX() + 2, track.getRight() - 2, (int) sliderPos);
        g.setColour(PinkXP::sel);
        g.fillRect(track.getX() + 2, track.getY() + 2, fillRight - (track.getX() + 2), track.getHeight() - 4);
    }

    juce::Rectangle<int> thumb;
    if (isVertical)
        thumb = juce::Rectangle<int>(x + (w - thumbW) / 2, (int) sliderPos - thumbH / 2, thumbW, thumbH);
    else
        thumb = juce::Rectangle<int>((int) sliderPos - thumbW / 2, y + (h - thumbH) / 2, thumbW, thumbH);

    PinkXP::drawRaised(g, thumb, PinkXP::btnFace);
}

juce::Font PinkXPLookAndFeel::getLabelFont(juce::Label&)
{
    return PinkXP::getFont(11.0f, juce::Font::bold);
}

// ----------------------------------------------------------
// Label：与 LookAndFeel_V2::drawLabel 同构（字体同样取 getLabelFont —— 每次
//   绘制现取，自动跟随当前界面语言字体），只多一步：文本按界面语言翻译
//   （Label 内部仍保存英文源文案，比较 / 持久化逻辑不受影响）。
// ----------------------------------------------------------
void PinkXPLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    g.fillAll (label.findColour (juce::Label::backgroundColourId));

    if (! label.isBeingEdited())
    {
        const float alpha = label.isEnabled() ? 1.0f : 0.5f;
        const juce::String shown = y2k::tr (label.getText());
        const juce::Font baseFont = getLabelFont (label);

        g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));

        const auto textArea = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());

        // 只有"含装饰符号"的文本才走 run 化绘制（CJK + 符号混排时符号换英文
        //   字体，CJK 留中文字体）；其余文本保持 drawFittedText 的原有缩放 /
        //   多行行为，避免影响既有布局。
        const auto runs = PinkXP::SymbolGlyphs::splitRuns (shown);
        const bool needsRunLayout = runs.size() > 1 || (runs.size() == 1 && runs[0].symbol);

        if (needsRunLayout && ! shown.containsChar ('\n'))
        {
            PinkXP::SymbolGlyphs::drawText (g, shown, textArea,
                                            label.getJustificationType(), baseFont);
        }
        else
        {
            const juce::Font font (PinkXP::SymbolGlyphs::rebase (baseFont, shown));
            g.setFont (font);
            g.drawFittedText (shown, textArea, label.getJustificationType(),
                              juce::jmax (1, (int) ((float) textArea.getHeight() / font.getHeight())),
                              label.getMinimumHorizontalScale());
        }

        g.setColour (label.findColour (juce::Label::outlineColourId).withMultipliedAlpha (alpha));
    }
    else if (label.isEnabled())
    {
        g.setColour (label.findColour (juce::Label::outlineColourId));
    }

    g.drawRect (label.getLocalBounds());
}

void PinkXPLookAndFeel::drawComboBoxTextWhenNothingSelected (juce::Graphics& g, juce::ComboBox& box,
                                                              juce::Label& label)
{
    g.setColour (findColour (juce::ComboBox::textColourId).withMultipliedAlpha (0.5f));

    const juce::String shown = y2k::tr (box.getTextWhenNothingSelected());
    const auto baseFont = label.getLookAndFeel().getLabelFont (label);

    const auto textArea = getLabelBorderSize (label).subtractedFrom (label.getBounds());

    const auto runs = PinkXP::SymbolGlyphs::splitRuns (shown);
    const bool needsRunLayout = runs.size() > 1 || (runs.size() == 1 && runs[0].symbol);

    if (needsRunLayout && ! shown.containsChar ('\n'))
    {
        PinkXP::SymbolGlyphs::drawText (g, shown, textArea, label.getJustificationType(), baseFont);
        return;
    }

    const juce::Font font (PinkXP::SymbolGlyphs::rebase (baseFont, shown));
    g.setFont (font);
    g.drawFittedText (shown, textArea, label.getJustificationType(),
                      juce::jmax (1, (int) ((float) textArea.getHeight() / font.getHeight())),
                      label.getMinimumHorizontalScale());
}

juce::Label* PinkXPLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* l = juce::LookAndFeel_V4::createSliderTextBox(slider);
    l->setFont(PinkXP::getFont(11.0f, juce::Font::plain));
    l->setColour(juce::Label::textColourId,       PinkXP::ink);
    l->setColour(juce::Label::backgroundColourId, PinkXP::btnFace);
    l->setColour(juce::Label::outlineColourId,    juce::Colours::transparentBlack);
    return l;
}

void PinkXPLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    // 诊断埋点（UIPOPUP）：菜单窗口的绘制（paint）被调起。距上一次绘制超过 2s
    //   视为"一个新菜单窗口的首帧绘制"—— 与 preparePopupMenuWindow 的时间差
    //   即"窗口 setVisible → 首帧真正画出"的延迟。hover 重绘不刷屏（间隔短）。
    {
        static double s_lastMenuPaintMs = 0.0;
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (nowMs - s_lastMenuPaintMs > 2000.0)
            Y2K_DIAG ("UIPOPUP", "menu first paint (drawPopupMenuBackground)");
        s_lastMenuPaintMs = nowMs;
    }

    PinkXP::drawRaised(g, { 0, 0, width, height }, PinkXP::btnFace);
}

void PinkXPLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& bar,
                                      int x, int y, int width, int height,
                                      bool isScrollbarVertical,
                                      int thumbStartPosition, int thumbSize,
                                      bool isMouseOver, bool isMouseDown)
{
    juce::ignoreUnused (bar);

    const juce::Rectangle<int> track (x, y, width, height);
    if (track.isEmpty())
        return;

    // 轨道：比底色略深的凹槽 + 1px 内框（像素风，硬边不圆角）
    g.setColour (PinkXP::content.darker (0.05f));
    g.fillRect (track);
    g.setColour (PinkXP::ink.withAlpha (0.18f));
    g.drawRect (track, 1);

    if (thumbSize <= 0)
        return;

    auto thumb = isScrollbarVertical
                   ? juce::Rectangle<int> (x, y + thumbStartPosition, width, thumbSize)
                   : juce::Rectangle<int> (x + thumbStartPosition, y, thumbSize, height);

    // 轨道内缩 1px，滑块与边框之间留一点缝隙
    thumb = thumb.reduced (1);
    if (thumb.isEmpty())
        return;

    const auto base = isMouseDown ? PinkXP::pink600
                     : isMouseOver ? PinkXP::pink500
                                   : PinkXP::pink400;

    g.setColour (base);
    g.fillRect (thumb);
    g.setColour (PinkXP::ink.withAlpha (0.55f));
    g.drawRect (thumb, 1);

    // 左上 1px 高光：XP 凸起质感
    g.setColour (PinkXP::hl.withAlpha (0.75f));
    if (isScrollbarVertical)
        g.fillRect (thumb.getX() + 1, thumb.getY() + 1, thumb.getWidth() - 2, 1);
    else
        g.fillRect (thumb.getX() + 1, thumb.getY() + 1, 1, thumb.getHeight() - 2);
}

void PinkXPLookAndFeel::drawScrollbarButton (juce::Graphics& g, juce::ScrollBar& bar,
                                             int width, int height, int buttonDirection,
                                             bool isScrollbarVertical,
                                             bool shouldDrawButtonAsHighlighted,
                                             bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused (bar, isScrollbarVertical);

    const juce::Rectangle<int> r (0, 0, width, height);
    if (r.isEmpty())
        return;

    // 底色：正常 / 悬停 / 按下（全部取主题色，不再走 ScrollBar 的默认蓝灰）
    g.setColour (shouldDrawButtonAsDown        ? PinkXP::pink300
               : shouldDrawButtonAsHighlighted ? PinkXP::btnFace.brighter (0.06f)
                                               : PinkXP::btnFace);
    g.fillRect (r);
    g.setColour (PinkXP::ink.withAlpha (0.35f));
    g.drawRect (r, 1);

    // 小三角箭头（ink）
    const float cx = (float) r.getCentreX();
    const float cy = (float) r.getCentreY();
    const float t  = juce::jlimit (2.0f, 4.5f, (float) juce::jmin (width, height) * 0.30f);

    juce::Path tri;
    switch (buttonDirection)
    {
        case 0:  tri.addTriangle (cx, cy - t, cx - t, cy + t, cx + t, cy + t); break;   // 上
        case 1:  tri.addTriangle (cx + t, cy, cx - t, cy - t, cx - t, cy + t); break;   // 右
        case 2:  tri.addTriangle (cx, cy + t, cx - t, cy - t, cx + t, cy - t); break;   // 下
        default: tri.addTriangle (cx - t, cy, cx + t, cy - t, cx + t, cy + t); break;   // 左
    }
    g.setColour (PinkXP::ink.withAlpha (0.85f));
    g.fillPath (tri);
}

void PinkXPLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                          bool isSeparator, bool isActive, bool isHighlighted,
                                          bool isTicked, bool hasSubMenu,
                                          const juce::String& text, const juce::String& /*shortcutKeyText*/,
                                          const juce::Drawable* /*icon*/, const juce::Colour* /*textColour*/)
{
    if (isSeparator)
    {
        g.setColour(PinkXP::shdw);
        g.fillRect(area.getX() + 6, area.getCentreY(), area.getWidth() - 12, 1);
        g.setColour(PinkXP::hl);
        g.fillRect(area.getX() + 6, area.getCentreY() + 1, area.getWidth() - 12, 1);
        return;
    }

    if (isHighlighted && isActive)
    {
        g.setColour(PinkXP::sel);
        g.fillRect(area);
        g.setColour(PinkXP::selInk);
    }
    else
    {
        g.setColour(isActive ? PinkXP::ink : PinkXP::ink.withAlpha(0.45f));
    }

    const juce::String shown = y2k::tr (text);

    auto textArea = area.reduced(8, 0);
    if (isTicked)
    {
        auto tick = textArea.removeFromLeft(14);
        // ✓ 属于"中文像素字体画不出来"的装饰符号：经 SymbolGlyphs 切英文字体渲染
        const auto tickFont = PinkXP::SymbolGlyphs::getFont (juce::String::fromUTF8("\xe2\x9c\x93"),
                                                             12.0f, juce::Font::bold);
        PinkXP::SymbolGlyphs::drawText (g, juce::String::fromUTF8("\xe2\x9c\x93"), tick,
                                        juce::Justification::centredLeft, tickFont); // ✓
    }
    else
    {
        textArea.removeFromLeft(14);
    }

    // 菜单条目可能中英/符号混排（如「高级 ▸」）：按 run 绘制
    const auto itemFont = PinkXP::SymbolGlyphs::getFont (shown, 12.0f, juce::Font::bold);
    PinkXP::SymbolGlyphs::drawText (g, shown, textArea, juce::Justification::centredLeft, itemFont);

    if (hasSubMenu)
    {
        auto arrow = area;
        g.drawText(">", arrow.removeFromRight(12), juce::Justification::centred, false);
    }
}

juce::Font PinkXPLookAndFeel::getPopupMenuFont()
{
    return PinkXP::getFont(12.0f, juce::Font::bold);
}

// 全局字体兜底：任何通过 LookAndFeel 请求 Typeface 的控件都优先使用 PinkXP
// 当前激活的自定义字体（Silkscreen）。这能覆盖未显式 setFont 的组件路径，
// 避免"局部生效、整体回退系统字体"的混用风格。Windows / macOS 均受益。
juce::Typeface::Ptr PinkXPLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    juce::ignoreUnused (f);
    // 使用已缓存的 Typeface 而非每次调用 loadActiveTypeface()。
    // loadActiveTypeface() 内部会创建 SharedResourcePointer<Direct2DFactories>，
    // 每次调用开销大且在 DWrite 未就绪时可能崩溃。
    if (PinkXP::gTypeface != nullptr)
        return PinkXP::gTypeface;
    return juce::LookAndFeel_V4::getTypefaceForFont (f);
}

// Popup 菜单条目理想尺寸：
//   drawPopupMenuItem 中 textArea 的左右内边距为：
//     - area.reduced(8, 0) 左右各 8px
//     - textArea.removeFromLeft(14) 再吃掉 14px（tick / icon 占位）
//     - 右侧若 hasSubMenu 还会 removeFromRight(12)
//   所以这里预留 8 + 14 + 8(右侧余量) = 30px 给 padding，并用
//   getPopupMenuFont() 的实际宽度测量文字像素宽，再 +2 作为小量余量。
void PinkXPLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                                  int standardMenuItemHeight,
                                                  int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth  = 50;
        idealHeight = (standardMenuItemHeight > 0 ? standardMenuItemHeight : 10) / 10 + 4;
        return;
    }

    // 测量与 drawPopupMenuItem 同一套"按文本选字体 + run 化"
    const auto shown    = y2k::tr (text);
    const auto font     = PinkXP::SymbolGlyphs::getFont (shown, 12.0f, juce::Font::bold);
    const int  textPx   = (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (shown, font));
    constexpr int hPad  = 8 + 14 + 8;   // 见上方注释：与 drawPopupMenuItem 保持一致

    idealWidth  = textPx + hPad + 2;
    idealHeight = (standardMenuItemHeight > 0)
                      ? standardMenuItemHeight
                      : juce::roundToInt (font.getHeight() * 1.3f);
}

// ==========================================================
// ComboBox —— Pink XP 像素风
//   · 整体框体沿用 drawRaised（按下时 drawPressed），与按钮/面板风格一致
//   · 右侧 buttonArea 单独画一条像素竖分隔线 + 像素 "▼" 箭头
//   · 字体与 Label 同步，使用 PinkXP::getFont（11.5pt）
// ==========================================================
void PinkXPLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height,
                                     bool isButtonDown,
                                     int buttonX, int buttonY, int buttonW, int buttonH,
                                     juce::ComboBox& box)
{
    juce::ignoreUnused(buttonW, buttonH);

    const juce::Rectangle<int> bounds(0, 0, width, height);

    // 1) 外壳：按下时 sunken，其余时候 raised（与按钮一致的像素风）
    if (isButtonDown || box.isPopupActive())
        PinkXP::drawPressed(g, bounds, PinkXP::pink100);
    else
        PinkXP::drawRaised(g, bounds, PinkXP::btnFace);

    // 2) 右侧"▼"按钮区：用一条像素竖线与文本区域分隔
    if (buttonX > 2)
    {
        g.setColour(PinkXP::shdw);
        g.fillRect(buttonX, 2, 1, height - 4);
        g.setColour(PinkXP::hl);
        g.fillRect(buttonX + 1, 2, 1, height - 4);
    }

    // 3) 像素三角箭头（用填充矩形阶梯组成，保持 8-bit 感）
    //    尺寸固定：7x4 像素；居中放在按钮区
    const int arrowW = 7;
    const int arrowH = 4;
    const int ax = buttonX + (width - buttonX - arrowW) / 2;
    const int ay = (height - arrowH) / 2 + (isButtonDown ? 1 : 0);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId)
                  .withAlpha(box.isEnabled() ? 1.0f : 0.4f));
    // 从宽到窄的 4 行
    g.fillRect(ax,     ay,     arrowW,     1); // 7
    g.fillRect(ax + 1, ay + 1, arrowW - 2, 1); // 5
    g.fillRect(ax + 2, ay + 2, arrowW - 4, 1); // 3
    g.fillRect(ax + 3, ay + 3, 1,          1); // 1
}

juce::Font PinkXPLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return PinkXP::getFont(11.5f, juce::Font::bold);
}

void PinkXPLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    // 让文本留出右侧箭头按钮宽度（与 LookAndFeel_V4 保持一致的布局，但加一点 padding）
    const int arrowButtonW = juce::jmin(20, box.getHeight());
    label.setBounds(4, 1,
                    box.getWidth() - arrowButtonW - 4,
                    box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId,
                    box.findColour(juce::ComboBox::textColourId));
}

juce::PopupMenu::Options PinkXPLookAndFeel::getOptionsForComboBoxPopupMenu (juce::ComboBox& box,
                                                                            juce::Label& label)
{
    // 诊断埋点（UIPOPUP）：ComboBox::showPopup 已真正执行 —— 说明 mouseDown 里
    //   排入的 callAsync 已经被消息线程取出。配合 ComboPopupProbe 的 mouseDown
    //   时间戳，两条日志的时间差就是"点击 → 弹菜单"的延迟。
    Y2K_DIAG ("UIPOPUP", "combobox showPopup (getOptionsForComboBoxPopupMenu)");

    auto options = juce::LookAndFeel_V4::getOptionsForComboBoxPopupMenu (box, label);

    // 需要放宽菜单允许区域的两种场景（默认区域 = display->userArea）：
    //   1) stick 驻留态（Windows）：AppBar 把条带从 userArea 挖掉，下拉框位于
    //      条带内（userArea 之外），默认 clamp 会把菜单整体推到条带上方；
    //   2) macOS 原生全屏：窗口铺满 totalArea（覆盖菜单栏/Dock），但 JUCE 报告的
    //      display->userArea 仍扣除了菜单栏/Dock —— 底部工具栏里的下拉框正好落在
    //      这段被扣除的区域内，默认 clamp 会把菜单推到控件上方"隔一段距离"。
    bool needsOverride = PinkXP::isStickDockActive();

   #if JUCE_MAC
    if (! needsOverride)
        if (auto* top = box.getTopLevelComponent())
            if (auto* rw = dynamic_cast<juce::ResizableWindow*> (top))
                needsOverride = rw->isFullScreen();
   #endif

    if (! needsOverride)
        return options;

    const auto boxScreen = box.getScreenBounds();
    const auto display = juce::Desktop::getInstance().getDisplays()
                             .getDisplayForPoint (boxScreen.getCentre());
    if (display == nullptr)
        return options;

    // 工作区内的下拉（例如浮动窗口）不受影响，保持默认锚点与约束。
    if (display->userArea.contains (boxScreen.getCentre()))
        return options;

    // 放宽允许区域到整个显示器，让菜单按默认逻辑贴着下拉框展开（可以盖在
    //   条带 / 全屏底边上）。withParentAreaOverride 是本仓库 JUCE 的扩展
    //   （见 juce_PopupMenu.h），全平台可用。
    return options.withParentAreaOverride (
        display->safeAreaInsets.subtractedFrom (display->totalArea));
}

// ==========================================================
// 全屏菜单父组件
//   背景：JUCE 的 PopupMenu 默认创建【独立顶层窗口】—— MenuWindow 内部执行
//   setAlwaysOnTop(true) + addToDesktop(windowIsTemporary|windowIgnoresKeyPresses)，
//   与主窗口是两个互不相干的 HWND。伪全屏在任务栏隐藏时让主窗口正好铺满整块
//   显示器，Windows 会把这种"无边框 + 置顶 + 整屏 + 单张 GL 表面"的窗口按全屏
//   窗口处理，独立弹窗会被压在主窗口表面之下（菜单确实创建了，只是看不见）。
//
//   这里把菜单挂到 GL 表面宿主（Editor）下面：菜单成为窗口内容的一部分，
//   由 Editor 的 OpenGL 合成统一绘制，因此永远在主窗口表面之内、不可能被压住。
//   普通窗口模式 / 插件宿主 / 非 Windows 下 getInWindowMenuHost 返回 nullptr，
//   菜单仍是独立顶层窗口，可自由伸出窗口外，行为与改动前完全一致。
// ==========================================================
juce::Component* PinkXPLookAndFeel::getParentComponentForMenuOptions (const juce::PopupMenu::Options& options)
{
    // 诊断埋点（UIPOPUP）：MenuWindow 构造函数正在询问父组件 —— 即菜单窗口
    //   已经进入创建阶段。它与上一条 showPopup 的时间差反映 MenuWindow 创建耗时。
    Y2K_DIAG ("UIPOPUP", "menu window create (getParentComponentForMenuOptions)");

    // 已经有父组件（子菜单、或调用方显式 withParentComponent）时原样保留，
    //   避免改写层级导致子菜单定位 / 关闭逻辑错乱。
    if (auto* existing = options.getParentComponent())
        return existing;

    return PinkXP::getInWindowMenuHost (options.getTargetComponent());
}

void PinkXPLookAndFeel::preparePopupMenuWindow (juce::Component& newWindow)
{
    // 诊断埋点（UIPOPUP）：MenuWindow 构造即将完成（addToDesktop 已执行，原生
    //   HWND 已创建）。与 "menu window create" 的时间差 = 构造主体 + 原生窗口
    //   创建耗时；与首帧 paint 的时间差 = setVisible 上屏 + 首帧调度耗时。
    Y2K_DIAG ("UIPOPUP", "menu window prepared (preparePopupMenuWindow)");

    juce::LookAndFeel_V4::preparePopupMenuWindow (newWindow);
}

std::unique_ptr<juce::DropShadower> PinkXPLookAndFeel::createDropShadowerForComponent (juce::Component& comp)
{
    // 菜单窗口（JUCE 的 MenuWindow，组件名为 "menu"）：跳过系统阴影。
    //   根因（2026-11 日志定位）：每次弹下拉菜单都会新建临时顶层窗口，JUCE 的
    //   updateShadower 会为它创建 DropShadower —— 阴影子窗口 + Direct2D 高斯
    //   阴影渲染实测 80~143ms（首次最慢，Direct2D 工厂初始化），正是
    //   "点击下拉框卡一小下"的主因。主题菜单自带凸起边框，不需要系统阴影。
    if (comp.getName() == "menu")
    {
        Y2K_DIAG ("UIPOPUP", "menu drop shadow skipped (createDropShadowerForComponent)");
        return nullptr;
    }

    // 其它窗口（主窗口等）保留阴影；顺带测量创建耗时（超 30ms 才写日志）。
    {
        Y2K_DIAG_SLOW ("SHADOW", "createDropShadowerForComponent", 30.0);
        return juce::LookAndFeel_V2::createDropShadowerForComponent (comp);
    }
}

// ==========================================================
// 悬停提示（HoverHelp）绘制
//   外观 = 一个迷你 Pink XP 窗口：凸起外框 + 粉色标题条（sel / selInk）+
//   画布底色正文（content / readableInkOn），全部取运行时主题色。
//   getTooltipBounds 与 drawTooltip 共用 measureTip()，保证"量出来的尺寸"
//   与"画出来的换行"完全一致（正文按实测最宽行宽度重新排版）。
// ==========================================================
namespace
{
    struct TipLayout
    {
        juce::String title, body;
        int border = 3;          // drawRaised：2px 外框 + 1px 内高光
        int padX = 0, padY = 0;
        int titleH = 0;
        int bodyW = 0, bodyH = 0;
        int width = 0, height = 0;
    };

    // 提示标题 / 正文的基准字体（当前语言字体）。装饰符号（如
    //   "Use=apply ↑↓=sort ✎=rename" 的译文）由 SymbolGlyphs 按 run 换字体：
    //   整串切换会让中文正文在 mac 上整段消失（Silkscreen 无 CJK 字形）。
    juce::Font tipTitleFont (const juce::String&) { return PinkXP::getFont (8.5f, juce::Font::bold); }
    juce::Font tipBodyFont  (const juce::String&) { return PinkXP::getFont (8.0f, juce::Font::plain); }

    juce::AttributedString makeTipBody (const juce::String& body, juce::Colour colour)
    {
        juce::AttributedString as;
        as.setWordWrap (juce::AttributedString::byWord);
        as.setJustification (juce::Justification::topLeft);
        as.setLineSpacing ((float) PinkXP::ui (2));
        PinkXP::SymbolGlyphs::appendRuns (as, body, tipBodyFont (body), colour);
        return as;
    }

    TipLayout measureTip (const juce::String& tip)
    {
        TipLayout L;

        const int nl = tip.indexOfChar ('\n');
        if (nl >= 0)
        {
            L.title = tip.substring (0, nl).trim();
            L.body  = tip.substring (nl + 1).trim();
        }
        else
        {
            L.body = tip.trim();
        }

        // 界面语言：标题 / 正文分别查表（正文整段未命中时逐行翻译）
        L.title = y2k::tr (L.title);
        L.body  = y2k::tr (L.body);

        L.padX = PinkXP::ui (7);
        L.padY = PinkXP::ui (5);
        const int maxTextW = PinkXP::ui (300);

        int titleW = 0;
        if (L.title.isNotEmpty())
        {
            const auto f = tipTitleFont (L.title);
            L.titleH = (int) std::ceil (f.getHeight()) + PinkXP::ui (5);
            titleW   = (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (L.title, f)) + 2;
        }

        if (L.body.isNotEmpty())
        {
            juce::TextLayout tl;
            tl.createLayout (makeTipBody (L.body, PinkXP::ink), (float) maxTextW);

            float widest = 0.0f;
            for (int i = 0; i < tl.getNumLines(); ++i)
                widest = juce::jmax (widest, tl.getLine (i).getLineBoundsX().getEnd());

            L.bodyW = juce::jmax (1, (int) std::ceil (widest) + 2);
            L.bodyH = (int) std::ceil (tl.getHeight());
        }

        const int textW = juce::jlimit (PinkXP::ui (40), maxTextW + 2, juce::jmax (titleW, L.bodyW));
        L.width  = textW + L.padX * 2 + L.border * 2;
        L.height = L.border * 2 + L.titleH
                 + (L.body.isNotEmpty() ? (L.bodyH + L.padY * 2) : 0);
        return L;
    }
}

juce::Rectangle<int> PinkXPLookAndFeel::getTooltipBounds (const juce::String& tipText,
                                                          juce::Point<int> screenPos,
                                                          juce::Rectangle<int> parentArea)
{
    const auto L = measureTip (tipText);

    // 提示框比宿主区域还大时（如 stick 条带很矮）按宿主区域截断
    const int w = juce::jmin (L.width,  juce::jmax (1, parentArea.getWidth()));
    const int h = juce::jmin (L.height, juce::jmax (1, parentArea.getHeight()));

    // 优先放在光标右下方（避开光标本体）；越界翻到左侧 / 上方
    int x = screenPos.x + PinkXP::ui (14);
    int y = screenPos.y + PinkXP::ui (20);
    if (x + w > parentArea.getRight())
        x = screenPos.x - w - PinkXP::ui (6);
    if (y + h > parentArea.getBottom())
        y = screenPos.y - h - PinkXP::ui (8);

    x = juce::jlimit (parentArea.getX(), juce::jmax (parentArea.getX(), parentArea.getRight()  - w), x);
    y = juce::jlimit (parentArea.getY(), juce::jmax (parentArea.getY(), parentArea.getBottom() - h), y);

    return { x, y, w, h };
}

void PinkXPLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto L = measureTip (text);
    const juce::Rectangle<int> bounds (0, 0, width, height);

    PinkXP::drawRaised (g, bounds, PinkXP::content);

    auto inner = bounds.reduced (L.border);

    if (L.titleH > 0)
    {
        auto strip = inner.removeFromTop (L.titleH);
        g.setColour (PinkXP::sel);
        g.fillRect (strip);
        g.setColour (PinkXP::dark.withAlpha (0.55f));
        g.fillRect (strip.getX(), strip.getBottom() - 1, strip.getWidth(), 1);

        g.setColour (PinkXP::selInk);
        PinkXP::SymbolGlyphs::drawText (g, L.title,
                                        strip.reduced (L.padX, 0).withTrimmedBottom (1),
                                        juce::Justification::centredLeft,
                                        tipTitleFont (L.title));
    }

    if (L.body.isNotEmpty())
    {
        const auto bodyArea = inner.reduced (L.padX, L.padY);

        juce::TextLayout tl;
        tl.createLayout (makeTipBody (L.body, PinkXP::readableInkOn (PinkXP::content)),
                         (float) L.bodyW);

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (bodyArea);
        tl.draw (g, bodyArea.toFloat().withWidth ((float) L.bodyW));
    }
}

PinkXPLookAndFeel& getPinkXPLookAndFeel()
{
    static PinkXPLookAndFeel lnf;
    return lnf;
}
