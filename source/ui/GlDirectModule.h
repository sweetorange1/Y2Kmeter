#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <string>
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"

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
    explicit GlDirectModuleBase (ModuleType type);
    ~GlDirectModuleBase() override;

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
        juce::Rectangle<int> fxRect;        // 可选后处理区域（模块本地坐标；空时默认取 canvasLocal 或 texTargetRect）

        // 纹理通道
        struct TexRowPatch
        {
            int rowStart = 0;
            int rowCount = 0;
            std::vector<juce::uint32> pixels;
        };
        int  texW = 0, texH = 0;
        bool texDirty = false;
        bool texLinear = false;             // true = GL_LINEAR 平滑过滤，false = GL_NEAREST 像素风
        bool texTransposed = false;         // true = 列主序转置纹理（u=Y，v=X 环形列，单行连续 glTexSubImage2D）
        bool texPremultiplied = false;      // true = 纹理已预乘 Alpha（GL_ONE, GL_ONE_MINUS_SRC_ALPHA 无暗边）
        bool texWrapRepeatT = false;        // true = GL_TEXTURE_WRAP_T 设 GL_REPEAT（环形滚动零接缝）
        juce::uint64 texDataGen = 0;        // 纹理内容/过滤/尺寸代次（0 表示未编号，非零与 glTexUploadedGen 配对）
        float texScrollU = 0.0f;            // uv 子区窗口起点（0..1；transposed 时为 v 起点）
        float texWinW = 1.0f;               // uv 子区窗口宽度（0..1]；transposed 时可为负以实现反向滚动）
        juce::Rectangle<int> texTargetRect; // 模块本地目标矩形
        // ARGB 字节序（0xAARRGGBB = juce::Colour::getARGB，小端内存即 B,G,R,A），
        //   Editor 以 GL_BGRA 上传避免字节重排；dirty 时全量上传，增量更新走 texRowPatches
        std::vector<juce::uint32> texPixels;
        std::vector<TexRowPatch>  texRowPatches;

        // trail 累积通道
        bool  trailFbo = false;
        float trailDecay = 0.92f;
        int   trailSegStart = 0;            // segments 前缀（背景）画后缓冲
        int   trailLayers = 1;              // 残影层数 1..3：每层独立 FBO、独立衰减
        bool  trailClear = false;           // true = 本帧先把所有层清空（切模式 / 改参数）

        juce::uint64 version = 0;
        std::vector<float> vertices;

        struct Segment
        {
            int       offset    = 0;   // 顶点下标
            int       count     = 0;
            Primitive primitive = Primitive::Triangles;
            int       trailLayer = 0;  // 该段写入哪一层残影（仅 trail 段有效）
        };
        std::vector<Segment> segments;
    };

    // ---- UI 线程 ----
    virtual void buildGlSnapshot() = 0;      // 构建本帧顶点流（无内容则发布空流）
    void setGlFrameRequester (std::function<void()> fn) { glFrameRequester_ = std::move (fn); }
    void setGlHostAvailable (bool available) noexcept { glHostAvailable_.store (available, std::memory_order_release); }
    bool isGlHostAvailable() const noexcept { return glHostAvailable_.load (std::memory_order_acquire); }

    // 切换宿主 GL 上下文（停靠 ↔ 脱离 / 恢复 GL）后强制立即重建一帧快照
    void forceRebuildGlSnapshot()
    {
        lastGlCanvasW_ = 0;
        lastGlCanvasH_ = 0;
        if (wantsGlSnapshot())
            buildGlSnapshot();
    }

    // 仅唤醒当前宿主 GL 线程出帧、不标脏软件 CachedImage（停靠态与独立 GL 浮窗通用，
    //   避免大尺寸模块每帧触发数 MB 的软件组件缓存重绘与纹理上传）。
    bool requestGlFrameOnly()
    {
        if (glDirectActive.load (std::memory_order_acquire) && glFrameRequester_)
        {
            glFrameRequester_();
            return true;
        }
        return false;
    }

    // ---- GL 线程（与 GlDirectRenderer 交互）----
    // 单调递增唯一键：GlDirectRenderer 在各自上下文中按此键隔离 VBO/FBO/纹理，
    //   停靠 ↔ 脱离跨 HGLRC 迁移时零句柄串扰、零 ABA 复用风险。
    const juce::uint64 glResourceKey;
    std::shared_ptr<VertexStream> getGlSnapshot() const;   // SpinLock 内取 shared_ptr
    std::atomic<bool> glDirectActive { false };            // 首帧绘制成功后接管
    std::atomic<juce::uint64> glTexUploadedGen { 0 };      // 最近一次成功上传的 texDataGen（GL 线程 release 写，UI 线程 acquire 读）
    std::atomic<bool> glForceTrailClear { false };         // 主题/皮肤切换或跨上下文迁移时置位，由 GL 线程消费清空旧残影

    // 停靠 ↔ 浮动态切换时模块更换 GL 宿主上下文（Editor ↔ FloatingModuleWindow）：
    //   先复位接管标志与纹理上传代次，待新宿主首帧 GL 渲染成功后再置回 glDirectActive=true，
    //   避免跨上下文切换瞬间出现透明洞或漏传全量纹理。
    void setFloating (bool floating) override
    {
        ModulePanel::setFloating (floating);
        if (glDirectActive.load (std::memory_order_acquire))
            glDirectActive.store (false, std::memory_order_release);
        glTexUploadedGen.store (0, std::memory_order_release);
        glForceTrailClear.store (true, std::memory_order_release);
        lastGlCanvasW_ = 0;
        lastGlCanvasH_ = 0;
        diagSetFloating (floating);
        repaint();
    }

    // ---- 诊断状态镜像（v3.0.x）----
    //   只写原子量；看门狗线程在卡死时读取，绝不回读组件树。
    //   全部在 .cpp 中实现（注册表也放在那里，避免头文件互相拖累）。
    static juce::String dumpDiagStates();                       // 卡死报告用（任意线程）
    void diagSetVisible (bool visible) noexcept;                // GL 线程，每帧
    void diagOnRendered (double renderMs) noexcept;             // GL 线程，每帧
    void diagSetFloating (bool floating) noexcept;

    int diagSlot = -1;                                          // 全局注册表下标
    std::string diagWaitPhase;                                  // 预生成的阶段标记（免热路径分配）
    std::string diagRenderPhase;                                // "GlDirect.render(<模块名>)"
    std::atomic<int> diagBuildUs { -1 };                        // 最近一次快照构建耗时
    std::atomic<int> diagRenderUs { -1 };                       // 最近一次渲染耗时
    std::atomic<int> diagRenderMaxUs { 0 };                     // 渲染耗时峰值
    std::atomic<unsigned> diagRenderCount { 0 };                // 累计渲染帧数
    double diagBuildStartMs = 0.0;                              // 仅 UI 线程使用

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

    // 只要当前宿主（Editor 或带独立 OpenGLContext 的 FloatingModuleWindow）存在 GL 管线，
    //   就持续构建快照，实现停靠态与脱离态 100% 统一的 GPU 直渲染。
    bool wantsGlSnapshot() const noexcept { return glHostAvailable_.load (std::memory_order_acquire); }

    // 接管后外壳填充透明（透出 FBO 0 的 GL 内容）；
    //   未接管时同步遵循透明/绿幕背景模式（透明模式未悬停时保持 alpha=0，不刷绿或 face）
    juce::Colour getShellFillColour() const override
    {
        if (glDirectActive.load())
            return juce::Colours::transparentBlack;
        if (PinkXP::isTransparentBackdrop() && ! PinkXP::isChromaRevealed (*this))
            return juce::Colours::transparentBlack;
        if (PinkXP::isGreenScreenBackdrop() && ! PinkXP::isChromaRevealed (*this))
            return PinkXP::chromaKeyGreen();
        return PinkXP::face;
    }

    // 统一 paintContent：接管时挖洞填充 + overlay 文字；否则旧路径
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) final;

    // 逐帧失效区域：接管后组件层随帧变化的只有画布内的 overlay（几何由 GL
    //   每帧直画，与组件层脏区无关），画布外的 btnFace 底衬 / 控件条是静态的。
    //   外扩 4px 兜住贴边绘制的文字；未接管时退回整个内容区（旧路径）。
    juce::Rectangle<int> getFrameRepaintBounds() override
    {
        const auto content = getContentBounds();
        if (glDirectActive.load())
        {
            const auto canvas = getGlCanvasBounds (content);
            if (canvas.getWidth() > 8 && canvas.getHeight() > 8)
                return canvas.expanded (4).getIntersection (content);
        }
        return content;
    }

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

public:
    // 把一个矩形 quad（2 三角形 × 6 floats/顶点）追加进顶点流
    static void appendQuad (std::vector<float>& v, juce::Rectangle<float> r, juce::Colour c);
    // 追加一条粗折线段（两端点连线的细 quad，用于网格线/圆环）
    static void appendLineSegment (std::vector<float>& v,
                                   float x0, float y0, float x1, float y1,
                                   float halfWidth, juce::Colour c);
    // 追加一个实心圆盘（三角扇，用于指针轴心等小圆）
    static void appendDisc (std::vector<float>& v,
                            float cx, float cy, float radius,
                            juce::Colour c, int segments = 16);
    // 令牌驱动的画布凹槽底板（按 PinkXP::getSkinTokens() 统一分派全部 7 种视觉风格，
    //   与 CPU 侧 PinkXP::Craft::drawPanelWell / drawSunken 严格一致）
    static void appendWellFrame (std::vector<float>& v, juce::Rectangle<int> canvas, juce::Colour fill);
    // 向后兼容转发包装：直接转发至 appendWellFrame(v, canvas, PinkXP::content)
    static void appendSunkenFrame (std::vector<float>& v, juce::Rectangle<int> canvas);
    // 圆角矩形 quad（radius <= 0.5 或 tokens.pixelSnapped 时直接退化为 2 三角形锐利直角）
    static void appendRoundedQuad (std::vector<float>& v, juce::Rectangle<float> rect,
                                   juce::Colour col, float radius, int cornerSegs = 4);
    // 风格化电平灯格（Pixel XP 锐利方块+1px 左上高光；平滑/霓虹/CRT 风格按 CellFinish 绘制；
    //   Green Screen 自动跳过未点亮格）
    static void appendStyledBarCell (std::vector<float>& v, juce::Rectangle<float> rect,
                                     juce::Colour col, bool lit, float pulse = 0.0f);
    // 风格化折线（tokens.strokeGlowPass 为 true 时先叠宽柔光底带，再画平滑抗锯齿主线条；
    //   不做像素阶梯吸附，确保斜线/曲线/圆弧在所有风格下柔滑无锯齿）
    static void appendStyledPolyline (std::vector<float>& v, const juce::Point<float>* pts,
                                      int count, juce::Colour col, float width);
    // 风格化坐标网格线（Pixel XP 像素点阵 / Modern 细实线 / Neon 科技虚线 /
    //   CRT 磷光标尺 / Green Screen 自动跳过）
    static void appendGridLine (std::vector<float>& v, float x0, float y0,
                                float x1, float y1, bool major, juce::Colour col);

private:
    struct GlAppearanceRefreshAsync final : juce::AsyncUpdater
    {
        explicit GlAppearanceRefreshAsync (GlDirectModuleBase& o) : owner (o) {}
        void handleAsyncUpdate() override { owner.syncGlAppearanceIfStale (true); }
        GlDirectModuleBase& owner;
    };

    void syncGlAppearanceIfStale (bool requestRepaint);

    mutable juce::SpinLock glSnapshotLock;
    std::shared_ptr<VertexStream> glSnapshot;
    juce::uint64 glSnapshotVersion = 0;
    int          lastGlCanvasW_ = 0, lastGlCanvasH_ = 0;   // 尺寸守卫缓存
    int          glThemeSubToken_ = -1;
    juce::uint32 lastGlAppearanceGen_ = 0;
    juce::uint32 lastGlFontGen_ = 0;
    GlAppearanceRefreshAsync glRefreshAsync_ { *this };
    std::function<void()>    glFrameRequester_;
    std::atomic<bool>        glHostAvailable_ { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlDirectModuleBase)
};
