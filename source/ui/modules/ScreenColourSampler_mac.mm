// ==========================================================
// ScreenColourSampler_mac.mm —— macOS 取色：系统取色放大镜 NSColorSampler
//
//   · 直接读屏（CGWindowListCreateImage 等）需要"屏幕录制"授权，且相关 API
//     在新 SDK 中已废弃；NSColorSampler 是 AppKit 公开接口（10.15+），
//     由系统绘制放大镜、无需任何授权，用户点一下即可取色。
//   · 本文件不引入 JuceHeader.h（避免与 AppKit 的 Point / Component 撞名）。
// ==========================================================

#include "ScreenColourSampler.h"

#import <AppKit/AppKit.h>

#include <cmath>
#include <memory>

namespace y2k::screencolour
{
    namespace
    {
        // 会话期间保持取色器存活（ARC / MRC 两种编译方式都安全）
        //   部署目标为 macOS 12.3（≥ 10.15），类型本身无需可用性标注。
        NSColorSampler* gActiveSampler = nil;

        std::uint8_t toByte (CGFloat v)
        {
            const double c = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : (double) v);
            return (std::uint8_t) std::lround (c * 255.0);
        }
    }

    bool supportsLiveSampling() noexcept  { return false; }
    bool sampleAroundCursor (int, Rgb*)   { return false; }

    bool supportsSystemSampler() noexcept
    {
        if (@available (macOS 10.15, *))
            return true;
        return false;
    }

    void pickWithSystemSampler (std::function<void (bool, Rgb)> callback)
    {
        auto cb = std::make_shared<std::function<void (bool, Rgb)>> (std::move (callback));

        if (@available (macOS 10.15, *))
        {
            if (gActiveSampler != nil)
                return;   // 已经在取色中

            NSColorSampler* sampler = [[NSColorSampler alloc] init];
            gActiveSampler = sampler;

            [sampler showSamplerWithSelectionHandler: ^(NSColor* selected)
            {
                Rgb rgb;
                bool ok = false;

                if (selected != nil)
                {
                    NSColor* c = [selected colorUsingColorSpace: [NSColorSpace sRGBColorSpace]];
                    if (c != nil)
                    {
                        rgb.r = toByte ([c redComponent]);
                        rgb.g = toByte ([c greenComponent]);
                        rgb.b = toByte ([c blueComponent]);
                        ok = true;
                    }
                }

                // 回调在主线程；释放放到下一轮 runloop，避免在取色器自身回调里销毁它
                dispatch_async (dispatch_get_main_queue(), ^
                {
                   #if ! __has_feature(objc_arc)
                    [gActiveSampler release];
                   #endif
                    gActiveSampler = nil;
                });

                if (*cb)
                    (*cb) (ok, rgb);
            }];
            return;
        }

        if (*cb)
            (*cb) (false, {});
    }
}
