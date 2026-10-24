#pragma once

// ==========================================================
// ToyPhysics —— Toybox 互动纯物理工具（不依赖 JUCE，纯 C++）
//
//   供单测（tests/toy_physics_test.cpp）与运行期粒子/弹簧复用。
//   所有函数均为无状态纯函数，线程安全，无分配。
// ==========================================================

#include <cmath>
#include <algorithm>

namespace y2ktoy
{
    // ---- 半隐式欧拉积分 ----
    //   先更新速度（重力 + 阻尼），再更新位置，dt 钳制到 [0, dtMax]。
    //   返回实际使用的 dt。阻尼按线性阻尼 v *= (1 - damping*dt)。
    inline float integrate (float& x, float& y,
                            float& vx, float& vy,
                            float dt, float gravity, float damping,
                            float dtMax = 0.05f)
    {
        dt = std::max (0.0f, std::min (dt, dtMax));
        vy += gravity * dt;
        const float d = 1.0f - damping * dt;
        vx *= d;
        vy *= d;
        x  += vx * dt;
        y  += vy * dt;
        return dt;
    }

    // ---- 单向平台碰撞 ----
    //   仅当"上一帧在平台上沿之上 且 当前帧下落穿越到其下"时返回 true。
    //   prevY/curY 为粒子中心 y 坐标；topY 为平台上沿 y。
    //   调用方在返回 true 时负责回弹（位置钳制 + vy 反向乘 restitution）。
    inline bool platformHit (float prevY, float curY, float topY, float vy)
    {
        return vy > 0.0f && prevY < topY && curY >= topY;
    }

    // ---- 带 x 范围的平台碰撞（FIX-3 / B-03）----
    //   平台为矩形 {x0, x1, y}，粒子 x 必须落在 [x0, x1] 内才参与碰撞，
    //   消除"没有模块的 x 范围内也反弹"的空气墙。
    inline bool platformHitX (float x, float x0, float x1,
                              float prevY, float curY, float topY, float vy)
    {
        return x >= x0 && x <= x1 && platformHit (prevY, curY, topY, vy);
    }

    // ---- 阻尼弹簧（用于 L2 指针摆动 / 压扁过冲等）----
    struct Spring
    {
        float x = 0.0f;
        float v = 0.0f;

        // target 为目标位移；stiffness 为刚度；damping 为阻尼系数。
        void step (float target, float dt, float stiffness, float damping)
        {
            const float a = -stiffness * (x - target) - damping * v;
            v += a * dt;
            x += v * dt;
        }
    };

    // ---- 指数平滑（一阶低通）----
    //   rate 为时间常数倒数（/s）。返回平滑后的值。
    inline float expSmooth (float value, float target, float dt, float rate)
    {
        const float k = 1.0f - std::exp (-rate * dt);
        return value + (target - value) * k;
    }

    // ---- 限制到 [0,1] ----
    inline float clamp01 (float v)
    {
        return std::max (0.0f, std::min (1.0f, v));
    }

    // ---- 一维波动（可选，预留：画布扫描线 / 频谱拨弦余振）----
    struct WaveLine
    {
        float amp = 0.0f;      // 当前幅度
        float phase = 0.0f;    // 相位（rad）

        // 衰减正弦：amp 指数衰减，phase 按频率推进。
        void step (float dt, float frequency, float decayRate)
        {
            amp = std::max (0.0f, amp - decayRate * amp * dt);
            phase += frequency * dt;
        }

        float value() const { return amp * std::sin (phase); }
    };

    // ---- emoji 绘制变换系数（FIX-1 / B-01）----
    //   把"左上角为 (0,0)、尺寸 imgW×imgW 的正方形图像"绘制为：
    //   缩放至 drawSize、中心对齐 (posX, posY)、绕中心旋转 angle 弧度，
    //   纵向再乘 scaleY（1=正常；<1 落地挤压）。
    //   返回 6 个仿射系数（列向量约定，对应 juce::AffineTransform 的
    //   mat00..mat12 顺序 a,b,e,c,d,f）：
    //     x' = a*x + b*y + e
    //     y' = c*x + d*y + f
    //   纯函数，无状态，供 drawEmoji() 与单测复用。
    struct EmojiTransform
    {
        float a, b, c, d, e, f;
    };

    inline EmojiTransform emojiTransform (float posX, float posY, float drawSize, float angle, float imgW,
                                          float scaleY = 1.0f)
    {
        const float s  = drawSize / imgW;
        const float cs = std::cos (angle);
        const float sn = std::sin (angle);
        const float hx = drawSize * 0.5f;
        const float hy = drawSize * 0.5f * scaleY;

        EmojiTransform t;
        t.a = s * cs;
        t.b = -s * scaleY * sn;
        t.c = s * sn;
        t.d = s * scaleY * cs;
        t.e = posX - hx * cs + hy * sn;
        t.f = posY - hx * sn - hy * cs;
        return t;
    }

    // 用上面的系数映射一个点（用于单测验证"图像中心映射到 pos"）。
    inline void emojiMapPoint (const EmojiTransform& t, float x, float y, float& ox, float& oy)
    {
        ox = t.a * x + t.b * y + t.e;
        oy = t.c * x + t.d * y + t.f;
    }
}
