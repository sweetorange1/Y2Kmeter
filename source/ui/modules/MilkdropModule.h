#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "projectM-4/types.h"
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"
#include "source/ui/modules/MilkdropVisualState.h"
#include "source/ui/modules/MilkdropEffect.h"

class Y2KmeterAudioProcessorEditor;  // 前向声明，用于 Milkdrop 脱离后仍能桥接项目M状态

// ==========================================================
// MilkdropModule —— Y2Kmeter 内置 Milkdrop 可视化模块
//
// 变更历史：
//   v2.0.3 起：初版 WebView2 + Butterchurn（JS）实现
//   v2.0.4  ：切换为原生 libprojectM 4（LGPL-2.1）+ juce::OpenGLContext
//             起因：WebView2 方案在"运行时手动添加多个模块"场景下
//             无法可靠触发 WebResourceRequested → 索性抛弃 Web 栈，
//             改为跨模块共享的 C ABI + 每模块独立 GL context 的方案。
//
// 架构概览：
//   本模块 = ModulePanel（Y2K 卡片外壳，粉色标题栏 + 边框）
//          + 内嵌 GLView 子组件（占据内容区，独立 OpenGL 上下文）
//   ┌────────────────────────────────────────┐
//   │ ModulePanel::paint()   ← 画标题栏、边框（CPU/JUCE 顶层 GL）│
//   │  ┌──────────────────────────────────┐  │
//   │  │  GLView (juce::Component)        │  │
//   │  │   ├─ juce::OpenGLContext.attach  │  │
//   │  │   └─ OpenGLRenderer::renderOpenGL│  │
//   │  │        └─ projectm_opengl_render_frame │
//   │  └──────────────────────────────────┘  │
//   └────────────────────────────────────────┘
//
//   为什么用嵌套 GLView，而不是让 MilkdropModule 自己 attach？
//     · MilkdropModule 是 ModulePanel，需要自己 paint 出 Y2K 卡片标题栏/边框
//       （CPU 路径）；若整个 ModulePanel 都被一个 OpenGLContext 接管，
//       粉色边框/标题的 CPU 绘制会被 GL 内容覆盖或走进 CachedImage 路径，
//       出现视觉错乱与线程告警。
//     · 只把内容区用一层"独立 GL surface 的子组件"接管，最干净：
//       父类完成外框绘制后，GLView 在其位置上叠加自己的 GL 输出，
//       视觉上完美嵌合。
//
//   attach 时机（重要）：
//     GLView 构造时"不要"立刻 attachTo。要等到 Component 已被加入桌面
//     层级且 isShowing() == true 之后再 attach，否则会在 GL 渲染线程
//     被回调 paint() 时触发 juce_OpenGLContext.cpp:239 的 jassertfalse
//     （"paint has been called from a thread other than the message thread"）。
//     具体做法：parentHierarchyChanged / visibilityChanged 里判断 isShowing()
//     后调 attachIfNeeded()。
//
// 数据流：
//   AnalyserHub.retain(Kind::Oscilloscope)
//     → hub 30Hz Timer.timerCallback()
//     → FrameSnapshot.oscL/oscR (2048 立体声样本，UI 线程)
//     → MilkdropModule::onFrame() 把 L/R 交错拷到 pcmInterleaved（锁保护）
//     → GLView::renderOpenGL() (GL 线程) 抓最新 pcm buffer
//     → projectm_pcm_add_float + projectm_opengl_render_frame
//
// 生命周期规则（重要）：
//   · projectM handle 必须在 GLView 的 GL 线程创建/销毁，绝不能跨线程。
//   · GLView::newOpenGLContextCreated() —— 创建 handle & 设置搜索路径。
//   · GLView::openGLContextClosing()    —— destroy handle。
//   · MilkdropModule 析构：先 detachGL()（同步等 GL 线程收尾），再 removeFrameListener。
//
// 多实例安全性：
//   每个 GLView 有独立的 OpenGLContext（自己的 GL 线程 + surface），
//   projectM handle 与 context 一一绑定；多个 Milkdrop 模块并存时
//   相互不干扰。projectM::Api 是单例但只做函数指针查找，无实例状态。
// ==========================================================
// MilkdropTintPass —— projectM 输出帧的整体视觉后处理着色器
//
// 负责在 projectM 渲染完成后、blit 到各模块之前，对 offscreen FBO 采样并应用
// 完整视觉状态（MilkdropVisualState）。shader 为统一 pass 内多效果分支，
// 与 MilkDrop3 Effect Injection（ADDMILKEFFECT）的执行顺序对齐：
//   efftop（采样前 uv 重映射：split → zoom → multi）
//   → tint（RGB 加性偏移）→ bright（增益）
//   → effbottom（采样后 ret 变换：shadows → invert → solarize → rainbow → blow → burn）
//   → clamp。
//
// 使用约束：
//   · init() / shutdown() / apply() 都必须在持有活动 OpenGL 上下文的 GL 线程
//     调用（分别对应 newOpenGLContextCreated / openGLContextClosing / renderOpenGL）。
//   · macOS Core Profile 用 GLSL 150 + VAO；Windows Compatibility 用 GLSL 120，
//     避免 Windows 端强制 Core Profile 导致 projectM 预设 shader 编译失败的历史问题。
// ==========================================================
class MilkdropTintPass
{
public:
    explicit MilkdropTintPass (juce::OpenGLContext& context);

    // GL 线程调用：编译着色器并建立全屏三角形顶点缓冲。
    bool init();
    // GL 线程调用：释放全部 GL 资源。
    void shutdown();

    // GL 线程调用：采样 srcTex（当前帧 projectM 输出），应用完整视觉状态
    // （RGB 染色 + bright + invert/shadows/solarize/split/zoom/multi/rainbow/blow/burn），
    // 把全屏三角形绘制到当前绑定的 framebuffer。
    void apply (GLuint srcTex, const MilkdropVisualState& state);

    bool isReady() const noexcept { return ready_; }
    juce::String getLastError() const { return lastError_; }

private:
    juce::OpenGLContext& context_;
    std::unique_ptr<juce::OpenGLShaderProgram> program_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLint  texLoc_               = -1;
    GLint  tintLoc_              = -1;
    GLint  brightnessLoc_        = -1;
    GLint  invertLoc_            = -1;
    GLint  shadowsLoc_           = -1;
    GLint  shadowsStrengthLoc_   = -1;
    GLint  solarizeLoc_          = -1;
    GLint  splitLoc_             = -1;
    GLint  zoomLoc_              = -1;
    GLint  multiLoc_             = -1;
    GLint  rainbowLoc_           = -1;
    GLint  blowLoc_              = -1;
    GLint  burnLoc_              = -1;
    GLint  kaleidoscopeLoc_      = -1;
    GLint  swirlLoc_             = -1;
    GLint  pinchLoc_             = -1;
    GLint  pixelateLoc_          = -1;
    GLint  glitchLoc_            = -1;
    GLint  posterizeLoc_         = -1;
    GLint  sepiaLoc_             = -1;
    GLint  grayscaleLoc_         = -1;
    GLint  edgeLoc_              = -1;
    GLint  vignetteLoc_          = -1;
    bool   coreProfile_ = false;
    bool   ready_ = false;
    juce::String lastError_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MilkdropTintPass)
};

// ==========================================================
class MilkdropModule : public ModulePanel,
                       public AnalyserHub::FrameListener
{
public:
    /**
     * @param hub 供拿 PCM 样本用。可选：若为 nullptr，模块显示"无音频输入"，
     *            但 projectM 仍会用其内部静音路径渲染 idle 预设。
     * @param editor Editor 引用，用于 projectM 桥接（脱离/浮动模式仍可用）
     */
    explicit MilkdropModule (AnalyserHub* hub,
                             Y2KmeterAudioProcessorEditor* editor);
    ~MilkdropModule() override;

    // === ModulePanel 覆写 ===
    void paint(juce::Graphics& g) override;  // 跳过内容区填充，projectM已由Editor渲染
    void paintContent(juce::Graphics& g,
                      juce::Rectangle<int> contentBounds) override;
    void layoutContent(juce::Rectangle<int> contentBounds) override;

    // === Editor projectM 桥接 ===
    // 返回本模块内容区在 MilkdropModule 自身坐标系中的矩形，
    // 供 Editor::renderOpenGL 通过 getLocalArea 转换为 Editor-local 坐标设置 viewport。
    juce::Rectangle<int> GetContentLocalBounds() const;

    // === 预设索引持久化 ===
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState(const juce::ValueTree& state) override;

    // === AnalyserHub::FrameListener 覆写 ===
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    // === 用户交互 API ===
    void nextPreset();
    void prevPreset();
    void randomPreset();
    void jumpToPresetIndex(int index);  ///< UI 线程调用：请求跳转到指定预设索引

    // === 焦点与叠加层交互 ===
    void setFocusVisual(bool shouldFocus);
    void touchOverlayIdleTimer() { lastInteractionTime_ = juce::Time::getMillisecondCounter(); }
    void checkOverlayAutoHide();  ///< 由 GLView::timerCallback 在 UI 线程轮询调用
    void checkAutoMode();         ///< 由 GLView::timerCallback 在 UI 线程轮询，自动切换预设

    // === 自动轮播模式 ===
    bool isAutoModeActive() const noexcept { return isAutoMode_; }
    void toggleAutoMode();
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

private:
    // ------------------------------------------------------
    // GLView：嵌入态为纯 CPU 子组件，浮动态挂载独立 OpenGLContext。
    //   嵌入态 projectM 仍由 Editor GL 上下文直接渲染；浮动态使用自身
    //   native surface，避免画面受 Editor 主窗口 framebuffer 尺寸裁剪。
    // ------------------------------------------------------
    class GLView : public juce::Component,
                   private juce::OpenGLRenderer,
                   private juce::Timer
    {
    public:
        explicit GLView(MilkdropModule& owner);
        ~GLView() override;

        // Timer: UI 线程 30Hz
        void timerCallback() override;

        void parentHierarchyChanged() override;
        void visibilityChanged() override;
        void resized() override;
        void paint(juce::Graphics& g) override;

        // OpenGLRenderer：仅浮动态 attach 后会被调用
        void newOpenGLContextCreated() override;
        void renderOpenGL() override;
        void openGLContextClosing() override;

        // 鼠标事件转发给 owner
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;

        // 推送 PCM 到 Editor 与浮动态本地 renderer（UI 线程安全）
        void PushPcm(const float* interleaved_lr, unsigned int frame_count);

        // Preset 请求
        void RequestPresetDelta(int delta);
        void RequestPresetRandom();
        void RequestPresetJump(int index);
        void RequestRenderScale();  // 循环 1→2→4→1
        int  GetLocalRenderScale() const noexcept { return local_render_scale_; }

        // 诊断
        bool IsRenderReady() const;
        juce::String GetError() const;
        int  GetCurrentPresetIndex() const;
        void SyncOwnerPresetIndexFromRenderer() const;
        int  GetTotalPresetCount() const;
        juce::String GetCurrentPresetName() const;
        int64_t GetLastPresetSwitchTimeMs() const;

        // 最后一帧快照（UI 线程读取，GL 线程写入，互斥锁保护）
        // 用于在 detach/attach 重建期间显示上一帧画面，消除切换模式时的黑屏闪烁。
        juce::Image GetLastFrameSnapshot() const;

        // 扫描 milkdrop_presets 目录（纯 CPU 磁盘 IO，无 GL 依赖）。
        // 在 GL 线程 newOpenGLContextCreated 中调用，保证读取
        // restored_preset_index_ 前预设列表是最新且完整的。
        void ScanPresetFiles();

    private:
        void UpdateOpenGLAttachment();
        void DetachOpenGL();
        void LoadCurrentPreset();
        void ConsumePresetRequests();
        void ConsumePcm();
        void CaptureLastFrame();  ///< 在 GL 线程（openGLContextClosing）中抓取最后一帧

        MilkdropModule& owner_;
        juce::OpenGLContext open_gl_context_;
        projectm_handle local_pm_handle_ = nullptr;
        bool local_render_ready_ = false;
        juce::String local_error_;
        juce::StringArray local_preset_paths_;
        int local_current_preset_ = -1;
        int local_render_scale_ = 1;
        std::atomic<int> requested_preset_delta_{0};
        std::atomic<int> requested_preset_jump_{-1};
        std::atomic<bool> requested_preset_random_{false};
        std::mutex pcm_mutex_;
        std::vector<float> pending_pcm_;
        unsigned int pending_frames_ = 0;
        int64_t last_preset_switch_ms_ = 0;
        bool attached_ = false;
        bool first_focus_done_ = false;

        // 最后一帧快照（GL 线程写，UI 线程读，snapshot_mutex_ 保护）
        mutable std::mutex snapshot_mutex_;
        juce::Image last_frame_snapshot_;

        // 降分辨率离屏 FBO（GL 线程独占，仅在 local_render_scale_ > 1 时使用）
        // 流程：projectM 渲染到 scale_fbo_（小尺寸），再 blit 到默认 framebuffer（全屏）
        GLuint scale_fbo_     = 0;   ///< 离屏 framebuffer object
        GLuint scale_texture_ = 0;   ///< 附加到 scale_fbo_ 的颜色纹理
        int    scale_fbo_w_   = 0;   ///< 当前 FBO 宽度（像素）
        int    scale_fbo_h_   = 0;   ///< 当前 FBO 高度（像素）

        // 整体染色后处理着色器（浮动态 / macOS 本地 GL 上下文渲染路径使用）。
        // 在 newOpenGLContextCreated 创建并初始化，openGLContextClosing 释放。
        std::unique_ptr<MilkdropTintPass> tint_pass_;

        /// 确保离屏 FBO 尺寸与 render_w×render_h 匹配，必要时重建。
        /// 必须在 GL 线程调用。
        void EnsureScaleFbo(int render_w, int render_h);
        /// 销毁离屏 FBO 及纹理，必须在 GL 线程调用。
        void DestroyScaleFbo();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GLView)
    };

    // ------------------------------------------------------
    // AutoIntervalDialog：自定义 PinkXP 风格自动轮播间隔设置对话框
    //   点击 auto 行中的时间数值时弹出，允许用户直接输入间隔秒数。
    // ------------------------------------------------------
    class AutoIntervalDialog : public juce::Component
    {
    public:
        AutoIntervalDialog(MilkdropModule& owner_, float current,
                           std::function<void(float)> onResult);
        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent& e) override;

    private:
        MilkdropModule& owner_;
        std::function<void(float)> onResult_;
        juce::TextEditor editor_;
    };

    // ------------------------------------------------------
    // PresetJumpDialog：自定义 PinkXP 风格预设跳转对话框
    //   替代 juce::AlertWindow，消除 Windows 系统提示音，
    //   并保持与插件整体 UI 风格一致。
    // ------------------------------------------------------
    class PresetJumpDialog : public juce::Component
    {
    public:
        PresetJumpDialog(MilkdropModule& owner_, int total, int current,
                         std::function<void(int)> onResult);
        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent& e) override;

    private:
        MilkdropModule& owner_;
        int total_;
        std::function<void(int)> onResult_;
        juce::TextEditor editor_;
    };

#if JUCE_MAC
    // ------------------------------------------------------
    // OverlayView（仅 macOS）：以独立顶层原生窗口（NSWindow）形式悬浮在
    //   MilkdropModule 内容区上方，专门负责绘制控制栏（overlay control bar）。
    //
    //   为什么必须用顶层窗口？
    //   macOS 上 juce::OpenGLContext::attachTo(GLView) 会为 GLView 创建
    //   一个原生 NSOpenGLView 作为子层。原生 NSView 的 Z-order 永远高于
    //   同一 NSWindow 内的所有 JUCE 组件绘制（后者只是 NSView 内的
    //   CoreGraphics 合成层）。即使启用 setComponentPaintingEnabled(true)，
    //   JUCE 层的 paintContent 依然会被 GL 帧覆盖，控制栏永远不可见。
    //   唯一可靠的解法：将 OverlayView 提升为独立 NSWindow（addToDesktop），
    //   并通过 addChildWindow:ordered:NSWindowAbove 绑定为主窗口的子窗口。
    //   操作系统的窗口合成器保证子窗口 Z-order 高于父窗口内嵌的任何子
    //   NSView（含 NSOpenGLView），同时子窗口跟随父窗口层级，不会盖住
    //   其他应用的窗口。
    //
    //   鼠标事件走位：
    //   OverlayView 设 setInterceptsMouseClicks(false, false)，鼠标事件
    //   透传到下方，由 MilkdropModule 的 mouseDown/Move/Up 处理。
    //   MilkdropModule 的 hit-test 走 JUCE 组件树，不受 native Z-order 影响。
    // ------------------------------------------------------
    class OverlayView : public juce::Component
    {
    public:
        explicit OverlayView(MilkdropModule& owner) : owner_(owner)
        {
            setInterceptsMouseClicks(false, false);
            setOpaque(false);
        }
        void paint(juce::Graphics& g) override;
    private:
        MilkdropModule& owner_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OverlayView)
    };
#endif

    // === 私有成员 ===
    AnalyserHub* hub;                       ///< 可为 nullptr（无音频源）
    bool         hubRetained = false;       ///< 标记是否成功 retain（析构时 release）
    Y2KmeterAudioProcessorEditor* editor_;  ///< Editor 引用，脱离浮动窗口后仍可用于桥接 projectM
    std::unique_ptr<GLView> glView;
#if JUCE_MAC
    std::unique_ptr<OverlayView> overlayView_;  ///< macOS：顶层悬浮窗口，绘制控制栏
    /// 将 overlayView_ 的屏幕位置同步到 MilkdropModule 内容区顶部 26px 处；
    /// 若 !focused_ 或 !isShowing() 则隐藏。UI 线程调用（每帧、聚焦切换、resize 时）。
    void UpdateOverlayViewPlacement();
#endif

    // 从布局存档恢复或浮动 renderer 同步来的预设索引（-1 = 无存档，首次启动）。
    // 作为持久化快照缓存使用；saveModuleSpecificState() 为 const，但需要在保存前
    // 从浮动态 GLView 同步最新索引，因此该字段允许在 const 上下文更新。
    mutable int  restored_preset_index_ = -1;

    // ---- 焦点与叠加层控件 ----
    bool focused_ { false };

    // 叠加层按钮类型
    enum class OverlayButton { kNone, kPrev, kNext, kRandom, kPresetName, kAuto, kRenderScale, kColor, kEffects };
    OverlayButton hoveredOverlayBtn_ { OverlayButton::kNone };
    OverlayButton pressedOverlayBtn_ { OverlayButton::kNone };

    // 缓存 nameArea 矩形，供 mouseDown/Up/Move 中做 hit test
    juce::Rectangle<int> cachedNameArea_;

    // 辅助
    juce::Rectangle<int> getOverlayBounds(juce::Rectangle<int> content) const;
    OverlayButton hitTestOverlayButton(juce::Point<int> pos, juce::Rectangle<int> overlay) const;
    juce::Rectangle<int> getOverlayButtonRect(juce::Rectangle<int> overlay, OverlayButton btn) const;
    void executeOverlayAction(OverlayButton btn);
    void paintOverlayControlBar(juce::Graphics& g, juce::Rectangle<int> content);
    void PaintLoadingIndicator(juce::Graphics& g, juce::Rectangle<int> content);
    void showPresetJumpDialog();

    // ---- 自动轮播控制行 ----
    void showAutoIntervalDialog();  ///< 弹出自动轮播间隔输入对话框
    void paintAutoControlRow(juce::Graphics& g, juce::Rectangle<int> topBar);
    juce::Rectangle<int> getAutoRowBounds(juce::Rectangle<int> topBar) const;
    juce::Rectangle<int> getSliderBounds(juce::Rectangle<int> autoRow) const;
    void updateAutoIntervalFromSlider(float proportion);
    void applyAutoInterval(float seconds);

    // ---- 整体染色控制（color 按钮 + RGB/Bright 控制器）----
    void toggleColorPanel();                                            ///< 展开/收起染色控制器
    void paintColorPanel(juce::Graphics& g, juce::Rectangle<int> topBar);
    juce::Rectangle<int> getColorPanelBounds(juce::Rectangle<int> topBar) const;
    juce::Rectangle<int> getTintSliderBounds(juce::Rectangle<int> panel, int row) const;
    juce::Rectangle<int> getTintResetBounds(juce::Rectangle<int> panel) const;
    void syncVisualFromEditor();                                        ///< 从 Editor 全局视觉状态读回本地缓存
    void applyVisualToEditor();                                         ///< 写回 Editor 全局视觉状态
    void updateTintFromSlider(int row, float proportion);               ///< 0=R,1=G,2=B,3=Bright

    // Bright 滑块非线性映射：使中性点 brightness=1.0 位于滑块正中间（比例 0.5），
    // 左端 0.0、右端 8.0，靠近两端时变化率放缓，便于精细调节暗部与强发光区域。
    static float SliderProportionToBrightness(float proportion);        ///< 滑块比例(0~1) → bright 增益(0~8)
    static float BrightnessToSliderProportion(float brightness);        ///< bright 增益(0~8) → 滑块比例(0~1)

    // ---- 效果控制（effects 按钮 + invert/shadows 开关）----
    void toggleEffectsPanel();                                          ///< 展开/收起效果控制器
    void paintEffectsPanel(juce::Graphics& g, juce::Rectangle<int> topBar);
    juce::Rectangle<int> getEffectsPanelBounds(juce::Rectangle<int> topBar) const;
    juce::Rectangle<int> getEffectsToggleBounds(juce::Rectangle<int> panel, int row) const;
    juce::Rectangle<int> getEffectsResetBounds(juce::Rectangle<int> panel) const;
    void toggleEffectSwitch(int row);                                   ///< 按 row 切换对应效果开关
    int getEffectsColumns(juce::Rectangle<int> panel) const;            ///< 根据面板宽度计算每行按钮数
    int getEffectsRowCount(juce::Rectangle<int> panel) const;           ///< 根据列数计算总行数

    // Auto-hide 逻辑（由 GLView::timerCallback 在 UI 线程驱动，30Hz 轮询）：
    //   · 检测 !hasKeyboardFocus → 窗口失焦即隐藏
    //   · 检测 idle > 4s → 长时间不操作 overlay 自动隐藏
    //   · mouseMove/mouseDown 在 overlay 区域交互时通过 touchOverlayIdleTimer() 刷新

    // overlay 最后一次交互时间（getMillisecondCounter），用于 idle 超时检测
    juce::uint32 lastInteractionTime_ { 0 };

    // ---- 自动轮播模式 ----
    bool isAutoMode_ { false };
    float autoIntervalSeconds_ { 10.0f };
    juce::uint32 lastAutoSwitchTime_ { 0 };
    bool isDraggingSlider_ { false };
    juce::Rectangle<int> cachedAutoTimeLabel_;  ///< 缓存 auto 行时间标签区域，供 hit-test 使用

    static constexpr float kAutoRowHeight = 28.0f;
    static constexpr int   kAutoBtnW = 32;
    static constexpr int   kResBtnW  = 32;  // [1:n] 渲染缩放按钮
    static constexpr float kMinAutoInterval = 1.0f;
    static constexpr float kMaxAutoInterval = 60.0f;

    // ---- 整体视觉状态（master output colors + effects）----
    MilkdropVisualState visualState_;          ///< 本地缓存：完整视觉状态
    bool isColorPanelOpen_ { false };          ///< 染色控制器是否展开
    bool isEffectsPanelOpen_ { false };        ///< 效果控制器是否展开
    int  draggingTintRow_ { -1 };              ///< 正在拖动的滑块行（0=R,1=G,2=B,3=Bright；-1=无）
    juce::Rectangle<int> cachedColorPanelRect_;  ///< 缓存染色面板区域，供 hit-test
    juce::Rectangle<int> cachedTintResetRect_;   ///< 缓存染色 Reset 按钮区域，供 hit-test
    juce::Rectangle<int> cachedEffectsPanelRect_; ///< 缓存效果面板区域，供 hit-test
    juce::Rectangle<int> cachedEffectsResetRect_; ///< 缓存效果 Reset 按钮区域，供 hit-test
    static constexpr float kColorPanelHeight = 104.0f; ///< 染色控制器面板高度（标题 + 4 行）
    static constexpr float kEffectsHeaderH = 22.0f;    ///< effects 面板标题高度
    static constexpr float kEffectsRowH = 22.0f;       ///< effects 面板每行开关高度
    static constexpr float kEffectsPadBottom = 6.0f;   ///< effects 面板底部 padding
    static constexpr float kEffectsToggleMinW = 56.0f; ///< 每个效果按钮最小宽度（不足则换行）
    static constexpr float kEffectsToggleGap = 4.0f;   ///< 效果按钮水平/垂直间距
    static constexpr int   kColorBtnW = 32;             ///< color 按钮宽度（与 auto 一致）
    static constexpr int   kEffectsBtnW = 42;           ///< effects 按钮宽度
    static constexpr float kTintMin = 0.0f;             ///< RGB 加性偏移下限（0%）
    static constexpr float kTintMax = 2.0f;             ///< RGB 加性偏移上限（200%）
    static constexpr float kBrightMin = 0.0f;           ///< bright 增益下限
    static constexpr float kBrightMax = 8.0f;           ///< bright 增益上限（对齐 MilkDrop3 的 1~8）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MilkdropModule)
};