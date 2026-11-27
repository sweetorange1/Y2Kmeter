#pragma once

#include <JuceHeader.h>
#include "source/ui/PinkXPStyle.h"

// ==========================================================
// SkinCraft —— 跨模块视觉风格（Skin）语义令牌 + CPU 工艺原语
//
//   职责划分：
//     · Theme（PinkXP::pink50..700 / face / content / sel / ink …）决定"配色"；
//     · SkinCraft（SkinTokens + Craft::*）决定"形状、材质、光学质感与像素精度"；
//     · 两者 100% 正交：任意 Theme × 任意 Skin 均可无损组合。
//
//   设计铁律：
//     1. 模块外框厚度维持 2px 直角（与既有内缩布局 & GL 挖洞几何严格一致）；
//     2. Pixel XP（SkinId::pinkXP）坚持 1998-2002 硬件像素工艺：
//        0px 圆角、2px 硬斜角、整数 Bresenham 线/圆弧/圆盘、锐利像素 LED、
//        像素点阵网格、硬阴影 —— 零柔焦高斯雾与现代圆胶囊；
//     3. Green Screen（SkinId::greenScreen）零绿幕污染：无凹槽框、无未点亮底格、
//        无网格线、无后处理特效、无柔光晕染；
//     4. 仅 `crt` / `glass` / `neon` 启用画布后处理（hasCanvasFx() == true），
//        `pinkXP` / `chrome` / `modern` / `greenScreen` 严格为 false（零额外 pass）。
// ==========================================================

namespace PinkXP
{
    // ----------------------------------------------------------
    // 语义工艺枚举
    // ----------------------------------------------------------
    enum class FrameStyle
    {
        pixelBevel,       // Pixel XP：Win95/XP 2px 硬斜角 + 1px 内倒角
        machinedChrome,   // Millennium Chrome：1px 深切边 + 1px 镜面内倒角 + 顶部内阴影
        modernStudio,     // Modern Minimal：1px 干净细描边 + 底部微抬升边（吸收原 Fluent 微工艺）
        frostedGlass,     // Frosted Glass：半透明暗边 + 顶/底玻璃折光高光
        neonTube,         // Neon Cyber：深色外沿 + 强调色发光灯管双线框
        crtPhosphor,      // CRT Terminal：单色磷光强调框 + 微弱次级余辉框
        chromaNone        // Green Screen：纯绿抠像，零边框
    };

    enum class CellFinish
    {
        flatPixel,        // Pixel XP：整数像素方块 + 1px 左上高光点缀
        metallicBevel,    // Millennium Chrome：短轴金属拉丝渐变 + 1px 切边
        cleanRounded,     // Modern Minimal：微圆角演播室纯色灯块
        glossyCapsule,    // Frosted Glass：圆角亚克力胶囊 + 顶部 40% 镜面反光
        neonLit,          // Neon Cyber：暗槽底 + 发光外晕 + 高亮灯芯
        phosphorBar       // CRT Terminal：像素对齐磷光灯条 + 细微扫描线纹理
    };

    enum class GridStyle
    {
        dottedPixel,      // Pixel XP：整数像素点阵线（主线 2on/2off，次线 1on/3off）
        solidHairline,    // Chrome / Modern / Glass：细实线
        dashedTech,       // Neon Cyber：科技虚线（主线 6on/3off，次线 3on/4off）
        phosphorCrosshair,// CRT Terminal：示波器磷光标尺线（主线实线 + 次线细虚线）
        none              // Green Screen：不画网格
    };

    enum class DialFinish
    {
        pixelHardware,    // Pixel XP：Bresenham 整数像素弧/指针 + 量化抖动背光 + 像素轴心
        brushedMetal,     // Millennium Chrome：拉丝金属表盘 + 镜面指针
        studioSmooth,     // Modern Minimal：抗锯齿现代矢量表盘 + 精致细刻度环
        glassDome,        // Frosted Glass：表镜高光穹顶 + 晶莹圆润指针
        neonRings,        // Neon Cyber：暗凹表盘 + 双环辉光 + 发光指针
        crtVector         // CRT Terminal：磷光单色矢量弧 + 准星刻度 + 磷光指针
    };

    enum class WellKind
    {
        canvas,           // 主仪表/图谱画布凹槽
        card,             // 设置分组卡片 / 读数卡片
        trough,           // 电平条 / 进度条 / 推子凹槽
        badge,            // 状态标签 / 紧凑徽章
        recessed          // 通用内凹面板
    };

    enum class ControlState
    {
        normal,
        hover,
        pressed,
        active,
        disabled
    };

    // ----------------------------------------------------------
    // SkinTokens —— 各风格的纯数据（POD）工艺参数表
    //   CPU 侧（Craft / LookAndFeel / Drawer / 17 模块）与
    //   GL 侧（GlDirectModuleBase / PluginEditor）共用此单一数据源。
    // ----------------------------------------------------------
    struct SkinTokens
    {
        // ---- 核心身份 ----
        FrameStyle frameStyle         = FrameStyle::pixelBevel;
        bool       isChromaKey        = false;
        bool       pixelSnapped       = true;    // true：pinkXP & crt（GL FBO 用 GL_NEAREST，坐标整数对齐）
        bool       preferLinearGl     = false;   // false：pinkXP & crt；true：modern / chrome / glass / neon
        bool       useFlatControls    = false;
        bool       useTextShadow      = true;

        // ---- 几何圆角（外框保持 2px 直角，此处仅作用于内部控件/卡片/灯块）----
        float      controlRadius      = 0.0f;
        float      cardRadius         = 0.0f;
        float      badgeRadius        = 0.0f;
        float      barCellRadius      = 0.0f;
        float      tabRadius          = 0.0f;
        float      sliderTrackRadius  = 0.0f;
        float      sliderThumbRadius  = 0.0f;

        // ---- 材质 / 光学 ----
        float      borderStrength     = 1.0f;
        float      surfaceSheenAlpha  = 0.0f;
        float      innerShadowAlpha   = 0.0f;
        float      scanlineAlpha      = 0.0f;
        int        scanlinePitch      = 0;
        float      vignetteAlpha      = 0.0f;
        float      glowIntensity      = 0.0f;

        // 仅当风格确实需要画布后处理时返回 true：
        //   glass（顶部镜面高光带）/ neon（内沿辉光）/ crt（扫描线+暗角）→ true；
        //   pinkXP / chrome / modern / greenScreen → 严格 false（零额外 GPU/CPU pass）。
        constexpr bool hasCanvasFx() const noexcept
        {
            return ! isChromaKey
                && (scanlineAlpha > 0.0f
                    || vignetteAlpha > 0.0f
                    || surfaceSheenAlpha > 0.0f
                    || frameStyle == FrameStyle::neonTube);
        }

        // ---- 电平柱 / LED 格 ----
        CellFinish cellFinish         = CellFinish::flatPixel;
        int        defaultCellGapPx   = 1;
        float      unlitCellAlpha     = 0.15f;
        float      cellHighlightAlpha = 0.36f;
        bool       peakCapGlow        = false;
        bool       hardPixelSteps     = true;

        // ---- 曲线 / 轨迹 ----
        float      strokeWidthScale   = 1.0f;
        bool       strokeGlowPass     = false;
        float      strokeGlowWidthMul = 1.0f;
        float      strokeGlowAlpha    = 0.0f;
        float      areaFillAlphaScale = 1.0f;
        bool       antiAliasStrokes   = false;

        // ---- 网格 / 准星 ----
        GridStyle  gridStyle          = GridStyle::dottedPixel;
        float      gridMajorAlpha     = 0.28f;
        float      gridMinorAlpha     = 0.14f;

        // ---- 表盘 / 指针 / HUD ----
        DialFinish dialFinish            = DialFinish::pixelHardware;
        float      dialGlossAlpha        = 0.0f;
        bool       needleShadow          = true;
        bool       needleHardPixelShadow = true;
        bool       needleGlow            = false;
        bool       statusDotCircular     = false;
        bool       statusDotGlow         = false;
        bool       pixelCornerAccents    = true;
    };

    // 读取指定风格（默认当前风格）的不可变令牌表（O(1) 静态查表，无分配）。
    //   · 当处于绿幕 / 透明背景模式（isChromaKeySkin() == true）时，返回该视觉风格对应的
    //     keyed 变体表：100% 继承 frameStyle / cellFinish / dialFinish / 圆角 / statusDotCircular
    //     等全部材质工艺，仅置 isChromaKey = true 并清零 scanline/vignette/sheen（hasCanvasFx()==false）。
    //   · 旧存档值 SkinId::fluent (6) 自动映射至 SkinId::modern (2)；
    //     遗留 SkinId::greenScreen (7) 自动映射至当前视觉风格的 keyed 令牌表。
    const SkinTokens& getSkinTokens (SkinId id = getCurrentSkin()) noexcept;

    // ----------------------------------------------------------
    // Craft —— CPU 侧风格化绘制原语
    //   所有函数自动读取 getSkinTokens() 与当前主题配色槽位。
    // ----------------------------------------------------------
    namespace Craft
    {
        // 容器凹槽 / 卡片 / 进度槽 / 徽章底板（Green Screen 下自动 no-op）
        void drawPanelWell (juce::Graphics& g,
                            juce::Rectangle<int> r,
                            juce::Colour fill,
                            WellKind kind = WellKind::canvas);

        // 通用交互控件外壳（按钮 / 下拉 / 开关 / 小工具按键）
        //   radiusOverride >= 0 时覆盖默认圆角（pinkXP / chrome / neon / crt 等直角风格除外，
        //   仅当风格允许圆角或调用方显式指定时生效；为保 Pixel XP 零圆角，pixelSnapped 风格恒 0px）。
        void drawControlChrome (juce::Graphics& g,
                                juce::Rectangle<int> r,
                                juce::Colour fill,
                                ControlState state = ControlState::normal,
                                float radiusOverride = -1.0f);

        // 设置抽屉 / 工具切换标签页按钮（含激活态标记 + 本地化光学居中文字）
        void drawTabButton (juce::Graphics& g,
                            juce::Rectangle<int> r,
                            bool active,
                            bool hover,
                            const juce::String& text,
                            float fontHeight = 10.5f);

        // 单颗电平 LED 格（Pixel XP：1px 左上像素高光；Green Screen：自动跳过未点亮格）
        //   pulse ∈ [0, 1] 为峰顶/尾格微脉动增量。
        void drawMeterCell (juce::Graphics& g,
                            juce::Rectangle<float> r,
                            juce::Colour col,
                            bool lit,
                            float pulse = 0.0f);

        inline void drawMeterCell (juce::Graphics& g,
                                   juce::Rectangle<int> r,
                                   juce::Colour col,
                                   bool lit,
                                   float pulse = 0.0f)
        {
            drawMeterCell (g, r.toFloat(), col, lit, pulse);
        }

        // 连续电平条 / 置信度条（可选自动绘制凹槽底衬）
        void drawContinuousBar (juce::Graphics& g,
                                juce::Rectangle<float> r,
                                juce::Rectangle<float> fillRect,
                                juce::Colour col,
                                bool vertical,
                                bool drawTrough = true);

        inline void drawContinuousBar (juce::Graphics& g,
                                       juce::Rectangle<int> r,
                                       juce::Rectangle<int> fillRect,
                                       juce::Colour col,
                                       bool vertical,
                                       bool drawTrough = true)
        {
            drawContinuousBar (g, r.toFloat(), fillRect.toFloat(), col, vertical, drawTrough);
        }

        // 读数卡片（自动转发 drawPanelWell(..., WellKind::card)，Green Screen 下 no-op）
        void drawReadoutCard (juce::Graphics& g,
                              juce::Rectangle<int> r,
                              juce::Colour fill);

        // 状态徽章 / 紧凑标签（Pixel XP：0px 锐利像素标签框；Green Screen 下 no-op）
        void drawBadgePill (juce::Graphics& g,
                            juce::Rectangle<int> r,
                            juce::Colour fill,
                            juce::Colour borderCol = {});

        // 状态指示灯（Pixel XP / CRT：锐利方形像素 LED；其余风格：精致圆形 LED）
        void drawStatusLed (juce::Graphics& g,
                            juce::Rectangle<float> bounds,
                            juce::Colour col,
                            bool lit,
                            bool useGlow = true);

        inline void drawStatusLed (juce::Graphics& g,
                                   juce::Rectangle<int> bounds,
                                   juce::Colour col,
                                   bool lit,
                                   bool useGlow = true)
        {
            drawStatusLed (g, bounds.toFloat(), col, lit, useGlow);
        }

        // 坐标网格 / 准星线：
        //   col 为基准色（通常不透明），最终不透明度 = col.alpha × (major ? gridMajorAlpha : gridMinorAlpha)。
        //   Green Screen 下自动 no-op；Pixel XP 自动画整数像素点阵线。
        void drawGridLine (juce::Graphics& g,
                           float x0, float y0,
                           float x1, float y1,
                           bool major,
                           juce::Colour col);

        // 风格化路径描边（Neon / CRT 自动叠加柔光底描边；Pixel XP 锐利无雾）
        void drawStyledStroke (juce::Graphics& g,
                               const juce::Path& path,
                               juce::Colour col,
                               float width = 1.5f,
                               juce::PathStrokeType::JointStyle joint = juce::PathStrokeType::curved,
                               juce::PathStrokeType::EndCapStyle cap = juce::PathStrokeType::rounded);

        // ---- 像素级几何原语（专为 Pixel XP 仪表盘 / 指针 / 刻度环 / 时钟打造）----

        // 整数 Bresenham 像素直线（零亚像素抗锯齿糊边，按行/列合并连续 span）
        void drawPixelLine (juce::Graphics& g,
                            int x0, int y0,
                            int x1, int y1,
                            juce::Colour col,
                            int thickness = 1);

        // 浮点端点像素粗线（坐标四舍五入至整数像素；hardShadow=true 时在 (+1,+1) 先画硬像素阴影）
        void drawPixelThickLine (juce::Graphics& g,
                                 float x0, float y0,
                                 float x1, float y1,
                                 juce::Colour col,
                                 int thickness = 2,
                                 bool hardShadow = false);

        // 整数像素圆弧（零亚像素糊边，逐行扫描环形扇区无重叠混合）：
        //   ★ 角度约定严格与 juce::Path::addCentredArc 一致：
        //     0 rad = 12 点钟方向（正上方，-Y），顺时针递增！
        //     x = cx + radius * std::sin(angle),  y = cy - radius * std::cos(angle)
        //   · stepPx <= 1：连续无缝整数像素圆弧；
        //   · stepPx >  1：按弧长步进 stepPx 像素的锐利像素点/刻度弧。
        void drawPixelArc (juce::Graphics& g,
                           float cx, float cy,
                           float radius,
                           float startAngleRad,
                           float endAngleRad,
                           juce::Colour col,
                           int thickness = 1,
                           int stepPx = 1);

        // 整数光栅化像素圆盘（表针轴心 / 像素指示点；border 非透明时外圈描 1px 像素边）
        void drawPixelDisc (juce::Graphics& g,
                            int cx, int cy,
                            int radius,
                            juce::Colour fill,
                            juce::Colour border = {});

        // 量化 Y2K 色带 + 2x2 Bayer 棋盘格抖动过渡（竖向 colA → colB，复古背光/灯罩用）
        void drawPixelDitherBand (juce::Graphics& g,
                                  juce::Rectangle<int> r,
                                  juce::Colour colA,
                                  juce::Colour colB,
                                  int bands = 3);

        // 画布数据绘制之后的光学质感覆层：
        //   · CRT：2px 扫描线 + 径向暗角
        //   · Glass：顶部 40% 玻璃反光穹顶带
        //   · Neon：微弱强调色内沿辉光
        //   · Pixel XP / Chrome / Modern / Green Screen：严格 no-op
        void drawCanvasFinishOverlay (juce::Graphics& g, juce::Rectangle<int> r);
    }
}
