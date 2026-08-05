/*
  ==============================================================================

  Milkdrop3Api.h
  Y2Kmeter — MilkDrop3 独立 D3D9 可视化引擎封装（自 v2.4.0 起）。

  职责
  --------------------------------------------------------------------------
  · 加载 d3d9.dll，创建 D3D9 设备与 CPlugin 引擎实例
  · 提供 5-phase 分步初始化接口，供 UI 侧显示加载进度
  · 提供 FeedPcm / FeedSpectrum 两条音频注入通道
  · 提供 pre-render injector 扩展点：宿主可随时追加/替换每帧要投喂的数据
  · 预设切换与预设名 overlay（直接使用引擎自带的 D3D9 绘制能力，
    避免被独立 D3D9 popup 遮挡问题）

  数据流
  --------------------------------------------------------------------------
    Y2Kmeter AnalyserHub ─┬─ FeedPcm(interleaved LR)  → m_sound.fWaveform
                          └─ FeedSpectrum(magL, magR) → m_sound.fSpectrum
                                                        (通过引擎的
                                                         m_bY2kExternalSpectrumValid
                                                         开关短路内建 FFT 路径)

  预设名显示（引擎内 D3D9 绘制，天然不会被 popup 遮挡）
  --------------------------------------------------------------------------
  · EnablePresetInfoOverlay(true) → 引擎每帧在 D3D9 surface 右上角写出
    当前预设文件名（m_bShowPresetInfo）。
  · ShowPresetTitleAnim(name) → 触发引擎的 song-title 弹出动画，切预设
    后播放几秒中心大字，不阻塞渲染。
  ==============================================================================
*/
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// 前向声明 MilkDrop3 引擎类（避免在外部头文件中暴露完整 CPlugin 定义）
class CPlugin;

namespace milkdrop3_api
{

/**
 * @brief 单例封装：D3D9 设备生命周期、CPlugin 引擎驱动、
 *        音频（PCM + Spectrum）注入、预设切换与 overlay。
 *
 * 线程安全：分步初始化 / Destroy / RenderFrame 必须在同一线程调用。
 * FeedPcm / FeedSpectrum 可跨线程调用（内部有锁保护）。
 */
class Api
{
public:
  static Api& Instance();

  Api(const Api&) = delete;
  Api& operator=(const Api&) = delete;

  /** 引擎是否已成功初始化且就绪可渲染。 */
  bool IsReady() const noexcept { return ready_; }

  /** 初始化失败时返回诊断字符串。 */
  const std::string& GetError() const noexcept { return error_message_; }

  // ---- 生命周期 ---------------------------------------------------------

  /** 销毁引擎与 D3D9 设备。必须与初始化在同一线程调用。 */
  void Destroy();

  // ---- 分步初始化 —— 由 Milkdrop3Module 通过 juce::Timer 逐步驱动 ------
  //   phase 1: Initialize_CreateRenderWindow(parent_hwnd, w, h)
  //   phase 2: Initialize_SetPaths()
  //   phase 3: Initialize_PreInit()
  //   phase 4: Initialize_CreateDevice(w, h)
  //   phase 5: Initialize_PluginInit(w, h)
  // ---------------------------------------------------------------------

  bool Initialize_CreateRenderWindow(HWND parent_hwnd, int width, int height);
  bool Initialize_SetPaths();
  bool Initialize_PreInit();
  bool Initialize_CreateDevice(int width, int height);
  bool Initialize_PluginInit(int engine_width, int engine_height);

  // ---- 窗口管理 ---------------------------------------------------------

  /** 通知引擎渲染分辨率发生变化（触发 D3D9 Device Reset）。 */
  void OnResize(int width, int height);

  /** 返回当前渲染目标 HWND。 */
  HWND GetRenderWindow() const noexcept { return render_hwnd_; }

  // ---- 每帧渲染 ---------------------------------------------------------

  /**
   * @brief 渲染一帧。
   *
   * 内部会先依次执行所有已注册的 pre-render injector（见 AddPreRenderInjector），
   * 再调用引擎 PluginRender()。
   */
  void RenderFrame();

  // ---- 音频注入 ---------------------------------------------------------

  /**
   * @brief 注入立体声 PCM（波形，供 m_sound.fWaveform 与波形可视化使用）。
   *
   * @param interleaved_lr LRLR 交错 float，范围 [-1, +1]
   * @param frame_count    帧数（interleaved_lr 长度 = 2 * frame_count）
   */
  void FeedPcm(const float* interleaved_lr, unsigned int frame_count);

  /**
   * @brief 注入外部预计算频谱（一次性覆盖 m_sound.fSpectrum）。
   *
   * 引擎侧 AnalyzeNewSound 消费一次即清空开关，因此宿主需每帧持续投喂。
   *
   * @param magL/magR     两声道线性 magnitude（长度 = num_bins）
   * @param num_bins      源频谱 bin 数
   * @param sample_rate   源频谱对应采样率（Hz）。用于把源 bin 频率轴
   *                      重采样映射到 MilkDrop3 的 0~11025 Hz × NUM_FREQUENCIES。
   */
  void FeedSpectrum(const float* magL, const float* magR,
                    unsigned int num_bins, float sample_rate);

  /**
   * @brief 追加一个 pre-render injector。
   *
   * 每次 RenderFrame() 开始前会依次调用所有已注册 injector，
   * 便于宿主集中投喂音频/分析/UI 事件等数据（支持后续扩展新的数据源）。
   * 返回一个非零 token，可用于后续 RemovePreRenderInjector。
   */
  using Injector = std::function<void()>;
  size_t AddPreRenderInjector(Injector injector);
  void   RemovePreRenderInjector(size_t token);

  // ---- 预设控制 ---------------------------------------------------------

  void LoadPreset(const wchar_t* filename, float blend_time);
  void NextPreset(float blend_time);
  void PrevPreset(float blend_time);
  void RandomPreset(float blend_time);

  /** 按文件级索引跳转到指定预设（0-based，仅计 .milk 文件，不含目录项）。
   *  内部自动映射 fileIndex → m_presets 数组下标 (fileIndex + m_nDirs)。 */
  void JumpToPreset(int fileIndex);

  // ---- 预设名 overlay（引擎侧 D3D9 绘制）--------------------------------

  /** 开启/关闭右上角常显的 "current preset filename" 文本。 */
  void EnablePresetInfoOverlay(bool on);

  /** 触发中央 song-title 弹出动画（切预设瞬间的大字提示）。 */
  void ShowPresetTitleAnim(const wchar_t* text);

  /** 禁用引擎内置的预设自动轮播（设置 m_bPresetLockedByUser = true）。
   *  之后由 Y2Kmeter 侧 Milkdrop3Module 接管轮播逻辑。 */
  void DisableAutoAdvance();

  // ---- 诊断 -------------------------------------------------------------

  /** 当前预设的文件级索引（0-based，仅计 .milk 文件，不含目录项）。
   *  等价于 m_nCurrentPreset - m_nDirs。 */
  int          GetCurrentPresetIndex() const noexcept;

  /** 预设数组总条目数（含目录 + 文件）。保留用于向后兼容，
   *  新代码如需获取可加载的 .milk 文件数量请使用 GetFileablePresetCount()。 */
  int          GetTotalPresets()       const noexcept;

  /** 实际可加载的 .milk 预设文件数量（不含目录项）。
   *  等价于 m_nPresets - m_nDirs。UI 编号范围应基于此值。 */
  int          GetFileablePresetCount() const noexcept;

  std::wstring GetCurrentPresetName()  const;

private:
  Api() = default;
  ~Api();

  bool CreateD3d9Device(int width, int height);

  /** 将 float PCM [-1, +1] 转换为 MilkDrop3 的 unsigned 8-bit PCM（centered at 128）。 */
  void ConvertPcmToMd3(const float* interleaved_lr,
                       unsigned int frame_count,
                       unsigned char* out_l,
                       unsigned char* out_r);

  /** 在渲染前依次调用所有注册的 pre-render injector（无锁复制到本地后调用）。 */
  void RunPreRenderInjectors();

  // ---- 分步初始化中间状态 ----------------------------------------------
  HWND    init_parent_hwnd_           = nullptr;
  int     init_window_width_          = 0;
  int     init_window_height_         = 0;
  wchar_t init_exe_dir_[MAX_PATH]     = {};
  wchar_t init_appdata_dir_[MAX_PATH] = {};

  // ---- D3D9 资源 --------------------------------------------------------
  HMODULE               d3d9_dll_    = nullptr;
  IDirect3D9*           d3d9_        = nullptr;
  IDirect3DDevice9*     device_      = nullptr;
  D3DPRESENT_PARAMETERS d3dpp_       = {};
  HWND                  render_hwnd_ = nullptr;

  // ---- 引擎实例 ---------------------------------------------------------
  CPlugin*              plugin_      = nullptr;

  // ---- PCM 缓冲（锁保护，支持跨线程注入 + 渲染消费）--------------------
  std::mutex     pcm_mutex_;
  unsigned char  pcm_left_[576]  = {};
  unsigned char  pcm_right_[576] = {};
  bool           pcm_ready_      = false;

  // ---- pre-render injector 表 ------------------------------------------
  struct InjectorSlot { size_t token; Injector fn; };
  std::mutex                injectors_mutex_;
  std::vector<InjectorSlot> injectors_;
  size_t                    next_injector_token_ = 1;

  // ---- 状态 -------------------------------------------------------------
  bool          ready_         = false;
  int           window_width_  = 0;
  int           window_height_ = 0;
  std::string   error_message_;
};

}  // namespace milkdrop3_api