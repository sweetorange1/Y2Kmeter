#include "source/ui/toys/ToyKit.h"
#include "source/i18n/Localisation.h"
#include "source/Y2KLogging.h"
#include <initializer_list>

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

    static_assert ((int) EmojiId::Count <= y2ktoy::EmojiAtlasLayout::kMaxEmojiSlots,
                   "y2ktoy::EmojiAtlasLayout::kMaxEmojiSlots must cover EmojiId::Count");

    void imageToPremulRgba (const juce::Image& img, std::vector<uint8_t>& out)
    {
        const int w = img.getWidth();
        const int h = img.getHeight();
        out.resize ((size_t) w * (size_t) h * 4u);
        const juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < h; ++y)
        {
            uint8_t* dst = out.data() + (size_t) y * (size_t) w * 4u;
            for (int x = 0; x < w; ++x)
            {
                const juce::Colour c = bd.getPixelColour (x, y);
                const uint32_t a = (uint32_t) c.getAlpha();
                dst[x * 4 + 0] = (uint8_t) (((uint32_t) c.getRed()   * a + 127u) / 255u);
                dst[x * 4 + 1] = (uint8_t) (((uint32_t) c.getGreen() * a + 127u) / 255u);
                dst[x * 4 + 2] = (uint8_t) (((uint32_t) c.getBlue()  * a + 127u) / 255u);
                dst[x * 4 + 3] = (uint8_t) a;
            }
        }
    }

    std::vector<EmojiId> priorityPoolForTheme (PinkXP::ThemeId id)
    {
        using T = PinkXP::ThemeId;
        std::vector<EmojiId> ids;
        auto push = [&] (std::initializer_list<EmojiId> list)
        {
            for (EmojiId e : list)
                ids.push_back (e);
        };

        switch (id)
        {
            case T::blackPink:
                push ({ EmojiId::Heart, EmojiId::SparklingHeart, EmojiId::Star,
                        EmojiId::Butterfly, EmojiId::Bolt, EmojiId::Cherry,
                        EmojiId::Vhs, EmojiId::Sparkles, EmojiId::Cd, EmojiId::BlackHeart });
                break;
            case T::bubblegum:
                push ({ EmojiId::SparklingHeart, EmojiId::Strawberry, EmojiId::Ribbon,
                        EmojiId::Lollipop, EmojiId::Candy, EmojiId::Blossom, EmojiId::Star });
                break;
            case T::starlight:
                push ({ EmojiId::Star, EmojiId::Moon, EmojiId::Earth, EmojiId::Sparkles,
                        EmojiId::Dizzy, EmojiId::Comet, EmojiId::ShootingStar });
                break;
            case T::cyberLilac:
                push ({ EmojiId::Alien, EmojiId::Bolt, EmojiId::Cd, EmojiId::CrystalBall,
                        EmojiId::Headphone, EmojiId::Joystick, EmojiId::SpaceInvader });
                break;
            case T::tangerinePop:
                push ({ EmojiId::Orange, EmojiId::Sun, EmojiId::IceCream,
                        EmojiId::Sunglasses, EmojiId::Cocktail, EmojiId::Umbrella });
                break;
            case T::aquaPearl:
                push ({ EmojiId::Shell, EmojiId::Drop, EmojiId::Fish, EmojiId::OceanWave,
                        EmojiId::Dolphin, EmojiId::Crab });
                break;
            case T::matchaSoda:
                push ({ EmojiId::CupWithStraw, EmojiId::Leaf, EmojiId::GreenApple,
                        EmojiId::Tea, EmojiId::Herb, EmojiId::Clover });
                break;
            case T::jungle:
                push ({ EmojiId::Leaf, EmojiId::Frog, EmojiId::Butterfly,
                        EmojiId::Mushroom, EmojiId::PalmTree, EmojiId::Lizard });
                break;
            case T::winXP:
                push ({ EmojiId::Floppy, EmojiId::Cd, EmojiId::Mouse, EmojiId::Hourglass,
                        EmojiId::Folder, EmojiId::Desktop, EmojiId::Laptop });
                break;
            case T::crimsonNoir:
                push ({ EmojiId::Skull, EmojiId::Bat, EmojiId::Rose,
                        EmojiId::WiltedRose, EmojiId::Spider, EmojiId::BlackHeart });
                break;
            case T::voidGrey:
            case T::paperGrey:
                push ({ EmojiId::BlackSquare, EmojiId::WhiteSquare, EmojiId::ButtonSquare,
                        EmojiId::FrameSquare, EmojiId::Plus, EmojiId::Check });
                break;
            default:
                push ({ EmojiId::Heart, EmojiId::SparklingHeart, EmojiId::Star, EmojiId::GlowStar,
                        EmojiId::Sparkles, EmojiId::Note, EmojiId::Notes, EmojiId::Sunglasses,
                        EmojiId::HeartEyes, EmojiId::Dizzy, EmojiId::Fire, EmojiId::PartyPopper, EmojiId::Gem });
                break;
        }

        // Always prioritize active ToyRole emojis + PartyPopper (confetti rain) + Tier-1 fallbacks
        for (int r = 0; r < (int) ToyRole::Count; ++r)
            ids.push_back (toyRoleEmoji ((ToyRole) r));
        push ({ EmojiId::PartyPopper, EmojiId::Sparkles, EmojiId::Star });
        return ids;
    }

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
    static void logToyEmoji (const juce::String& msg)
    {
        const juce::String s = "[ToyEmoji] " + msg;
        OutputDebugStringW (s.toWideCharPointer());
        juce::Logger::writeToLog (s);
    }
#endif
}

// ==========================================================
// EmojiAtlas::WinEmojiRenderer —— 3 档位复用 D2D1/WIC 软渲染器
//   每个线程独享自己的 COM 工厂 + 3 套常驻 (TextFormat, WICBitmap,
//   WicBitmapRenderTarget, White/Black Brush) —— 291 次烘焙仅创建 3 个 RT！
//   析构时严格先释放全部 ComPtr，再按需调用 CoUninitialize()。
// ==========================================================
struct EmojiAtlas::WinEmojiRenderer
{
#if JUCE_WINDOWS
    struct SizeSlot
    {
        int px = 0;
        Microsoft::WRL::ComPtr<IDWriteTextFormat>    format;
        Microsoft::WRL::ComPtr<IWICBitmap>           wicBitmap;
        Microsoft::WRL::ComPtr<ID2D1RenderTarget>    rt;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> whiteBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> blackBrush;
        bool ready = false;

        void reset() noexcept
        {
            blackBrush.Reset();
            whiteBrush.Reset();
            rt.Reset();
            wicBitmap.Reset();
            format.Reset();
            px = 0;
            ready = false;
        }
    };

    Microsoft::WRL::ComPtr<IDWriteFactory>     dw;
    Microsoft::WRL::ComPtr<ID2D1Factory>       d2d;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    SizeSlot slots[(int) EmojiAtlas::Size::Count];
    bool ok = false;
    bool comShouldUninit = false;

    explicit WinEmojiRenderer (bool initComForThread)
    {
        if (initComForThread)
        {
            const HRESULT comHr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            comShouldUninit = (comHr == S_OK || comHr == S_FALSE);
            if (FAILED (comHr) && comHr != RPC_E_CHANGED_MODE)
            {
                logToyEmoji ("CoInitializeEx failed hr=" + juce::String::toHexString ((int) comHr));
                return;
            }
        }

        HRESULT hr = DWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED,
                                          __uuidof (IDWriteFactory),
                                          reinterpret_cast<IUnknown**> (dw.GetAddressOf()));
        if (FAILED (hr) || ! dw) { logToyEmoji ("DWrite factory failed hr=" + juce::String::toHexString ((int) hr)); return; }

        hr = D2D1CreateFactory (D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                __uuidof (ID2D1Factory), nullptr,
                                reinterpret_cast<void**> (d2d.GetAddressOf()));
        if (FAILED (hr) || ! d2d) { logToyEmoji ("D2D1 factory failed hr=" + juce::String::toHexString ((int) hr)); return; }

        hr = CoCreateInstance (CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                               __uuidof (IWICImagingFactory),
                               reinterpret_cast<void**> (wic.GetAddressOf()));
        if (FAILED (hr) || ! wic) { logToyEmoji ("WIC factory failed hr=" + juce::String::toHexString ((int) hr)); return; }

        ok = true;
        logToyEmoji (initComForThread ? "worker renderer ready (3x reusable WIC targets)"
                                      : "on-demand renderer ready (3x reusable WIC targets)");
    }

    ~WinEmojiRenderer()
    {
        for (auto& s : slots)
            s.reset();
        wic.Reset();
        d2d.Reset();
        dw.Reset();
        if (comShouldUninit)
            CoUninitialize();
    }

    bool ensureSlot (int sizeIndex, int px)
    {
        if (! ok || sizeIndex < 0 || sizeIndex >= (int) EmojiAtlas::Size::Count || px <= 0)
            return false;

        auto& slot = slots[sizeIndex];
        if (slot.ready && slot.px == px)
            return true;

        slot.reset();

        HRESULT hr = dw->CreateTextFormat (L"Segoe UI Emoji", nullptr,
                                           DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, (float) (px * 0.82f), L"en-US",
                                           slot.format.GetAddressOf());
        if (FAILED (hr) || ! slot.format) { slot.reset(); return false; }

        slot.format->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
        slot.format->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        hr = wic->CreateBitmap ((UINT) px, (UINT) px, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnDemand, slot.wicBitmap.GetAddressOf());
        if (FAILED (hr) || ! slot.wicBitmap) { slot.reset(); return false; }

        const auto rtp = D2D1::RenderTargetProperties (
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat (DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);

        hr = d2d->CreateWicBitmapRenderTarget (slot.wicBitmap.Get(), rtp, slot.rt.GetAddressOf());
        if (FAILED (hr) || ! slot.rt) { slot.reset(); return false; }

        hr = slot.rt->CreateSolidColorBrush (D2D1::ColorF (D2D1::ColorF::White), slot.whiteBrush.GetAddressOf());
        if (FAILED (hr) || ! slot.whiteBrush) { slot.reset(); return false; }

        hr = slot.rt->CreateSolidColorBrush (D2D1::ColorF (D2D1::ColorF::Black), slot.blackBrush.GetAddressOf());
        if (FAILED (hr) || ! slot.blackBrush) { slot.reset(); return false; }

        slot.px = px;
        slot.ready = true;
        return true;
    }

    juce::Image render (const WCHAR* utf16, int numChars, int sizeIndex, int px,
                        bool enableColorFont, bool useBlackBrush)
    {
        if (! ensureSlot (sizeIndex, px))
            return {};

        auto& slot = slots[sizeIndex];
        const UINT32 len = (UINT32) juce::jmax (0, numChars);

        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = dw->CreateTextLayout (utf16, len, slot.format.Get(), (float) px, (float) px,
                                           layout.GetAddressOf());
        if (FAILED (hr) || ! layout)
            return {};

        slot.rt->BeginDraw();
        slot.rt->Clear (D2D1::ColorF (0.0f, 0.0f, 0.0f, 0.0f));
        slot.rt->DrawTextLayout (D2D1::Point2F (0.0f, 0.0f), layout.Get(),
                                 useBlackBrush ? slot.blackBrush.Get() : slot.whiteBrush.Get(),
                                 enableColorFont ? D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
                                                 : D2D1_DRAW_TEXT_OPTIONS_NONE);
        hr = slot.rt->EndDraw();
        if (FAILED (hr))
        {
            if (hr == (HRESULT) D2DERR_RECREATE_TARGET)
                slot.reset();
            return {};
        }

        Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
        hr = slot.wicBitmap->Lock (nullptr, WICBitmapLockRead, lock.GetAddressOf());
        if (FAILED (hr) || ! lock)
            return {};

        UINT stride = 0, bufSize = 0;
        BYTE* buf = nullptr;
        hr = lock->GetStride (&stride);
        if (FAILED (hr) || stride < (UINT) px * 4u)
            return {};
        hr = lock->GetDataPointer (&bufSize, &buf);
        if (FAILED (hr) || buf == nullptr || bufSize < (UINT) px * stride)
            return {};

        juce::Image img (juce::Image::ARGB, px, px, true, juce::SoftwareImageType());
        {
            const juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);

            for (int y = 0; y < px; ++y)
            {
                const BYTE* src = buf + (size_t) y * stride;
                for (int x = 0; x < px; ++x)
                {
                    const int a = src[x * 4 + 3];
                    int r = src[x * 4 + 2];
                    int g = src[x * 4 + 1];
                    int b = src[x * 4 + 0];
                    if (a > 0 && a < 255)
                    {
                        r = juce::jmin (255, (r * 255) / a);
                        g = juce::jmin (255, (g * 255) / a);
                        b = juce::jmin (255, (b * 255) / a);
                    }
                    bd.setPixelColour (x, y, juce::Colour::fromRGBA (
                        (juce::uint8) r, (juce::uint8) g, (juce::uint8) b, (juce::uint8) a));
                }
            }
        }
        return img;
    }
#else
    explicit WinEmojiRenderer (bool) {}
#endif
};

// ==========================================================
// EmojiAtlas::PrebakeWorker —— Windows 低优先级后台预烘焙线程
//   零消息线程占用：先烘当前主题池（3 档位），再烘全量槽位，
//   切主题时由 invalidateThemed() 唤醒并按新 epoch 重跑。
// ==========================================================
class EmojiAtlas::PrebakeWorker final : public juce::Thread
{
public:
    explicit PrebakeWorker (EmojiAtlas& owner)
        : juce::Thread ("Y2K EmojiAtlas Prebake"), owner_ (owner)
    {}

    void run() override
    {
       #if JUCE_WINDOWS
        // 避开首帧窗体创建 / GL 初始化峰值（可随时被退出或切主题打断）
        if (wait (150) && threadShouldExit())
            return;
        if (threadShouldExit())
            return;

        // 线程栈上持有 COM 渲染器：确保退出 run() 前先释放全部 ComPtr 再 CoUninitialize()
        WinEmojiRenderer renderer (/*initComForThread=*/ true);
        if (! renderer.ok)
            return;

        uint32_t lastCompletedEpoch = 0;
        const int total = (int) EmojiId::Count * (int) EmojiAtlas::Size::Count;

        while (! threadShouldExit())
        {
            ThemeBakeParams params;
            std::vector<int> priorityOrder;
            {
                const juce::ScopedLock sl (owner_.workConfigLock_);
                params        = owner_.pendingParams_;
                priorityOrder = owner_.pendingPriorityOrder_;
            }

            if (params.epoch == lastCompletedEpoch)
            {
                wait (-1);
                continue;
            }

            const double t0 = juce::Time::getMillisecondCounterHiRes();
            bool aborted = false;

            auto bakeEntry = [&] (int id, int sz) -> bool
            {
                if (threadShouldExit()
                    || owner_.themeEpoch_.load (std::memory_order_acquire) != params.epoch)
                    return false;

                owner_.bakeBaseSlot ((EmojiId) id, (Size) sz, &renderer,
                                     /*allowJuceFallback=*/ false,
                                     /*publishBaseToGl=*/ false);

                if (threadShouldExit()
                    || owner_.themeEpoch_.load (std::memory_order_acquire) != params.epoch)
                    return false;

                if (owner_.baseState_[id][sz].load (std::memory_order_acquire) == 2)
                    owner_.bakeThemedSlot ((EmojiId) id, (Size) sz, params);
                return true;
            };

            // 1) 优先当前主题池 + ToyRole + PartyPopper（~16 个 × 3 尺寸）
            for (int flat : priorityOrder)
            {
                if (! bakeEntry (flat / (int) Size::Count, flat % (int) Size::Count))
                {
                    aborted = true;
                    break;
                }
            }

            // 2) 全量剩余条目
            if (! aborted)
            {
                for (int flat = 0; flat < total; ++flat)
                {
                    if (! bakeEntry (flat / (int) Size::Count, flat % (int) Size::Count))
                    {
                        aborted = true;
                        break;
                    }
                }
            }

            if (! aborted && ! threadShouldExit()
                && owner_.themeEpoch_.load (std::memory_order_acquire) == params.epoch)
            {
                const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                owner_.prebakeMs_.store (ms, std::memory_order_release);
                owner_.prebakeDone_.store (true, std::memory_order_release);
                lastCompletedEpoch = params.epoch;
                TOYPERF_LOG ("atlas worker prebake done: " + juce::String (total)
                             + " entries in " + juce::String (ms, 1) + "ms");
            }
        }
       #endif
    }

private:
    EmojiAtlas& owner_;
};

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

    // 优先排队当前主题池 + ToyRole，保证首秒点击即命中已烘焙槽位
    rebuildPriorityOrder();

    const uint32_t initEpoch = themeEpoch_.load (std::memory_order_acquire);
    {
        const juce::ScopedLock sl (workConfigLock_);
        pendingParams_        = captureCurrentThemeParams (initEpoch);
        pendingPriorityOrder_ = priorityOrder_;
    }

#if JUCE_WINDOWS
    // Windows：使用低优先级后台线程 + 3 套复用 D2D/WIC 软目标预烘焙，零消息线程抢帧
    Y2K_LOG ("[toy] EmojiAtlas starting background prebake worker");
    worker_ = std::make_unique<PrebakeWorker> (*this);
    worker_->startThread (juce::Thread::Priority::low);
#else
    // macOS / Linux：消息线程分帧预烘焙（CoreText / FreeType 需留在消息线程）
    Y2K_LOG ("[toy] EmojiAtlas startTimer begin");
    startTimerHz (30);
#endif
    Y2K_LOG ("[toy] EmojiAtlas ctor done");
}

EmojiAtlas::~EmojiAtlas()
{
    stopTimer();
    release();
}

EmojiAtlas::ThemeBakeParams EmojiAtlas::captureCurrentThemeParams (uint32_t epoch) const
{
    ThemeBakeParams p;
    p.themeId         = PinkXP::getCurrentTheme().id;
    p.duotoneStrength = duotoneStrengthFor (p.themeId);
    p.darkC           = PinkXP::pink700;
    p.midC            = PinkXP::sel;
    p.lightC          = PinkXP::pink100;
    p.stroke          = PinkXP::ink;
    p.strokeRadius    = juce::jmax (1, juce::roundToInt (PinkXP::uiScale()));
    p.epoch           = epoch;
    return p;
}

EmojiAtlas::WinEmojiRenderer* EmojiAtlas::getOrCreateOnDemandWinRenderer()
{
   #if JUCE_WINDOWS
    if (onDemandWinRenderer_ == nullptr)
        onDemandWinRenderer_ = std::make_unique<WinEmojiRenderer> (/*initComForThread=*/ false);
    return onDemandWinRenderer_.get();
   #else
    return nullptr;
   #endif
}

void EmojiAtlas::rebuildPriorityOrder()
{
    priorityOrder_.clear();
    bool seen[(int) EmojiId::Count] {};
    const auto pool = priorityPoolForTheme (PinkXP::getCurrentTheme().id);
    for (EmojiId id : pool)
    {
        const int ei = (int) id;
        if (ei < 0 || ei >= (int) EmojiId::Count || seen[ei])
            continue;
        seen[ei] = true;
        for (int s = 0; s < (int) Size::Count; ++s)
            priorityOrder_.push_back (ei * (int) Size::Count + s);
    }
    priorityBaseCursor_   = 0;
    priorityThemedCursor_ = 0;
}

void EmojiAtlas::release()
{
    stopTimer();

    if (worker_ != nullptr)
    {
        worker_->signalThreadShouldExit();
        worker_->notify();
        worker_->stopThread (3000);
        worker_.reset();
    }

    onDemandWinRenderer_.reset();

    {
        const juce::ScopedLock sl (bakeLock_);
        for (int e = 0; e < (int) EmojiId::Count; ++e)
            for (int s = 0; s < (int) Size::Count; ++s)
            {
                entries_[e][s] = Entry{};
                themed_[e][s]  = ThemedEntry{};
                baseState_[e][s].store (0, std::memory_order_relaxed);
                themedReadyEpoch_[e][s].store (0, std::memory_order_relaxed);
            }
    }
    {
        const juce::ScopedLock sl (rotLock_);
        rotCache_.clear();
    }
    {
        const juce::ScopedLock sl (glAtlasLock_);
        for (int e = 0; e < (int) EmojiId::Count; ++e)
            for (int s = 0; s < (int) Size::Count; ++s)
            {
                glSlotPixels_[e][s].clear();
                glSlotPixels_[e][s].shrink_to_fit();
                glSlotW_[e][s] = 0;
                glSlotH_[e][s] = 0;
                glSlotHasThemed_[e][s] = false;
                glSlotDirty_[e][s] = false;
            }
    }
    partyGlowSprite_ = juce::Image();
    prebakeDone_.store (false, std::memory_order_release);
    prebakeMs_.store (0.0, std::memory_order_release);
    prebakeCursor_ = 0;
    themedCursor_  = 0;
    priorityBaseCursor_   = 0;
    priorityThemedCursor_ = 0;
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

bool EmojiAtlas::has (EmojiId id) const noexcept
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return false;
    return baseState_[i][(int) Size::M].load (std::memory_order_acquire) == 2;
}

bool EmojiAtlas::isBaked (EmojiId id, Size size) const noexcept
{
    const int i = (int) id;
    const int s = (int) size;
    if (i < 0 || i >= (int) EmojiId::Count || s < 0 || s >= (int) Size::Count)
        return false;
    return baseState_[i][s].load (std::memory_order_acquire) != 0;
}

void EmojiAtlas::bakeOne (EmojiId id, Size size)
{
    bakeBaseSlot (id, size, getOrCreateOnDemandWinRenderer(),
                  /*allowJuceFallback=*/ true, /*publishBaseToGl=*/ true);
}

void EmojiAtlas::bakeBaseSlot (EmojiId id, Size size, WinEmojiRenderer* winRenderer,
                               bool allowJuceFallback, bool publishBaseToGl)
{
    const int i = (int) id;
    const int s = (int) size;
    if (i < 0 || i >= (int) EmojiId::Count || s < 0 || s >= (int) Size::Count)
        return;

    if (baseState_[i][s].load (std::memory_order_acquire) != 0)
        return;

    const int logical = logicalSize (size);
    const int px = logical * 2 + 2;

    bool ok = false;
    juce::Image bakedImg;

#if JUCE_WINDOWS
    const char16_t* u16src = kSpecs[i].utf16;
    int u16len = 0;
    while (u16src[u16len]) ++u16len;

    if (winRenderer != nullptr && winRenderer->ok)
    {
        // 首选：D2D COLR 彩色渲染（复用 3 档位 WIC 目标，无每字形 RT 分配）
        juce::Image color = winRenderer->render ((const WCHAR*) u16src, u16len, s, px,
                                                 /*enableColorFont=*/ true, /*useBlackBrush=*/ false);
        if (imageHasContent (color, 0.015f) && imageIsColorful (color))
        {
            ok       = true;
            bakedImg = std::move (color);
        }
        else if (imageHasContent (color, 0.015f))
        {
            // 纯单色符号（⬛/⬜/➕/✔️ 等）：用同一 D2D/WIC 目标以单色白/黑双色差分探测，
            // 完全不碰 JUCE Graphics，后台线程 100% 安全且保持 voidGrey/paperGrey 视觉一致。
            juce::Image monoW = winRenderer->render ((const WCHAR*) u16src, u16len, s, px,
                                                     /*enableColorFont=*/ false, /*useBlackBrush=*/ false);
            juce::Image monoB = winRenderer->render ((const WCHAR*) u16src, u16len, s, px,
                                                     /*enableColorFont=*/ false, /*useBlackBrush=*/ true);
            if (probe (monoW, monoB, 0.12f))
            {
                ok       = true;
                bakedImg = std::move (monoW);
            }
        }
    }
    else if (allowJuceFallback)
    {
        auto renderGlyph = [&](juce::Colour colour)
        {
            juce::Image img (juce::Image::ARGB, px, px, true, juce::SoftwareImageType());
            juce::Graphics g (img);
            g.setFont (juce::FontOptions (emojiFamily, (float) (px * 0.82f), juce::Font::plain));
            g.setColour (colour);
            g.drawText (y2k::tr (juce::String (juce::CharPointer_UTF16 (
                            reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (u16src)))),
                        juce::Rectangle<int> (0, 0, px, px),
                        juce::Justification::centred, false);
            return img;
        };

        juce::Image white = renderGlyph (juce::Colours::white);
        juce::Image black = renderGlyph (juce::Colours::black);
        ok = probe (white, black, 0.12f);
        if (ok)
            bakedImg = std::move (white);
    }
#else
    juce::ignoreUnused (winRenderer, allowJuceFallback);
    auto renderGlyph = [&](juce::Colour colour)
    {
        juce::Image img (juce::Image::ARGB, px, px, true, juce::SoftwareImageType());
        juce::Graphics g (img);
        g.setFont (juce::FontOptions (emojiFamily, (float) (px * 0.82f), juce::Font::plain));
        g.setColour (colour);
        g.drawText (y2k::tr (emojiUtf8 (id)), juce::Rectangle<int> (0, 0, px, px),
                    juce::Justification::centred, false);
        return img;
    };

    juce::Image white = renderGlyph (juce::Colours::white);

#if JUCE_MAC
    ok = imageHasContent (white, 0.12f);
#else
    juce::Image black = renderGlyph (juce::Colours::black);
    ok = probe (white, black, 0.12f);
#endif
    if (ok)
        bakedImg = std::move (white);
#endif

    juce::Image publishedImg;
    {
        const juce::ScopedLock sl (bakeLock_);
        Entry& entry = entries_[i][s];
        if (entry.baked)
            return;
        entry.baked = true;
        entry.ok    = ok;
        if (ok)
        {
            entry.img    = std::move (bakedImg);
            publishedImg = entry.img;
        }
        else
        {
            entry.img = juce::Image();
        }
        baseState_[i][s].store (ok ? 2 : 1, std::memory_order_release);
    }

    if (ok && publishBaseToGl)
        enqueueGlSlotLocked (i, s, publishedImg, false);
}

void EmojiAtlas::publishGlSlotPremul (int emojiIndex, int sizeIndex, int w, int h,
                                      std::vector<uint8_t>&& premulRgba,
                                      bool isThemed, uint32_t expectedEpoch)
{
    if (! y2ktoy::EmojiAtlasLayout::isValidSlot (emojiIndex, sizeIndex))
        return;
    const int expectedSz = y2ktoy::EmojiAtlasLayout::imageSizeForTier (sizeIndex);
    if (w != expectedSz || h != expectedSz || premulRgba.size() < (size_t) (w * h * 4))
        return;

    const juce::ScopedLock sl (glAtlasLock_);
    if (isThemed && expectedEpoch != 0
        && themeEpoch_.load (std::memory_order_acquire) != expectedEpoch)
        return;
    if (! isThemed && glSlotHasThemed_[emojiIndex][sizeIndex])
        return;

    glSlotPixels_[emojiIndex][sizeIndex]    = std::move (premulRgba);
    glSlotW_[emojiIndex][sizeIndex]         = w;
    glSlotH_[emojiIndex][sizeIndex]         = h;
    glSlotHasThemed_[emojiIndex][sizeIndex] = isThemed;
    glSlotDirty_[emojiIndex][sizeIndex]     = true;
}

void EmojiAtlas::enqueueGlSlotLocked (int emojiIndex, int sizeIndex, const juce::Image& src, bool isThemed)
{
    if (! src.isValid())
        return;
    if (! y2ktoy::EmojiAtlasLayout::isValidSlot (emojiIndex, sizeIndex))
        return;
    const int expectedSz = y2ktoy::EmojiAtlasLayout::imageSizeForTier (sizeIndex);
    if (src.getWidth() != expectedSz || src.getHeight() != expectedSz)
        return;

    std::vector<uint8_t> rgba;
    imageToPremulRgba (src, rgba);
    publishGlSlotPremul (emojiIndex, sizeIndex, src.getWidth(), src.getHeight(),
                         std::move (rgba), isThemed, 0u);
}

bool EmojiAtlas::ensureBakedForGl (EmojiId id, Size size)
{
    const int i = (int) id;
    const int s = (int) size;
    if (i < 0 || i >= (int) EmojiId::Count || s < 0 || s >= (int) Size::Count)
        return false;

    const uint32_t curEpoch = themeEpoch_.load (std::memory_order_acquire);

    // 99.99% 热路径：全无锁原子读取（零锁、零分配）
    const uint8_t st = baseState_[i][s].load (std::memory_order_acquire);
    if (st == 2 && themedReadyEpoch_[i][s].load (std::memory_order_acquire) == curEpoch)
        return true;

    auto ensureSingle = [&] (EmojiId targetId) -> bool
    {
        const int ti = (int) targetId;
        if (ti < 0 || ti >= (int) EmojiId::Count)
            return false;

        if (baseState_[ti][s].load (std::memory_order_acquire) == 0)
        {
            bakeBaseSlot (targetId, size, getOrCreateOnDemandWinRenderer(),
                          /*allowJuceFallback=*/ true, /*publishBaseToGl=*/ false);
        }

        if (baseState_[ti][s].load (std::memory_order_acquire) != 2)
            return false;

        if (themedReadyEpoch_[ti][s].load (std::memory_order_acquire) != curEpoch)
        {
            const auto params = captureCurrentThemeParams (curEpoch);
            bakeThemedSlot (targetId, size, params);
        }
        return true;
    };

    if (ensureSingle (id))
        return true;

    const EmojiId fb = emojiFallback (id);
    if (fb != id)
        return ensureSingle (fb);
    return false;
}

bool EmojiAtlas::resolveGlSlot (EmojiId id, Size size, y2ktoy::AtlasSlot& outSlot) const noexcept
{
    outSlot = {};
    const int i = (int) id;
    const int s = (int) size;
    if (i < 0 || i >= (int) EmojiId::Count || s < 0 || s >= (int) Size::Count)
        return false;

    if (baseState_[i][s].load (std::memory_order_acquire) == 2)
    {
        outSlot = y2ktoy::EmojiAtlasLayout::slotFor (i, s);
        return outSlot.valid;
    }

    const EmojiId fb = emojiFallback (id);
    const int fi = (int) fb;
    if (fb != id && fi >= 0 && fi < (int) EmojiId::Count
        && baseState_[fi][s].load (std::memory_order_acquire) == 2)
    {
        outSlot = y2ktoy::EmojiAtlasLayout::slotFor (fi, s);
        return outSlot.valid;
    }
    return false;
}

void EmojiAtlas::requestFullGlAtlasReupload()
{
    const juce::ScopedLock sl (glAtlasLock_);
    for (int e = 0; e < (int) EmojiId::Count; ++e)
    {
        for (int s = 0; s < (int) Size::Count; ++s)
        {
            const auto& pix = glSlotPixels_[e][s];
            const int w = glSlotW_[e][s];
            const int h = glSlotH_[e][s];
            glSlotDirty_[e][s] = (w > 0 && h > 0 && pix.size() >= (size_t) (w * h * 4));
        }
    }
}

void EmojiAtlas::drainGlAtlasUploads (std::vector<GlAtlasUpload>& out, uint32_t& outGen)
{
    const juce::ScopedLock sl (glAtlasLock_);
    outGen = glAtlasGen_.load (std::memory_order_acquire);
    out.clear();
    for (int e = 0; e < (int) EmojiId::Count; ++e)
    {
        for (int s = 0; s < (int) Size::Count; ++s)
        {
            if (! glSlotDirty_[e][s])
                continue;
            glSlotDirty_[e][s] = false;
            const auto& pix = glSlotPixels_[e][s];
            const int w = glSlotW_[e][s];
            const int h = glSlotH_[e][s];
            if (w > 0 && h > 0 && pix.size() >= (size_t) (w * h * 4))
            {
                GlAtlasUpload up;
                up.emojiIndex = e;
                up.sizeIndex  = s;
                up.width      = w;
                up.height     = h;
                up.premulRgba = pix;
                out.push_back (std::move (up));
            }
        }
    }
}

const juce::Image& EmojiAtlas::partyGlowSprite()
{
    if (! partyGlowSprite_.isValid())
    {
        constexpr int kSz = 32;
        partyGlowSprite_ = juce::Image (juce::Image::ARGB, kSz, kSz, true, juce::SoftwareImageType());
        const juce::Image::BitmapData bd (partyGlowSprite_, juce::Image::BitmapData::writeOnly);
        const float c = (float) kSz * 0.5f;
        for (int y = 0; y < kSz; ++y)
        {
            for (int x = 0; x < kSz; ++x)
            {
                const float dx = ((float) x + 0.5f - c) / c;
                const float dy = ((float) y + 0.5f - c) / c;
                const float d  = std::sqrt (dx * dx + dy * dy);
                float a = 0.0f;
                if (d < 1.0f)
                {
                    const float t = juce::jlimit (0.0f, 1.0f, (d - 0.86f) / 0.14f);
                    const float smooth = t * t * (3.0f - 2.0f * t);
                    a = 1.0f - smooth;
                }
                bd.setPixelColour (x, y, juce::Colours::white.withAlpha (a));
            }
        }
    }
    return partyGlowSprite_;
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

juce::Image EmojiAtlas::image (EmojiId id, Size size)
{
    const int i = (int) id;
    const int s = (int) size;
    if (i < 0 || i >= (int) EmojiId::Count || s < 0 || s >= (int) Size::Count)
        return {};

    const juce::ScopedLock sl (bakeLock_);
    const Entry& entry = entries_[i][s];
    if (entry.ok)
        return entry.img;

    // 探测失败 → 最多回退一层（避免 Star ↔ Sparkles 互指死循环）
    const EmojiId fb = emojiFallback (id);
    const int fi = (int) fb;
    if (fb != id && fi >= 0 && fi < (int) EmojiId::Count && entries_[fi][s].ok)
        return entries_[fi][s].img;

    return {};
}

juce::Image EmojiAtlas::themedImage (EmojiId id, Size size)
{
    const int i = (int) id;
    const int s = (int) size;
    if (i < 0 || i >= (int) EmojiId::Count || s < 0 || s >= (int) Size::Count)
        return image (id, size);

    const uint32_t curEpoch = themeEpoch_.load (std::memory_order_acquire);
    {
        const juce::ScopedLock sl (bakeLock_);
        const ThemedEntry& t = themed_[i][s];
        if (t.ok && t.epoch == curEpoch)
            return t.img;
    }

    // 主题版本未就绪 → 回退原图
    return image (id, size);
}

void EmojiAtlas::invalidateThemed()
{
    const uint32_t newEpoch = themeEpoch_.fetch_add (1, std::memory_order_acq_rel) + 1;
    prebakeDone_.store (false, std::memory_order_release);

    {
        const juce::ScopedLock sl (bakeLock_);
        for (int e = 0; e < (int) EmojiId::Count; ++e)
            for (int s = 0; s < (int) Size::Count; ++s)
            {
                themed_[e][s] = ThemedEntry{};
                themedReadyEpoch_[e][s].store (0, std::memory_order_relaxed);
            }
    }
    themedCursor_ = 0;

    {
        const juce::ScopedLock sl (glAtlasLock_);
        for (int e = 0; e < (int) EmojiId::Count; ++e)
            for (int s = 0; s < (int) Size::Count; ++s)
                glSlotHasThemed_[e][s] = false;
    }

    // 旋转分桶缓存也随之失效（主题色变了）
    {
        const juce::ScopedLock sl (rotLock_);
        rotCache_.clear();
    }

    rebuildPriorityOrder();

    {
        const juce::ScopedLock sl (workConfigLock_);
        pendingParams_        = captureCurrentThemeParams (newEpoch);
        pendingPriorityOrder_ = priorityOrder_;
    }

#if JUCE_WINDOWS
    if (worker_ != nullptr)
    {
        if (! worker_->isThreadRunning())
            worker_->startThread (juce::Thread::Priority::low);
        worker_->notify();
    }
#else
    if (! isTimerRunning())
        startTimerHz (30);
#endif
}

int EmojiAtlas::bucketOf (float angleRad)
{
    const float step = 6.2831853f / (float) kRotBuckets;
    int bucket = (int) std::llround (angleRad / step) % kRotBuckets;
    if (bucket < 0)
        bucket += kRotBuckets;
    return bucket;
}

static size_t rotKey (int id, int size, float angleRad, bool fit, int scaleBucket)
{
    return ((size_t) scaleBucket << 20)
         | ((size_t) id << 8)
         | (fit ? (size_t) 0x40 : (size_t) 0)
         | ((size_t) size << 4)
         | (size_t) EmojiAtlas::bucketOf (angleRad);
}

juce::Image EmojiAtlas::rotatedCached (EmojiId id, Size size, float angleRad)
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return {};

    // paint 路径：只做无锁快查（值拷贝，引用计数，零烘焙、零风险）
    const int scaleBucket = juce::jlimit (50, 400, juce::roundToInt (physicalScale_ * 100.0f));
    const size_t key = rotKey (i, (int) size, angleRad, fitDrawSize_, scaleBucket);
    const juce::ScopedLock sl (rotLock_);
    auto it = rotCache_.find (key);
    if (it != rotCache_.end())
        return it->second;
    return {};
}

bool EmojiAtlas::ensureRotated (EmojiId id, Size size, float angleRad)
{
    const int i = (int) id;
    if (i < 0 || i >= (int) EmojiId::Count)
        return true;   // 无效 id：无需重试

    const bool fit = fitDrawSize_;
    const int scaleBucket = juce::jlimit (50, 400, juce::roundToInt (physicalScale_ * 100.0f));
    const size_t key = rotKey (i, (int) size, angleRad, fit, scaleBucket);
    {
        const juce::ScopedLock sl (rotLock_);
        if (rotCache_.find (key) != rotCache_.end())
            return true;
    }

    // 性能优化 P11：烘焙节流（每 16ms 窗口最多 6 张），
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
        if (bakedInWindow >= 6)   // Win 性能优化：2 → 6（fit 模式单张 <0.1ms，一次点击要补 ~20 张）
            return false;
        ++bakedInWindow;
    }

    // 原图尚未烘焙完成（预烘焙 timer 未走到该条目）→ 本轮跳过，
    //   绝不在这里触发 bakeOne（D2D 工厂是 SINGLE_THREADED，只能由消息线程 timer 独占使用）
    if (! isBaked (id, size))
        return false;

    // step 阶段烘焙（不嵌套在 paint 回调里）：以主题版（或原图）为源
    const juce::Image& src = themedImage (id, size);
    if (! src.isValid() || src.getWidth() <= 0)
        return true;

    // 自写最邻近采样旋转（固定 w×w 次循环，不依赖 JUCE 软件渲染的 transform 路径，
    //   从机制上杜绝渲染层死循环；24~48px 小图每次烘焙 ~百微秒级）
    const juce::Image srcCopy = src;   // BitmapData 需要非 const Image&
    const int w = srcCopy.getWidth();
    const int outW = fit ? juce::jmax (8, juce::roundToInt ((float) drawSize (size) * physicalScale_)) : w;
    juce::Image rotated (juce::Image::ARGB, outW, outW, true, juce::SoftwareImageType());
    {
        const juce::Image::BitmapData sbd (const_cast<juce::Image&> (srcCopy), 0, 0, w, w,
                                           juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dbd (rotated, 0, 0, outW, outW, juce::Image::BitmapData::writeOnly);

        const float step = 6.2831853f / (float) kRotBuckets;
        const int bucket = (int) (key & 0xF);
        const float h = (float) w * 0.5f;
        const float ang = (float) bucket * step;
        const float cs = std::cos (ang);
        const float sn = std::sin (ang);

        if (! fit)
        {
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
        else
        {
            // 直接烘焙成屏幕尺寸：每个目标像素 2×2 子采样（预乘空间平均）= 等价于原先
            //   "2x 图 + GPU 线性缩小"的抗锯齿效果，且 paint 时无需任何缩放。
            const float k = (float) w / (float) outW;
            const float oh = (float) outW * 0.5f;
            for (int y = 0; y < outW; ++y)
            {
                for (int x = 0; x < outW; ++x)
                {
                    int sa = 0, sr = 0, sg = 0, sb = 0;
                    for (int sj = 0; sj < 2; ++sj)
                    {
                        for (int si = 0; si < 2; ++si)
                        {
                            const float dx = ((float) x + ((float) si + 0.5f) * 0.5f - oh) * k;
                            const float dy = ((float) y + ((float) sj + 0.5f) * 0.5f - oh) * k;
                            const int sx = juce::jlimit (0, w - 1, (int) std::floor (dx * cs + dy * sn + h));
                            const int sy = juce::jlimit (0, w - 1, (int) std::floor (-dx * sn + dy * cs + h));
                            const juce::PixelARGB p = sbd.getPixelColour (sx, sy).getPixelARGB();
                            sa += p.getAlpha(); sr += p.getRed(); sg += p.getGreen(); sb += p.getBlue();
                        }
                    }
                    const juce::PixelARGB avg ((juce::uint8) (sa / 4), (juce::uint8) (sr / 4),
                                               (juce::uint8) (sg / 4), (juce::uint8) (sb / 4));
                    dbd.setPixelColour (x, y, juce::Colour (avg));
                }
            }
        }
    }

    // 容量保护：超过上限整体清空（避免极端主题组合下内存失控）
    const juce::ScopedLock sl (rotLock_);
    if (rotCache_.size() >= 4096)
        rotCache_.clear();
    rotCache_.emplace (key, std::move (rotated));
    return true;
}
float EmojiAtlas::duotoneStrengthFor (PinkXP::ThemeId themeId) noexcept
{
    using T = PinkXP::ThemeId;
    switch (themeId)
    {
        case T::winXP:                  return 0.35f;
        case T::voidGrey:
        case T::paperGrey:              return 1.0f;
        default:                        return 0.55f;
    }
}

float EmojiAtlas::duotoneStrength()
{
    return duotoneStrengthFor (PinkXP::getCurrentTheme().id);
}

void EmojiAtlas::timerCallback()
{
#if JUCE_WINDOWS
    stopTimer();
    return;
#else
    // 启动约 1 秒后再开始，避开初始化抢帧（B-05）；30Hz 下每 tick 约 1/30 秒
    prebakeStartDelay_ += 1.0 / 30.0;
    if (prebakeStartDelay_ < 1.0)
        return;

    const int total = (int) EmojiId::Count * (int) Size::Count;
    const uint32_t curEpoch = themeEpoch_.load (std::memory_order_acquire);
    int budget = 3;

    // 0) 优先烘焙当前主题池（原图 → 主题版）
    while (budget > 0 && priorityBaseCursor_ < priorityOrder_.size())
    {
        const int flat = priorityOrder_[priorityBaseCursor_++];
        const int id   = flat / (int) Size::Count;
        const int sz   = flat % (int) Size::Count;
        if (baseState_[id][sz].load (std::memory_order_acquire) == 0)
        {
            bakeOne ((EmojiId) id, (Size) sz);
            --budget;
        }
    }
    while (budget > 0 && priorityThemedCursor_ < priorityOrder_.size())
    {
        const int flat = priorityOrder_[priorityThemedCursor_++];
        const int id   = flat / (int) Size::Count;
        const int sz   = flat % (int) Size::Count;
        if (baseState_[id][sz].load (std::memory_order_acquire) == 2
            && themedReadyEpoch_[id][sz].load (std::memory_order_acquire) != curEpoch)
        {
            bakeThemedOne ((EmojiId) id, (Size) sz);
            --budget;
        }
    }

    // 1) 全量原图预烘焙
    while (budget > 0 && prebakeCursor_ < total)
    {
        const int id = prebakeCursor_ / (int) Size::Count;
        const int sz = prebakeCursor_ % (int) Size::Count;
        ++prebakeCursor_;
        if (baseState_[id][sz].load (std::memory_order_acquire) == 0)
        {
            bakeOne ((EmojiId) id, (Size) sz);
            --budget;
        }
    }

    // 2) 全量主题版本（仅对已烘焙成功的条目）
    while (budget > 0 && themedCursor_ < total)
    {
        const int id = themedCursor_ / (int) Size::Count;
        const int sz = themedCursor_ % (int) Size::Count;
        ++themedCursor_;
        if (baseState_[id][sz].load (std::memory_order_acquire) == 2
            && themedReadyEpoch_[id][sz].load (std::memory_order_acquire) != curEpoch)
        {
            bakeThemedOne ((EmojiId) id, (Size) sz);
            --budget;
        }
    }

    if (priorityBaseCursor_ >= priorityOrder_.size()
        && priorityThemedCursor_ >= priorityOrder_.size()
        && prebakeCursor_ >= total
        && themedCursor_ >= total)
    {
        stopTimer();
        prebakeDone_.store (true, std::memory_order_release);
        static bool logged = false;
        if (! logged)
        {
            logged = true;
            TOYPERF_LOG (juce::String ("atlas prebake done: ") + juce::String (total) + " entries");
        }
    }
#endif
}

// 主题版本：duotone（亮度 → 主题三色渐变，按强度混合）+ 1 逻辑像素描边（E.3.2）
void EmojiAtlas::bakeThemedOne (EmojiId id, Size size)
{
    const uint32_t ep = themeEpoch_.load (std::memory_order_acquire);
    bakeThemedSlot (id, size, captureCurrentThemeParams (ep));
}

void EmojiAtlas::bakeThemedSlot (EmojiId id, Size size, const ThemeBakeParams& params)
{
    const int i = (int) id;
    const int s = (int) size;
    if (i < 0 || i >= (int) EmojiId::Count || s < 0 || s >= (int) Size::Count)
        return;

    if (themedReadyEpoch_[i][s].load (std::memory_order_acquire) == params.epoch)
        return;
    if (baseState_[i][s].load (std::memory_order_acquire) != 2)
        return;

    juce::Image src;
    {
        const juce::ScopedLock sl (bakeLock_);
        if (! entries_[i][s].ok || ! entries_[i][s].img.isValid())
            return;
        src = entries_[i][s].img;
    }

    const float strength = params.duotoneStrength;
    const juce::Colour darkC  = params.darkC;
    const juce::Colour midC   = params.midC;
    const juce::Colour lightC = params.lightC;

    auto grad = [&] (float L)
    {
        if (L < 0.5f) return darkC.interpolatedWith (midC, L * 2.0f);
        return midC.interpolatedWith (lightC, (L - 0.5f) * 2.0f);
    };

    const int w = src.getWidth(), h = src.getHeight();
    juce::Image out (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
    {
        const juce::Image::BitmapData sd (src, juce::Image::BitmapData::readOnly);
        const juce::Image::BitmapData od (out, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const juce::Colour c = sd.getPixelColour (x, y);
                if (c.getAlpha() == 0)
                    continue;
                const float L = (0.299f * (float) c.getRed() + 0.587f * (float) c.getGreen()
                                 + 0.114f * (float) c.getBlue()) / 255.0f;
                juce::Colour nc = c.interpolatedWith (grad (L), strength).withAlpha (c.getAlpha());
                od.setPixelColour (x, y, nc);
            }
        }
    }

    // 贴纸描边 + 同步生成 GL premultiplied RGBA8（单趟完成，消除二次全图扫描）
    const int rad = juce::jmax (1, params.strokeRadius);
    const juce::Colour strokeCol = params.stroke.withAlpha ((juce::uint8) 220);
    const uint32_t stA = 220u;
    const uint8_t  stR = (uint8_t) (((uint32_t) strokeCol.getRed()   * stA + 127u) / 255u);
    const uint8_t  stG = (uint8_t) (((uint32_t) strokeCol.getGreen() * stA + 127u) / 255u);
    const uint8_t  stB = (uint8_t) (((uint32_t) strokeCol.getBlue()  * stA + 127u) / 255u);

    juce::Image strokeLayer (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
    std::vector<uint8_t> premulRgba ((size_t) w * (size_t) h * 4u, 0u);
    {
        const juce::Image::BitmapData sd (out, juce::Image::BitmapData::readOnly);
        const juce::Image::BitmapData od (strokeLayer, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < h; ++y)
        {
            const auto* sRow = reinterpret_cast<const juce::PixelARGB*> (sd.getLinePointer (y));
            uint8_t* glDst   = premulRgba.data() + (size_t) y * (size_t) w * 4u;

            for (int x = 0; x < w; ++x)
            {
                if (sRow[x].getAlpha() > 128)
                {
                    const juce::Colour c = sd.getPixelColour (x, y);
                    od.setPixelColour (x, y, c);
                    const uint32_t a = (uint32_t) c.getAlpha();
                    glDst[x * 4 + 0] = (uint8_t) (((uint32_t) c.getRed()   * a + 127u) / 255u);
                    glDst[x * 4 + 1] = (uint8_t) (((uint32_t) c.getGreen() * a + 127u) / 255u);
                    glDst[x * 4 + 2] = (uint8_t) (((uint32_t) c.getBlue()  * a + 127u) / 255u);
                    glDst[x * 4 + 3] = (uint8_t) a;
                    continue;
                }

                bool edge = false;
                const int y0 = juce::jmax (0, y - rad);
                const int y1 = juce::jmin (h - 1, y + rad);
                const int x0 = juce::jmax (0, x - rad);
                const int x1 = juce::jmin (w - 1, x + rad);
                for (int ny = y0; ny <= y1 && ! edge; ++ny)
                {
                    const auto* nRow = reinterpret_cast<const juce::PixelARGB*> (sd.getLinePointer (ny));
                    for (int nx = x0; nx <= x1; ++nx)
                    {
                        if (nRow[nx].getAlpha() > 128)
                        {
                            edge = true;
                            break;
                        }
                    }
                }

                if (edge)
                {
                    od.setPixelColour (x, y, strokeCol);
                    glDst[x * 4 + 0] = stR;
                    glDst[x * 4 + 1] = stG;
                    glDst[x * 4 + 2] = stB;
                    glDst[x * 4 + 3] = (uint8_t) stA;
                }
            }
        }
    }

    if (themeEpoch_.load (std::memory_order_acquire) != params.epoch)
        return;

    {
        const juce::ScopedLock sl (bakeLock_);
        if (themeEpoch_.load (std::memory_order_acquire) != params.epoch)
            return;
        ThemedEntry& t = themed_[i][s];
        t.epoch = params.epoch;
        t.baked = true;
        t.ok    = true;
        t.img   = std::move (strokeLayer);
        themedReadyEpoch_[i][s].store (params.epoch, std::memory_order_release);
    }

    publishGlSlotPremul (i, s, w, h, std::move (premulRgba), /*isThemed=*/ true, params.epoch);
}
