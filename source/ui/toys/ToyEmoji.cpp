#include "source/ui/toys/ToyKit.h"
#include "source/Y2KLogging.h"

#if JUCE_WINDOWS
 #include <cwchar>
 #include <vector>
 #include <wrl/client.h>
 #include <dwrite.h>
 #include <d2d1.h>
 #include <wincodec.h>
 #pragma comment(lib, "windowscodecs.lib")
#endif

// ==========================================================
// ToyEmoji —— emoji 白名单表 + 烘焙 + 探测 + 分级降级（§5）
//   烘焙成图集，运行时只画图。
//   Windows：JUCE 的 SoftwareImageType 走 LowLevelGraphicsSoftwareRenderer，
//   只渲染单色轮廓，彩色 COLR emoji 会退化成空白 —— 因此用 DirectWrite +
//   D2D1.1(WARP 软件设备) + ENABLE_COLOR_FONT 自行渲染彩色 emoji（§5.4）。
//   macOS/Linux：沿用 JUCE 软件渲染 + 覆盖率/双色差分探测。
// ==========================================================

namespace
{
    struct EmojiSpec
    {
        const char16_t* utf16;   // UTF-16 码元（u"..." 字面量，编码固定，无歧义）
        int             tier;    // 1 / 2 / 3
        EmojiId         fallback; // 失败时的 Tier-1 备选
    };

    // 顺序必须与 EmojiId 枚举严格一致。
    const EmojiSpec kSpecs[(int) EmojiId::Count] =
    {
        // 通用池
        { u"\u2764\uFE0F", 1, EmojiId::Sparkles },  // ❤️
        { u"\U0001F496",          1, EmojiId::Sparkles },  // 💖
        { u"\u2B50",              1, EmojiId::Sparkles },  // ⭐
        { u"\U0001F31F",          1, EmojiId::Sparkles },  // 🌟
        { u"\u2728",              1, EmojiId::Star },      // ✨
        { u"\U0001F3B5",          1, EmojiId::Sparkles },  // 🎵
        { u"\U0001F3B6",          1, EmojiId::Sparkles },  // 🎶
        { u"\U0001F60E",          1, EmojiId::Sparkles },  // 😎
        { u"\U0001F60D",          1, EmojiId::Sparkles },  // 😍
        { u"\U0001F4AB",          1, EmojiId::Sparkles },  // 💫
        { u"\U0001F525",          1, EmojiId::Sparkles },  // 🔥
        { u"\U0001F389",          1, EmojiId::Sparkles },  // 🎉
        { u"\U0001F48E",          1, EmojiId::Sparkles },  // 💎
        // Tier-2 加成
        { u"\U0001F973",          2, EmojiId::HeartEyes }, // 🥳
        // 主题池
        { u"\U0001F98B",          1, EmojiId::Sparkles },  // 🦋
        { u"\u26A1",              1, EmojiId::Sparkles },  // ⚡
        { u"\U0001F352",          1, EmojiId::Sparkles },  // 🍒
        { u"\U0001F4FC",          1, EmojiId::Sparkles },  // 📼
        { u"\U0001F4BF",          1, EmojiId::Sparkles },  // 💿
        { u"\U0001F5A4",          1, EmojiId::Sparkles },  // 🖤
        { u"\U0001F353",          1, EmojiId::Sparkles },  // 🍓
        { u"\U0001F380",          1, EmojiId::Sparkles },  // 🎀
        { u"\U0001F36D",          1, EmojiId::Sparkles },  // 🍭
        { u"\U0001F36C",          1, EmojiId::Sparkles },  // 🍬
        { u"\U0001F338",          1, EmojiId::Sparkles },  // 🌸
        { u"\U0001F319",          1, EmojiId::Sparkles },  // 🌙
        { u"\U0001F30D",          1, EmojiId::Sparkles },  // 🌍
        { u"\u2604\uFE0F",  1, EmojiId::Sparkles },  // ☄️
        { u"\U0001F320",          1, EmojiId::Sparkles },  // 🌠
        { u"\U0001F47D",          1, EmojiId::Sparkles },  // 👽
        { u"\U0001F52E",          1, EmojiId::Sparkles },  // 🔮
        { u"\U0001F3A7",          1, EmojiId::Sparkles },  // 🎧
        { u"\U0001F579\uFE0F", 1, EmojiId::Sparkles }, // 🕹️
        { u"\U0001F47E",          1, EmojiId::Sparkles },  // 👾
        { u"\U0001F34A",          1, EmojiId::Sparkles },  // 🍊
        { u"\u2600\uFE0F",  1, EmojiId::Sparkles },  // ☀️
        { u"\U0001F366",          1, EmojiId::Sparkles },  // 🍦
        { u"\U0001F379",          1, EmojiId::Sparkles },  // 🍹
        { u"\U0001F3D6\uFE0F", 1, EmojiId::Sparkles }, // 🏖️
        { u"\U0001F41A",          1, EmojiId::Sparkles },  // 🐚
        { u"\U0001F4A7",          1, EmojiId::Sparkles },  // 💧
        { u"\U0001F41F",          1, EmojiId::Sparkles },  // 🐟
        { u"\U0001F30A",          1, EmojiId::Sparkles },  // 🌊
        { u"\U0001F42C",          1, EmojiId::Sparkles },  // 🐬
        { u"\U0001F980",          1, EmojiId::Sparkles },  // 🦀
        { u"\U0001F964",          1, EmojiId::Sparkles },  // 🥤
        { u"\U0001F343",          1, EmojiId::Sparkles },  // 🍃
        { u"\U0001F34F",          1, EmojiId::Sparkles },  // 🍏
        { u"\U0001F375",          1, EmojiId::Sparkles },  // 🍵
        { u"\U0001F33F",          1, EmojiId::Sparkles },  // 🌿
        { u"\U0001F340",          1, EmojiId::Sparkles },  // 🍀
        { u"\U0001F438",          1, EmojiId::Sparkles },  // 🐸
        { u"\U0001F344",          1, EmojiId::Sparkles },  // 🍄
        { u"\U0001F334",          1, EmojiId::Sparkles },  // 🌴
        { u"\U0001F98E",          1, EmojiId::Sparkles },  // 🦎
        { u"\U0001F4BE",          1, EmojiId::Sparkles },  // 💾
        { u"\U0001F5B1\uFE0F", 1, EmojiId::Sparkles }, // 🖱️
        { u"\u231B",              1, EmojiId::Sparkles },  // ⌛
        { u"\U0001F4C1",          1, EmojiId::Sparkles },  // 📁
        { u"\U0001F5A5\uFE0F", 1, EmojiId::Sparkles }, // 🖥️
        { u"\U0001F4BB",          1, EmojiId::Sparkles },  // 💻
        { u"\U0001F480",          1, EmojiId::Sparkles },  // 💀
        { u"\U0001F987",          1, EmojiId::Sparkles },  // 🦇
        { u"\U0001F339",          1, EmojiId::Sparkles },  // 🌹
        { u"\U0001F940",          1, EmojiId::Sparkles },  // 🥀
        { u"\U0001F577\uFE0F", 1, EmojiId::Sparkles }, // 🕷️
        { u"\u2B1B",              1, EmojiId::Sparkles },  // ⬛
        { u"\u2B1C",              1, EmojiId::Sparkles },  // ⬜
        { u"\U0001F532",          1, EmojiId::Sparkles },  // 🔲
        { u"\U0001F533",          1, EmojiId::Sparkles },  // 🔳
        { u"\u2795",              1, EmojiId::Sparkles },  // ➕
        { u"\u2714\uFE0F",  1, EmojiId::Sparkles },  // ✔️
        // 模块玩法
        { u"\U0001F4A5",          1, EmojiId::Sparkles },  // 💥
        { u"\U0001F3B0",          1, EmojiId::Sparkles },  // 🎰
        { u"\U0001F4B0",          1, EmojiId::Sparkles },  // 💰
        { u"\U0001F5DC\uFE0F", 2, EmojiId::Boom },   // 🗜️
        { u"\U0001F4E1",          1, EmojiId::Dizzy },     // 📡
        { u"\U0001F534",          1, EmojiId::Sparkles },  // 🔴
        { u"\u26BD",              1, EmojiId::Sparkles },  // ⚽
        { u"\U0001F37F",          1, EmojiId::Sparkles },  // 🍿
        { u"\U0001F4F8",          2, EmojiId::CameraOld }, // 📸
        { u"\U0001F4F7",          1, EmojiId::Sparkles },  // 📷
        { u"\U0001F3B8",          1, EmojiId::Sparkles },  // 🎸
        { u"\U0001F64C",          1, EmojiId::Sparkles },  // 🙌
        { u"\U0001F44B",          1, EmojiId::Sparkles },  // 👋
        { u"\U0001F4FA",          1, EmojiId::Sparkles },  // 📺
        { u"\U0001F6A9",          1, EmojiId::Sparkles },  // 🚩
        { u"\U0001F30B",          1, EmojiId::Sparkles },  // 🌋
        { u"\u26A0\uFE0F",  1, EmojiId::Sparkles },  // ⚠️
        { u"\u274C",              1, EmojiId::Sparkles },  // ❌
        { u"\U0001F4CE",          1, EmojiId::Sparkles },  // 📎
        // Tier-3 加成
        { u"\U0001FA99",          3, EmojiId::Sparkles },  // 🪙
        { u"\U0001FA90",          3, EmojiId::Sparkles },  // 🪐
        { u"\U0001FAE7",          3, EmojiId::Sparkles },  // 🫧
        { u"\U0001FAA9",          3, EmojiId::Sparkles },  // 🪩
        { u"\U0001F9CA",          3, EmojiId::Sparkles },  // 🧊
        { u"\U0001FA78",          3, EmojiId::Sparkles },  // 🩸
    };

    // 检测图像是否有内容（非透明像素占比 ≥ 阈值）。
    bool imageHasContent (const juce::Image& img, float minCoverage)
    {
        if (! img.isValid()) return false;
        const int w = img.getWidth(), h = img.getHeight();
        if (w <= 0 || h <= 0) return false;

        const juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
        int opaque = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (bd.getPixelColour (x, y).getAlpha() > 8)
                    ++opaque;

        return (float) opaque / (float) (w * h) >= minCoverage;
    }

    // 检测图像是否"彩色"（≥3 种显著颜色）。豆腐块 / 单色退化只有 1~2 种颜色。
    bool imageIsColorful (const juce::Image& img)
    {
        if (! img.isValid()) return false;
        const int w = img.getWidth(), h = img.getHeight();
        const juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);

        juce::uint32 colours[16] = {};
        int n = 0;
        for (int y = 0; y < h; y += 2)
        {
            for (int x = 0; x < w; x += 2)
            {
                const juce::Colour c = bd.getPixelColour (x, y);
                if (c.getAlpha() < 32) continue;
                const juce::uint32 key = (c.getRed() >> 5) | ((c.getGreen() >> 5) << 3) | ((c.getBlue() >> 5) << 6);
                bool seen = false;
                for (int i = 0; i < n; ++i)
                    if (colours[i] == key) { seen = true; break; }
                if (! seen && n < 16)
                    colours[n++] = key;
                if (n >= 3) return true;
            }
        }
        return false;
    }

#if JUCE_WINDOWS
    // DirectWrite + D2D1.0 WIC 软件渲染目标渲染彩色 emoji（COLR）。
    //   · 之前用 D2D1.1 硬件设备 + CPU_READ 位图失败：CPU_READ 不支持硬件目标，
    //     且 CPU_READ|TARGET 是非法组合（E_INVALIDARG）。
    //   · WIC 软件目标（CreateWicBitmapRenderTarget）可直接 Lock 读像素，
    //     且 ENABLE_COLOR_FONT 在软件目标上正确渲染 COLR 彩色 emoji。
    // 工厂只初始化一次（static 缓存），WIC 位图按尺寸每次创建。
    juce::Image renderEmojiWin (const WCHAR* utf16, int numChars, int px)
    {
        namespace wrl = Microsoft::WRL;

        struct Devices
        {
            wrl::ComPtr<IDWriteFactory>      dw;
            wrl::ComPtr<ID2D1Factory>        d2d;
            wrl::ComPtr<IWICImagingFactory>  wic;
            bool ok = false;

            Devices()
            {
                HRESULT hr = DWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED,
                                                  __uuidof (IDWriteFactory),
                                                  reinterpret_cast<IUnknown**> (dw.GetAddressOf()));
                if (FAILED (hr) || ! dw) { log ("DWrite factory failed hr=" + juce::String::toHexString ((int) hr)); return; }

                hr = D2D1CreateFactory (D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                        __uuidof (ID2D1Factory), nullptr,
                                        reinterpret_cast<void**> (d2d.GetAddressOf()));
                if (FAILED (hr) || ! d2d) { log ("D2D1 factory failed hr=" + juce::String::toHexString ((int) hr)); return; }

                hr = CoCreateInstance (CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       __uuidof (IWICImagingFactory),
                                       reinterpret_cast<void**> (wic.GetAddressOf()));
                if (FAILED (hr) || ! wic) { log ("WIC factory failed hr=" + juce::String::toHexString ((int) hr)); return; }

                ok = true;
                log ("renderer ready (WIC software target)");
            }

            static void log (const juce::String& msg)
            {
                // 仅调试输出（B-06：不再写桌面文件）
                const juce::String s = "[ToyEmoji] " + msg;
                OutputDebugStringW (s.toWideCharPointer());
                juce::Logger::writeToLog (s);
            }
        };

        static Devices devs;
        if (! devs.ok)
            return {};

        // 文本布局
        wrl::ComPtr<IDWriteTextFormat> format;
        HRESULT hr = devs.dw->CreateTextFormat (L"Segoe UI Emoji", nullptr,
                                                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, (float) (px * 0.82f), L"en-US",
                                                format.GetAddressOf());
        if (FAILED (hr) || ! format) { devs.log ("CreateTextFormat failed hr=" + juce::String::toHexString ((int) hr)); return {}; }

        format->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const UINT32 len = (UINT32) juce::jmax (0, numChars);

        wrl::ComPtr<IDWriteTextLayout> layout;
        hr = devs.dw->CreateTextLayout (utf16, len, format.Get(), (float) px, (float) px,
                                        layout.GetAddressOf());
        if (FAILED (hr) || ! layout) { devs.log ("CreateTextLayout failed hr=" + juce::String::toHexString ((int) hr)); return {}; }

        // WIC 软件渲染目标
        wrl::ComPtr<IWICBitmap> wicBitmap;
        hr = devs.wic->CreateBitmap ((UINT) px, (UINT) px, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnDemand, wicBitmap.GetAddressOf());
        if (FAILED (hr) || ! wicBitmap) { devs.log ("WIC CreateBitmap failed hr=" + juce::String::toHexString ((int) hr)); return {}; }

        const auto rtp = D2D1::RenderTargetProperties (
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat (DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);

        wrl::ComPtr<ID2D1RenderTarget> rt;
        hr = devs.d2d->CreateWicBitmapRenderTarget (wicBitmap.Get(), rtp, rt.GetAddressOf());
        if (FAILED (hr) || ! rt) { devs.log ("CreateWicBitmapRenderTarget failed hr=" + juce::String::toHexString ((int) hr)); return {}; }

        rt->BeginDraw();
        rt->Clear (D2D1::ColorF (0.0f, 0.0f, 0.0f, 0.0f));

        wrl::ComPtr<ID2D1SolidColorBrush> brush;
        rt->CreateSolidColorBrush (D2D1::ColorF (D2D1::ColorF::White), brush.GetAddressOf());

        // 关键：ENABLE_COLOR_FONT 让 COLR 彩色 emoji 以彩色渲染
        rt->DrawTextLayout (D2D1::Point2F (0.0f, 0.0f), layout.Get(), brush.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        hr = rt->EndDraw();
        if (FAILED (hr)) { devs.log ("EndDraw failed hr=" + juce::String::toHexString ((int) hr)); return {}; }

        // 读取像素（PBGRA premultiplied → ARGB straight）
        wrl::ComPtr<IWICBitmapLock> lock;
        hr = wicBitmap->Lock (nullptr, WICBitmapLockRead, lock.GetAddressOf());
        if (FAILED (hr) || ! lock) { devs.log ("WIC Lock failed hr=" + juce::String::toHexString ((int) hr)); return {}; }

        UINT stride = 0, bufSize = 0;
        BYTE* buf = nullptr;
        lock->GetStride (&stride);
        lock->GetDataPointer (&bufSize, &buf);

        juce::Image img (juce::Image::ARGB, px, px, true);
        {
            const juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);

            for (int y = 0; y < px; ++y)
            {
                const BYTE* src = buf + (size_t) y * stride;
                juce::uint8* dst = bd.getLinePointer (y);
                for (int x = 0; x < px; ++x)
                {
                    const int a = src[x * 4 + 3];
                    int r = src[x * 4 + 2];
                    int g = src[x * 4 + 1];
                    int b = src[x * 4 + 0];
                    if (a > 0 && a < 255)   // 反 premultiply
                    {
                        r = (r * 255) / a;
                        g = (g * 255) / a;
                        b = (b * 255) / a;
                    }
                    dst[x * 4 + 0] = (juce::uint8) r;
                    dst[x * 4 + 1] = (juce::uint8) g;
                    dst[x * 4 + 2] = (juce::uint8) b;
                    dst[x * 4 + 3] = (juce::uint8) a;
                }
            }
        }
        return img;
    }
#endif
}

juce::String emojiUtf8 (EmojiId id)
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return {};
    return juce::String (juce::CharPointer_UTF16 (
        reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (kSpecs[i].utf16)));
}

bool emojiIsTier3 (EmojiId id)
{
    const int i = (int) id;
    return i >= 0 && i < (int) EmojiId::Count && kSpecs[i].tier == 3;
}

EmojiId emojiFallback (EmojiId id)
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return EmojiId::Sparkles;
    return kSpecs[i].fallback;
}

// ==========================================================
// drawEmoji —— 唯一 emoji 绘制入口（FIX-1 / B-01）
// ==========================================================
void drawEmoji (juce::Graphics& g, const juce::Image& img,
                juce::Point<float> pos, float drawSize, float angle, float alpha, float scaleY)
{
    if (! img.isValid() || drawSize <= 0.0f)
        return;

    const auto t = y2ktoy::emojiTransform (pos.x, pos.y, drawSize, angle, (float) img.getWidth(), scaleY);
    const juce::AffineTransform xform (t.a, t.b, t.e, t.c, t.d, t.f);

    g.setOpacity (alpha);
    g.drawImageTransformed (img, xform, false);
    g.setOpacity (1.0f);
}

// ==========================================================
// EmojiAtlas
// ==========================================================
EmojiAtlas::EmojiAtlas()
{
    Y2K_LOG ("[toy] EmojiAtlas ctor begin");
    // 显式指定系统 emoji 字体（F18：绝不依赖字体回退）。
#if JUCE_WINDOWS
    emojiFamily = "Segoe UI Emoji";
#elif JUCE_MAC
    emojiFamily = "Apple Color Emoji";
#else
    emojiFamily = "Noto Color Emoji";
#endif

    // 预烘焙：消息线程分帧（B-05），启动延迟约 1 秒由 timerCallback 控制；
    //   30Hz + 每 tick 3 个（P7），启动期抢帧更平缓
    Y2K_LOG ("[toy] EmojiAtlas startTimer begin");
    startTimerHz (30);
    Y2K_LOG ("[toy] EmojiAtlas ctor done");
}

EmojiAtlas::~EmojiAtlas()
{
    stopTimer();
    release();
}

void EmojiAtlas::release()
{
    stopTimer();
    for (int e = 0; e < (int) EmojiId::Count; ++e)
        for (int s = 0; s < (int) Size::Count; ++s)
        {
            entries_[e][s] = Entry{};
            themed_[e][s]  = ThemedEntry{};
        }
    prebakeCursor_ = 0;
    themedCursor_  = 0;
    prebakeStartDelay_ = 0.0;
}

int EmojiAtlas::logicalSize (Size s)
{
    switch (s)
    {
        case Size::S: return 18;
        case Size::M: return 24;
        case Size::L: return 34;
        default:      return 24;
    }
}

int EmojiAtlas::drawSize (Size s) const
{
    return juce::jmax (8, juce::roundToInt (logicalSize (s) * PinkXP::uiScale()));
}

bool EmojiAtlas::has (EmojiId id) const
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return false;
    return entries_[i][(int) Size::M].ok;
}

bool EmojiAtlas::isBaked (EmojiId id, Size size) const
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return false;
    return entries_[i][(int) size].baked;
}

void EmojiAtlas::bakeOne (EmojiId id, Size size)
{
    Entry& entry = entries_[(int) id][(int) size];
    if (entry.baked)
        return;
    entry.baked = true;

    const int logical = logicalSize (size);
    // 2x 分辨率留 DPI 余量 + 2px 边距（§5.2）
    const int px = logical * 2 + 2;

#if JUCE_WINDOWS
    // 直接用 UTF-16 码元（kSpecs 已是 u"..." 字面量，编码固定无歧义）
    const char16_t* u16src = kSpecs[(int) id].utf16;
    int u16len = 0;
    while (u16src[u16len]) ++u16len;

    // 首选：D2D 彩色渲染（COLR）。有内容且多色才视为成功（排除豆腐块单色方框）。
    {
        juce::Image color = renderEmojiWin ((const WCHAR*) u16src, u16len, px);
        if (imageHasContent (color, 0.015f) && imageIsColorful (color))
        {
            entry.ok  = true;
            entry.img = std::move (color);
            return;
        }
    }

    // 回退：JUCE 软件单色渲染（用同一份 UTF-16，避免编码差异）
    auto renderGlyph = [&](juce::Colour colour)
    {
        juce::Image img (juce::Image::ARGB, px, px, true, juce::SoftwareImageType());
        juce::Graphics g (img);
        g.setFont (juce::FontOptions (emojiFamily, (float) (px * 0.82f), juce::Font::plain));
        g.setColour (colour);
        g.drawText (juce::String (juce::CharPointer_UTF16 (
                        reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (u16src))),
                    juce::Rectangle<int> (0, 0, px, px),
                    juce::Justification::centred, false);
        return img;
    };

    juce::Image white = renderGlyph (juce::Colours::white);
    juce::Image black = renderGlyph (juce::Colours::black);

    entry.ok = probe (white, black, 0.12f);
    if (entry.ok)
        entry.img = std::move (white);
    else
        entry.img = juce::Image();
#else
    auto renderGlyph = [&](juce::Colour colour)
    {
        juce::Image img (juce::Image::ARGB, px, px, true, juce::SoftwareImageType());
        juce::Graphics g (img);
        g.setFont (juce::FontOptions (emojiFamily, (float) (px * 0.82f), juce::Font::plain));
        g.setColour (colour);
        g.drawText (emojiUtf8 (id), juce::Rectangle<int> (0, 0, px, px),
                    juce::Justification::centred, false);
        return img;
    };

    juce::Image white = renderGlyph (juce::Colours::white);

#if JUCE_MAC
    // macOS：Apple Color Emoji 是彩色位图字形（sbix），CoreText 绘制时忽略
    //   前景色，白/黑两次渲染结果相同 → 双色差分判据会误判失败，进而把大量
    //   emoji 逼进 fallback 链 + 环，触发无限递归卡死。这里改用纯覆盖率判据：
    //   有足够非透明像素即视为字形真实存在、烘焙成功。
    entry.ok = imageHasContent (white, 0.12f);
#else
    juce::Image black = renderGlyph (juce::Colours::black);
    entry.ok = probe (white, black, 0.12f);
#endif

    if (entry.ok)
        entry.img = std::move (white);
    else
        entry.img = juce::Image();
#endif
}

bool EmojiAtlas::probe (const juce::Image& white, const juce::Image& black, float coverageThreshold)
{
    if (! white.isValid() || ! black.isValid())
        return false;

    const int w = white.getWidth();
    const int h = white.getHeight();
    if (w <= 0 || h <= 0)
        return false;

    const juce::Image::BitmapData wd (white, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData bd (black, juce::Image::BitmapData::readOnly);

    int opaque = 0;
    int total  = 0;
    int diffPx = 0;

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const juce::Colour wc = wd.getPixelColour (x, y);
            const juce::Colour bc = bd.getPixelColour (x, y);
            const bool wo = wc.getAlpha() > 8;
            const bool bo = bc.getAlpha() > 8;
            if (wo || bo)
            {
                ++total;
                if (wo) ++opaque;
                // 双色差分：白/黑渲染结果差异小 → 单色回退
                const int dr = std::abs ((int) wc.getRed()   - (int) bc.getRed());
                const int dg = std::abs ((int) wc.getGreen() - (int) bc.getGreen());
                const int db = std::abs ((int) wc.getBlue()  - (int) bc.getBlue());
                if (dr + dg + db > 48)
                    ++diffPx;
            }
        }
    }

    if (total <= 0)
        return false;

    // ① 覆盖率 ≥ 12%
    const float coverage = (float) opaque / (float) (w * h);
    if (coverage < coverageThreshold)
        return false;

    // ② 双色差分：至少 5% 的像素在白色与黑色渲染下有颜色差异
    const float diffRatio = (float) diffPx / (float) total;
    if (diffRatio < 0.05f)
        return false;

    return true;
}

const juce::Image& EmojiAtlas::image (EmojiId id, Size size)
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
    {
        static const juce::Image empty;
        return empty;
    }

    // 只读（B-05）：烘焙由预烘焙 Timer 在消息线程分帧完成；
    // 未就绪返回空图，调用方跳过绘制。
    Entry& entry = entries_[i][(int) size];

    // 探测失败 → 走 Tier-1 备选。
    //   BUGFIX（卡死）：fallback 表存在环（Star ↔ Sparkles）。macOS 上彩色
    //   emoji 会被旧的双色差分探测误判失败，一旦两个互指 fallback 的 emoji
    //   都 !ok，这里就会沿环无限递归 → 栈溢出 → 主线程卡死（beach ball）。
    //   改为「最多回退一层」：备选也失败时直接返回空图，绝不二次递归。
    if (! entry.ok)
    {
        const EmojiId fb = emojiFallback (id);
        if (fb != id)
        {
            const int fi = (int) fb;
            if (fi >= 0 && fi < (int) EmojiId::Count)
            {
                Entry& fbEntry = entries_[fi][(int) size];
                if (fbEntry.ok)
                    return fbEntry.img;
            }
        }
        return entry.img;   // 空图
    }

    return entry.img;
}

const juce::Image& EmojiAtlas::themedImage (EmojiId id, Size size)
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return image (id, size);

    ThemedEntry& t = themed_[i][(int) size];
    if (t.ok)
        return t.img;

    // 主题版本未就绪 → 回退原图
    return image (id, size);
}

void EmojiAtlas::invalidateThemed()
{
    for (int e = 0; e < (int) EmojiId::Count; ++e)
        for (int s = 0; s < (int) Size::Count; ++s)
            themed_[e][s] = ThemedEntry{};
    themedCursor_ = 0;

    // 旋转分桶缓存也随之失效（主题色变了）
    {
        const juce::ScopedLock sl (rotLock_);
        rotCache_.clear();
    }

    if (! isTimerRunning())
        startTimerHz (30);
}

int EmojiAtlas::bucketOf (float angleRad)
{
    const float step = 6.2831853f / (float) kRotBuckets;
    int bucket = (int) std::llround (angleRad / step) % kRotBuckets;
    if (bucket < 0)
        bucket += kRotBuckets;
    return bucket;
}

static size_t rotKey (int id, int size, float angleRad)
{
    return ((size_t) id << 8) | ((size_t) size << 4) | (size_t) EmojiAtlas::bucketOf (angleRad);
}

juce::Image EmojiAtlas::rotatedCached (EmojiId id, Size size, float angleRad)
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return {};

    // paint 路径：只做无锁快查（值拷贝，引用计数，零烘焙、零风险）
    const size_t key = rotKey (i, (int) size, angleRad);
    const juce::ScopedLock sl (rotLock_);
    auto it = rotCache_.find (key);
    if (it != rotCache_.end())
        return it->second;
    return {};
}

void EmojiAtlas::ensureRotated (EmojiId id, Size size, float angleRad)
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return;

    const size_t key = rotKey (i, (int) size, angleRad);
    {
        const juce::ScopedLock sl (rotLock_);
        if (rotCache_.find (key) != rotCache_.end())
            return;
    }

    // 性能优化 P11：烘焙节流（每 16ms 窗口最多 2 张），
    //   消除主题切换/粒子风暴时一帧内多张烘焙的 frameMs 尖峰（日志实测 2~4ms）。
    {
        static double lastWindow = 0.0;
        static int    bakedInWindow = 0;
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (nowMs - lastWindow > 16.0)
        {
            lastWindow = nowMs;
            bakedInWindow = 0;
        }
        if (bakedInWindow >= 2)
            return;
        ++bakedInWindow;
    }

    // 原图尚未烘焙完成（预烘焙 timer 未走到该条目）→ 本轮跳过，
    //   绝不在这里触发 bakeOne（D2D 工厂是 SINGLE_THREADED，只能由消息线程 timer 独占使用）
    if (! isBaked (id, size))
        return;

    // step 阶段烘焙（不嵌套在 paint 回调里）：以主题版（或原图）为源
    const juce::Image& src = themedImage (id, size);
    if (! src.isValid() || src.getWidth() <= 0)
        return;

    // 自写最邻近采样旋转（固定 w×w 次循环，不依赖 JUCE 软件渲染的 transform 路径，
    //   从机制上杜绝渲染层死循环；24~48px 小图每次烘焙 ~百微秒级）
    const juce::Image srcCopy = src;   // BitmapData 需要非 const Image&
    const int w = srcCopy.getWidth();
    juce::Image rotated (juce::Image::ARGB, w, w, true);
    {
        const juce::Image::BitmapData sbd (const_cast<juce::Image&> (srcCopy), 0, 0, w, w,
                                           juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dbd (rotated, 0, 0, w, w, juce::Image::BitmapData::writeOnly);

        const float step = 6.2831853f / (float) kRotBuckets;
        const int bucket = (int) (key & 0xF);
        const float h = (float) w * 0.5f;
        const float ang = (float) bucket * step;
        const float cs = std::cos (ang);
        const float sn = std::sin (ang);

        for (int y = 0; y < w; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const float dx = (float) x - h;
                const float dy = (float) y - h;
                const int sx = juce::jlimit (0, w - 1, juce::roundToInt (dx * cs + dy * sn + h));
                const int sy = juce::jlimit (0, w - 1, juce::roundToInt (-dx * sn + dy * cs + h));
                dbd.setPixelColour (x, y, sbd.getPixelColour (sx, sy));
            }
        }
    }

    // 容量保护：超过上限整体清空（避免极端主题组合下内存失控）
    const juce::ScopedLock sl (rotLock_);
    if (rotCache_.size() >= 4096)
        rotCache_.clear();
    rotCache_.emplace (key, std::move (rotated));
}

float EmojiAtlas::duotoneStrength()
{
    using T = PinkXP::ThemeId;
    switch (PinkXP::getCurrentTheme().id)
    {
        case T::winXP:                  return 0.35f;
        case T::voidGrey:
        case T::paperGrey:              return 1.0f;
        default:                        return 0.55f;
    }
}

void EmojiAtlas::timerCallback()
{
    // 启动约 1 秒后再开始，避开初始化抢帧（B-05）；30Hz 下每 tick 约 1/30 秒
    prebakeStartDelay_ += 1.0 / 30.0;
    if (prebakeStartDelay_ < 1.0)
        return;

    const int total = (int) EmojiId::Count * (int) Size::Count;
    // 性能优化 P7：每 tick 烘焙数量 8 → 3，且 timer 降为 30Hz，
    //   消除启动初期 D2D 烘焙抢帧（日志实测烘焙期间 fps 跌到 31）。
    int budget = 3;

    // 1) 原图预烘焙
    while (budget > 0 && prebakeCursor_ < total)
    {
        bakeOne ((EmojiId) (prebakeCursor_ / (int) Size::Count),
                 (Size)     (prebakeCursor_ % (int) Size::Count));
        ++prebakeCursor_;
        --budget;
    }

    // 2) 主题版本（仅对已烘焙成功的条目）
    while (budget > 0 && themedCursor_ < total)
    {
        const int id = themedCursor_ / (int) Size::Count;
        const int sz = themedCursor_ % (int) Size::Count;
        if (entries_[id][sz].ok)
            bakeThemedOne ((EmojiId) id, (Size) sz);
        ++themedCursor_;
        --budget;
    }

    if (prebakeCursor_ >= total && themedCursor_ >= total)
    {
        stopTimer();
        // 性能日志：预烘焙完成（一次性，记录总耗时供多轮比对）
        static bool logged = false;
        if (! logged)
        {
            logged = true;
            toyperf::log (juce::String ("atlas prebake done: ") + juce::String (total) + " entries");
        }
    }
}

// 主题版本：duotone（亮度 → 主题三色渐变，按强度混合）+ 1 逻辑像素描边（E.3.2）
void EmojiAtlas::bakeThemedOne (EmojiId id, Size size)
{
    ThemedEntry& t = themed_[(int) id][(int) size];
    if (t.baked)
        return;
    t.baked = true;

    const juce::Image& src = image (id, size);
    if (! src.isValid() || ! entries_[(int) id][(int) size].ok)
        return;

    const float s = duotoneStrength();
    const juce::Colour darkC  = PinkXP::pink700;
    const juce::Colour midC   = PinkXP::sel;
    const juce::Colour lightC = PinkXP::pink100;

    auto grad = [&](float L)
    {
        if (L < 0.5f) return darkC.interpolatedWith (midC, L / 0.5f);
        return midC.interpolatedWith (lightC, (L - 0.5f) / 0.5f);
    };

    const int w = src.getWidth(), h = src.getHeight();
    juce::Image out (juce::Image::ARGB, w, h, true);
    {
        const juce::Image::BitmapData sd (src, juce::Image::BitmapData::readOnly);
        const juce::Image::BitmapData od (out, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const juce::Colour c = sd.getPixelColour (x, y);
                const float L = (0.299f * (float) c.getRed() + 0.587f * (float) c.getGreen()
                                 + 0.114f * (float) c.getBlue()) / 255.0f;
                juce::Colour nc = c.interpolatedWith (grad (L), s);
                nc = nc.withAlpha (c.getAlpha());
                od.setPixelColour (x, y, nc);
            }
        }
    }

    // 贴纸描边：alpha 膨胀 1 逻辑像素，描边色 ink（浅色画布）
    const int rad = juce::jmax (1, juce::roundToInt (PinkXP::uiScale()));
    const juce::Colour stroke = PinkXP::ink;
    juce::Image strokeLayer (juce::Image::ARGB, w, h, true);
    {
        const juce::Image::BitmapData sd (out, juce::Image::BitmapData::readOnly);
        const juce::Image::BitmapData od (strokeLayer, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const juce::Colour c = sd.getPixelColour (x, y);
                if (c.getAlpha() > 128) { od.setPixelColour (x, y, c); continue; }

                bool edge = false;
                for (int dy = -rad; dy <= rad && ! edge; ++dy)
                    for (int dx = -rad; dx <= rad && ! edge; ++dx)
                    {
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && nx < w && ny >= 0 && ny < h
                            && sd.getPixelColour (nx, ny).getAlpha() > 128)
                            edge = true;
                    }
                if (edge)
                    od.setPixelColour (x, y, stroke.withAlpha ((juce::uint8) 220));
                else
                    od.setPixelColour (x, y, juce::Colours::transparentBlack);
            }
        }
    }

    t.ok  = true;
    t.img = std::move (strokeLayer);
}
