//
// macos_iconize.m  —  macOS App Icon 模板化工具（Objective-C + CoreGraphics）
// ------------------------------------------------------------
// 用途：
//   把任意正方形 PNG 渲染成符合 macOS Big Sur+ "squircle app icon"
//   规范的 1024×1024 PNG：
//
//     · 整体画布 1024×1024，透明底
//     · 内容按 aspect-fill 内缩到约 824×824（留 100px 边距，Apple HIG 模板）
//     · 内容被裁剪进圆角矩形（cornerRadius ≈ 内容尺寸 * 0.225，squircle 近似）
//     · 可选一层 drop shadow（--shadow）
//
// 编译：
//   clang -framework AppKit -framework CoreGraphics -framework ImageIO \
//         -framework CoreFoundation -framework Foundation \
//         -fobjc-arc -O2 macos_iconize.m -o macos_iconize
//
// 运行：
//   ./macos_iconize <input.png> <output.png> [--shadow]
//
// 依赖：
//   仅依赖 macOS 自带 framework，不需要 Swift toolchain、不需要 pyobjc。
// ------------------------------------------------------------

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <stdio.h>
#import <string.h>

static int writePng(CGImageRef image, const char* path)
{
    NSURL* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: path]];
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef) url, CFSTR("public.png"), 1, NULL);
    if (!dest)
    {
        fprintf(stderr, "无法创建目标 PNG: %s\n", path);
        return 1;
    }
    CGImageDestinationAddImage(dest, image, NULL);
    bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    if (!ok)
    {
        fprintf(stderr, "PNG 写出失败: %s\n", path);
        return 1;
    }
    return 0;
}

int main(int argc, const char* argv[])
{
    @autoreleasepool
    {
        if (argc < 3)
        {
            fprintf(stderr,
                    "usage: macos_iconize <input.png> <output.png> [--shadow]\n");
            return 2;
        }
        const char* inputPath  = argv[1];
        const char* outputPath = argv[2];
        bool wantShadow = false;
        for (int i = 3; i < argc; ++i)
            if (strcmp(argv[i], "--shadow") == 0) wantShadow = true;

        // 1) 读取源图
        NSImage* src = [[NSImage alloc] initWithContentsOfFile:
                        [NSString stringWithUTF8String: inputPath]];
        if (!src)
        {
            fprintf(stderr, "无法读取图片: %s\n", inputPath);
            return 1;
        }
        NSRect proposed = NSZeroRect;
        CGImageRef srcCG = [src CGImageForProposedRect:&proposed context:nil hints:nil];
        if (!srcCG)
        {
            fprintf(stderr, "无法获得 CGImage: %s\n", inputPath);
            return 1;
        }

        // 2) 目标画布 1024 & 模板参数
        const CGFloat canvas  = 1024.0;
        const CGFloat padding = 100.0;                      // HIG 模板边距
        const CGFloat content = canvas - padding * 2.0;     // 824
        const CGFloat radius  = content * 0.225;            // squircle 近似

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(
            NULL, (size_t) canvas, (size_t) canvas,
            8, 0, cs,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);
        if (!ctx)
        {
            fprintf(stderr, "CGBitmapContext 创建失败\n");
            return 1;
        }

        CGContextSetAllowsAntialiasing(ctx, true);
        CGContextSetShouldAntialias(ctx, true);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);

        CGContextClearRect(ctx, CGRectMake(0, 0, canvas, canvas));

        CGRect contentRect = CGRectMake(padding, padding, content, content);
        CGPathRef clipPath = CGPathCreateWithRoundedRect(
            contentRect, radius, radius, NULL);

        if (wantShadow)
        {
            CGContextSaveGState(ctx);
            CGColorRef shadowColor = CGColorCreateGenericRGB(0, 0, 0, 0.25);
            CGContextSetShadowWithColor(ctx, CGSizeMake(0, -10), 24, shadowColor);
            CGColorRelease(shadowColor);
            CGContextAddPath(ctx, clipPath);
            CGColorRef white = CGColorCreateGenericRGB(1, 1, 1, 1);
            CGContextSetFillColorWithColor(ctx, white);
            CGColorRelease(white);
            CGContextFillPath(ctx);
            CGContextRestoreGState(ctx);
        }

        CGContextSaveGState(ctx);
        CGContextAddPath(ctx, clipPath);
        CGContextClip(ctx);

        CGFloat srcW = (CGFloat) CGImageGetWidth(srcCG);
        CGFloat srcH = (CGFloat) CGImageGetHeight(srcCG);
        CGFloat scale = MAX(content / srcW, content / srcH);
        CGFloat drawW = srcW * scale;
        CGFloat drawH = srcH * scale;
        CGRect drawRect = CGRectMake(
            padding + (content - drawW) * 0.5,
            padding + (content - drawH) * 0.5,
            drawW, drawH);
        CGContextDrawImage(ctx, drawRect, srcCG);

        CGContextRestoreGState(ctx);
        CGPathRelease(clipPath);

        CGImageRef outCG = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        if (!outCG)
        {
            fprintf(stderr, "CGBitmapContext → CGImage 失败\n");
            return 1;
        }
        int rc = writePng(outCG, outputPath);
        CGImageRelease(outCG);

        if (rc == 0)
        {
            fprintf(stdout,
                    "ok: %s  (%dx%d, padding=%d, radius=%d%s)\n",
                    outputPath, (int) canvas, (int) canvas,
                    (int) padding, (int) radius,
                    wantShadow ? ", shadow" : "");
        }
        return rc;
    }
}
