/*
  ==============================================================================

  Milkdrop3Window.h
  Y2Kmeter — MilkDrop3 独立弹窗（自 v2.5.0 起）。

  架构
  --------------------------------------------------------------------------
  将 MilkDrop3 从模块系统（ModulePanel / ModuleWorkspace）中抽离为独立
  ResizableWindow，通过 addToDesktop() 成为顶层原生窗口。窗口内部直接管理：
    · D3D9 渲染子 HWND（WS_CHILD，填满客户区）
    · 控件栏 overlay（WS_CHILD，GDI 绘制，悬浮于顶部）
    · 音频注入、预设控制、分步异步初始化

  坐标空间
  --------------------------------------------------------------------------
  进程为 PMv2。窗口客户区坐标经由 getScreenPosition() + logicalToPhysical
  转换后用于 Win32 API 调用（CreateWindowEx / SetWindowPos）。

  启动序列约束
  --------------------------------------------------------------------------
  Milkdrop3Window 必须在 mainWindow->addToDesktop() + setVisible(true) 之后、
  callAsync 中创建和显示，确保不触发 LdrLockLoaderLock 死锁。

  ==============================================================================
*/
#pragma once

#include <JuceHeader.h>
#include <windows.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>

#include "source/analysis/AnalyserHub.h"

namespace milkdrop3_api { class Api; }

class Milkdrop3Window : public juce::ResizableWindow,
                        public AnalyserHub::FrameListener,
                        private juce::Timer
{
public:
  /**
   * @param hub 供拿 PCM/Spectrum。若为 nullptr，窗口以静音模式运行。
   */
  explicit Milkdrop3Window(AnalyserHub* hub);
  ~Milkdrop3Window() override;

  // === ResizableWindow 覆写 ===
  void userTriedToCloseWindow() override;
  void resized() override;

  // === juce::Timer（分阶段异步初始化 + 渲染循环）===
  void timerCallback() override;

  // === AnalyserHub::FrameListener 覆写 ===
  void onFrame(const AnalyserHub::FrameSnapshot& frame) override;

  // === 用户交互 API ===
  void NextPreset();
  void PrevPreset();
  void RandomPreset();
  void ToggleAutoMode();
  bool IsAutoModeActive() const noexcept { return is_auto_mode_; }

  // === 置顶 ===
  void ToggleAlwaysOnTop();
  bool IsAlwaysOnTop() const noexcept { return always_on_top_; }

private:
  // ---- 布局常量 ----
  static constexpr int kControlBarHeight = 26;
  static constexpr int kDefaultWidth     = 640;
  static constexpr int kDefaultHeight    = 480;
  static constexpr int kMinWidth         = 160;
  static constexpr int kMinHeight        = 120;

  // ---- 控件栏按钮类型（比 Milkdrop3Module 多一个 kPin）----
  enum class OverlayButton {
    kNone, kPin, kPrev, kRandom, kNext, kAuto, kPresetName
  };

  // ---- 内部 HWND 管理 ----

  /** D3D9 渲染子 HWND（WS_CHILD，填满客户区）。 */
  struct D3dChildHwnd {
    bool  Create(HWND parent, int w, int h);
    void  Destroy();
    void  Resize(int w, int h);
    HWND  hwnd() const { return hwnd_; }

    HWND  hwnd_ = nullptr;
  };

  /** 控件栏 overlay（WS_CHILD，GDI 绘制，悬浮于顶部）。 */
  class ControlBarOverlay;
  std::unique_ptr<ControlBarOverlay> overlay_;

  // ---- 引擎 API 单例引用 ----
  milkdrop3_api::Api& api_;

  // ---- 音频源 ----
  AnalyserHub* hub_ = nullptr;

  // ---- pre-render injector 令牌 ----
  size_t audio_injector_token_ = 0;

  // ---- 状态 ----
  bool          initialized_  = false;
  bool          error_state_  = false;
  juce::String  error_message_;

  // ---- 音频数据快照（onFrame 写入 / RenderFrame 读取，跨线程安全）----
  struct AudioSnapshot {
    static constexpr int kOscSize =
        static_cast<int>(AnalyserHub::oscilloscopeBufferSize);   // 2048
    static constexpr int kSpectrumSize =
        static_cast<int>(AnalyserHub::spectrumMagSize);          // 1024

    std::array<float, kOscSize * 2>  interleaved{};
    std::array<float, kSpectrumSize> specL{};
    std::array<float, kSpectrumSize> specR{};
    float sample_rate  = 48000.0f;
    bool  has_pcm      = false;
    bool  has_spectrum = false;
  };
  std::mutex     audio_mutex_;
  AudioSnapshot  audio_snapshot_;

  // ---- 分阶段异步初始化状态 ----
  int          init_phase_   = -1;
  juce::String status_message_;

  // ---- 布局缓存 ----
  int last_w_ = 0;
  int last_h_ = 0;

  // ---- 置顶状态 ----
  bool always_on_top_ = false;

  // ---- D3D9 子 HWND ----
  D3dChildHwnd d3d_child_;

  // ---- 预设名称变更检测 ----
  std::wstring last_announced_name_;

  // ---- 控件栏交互状态 ----
  OverlayButton hovered_btn_ = OverlayButton::kNone;
  OverlayButton pressed_btn_ = OverlayButton::kNone;

  // ---- 自动轮播 ----
  bool         is_auto_mode_         = false;
  float        auto_interval_secs_   = 10.0f;
  juce::uint32 last_auto_switch_ms_  = 0;

  static constexpr float kMinAutoInterval = 3.0f;
  static constexpr float kMaxAutoInterval = 120.0f;

  void CheckAutoMode();
  void UpdateAutoIntervalFromSlider(float proportion);

  // ---- 内部工具 ----
  void FeedEngineFromSnapshot();
  void AnnouncePresetNameToEngine();
  void SyncOverlayContent();
  void ShowPresetJumpDialog();
  void CloseJumpDialog();
  juce::String GetPresetDisplayName() const;

  // ---- 控件栏交互 ----
  OverlayButton HitTestOverlayBtn(int px, int py) const;
  void          ExecuteOverlayAction(OverlayButton btn);

  // ---- 预设跳转弹窗 ----
  static constexpr int kJumpDlgHeight = 80;
  bool jump_dialog_open_ = false;

  // ---- 键盘 / 鼠标 ----
  bool keyPressed(const juce::KeyPress& key) override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Milkdrop3Window)
};
