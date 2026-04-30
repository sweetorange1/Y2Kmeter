//
// macos_dmg_background.m  —  DMG 窗口背景图生成工具
// ------------------------------------------------------------
// 用途：
//   为 build_macos_installer.sh 打出的 DMG 生成一张引导背景图，
//   指引用户把 .app / .vst3 分别拖到右侧的安装目录链接上。
//
//   输出两张 PNG：
//     <outBase>.png       720 × 460   （@1x）
//     <outBase>@2x.png    1440 × 920  （@2x，Retina）
//
//   图内容（与 AppleScript 里图标坐标一一对应）：
//     · 浅粉渐变背景
//     · 顶部大字标题  "Y2Kmeter"  + 版本号 + 副标题
//     · 上下两条粉色大箭头：
//         行 1 (y≈200): [App 图标占位]   ——▶   [Applications]
//         行 2 (y≈340): [VST3 图标占位]  ——▶   [VST3 插件目录]
//       每条箭头上/下附一句说明文字（"拖到这里即完成安装"）
//     · 底部小字提示：首次打开若被 Gatekeeper 拦 → 右键打开
//
// 编译：
//   clang -framework AppKit -framework CoreGraphics -framework ImageIO \
//         -framework CoreFoundation -framework Foundation \
//         -fobjc-arc -O2 macos_dmg_background.m -o macos_dmg_background
//
// 运行：
//   ./macos_dmg_background  "<outBasePathNoExt>"  "<productName>"  "<version>"
//
// 例：
//   ./macos_dmg_background  /tmp/dmg_bg  "Y2Kmeter"  "1.6.0"
//   → /tmp/dmg_bg.png      (720×460)
//   → /tmp/dmg_bg@2x.png   (1440×920)
// ------------------------------------------------------------

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <stdio.h>
#import <string.h>

// 与 build_macos_installer.sh 里 osascript 的图标坐标保持一致
static const CGFloat kBaseW = 720.0;
static const CGFloat kBaseH = 460.0;

// 图标中心坐标（Finder 里我们会把图标放在同样的位置）
static const CGFloat kRow1Y = 170.0;   // App / Applications 链接（上行）
static const CGFloat kRow2Y = 290.0;   // VST3 / VST3 目录（下行）
static const CGFloat kLeftX  = 150.0;  // 左侧（源文件）
static const CGFloat kRightX = 570.0;  // 右侧（安装目标链接）
static const CGFloat kIconHalf = 64.0; // 图标绘制半径 128 / 2

static int writePng(CGImageRef image, NSString* path)
{
    NSURL* url = [NSURL fileURLWithPath: path];
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef) url, CFSTR("public.png"), 1, NULL);
    if (!dest) { fprintf(stderr, "无法创建 PNG: %s\n", path.UTF8String); return 1; }
    CGImageDestinationAddImage(dest, image, NULL);
    bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    if (!ok) { fprintf(stderr, "PNG 写出失败: %s\n", path.UTF8String); return 1; }
    return 0;
}

// 画一个带箭头头的横向粗箭头（从 (x1,y) 到 (x2,y)），填色 fill，描边色 stroke
static void drawArrow(CGContextRef ctx,
                       CGFloat x1, CGFloat x2, CGFloat y,
                       CGFloat shaftH,
                       CGColorRef fill)
{
    const CGFloat headLen = shaftH * 1.9;     // 三角头长度
    const CGFloat headH   = shaftH * 2.2;     // 三角头高（超出 shaft）

    CGMutablePathRef p = CGPathCreateMutable();
    const CGFloat halfH = shaftH * 0.5;
    const CGFloat tipX  = x2;
    const CGFloat neckX = x2 - headLen;

    // 箭身矩形（四个角）+ 箭头三角（三个点），逆时针
    CGPathMoveToPoint   (p, NULL, x1,    y - halfH);
    CGPathAddLineToPoint(p, NULL, neckX, y - halfH);
    CGPathAddLineToPoint(p, NULL, neckX, y - headH * 0.5);
    CGPathAddLineToPoint(p, NULL, tipX,  y);
    CGPathAddLineToPoint(p, NULL, neckX, y + headH * 0.5);
    CGPathAddLineToPoint(p, NULL, neckX, y + halfH);
    CGPathAddLineToPoint(p, NULL, x1,    y + halfH);
    CGPathCloseSubpath  (p);

    CGContextSetFillColorWithColor(ctx, fill);
    CGContextAddPath(ctx, p);
    CGContextFillPath(ctx);
    CGPathRelease(p);
}

// 居中绘制一行 NSAttributedString，基于 (cx, cy) —— (cx,cy) 采用 "y 从上往下"
// 的 Finder 坐标语义（与 renderBackground 里的坐标常量一致）。
// 内部会临时把上下文 y 翻回"从下往上"，保证文字不会倒置。
static void drawTextCentered(CGContextRef ctx,
                             NSString* s, NSDictionary* attrs,
                             CGFloat cx, CGFloat cy)
{
    NSAttributedString* as = [[NSAttributedString alloc] initWithString:s attributes:attrs];
    NSSize sz = [as size];

    CGContextSaveGState(ctx);
    // 整个渲染流程先对 y 做过一次翻转（见 renderBackground），
    // 这里再翻一次 → 抵消 → 文字以正常方向绘制
    CGContextTranslateCTM(ctx, 0, cy);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextTranslateCTM(ctx, 0, -cy);

    NSGraphicsContext* nsCtx =
        [NSGraphicsContext graphicsContextWithCGContext: ctx flipped: NO];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext: nsCtx];

    NSPoint p = NSMakePoint(cx - sz.width * 0.5, cy - sz.height * 0.5);
    [as drawAtPoint: p];

    [NSGraphicsContext restoreGraphicsState];
    CGContextRestoreGState(ctx);
}

// 生成一张 (W × H) 的背景 PNG，scale 控制 Retina
static int renderBackground(NSString* outPath,
                            NSString* productName,
                            NSString* version,
                            CGFloat scale)
{
    const CGFloat W = kBaseW * scale;
    const CGFloat H = kBaseH * scale;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL,
        (size_t) W, (size_t) H, 8, 0, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) { fprintf(stderr, "CGBitmapContext 创建失败\n"); return 1; }

    CGContextSetAllowsAntialiasing(ctx, true);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);

    // 把坐标系缩放到"逻辑 720×460"工作，后面所有绘制都用逻辑尺寸
    CGContextScaleCTM(ctx, scale, scale);

    // 让 y 方向与 Finder 坐标一致（y 从上往下）：
    //   · 默认 CG 坐标系 y 从下往上，而 AppleScript 里我们给 Finder
    //     图标传的 {x, y} 是从窗口左上角往下、往右算。
    //   · 为了让 kRow1Y / kRow2Y 这些常量在"背景图"和"Finder 图标位置"
    //     两个地方语义完全一致，这里整体翻转一下 Y 轴。
    //   · 翻转后 NSAttributedString -drawAtPoint 会把文字画倒，所以稍后
    //     在需要画文字的绘制阶段再临时把上下文切回去。
    CGContextTranslateCTM(ctx, 0, kBaseH);
    CGContextScaleCTM(ctx, 1.0, -1.0);

    // ---- 1) 背景：浅粉→白的垂直渐变（注意 CTM 已翻转为 y 从上往下）----
    {
        CGFloat locs[2] = { 0.0, 1.0 };
        CGFloat comps[8] = {
            1.00, 0.96, 0.98, 1.0,   // 顶部 浅粉白
            0.98, 0.86, 0.92, 1.0    // 底部 粉
        };
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        CGGradientRef grad = CGGradientCreateWithColorComponents(rgb, comps, locs, 2);
        // start 在 y=0（画面顶部，locs=0 颜色） → end 在 y=kBaseH（画面底部，locs=1 颜色）
        CGContextDrawLinearGradient(ctx, grad,
                                    CGPointMake(0, 0),
                                    CGPointMake(0, kBaseH),
                                    0);
        CGGradientRelease(grad);
        CGColorSpaceRelease(rgb);
    }

    // ---- 2) 顶部标题 ----
    NSColor* ink    = [NSColor colorWithCalibratedRed:0.20 green:0.10 blue:0.18 alpha:1.0];
    NSColor* subInk = [NSColor colorWithCalibratedRed:0.35 green:0.25 blue:0.32 alpha:1.0];
    NSColor* pink   = [NSColor colorWithCalibratedRed:0.93 green:0.30 blue:0.52 alpha:1.0];

    {
        NSString* title = [NSString stringWithFormat:@"%@  v%@", productName, version];
        NSDictionary* attrs = @{
            NSFontAttributeName: [NSFont boldSystemFontOfSize: 28],
            NSForegroundColorAttributeName: ink,
        };
        // 上下文已翻转为 y 从上往下；40 / 68 表示距顶部 40 / 68 px
        drawTextCentered(ctx, title, attrs, kBaseW * 0.5, 40);

        NSDictionary* subAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize: 13],
            NSForegroundColorAttributeName: subInk,
        };
        drawTextCentered(ctx, @"macOS Installer  ·  Standalone + VST3",
                         subAttrs, kBaseW * 0.5, 68);
    }

    // ---- 3) 两条粉色引导箭头 ----
    {
        CGColorRef pinkCG = [pink CGColor];
        const CGFloat shaftH = 14.0;

        // 箭头起止：两侧图标中心之间留出 ~72px 间隙给图标本身
        const CGFloat gap = kIconHalf + 12; // 到图标中心的安全距离
        drawArrow(ctx, kLeftX + gap, kRightX - gap, kRow1Y, shaftH, pinkCG);
        drawArrow(ctx, kLeftX + gap, kRightX - gap, kRow2Y, shaftH, pinkCG);
    }

    // ---- 4) 每行图标下方标注说明 ----
    {
        NSDictionary* labelAttrs = @{
            NSFontAttributeName: [NSFont boldSystemFontOfSize: 11],
            NSForegroundColorAttributeName: subInk,
        };

        // 第 1 行（kRow1Y，上方）：Standalone 安装引导
        drawTextCentered(ctx, @"Y2Kmeter.app", labelAttrs,
                         kLeftX,  kRow1Y + kIconHalf + 20);
        drawTextCentered(ctx, @"Applications", labelAttrs,
                         kRightX, kRow1Y + kIconHalf + 20);

        // 第 2 行（kRow2Y，下方）：VST3 插件安装引导
        drawTextCentered(ctx, @"Y2Kmeter.vst3", labelAttrs,
                         kLeftX,  kRow2Y + kIconHalf + 20);
        drawTextCentered(ctx, @"VST3 Plug-Ins", labelAttrs,
                         kRightX, kRow2Y + kIconHalf + 20);

        // 箭头上方的 hint → y - 22（y 已翻转为从上往下）
        NSDictionary* hintAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize: 11],
            NSForegroundColorAttributeName: pink,
        };
        drawTextCentered(ctx, @"拖到这里即完成安装（Standalone）", hintAttrs,
                         (kLeftX + kRightX) * 0.5, kRow1Y - 22);
        drawTextCentered(ctx, @"Drag to install VST3 plugin", hintAttrs,
                         (kLeftX + kRightX) * 0.5, kRow2Y - 22);
    }

    // ---- 5) 底部 Gatekeeper 小字提示 ----
    {
        NSDictionary* footAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize: 10],
            NSForegroundColorAttributeName: subInk,
        };
        drawTextCentered(ctx, @"首次打开若提示未验证开发者，请在 Finder 中右键图标 → 选择「打开」",
                         footAttrs, kBaseW * 0.5, kBaseH - 36);
        drawTextCentered(ctx, @"Installed via ad-hoc signature  ·  No Apple notarization required",
                         footAttrs, kBaseW * 0.5, kBaseH - 20);
    }

    // ---- 6) 导出 PNG ----
    CGImageRef outCG = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!outCG) { fprintf(stderr, "Bitmap → CGImage 失败\n"); return 1; }
    int rc = writePng(outCG, outPath);
    CGImageRelease(outCG);
    return rc;
}

int main(int argc, const char* argv[])
{
    @autoreleasepool
    {
        if (argc < 4)
        {
            fprintf(stderr,
                "usage: macos_dmg_background <outBasePathNoExt> <productName> <version>\n"
                "  例: macos_dmg_background /tmp/dmg_bg Y2Kmeter 1.6.0\n");
            return 2;
        }

        NSString* base    = [NSString stringWithUTF8String: argv[1]];
        NSString* product = [NSString stringWithUTF8String: argv[2]];
        NSString* version = [NSString stringWithUTF8String: argv[3]];

        NSString* out1x = [base stringByAppendingString: @".png"];
        NSString* out2x = [base stringByAppendingString: @"@2x.png"];

        int rc = renderBackground(out1x, product, version, 1.0);
        if (rc != 0) return rc;

        rc = renderBackground(out2x, product, version, 2.0);
        if (rc != 0) return rc;

        fprintf(stdout, "ok: %s  (%d x %d)\n",
                out1x.UTF8String, (int) kBaseW, (int) kBaseH);
        fprintf(stdout, "ok: %s  (%d x %d @2x)\n",
                out2x.UTF8String, (int) (kBaseW * 2), (int) (kBaseH * 2));
        return 0;
    }
}
