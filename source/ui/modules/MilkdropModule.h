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

        // 诊断
        bool IsRenderReady() const;
        juce::String GetError() const;
        int  GetCurrentPresetIndex() const;
        void SyncOwnerPresetIndexFromRenderer() const;
        int  GetTotalPresetCount() const;
        juce::String GetCurrentPresetName() const;
        int64_t GetLastPresetSwitchTimeMs() const;

    private:
        void UpdateOpenGLAttachment();
        void DetachOpenGL();
        void ScanPresetFiles();
        void LoadCurrentPreset();
        void ConsumePresetRequests();
        void ConsumePcm();

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

    // === 私有成员 ===
    AnalyserHub* hub;                       ///< 可为 nullptr（无音频源）
    bool         hubRetained = false;       ///< 标记是否成功 retain（析构时 release）
    Y2KmeterAudioProcessorEditor* editor_;  ///< Editor 引用，脱离浮动窗口后仍可用于桥接 projectM
    std::unique_ptr<GLView> glView;

    // 从布局存档恢复或浮动 renderer 同步来的预设索引（-1 = 无存档，首次启动）。
    // 作为持久化快照缓存使用；saveModuleSpecificState() 为 const，但需要在保存前
    // 从浮动态 GLView 同步最新索引，因此该字段允许在 const 上下文更新。
    mutable int  restored_preset_index_ = -1;

    // ---- 焦点与叠加层控件 ----
    bool focused_ { false };

    // 叠加层按钮类型
    enum class OverlayButton { kNone, kPrev, kNext, kRandom, kPresetName, kAuto, kRenderScale };
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MilkdropModule)
};