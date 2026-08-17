#pragma once

#include <cmath>

// ==========================================================
// MilkdropVisualState —— Milkdrop 模块整体后处理的全局视觉状态
//
// 由 MilkdropModule 的 color / effects 面板修改，经 Editor 全局共享给
// 所有 Milkdrop 模块，并持久化到 PluginProcessor 的 host state 中。
//
// 字段语义：
//   · tint_r/g/b  —— RGB 三通道加性偏移（master output colors），
//                    1.0 = 中性；>1 整体偏向该色（含黑色），<1 偏向补色。
//   · brightness  —— bright 增益（对齐 MilkDrop3 的 ret *= brightness），
//                    范围 0~8，1.0 = 中性；>1 提亮（暗部大幅抬升、高光溢出），
//                    <1 压暗。后处理阶段为纯线性增益 + 最终 clamp。
//
// 以下开关型效果对齐 MilkDrop3 的 Effect Injection（ADDMILKEFFECT）目录，
// 分为两类：efftop（在采样前重映射 uv）与 effbottom（在采样后修改 ret）。
//   · invert   —— ret 反相 / 负片（ret = 1 - ret），effbottom。
//   · shadows  —— ret 加性叠加"上下翻转 + 灰度 + pow"纹理（ret += pow(...)），
//                 effbottom。注意：是加法叠加，不压暗，产生黑白镜像纹理。
//   · solarize —— 曝光反转 / 阈值反相（ret = ret*(1-ret)*4），effbottom。
//   · split    —— uv 分裂（uv = float2(abs(uv.x-0.5), uv.y)），efftop。
//   · zoom     —— uv 缩放（uv = 0.25 + 0.5*uv），efftop。
//   · multi    —— uv 多重折叠（multiplicate），efftop。
//   · rainbow  —— 彩虹染色，effbottom（降级为程序化彩虹，原版依赖噪声纹理）。
//   · blow     —— 加性模糊叠加（ret += GetBlur1(uv)），effbottom（近似）。
//   · burn     —— 灼烧混合（近似 overlay），effbottom。
//
// 扩展效果（本次新增，基于同样 efftop/effbottom 二分类）：
//   · kaleidoscope —— 万花筒，极坐标角度折叠（efftop）。
//   · swirl        —— 漩涡扭曲，绕中心旋转（efftop）。
//   · pinch        —— 鱼眼/挤压，径向缩放（efftop）。
//   · pixelate     —— 像素化，uv 量化（efftop）。
//   · glitch       —— 故障色差，RGB 通道微偏移采样（effbottom 采样）。
//   · posterize    —— 色调分离，ret 量化（effbottom）。
//   · sepia        —— 复古棕褐，颜色矩阵（effbottom）。
//   · grayscale    —— 灰度，亮度加权（effbottom）。
//   · edge         —— 边缘检测，邻域差分（effbottom）。
//   · vignette     —— 暗角，径向暗化（effbottom）。
//
// 开关型效果通过 MilkdropEffect.h 中的注册表统一驱动 UI 与 shader uniform 传递；
// 本结构体仅作为纯数据载体，新增效果时在此追加字段 + isNeutral 判定 +
// PluginProcessor 持久化字段三处同步。
// ==========================================================
struct MilkdropVisualState
{
    float tint_r     = 1.0f;
    float tint_g     = 1.0f;
    float tint_b     = 1.0f;
    float brightness = 1.0f;
    bool  invert     = false;
    bool  shadows    = false;
    bool  solarize   = false;
    bool  split      = false;
    bool  zoom       = false;
    bool  multi      = false;
    bool  rainbow    = false;
    bool  blow       = false;
    bool  burn       = false;
    // 以下为扩展效果（对齐常见后处理视觉，efftop=采样前 uv 变换，effbottom=采样后 ret 变换）
    bool  kaleidoscope = false;  // 万花筒：极坐标角度折叠（efftop）
    bool  swirl        = false;  // 漩涡扭曲：绕中心旋转，越远旋转越多（efftop）
    bool  pinch        = false;  // 鱼眼/挤压：径向缩放（efftop）
    bool  pixelate     = false;  // 像素化：uv 量化成块（efftop）
    bool  glitch       = false;  // 故障色差：RGB 通道微偏移采样（effbottom 采样）
    bool  posterize    = false;  // 色调分离：ret 量化成 N 级（effbottom）
    bool  sepia        = false;  // 复古棕褐：颜色矩阵（effbottom）
    bool  grayscale    = false;  // 灰度：亮度加权（effbottom）
    bool  edge         = false;  // 边缘检测/浮雕：邻域差分（effbottom）
    bool  vignette     = false;  // 暗角：径向暗化（effbottom）

    // 无任何染色 / 效果时返回 true，用于跳过零开销的 offscreen 后处理路径。
    bool isNeutral() const noexcept
    {
        return std::fabs (tint_r - 1.0f)     <= 1e-4f
            && std::fabs (tint_g - 1.0f)     <= 1e-4f
            && std::fabs (tint_b - 1.0f)     <= 1e-4f
            && std::fabs (brightness - 1.0f) <= 1e-4f
            && ! invert
            && ! shadows
            && ! solarize
            && ! split
            && ! zoom
            && ! multi
            && ! rainbow
            && ! blow
            && ! burn
            && ! kaleidoscope
            && ! swirl
            && ! pinch
            && ! pixelate
            && ! glitch
            && ! posterize
            && ! sepia
            && ! grayscale
            && ! edge
            && ! vignette;
    }
};
