/*
  ==============================================================================

  Milkdrop3Api.cpp
  Y2Kmeter — MilkDrop3 独立 D3D9 可视化引擎封装实现。

  ==============================================================================
*/

#include "Milkdrop3Api.h"

#include "Md3DebugLog.h"

// MilkDrop3 引擎头文件
#include "plugin.h"       // CPlugin
#include "pluginshell.h"  // CPluginShell
#include "audiobuf.h"     // SetAudioBuf

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shlobj.h>        // SHGetFolderPathW

// MilkDrop3 引擎全局实例 —— menu.cpp 通过 extern CPlugin g_plugin 引用。
CPlugin g_plugin;

// 原始 Winamp 插件 HINSTANCE —— utility.cpp 通过 extern 引用。
HINSTANCE api_orig_hinstance = nullptr;

// C locale 指针 —— utility.cpp / state.cpp / plugin.cpp 通过 extern 引用。
_locale_t g_use_C_locale;

// 桌面图标快捷键映射 —— plugin.cpp 通过 extern 引用。
char keyMappings[8];

namespace milkdrop3_api
{

// ==========================================================================
// 单例
// ==========================================================================

Api& Api::Instance() {
  static Api instance;
  return instance;
}

Api::~Api() {
  Destroy();
}

// ==========================================================================
// 分步初始化
// ==========================================================================

bool Api::Initialize_CreateRenderWindow(HWND parent_hwnd, int width, int height) {
  init_parent_hwnd_   = parent_hwnd;
  init_window_width_  = width;
  init_window_height_ = height;

  // Milkdrop3Module 已经为 D3D9 Present 目标准备好了单层 WS_POPUP HWND，
  // 这里直接把它当作 render target 使用，避免再嵌套一层 WS_CHILD。
  render_hwnd_ = parent_hwnd;

  // 使用全局 CPlugin 实例（menu.cpp extern 引用）
  plugin_ = &g_plugin;
  return true;
}

bool Api::Initialize_SetPaths() {
  if (!plugin_) {
    error_message_ = "Initialize_SetPaths: plugin_ is null "
                     "(call Initialize_CreateRenderWindow first).";
    return false;
  }

  wchar_t exe_dir[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
  wchar_t* last_slash = wcsrchr(exe_dir, L'\\');
  if (last_slash) *last_slash = L'\0';
  wcscpy_s(init_exe_dir_, exe_dir);

  wchar_t appdata_dir[MAX_PATH] = {0};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata_dir))) {
    wcscat_s(appdata_dir, L"\\Y2Kmeter");
    CreateDirectoryW(appdata_dir, nullptr);
  }
  wcscpy_s(init_appdata_dir_, appdata_dir);

  plugin_->SetY2KPaths(exe_dir, appdata_dir);
  return true;
}

bool Api::Initialize_PreInit() {
  if (!plugin_) {
    error_message_ = "Initialize_PreInit: plugin_ is null.";
    return false;
  }

  HINSTANCE hinst = GetModuleHandleW(nullptr);
  if (plugin_->PluginPreInitialize(nullptr, hinst) == 0) {
    error_message_ = "PluginPreInitialize failed.";
    return false;
  }

  // Milkdrop2Path = EXE 根目录（CMake post-build 已把 data/ 复制到此）
  swprintf_s(plugin_->m_szMilkdrop2Path, L"%s\\", init_exe_dir_);

  // 复用 EXE 旁部署的 milkdrop_presets/（MilkDrop3 向后兼容 .milk / .milk2）
  swprintf_s(plugin_->m_szPresetDir, L"%s\\milkdrop_presets\\", init_exe_dir_);
  CreateDirectoryW(plugin_->m_szPresetDir, nullptr);

  wchar_t tex_dir[MAX_PATH] = {0};
  swprintf_s(tex_dir, L"%s\\textures\\", init_exe_dir_);
  CreateDirectoryW(tex_dir, nullptr);

  return true;
}

bool Api::Initialize_CreateDevice(int width, int height) {
  return CreateD3d9Device(width, height);
}

bool Api::Initialize_PluginInit(int engine_width, int engine_height) {
  if (!plugin_ || !device_) {
    error_message_ = "Initialize_PluginInit: plugin_ or device_ is null.";
    return false;
  }

  // ---- Pre-flight: 检查 data/*.fx 着色器是否存在 ----
  {
    wchar_t shader_test[MAX_PATH] = {0};
    swprintf_s(shader_test, L"%s\\data\\include.fx", init_exe_dir_);
    if (GetFileAttributesW(shader_test) == INVALID_FILE_ATTRIBUTES) {
      error_message_ =
          "MilkDrop3 HLSL shader files (*.fx) are missing.\n"
          "Expected: <EXE_DIR>\\data\\include.fx\n"
          "Source:   third_party/milkdrop3/resources/Milkdrop2/data/*.fx\n"
          "Re-run CMake configure + build to fix.";
      return false;
    }
  }

  // 跳过 PluginInitialize 内的 LoadRandomPreset（NSEEL x64 死循环规避），
  // 初始化完成后统一在此处 UpdatePresetList + LoadRandomPreset。
  plugin_->m_bInitialPresetSelected = true;

  if (!plugin_->PluginInitialize(device_, &d3dpp_, render_hwnd_,
                                  engine_width, engine_height)) {
    error_message_ = "PluginInitialize failed. "
                     "Check that Direct3D 9 is available.";
    return false;
  }

  // 校验引擎内部渲染尺寸并在需要时纠正
  const int engine_w = plugin_->GetClientWidth();
  const int engine_h = plugin_->GetClientHeight();
  if (engine_w != engine_width || engine_h != engine_height) {
    plugin_->SetClientSize(engine_width, engine_height);
  }

  window_width_  = engine_width;
  window_height_ = engine_height;
  ready_ = true;

  // 补做 AllocateMyDX9Stuff() 里被跳过的两步。
  // 强制顺序模式：引擎默认 m_bSequentialPresetOrder=false 时 CPlugin::NextPreset()
  // 直接走 LoadRandomPreset→rand()，表现为"下一首=随机"。设为 true 后
  // NextPreset(→LoadRandomPreset→sequential分支 m_nCurrentPreset++) 和
  // PrevPreset(→sequential分支 m_nCurrentPreset--) 都会按文件名字典序遍历。
  plugin_->m_bSequentialPresetOrder = true;
  plugin_->UpdatePresetList(false, true, false);
  if (plugin_->m_nPresets > 0) {
    plugin_->LoadRandomPreset(0.0f);
  }

  return true;
}

// ==========================================================================
// Destroy —— 销毁引擎和 D3D9 设备
// ==========================================================================

void Api::Destroy() {
  ready_ = false;

  {
    std::lock_guard<std::mutex> lock(injectors_mutex_);
    injectors_.clear();
  }

  if (plugin_) {
    plugin_->PluginQuit();
    plugin_ = nullptr;  // g_plugin 是全局对象，不 delete
  }

  if (device_) {
    device_->Release();
    device_ = nullptr;
  }

  if (d3d9_) {
    d3d9_->Release();
    d3d9_ = nullptr;
  }

  if (d3d9_dll_) {
    FreeLibrary(d3d9_dll_);
    d3d9_dll_ = nullptr;
  }

  render_hwnd_ = nullptr;
}

// ==========================================================================
// OnResize —— 窗口大小变更时触发 D3D9 Device Reset
// ==========================================================================

void Api::OnResize(int width, int height) {
  if (!ready_ || !device_ || !plugin_) return;

  window_width_  = width;
  window_height_ = height;

  plugin_->CleanUpDX9Stuff(0);
  plugin_->SetClientSize(width, height);

  d3dpp_.BackBufferWidth  = width;
  d3dpp_.BackBufferHeight = height;

  const HRESULT hr = device_->Reset(&d3dpp_);
  if (SUCCEEDED(hr)) {
    plugin_->AllocateDX9Stuff();
    SetWindowPos(render_hwnd_, nullptr, 0, 0, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
  } else {
    MD3_LOG("Api::OnResize: Device Reset FAILED hr=0x%08X",
            static_cast<unsigned>(hr));
  }
}

// ==========================================================================
// RenderFrame —— 渲染一帧（先运行 injector，再拿 PCM 调用引擎）
// ==========================================================================

void Api::RenderFrame() {
  if (!ready_ || !plugin_) return;

  RunPreRenderInjectors();

  unsigned char pcm_l[576] = {};
  unsigned char pcm_r[576] = {};
  {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    if (pcm_ready_) {
      memcpy(pcm_l, pcm_left_, sizeof(pcm_l));
      memcpy(pcm_r, pcm_right_, sizeof(pcm_r));
      pcm_ready_ = false;
    }
  }

  plugin_->PluginRender(pcm_l, pcm_r);
}

// ==========================================================================
// FeedPcm —— 注入立体声 float PCM（线程安全）
// ==========================================================================

void Api::FeedPcm(const float* interleaved_lr, unsigned int frame_count) {
  if (!ready_ || !interleaved_lr || frame_count == 0) return;

  std::lock_guard<std::mutex> lock(pcm_mutex_);
  ConvertPcmToMd3(interleaved_lr, frame_count, pcm_left_, pcm_right_);
  pcm_ready_ = true;
}

// ==========================================================================
// FeedSpectrum —— 注入外部预计算频谱（重采样到引擎的 0~11025 Hz × NUM_FREQUENCIES）
// ==========================================================================

void Api::FeedSpectrum(const float* magL, const float* magR,
                       unsigned int num_bins, float sample_rate) {
  if (!ready_ || !plugin_ || !magL || !magR ||
      num_bins == 0 || sample_rate <= 0.0f) {
    return;
  }

  // 源频谱 bin i 对应频率 f_i = i * sample_rate / (2 * num_bins)   （单边 FFT）
  // 目标 fSpectrum[j]（j∈[0, NUM_FREQUENCIES)）对应 f_j = j * 11025 / NUM_FREQUENCIES
  // → 源索引 src = f_j * 2 * num_bins / sample_rate
  //             = j * 11025 * 2 * num_bins / (NUM_FREQUENCIES * sample_rate)
  const float src_step = 11025.0f * 2.0f * static_cast<float>(num_bins) /
                         (static_cast<float>(NUM_FREQUENCIES) * sample_rate);

  // MilkDrop3 引擎的 fSpectrum 是"未归一化的 FFT 幅度累加"（大概量级 100+），
  // Y2Kmeter magData 是 FFT 输出的线性 mag（量级 0~5000+）。
  // AnalyzeNewSound 后续会把 3 段带内累加值除以 (end-start)，再除以经验因子
  // (0.32/0.38/0.20)，所以幅度绝对值不需要严格匹配，形状（相对能量）才重要。
  // 因此直接用线性 mag 即可，无需额外缩放。
  //
  // 若源比目标 bin 密（src_step > 1），则 O(N) 线性重采样即可；
  // 若源比目标 bin 稀（src_step < 1），也用相同公式，允许上采样。
  for (int j = 0; j < NUM_FREQUENCIES; ++j) {
    const float src_f = static_cast<float>(j) * src_step;
    int src_i = static_cast<int>(src_f);
    if (src_i < 0) src_i = 0;
    if (src_i >= static_cast<int>(num_bins)) src_i = static_cast<int>(num_bins) - 1;
    plugin_->m_y2kExternalSpectrum[0][j] = magL[src_i];
    plugin_->m_y2kExternalSpectrum[1][j] = magR[src_i];
  }
  plugin_->m_bY2kExternalSpectrumValid = true;
}

// ==========================================================================
// pre-render injector
// ==========================================================================

size_t Api::AddPreRenderInjector(Injector injector) {
  if (!injector) return 0;
  std::lock_guard<std::mutex> lock(injectors_mutex_);
  const size_t token = next_injector_token_++;
  injectors_.push_back({token, std::move(injector)});
  return token;
}

void Api::RemovePreRenderInjector(size_t token) {
  if (token == 0) return;
  std::lock_guard<std::mutex> lock(injectors_mutex_);
  injectors_.erase(
      std::remove_if(injectors_.begin(), injectors_.end(),
                     [token](const InjectorSlot& s) { return s.token == token; }),
      injectors_.end());
}

void Api::RunPreRenderInjectors() {
  // 复制到栈上后释放锁，避免用户 injector 内嵌调用 Add/Remove 引发死锁
  std::vector<Injector> local;
  {
    std::lock_guard<std::mutex> lock(injectors_mutex_);
    local.reserve(injectors_.size());
    for (const auto& s : injectors_) local.push_back(s.fn);
  }
  for (auto& fn : local) {
    if (fn) fn();
  }
}

// ==========================================================================
// 预设控制
// ==========================================================================

void Api::LoadPreset(const wchar_t* filename, float blend_time) {
  if (!plugin_ || !ready_) return;
  plugin_->LoadPreset(filename, blend_time);
}

void Api::NextPreset(float blend_time) {
  if (!plugin_ || !ready_) return;
  plugin_->NextPreset(blend_time);
}

void Api::PrevPreset(float blend_time) {
  if (!plugin_ || !ready_) return;
  plugin_->PrevPreset(blend_time);
}

void Api::RandomPreset(float blend_time) {
  if (!plugin_ || !ready_) return;
  // 临时关闭顺序模式，强制真正的随机预设跳转。
  // LoadRandomPreset 在 m_bSequentialPresetOrder=true 时会走 m_nCurrentPreset++ 顺序路径，
  // 与 NextPreset 行为完全一致，导致 ? 按钮失去随机语义。
  bool prev_order = plugin_->m_bSequentialPresetOrder;
  plugin_->m_bSequentialPresetOrder = false;
  plugin_->LoadRandomPreset(blend_time);
  plugin_->m_bSequentialPresetOrder = prev_order;
}

void Api::JumpToPreset(int fileIndex) {
  if (!plugin_ || !ready_) return;

  const int nFiles = plugin_->m_nPresets - plugin_->m_nDirs;
  if (fileIndex < 0 || fileIndex >= nFiles) return;

  // 文件级索引 → 数组下标：跳过前 m_nDirs 个目录项。
  // MergeSortPresets 将目录与文件按文件名统一排序后，目录项仍聚集在前 m_nDirs 个位置，
  // 因为排序前 temp_presets 是先目录后文件依次 push 的，且 MergeSortPresets 是稳定排序。
  const int arrayIndex = fileIndex + plugin_->m_nDirs;

  plugin_->m_nCurrentPreset = arrayIndex;

  // 构造完整路径：预设目录（以 \\ 结尾） + 文件名
  // szFilename 仅含文件名不含路径，LoadPreset 内部用 GetFileAttributesW
  // 检查文件是否存在，必须传入完整路径。
  wchar_t szFullPath[MAX_PATH];
  wcscpy_s(szFullPath, plugin_->m_szPresetDir);
  wcscat_s(szFullPath, plugin_->m_presets[arrayIndex].szFilename.c_str());
  plugin_->LoadPreset(szFullPath, 2.0f);
}

// ==========================================================================
// 预设名 overlay（引擎侧 D3D9 绘制）
// ==========================================================================

void Api::EnablePresetInfoOverlay(bool on) {
  if (!plugin_) return;
  plugin_->m_bShowPresetInfo = on;
}

void Api::ShowPresetTitleAnim(const wchar_t* text) {
  if (!plugin_ || !ready_ || !text) return;

  // 通过引擎的 song-title 通道：写入 m_szSongTitle 并触发动画状态机
  wcscpy_s(plugin_->m_szSongTitle, text);
  plugin_->LaunchSongTitleAnim();
}

void Api::DisableAutoAdvance() {
  if (plugin_) {
    plugin_->m_bPresetLockedByUser = true;
  }
}

// ==========================================================================
// 诊断
// ==========================================================================

int Api::GetCurrentPresetIndex() const noexcept {
  if (!plugin_) return -1;
  if (plugin_->m_nCurrentPreset < plugin_->m_nDirs) return -1;
  return plugin_->m_nCurrentPreset - plugin_->m_nDirs;
}

int Api::GetTotalPresets() const noexcept {
  return plugin_ ? plugin_->m_nPresets : 0;
}

int Api::GetFileablePresetCount() const noexcept {
  if (!plugin_) return 0;
  return plugin_->m_nPresets - plugin_->m_nDirs;
}

std::wstring Api::GetCurrentPresetName() const {
  if (!plugin_) return L"";
  return std::wstring(plugin_->m_szCurrentPresetFile);
}

// ==========================================================================
// 内部辅助
// ==========================================================================

bool Api::CreateD3d9Device(int width, int height) {
  d3d9_dll_ = LoadLibraryW(L"d3d9.dll");
  if (!d3d9_dll_) {
    error_message_ = "Direct3D 9 (d3d9.dll) is not available on this system.";
    return false;
  }

  using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
  auto d3d9_create = reinterpret_cast<Direct3DCreate9Fn>(
      GetProcAddress(d3d9_dll_, "Direct3DCreate9"));
  if (!d3d9_create) {
    error_message_ = "Direct3DCreate9 not found in d3d9.dll.";
    return false;
  }

  d3d9_ = d3d9_create(D3D_SDK_VERSION);
  if (!d3d9_) {
    error_message_ = "Direct3DCreate9 failed.";
    return false;
  }

  ZeroMemory(&d3dpp_, sizeof(d3dpp_));
  d3dpp_.Windowed               = TRUE;
  d3dpp_.SwapEffect             = D3DSWAPEFFECT_DISCARD;
  d3dpp_.BackBufferFormat       = D3DFMT_X8R8G8B8;
  d3dpp_.BackBufferWidth        = width;
  d3dpp_.BackBufferHeight       = height;
  d3dpp_.hDeviceWindow          = render_hwnd_;
  d3dpp_.EnableAutoDepthStencil = TRUE;
  d3dpp_.AutoDepthStencilFormat = D3DFMT_D16;
  d3dpp_.PresentationInterval   = D3DPRESENT_INTERVAL_IMMEDIATE;

  D3DCAPS9 caps;
  d3d9_->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);

  DWORD behavior_flags = D3DCREATE_MULTITHREADED;
  behavior_flags |= (caps.VertexProcessingCaps != 0)
                        ? D3DCREATE_HARDWARE_VERTEXPROCESSING
                        : D3DCREATE_SOFTWARE_VERTEXPROCESSING;

  HRESULT hr = d3d9_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                    render_hwnd_, behavior_flags,
                                    &d3dpp_, &device_);
  if (FAILED(hr)) {
    // 兜底：禁用 depth-stencil 再试一次
    d3dpp_.EnableAutoDepthStencil = FALSE;
    hr = d3d9_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                              render_hwnd_, behavior_flags, &d3dpp_, &device_);
  }

  if (FAILED(hr)) {
    error_message_ = "Failed to create Direct3D 9 device. HRESULT: 0x" +
                     std::to_string(static_cast<unsigned>(hr));
    return false;
  }

  return true;
}

void Api::ConvertPcmToMd3(const float* interleaved_lr,
                          unsigned int frame_count,
                          unsigned char* out_l,
                          unsigned char* out_r) {
  constexpr int kMd3Samples = 576;

  // float [-1, +1] → unsigned char centered at 128（引擎侧 ^128-128 会还原）
  auto float_to_u8 = [](float v) -> unsigned char {
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<unsigned char>(
        std::lround(v * 127.0f) + 128);
  };

  for (int i = 0; i < kMd3Samples; ++i) {
    // 线性重采样源 frame_count → 目标 576
    const int src_idx = std::min<int>(
        static_cast<int>(static_cast<float>(i) *
                         static_cast<float>(frame_count) /
                         static_cast<float>(kMd3Samples)),
        static_cast<int>(frame_count) - 1);
    out_l[i] = float_to_u8(interleaved_lr[src_idx * 2]);
    out_r[i] = float_to_u8(interleaved_lr[src_idx * 2 + 1]);
  }
}

}  // namespace milkdrop3_api
