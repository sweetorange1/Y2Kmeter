#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"

// projectM C API 类型定义（projectm_handle）
#include "projectM-4/types.h"

// 注：types.h 里声明了 struct projectm，不能再开同名 namespace，
// 因此我们的 shim 单例放在 namespace projectm_api 下。
namespace projectm_api { class Api; }

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
     */
    explicit MilkdropModule (AnalyserHub* hub);
    ~MilkdropModule() override;

    // === ModulePanel 覆写 ===
    void paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;
    void layoutContent (juce::Rectangle<int> contentBounds) override;

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
    // GLView：内嵌 OpenGL surface。负责 projectM handle 生命周期与渲染。
    // ------------------------------------------------------
    class GLView : public juce::Component,
                   public juce::OpenGLRenderer,
                   private juce::Timer
    {
    public:
        explicit GLView (MilkdropModule& owner_);
        ~GLView() override;

        // OpenGLRenderer
        void newOpenGLContextCreated() override;
        void renderOpenGL() override;
        void openGLContextClosing() override;

        // Component
        void paint(juce::Graphics&) override {}  // 不画任何东西——内容由 renderOpenGL 提供
        void parentHierarchyChanged() override;
        void visibilityChanged() override;
        void resized() override;

        // GLView 覆盖整个内容区，必须把鼠标事件转发给父组件 MilkdropModule，
        // 否则焦点获取和叠加层按钮交互全部被 GLView 吞掉。
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;

        // 显式 detach —— 在 MilkdropModule 析构最前面调用，
        // 保证 GL 线程完全收尾后再销毁 hub listener 等成员。
        void detachAndWait();

        // 主动请求 GL 线程重绘一帧（下一次 vsync）。GL 线程内部
        // 会自动在下次 buffer swap 中重新走 renderOpenGL()。
        void triggerRepaint();

        // 供 owner 在 UI 线程 push PCM（内部加锁）。
        void pushPcm (const float* interleavedLR, unsigned int frameCount);

        // UI 线程调用；GL 线程会在下次 renderOpenGL 里消费这些请求。
        // 采用原子累加：连点 next 多次，delta 累计到位。
        void requestPresetDelta (int delta) noexcept { requestedPresetDelta.fetch_add (delta); triggerRepaint(); }
        void requestPresetRandom() noexcept { requestedPresetRandom = true; triggerRepaint(); }
        void requestPresetJump (int index) noexcept { requestedPresetJump.store (index); triggerRepaint(); }

        // 诊断接口：供 owner 在 paintContent 中展示错误信息。
        // 当 renderInitialized == false 时，说明 projectM 未能成功创建或已销毁；
        // 此时 CPU 层应展示一段可读的报错文案而非静默黑屏。
        bool isRenderInitialized() const noexcept { return renderInitialized; }
        juce::String getRenderError() const { return juce::String (renderErrorMessage); }

        // 预设索引：GL 线程在 loadPresetInternal 中写入，UI 线程在 paintContent
        // 中读取以显示当前预设名。atomic 保证跨线程安全。
        int getCurrentPresetIndex() const noexcept { return currentPresetIndexUi_.load(); }

        // 预设总数（UI 线程安全只读）。
        int getTotalPresetCount() const noexcept { return presetPaths.size(); }

        // CPU 帧缓存访问器：GL 线程在 renderOpenGL 中通过 glReadPixels 写入，
        // UI 线程在 GLView::paint 中通过 g.drawImage 绘制。调用者负责持锁。
        juce::Image& getCachedGlFrame() { return cachedGlFrame_; }
        std::mutex& getGlFrameMutex() { return glFrameMutex_; }

        // 最后一次预设切换的时间戳（毫秒，steady_clock）。GL 线程写，UI 线程读。
        // 用于在 soft-cut 过渡期间显示"Loading..."提示。
        int64_t getLastPresetSwitchTimeMs() const noexcept { return last_preset_switch_time_ms_.load(); }

        // ---- 分辨率缩放（glReadPixels 降采样） ----
        enum class ReadbackScale { kFull = 1, kHalf = 2, kQuarter = 4 };
        ReadbackScale getReadbackScale() const noexcept { return readback_scale_.load(); }
        void cycleReadbackScale() noexcept
        {
          // 三态循环: 1:1 → 1:2 → 1:4 → 1:1
          auto cur = readback_scale_.load();
          switch (cur)
          {
          case ReadbackScale::kFull:    readback_scale_.store(ReadbackScale::kHalf);    break;
          case ReadbackScale::kHalf:    readback_scale_.store(ReadbackScale::kQuarter); break;
          case ReadbackScale::kQuarter: readback_scale_.store(ReadbackScale::kFull);    break;
          }
        }
        juce::String getReadbackScaleLabel() const noexcept
        {
          switch (readback_scale_.load())
          {
          case ReadbackScale::kFull:    return "1:1";
          case ReadbackScale::kHalf:    return "1:2";
          case ReadbackScale::kQuarter: return "1:4";
          }
          return {};
        }

        // 根据当前预设索引返回用于 UI 显示的预设名（不含路径和 .milk 扩展名）。
        // 可在 UI 线程安全调用。
        juce::String getCurrentPresetName() const
        {
            int idx = currentPresetIndexUi_.load();
            if (idx >= 0 && idx < presetPaths.size())
            {
                return presetPaths[idx].fromLastOccurrenceOf("/", false, false)
                                       .fromLastOccurrenceOf("\\", false, false)
                                       .upToLastOccurrenceOf(".milk", false, false);
            }
            return {};
        }

    private:
        // Timer: UI 线程 30Hz。projectM 帧通过 glReadPixels → cachedGlFrame_
        // → paintContent(g.drawImage) 显示。timer 驱动重绘、auto-hide 与 auto 轮播。
        void timerCallback() override;
        void loadPresetInternal();
        void attachIfNeeded();
        void scheduleAsyncAttach();
        static juce::File findAssetsDir (const juce::String& subdir);

        MilkdropModule& owner;
        juce::OpenGLContext glContext;

        // scheduleAsyncAttach 递归重试上限。
        // 每次 callAsync 间隔 ~16ms（Windows 消息循环），60 次 ≈ 1 秒。
        // 超过上限说明宿主窗口永远不会 visible，放弃并写入 renderErrorMessage。
        static constexpr int kMaxAttachRetries = 60;

        // 递归重试计数器。scheduleAsyncAttach 每调度一次 callAsync 就 +1；
        // callAsync 回调中条件不满足时递归调用 scheduleAsyncAttach 继续累加。
        // 达到 kMaxAttachRetries 后放弃。仅在 UI 线程读写。
        int attachRetries = 0;

        // 析构保护标志：~GLView 开头置 true，scheduleAsyncAttach / callAsync
        // 回调检查此标志立即返回，防止析构期间 post 的 callAsync 与
        // glContext.detach() 形成竞态导致卡死。
        std::atomic<bool> destroying_ { false };

        // 首次焦点激活标志：render 就绪后仅自动激活一次焦点（展示预设名等控件），
        // 之后不再自动激活。避免 auto 模式下 auto-hide 清除焦点后被 timer
        // 立即恢复，造成控制栏闪烁。
        bool first_focus_done_ { false };

        // projectM handle —— 只在 GL 线程访问。
        projectm_handle pmHandle = nullptr;

        // GL 线程使用的最新 PCM 缓冲（LRLR 交错）。UI 线程写、GL 线程读。
        std::mutex               pcmMutex;
        std::vector<float>       pendingPcm;   // 长度 = 2 * frameCount
        unsigned int             pendingFrames = 0;

        // 上一次真实音频的备份（GL 线程独占）。当本帧没有新 PCM 到达时，
        // 用此备份复播以保持 projectM 的频谱活力，避免合成假音频。
        std::vector<float>       lastRealPcm;
        unsigned int             lastRealFrames = 0;
        bool                     hasEverReceivedRealPcm = false;

        // 预设列表（构造 GL 时扫描一次；GL 线程也可读，UI 线程不再修改）
        juce::StringArray        presetPaths;
        int                      currentPresetIndex = -1;

        // GL 线程侧的 preset 切换请求。UI 线程置位，GL 线程消费。
        std::atomic<int>         requestedPresetDelta { 0 };   // -1 / +1 / 0
        std::atomic<bool>        requestedPresetRandom { false };
        std::atomic<int>         requestedPresetJump { -1 };   // -1=无请求, >=0=目标索引

        // 分辨率缩放级别：GL 线程在 glReadPixels 时按此值降采样回读（1/2/4）。
        // UI 线程通过 cycleReadbackScale() 切换，GL 线程在 renderOpenGL 消费。
        std::atomic<ReadbackScale> readback_scale_ { ReadbackScale::kFull };

        // 尺寸变化：UI 线程 setBounds 时置位，GL 线程 renderOpenGL 消费并
        // 调用 projectm_set_window_size —— projectM 的 fbo 会随之重建。
        std::atomic<int>         desiredWidth  { 0 };
        std::atomic<int>         desiredHeight { 0 };
        int                      lastAppliedWidth  = 0;
        int                      lastAppliedHeight = 0;

        // 加载失败诊断（若 projectm.dll 不可用则赋值，由 owner 在 paintContent
        // 中展示给用户）。
        std::atomic<bool>        renderInitialized { false };
        std::string              renderErrorMessage;

        // 当前预设索引的 UI 线程可读镜像。GL 线程在 loadPresetInternal() 中
        // store，UI 线程通过 getCurrentPresetIndex() load，用于显示预设名。
        std::atomic<int>         currentPresetIndexUi_ { -1 };

        // 最后一次预设切换的 steady_clock 毫秒时间戳。
        // GL 线程在 renderOpenGL 消费切换请求时 store；
        // UI 线程通过 getLastPresetSwitchTimeMs() load，
        // 用于在 soft-cut 过渡期间显示"Loading..."提示。
        std::atomic<int64_t>     last_preset_switch_time_ms_ { 0 };

        // 帧缓存：renderOpenGL（GL 线程）通过 glReadPixels 从 GLView 的 FBO 0 回读，
        // paintContent（UI 线程）以 g.drawImage 绘制到界面。mutex 保护。
        juce::Image              cachedGlFrame_;
        std::mutex               glFrameMutex_;

        // setComponentPaintingEnabled(false) 时 GLView 拥有独立原生 HWND 子窗口。
        // 调用此方法将其推到父窗口子控件 Z-order 最底层 + WS_EX_TRANSPARENT，
        // 让 GDI 绘制的其他模块覆盖在 GL HWND 之上。
        void pushNativeWindowToBottom();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GLView)
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
    std::unique_ptr<GLView> glView;

    // 从布局存档恢复的预设索引（-1 = 无存档，首次启动）。在 GL context 创建后
    // newOpenGLContextCreated 中消费，消费后重置为 -1 防止重复覆盖。
    int          restored_preset_index_ = -1;

    // ---- 焦点与叠加层控件 ----
    bool focused_ { false };

    // 叠加层按钮类型
    enum class OverlayButton { kNone, kPrev, kNext, kRandom, kRes, kPresetName, kAuto };
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
    void ensureAutoIntervalEditor();  ///< 延迟创建 TextEditor（首次进入 auto 模式时调用）
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
    std::unique_ptr<juce::TextEditor> autoIntervalEditor_;
    bool isDraggingSlider_ { false };

    static constexpr float kAutoRowHeight = 28.0f;
    static constexpr int   kAutoBtnW = 32;
    static constexpr float kMinAutoInterval = 1.0f;
    static constexpr float kMaxAutoInterval = 60.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MilkdropModule)
};