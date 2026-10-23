#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"

// ==========================================================
// GlDirectModuleBase —— 每帧动态图形的 GL 直渲染统一框架（v2.9.4）
//
//   背景：v2.9.3 先后把 Spectrogram3D / Oscilloscope / OscilloscopeWave
//   的每帧动态图形从 JUCE 组件绘制（CPU 光栅化 + 每帧纹理重传）迁移到
//   Editor::renderOpenGL 里的 VBO 直画。三个模块各自复制了一份样板：
//   快照锁、版本计数、VBO 句柄、外壳透明钩子、挖洞填充 paintContent，
//   Editor 侧也有三个几乎相同的渲染 lambda。本类把样板收敛成一份：
//
//   子类只需要回答三个问题：
//     1. buildGlSnapshot()  —— 本帧顶点流长什么样（UI 线程，纯计算）；
//     2. paintGlOverlay()   —— 接管后组件缓存里画哪些文字/标记；
//     3. paintLegacyContent() —— 未被接管时（GL 不可用/回退态）的旧绘制路径。
//
//   接管流程（全自动）：
//     UI 线程 buildGlSnapshot() 发布快照 → Editor GL 线程 z 序遍历时
//     getGlSnapshot() 取流、懒建 VBO、版本化上传、按 segments 逐段 draw →
//     首次绘制成功后置 glDirectActive → 此后外壳透明（透出 FBO 0 的 GL
//     内容）、paintContent 走挖洞 + overlay 分支。
//
//   本框架内化的历史教训：
//     · 每模块独立 VBO（共享单 VBO + 版本化跳过上传会画出别的模块的数据）；
//     · attrib 指针只在绑 VBO 后设置一次，多段 draw 只用 offset 区分
//       （多 VBO 切换时 attrib 不跟随 ARRAY_BUFFER 绑定）；
//     · GL 渲染段结束时恢复 program/blend 状态（否则污染 JUCE 组件绘制）。
// ==========================================================
class GlDirectModuleBase : public ModulePanel
{
public:
    explicit GlDirectModuleBase (ModuleType type) : ModulePanel (type) {}

    enum class Primitive : juce::uint32 { Triangles = 0, Lines = 1 };

    // 单一顶点流：x,y,r,g,b,a 交错（24B/顶点），顶点坐标为"模块本地"坐标。
    //   一个流可分多段（S3D 的"背景 + 逐层条带/轮廓交错"），段描述绘制顺序。
    //   snapToPixels：顶点在 shader 里对齐物理像素中心（floor+0.5）。
    //   renderToLogical：渲染到"逻辑分辨率"FBO 再线性放大 blit（与旧版
    //   "CPU 光栅化到逻辑纹理 + 纹理放大"完全等效）。150% DPI 下 1 逻辑
    //   像素宽的柱在物理网格上无解（1.5px 节距必产生交替混合 → 滚动闪动，
    //   Waveform 迁移实测），逻辑域渲染是唯一逐像素一致的方案。
    //   启用时：顶点坐标域 = canvasLocal（逻辑），logicalW/H = canvas 尺寸。
    //   ---- 纹理通道（v2.9.5，瀑布图等逐像素数据）----
    //   与顶点流并存：先画顶点（背景/边框），再画纹理 quad（texTargetRect，
    //   双线性放大，uv 子区窗口 = 双拷贝环形滚动的零 memmove 映射）。
    //   ---- trail 累积通道（v2.9.5，GPU 端持久残影，如 StereoField）----
    //   trailFbo=true 时：segments[0..trailSegStart) 直接画后缓冲（静态背景），
    //   其余段画进持久 trail FBO；每帧先做衰减 pass（×trailDecay）再画点，
    //   最后用纹理 shader 把 trail 纹理 alpha 混合到 texTargetRect。
    struct VertexStream
    {
        bool snapToPixels = false;
        bool renderToLogical = false;
        int  logicalW = 0, logicalH = 0;
        juce::Rectangle<int> canvasLocal;   // 逻辑渲染的画布矩形（blit 目标）

        // 纹理通道
        int  texW = 0, texH = 0;
        bool texDirty = false;
        float texScrollU = 0.0f;            // uv 子区窗口起点（0..1）
        float texWinW = 1.0f;               // uv 子区窗口宽度（0..1]
        juce::Rectangle<int> texTargetRect; // 模块本地目标矩形
        // ARGB 字节序（0xAARRGGBB = juce::Colour::getARGB，小端内存即 B,G,R,A），
        //   Editor 以 GL_BGRA 上传避免字节重排；dirty 时全量上传
        std::vector<juce::uint32> texPixels;

        // trail 累积通道
        bool  trailFbo = false;
        float trailDecay = 0.92f;
        int   trailSegStart = 0;            // segments 前缀（背景）画后缓冲

        juce::uint64 version = 0;
        std::vector<float> vertices;

        struct Segment
        {
            int       offset    = 0;   // 顶点下标
            int       count     = 0;
            Primitive primitive = Primitive::Triangles;
        };
        std::vector<Segment> segments;
    };

    // ---- UI 线程 ----
    virtual void buildGlSnapshot() = 0;      // 构建本帧顶点流（无内容则发布空流）

    // ---- GL 线程 ----
    std::shared_ptr<VertexStream> getGlSnapshot() const;   // SpinLock 内取 shared_ptr
    std::atomic<bool> glDirectActive { false };            // 首帧绘制成功后接管
    unsigned     glVbo = 0;                                // 本模块专属 VBO（Editor 懒创建）
    unsigned     glFbo = 0, glFboTex = 0;                  // 逻辑分辨率渲染 FBO（懒创建）
    int          glFboW = 0, glFboH = 0;
    unsigned     glTex = 0;                                // 纹理通道纹理（懒创建）
    bool         glTrailCleared = false;                   // trail FBO 是否已清透明
    juce::uint64 lastUploadedVersion = 0;                  // 已上传版本（GL 线程写）

    // 浮动态（脱离到独立窗口）时模块移出 Editor 组件树，Editor 不再渲染它的
    //   GL 内容 —— 复位接管标志回退软件路径，避免画布区域纯透明（v2.9.4）。
    //   dock 回来：保持未接管，由 Editor 下一帧渲染成功后重新置位（不能在此
    //   主动置 true，否则"挖洞透明"先行、GL 内容还没画 → 内部透明）。
    //   两个方向都 repaint：接管标志变化后组件缓存若不立即重绘，会残留
    //   另一条路径的内容（dock 回来"内部透明 + 拖影"的根源）。
    void setFloating (bool floating) noexcept override
    {
        ModulePanel::setFloating (floating);
        if (floating && glDirectActive.load())
            glDirectActive.store (false);
        repaint();
    }

protected:
    // 快照重建尺寸守卫（v2.9.5）：拖动窗口 / dock 时 workspace 每帧对所有
    //   模块重新 layout，layoutContent 若每帧无条件重建快照（S3D 3.7MB 顶点
    //   ~2ms + 各模块 ~3-5ms × 60fps），消息线程会被打满 → 交互严重卡顿。
    //   调用方（layoutContent）应只在本函数返回 true 时重建。
    bool glCanvasSizeChanged (juce::Rectangle<int> canvas) noexcept
    {
        if (canvas.getWidth() == lastGlCanvasW_ && canvas.getHeight() == lastGlCanvasH_)
            return false;
        lastGlCanvasW_ = canvas.getWidth();
        lastGlCanvasH_ = canvas.getHeight();
        return true;
    }

    // 浮动态（脱离到独立窗口）没有 GL 渲染目标 —— 应跳过快照构建。
    //   拖动浮动窗口 resize 每帧触发 layoutContent，若仍全量重建快照
    //   （S3D ~2ms 等），消息线程被打满 → 浮动窗口交互卡死（v2.9.5 实测）。
    //   各模块 buildGlSnapshot 开头调用本函数守卫。
    bool wantsGlSnapshot() const noexcept { return ! isFloating(); }

    // 接管后外壳填充透明（透出 FBO 0 的 GL 内容）
    juce::Colour getShellFillColour() const override
    {
        return glDirectActive.load() ? juce::Colours::transparentBlack : PinkXP::face;
    }

    // 统一 paintContent：接管时挖洞填充 + overlay 文字；否则旧路径
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) final;

    // ---- 子类钩子 ----
    // GL 直渲染的画布区域（模块本地坐标，用于挖洞）
    virtual juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) = 0;
    // 接管后画在 GL 内容之上的文字/标记（图例、轴标签、说明）
    virtual void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) {}
    // 未被接管时的完整旧绘制路径（含 btnFace 底衬，子类全权处理）
    virtual void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) = 0;

    // ---- 顶点构建辅助 ----
    // 新快照对象（自动分配自增版本号）
    std::shared_ptr<VertexStream> beginGlSnapshot();
    // 发布快照（SpinLock 保护）
    void publishGlSnapshot (std::shared_ptr<VertexStream> snap);
    // 把一个矩形 quad（2 三角形 × 6 floats/顶点）追加进顶点流
    static void appendQuad (std::vector<float>& v, juce::Rectangle<float> r, juce::Colour c);
    // 追加一条 1px 粗折线段（两端点连线的细 quad，用于网格线/圆环）
    static void appendLineSegment (std::vector<float>& v,
                                   float x0, float y0, float x1, float y1,
                                   float halfWidth, juce::Colour c);
    // 追加一个实心圆盘（三角扇，用于指针轴心等小圆）
    static void appendDisc (std::vector<float>& v,
                            float cx, float cy, float radius,
                            juce::Colour c, int segments = 16);
    // 追加凹陷背景（与 PinkXP::drawSunken 逐笔对应的 9 矩形，v2.9.5 收敛
    //   此前 8 个模块各自复制的样板）
    static void appendSunkenFrame (std::vector<float>& v, juce::Rectangle<int> canvas);

private:
    mutable juce::SpinLock glSnapshotLock;
    std::shared_ptr<VertexStream> glSnapshot;
    juce::uint64 glSnapshotVersion = 0;
    int          lastGlCanvasW_ = 0, lastGlCanvasH_ = 0;   // 尺寸守卫缓存

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlDirectModuleBase)
};
