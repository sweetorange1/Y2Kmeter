# MilkDrop3 引擎集成文档

> 本文档记录将 milkdrop2077/MilkDrop3 音频可视化渲染引擎集成到 Y2Kmeter
> 项目的完整过程，包括源码获取、架构分析、代码剥离、接口封装与模块化设计。

---

## 1. 项目背景与目标

### 1.1 现有架构

Y2Kmeter 自 v2.0.4 起集成了 **libprojectM 4**（来自 `projectM-visualizer/projectM`）
作为 Milkdrop 可视化模块，通过 `ProjectMApi` 动态加载 `projectM-4.dll` 运行。

```
现有 Milkdrop 模块架构:
┌───────────────────────────────────────────────────────────┐
│  ProjectMApi (LoadLibrary + GetProcAddress 封装)           │
│    ├─ 动态加载 third_party/projectm/bin/projectM-4.dll    │
│    ├─ 提供 C API 函数指针                                  │
│    └─ 线程安全单例                                        │
│                                                           │
│  MilkdropModule (ModulePanel 子类)                         │
│    ├─ UI 交互（预设切换/自动轮播/Overlay 控制栏）           │
│    ├─ AnalyserHub → PCM → projectM                         │
│    └─ Editor OpenGL 上下文 → FBO 渲染                      │
│                                                           │
│  PluginEditor (OpenGLRenderer)                             │
│    ├─ newOpenGLContextCreated: GLEW init → projectm_create  │
│    ├─ renderOpenGL: openglRenderFrameFbo → offscreen FBO  │
│    └─ openGLContextClosing: projectm_destroy               │
└───────────────────────────────────────────────────────────┘
```

### 1.2 集成目标

在保留现有 libprojectM 模块的基础上，新增 **MilkDrop3 原生引擎** 作为独立模块：

| 维度 | 现有 projectM 模块 | 新增 MilkDrop3 模块 |
|------|:------------------:|:-------------------:|
| 源码位置 | `projectM-visualizer/projectM` | `milkdrop2077/MilkDrop3` |
| 渲染 API | OpenGL 4.1 (GLSL) | Direct3D 9 (HLSL) |
| 链接方式 | LoadLibrary 动态加载 DLL | 编译期静态链接源码 |
| 封装层 | `ProjectMApi` | `Milkdrop3Api` |
| 模块类 | `MilkdropModule` | `Milkdrop3Module` |
| 窗口模式 | Editor GL Context 内渲染 | 独立 D3D9 子窗口 (HWND) |
| 预设格式 | `.milk` | `.milk` + `.milk2` (双预设) |
| 特色功能 | 标准 Milkdrop | +16 shapes/waves、节拍检测、HLSL 效果 |

---

## 2. 源码获取

### 2.1 来源

- **仓库**: [github.com/milkdrop2077/MilkDrop3](https://github.com/milkdrop2077/MilkDrop3)
- **分支**: `main` (最新提交: ~2026-07)
- **授权**: 已获得 MilkDrop3 作者明确许可授权
- **获取方式**: `git clone --depth 1` 浅克隆
- **落盘路径**: `I:/Y2KMeter/third_party/milkdrop3/`

### 2.2 目录结构

```
third_party/milkdrop3/
├── README.md                          # 原始项目说明
└── code/
    ├── LICENSE.txt                    # BSD 3-Clause (BeatDrop)
    ├── MilkDrop3.sln                  # Visual Studio 2022 解决方案
    ├── audio/                         # WASAPI 音频捕获子系统 (将被替换)
    │   ├── audiobuf.h/.cpp            # 环形缓冲区 (8-bit PCM)
    │   ├── loopback-capture.h/.cpp    # WASAPI Loopback 捕获
    │   ├── prefs.h/.cpp              # 音频设备选择
    │   └── common.h/log.h/cleanup.h  # 辅助模块
    ├── ns-eel2/                       # EEL2 表达式编译器/VM (保留)
    │   ├── ns-eel.h / ns-eel-int.h   # 公共头文件
    │   ├── nseel-compiler.c          # 表达式编译器
    │   ├── nseel-eval.c              # 表达式求值器
    │   ├── asm-nseel-x86-msvc.c      # x86 MSVC JIT 汇编
    │   ├── asm-nseel-x86-gcc.c       # x86 GCC JIT 汇编
    │   └── asm-nseel-ppc-gcc.c       # PPC JIT 汇编
    ├── resources/Milkdrop2/data/      # HLSL Shader 模板 (保留)
    │   ├── warp_vs.fx / warp_ps.fx   # Warp 通道 Shader
    │   ├── comp_vs.fx / comp_ps.fx   # Composite 通道 Shader
    │   ├── blur1_ps.fx / blur2_ps.fx # 模糊通道 Shader
    │   ├── blur_vs.fx                # 模糊顶点 Shader
    │   └── include.fx                # Shader 公共头文件
    └── vis_milk2/                     # 核心渲染引擎 (保留 + 适配)
        ├── plugin.h (.cpp: 355KB)    # CPlugin 主类
        ├── pluginshell.h (.cpp: 88KB) # CPluginShell 基类
        ├── state.h (.cpp: 80KB)      # CState 预设状态机
        ├── dxcontext.h (.cpp)        # DXContext D3D9 管理
        ├── fft.h/.cpp                # FFT 快速傅里叶变换
        ├── texmgr.h/.cpp             # 纹理管理器
        ├── textmgr.h/.cpp            # 文本管理器
        ├── menu.h/.cpp               # UI 菜单系统
        ├── support.h/.cpp            # 辅助函数
        ├── utility.h/.cpp            # 工具函数
        ├── md_defines.h              # 模块常量定义
        ├── shell_defines.h           # Shell 层常量
        └── defines.h                 # 公共定义
```

### 2.3 清理操作

- 移除仓库根目录截图文件 (`*.jpg`)
- 移除 `linux/` 目录 (与 Windows 集成无关)

---

## 3. 许可证合规分析

### 3.1 许可证确认

| 文件/组件 | 许可协议 | 权利人 | 兼容性 |
|-----------|---------|--------|:------:|
| `code/LICENSE.txt` | **BSD 3-Clause** | Maxim Volskiy (BeatDrop) | ✅ |
| `vis_milk2/plugin.h` | BSD 3-Clause | Nullsoft, Inc. (MilkDrop) | ✅ |
| `ns-eel2/` | 内置于 MilkDrop2 源码 | Nullsoft, Inc. | ✅ |
| `resources/Milkdrop2/` | BSD 3-Clause | Nullsoft, Inc. | ✅ |

### 3.2 合规措施

1. 完整保留所有源码中的 BSD 3-Clause 版权声明头
2. 在 Y2Kmeter 的 README.md 中添加 MilkDrop3 版权说明
3. 安装目录中附带 `LICENSE.txt` 文件

---

## 4. 架构分析

### 4.1 继承层级

```
CPluginShell (pluginshell.h/.cpp)          ← Winamp vis 插件外壳
  ├─ D3D9 设备管理 (DXContext)
  ├─ 窗口管理 (全屏/窗口/桌面/VJ 四种模式)
  ├─ 帧计时 / FPS 限制
  ├─ 音频分析 / 波形对齐
  ├─ 字体 / UI 绘制
  └─ 配置文件读写 (milkdrop_config.ini)
      │
      └── CPlugin (plugin.h/.cpp)          ← MilkDrop 核心引擎
           ├─ MyRenderFn()         ← 每帧渲染入口
           ├─ LoadPreset()         ← 预设加载
           ├─ WarpedBlit_Shaders() ← HLSL Warp 通道
           ├─ ShowToUser_Shaders() ← HLSL Comp 通道
           ├─ DrawCustomWaves()    ← 最多 16 条波形
           ├─ DrawCustomShapes()   ← 最多 16 个图形
           ├─ DrawSprites()        ← 精灵/纹理叠加
           ├─ RunPerFrameEquations() ← ns-eel2 表达式执行
           ├─ BlurPasses()         ← 多级模糊
           ├─ CState ×3            ← 当前/旧/新 预设状态
           └─ PShaderSet / VShaderSet ← HLSL Shader 管理
```

### 4.2 数据流 (每帧)

```
PCM 音频数据
    │
    ▼
AnalyzeNewSound()            ← 将 8-bit PCM → 频谱分析 (FFT)
    │                          产出: m_sound (bass/mid/treble, waveform, spectrum)
    ▼
LoadPresetTick()             ← 异步加载预设 (如果需要)
    │
    ▼
RunPerFrameEquations()       ← 执行 .milk 中的 per_frame 方程
    │                          更新: q1-q32 变量, 所有 CBlendableFloat
    ▼
DrawCustomWaves()            ← 渲染自定义波形 (最多 16 条)
DrawCustomShapes()           ← 渲染自定义图形 (最多 16 个)
    │
    ▼
WarpedBlit_Shaders()         ← Warp 通道: 采样上一帧纹理, 通过变形网格
    │                          glCopyTexSubImage2D → VS texture
    ▼
BlurPasses()                 ← 可选多级模糊 (6 级, 每级 1/2 缩小)
    │
    ▼
ShowToUser_Shaders()         ← Composite 通道: 合成 Warp 结果 + 最终输出
    │
    ▼
DrawSprites() / DrawMotionVectors() ← 覆盖层
    │
    ▼
Present()                    ← IDirect3DSwapChain9::Present()
```

### 4.3 关键数据结构

**AudioBuf 格式转换** (与 Y2Kmeter 的适配关键点):

```
MilkDrop3 内部音频格式:
  ┌────────────────────────────────────────────────────┐
  │ AudioBuf: unsigned char[576] × 2 (L/R)            │
  │ 内部存储: int8_t (有符号) 存储在 unsigned char 中   │
  │ float → int8_t: (int8_t)(flt * 128), clamp[-128,127]│
  │ int16 → int8_t: (signed char)(sample / 256)        │
  │ SAMPLE_SIZE_LPB = 576                              │
  └────────────────────────────────────────────────────┘

Y2Kmeter AnalyserHub 输出:
  ┌────────────────────────────────────────────────────┐
  │ FrameSnapshot: float oscL[2048] / oscR[2048]       │
  │ 32-bit float, [-1.0, +1.0]                        │
  └────────────────────────────────────────────────────┘

适配: 将 float[-1,+1] → int8_t[-128,+127] → unsigned char
```

**CState 表达求值系统**:

```
CState (预设状态)
  ├─ per_frame 方程 → ns-eel2 编译器 → NSEEL_CODEHANDLE
  │   注册变量: time, fps, frame, progress, bass/mid/treb,
  │            bass_att/mid_att/treb_att,
  │            zoom, rot, warp, cx, cy, dx, dy, sx, sy,
  │            decay, wave_*, ob_*/ib_*,
  │            q1-q32, t1-t8, monitor, echo_*, ...
  │
  ├─ per_pixel 方程 → 嵌入 Warp/Comp Shader HLSL 代码中
  │
  └─ 自定义 Wave/Shape 各自的 per_frame / per_point 方程
```

### 4.4 外部耦合点识别

| 模块 | 耦合目标 | Y2Kmeter 集成策略 |
|------|---------|------------------|
| `CPluginShell::PluginInitialize()` | Winamp vis plugin API (D3D Device + HWND 由宿主传入) | **适配**: 由 `Milkdrop3Api` 创建 D3D9 Device 并传入 |
| `CPluginShell::PluginRender()` | Winamp 每帧回调, 传入 8-bit PCM | **适配**: 由 `Milkdrop3Module::onFrame()` 驱动, AnalyserHub → SetAudioBuf |
| `CPluginShell::PluginQuit()` | Winamp 卸载回调 | **适配**: 由 `Milkdrop3Api::Destroy()` 调用 |
| `audio/loopback-capture.cpp` | WASAPI COM API 获取系统音频 | **移除**: Y2Kmeter 已有 `AnalyserHub` |
| `CPluginShell` 窗口管理 | `CreateWindowEx`, 全屏/窗口切换 | **重构**: 固定窗口模式, HWND 由 Y2Kmeter 托管 |
| `CPluginShell` 配置读写 | `milkdrop_config.ini` (Winamp 插件目录) | **适配**: 改为 `{userappdata}/Y2Kmeter/milkdrop3/` |
| `ns-eel2` | 独立表达式引擎 | **保留**: 零耦合, 直接编译 |
| HLSL Shader 模板 (`.fx`) | D3DXCompileShader | **保留**: Shader 源码是纯文本 |

---

## 5. 代码剥离方案

### 5.1 保留清单

| 模块 | 文件 | 说明 |
|------|------|------|
| 核心引擎 | `vis_milk2/plugin.h/.cpp` | CPlugin 完整类, 包含全部渲染逻辑 |
| 预设状态 | `vis_milk2/state.h/.cpp` | CState / CShape / CWave |
| D3D9 管理 | `vis_milk2/dxcontext.h/.cpp` | DXContext (简化使用) |
| 表达式引擎 | `ns-eel2/*` | 完整子项目 |
| Shader 模板 | `resources/Milkdrop2/data/*.fx` | 7 个 .fx 文件 |
| FFT | `vis_milk2/fft.h/.cpp` | 音频频谱分析 |
| 纹理管理 | `vis_milk2/texmgr.h/.cpp` | 用户精灵纹理 |
| 定义文件 | `vis_milk2/md_defines.h`, `shell_defines.h`, `defines.h` | 常量与宏 |
| 辅助函数 | `vis_milk2/support.h/.cpp`, `utility.h/.cpp` | 工具函数 |
| 音频缓冲 | `audio/audiobuf.h/.cpp` | SetAudioBuf / GetAudioBuf |

### 5.2 移除清单

| 模块 | 文件 | 原因 |
|------|------|------|
| WASAPI 捕获 | `audio/loopback-capture.h/.cpp` | Y2Kmeter 用 AnalyserHub 替代 |
| 设备选择 | `audio/prefs.h/.cpp` | 同上 |
| 辅助 | `audio/common.h`, `log.h/.cpp`, `cleanup.h`, `guid.cpp` | WASAPI 依赖 |
| Winamp DLL 入口 | `plugin.cpp` 中的 `winampVisGetHeader()` 等 | 非 Winamp 宿主 |
| Winamp 资源 | `plugin.rc`, `resource.h`, `plugin_icon.ico` | Winamp 插件资源 |
| 项目文件 | `MilkDrop3.sln`, `plugin.vcxproj` | 用 CMake 替代 |
| 菜单系统 | `menu.h/.cpp` | 独立应用 UI, 需简化 |
| 文本管理 | `textmgr.h/.cpp` | 独立应用 UI |

### 5.3 需要适配的文件

| 文件 | 适配内容 |
|------|---------|
| `pluginshell.h/.cpp` | 移除 Winamp 宿主依赖, 改为 Y2Kmeter 的初始化路径 |
| `plugin.h/.cpp` | 移除 `winampVisGetHeader()`, 添加 `MD3_STANDALONE` 编译宏 |
| `dxcontext.h/.cpp` | 固定为窗口模式, 移除全屏/桌面模式 |
| `pluginshell.h/.cpp` | 配置文件路径改为 `{appdata}/Y2Kmeter/milkdrop3/` |
| `state.h/.cpp` | 预设路径改为相对路径或绝对路径参数化 |

### 5.4 编译宏设计

```cpp
// 新增: Y2Kmeter/milkdrop3 编译时定义
#define MD3_Y2KMETER 1     // 标识在 Y2Kmeter 环境内编译
// 效果:
//   - 跳过 winampVisGetHeader() 等 Winamp DLL 入口
//   - 跳过 WASAPI 音频捕获初始化
//   - 预设/纹理路径使用 Y2Kmeter 的 AppData 目录
//   - 禁用全屏/桌面模式, 仅保留窗口模式
//   - 禁用独立配置文件读写
```

---

## 6. 封装设计

### 6.1 Milkdrop3Api (对标 ProjectMApi)

```cpp
// 设计原则:
//   - 对标 ProjectMApi 的简洁接口风格
//   - 管理 D3D9 设备生命周期
//   - 管理 CPlugin 引擎实例
//   - 提供 PCM 注入接口
//   - 线程安全 (锁保护 PCM 缓冲区)

namespace milkdrop3_api {

class Api {
public:
  static Api& Instance();

  // ---- 生命周期 ----
  bool Initialize(HWND parent_hwnd, int width, int height);
  void Destroy();

  // ---- 每帧渲染 ----
  void RenderFrame();

  // ---- 音频注入 ----
  void FeedPcm(const float* interleaved_lr, unsigned int frame_count);

  // ---- 预设控制 ----
  void LoadPreset(const wchar_t* filename, float blend_time);
  void NextPreset(float blend_time);
  void PrevPreset(float blend_time);
  void RandomPreset(float blend_time);

  // ---- 诊断 ----
  bool IsReady() const;
  int  GetCurrentPresetIndex() const;
  int  GetTotalPresets() const;
  std::wstring GetCurrentPresetName() const;

private:
  // D3D9 设备
  IDirect3D9*        d3d9_ = nullptr;
  IDirect3DDevice9*  device_ = nullptr;
  IDirect3DSwapChain9* swap_chain_ = nullptr;
  HWND               hwnd_ = nullptr;

  // MilkDrop3 引擎
  CPlugin*           plugin_ = nullptr;  // CPlugin 实例

  // 音频缓冲区
  std::mutex         pcm_mutex_;
  std::vector<float> pcm_buffer_;  // 交错 L/R
};

}  // namespace milkdrop3_api
```

### 6.2 Milkdrop3Module (对标 MilkdropModule)

```cpp
// 设计原则:
//   - 继承 ModulePanel (Y2K 卡片外壳)
//   - 内嵌原生 HWND 子窗口承载 D3D9 渲染
//   - 实现 AnalyserHub::FrameListener 获取 PCM
//   - 对标 MilkdropModule 的 UI 交互模式

class Milkdrop3Module : public ModulePanel,
                        public AnalyserHub::FrameListener {
public:
  explicit Milkdrop3Module(AnalyserHub* hub);
  ~Milkdrop3Module() override;

  // ModulePanel 覆写
  void paintContent(juce::Graphics& g,
                    juce::Rectangle<int> contentBounds) override;
  void layoutContent(juce::Rectangle<int> contentBounds) override;

  // FrameListener
  void onFrame(const AnalyserHub::FrameSnapshot& frame) override;

  // 用户交互
  void NextPreset();
  void PrevPreset();
  void RandomPreset();

private:
  class D3dChildWindow;  // 原生 HWND 子窗口管理
  std::unique_ptr<D3dChildWindow> d3d_window_;
  AnalyserHub* hub_;
};
```

### 6.3 与 projectM 模块的并行共存

```
ModuleWorkspace (模块管理框架)
│
├─ ModuleType::Milkdrop    → MilkdropModule  (projectM, OpenGL)
│   保持原有: ProjectMApi → LoadLibrary("projectM-4.dll")
│   Editor OpenGL 上下文 → offscreen FBO → glBlitFramebuffer
│
├─ ModuleType::Milkdrop3   → Milkdrop3Module (MilkDrop3, Direct3D 9) [新增]
│   新增: Milkdrop3Api → 编译期链接 MilkDrop3 源码
│   独立 D3D9 子窗口, 不与 Editor OpenGL 上下文冲突
│   PCM 来自 AnalyserHub → FeedPcm() → SetAudioBuf()
│
├─ ModuleType::Eq          → EqModule
├─ ModuleType::Loudness    → LoudnessModule
└─ ... 其他模块不变
```

两个模块各自独立注册、独立渲染、独立管理生命周期。projectM 的 `ModuleType::Milkdrop` 逻辑**零改动**。

---

## 7. 实现细节

### 7.1 D3D9 子窗口嵌入 JUCE Component

```cpp
// 核心思路: 在 JUCE Component 之上叠加一个原生 HWND 子窗口
// 
// 1. Milkdrop3Module::D3dChildWindow 持有原生 HWND
// 2. 使用 juce::Component::addAndMakeVisible 不可行 (原生HWND)
// 3. 使用 SetParent + SetWindowPos 将 HWND 嵌入 Component 区域
// 4. 监听父 Component 的移动/缩放事件, 同步调整 HWND 位置
//
// 关键 Windows API:
//   SetParent(d3d_hwnd, parent_component_hwnd)
//   SetWindowPos(d3d_hwnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE)
//   SetWindowLong(d3d_hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE)
```

### 7.2 PCM 数据适配

```cpp
// Y2Kmeter AnalyserHub → MilkDrop3 AudioBuf 的转换流程:
//
// 1. AnalyserHub::FrameSnapshot 提供:
//    float oscL[2048], oscR[2048]  (32-bit float, [-1, +1])
//
// 2. MilkDrop3 需要:
//    unsigned char[576] × 2  (8-bit signed stored as unsigned)
//
// 3. 转换:
//    for (int i = 0; i < 576; ++i) {
//      int sample_idx = i * 2048 / 576;  // 降采样
//      float valL = clamp(oscL[sample_idx], -1.0f, 1.0f);
//      float valR = clamp(oscR[sample_idx], -1.0f, 1.0f);
//      bufL[i] = static_cast<unsigned char>(static_cast<int8_t>(valL * 127.0f));
//      bufR[i] = static_cast<unsigned char>(static_cast<int8_t>(valR * 127.0f));
//    }
//    SetAudioBuf(pData, 576, &wf, false);
//
// 4. 每帧调用 CPlugin::PluginRender(bufL, bufR)
```

### 7.3 渲染缩放支持

对标 MilkdropModule 的渲染缩放功能 (1:1 / 1:2 / 1:4):

```cpp
// MilkDrop3 内部通过 m_nTexSizeX/Y 控制内部渲染分辨率
// 对应关系:
//   缩放 1:1 → m_nTexSizeX = -2 (nearest power of 2 to window size)
//   缩放 1:2 → 手动设置 m_nTexSizeX = window_width/2 (并同步 m_nTexSizeY)
//   缩放 1:4 → 手动设置 m_nTexSizeX = window_width/4
```

---

## 8. CMake 集成

### 8.1 源文件列表

```cmake
# ==========================================================
# MilkDrop3 (独立 D3D9 可视化引擎，BSD 3-Clause)
# ==========================================================
set(Y2KM_MILKDROP3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/milkdrop3/code")

# ns-eel2 表达式引擎
set(MD3_NS_EEL2_SOURCES
  ${Y2KM_MILKDROP3_DIR}/ns-eel2/nseel-caltab.c
  ${Y2KM_MILKDROP3_DIR}/ns-eel2/nseel-cfunc.c
  ${Y2KM_MILKDROP3_DIR}/ns-eel2/nseel-compiler.c
  ${Y2KM_MILKDROP3_DIR}/ns-eel2/nseel-eval.c
  ${Y2KM_MILKDROP3_DIR}/ns-eel2/nseel-lextab.c
  ${Y2KM_MILKDROP3_DIR}/ns-eel2/nseel-ram.c
  ${Y2KM_MILKDROP3_DIR}/ns-eel2/nseel-yylex.c
  ${Y2KM_MILKDROP3_DIR}/ns-eel2/asm-nseel-x86-msvc.c
)

# 核心渲染引擎 (vis_milk2)
set(MD3_VIS_MILK2_SOURCES
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/plugin.cpp
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/pluginshell.cpp
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/state.cpp
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/dxcontext.cpp
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/fft.cpp
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/texmgr.cpp
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/support.cpp
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/utility.cpp
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/textmgr.cpp
  ${Y2KM_MILKDROP3_DIR}/vis_milk2/menu.cpp
)

# 音频缓冲区 (仅保留 audiobuf)
set(MD3_AUDIO_SOURCES
  ${Y2KM_MILKDROP3_DIR}/audio/audiobuf.cpp
)
```

### 8.2 include 路径与链接

```cmake
target_include_directories(Y2Kmeter PRIVATE
  ${Y2KM_MILKDROP3_DIR}
  ${Y2KM_MILKDROP3_DIR}/vis_milk2
  ${Y2KM_MILKDROP3_DIR}/ns-eel2
  ${Y2KM_MILKDROP3_DIR}/audio
)

target_link_libraries(Y2Kmeter PRIVATE
  d3d9
  d3dx9
)

target_compile_definitions(Y2Kmeter PRIVATE
  MD3_Y2KMETER=1       # 标识在 Y2Kmeter 环境内编译
)
```

---

## 9. 安装器更新

### 9.1 D3DX9 运行时

MilkDrop3 依赖 `d3dx9_43.dll` (HLSL Shader 编译)。该 DLL 是 DirectX End-User Runtime 的一部分，大多数现代 Windows 系统已预装。为确保兼容性：

```iss
; Y2Kmeter_installer.iss 新增:
; Standalone: 删除旧 MilkDrop3 DLL (如果有)
Type: files; Name: "{app}\d3dx9_43.dll"; Components: standalone

; 可选: 如果用户系统没有 D3DX9, 提示安装 DirectX Runtime
; 大多数 Windows 10/11 已自带
```

### 9.2 MilkDrop3 预设目录

```iss
; 预设和纹理存放在 %APPDATA%\Y2Kmeter\milkdrop3\
; 与现有 milkdrop_presets/ 分开管理
; ZIP 打包方式对标 Milkdrop 预设的安装模式
```

---

## 10. 关键决策记录

| 日期 | 决策 | 原因 |
|------|------|------|
| 2026-07-31 | 采用独立 D3D9 子窗口而非 OGL 移植 | MilkDrop3 深度绑定 D3D9/HLSL, 重写成本过高 |
| 2026-07-31 | 保留 CPlugin/CPluginShell 不做大重构 | 355KB 单文件, 重构风险极高; 采用宏条件编译最小化改动 |
| 2026-07-31 | audio/ 子系统完全移除, 改用 AnalyserHub | Y2Kmeter 已有完整的音频分析管线, 避免重复 |
| 2026-07-31 | 不使用 Spout/共享纹理跨 API | D3D 子窗口直接覆盖显示, 无需纹理共享 |
| 2026-07-31 | 预设目录独立管理 (milkdrop3/ vs milkdrop_presets/) | `.milk2` 格式互不兼容, 分开管理避免混淆 |

---

## 11. 实现难点与解决方案

### 11.1 D3D9 窗口嵌入 JUCE

**难点**: JUCE Component 使用自己的 HWND 层级, 原生 D3D9 子窗口需要正确处理 z-order 和焦点。

**方案**: 
- 使用 `WS_CHILD` 样式 + `SetParent()` 嵌入
- 在 `Component::moved()`/`resized()` 回调中同步 HWND 位置
- 鼠标事件: 原生 HWND 捕获后通过 `SendMessage` 转发给父 Component

### 11.2 HLSL Shader 编译

**难点**: `D3DXCompileShader` 在 `d3dx9_43.dll` 中, 需要确保系统安装了该 DLL。

**方案**: 检测 DLL 可用性, 不可用时提供用户友好的错误提示 (类似 projectM 的兜底面板)。

### 11.3 FPS 同步

**难点**: D3D9 窗口有独立的渲染循环, 需要与 JUCE 的 UI 线程帧率同步。

**方案**: Y2Kmeter 的 60fps 帧驱动 (AnalyserHub startFrameDispatcher) → 在 `onFrame()` 中调用 `Api::RenderFrame()`。

---

## 12. 文件变更清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `docs/MilkDrop3_Integration.md` | 本集成文档 |
| `source/ui/modules/Milkdrop3Api.h` | MilkDrop3 引擎 API 封装 |
| `source/ui/modules/Milkdrop3Api.cpp` | MilkDrop3 引擎 API 实现 |
| `source/ui/modules/Milkdrop3Module.h` | MilkDrop3 模块面板 |
| `source/ui/modules/Milkdrop3Module.cpp` | MilkDrop3 模块面板实现 |
| `third_party/milkdrop3/` (整个目录) | MilkDrop3 引擎源码 |

### 修改文件

| 文件 | 变更内容 |
|------|---------|
| `CMakeLists.txt` | 添加 MilkDrop3 源文件编译、include 路径、d3d9/d3dx9 链接 |
| `Y2Kmeter_installer.iss` | 添加 d3dx9 运行时检查 + 预设解压逻辑 |
| `PluginEditor.h/.cpp` | (如需要) 注册 Milkdrop3 模块类型 |
| `ModuleWorkspace.h/.cpp` | 注册 `ModuleType::Milkdrop3` |
| `README.md` | 添加 MilkDrop3 集成说明与版权声明 |

---

## 附录 A: 参考链接

- [MilkDrop3 仓库](https://github.com/milkdrop2077/MilkDrop3)
- [BeatDrop 仓库](https://github.com/mvsoft74/BeatDrop) (MilkDrop3 上游)
- [projectM 仓库](https://github.com/projectM-visualizer/projectm) (现有引擎)
- [MilkDrop3 DeepWiki - Architecture](https://deepwiki.com/milkdrop2077/MilkDrop3/3-architecture)
- [MilkDrop3 DeepWiki - Rendering Pipeline](https://deepwiki.com/milkdrop2077/MilkDrop3/3.4-rendering-pipeline)
- [MilkDrop3 DeepWiki - Audio Pipeline](https://deepwiki.com/milkdrop2077/MilkDrop3/3.3-audio-pipeline)

## 附录 B: 实际实现详情

### B.1 新增文件（Y2Kmeter 侧）

| 文件 | 行数 | 说明 |
|------|:---:|------|
| `source/ui/modules/Milkdrop3Api.h` | ~180 | 引擎封装头文件，对标 ProjectMApi.h |
| `source/ui/modules/Milkdrop3Api.cpp` | ~320 | 引擎封装实现：D3D9 设备管理 + CPlugin 生命周期 + PCM 注入 |
| `source/ui/modules/Milkdrop3Module.h` | ~90 | 模块面板头文件，继承 ModulePanel + FrameListener |
| `source/ui/modules/Milkdrop3Module.cpp` | ~250 | 模块面板实现：D3D9 子窗口嵌入 + 帧驱动 |
| `third_party/d3dx9_headers/` | 18 文件 | D3DX9 头文件 + x64 导入库（来自 Microsoft.DXSDK.D3DX NuGet 包 v9.29.952.8），含 `*.h` (12) + `*.inl` (3) + `lib/x64/` (3 .lib) |
| `docs/MilkDrop3_Integration.md` | ~640 | 本集成文档 |

### B.2 修改的 MilkDrop3 引擎源码（共计 14 处，功能修改受 `#ifdef MD3_Y2KMETER` 或 `#ifdef _M_IX86` 保护；变量声明补全/内联汇编屏蔽/缺失文件补全为 MSVC x64 上游兼容性修复）

| 文件 | 修改内容 | 原因 |
|------|---------|------|
| `vis_milk2/pluginshell.h` | ① `m_hInstance`/`m_lpDX`/`m_szPluginsDirPath`/`m_szConfigIniFile`/`m_szConfigIniFileA` 提升为 `protected`; ② `AllocateDX9Stuff()` / `CleanUpDX9Stuff()` 移入 `protected:`; ③ 新增 `SetY2KPaths()` 方法 | API 封装层需注入 Y2Kmeter 路径和访问 D3D9 重置方法 |
| `vis_milk2/pluginshell.cpp` | ① `PluginPreInitialize()` 中 `GetModuleFileNameW` 推导包裹 `#ifndef MD3_Y2KMETER`；② 上游兼容性：补全 10 处缺少 `int` 的循环变量（`i`/`ch`/`octave`/`n`） | 跳过 Winamp DLL 目录推导；MSVC x64 严格模式要求 C++ 标准作用域规则 |
| `vis_milk2/plugin.cpp` | ① `MyPreInitialize()`/`FindValidPresetDir()` 包裹 `#ifdef MD3_Y2KMETER`；② 上游兼容性：补全 20+ 处缺少 `int` 声明的循环变量，`WPARAM`→`(int)`，`DWORD`→`DWORD_PTR`，格式化参数修正等 | 路径拦截 + MSVC x64 严格模式兼容 |
| `vis_milk2/milkdropfs.cpp` | **新增到 CMake**（原被遗漏，导致 `CPlugin::RenderFrame` / `UvToMathSpace` / `LoadPerFrameEvallibVars` / `LoadCustomWavePerFrameEvallibVars` / `LoadCustomShapePerFrameEvallibVars` 5 个符号链接缺失）；待后续轮次修复内含的 `__asm`（3 处）及 `for(i=`（数十处）变量声明 | LNK1120 主因之一：核心渲染实现文件未参与编译 |
| `vis_milk2/state.cpp` | 上游兼容性：补全 16 处缺少声明的循环/局部变量（`i` 外提 2 处 + 内联 6 处 + `vi` 内联 7 处） | MSVC x64 严格模式要求 C++ 标准作用域规则；连带消除 C4473 警告 |
| `vis_milk2/textmgr.cpp` | 上游兼容性：① `i`/`j` 声明从 else 块内提至 if/else 前（消除 C2065 + 连带 C2660）；② `(DWORD)`→`(DWORD_PTR)` 消除 x64 C4311（第 104/136/188 行） | MSVC x64 严格模式 + 64 位兼容性 |
| `vis_milk2/menu.cpp` | 上游兼容性：`i` 声明从 `for(int i=...)` 回退为外提 `int i; for(i=...)`（`EnableItem` 及 `DrawMenu` 各 1 处），因 while 循环后需继续访问 `i` | MSVC x64 严格模式要求 C++ 标准作用域规则 |
| `vis_milk2/utility.cpp` | x64 兼容性：① `CheckForMMX()` 用 `#ifdef _WIN64` 包裹 `__asm` CPUID 块（x64 直接 return true），消除 C4235 + 连带错误；② `GWL_WNDPROC`→`GWLP_WNDPROC` 消除 C2065 | MSVC x64 不支持 `__asm` + 32-bit 窗口宏已废弃 |
| `vis_milk2/wasabi.h` | 注释更新，接口不变 | Winamp 资源 API stub 说明 |
| `vis_milk2/wasabi.cpp` | `#ifdef MD3_Y2KMETER` 下提供 stub 实现（`[MD3:id]` 格式） | 移除 `api_orig_hinstance` + `LoadStringW` 依赖 |
| `ns-eel2/nseel-compiler.c` | `__floor` → `eel_floor`（定义 + 引用各一处） | MSVC 将 `__floor` 视为 intrinsic，禁止用户重定义或取地址 |
| `ns-eel2/asm-nseel-x86-msvc.c` | 整体 `#ifdef _M_IX86` 包裹 x86 汇编；`#else` 提供 50+ 个空 stub（`DECL_STUB` 宏）+ `win64_callcode(INT_PTR)` 空 stub | MSVC x64 不支持 `__asm` / `__declspec(naked)`；空 stub 使 EEL2 回退到纯解释模式 |
| `source/ui/modules/Milkdrop3Api.cpp` | 新增 `g_use_C_locale` 和 `keyMappings` 全局变量定义（原定义于 Winamp 插件入口 `Milkdrop2PcmVisualizer.cpp`） | LNK1120：`utility.h` 中 `extern` 声明需 Y2Kmeter 侧提供实现 |

### B.3 Y2Kmeter 侧全局符号定义

[Y2K项目下的/Milkdrop3Api.cpp](I:/Y2KMeter/source/ui/modules/Milkdrop3Api.cpp) 文件顶部定义了下述全局符号，供 MilkDrop3 引擎内部 `extern` 引用：

```cpp
CPlugin g_plugin;                         // menu.cpp → extern CPlugin g_plugin
HINSTANCE api_orig_hinstance = nullptr;   // utility.cpp → extern HINSTANCE api_orig_hinstance
_locale_t g_use_C_locale;                // utility.h → extern _locale_t g_use_C_locale
char keyMappings[8];                     // utility.h → extern char keyMappings[8]
```

原版在 Winamp 的 `main.cpp` / `Milkdrop2PcmVisualizer.cpp` 中定义；Y2Kmeter 侧由 `Milkdrop3Api.cpp` 提供。

### B.4 修改的 Y2Kmeter 已有文件

| 文件 | 修改内容 |
|------|---------|
| `source/ui/ModuleWorkspace.h` | 枚举添加 `ModuleType::milkdrop3` |
| `source/ui/ModulePanel.cpp` | `getModuleDisplayName()` 添加 `"MilkDrop3"` |
| `source/ui/ModuleWorkspace.cpp` | `moduleTypeToString()` / `stringToModuleType()` 添加 `milkdrop3` |
| `PluginEditor.cpp` | ① `#include` Milkdrop3Module.h; ② 模块工厂添加 `Milkdrop3Module` 创建 |
| `CMakeLists.txt` | ① `Y2KM_MILKDROP3_DIR` 变量 + 存在性检测; ② `MD3_Y2KMETER=1` 编译宏; ③ 引擎源码 (vis_milk2 + ns-eel2 + audio) + 模块源码; ④ `d3d9` + `find_library(d3dx9)` 链接; ⑤ include 路径 (含 `third_party/d3dx9_headers/`); ⑥ 构建时复制 `.fx` shader 到 `data/`; ⑦ `find_path(d3dx9.h)` 自动搜索（项目内嵌 → 环境变量 → 旧版 SDK） |

### B.5 与 projectM 模块并行共存验证

```
✅ projectM 模块 (ModuleType::milkdrop):
   ProjectMApi.h/cpp           —— 未修改
   MilkdropModule.h/cpp        —— 未修改
   PluginEditor.h (milkdrop_*) —— 未修改
   third_party/projectm/       —— 未修改
   CMake (projectM 部分)       —— 未修改

✅ MilkDrop3 模块 (ModuleType::milkdrop3):
   Milkdrop3Api.h/cpp          —— 新增
   Milkdrop3Module.h/cpp       —— 新增
   third_party/milkdrop3/      —— 新增 (5 处源文件修改，均受 MD3_Y2KMETER 保护)
   CMake (MD3 部分)            —— 新增
```

### B.6 编译前必备步骤

1. **确保 MilkDrop3 源码已克隆**:
   ```bash
   git clone --depth 1 https://github.com/milkdrop2077/MilkDrop3.git third_party/milkdrop3
   ```
   CMake 会在 configure 阶段检测 `third_party/milkdrop3/code/vis_milk2/plugin.h` 是否存在，若缺失则 `FATAL_ERROR`。

2. **D3DX9 头文件、内联实现与导入库已内嵌**: `third_party/d3dx9_headers/` 目录包含：
   - `*.h`（12 个 D3DX9 头文件）
   - `*.inl`（3 个内联实现：`d3dx9math.inl`、`D3DX10math.inl`、`D3DX_DXGIFormatConvert.inl`）
   - `lib/x64/d3dx9.lib`（x64 Release 导入库）
   
   均提取自 Microsoft.DXSDK.D3DX NuGet 包 v9.29.952.8（MS-LPL 许可，允许再分发）。CMake 优先从该目录搜索，无需单独安装 DirectX SDK。

3. **D3DX9 链接库**: `d3dx9.lib` 导入库已随 Windows SDK 提供，无需额外安装。运行时需要 `d3dx9_43.dll`（Windows 10/11 通常已预装）。

4. **Shader 文件自动复制**: CMake 构建时自动将 `resources/Milkdrop2/data/*.fx`（共 8 个文件）复制到 `${CMAKE_BINARY_DIR}/data/`。运行时引擎通过 `m_szMilkdrop2Path + \"data\\\\*.fx\"` 查找。

3. **预设目录**: 运行时自动创建 `%APPDATA%\Y2Kmeter\milkdrop3_presets\`。用户需将 `.milk` / `.milk2` 文件放入该目录。

4. **textures 目录**: 运行时自动创建 `{exe_dir}\textures\`。精灵纹理（`.jpg`/`.png`）需放入该目录。

### B.7 已知限制与后续待办

1. **D3D9 Device Lost 处理**：MilkDrop3 的 `PluginRender` 内部已有 `TestCooperativeLevel` 逻辑处理设备丢失。但在 JUCE 组件被遮挡/最小化场景下的行为待测试验证。

2. **D3DX9_43.dll 依赖**：HLSL Shader 编译需要 `d3dx9_43.dll`。Windows 10/11 通常已预装，但极少数干净系统可能缺失。后续可在安装器中添加检测和提示。

3. **预设格式兼容性**：MilkDrop3 支持 `.milk2` 双预设格式。在 Y2Kmeter 模块中的预设交互（前后切换、随机、跳转）目前使用 MilkDrop3 内置接口，后续可添加专属 UI 控制栏（对标 `MilkdropModule` 的 overlay 交互）。

4. **首次预设加载**：模块初始化后需等待 `UpdatePresetList` 后台线程扫描完预设目录后才会加载第一个预设。初始几帧可能显示黑屏。

5. **D3D9 子窗口鼠标事件**：当前仅转发基本鼠标消息（单击/拖动/滚轮）。复杂交互（右键菜单、键盘事件）需后续完善。右键点击默认映射为 NextPreset。

6. **ns-eel2 编译器警告**：EEL2 表达式编译器（C 语言实现）在 MSVC 下可能产生 C4244/C4267 等截断警告，源自上游代码风格，不影响功能。

## 附录 C: 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 0.1 | 2026-07-31 | 初稿: 源码获取、架构分析、剥离方案、封装设计 |
| 0.2 | 2026-07-31 | 实现阶段: API 封装、模块创建、CMake 集成、源码适配 |
| 0.3 | 2026-07-31 | 编译修复: MyPreInitialize/FindValidPresetDir MD3_Y2KMETER guard、wasabi stub、g_plugin 全局定义、.fx shader 构建复制、路径设置完善 |
| 0.4 | 2026-07-31 | D3DX9 环境修复: 内嵌 12 个 D3DX9 头文件到 `third_party/d3dx9_headers/`; CMake `find_path` 自动搜索链（项目内嵌 → 环境变量 → 旧版 SDK）; `#define NOMINMAX` 解决 `std::max` 宏冲突 |
| 0.5 | 2026-07-31 | D3DX9 链接修复: 内嵌 x64 `d3dx9.lib`/`d3dx10.lib`/`d3dx11.lib` 到 `third_party/d3dx9_headers/lib/x64/`; CMake `find_library` 搜索链新增项目内嵌路径作为第一优先级; 移除无效的 fallback `d3dx9` 裸名称方案 |
| 0.6 | 2026-07-31 | 编译修复: ① `Milkdrop3Api.h` 第 31 行 `/* 错误处理 */` 内嵌 `*/` 提前关闭外层 `/*` 块注释导致 C2059, 改为移除内嵌注释; ② 补全 `d3dx9math.inl`/`D3DX10math.inl`/`D3DX_DXGIFormatConvert.inl` 三个内联实现文件到 `third_party/d3dx9_headers/` |
| 0.8 | 2026-07-31 | x64 编译修复: ① `asm-nseel-x86-msvc.c` 整体 `#ifdef _M_IX86` 包裹 + x64 空 stub（50+ 符号），MSVC x64 不支持 `__asm`/`__declspec(naked)`，EEL2 回退到纯解释模式；② CMake Release 移除 `/Ob3`（与 MSVC 默认 `/Ob2` 冲突导致 D9025 警告） |
| 0.9 | 2026-07-31 | **plugin.cpp 上游兼容性修复（两轮）**: ① 变量声明补全——补全 `for (i=0;...)`/`for (x=0;...)`/`for (y=0;...)`/`for (z=0;...)`/`for (mash=...)`/`for (pass=...)` 等共 20+ 处缺少 `int` 声明的循环变量，以及 `i = rand() %` 改为 `int i = rand() % static_cast<int>(...)`（MSVC 严格模式要求 C++ 标准作用域规则）；② 行继续符——移除第 1128 行注释末尾 `\` 防止预处理器吞掉 `GetModuleFileNameW` 声明；③ 格式化参数——第 4850 行 `swprintf` 的 `m_presets[idx].szFilename`（`std::wstring`）加 `.c_str()`；`sprintf`/`swprintf` 对 `WPARAM` 加 `(int)` 转型（第 5219/5259 行）；`sprintf` 宽字符串字面量 `L""`→`""`（第 8814 行）；④ 指针截断——`__UpdatePresetList` 及调用侧 `DWORD`→`DWORD_PTR` 消除 x64 警告 C4311/C4312（第 7441/7776 行） |
| 0.10 | 2026-07-31 | **hotfix 六则**: ① 回退 v0.9 中第 4850 行对 `wasabiApiLangString()` 的误加 `.c_str()`，仅保留 `szFilename`（`std::wstring`）的 `.c_str()`；② **pluginshell.cpp** 补全 10 处缺少 `int` 声明的循环变量（`i`/`ch`/`octave`/`n`）；③ **state.cpp** 补全 16 处变量声明（`i` 外提 2 处 + 内联 13 处 + `vi` 内联 7 处）；④ **textmgr.cpp** 修复 `i`/`j` 作用域 + `(DWORD)`→`(DWORD_PTR)`；⑤ **menu.cpp** `i` 声明从 `for(int i=...)` 改为外提 `int i; for(i=...)`（`EnableItem`/`DrawMenu` 各 1 处）；⑥ **utility.cpp** x64 修复两则——(a) `CheckForMMX()` 用 `#ifdef _WIN64` 包裹 `__asm` CPUID 块（x64 直接 return true），(b) `GWL_WNDPROC`→`GWLP_WNDPROC` 消除 C2065 |
| 0.11 | 2026-07-31 | **LNK1120 链接修复**: ① `Milkdrop3Api.cpp` 补全 `g_use_C_locale` / `keyMappings` 全局变量定义（原定义于 Winamp 插件入口 `Milkdrop2PcmVisualizer.cpp`，`utility.h` 中 `extern` 声明）；② `asm-nseel-x86-msvc.c` 的 `#else`（x64 stub）块补全 `win64_callcode(void*)` 空 stub（初始误用 `INT_PTR`→后续改为 `void*`，因该文件无 `#include <windows.h>`，`INT_PTR` 不可用，`nseel-compiler.c` 的 `extern` 声明为 C 链接，参数类型差异不影响 ABI）；③ `CMakeLists.txt` 新增 `milkdropfs.cpp`（含 `CPlugin::RenderFrame` / `UvToMathSpace` / `LoadPerFrameEvallibVars` / `LoadCustomWavePerFrameEvallibVars` / `LoadCustomShapePerFrameEvallibVars`） |
| 0.12 | 2026-07-31 | **milkdropfs.cpp x64 编译修复**: ① `RestoreFPCW()` 的 `__asm fldcw wSave` 用 `#ifdef _M_IX86` 包裹（`MungeFPCW` 中同样代码已被 `#if 0` 禁用，无需处理）；② `for(i/j/k/x/y=` 共 ~60+ 处缺少 `int` 声明的循环变量，通过 `sed` 批量替换 `for (VAR=` → `for (int VAR=`（变量: i/j/k/x/y） |
| 0.13 | 2026-07-31 | **CMake 变量名拼写不一致 → copy_directory 源路径为空**: `CMakeLists.txt` 第 385~386 行定义 `Y2KM_MILKDROP3_PRESETS` / `Y2KM_MILKDROP3_TEXTURES`（带 `3`），但 `y2km_copy_projectm_runtime()` 函数第 951/961 行引用 `${Y2KM_MILKDROP_PRESETS}` / `${Y2KM_MILKDROP_TEXTURES}`（缺 `3`），CMake 将未定义变量解析为空字符串，导致 `cmake -E copy_directory "" ...` 报错并中止 NMAKE。修复：将第 385~386 行变量名去掉 `3`，改为 `Y2KM_MILKDROP_PRESETS` / `Y2KM_MILKDROP_TEXTURES`。 |
| 0.14 | 2026-07-31 | **Module 注册 + 着色器部署 + 渲染循环 + 错误可读化 四连修**: ① `PluginEditor.cpp`/`ModuleWorkspace.h` 的 `availableTypes` 列表补齐 `ModuleType::milkdrop3`，使牛奶3模块出现在右键"添加"菜单中；② `CMakeLists.txt` 新增 Y2Kmeter_Standalone 的 post-build `copy_directory data/*.fx → EXE输出目录/data/`，确保 `ReadFileToString("data\\include.fx")` 能找到着色器文件（此前 CMake 仅复制到 `${CMAKE_BINARY_DIR}/data/`，而引擎在 EXE 目录查找 → MessageBox `[MD3:466]` 弹框阻断）；③ `wasabi.cpp` 中 `wasabiApiLangString` 改为返回真实英文文本（通过 `Md3LookupString` 映射 resource.h ID → 字符串），不再显示 `[MD3:466]` 类无意义编号；④ `Milkdrop3Api::Initialize_PluginInit()` 中 pre-flight 检查 `data/include.fx` 是否存在，不存在则提前返回有诊断价值的 error_message_ 而非进入 PluginInitialize 触发阻塞式 MessageBox；⑤ `Milkdrop3Module::paintContent()` 初始化态填黑底色 + 右下角显示预设计数；⑥ `Milkdrop3Module::timerCallback()` 新增渲染分支：`initialized_=true` 后持续按 ~60fps 调用 `api_.RenderFrame()`，解除对音频帧（`onFrame`）的依赖；⑦ Phase 5 完成后 `SetWindowPos(HWND_TOP)` 确保 D3D9 子窗口不被 JUCE 组件绘制覆盖。 |
| 0.15 | 2026-08-04 | **渲染画面位置偏移 Bug 最终修复（v19）**：定位根因为"JUCE 逻辑 DIP 被直接当作物理像素坐标传给 Win32 API"，在 PMv2 感知进程中导致偏移量随离原点距离线性累积（越靠右下越偏移，右侧屏比左侧屏偏移大）。修改 [Milkdrop3Module.cpp](I:/Y2KMeter/source/ui/modules/Milkdrop3Module.cpp) 三处：① `D3dChildWindow::CreateHWNDOnly()` 使用 `juce::Desktop::getInstance().getDisplays().logicalToPhysical(Rectangle)` 将逻辑 DIP 转为物理像素后再传给 `CreateWindowExW`，物理尺寸也用该 API 结果；② `D3dChildWindow::Reposition()` 同上转换，每次 `SetWindowPos` 均走物理像素路径；③ `timerCallback()` phase=-1 的 DPI 修正改用 `logicalToPhysical`，替代原先 `GetDpiForWindow(parent_hwnd) × 逻辑尺寸`（避免跨屏后 DPI 取值不一致）；④ `MD3_BUILD_TAG` 更新为 `v19_20260804_LogicalToPhysical_Coord_Fix`。经用户测试确认：拖至任意屏幕任意位置，偏移全部消失（或仅剩 ≤ 1 px 舍入误差）。详见附录 D.2。 |
| 0.16 | 2026-08-04 | **外部 Spectrum 接入 + UI 交互 + 死代码清理（v20）**：① `pluginshell.h/cpp` 引擎侧新增 `m_bY2kExternalSpectrumValid + m_y2kExternalSpectrum[2][NUM_FREQUENCIES]`，`AnalyzeNewSound` 一次性覆盖内建 FFT 结果；② `Milkdrop3Api` 新增 `FeedSpectrum / AddPreRenderInjector / RemovePreRenderInjector / EnablePresetInfoOverlay / ShowPresetTitleAnim` 五个接口，删除 `Initialize / CreateRenderWindow / SetPresetDir / CycleRenderScale / ApplyRenderScale / render_scale_` 等死代码；③ `Milkdrop3Module` 补上 `addFrameListener(this)`（此前从未注册，引擎一直用零向量渲染）、在 `onFrame` 中做加锁快照、初始化完成后注册 pre-render injector 集中投喂 PCM + Spectrum；④ UI 交互重构：使用引擎自带 `m_bShowPresetInfo`（D3D9 surface 右上角常显预设名）+ `LaunchSongTitleAnim`（切预设时中央大字弹出），JUCE 标题栏新增 `< ? > i` 四个按钮，全部左键，右键回退到基类默认；⑤ `D3dChildWindow::WndProc` 直接处理 `WM_LBUTTONDOWN → NextPreset()`，不再向 JUCE 父窗口转发；⑥ 清理 v19 遗留：`Md3BuildTagLogger` / `MD3_BUILD_TAG` / `MonitorFromPoint` 探测日志 / 多处冗余 `SetThreadDpiAwarenessContext` / 仅用于日志的 `GetDpiForWindow` / kDpiScale 二次算，只保留错误路径的 MD3_LOG；⑦ `ConvertPcmToMd3` 改用 `unsigned centered at 128` 消除 UB。详见附录 D.3。 |

---

## 附录 D: 渲染画面位置偏移 Bug — 修复记录

> **问题摘要**：MilkDrop3 模块的 D3D9 渲染画面早期一直相对模块正确位置向左上方
> 偏移；偏移量在 v14 之前固定于模块创建时的物理屏幕坐标，v15 之后（进程转 PMv2）
> 变为随模块拖动实时缩放变化。经过 v6–v18 共 18 轮的窗口风格、DPI 感知、呈现
> 模型等多维度尝试均未根除，最终在 **v19** 通过引入 JUCE
> `logicalToPhysical()` 坐标转换消除。本附录保留 v19 修复的完整根因分析与
> 早期尝试的高层总览，供后续多屏 + 混合 DPI 场景排查参考。

---

### D.1 轮次总览

| 轮次 | BUILD_TAG | 核心修改 | 偏移现象 | Z 序 |
|:----:|-----------|---------|:--------:|:----:|
| v6–v9 | (早期) | DPI fixup、诊断日志、`Shcore.lib`、`WS_POPUP` 尝试 | 固定于创建时 | ❌ |
| v10 | `v10_WSCHILD_ClipChildren_CopySwap` | 回到 `WS_CHILD` + `WS_CLIPCHILDREN` + `COPY` | 同上 | ✅ |
| v11 | `v11_OwnedPopup_GetAncestor` | owned `WS_POPUP` + `GetAncestor(GA_ROOT)` | 固定 | ✅ |
| v12 | `v12_UnownedPopup_HWND_TOP` | unowned `WS_POPUP` + 手动 `HWND_TOP` 轮询 | 固定 | ❌ 延迟恢复 |
| v13 | `v13_PerMonitorV2_hDestOverride` | `SetThreadDpiAwarenessContext(PMv2)` + `hDestWindowOverride` | 固定 | ❌ 同上 |
| v14 | `v14_OnResize_Reset_PMv2_context` | `OnResize::Reset` 包裹 PMv2 | 固定 | ❌ |
| v15 | `v15_ProcessPMv2_ZOrderFix` | `SetProcessDpiAwarenessContext(PMv2)` + owned popup | **动态缩放** | ✅ |
| v16 | `v16_NoDestWndOverride` | 移除 `hDestWindowOverride` | 动态缩放 | ✅ |
| v17 | `v17_NoToolWindow_FrameChanged` | 移除 `WS_EX_TOOLWINDOW` / 添加 `SWP_FRAMECHANGED` | 动态缩放 | ✅ |
| v18 | `v18_SwapEffect_DISCARD_DPI_Diag` | `D3DSWAPEFFECT_COPY`→`DISCARD` + DPI 诊断日志 | 无变化 | ✅ |
| **v19** | **`v19_LogicalToPhysical_Coord_Fix`** | **JUCE 逻辑 DIP → Win32 物理像素坐标转换** | **✅ 消除** | ✅ |

**关键转折点**：v15 的 `SetProcessDpiAwarenessContext(PMv2)` 是唯一改变偏移行为的修改——从"固定于创建时"变为"动态随拖动缩放"，证明进程 DPI 上下文确实影响 DWM 合成。**v19 通过 `juce::Displays::logicalToPhysical()` 将 JUCE 逻辑 DIP 转为 Windows 物理像素后再传入 Win32 API，彻底消除偏移。**

---

### D.2 第 19 轮：坐标空间根因定位（v19 —— 偏移 Bug 已消除）

**BuildTag**：`v19_20260804_LogicalToPhysical_Coord_Fix`

#### D.2.1 Bug 现象回顾

| 维度 | 现象 |
|------|------|
| 偏移方向 | 恒向左上方 |
| 与模块位置的关系 | 越靠近界面右下角，偏移量越大；越靠近左上角，偏移量越小 |
| 与物理屏幕距离的关系 | 三屏幕场景下**左侧屏偏移小、右侧屏偏移大**（越靠右的屏幕渲染画面越靠左） |
| 影响 | 视频渲染位置与模块布局不匹配，且跨屏幕时偏移差异显著 |

#### D.2.2 根本原因（Root Cause）

**代码将 JUCE 的"逻辑坐标（DIP）"直接传给了 Windows Win32 API（`CreateWindowExW` / `SetWindowPos`），但在 PerMonitorV2（PMv2）进程中这些 API 期望的是物理像素坐标。**

两种坐标系的差异：

- **JUCE 逻辑坐标（DIP）**：由 `Component::localPointToGlobal()`、`getScreenPosition()` 等 API 返回，位于 JUCE `Desktop::Displays` 维护的逻辑坐标空间中——各屏幕按逻辑（post-scaling）尺寸横向拼接。
- **Windows 物理像素坐标**：PMv2 感知进程中，`CreateWindowExW` / `SetWindowPos` 的 `x/y` 参数以及 `GetWindowRect` 的返回值都以物理像素为单位。

**两者仅在主屏 100% 缩放且所有屏幕缩放一致时才恰好相等。** 只要存在任一屏幕缩放 ≠ 100%，两者关系为：

```
physical = logicalToPhysical(logical, targetDisplay)
```

差值大致等于 `logical × (scale − 1)`，即**离逻辑原点越远、目标屏缩放差异越大，偏移量越大**——恰好对应本 bug 报告的"越靠右下越偏移、右侧屏偏移比左侧屏大"的规律。

#### D.2.3 为什么之前 18 轮排查未定位

1. **日志中所有窗口坐标都是"同一种"逻辑坐标，看起来对齐**：
   - `owner_.getScreenPosition()`、`localPointToGlobal()` 返回逻辑 DIP；
   - `GetWindowRect(popup)` 也返回同样的数值（因为 popup 是用逻辑 DIP 创建的，Windows 只是把这些数字"如实"存了下来）；
   - `hDeviceWindow` 屏幕坐标同上；
   - 三个观测值**互相吻合**，让排查方向误判为"坐标已对齐、偏差在 DWM 合成层"。

2. **实际 D3D9 swap chain 落在"物理坐标位置"**：DWM 使用 popup HWND 的物理坐标进行合成，而该物理坐标 = 我们传入的"逻辑 DIP 数值"被 Windows 视作物理像素解释，与真正应处的物理位置存在缩放误差。

3. **v15 前后偏移由"固定"变为"随距离缩放"**：
   - v15 之前进程未显式声明 PMv2，Win32 API 内部对逻辑/物理坐标的处理不严格，偏移表现为固定值；
   - v15 起进程级 `SetProcessDpiAwarenessContext(PMv2)` 生效后，Win32 API 开始严格按物理像素解释入参，误差随距离原点距离线性增长，偏移变为动态缩放形式——这也正是"进程 PMv2 化"这一改动**唯一改变偏移行为**的原因。

#### D.2.4 修改内容

**涉及文件**：[Milkdrop3Module.cpp](I:/Y2KMeter/source/ui/modules/Milkdrop3Module.cpp)

**核心思路**：在把坐标交给 Win32 API 之前，一律使用 JUCE 自带的转换：

```cpp
juce::Rectangle<int> physRect =
    juce::Desktop::getInstance().getDisplays()
        .logicalToPhysical(juce::Rectangle<int>(logicalGlobalPt.x,
                                                logicalGlobalPt.y, w, h));
```

该 API 会根据**目标点所在屏幕**的真实 DPI 缩放，把逻辑 DIP 精确转换为物理像素，天然覆盖多屏幕不同缩放比的场景。

| 修改点 | 变更内容 |
|--------|----------|
| `D3dChildWindow::CreateHWNDOnly()` | ① `localPointToGlobal(...)` 返回的逻辑 DIP 用 `Displays::logicalToPhysical(Rectangle)` 一次性转换为物理坐标+物理尺寸；② `CreateWindowExW` 使用转换后的 `physRect.getX()/getY()/getWidth()/getHeight()`；③ 保留原有 `GetDpiForMonitor` 仅作诊断日志，不再参与尺寸计算（避免与 `logicalToPhysical` 结果不一致） |
| `D3dChildWindow::Reposition()` | 同上转换：每次拖动/缩放触发的 `SetWindowPos` 也走 `logicalToPhysical` 路径，确保物理像素坐标 |
| `timerCallback()` phase=-1 的 DPI 修正 | 改用 `logicalToPhysical(Rectangle)` 重算 `init_phys_w_/init_phys_h_`，替代原先的 `GetDpiForWindow(parent_hwnd) × 逻辑尺寸`。原因：`parent_hwnd` 所在屏幕的 DPI 与"模块 top-left 所在屏幕"的 DPI 可能不同（模块被拖到相邻屏时），沿用父窗口 DPI 会导致 BackBuffer 尺寸与目标屏不匹配。使用 `logicalToPhysical` 与 `CreateHWNDOnly` / `Reposition` 共用同一目标屏，保证三者结果一致 |
| `MD3_BUILD_TAG` | 更新为 `v19_20260804_LogicalToPhysical_Coord_Fix`，便于日志核对二进制版本 |

**日志增强**：`CreateHWNDOnly` / `Reposition` 中新增 `logical(x,y) → physical(x,y, w×h)` 一行，便于观测转换是否发生。

#### D.2.5 验证情况

- 编译运行后，将 milkdrop3 模块拖到不同屏幕、不同位置，各屏偏移**已全部消失**（或仅剩 ≤ 1 px 的舍入误差）；
- 日志中新增的 `CreateHWNDOnly: ... → physical(X, Y, W×H)` 值与 `Reposition` 后 `GetWindowRect` 报告的物理坐标完全一致；
- **用户已确认 bug 已修复**。

#### D.2.6 备注

按协作规则，本次修复：

- 暂不升级整体版本号、暂不做 git commit / push；
- 待测试稳定、用户确认可提交后，再执行 +0.0.1 版本升级并同步至 [PROJECT_OVERVIEW.md](I:/Y2KMeter/PROJECT_OVERVIEW.md)（补充"多屏 DPI 坐标空间"这一类别的踩坑总结）。

**核心经验（供后续参考）**：

> **在 PerMonitorV2 感知的进程中，任何时候把 JUCE 坐标交给 Win32 API（`CreateWindowEx` / `SetWindowPos` / `MoveWindow` 等），都必须先经过 `juce::Desktop::getInstance().getDisplays().logicalToPhysical(...)` 转换；反向从 Win32 读取的物理坐标要用回 JUCE 时，则用 `physicalToLogical(...)`。跨多屏 + 混合 DPI 场景下，两个坐标空间的差异会随距离原点距离累积，非常隐蔽。**

---

### D.3 第 20 轮：外部 Spectrum 接入 + UI 交互 + 死代码清理

在 v19 偏移 Bug 修复的基础上，本轮完成了功能补全与代码精简，主要目标是"让 milkdrop3 模块真正跑起来"，并去除所有为排查偏移 Bug 而堆积的临时性代码。

#### D.3.1 外部 Spectrum 接入（可扩展）

**问题**：v19 及之前 `Milkdrop3Module` 声明了继承 `AnalyserHub::FrameListener` 且实现了 `onFrame()`，但**从未调用 `hub_->addFrameListener(this)`**，导致引擎一直在用零向量的 PCM/FFT 渲染，视觉效果与音频完全脱钩。

**方案**：分两条通道 + 一个通用扩展点。

| 通道 | API | 数据 | 引擎侧消费 |
|---|---|---|---|
| PCM | `Milkdrop3Api::FeedPcm(interleaved_lr, frame_count)` | Y2Kmeter `AnalyserHub::oscL/oscR` | `CPluginShell::m_sound.fWaveform` |
| Spectrum | `Milkdrop3Api::FeedSpectrum(magL, magR, num_bins, sample_rate)` | Y2Kmeter `AnalyserHub::spectrumMag` | `CPluginShell::m_sound.fSpectrum`（短路 FFT）|
| 扩展点 | `AddPreRenderInjector(std::function<void()>)` / `RemovePreRenderInjector(token)` | —— | 每帧渲染前依次执行 |

**引擎侧改动（`third_party/milkdrop3/code/vis_milk2/pluginshell.{h,cpp}`）**：

- `pluginshell.h` public 区新增：
  ```cpp
  bool  m_bY2kExternalSpectrumValid = false;
  float m_y2kExternalSpectrum[2][NUM_FREQUENCIES] = {};
  ```
- `pluginshell.cpp::AnalyzeNewSound` 在 `time_to_frequency_domain(...)` 之后追加：
  ```cpp
  if (m_bY2kExternalSpectrumValid) {
    memcpy(m_sound.fSpectrum[0], m_y2kExternalSpectrum[0], sizeof(float)*NUM_FREQUENCIES);
    memcpy(m_sound.fSpectrum[1], m_y2kExternalSpectrum[1], sizeof(float)*NUM_FREQUENCIES);
    m_bY2kExternalSpectrumValid = false;  // consume-once
  }
  ```
  引擎内建 3 段 bass/mid/treb 平滑 → `imm/avg/med_avg/long_avg` 全部基于宿主提供的谱工作，无需修改。

**模块侧改动**：

- 构造函数：`hub_->retain(Oscilloscope/Spectrum) + addFrameListener(this)`。
- `onFrame`：只做加锁快照（`AudioSnapshot { interleaved[4096], specL/R[1024], sample_rate, has_pcm, has_spectrum }`）。
- 初始化完成后注册 pre-render injector：`AddPreRenderInjector([this]{ FeedEngineFromSnapshot(); })`，把 `FeedPcm`+`FeedSpectrum` 集中在一处；析构函数持 token 反注册。
- `Milkdrop3Api::RenderFrame()` 每帧开头先 `RunPreRenderInjectors()`，再走 `PluginRender()`。

**频率轴映射**：Y2Kmeter `spectrumMag[1024]` 对应 0~24 kHz（fftSize=2048, sr=48 kHz），MilkDrop3 `fSpectrum[NUM_FREQUENCIES=512]` 对应 0~11025 Hz。`FeedSpectrum` 内部一次 O(N) 线性重采样，`src_step = 11025 · 2 · num_bins / (NUM_FREQUENCIES · sample_rate)`。

#### D.3.2 预设名与切换 UI（引擎侧 D3D9 绘制）

**问题**：D3D9 popup（`WS_POPUP` owned by root）覆盖模块内容区，JUCE `Graphics` 无法在其上叠加文字/按钮——之前尝试过的"JUCE 侧画预设名"会被 popup 完全遮住。

**方案**：把 overlay 完全交给 MilkDrop3 引擎自身的 D3D9 绘制能力，JUCE 侧只在**标题栏**（popup 未覆盖区）画切换按钮。

- **预设名 overlay（引擎绘制）**：
  - `m_bShowPresetInfo = true` → 引擎每帧在 D3D9 surface 右上角写当前预设名（`Milkdrop3Api::EnablePresetInfoOverlay(bool)`）。
  - 切换预设时 `wcscpy(m_szSongTitle, name) + LaunchSongTitleAnim()` 触发中央大字弹出动画（`Milkdrop3Api::ShowPresetTitleAnim(const wchar_t*)`）。
- **标题栏按钮**（JUCE 绘制，位于关闭按钮左侧）：
  - `<`  →  上一预设
  - `?`  →  随机预设
  - `>`  →  下一预设
  - `i` / `-`  →  切换 `m_bShowPresetInfo`
  - 布局逻辑集中于 `Milkdrop3Module::getHeaderButtonRect / hitTestHeaderButton / drawHeaderButtons`。
- **键盘快捷键**：`←/→` 上/下一预设，`Space` 随机。

#### D.3.3 鼠标交互内敛（仅左键、不转发）

- 模块内所有交互仅使用左键；不再有任何右键相关代码（右键继续走 `ModulePanel::mouseDown` 的默认行为，用于弹出 workspace"添加模块"菜单）。
- 内容区被 D3D9 popup 覆盖 → `D3dChildWindow::WndProc` **直接**处理 `WM_LBUTTONDOWN`：`juce::MessageManager::callAsync([this]{ NextPreset(); })`，不再向 JUCE 父窗口 `PostMessage` 转发。

#### D.3.4 死代码清理

| 移除项 | 原因 |
|---|---|
| `Milkdrop3Api::Initialize()` (~130 行) | 被 5-phase 分步初始化完全替代，从未被调用 |
| `Milkdrop3Api::CreateRenderWindow()`（单句包装） | 内容合并入 `Initialize_CreateRenderWindow` |
| `Milkdrop3Api::SetPresetDir` | 内部 `Initialize_PreInit` 已固定使用 `EXE\milkdrop_presets\`，外部调用无法覆盖 |
| `CycleRenderScale` / `ApplyRenderScale` / `render_scale_` / `GetRenderScale` | 从未被调用 |
| `hub_retained_` | 与 `hub_ != nullptr` 语义完全重叠 |

#### D.3.5 调试日志清理（v19 临时代码）

删除仅为定位偏移 Bug 添加的临时日志与冗余上下文切换：

- `Md3BuildTagLogger` 静态初始化 + `MD3_BUILD_TAG` 常量；
- `CreateHWNDOnly` 中 `MonitorFromPoint + GetDpiForMonitor` 探测日志（不参与计算）；
- 多处冗余 `SetThreadDpiAwarenessContext(PMv2)`（进程级已经是 PMv2）；
- 仅用于日志的 `GetDpiForWindow` / `kDpiScale` 计算；
- `layoutContent()` 首次初始化路径中的"粗略 kDpiScale 估算 + phase=-1 覆盖"两次算，改为**只**在 phase=-1 中通过 `logicalToPhysical` 一次算出物理尺寸。
- 保留 `MD3_LOG` 宏与 `md3_debug` 命名空间基础设施，仅在错误路径（如 `OnResize` 的 `Device Reset` 失败、初始化完成汇报）使用。

#### D.3.6 文件变更清单（v20）

| 文件 | 变更 |
|---|---|
| `third_party/milkdrop3/code/vis_milk2/pluginshell.h`  | 新增 `m_bY2kExternalSpectrumValid` / `m_y2kExternalSpectrum[2][NUM_FREQUENCIES]` |
| `third_party/milkdrop3/code/vis_milk2/pluginshell.cpp` | `AnalyzeNewSound` 消费外部 spectrum |
| `source/ui/modules/Milkdrop3Api.h`   | 重写：删死码 + 新增 `FeedSpectrum / AddPreRenderInjector / EnablePresetInfoOverlay / ShowPresetTitleAnim` |
| `source/ui/modules/Milkdrop3Api.cpp` | 与 .h 同步；`ConvertPcmToMd3` 改用 `unsigned centered at 128`（消除 UB） |
| `source/ui/modules/Milkdrop3Module.h`   | 新增 `AudioSnapshot / injector token / HeaderButton` 相关成员，删除 `hub_retained_` |
| `source/ui/modules/Milkdrop3Module.cpp` | 重写：注册 FrameListener、拆分快照与投喂、标题栏按钮、左键交互内敛、清理 v19 日志 |
| `source/ui/modules/Md3DebugLog.h` | 精简：删除 v7 tag 与 `#pragma message` 编译期噪音 |
| `docs/MilkDrop3_Integration.md` | 精简附录 D 失败尝试记录（D.2~D.5 → 保留 D.1 总览表 + D.2 最终修复 + D.3 本轮） |

#### D.3.7 备注

按协作规则，本轮：

- 未 git commit / push，未升级整体版本号；
- 已同步至 [PROJECT_OVERVIEW.md](I:/Y2KMeter/PROJECT_OVERVIEW.md)，方便下次会话读取上下文；
- 待用户测试稳定并明确确认后，再执行 +0.0.1 版本升级并做正式合入。

---

### D.4 添加 MilkDrop3 模块时的多轮死锁根因分析（v20→v21 踩坑记录）

在 v20 模块实现完成后，点击"添加 MilkDrop3 模块"瞬间必现异常卡死。经过四轮排查与修复，根因逐层递进，最终定位。以下是完整的因果链与修复方案。

#### D.4.1 第一轮：getDefaultSizeForType 中同步构造重型模块 → AnalyserHub 互斥锁死锁

**调用链**：

```
右键菜单 → mouseEnter MilkDrop3 项
  → setAddMenuHoverPreview(true, milkdrop3)
    → getDefaultSizeForType(milkdrop3)          [cache miss]
      → factory(milkdrop3)
        → new Milkdrop3Module(&hub)
          → hub_->retain(Oscilloscope)            ← mutex lock
          → hub_->retain(Spectrum)                ← mutex lock
          → hub_->addFrameListener(this)          ← mutex lock
        → panel->getDefaultWidth() → 640
        → ~Milkdrop3Module()
          → hub_->removeFrameListener()           ← mutex lock
          → hub_->release(Spectrum)               ← mutex lock
          → hub_->release(Oscilloscope)           ← mutex lock
```

**根因**：`getDefaultSizeForType` 为拿到尺寸 `(640,480)`，创建完整 MilkDrop3Module 实例（含 AnalyserHub retain/addFrameListener），这些操作发生在 mouseEnter（主线程消息泵）上下文中，AnalyserHub 内部 mutex 可能被音频线程持有 → **主线程等待 mutex → 死锁**。

**修复**：在 `ModuleWorkspace::getDefaultSizeForType` 中增加硬编码尺寸表（覆盖全部 21 个 ModuleType），优先查表 O(1)，不再走 factory。

#### D.4.2 第二轮：getHoverPreviewImage 中创建完整模块 + createComponentSnapshot → 同样的 AnalyserHub 互斥锁死锁

**调用链**：

```
paintOverChildren → getHoverPreviewImage(milkdrop3)   [cache miss]
  → factory(milkdrop3)
    → new Milkdrop3Module(&hub)                       ← AnalyserHub 全套 mutex 操作
  → setBounds(0, 0, 640, 480)
    → layoutContent() → startTimer(5)                 ← 启动 D3D9 异步初始化 timer
  → createComponentSnapshot                            ← 1.2MB 堆分配
  → ~Milkdrop3Module()                                 ← AnalyserHub 全套 mutex 操作
```

**根因**：`getHoverPreviewImage` 与 `getDefaultSizeForType` 问题同源——为生成预览图创建完整重型模块。且此路径发生在 `paintOverChildren` 渲染线程上下文中，比 mouseEnter 更危险。

**修复**：在 `ModuleWorkspace::getHoverPreviewImage` 中对 `milkdrop` / `milkdrop3` 提前短路，改为纯 CPU 绘制静态占位图（暗底 + 模块名 + PinkXP 风格边框），不创建任何模块实例。

#### D.4.3 第三轮：renderOpenGL 中 measureMax/blitToModules 与 addModule 的组件树数据竞争 → GPU 驱动 hang

**调用链**：

```
addModule 主线程:                     OpenGL 渲染线程:
  addAndMakeVisible(raw)    ← 修改组件树
  raw->setBounds(...)                     renderOpenGL()
  modules.add(...)                         measureMax(this)
  toFront(false)                            getNumChildComponents()  ← 读取组件树
                                            getChildComponent(i)     ← 并发读写！
                                          → 返回无效尺寸
                                          glTexImage2D(垃圾 w,h)
                                          glBlitFramebuffer(垃圾)
                                          → GPU 驱动 hang → 0x00007ffceed8d9f8
```

**根因**：`renderOpenGL` 的 `measureMax` lambda 遍历组件树读取子组件信息（`getNumChildComponents` / `getChildComponent`），而主线程 `addModule` 同步修改同一棵树（`addAndMakeVisible` / `toFront`）。JUCE 的 `MessageManager::Lock` 只阻止消息调度，不阻止直接 API 调用。组件树被并发读写 → 返回无效尺寸 → FBO 分配/Blit 操作传入脏数据 → GPU 驱动级 hang。

**修复**：在 `ModuleWorkspace` 中新增 `std::atomic<int> glRenderSuppressed{0}` 标志。
- `addModule` 进入时 `++`，离开时（通过 `MessageManager::callAsync` 延迟）`--`。
- `renderOpenGL` 的开头条件从 `if (milkdrop_render_ready_ && milkdrop_pm_handle_ != nullptr)` 扩展为 `if (... && workspace && !workspace->isGlRenderSuppressed())`。
- `removeModule` / `clearAllModules` 同理。

#### D.4.4 第四轮：renderFrame 中 paintComponent（CachedImage 组件绘制）不受 glRenderSuppressed 保护 → Editor::paint() 的标题文字 glyph 光栅化 malloc 与主线程 D3D9 初始化 CreateWindowEx 堆分配形成堆锁死锁

**调用链**：

```
JUCE renderFrame() 中：
  renderOpenGL()          ← glRenderSuppressed 保护 ✓ （项目M 路径跳过）
  paintComponent()        ← 不受保护 ✗
    → paintEntireComponent → 遍历整个组件树
      → Editor::paint()
        → g.drawText("Y2Kmeter")
        → g.drawText("v2.3.5")
        → g.drawText("iisaacbeats.cn")
          → GlyphArrangement → drawGlyph
            → EdgeTable(HeapBlock<int>)  ← 新分配 HeapBlock
              → std::malloc(size)
                → Windows HeapAlloc → heap lock

主线程：
  addModule → layoutContent → startTimer(5)
  → phase 0: CreateWindowExW(L"Y2Kmeter_MD3_Embed", ...) ← Windows 堆分配 → heap lock
```

**关键误区**：崩溃堆栈 `<unknown> 0x00007ffceed8d9f8` + `HeapBlock::mallocWrapper` → 直觉认为是 Milkdrop3Module 自己的 paint 触发的。但实际是 **Editor::paint()** 的标题文字（`"Y2Kmeter v2.3.5 iisaacbeats.cn"`）触发的。

**根因**：`glRenderSuppressed` 只保护了 `renderOpenGL`（项目M 渲染），但没有保护同一帧中紧接着执行的 `paintComponent`（CachedImage 组件树缓存绘制）。Milkdrop3Module `640×480` + `setOpaque(true)` 触发 FBO 大幅扩张，`paintComponent` 必须重绘整个编辑器（包括标题栏文字）。此时 OpenGL 渲染线程的 glyph 光栅化 `malloc` 与主线程 D3D9 初始化 `CreateWindowEx` 的堆分配**同时发生** → 两个线程竞争 Windows 堆锁 → 死锁。

**修复**（两处）：

1. **PluginEditor.cpp `paint()`**：用 `if (juce::MessageManager::getInstance()->isThisTheMessageThread())` 包裹标题文字绘制段（~50 行，6 次 `drawText`）。GL 渲染线程中跳过文字绘制，仅保留 `drawPinkTitleBar`（纯 fillRect 渐变背景，无 glyph 光栅化）和分割线。视觉几乎无差异。

2. **ModuleWorkspace.cpp `addModuleByType()`**：将 `glRenderSuppressed` 的 `++` 提升到 `factory(t)` 之前（覆盖构造阶段），清零改用 `MessageManager::callAsync` 延迟到下一个消息迭代。确保模块添加后的**第一个** `renderFrame` 完全跳过项目M 渲染，给 CachedImage FBO 一帧时间稳定。

#### D.4.5 四轮死锁总结

| 轮次 | 死锁位置 | 竞争双方 | 直接原因 | 修复方式 |
|---|---|---|---|---|
| 1 | `getDefaultSizeForType` | 主线程 vs 音频线程 | factory 构造 MilkDrop3Module → AnalyserHub mutex | 硬编码尺寸表 |
| 2 | `getHoverPreviewImage` | 渲染线程 vs 音频线程 | factory + createComponentSnapshot → AnalyserHub mutex | 静态占位图 |
| 3 | `renderOpenGL` | 主线程 vs 渲染线程 | measureMax 遍历组件树 vs addAndMakeVisible 并发修改 | glRenderSuppressed 原子标记 |
| 4 | `renderFrame → paintComponent` | 渲染线程 vs 主线程 | glyph malloc vs CreateWindowEx 堆分配竞争堆锁 | Editor::paint 消息线程检查 + callAsync 延迟 |

**核心教训**：

1. 任何创建 GPU 资源（D3D9 Device / OpenGL Context）的重型模块，不要在主线程同步构造路径中做任何重量级 hub 操作。构造函数应当轻量，初始化延迟到 Timer 或 callAsync。
2. OpenGL CachedImage 的 `paintComponent` 与 `renderOpenGL` 是同一帧的两个独立阶段，保护其中一个不足以保护完整帧。
3. GDI glyph 光栅化在 OpenGL rendering context（`createOpenGLGraphicsContext`）中走的是 `EdgeTable → malloc` 路径，与主线程 Windows API（如 `CreateWindowEx`）共享堆锁，不能无保护地执行。
4. `MessageManager::Lock` 不阻止直接 Component API 调用（`addAndMakeVisible` / `setBounds`），需要额外的原子标志做并发控制。

#### D.4.6 控制栏内敛验证（v21）

v21 将控制栏从底部移至顶部置顶后，整个 Milkdrop3 模块的内容绘制链路已完全内敛：

| 层级 | 绘制内容 | 负责组件 |
|---|---|---|
| 标题栏 | 粉色渐变 + "MilkDrop3" + [×] | ModulePanel::paint()（基类，所有模块共享） |
| 控件栏 | `<` `A` `>` `?` + 预设名文本 | Milkdrop3Module::PaintControlBar()（private） |
| popup 区 | D3D9 RenderFrame → Present | Milkdrop3Api + D3dChildWindow（Milkdrop3Module 管理） |
| 加载页 | 黑底 + 进度条 | Milkdrop3Module::paint()（override） |
| 错误页 | 错误信息文字 | Milkdrop3Module::paintContent()（override） |

所有按钮的布局（`GetControlBarBtnRect`）、命中测试（`HitTestControlBarBtn`）、绘制（`PaintControlBar`）、动作分发（`ExecuteOverlayAction`）、焦点显隐（`SetFocusVisual`/`CheckOverlayAutoHide`）均为 Milkdrop3Module 的 private 方法，无任何外部组件参与 Milkdrop3 独有的交互逻辑。

---

**最后更新**：2026-08-04（v22 交互逻辑大修 + 初始化死锁根除 + 弹窗按钮齐全）

---

## 附录 F. v23（v2.3.6）：Milkdrop3 交互 bug 全面修复 + Skill v1.2.0

**日期**：2026-08-05  
**触发**：用户报告"控件栏与 D3D 视频区交互后消失、预设跳转弹窗回车无效、颜色跟随全局主题不合适"3 个 bug。

### F.1 修复清单

| # | 症状 | 根因 | 修复 |
|---|---|---|---|
| Bug 1 | 打开跳转弹窗后点击非本模块区域 → 整个控件栏卡入不可恢复的隐藏状态 | `SetWindowPos(overlay, d3d_child_hwnd_, ...)` 中 `hWndInsertAfter` Win32 语义是"置于其下"，overlay 被塞到 D3D popup 下方 | 改用 `SetWindowPos(overlay, HWND_TOP, ...)`；模块内点击（含视频区）不再触发隐藏 |
| Bug 2 | 抬头颜色跟随软件主题预设，视频黑底上不协调 | `UpdateThemeColors()` 直接引用 `PinkXP` 系列色板 | 改为固定灰阶（`0x1E1E1E / 0x333333 / 0x666666 / 0xCCCCCC / 0xF0F0F0`），与主题解耦 |
| Bug 3 | 跳转弹窗输入编号 + 回车后预设未实际切换 | 单行 EDIT 子控件中 `WM_CHAR` 是否收到 `VK_RETURN` 依赖父窗口 dialog 属性 / `IsDialogMessage` 循环 | 把 `VK_RETURN` 处理从 `WM_CHAR` 搬到 `WM_KEYDOWN`，与 `VK_ESCAPE` 一致 |
| Bug 4（衍生） | 全软件外围出现黑边、标题栏文字变成小圆圈、右侧边不对称 | 中间尝试修 D3D popup 偏移时误改 `third_party/JUCE/modules/juce_gui_basics/native/juce_Windowing_windows.cpp`（`UWPUIViewSettings` / renderer / `DwmSetWindowAttribute` / `getBorderThickness`） | `git checkout -- third_party/JUCE` 完整回滚；milkdrop3 的所有修复只允许在 milkdrop3 目录内 |
| Bug 5（衍生） | 一度用 `Milkdrop3Module::paint` 自绘标题栏 + 关闭按钮时 MSVC C2065 | 直接引用了基类 `ModulePanel::closeButtonPressed / closeButtonHovered`，二者是 `private` | 删除自定义标题栏绘制，让 `ModulePanel::paint(g)` 走默认路径 |
| Bug 6（衍生） | Cancel 按钮绘制段 5 处 C2065 未声明 | copy-paste Go 按钮代码时跨 `if` 块复用了 `oldPn / nullBr / oldBr2` 局部变量 | Cancel 段独立命名（`oldPnCn / nullBrCn / oldBrCn`）+ 重新声明 |

### F.2 关键代码位置

| 文件 | 修改点 |
|---|---|
| `source/ui/modules/Milkdrop3Module.cpp` | `ControlBarOverlay::SetVisible / Reposition` 用 `HWND_TOP`；`UpdateThemeColors()` 全部改固定灰阶；`EditSubclassProc` 的 Enter/Esc 统一 `WM_KEYDOWN`；`PaintJumpDialog` Cancel 段变量独立声明 |
| `third_party/JUCE/**` | ⚠️ **无变更**（本轮误改后已完整回滚，禁止再次触碰） |

### F.3 教训（已并入 `milkdrop3-dev-guard` Skill）

1. **`SetWindowPos hWndInsertAfter` 语义 = 置于其下**（不是"置于其上"）；overlay 想置顶只能传 `HWND_TOP`。
2. **单行 EDIT 中 `VK_RETURN` 必须在 `WM_KEYDOWN` 处理**——`WM_CHAR` 不可靠。
3. **`third_party/JUCE/**` 是全软件共享代码**，milkdrop3 相关 DPI / 边框问题只能在 milkdrop3 内部用 `logicalToPhysical` 解决，禁止碰 JUCE。
4. **`ModulePanel::closeButtonPressed / closeButtonHovered` 是 private**，子类要么让基类 paint 走默认，要么自己维护状态。
5. **C++ 块作用域**：copy-paste 复用的绘制代码，各 `if` 块内局部变量必须独立命名或提到外层块。
6. **AI 命令行编译验证不可靠**：Git Bash 里 `cmd //c` 路径转义反复失败；正确协作方式是 AI 只做静态自检，完整编译交给用户在 CLion 里跑。

### F.4 Skill 优化（v1.1.0 → v1.2.0）

- SKILL.md：触发条件、前置检查（6→12 项）、禁止事项、自检（8→12 问）
- `references/forbidden-list.md`：追加 F11-F15 五条禁令
- `references/symbol-facts.md`：追加 §10 ModulePanel 成员可见性 + §11 Win32 API 语义速查
- `references/lessons.md`：追加 v23 章节
- `references/compile-verify.md`：追加 3 条编译错误、2 条运行时故障、§8 AI 编译验证协作原则
- `scripts/check_forbidden_patterns.py`：**新增**——8 条判定的静态扫描器，与 `check_init_sequence.py` 组成双检

**验证**：`check_init_sequence.py` `[OK]`；`check_forbidden_patterns.py` `[OK]`；`third_party/JUCE` 未改动；编译由用户在 CLion RelWithDebInfo 中确认稳定。

### F.5 版本号

- 项目：`2.3.5` → `2.3.6`（CMakeLists 2 处 + installer + PluginEditor 4 处）
- Skill：`1.1.0` → `1.2.0`（YAML `metadata.version`）

**备注**：本轮未执行版本号升级和 git push。按协作约定 [[memory:jgjmv0lq]]，待用户明确确认"OK/没问题"后，再执行 +0.0.1 版本升级并合入。