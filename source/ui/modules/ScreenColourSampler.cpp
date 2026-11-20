#include "source/ui/modules/ScreenColourSampler.h"

// ==========================================================
// ScreenColourSampler.cpp —— Windows 实现 + 非 Windows / 非 macOS 的空实现
//   macOS 实现见 ScreenColourSampler_mac.mm。
// ==========================================================

#if defined (_WIN32)

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace y2k::screencolour
{
    bool supportsLiveSampling() noexcept   { return true; }
    bool supportsSystemSampler() noexcept  { return false; }

    void pickWithSystemSampler (std::function<void (bool, Rgb)> callback)
    {
        if (callback)
            callback (false, {});
    }

    bool sampleAroundCursor (int radius, Rgb* out)
    {
        if (out == nullptr || radius < 0 || radius > 32)
            return false;

        POINT p {};
        if (! GetCursorPos (&p))
            return false;

        const int size = radius * 2 + 1;

        HDC screen = GetDC (nullptr);
        if (screen == nullptr)
            return false;

        bool ok = false;
        HDC mem = CreateCompatibleDC (screen);

        BITMAPINFO bi {};
        bi.bmiHeader.biSize        = sizeof (BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = size;
        bi.bmiHeader.biHeight      = -size;   // 自上而下
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bmp = mem != nullptr ? CreateDIBSection (screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;

        if (bmp != nullptr && bits != nullptr)
        {
            HGDIOBJ old = SelectObject (mem, bmp);

            if (BitBlt (mem, 0, 0, size, size, screen, p.x - radius, p.y - radius, SRCCOPY | CAPTUREBLT))
            {
                GdiFlush();
                const auto* px = static_cast<const std::uint8_t*> (bits);   // BGRA

                for (int i = 0; i < size * size; ++i)
                {
                    out[i].b = px[i * 4 + 0];
                    out[i].g = px[i * 4 + 1];
                    out[i].r = px[i * 4 + 2];
                }
                ok = true;
            }

            SelectObject (mem, old);
        }

        if (bmp != nullptr) DeleteObject (bmp);
        if (mem != nullptr) DeleteDC (mem);
        ReleaseDC (nullptr, screen);
        return ok;
    }
}

#elif ! defined (__APPLE__)

namespace y2k::screencolour
{
    bool supportsLiveSampling() noexcept  { return false; }
    bool supportsSystemSampler() noexcept { return false; }
    bool sampleAroundCursor (int, Rgb*)   { return false; }

    void pickWithSystemSampler (std::function<void (bool, Rgb)> callback)
    {
        if (callback)
            callback (false, {});
    }
}

#endif
