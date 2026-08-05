/*
  ==============================================================================

  Milkdrop3Module.h
  Y2Kmeter — MilkDrop3 独立 D3D9 可视化模块（自 v2.4.0 起）。

  架构
  --------------------------------------------------------------------------
  D3D9 popup（WS_POPUP owned by JUCE root）始终占据整个模块内容区，
  不再因控件栏显隐而改变尺寸。控件栏以独立 HWND overlay（GDI 渲染）
  悬浮于 D3D9 popup 之上，聚焦时显示、失焦时隐藏，避免 D3D9 设备重置闪烁。

    ┌─ Milkdrop3Module (ModulePanel) ─────────────────────────────┐
    │  JUCE 标题栏："MilkDrop3"                          [×]       │
    ├─────────────────────────────────────────────────────────── ┤
    │  ┌─ Overlay HWND（聚焦可见，GDI 渲染，位于 D3D9 popup 之上）─┐ │
    │  │ [<]  nameArea  [A] [>] [?]                              │ │
    │  └─────────────────────────────────────────────────────────┘ │
    │  ┌────────────────────────────────────────────────────────┐ │
    │  │ D3D9 popup (WS_POPUP, 固定全尺寸，不再因焦点而 resize)  │ │
    │  │  ├─ D3D9 Device / BackBuffer                          │ │
    │  │  └─ Milkdrop3Api::RenderFrame() → Present             │ │
    │  └────────────────────────────────────────────────────────┘ │
    └──────────────────────────────────────────────────────────────┘

  交互逻辑（与 MilkdropModule 统一）
  --------------------------------------------------------------------------
  · 控件栏 overlay 悬浮于 D3D9 popup 之上，聚焦时显示、非聚焦时隐藏。
  · 鼠标进入模块内容区 / 点击 D3D9 popup 时自动聚焦；4 秒无交互自动隐藏。
  · 预设名以 "3/100 presetName" 格式显示在 overlay 内（不再依赖 D3D9 引擎 overlay）。
  · overlay 内原生 EDIT 控件处理预设跳转输入（不再依赖 JUCE TextEditor）。
  · overlay 不挤占 D3D9 布局空间，视频窗尺寸始终保持固定，无界面刷新频闪。

  数据流（引擎所需数据全部经 pre-render injector 通道，方便扩展）
  --------------------------------------------------------------------------
    AnalyserHub::FrameListener::onFrame()
      → cache latest PCM + spectrum snapshot (thread-safe)
    Timer render tick (~30 fps)
      → api_.RenderFrame()
        → injector: FeedPcm(interleaved LR)       → m_sound.fWaveform
        → injector: FeedSpectrum(magL, magR, ...) → m_sound.fSpectrum
        → CPlugin::PluginRender()
        → D3D9 Present → popup

  ==============================================================================
*/
#pragma once

#include <JuceHeader.h>
#include <windows.h>

#include <array>
#include <memory>
#include <mutex>

#include "source/ui/ModuleWorkspace.h"
#include "source/analysis/AnalyserHub.h"

namespace milkdrop3_api { class Api; }

class Milkdrop3Module : public ModulePanel,
                        public AnalyserHub::FrameListener,
                        private juce::Timer
{
public:
  /**
   * @param hub 供拿 PCM/Spectrum。若为 nullptr，模块以静音模式运行。
   */
  explicit Milkdrop3Module(AnalyserHub* hub);
  ~Milkdrop3Module() override;

  // === ModulePanel 覆写 ===
  void paint(juce::Graphics& g) override;
  void paintContent(juce::Graphics& g,
                    juce::Rectangle<int> contentBounds) override;
  void layoutContent(juce::Rectangle<int> contentBounds) override;

  // === juce::Timer （分阶段异步初始化 + 渲染循环）===
  void timerCallback() override;

  // === AnalyserHub::FrameListener 覆写 ===
  void onFrame(const AnalyserHub::FrameSnapshot& frame) override;

  // === 用户交互 API ===
  void NextPreset();
  void PrevPreset();
  void RandomPreset();
  void ToggleAutoMode();      ///< 切换自动轮播开关

  bool IsAutoModeActive() const noexcept { return is_auto_mode_; }

  // === 焦点与叠加层交互（与 MilkdropModule 统一）===
  void SetFocusVisual(bool shouldFocus);

  // === 键盘交互 ===
  bool keyPressed(const juce::KeyPress& key) override;

  // === 鼠标交互 ===
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;

private:
  // ---- 布局常量 ----
  static constexpr int kControlBarHeight = 26;

  // ---- 控件栏按钮类型 ----
  enum class OverlayButton {
    kNone, kPrev, kRandom, kNext, kAuto, kPresetName
  };

  // ---- D3D9 popup 子窗口管理 ----
  class D3dChildWindow;
  std::unique_ptr<D3dChildWindow> d3d_window_;

  // ---- 控件栏覆盖层（独立 HWND，悬浮于 D3D9 popup 之上）----
  class ControlBarOverlay;
  std::unique_ptr<ControlBarOverlay> overlay_;

  // ---- 引擎 API 单例引用 ----
  milkdrop3_api::Api& api_;

  // ---- 音频源 ----
  AnalyserHub* hub_ = nullptr;

  // ---- pre-render injector 令牌（由 Api 分配，用于在 dtor 中反注册）----
  size_t audio_injector_token_ = 0;

  // ---- 状态 ----
  bool         initialized_    = false;
  bool         error_state_    = false;
  juce::String error_message_;

  // ---- 音频数据快照（onFrame 写入 / RenderFrame 读取，跨线程安全）----
  struct AudioSnapshot {
    static constexpr int kOscSize      = static_cast<int>(
        AnalyserHub::oscilloscopeBufferSize);        // 2048
    static constexpr int kSpectrumSize = static_cast<int>(
        AnalyserHub::spectrumMagSize);               // 1024

    std::array<float, kOscSize * 2>      interleaved{};   // LRLR
    std::array<float, kSpectrumSize>     specL{};
    std::array<float, kSpectrumSize>     specR{};
    float sample_rate = 48000.0f;
    bool  has_pcm      = false;
    bool  has_spectrum = false;
  };
  std::mutex     audio_mutex_;
  AudioSnapshot  audio_snapshot_;

  // ---- 分阶段异步初始化状态 ----
  int  init_phase_       = -1;
  HWND init_parent_hwnd_ = nullptr;
  int  init_width_       = 0;
  int  init_height_      = 0;
  int  init_phys_w_      = 0;
  int  init_phys_h_      = 0;
  juce::String status_message_;

  // ---- 布局缓存 ----
  int last_content_x_ = 0;
  int last_content_y_ = 0;
  int last_content_w_ = 0;
  int last_content_h_ = 0;
  int last_screen_x_  = 0;
  int last_screen_y_  = 0;

  // ---- 首次布局记录 ----
  int init_x_ = 0;
  int init_y_ = 0;

  // ---- 焦点与显隐（与 MilkdropModule 统一）----
  bool         focused_               = true;

  // ---- 预设名称变更检测（避免异步加载中读取旧名称）----
  std::wstring last_announced_name_;

  // ---- 控件栏交互状态 ----
  OverlayButton hovered_btn_ = OverlayButton::kNone;
  OverlayButton pressed_btn_ = OverlayButton::kNone;
  juce::Rectangle<int> cached_name_area_;  ///< 缓存预设名区域供 hit-test

  // ---- 自动轮播 ----
  bool          is_auto_mode_        = false;
  float         auto_interval_secs_  = 10.0f;
  juce::uint32  last_auto_switch_ms_ = 0;

  static constexpr float kMinAutoInterval = 3.0f;
  static constexpr float kMaxAutoInterval = 120.0f;

  void CheckAutoMode();                            ///< 每帧 tick 中调用
  void UpdateAutoIntervalFromSlider(float proportion);

  // ---- 控件栏布局 / 绘制 ----
  juce::Rectangle<int> GetControlBarRect() const;
  juce::Rectangle<int> GetControlBarBtnRect(OverlayButton btn) const;
  OverlayButton        HitTestControlBarBtn(juce::Point<int> pos) const;
  void                 PaintControlBar(juce::Graphics& g);
  void                 ExecuteOverlayAction(OverlayButton btn);

  // ---- 内部工具 ----
  void FeedEngineFromSnapshot();  // 由 Api pre-render injector 调用
  void AnnouncePresetNameToEngine();
  juce::String GetPresetDisplayName() const;
  void SyncOverlayContent();      ///< 同步预设名/自动轮播状态到 overlay
  void ShowPresetJumpDialog();    ///< 打开预设跳转弹窗（Enter=Go, Esc/点击=关闭）
  void CloseJumpDialog();         ///< 关闭跳转 TextEditor

  // ---- 控件栏内嵌预设跳转（非模态，Milkdrop3Module 内部绘制与管理）----
  //   弹窗打开时 overlay 向下扩展 kJumpDlgHeight px，不再通过 D3D9 popup 位移实现。
  //   overlay 内嵌原生 EDIT 控件接收输入（不再依赖 JUCE TextEditor）。
  //   Enter / 点击 Go → 跳转；Esc / 点击 Cancel / 点击别处 → 关闭。
  static constexpr int kJumpDlgHeight = 80;
  bool                          jump_dialog_open_   = false;
  void PaintJumpDialog(juce::Graphics& g, juce::Rectangle<int> bar);
  juce::Rectangle<int> GetJumpDlgGoRect() const;
  juce::Rectangle<int> GetJumpDlgCancelRect() const;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Milkdrop3Module)
};