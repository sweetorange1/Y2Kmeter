// 独立诊断：验证 Windows D2D/DirectWrite 彩色 emoji 渲染路径（WIC 软件目标）。
// 不依赖 JUCE，输出诊断信息 + 保存 BMP。仅用于本地排查。
#include <windows.h>
#include <wrl/client.h>
#include <dwrite.h>
#include <d2d1.h>
#include <wincodec.h>
#include <cstdio>
#include <cwchar>
#include <vector>

#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

static void saveBmp (const char* path, const BYTE* bits, int w, int h, UINT pitch)
{
    std::vector<BYTE> buf (54 + (size_t) h * pitch);
    BYTE* p = buf.data();
    auto w32 = [&](int v) { *p++ = (BYTE) v; *p++ = (BYTE)(v>>8); *p++ = (BYTE)(v>>16); *p++ = (BYTE)(v>>24); };
    auto w16 = [&](int v) { *p++ = (BYTE) v; *p++ = (BYTE)(v>>8); };
    p[0]='B'; p[1]='M'; p += 2;
    w32 ((int) buf.size()); w32 (0); w32 (54);
    w32 (40); w32 (w); w32 (h); w16 (1); w16 (32); w32 (0); w32 (0); w32 (0); w32 (0); w32 (0); w32 (0);
    for (int y = h - 1; y >= 0; --y)
        memcpy (buf.data() + 54 + (size_t)(h - 1 - y) * pitch, bits + (size_t) y * pitch, pitch);
    FILE* f = fopen (path, "wb");
    if (f) { fwrite (buf.data(), 1, buf.size(), f); fclose (f); }
}

int main()
{
    setvbuf (stdout, nullptr, _IONBF, 0);
    printf ("=== emoji diag (WIC software target) ===\n");
    CoInitialize (nullptr);
    const int px = 48;
    const wchar_t* text = L"\xD83D\xDC96";  // 💖 U+1F496

    ComPtr<IDWriteFactory> dw;
    HRESULT hr = DWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
    printf ("DWriteCreateFactory hr=0x%08X\n", (unsigned) hr);

    {
        ComPtr<IDWriteFontCollection> coll;
        dw->GetSystemFontCollection (coll.GetAddressOf());
        UINT32 idx = 0xFFFFFFFF; BOOL exists = FALSE;
        coll->FindFamilyName (L"Segoe UI Emoji", &idx, &exists);
        printf ("Segoe UI Emoji exists=%d idx=%u\n", exists, idx);
    }

    ComPtr<IDWriteTextFormat> fmt;
    hr = dw->CreateTextFormat (L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               (float)(px*0.82f), L"en-US", fmt.GetAddressOf());
    printf ("CreateTextFormat hr=0x%08X\n", (unsigned) hr);
    fmt->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ComPtr<IDWriteTextLayout> layout;
    hr = dw->CreateTextLayout (text, 2, fmt.Get(), (float)px, (float)px, layout.GetAddressOf());
    printf ("CreateTextLayout hr=0x%08X\n", (unsigned) hr);

    ComPtr<ID2D1Factory> d2d;
    hr = D2D1CreateFactory (D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                            reinterpret_cast<void**>(d2d.GetAddressOf()));
    printf ("D2D1CreateFactory hr=0x%08X\n", (unsigned) hr);

    ComPtr<IWICImagingFactory> wic;
    hr = CoCreateInstance (CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.GetAddressOf()));
    printf ("CoCreateInstance(WIC) hr=0x%08X\n", (unsigned) hr);

    ComPtr<IWICBitmap> wicBitmap;
    hr = wic->CreateBitmap (px, px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                            wicBitmap.GetAddressOf());
    printf ("WIC CreateBitmap hr=0x%08X\n", (unsigned) hr);

    const auto rtp = D2D1::RenderTargetProperties (
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat (DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> rt;
    hr = d2d->CreateWicBitmapRenderTarget (wicBitmap.Get(), rtp, rt.GetAddressOf());
    printf ("CreateWicBitmapRenderTarget hr=0x%08X\n", (unsigned) hr);

    rt->BeginDraw();
    rt->Clear (D2D1::ColorF (0, 0, 0, 0));
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush (D2D1::ColorF (D2D1::ColorF::White), brush.GetAddressOf());
    rt->DrawTextLayout (D2D1::Point2F (0, 0), layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    hr = rt->EndDraw();
    printf ("DrawTextLayout+EndDraw hr=0x%08X\n", (unsigned) hr);

    ComPtr<IWICBitmapLock> lock;
    hr = wicBitmap->Lock (nullptr, WICBitmapLockRead, lock.GetAddressOf());
    printf ("WIC Lock hr=0x%08X\n", (unsigned) hr);
    UINT stride = 0, bufSize = 0;
    BYTE* buf = nullptr;
    lock->GetStride (&stride);
    lock->GetDataPointer (&bufSize, &buf);
    printf ("stride=%u bufSize=%u\n", stride, bufSize);

    int opaque = 0;
    int colorCount = 0;
    BYTE colorKeys[32][3] = {};
    for (int y = 0; y < px; ++y)
    {
        const BYTE* s = buf + (size_t) y * stride;
        for (int x = 0; x < px; ++x)
        {
            int a = s[x*4+3];
            if (a < 32) continue;
            ++opaque;
            BYTE r = s[x*4+2], g = s[x*4+1], b = s[x*4+0];
            bool seen = false;
            for (int i = 0; i < colorCount; ++i)
                if (abs (colorKeys[i][0]-r) < 24 && abs (colorKeys[i][1]-g) < 24 && abs (colorKeys[i][2]-b) < 24) { seen = true; break; }
            if (! seen && colorCount < 32) { colorKeys[colorCount][0]=r; colorKeys[colorCount][1]=g; colorKeys[colorCount][2]=b; ++colorCount; }
        }
    }
    printf ("coverage=%.2f%%  colors=%d\n", 100.0f*opaque/(px*px), colorCount);

    saveBmp ("emoji_diag.bmp", buf, px, px, stride);
    printf ("saved emoji_diag.bmp\n");

    // ---- 模拟项目内路径：手工 UTF-8 → UTF-16（utf8ToUtf16 逻辑）再渲染 ----
    {
        const char* u8 = "\xF0\x9F\x91\xBE";   // 👾 与 kSpecs 相同的转义字节
        std::vector<wchar_t> out;
        while (*u8)
        {
            unsigned char c = (unsigned char) *u8;
            unsigned int cp = 0; int n = 0;
            if (c < 0x80) { cp = c; n = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
            else { cp = c & 0x07; n = 4; }
            for (int i = 1; i < n && u8[i]; ++i) cp = (cp << 6) | ((unsigned char) u8[i] & 0x3F);
            u8 += n;
            if (cp < 0x10000) out.push_back ((wchar_t) cp);
            else { cp -= 0x10000; out.push_back ((wchar_t)(0xD800 + (cp >> 10))); out.push_back ((wchar_t)(0xDC00 + (cp & 0x3FF))); }
        }
        out.push_back (0);
        printf ("utf8ToUtf16: %04X %04X (expect D83D DC7E)\n", out[0], out[1]);

        ComPtr<IDWriteTextLayout> l2;
        hr = dw->CreateTextLayout (out.data(), 2, fmt.Get(), (float)px, (float)px, l2.GetAddressOf());
        ComPtr<IWICBitmap> b2;
        hr = wic->CreateBitmap (px, px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, b2.GetAddressOf());
        ComPtr<ID2D1RenderTarget> rt2;
        hr = d2d->CreateWicBitmapRenderTarget (b2.Get(), rtp, rt2.GetAddressOf());
        rt2->BeginDraw();
        rt2->Clear (D2D1::ColorF (0, 0, 0, 0));
        rt2->DrawTextLayout (D2D1::Point2F (0, 0), l2.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        rt2->EndDraw();
        ComPtr<IWICBitmapLock> lk;
        b2->Lock (nullptr, WICBitmapLockRead, lk.GetAddressOf());
        UINT st = 0, bs = 0; BYTE* bf = nullptr;
        lk->GetStride (&st); lk->GetDataPointer (&bs, &bf);
        int op = 0, cc = 0; BYTE keys[32][3] = {};
        for (int y = 0; y < px; ++y)
            for (int x = 0; x < px; ++x)
            {
                const BYTE* s = bf + (size_t) y * st;
                if (s[x*4+3] < 32) continue;
                ++op;
                BYTE r = s[x*4+2], g = s[x*4+1], b = s[x*4+0];
                bool seen = false;
                for (int i = 0; i < cc; ++i)
                    if (abs (keys[i][0]-r) < 24 && abs (keys[i][1]-g) < 24 && abs (keys[i][2]-b) < 24) { seen = true; break; }
                if (! seen && cc < 32) { keys[cc][0]=r; keys[cc][1]=g; keys[cc][2]=b; ++cc; }
            }
        printf ("via-utf8 conversion: coverage=%.2f%% colors=%d\n", 100.0f*op/(px*px), cc);
        saveBmp ("emoji_diag_invader.bmp", bf, px, px, st);
    }
    return 0;
}
