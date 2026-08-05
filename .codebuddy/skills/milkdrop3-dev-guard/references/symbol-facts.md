# Symbol Facts (关键符号事实清单)

> 本文档为 [SKILL.md](../SKILL.md) §2 前置检查表的扩展版，附带**验证命令**与**常见误解**。

修改代码前，请对以下事实用 `grep_search` 或 `read_file` **重新验证**，不要凭记忆。

---

## 1. 引擎侧常量

| 事实 | 出处 | 验证命令 |
|---|---|---|
| `NUM_FREQUENCIES == 512`（0~11 kHz 输出频段数） | `third_party/milkdrop3/code/vis_milk2/defines.h:199` | `grep -n "NUM_FREQUENCIES" third_party/milkdrop3/code/vis_milk2/defines.h` |
| `SAMPLE_SIZE_LPB == 576`（音频缓冲深度） | `third_party/milkdrop3/code/audio/audiobuf.cpp:5` | 同上 |
| `NUM_WAVEFORM_SAMPLES` 定义在 `shell_defines.h`，`fWaveform[2][576]` 中只有前 N 个有效 | `pluginshell.h:59` | `grep -n "NUM_WAVEFORM_SAMPLES" third_party/milkdrop3/code/vis_milk2/shell_defines.h` |

**常见误解**：文档旧版曾把 `NUM_FREQUENCIES` 误标注在 `shell_defines.h`。正确位置是 `defines.h`。

---

## 2. 引擎侧结构

| 事实 | 出处 | 备注 |
|---|---|---|
| `m_szSongTitle` 类型 `wchar_t[512]` | `plugin.h:470` | 不是 256；写入用 `_snwprintf_s(dst, 512, _TRUNCATE, ...)` |
| `m_szSongTitlePrev` 类型 `wchar_t[512]` | `plugin.h:471` | — |
| `m_bShowPresetInfo` 类型 `bool` | `plugin.h:446` | 每帧右上角写预设名的常显开关 |
| `LaunchSongTitleAnim()` 无参数 void 函数 | `plugin.h:583` | 触发中央大字动画；文本来源是 `m_szSongTitle` |
| `m_bSequentialPresetOrder` 类型 `bool` | `plugin.h:283` | 真时 `LoadRandomPreset` 变顺序切；随机分支需临时关掉 |
| `m_presets` 类型 `std::vector<PresetInfo>`，`PresetInfo::szFilename` 类型 `std::wstring` | `plugin.h` 附近 | 不是 `m_pPresetAddr` |

---

## 3. Y2K 引擎侧扩展字段（白名单唯一开口）

| 字段 | 类型 | 出处 | 语义 |
|---|---|---|---|
| `m_bY2kExternalSpectrumValid` | `bool = false` | `pluginshell.h:120` | consume-once 开关 |
| `m_y2kExternalSpectrum` | `float[2][NUM_FREQUENCIES]` | `pluginshell.h:121` | 外部注入频谱 |

**消费点**：`pluginshell.cpp::CPluginShell::AnalyzeNewSound()` 在 `time_to_frequency_domain(...)` 之后检查 `m_bY2kExternalSpectrumValid`，若为 true：
1. `memcpy` 外部谱到 `m_sound.fSpectrum`；
2. 跳过内建 FFT；
3. 置 `m_bY2kExternalSpectrumValid = false`（consume-once）。

**验证命令**：`grep -n "m_bY2kExternalSpectrumValid\|m_y2kExternalSpectrum" third_party/milkdrop3/code/vis_milk2/pluginshell.h third_party/milkdrop3/code/vis_milk2/pluginshell.cpp`

---

## 9. Y2KStandaloneApp 启动序列关键事实

| # | 事实 | 验证 |
|---|---|---|
| a | 进程 DPI 感知只用 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`，在 initialise 首行。 | `grep -n "SetProcess\|SetThreadDpi" source/standalone/Y2KStandaloneApp.cpp` |
| b | `pluginHolder = std::make_unique<juce::StandalonePluginHolder>` 必须早于 `mainWindow = std::make_unique<Y2KMainWindow>` | 手工或脚本 `check_init_sequence.py` |
| c | `mainWindow` 创建后**到函数末尾前**，不得调用 `addToDesktop()` 或 `setVisible(...)`。 | `grep -c "addToDesktop\|setVisible" …` 预期 ≤ 2 |
| d | `addToDesktop()` 与 `setVisible(true)` 必须在 initialise 的中后段直接相邻，中间不得插入任何其它语句。 | 手工 |
| e | 不存在 `TimerThreadBoot` / `boot.startTimer(1)` / `SharedResourcePointer<TimerThread>` 之类预热代码。 | `grep -n "TimerThreadBoot\|SharedResourcePointer<TimerThread>" …` 应为空 |

**一句话铁律**：这个序列是 v2.3.4 （commit `89924448`）已验证稳定的。任何背离都会引发 `<unknown> 0x00007ffc*d9f8`（`ntdll!LdrLockLoaderLock`）死锁。详情 → [init-sequence.md](init-sequence.md) / [forbidden-list.md#f10](forbidden-list.md)

---

## 4. Y2Kmeter 侧 AnalyserHub 接口

| 事实 | 出处 |
|---|---|
| 类名 `class AnalyserHub` | `source/analysis/AnalyserHub.h:291` |
| `retain(Kind) noexcept` / `release(Kind) noexcept` | `AnalyserHub.h:346-348` |
| `addFrameListener(FrameListener*)` / `removeFrameListener(FrameListener*)` | `AnalyserHub.h:415-416` |
| `FrameListener::onFrame(const FrameSnapshot&)` UI 线程回调 | `AnalyserHub.h:407-411` |
| `FrameSnapshot::spectrumMag` 大小 = `spectrumMagSize = 1024` | `AnalyserHub.h:323, 390` |
| `FrameSnapshot::spectrumMagLo` 大小 = `spectrumMagSizeLo = 4096` | `AnalyserHub.h:331, 393` |
| `FrameSnapshot::oscL / oscR` 大小 = `oscilloscopeBufferSize` | `AnalyserHub.h:386-387` |
| `getLatestFrame()` 返回 `std::shared_ptr<const FrameSnapshot>` | `AnalyserHub.h:420` |

**常见误解**：曾错误引用 `AudioSnapshot::kSpectrumSize`——**不存在**。真实符号是 `AnalyserHub::spectrumMagSize`。

---

## 5. Y2Kmeter 侧 Milkdrop3Api 接口现状

| 存在（v20 之后） | 已删除（禁止复活） |
|---|---|
| `Initialize_CreateRenderWindow / SetPaths / PreInit / CreateDevice / PluginInit`（五段式） | `Initialize()`（单大函数） |
| `Destroy()` | `CreateRenderWindow(HWND, int, int)`（单句包装） |
| `OnResize(w, h)` | `SetPresetDir(...)` |
| `RenderFrame()` | `CycleRenderScale / ApplyRenderScale / render_scale_` |
| `FeedPcm(const float* interleaved_lr, unsigned frame_count)` | `hub_retained_` 冗余布尔 |
| `FeedSpectrum(magL, magR, num_bins, sample_rate)` | |
| `AddPreRenderInjector(std::function<void()>) → size_t token` | |
| `RemovePreRenderInjector(token)` | |
| `LoadPreset(wchar_t*, blend) / NextPreset / PrevPreset / RandomPreset / JumpToPreset(int)` | |
| `EnablePresetInfoOverlay(bool)` | |
| `ShowPresetTitleAnim(const wchar_t*)` | |
| `DisableAutoAdvance()` | |
| `GetCurrentPresetIndex() / GetTotalPresets() / GetCurrentPresetName()` | |

**验证命令**：`grep -n "^\s*\(void\|bool\|int\|size_t\|std::wstring\)" source/ui/modules/Milkdrop3Api.h`

---

## 6. FeedSpectrum 的重采样公式

MilkDrop3 `fSpectrum[512]` 只覆盖 **0~11025 Hz**。Y2Kmeter 的 `FrameSnapshot::spectrumMag[1024]` 覆盖 **0~24000 Hz**（fftSize=2048, sr=48 kHz）。

`Milkdrop3Api::FeedSpectrum` 内部做 O(N) 线性重采样：

```
src_step = 11025.0f * 2.0f * num_bins / (NUM_FREQUENCIES * sample_rate);
for (int i = 0; i < NUM_FREQUENCIES; ++i) {
  float src_idx = i * src_step;
  // 线性插值 src[floor(src_idx)] 和 src[ceil(src_idx)]
}
```

**禁止**：直接 `memcpy(m_y2kExternalSpectrum[c], magL_or_R, sizeof(float) * NUM_FREQUENCIES)`——高频段会被截断到静音，低频段被拉伸。

---

## 7. 预设目录

固定为 `<EXE_DIR>\milkdrop_presets\`，由 `Initialize_PreInit` 内 `swprintf_s(m_szPresetDir, ...)` 设置。

**改路径**：只能改 `Initialize_PreInit` 内的字符串拼接；`SetPresetDir` 已删除。

---

## 8. DPI 感知

- 进程级：`SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`，在 `PluginProcessor` / `PluginEditor` 构造早期完成。
- 坐标转换 API：
  ```cpp
  auto& disp = juce::Desktop::getInstance().getDisplays();
  auto phys = disp.logicalToPhysical(juce::Rectangle<int>{x, y, w, h});
  auto logi = disp.physicalToLogical(juce::Rectangle<int>{px, py, pw, ph});
  ```
- **禁止**用 `GetDpiForWindow / GetDpiForMonitor` 手工换算。

---

## 10. ModulePanel 成员可见性

`ModulePanel`（`source/ui/ModuleWorkspace.h`）作为所有可视化模块的基类，成员分区严格：

| 分区 | 成员 | 子类可用性 |
|---|---|---|
| `protected` | `titleText`（`juce::String`） | ✅ |
| `protected` | `getTitleBarBounds()` | ✅ |
| `protected` | `getCloseButtonBounds()` | ✅ |
| `protected` | `getContentBounds()` | ✅ |
| `protected` | `paintContent(juce::Graphics&)` | ✅ 可 override |
| `protected` | `layoutContent(int, int, int, int)` | ✅ 可 override |
| **`private`** | `closeButtonPressed`（`bool`） | ❌ 禁止访问 |
| **`private`** | `closeButtonHovered`（`bool`） | ❌ 禁止访问 |
| **`private`** | 拖拽 / detectEdge 相关字段 | ❌ |

**关键陷阱**：`closeButtonPressed` / `closeButtonHovered` 是 `private`，`Milkdrop3Module::paint(g)` 里直接 `if (closeButtonPressed)` 会 MSVC C2065（未声明的标识符）。这是本轮第 12 轮 FAIL 的直接原因。

**首选做法**：让 `ModulePanel::paint(g)` 走默认路径完成标题栏 + 关闭按钮绘制，子类只在 `paintContent` / `paintOverChildren` 追加自己的内容。

**验证命令**：
```bash
grep -nE "closeButtonPressed|closeButtonHovered" source/ui/ModuleWorkspace.h
# 期望：出现在 `private:` 段，而不是 `protected:` 段
```

---

## 11. Win32 关键 API 语义速查（防呆）

### 11.1 `SetWindowPos(hwnd, hWndInsertAfter, ...)`

`hWndInsertAfter` 语义 = "把 `hwnd` **放到** `hWndInsertAfter` **之后（下方）**"。

| `hWndInsertAfter` | 效果 |
|---|---|
| `HWND_TOP`     | 置顶（top-most 之下、非 top-most 之上） |
| `HWND_BOTTOM`  | 置底 |
| `HWND_TOPMOST` | 恒定置顶（并加 `WS_EX_TOPMOST`） |
| `HWND_NOTOPMOST` | 从 topmost 摘除，置于其下 |
| `hOther` 具体 HWND | `hwnd` 置于 `hOther` **之下** |

**常见误用**：想把 `overlay` 提到 `d3d_popup` 之上，写 `SetWindowPos(overlay, d3d_popup, ...)` — **反了**，overlay 会被推到 D3D 下方彻底不可见。正确写 `HWND_TOP`（详见 [forbidden-list.md#f13](forbidden-list.md)）。

### 11.2 `EDIT` 子控件的 Enter / Esc 处理

单行 `EDIT`（`CreateWindowExW(L"EDIT", ...)`）中：

| 键 | 靠什么消息 |
|---|---|
| `VK_ESCAPE` | ✅ `WM_KEYDOWN`（可靠） |
| `VK_RETURN` | ❌ `WM_CHAR` **不可靠**——依赖父窗口 dialog 属性 / `IsDialogMessage` 循环 / `ES_WANTRETURN` |
| `VK_RETURN` | ✅ `WM_KEYDOWN`（可靠） |

**约定**：milkdrop3 的所有 EDIT 子控件（如预设跳转弹窗）都必须在 `WM_KEYDOWN` 里检测 `VK_RETURN` 和 `VK_ESCAPE`，禁止用 `WM_CHAR`（详见 [forbidden-list.md#f14](forbidden-list.md)）。

### 11.3 `WndProc` 里的 `MessageManager::callAsync`

D3D9 popup 收到 `WM_LBUTTONDOWN` 等消息时，回到 JUCE 主逻辑必须走 `MessageManager::callAsync`，避免在 audio/GDI 线程直接操作 JUCE 组件：

```cpp
case WM_LBUTTONDOWN:
  juce::MessageManager::callAsync([this]{
    // 现在在 JUCE 消息线程，可以安全操作 owner_ / overlay_
  });
  return 0;
```

**禁止**：`PostMessage(GetParent(hwnd), WM_LBUTTONDOWN, ...)` 转发（历史上偶发失效，详见 [forbidden-list.md#f3](forbidden-list.md)）。

---