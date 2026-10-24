// ==========================================================
// Toybox 物理单测（§13.4）：纯 C++，不链接 JUCE。
//   验证：弹簧能量衰减 / 碰撞次数上限 / 粒子积分 / 单向平台判定 / 脏矩形并集。
// ==========================================================

#include "source/ui/toys/ToyPhysics.h"

#include <cstdio>
#include <cmath>
#include <vector>

static int gFailures = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::printf ("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++gFailures;                                              \
        }                                                             \
    } while (0)

static void testSpringConverges()
{
    y2ktoy::Spring s;
    for (int i = 0; i < 10000; ++i)
        s.step (1.0f, 0.001f, 200.0f, 20.0f);
    CHECK (std::abs (s.x - 1.0f) < 0.01f, "spring converges to target");
    CHECK (std::abs (s.v) < 0.1f, "spring velocity decays");
}

static void testIntegrateDamping()
{
    // 初速向上，重力应最终将其拉回并下落（y 变为正）
    float x = 0, y = 0, vx = 0, vy = -100.0f;
    for (int i = 0; i < 2000; ++i)
        y2ktoy::integrate (x, y, vx, vy, 0.01f, 100.0f, 0.0f);
    CHECK (y > 0.0f, "gravity pulls downward after initial upward velocity");

    // 阻尼：速度应被衰减
    float x2 = 0, y2 = 0, vx2 = 0, vy2 = -100.0f;
    y2ktoy::integrate (x2, y2, vx2, vy2, 0.01f, 0.0f, 5.0f);
    CHECK (vy2 > -100.0f, "damping reduces velocity");
}

static void testPlatformHitOnlyWhenFalling()
{
    // 下落穿越平台：应碰撞
    CHECK (y2ktoy::platformHit (9.0f, 11.0f, 10.0f, 5.0f), "falling through platform hits");
    // 上升穿越：不碰撞
    CHECK (! y2ktoy::platformHit (9.0f, 11.0f, 10.0f, -5.0f), "rising through platform does not hit");
    // 已在平台下方：不碰撞
    CHECK (! y2ktoy::platformHit (11.0f, 13.0f, 10.0f, 5.0f), "already below does not hit");
    // 速度向上：不碰撞
    CHECK (! y2ktoy::platformHit (9.0f, 11.0f, 10.0f, 0.0f), "zero velocity does not hit");
}

static void testBounceLimit()
{
    // 模拟精灵最多弹 2 次（§8.1）：带重力，弹满 2 次后第 3 次穿越不再反弹。
    const float top = 100.0f;
    const float restitution = 0.45f;
    const float gravity = 900.0f;
    const int   maxBounces = 2;
    float y = 80.0f, vy = 200.0f;
    int bounces = 0;
    bool passedThrough = false;

    for (int i = 0; i < 100000 && ! passedThrough; ++i)
    {
        const float dt = 0.001f;
        const float prevY = y;
        vy += gravity * dt;
        y  += vy * dt;

        if (y2ktoy::platformHit (prevY, y, top, vy))
        {
            if (bounces < maxBounces)
            {
                y = top;
                vy = -vy * restitution;
                ++bounces;
            }
            else
            {
                passedThrough = true;   // 已达上限，不再弹
            }
        }
    }
    CHECK (bounces == maxBounces, "sprite bounces exactly maxBounces times");
    CHECK (passedThrough, "sprite passes through platform after bounce limit");
}

static void testDirtyRectUnion()
{
    // 模拟两个包围盒的并集合并
    struct R { float x0, y0, x1, y1; };
    auto unionR = [](R a, R b) -> R {
        return { std::min (a.x0, b.x0), std::min (a.y0, b.y0),
                 std::max (a.x1, b.x1), std::max (a.y1, b.y1) };
    };
    R prev { 10, 10, 20, 20 };
    R curr { 30, 30, 40, 40 };
    R u = unionR (prev, curr);
    CHECK (u.x0 == 10 && u.y0 == 10 && u.x1 == 40 && u.y1 == 40, "dirty union covers prev+curr");
}

// FIX-1：任意 spin 下图像中心必须映射到 pos（偏差 ≤ 1px）
static void testEmojiTransformCentre()
{
    const float imgW = 50.0f;   // 烘焙原尺寸
    for (int i = 0; i < 1000; ++i)
    {
        const float posX = (float) (i * 37 % 1000);
        const float posY = (float) (i * 53 % 800);
        const float drawSize = 18.0f + (float) (i % 3) * 8.0f;   // S/M/L
        const float angle = (float) (i * 0.7);

        const auto t = y2ktoy::emojiTransform (posX, posY, drawSize, angle, imgW);

        // 图像中心 (imgW/2, imgW/2) 映射后应等于 (posX, posY)
        float ox = 0.0f, oy = 0.0f;
        y2ktoy::emojiMapPoint (t, imgW * 0.5f, imgW * 0.5f, ox, oy);
        CHECK (std::abs (ox - posX) < 1.0f && std::abs (oy - posY) < 1.0f,
               "emoji centre maps to pos at any spin");
    }
}

// FIX-3：带 x 范围的平台碰撞
static void testPlatformHitXRange()
{
    // 平台 x∈[100,200]，y=50。粒子 x=150 应碰撞；x=90 不碰撞。
    CHECK (y2ktoy::platformHitX (150.0f, 100.0f, 200.0f, 40.0f, 51.0f, 50.0f, 5.0f),
           "in-range x hits platform");
    CHECK (! y2ktoy::platformHitX (90.0f, 100.0f, 200.0f, 40.0f, 51.0f, 50.0f, 5.0f),
           "out-of-range x does not hit (no air wall)");
    CHECK (! y2ktoy::platformHitX (150.0f, 100.0f, 200.0f, 40.0f, 51.0f, 50.0f, -5.0f),
           "rising does not hit even in range");
}

int main()
{
    testSpringConverges();
    testIntegrateDamping();
    testPlatformHitOnlyWhenFalling();
    testBounceLimit();
    testDirtyRectUnion();
    testEmojiTransformCentre();
    testPlatformHitXRange();

    if (gFailures == 0)
    {
        std::printf ("ALL_OK\n");
        return 0;
    }
    std::printf ("%d FAILURES\n", gFailures);
    return 1;
}
