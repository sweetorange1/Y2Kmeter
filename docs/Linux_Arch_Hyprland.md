# Linux / Arch + Hyprland 适配说明

> 适用版本：v2.8.6 ｜ 目标环境：Arch Linux + Wayland（Hyprland）+ PipeWire ｜ XWayland
>
> 本文记录 Y2Kmeter 在 Linux 桌面上的落地方式、改动点与已知限制。
> 上游原本只支持 Windows / macOS，Linux 相关代码是本次移植新增的。

---

## 一、结论速览

| 能力 | Linux 状态 | 说明 |
|------|-----------|------|
| 构建（Standalone） | 支持 | CMake + Ninja，GCC 16 实测通过 |
| 窗口显示 | 支持 | 无边框浮动条；Hyprland 下自动转 floating + 置顶 |
| 置顶（Pin） | 支持 | 走 X11 `_NET_WM_STATE_ABOVE`，Hyprland 实测生效 |
| Stick 驻留条 | 支持（视觉等价） | 浮动条贴屏幕顶/底边 + pin；**不压缩工作区**（Wayland 无 AppBar API） |
| Hide 模式 | 支持 | 窗口收缩，悬停浮层逻辑与 Windows 一致 |
| 系统输出（Output 源） | 支持 | 采集默认 sink 的 monitor（`pw-record`；PulseAudio 回退 `parec`） |
| 麦克风（Microphone 源） | 支持 | 走 JUCE ALSA 设备 |
| 预设 Save / Load（.settings） | 支持 | 原生对话框走 zenity/kdialog；失败会弹错误提示 |
| Milkdrop 模块 | 支持（需 libprojectM 4.x） | 运行时 dlopen `libprojectM-4.so`，预设/纹理从 `~/.local/share/Y2Kmeter/` 读取 |

---

## 二、依赖与构建

### 2.1 依赖（Arch）

```bash
sudo pacman -S --needed base-devel cmake ninja gcc ccache \
    libcurl alsa-lib freetype2 libpng \
    libx11 libxrandr libxinerama libxcursor libxext \
    pipewire pipewire-pulse wireplumber \
    zenity        # 文件对话框（非必需：缺省时回退 JUCE 自绘对话框）

# 可选：Milkdrop 模块需要 libprojectM 4.x
sudo pacman -S --needed libprojectm
```

说明：

- `gtk3` / `webkit2gtk` **不再是构建依赖**：CMake 里显式 `JUCE_WEB_BROWSER=0`，
  避免 juce_gui_extra 去包含 `gtk/gtk.h`（Arch 默认没有 webkit2gtk-4.0）。
- `libcurl` 是 Linux 上 JUCE 网络栈的硬依赖（`JUCE_USE_CURL=1`），CMake 通过
  `NEEDS_CURL TRUE` 让 JUCE 把 pkg-config 的 libcurl 链进来。
- `zenity` 只在"保存/导入预设"的文件对话框里用到；没有它 JUCE 会退回自绘
  文件选择器（功能等价）。

### 2.2 构建

```bash
cmake --preset linux-release
cmake --build --preset linux-release --target Y2Kmeter_Standalone
# 产物：build/linux/Y2Kmeter_artefacts/Release/Standalone/Y2Kmeter
```

或手动：

```bash
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build/linux --target Y2Kmeter_Standalone -j"$(nproc)"
```

### 2.3 运行 / 安装

```bash
# 直接运行
./build/linux/Y2Kmeter_artefacts/Release/Standalone/Y2Kmeter

# 安装到用户目录（推荐用脚本：会一并 seed Milkdrop 资源 + 建启动项）
scripts/linux_install.sh
# 等价于：
#   install -Dm755 build/linux/Y2Kmeter_artefacts/Release/Standalone/Y2Kmeter ~/.local/opt/Y2Kmeter/Y2Kmeter
#   cp -r assets/milkdrop_presets assets/milkdrop_textures ~/.local/share/Y2Kmeter/
```

应用配置：`~/.config/Y2Kmeter.settings`；Milkdrop 资源：`~/.local/share/Y2Kmeter/milkdrop_presets|milkdrop_textures`。



---

## 三、本次为 Linux 修的构建问题

| # | 现象 | 根因 | 修法 |
|---|------|------|------|
| 1 | `fatal error: gtk/gtk.h: No such file` | `JUCE_WEB_BROWSER` 默认 1，Linux 分支会强包含 GTK/WebKit 头，而 CMake 只有在 pkg-config 找到 webkit2gtk-4.x 时才加 include 路径 | CMake 增加 `JUCE_WEB_BROWSER=0` |
| 2 | `invalid use of incomplete type SIMDRegister<long long>` | `juce::jmax<juce::int64>` 显式模板实参命中了 JUCE 的 `SIMDRegister` 重载，GCC 下实例化不完整类型 | 改成自然推导 `juce::jmax ((juce::int64) 1, now - start)` |
| 3 | `undefined reference to curl_multi_timeout` 等 | Linux 上 JUCE 的 URL 走 libcurl，但工程没声明该依赖 | `juce_add_plugin(... NEEDS_CURL TRUE)` |

---

## 四、窗口行为（Hyprland）

### 4.1 为什么需要专门处理

- Hyprland 是平铺式合成器：普通 X11 窗口默认会被塞进平铺布局，表现为
  窗口被拉满屏幕、无法拖动/缩放，`setBounds` 也会被合成器覆盖。
- Hyprland **不采纳 X11 `ConfigureRequest` 里的坐标**（映射时会把浮动窗口
  居中），只采纳尺寸 —— 所以"贴边驻留"必须显式通过 IPC 搬窗口。
- 上游的 Stick 是 Windows AppBar（`SHAppBarMessage`），Linux 没有对应 API。

### 4.2 实现方式

新增 `source/standalone/LinuxWindowSupport.{h,cpp}`：

- 通过环境变量 `HYPRLAND_INSTANCE_SIGNATURE` 判定是否 Hyprland；
- `hyprctl -j clients` 里按 pid 找到本进程窗口的 address；
- `hyprctl version -j` 解析 tag，**同时适配两代 dispatch 语法**：
  - Hyprland ≥ 0.55（Lua 配置）：`hl.dsp.window.float{action="enable", window="address:0x..."}` /
    `hl.dsp.window.pin{...}` / `hl.dsp.window.move{x=.., y=.., ...}`；
  - Hyprland < 0.55（hyprlang）：`hyprctl dispatch setfloating` 等旧语法；
- 坐标换算：应用拿到的 X11 像素坐标 × (显示器逻辑宽 / X11 像素宽) = Hyprland
  逻辑坐标（本机 2560 物理 ↔ 2400 逻辑，scale 1.0667）。

启动时的窗口流程（`Y2KStandaloneApp::initialise`）：

1. 恢复/计算窗口 bounds（见下）；
2. `addToDesktop()` + `setVisible(true)`；
3. `tryAdoptLinuxWindow()`：最多 8 次 × 250ms 重试，把窗口转 floating + 提到最前；
4. 成功后重新提交期望几何，并用 `hyprctl` 把窗口搬到目标位置。

平铺式 WM 的兜底：非 Hyprland 环境不会做任何 IPC 调用，若仍被平铺，可在
合成器配置里加窗口规则。

Hyprland ≥ 0.55（Lua 配置，推荐，双保险 + 禁用合成器圆角）：

```lua
-- ~/.config/hypr/custom/rules.lua 之类的用户配置里
hl.window_rule({ match = { class = "^(Y2Kmeter)$" }, float = true, rounding = 0 })
```

Hyprland < 0.55（hyprlang）：

```ini
windowrulev2 = float, class:^(Y2Kmeter)$
windowrulev2 = rounding 0, class:^(Y2Kmeter)$
```

> `float` 规则的作用：应用启动时会自己 `hyprctl` 转 floating，但用户执行
> `hyprctl reload` 或重启合成器后，dispatch 设置的浮动状态可能被规则重算覆盖；
> 有这条规则兜底后，Y2Kmeter 始终保持浮动。`rounding = 0` 只对 Y2Kmeter 生效，
> 因为它的像素 UI 不需要合成器圆角。

### 4.3 Stick（驻留条）

Linux 上 Stick 走"浮动条贴边"：

- 进入：窗口改为屏幕宽 × 条高，贴显示器 `userArea` 顶边或底边，`pin` 置顶，
  布局切换到对应的 stick 槽（T/B 与 Windows 一致，独立存档）；
- 退出：取消 pin，恢复进入前的窗口 bounds 与常驻布局；
- 差异：**Hyprland/Wayland 没有 AppBar 那种"缩小其他窗口工作区"的 API**，
  所以是视觉等价的覆盖条，而不是真正的占位。介意重叠可在合成器侧配合
  `windowrulev2` 或留出顶部间距。

### 4.4 窗口 bounds 的恢复策略

- 忠实恢复存档里的窗口尺寸/位置（包括"整屏尺寸"）：但恢复出来的是 **floating**
  窗口，可随时拖动/缩放，不再是"被平铺锁死、盖满桌面动不了"的状态。
- 若存档尺寸超出当前显示器 `userArea`（例如换了更小的屏），才会被夹到
  `userArea` 内。
- Linux Standalone 的缩放上限放宽到 8192×8192（原来是 1600×1100），用户想
  拉满 2560×1600 全屏不会被尺寸上限卡住。
- 恢复时使用 `addToDesktop()` **之前** 记录的期望 bounds：Hyprland 的 float
  规则会在窗口 map 时重新决定浮动窗尺寸，如果用 map 之后的 bounds 再提交一次，
  会把合成器给的尺寸固化下来。
- 关闭时按"Show 态等效尺寸"保存，语义与上游一致。

### 4.5 文件对话框层级

保存/导入预设用的 `FileChooser` 在 Linux 上可能是独立顶层窗口（zenity 或
JUCE 自绘），旧代码 `parent = nullptr` + 主窗 AlwaysOnTop 会让对话框出现在
主窗下面，表现为"点了 Save 没反应"。现在：

- `FileChooser` 传入顶层窗口作为 parent（居中/归属更合理）；
- 对话框打开期间临时摘掉主窗 AlwaysOnTop，关闭后恢复。

---

## 五、系统输出（Output 音频源）

### 5.1 旧实现为什么不行

Linux 上原来的 "Output" 走 `findPreferredVirtualLoopbackInputName()`：在
JUCE 的 **ALSA 输入设备列表**里按关键词（blackhole/loopback/monitor/...）挑一个
设备当回环输入。

但 PipeWire 的 monitor 源**不是 ALSA 物理设备**，JUCE 的 ALSA 设备枚举里
根本没有它；关键词匹配失败后它按"排除麦克风关键词"兜底，会挑中 ALSA 的
`default` 播放 PCM（描述为 `Default ALSA Output (currently PipeWire Media Server)`），
打开后拿到的其实是默认采集源 —— 也就是麦克风。

### 5.2 新实现

新增 `source/standalone/LinuxMonitorCapture.{h,cpp}`（零 JUCE 依赖）：

- **PipeWire（主路径）**：
  `pw-record --target <sink-node-name> -P stream.capture.sink=true --format f32 --rate 48000 --channels 2 --container raw -`
- **PulseAudio（回退）**：
  `parec --device=<sink>.monitor --format=float32le --rate=48000 --channels=2 --raw`
- 默认 sink 的解析顺序：`pactl get-default-sink` → `wpctl inspect
  @DEFAULT_AUDIO_SINK@` → `pw-metadata` → `pw-dump`，逐级兜底；
- 启动后用 `pactl list short source-outputs` 交叉校验：新流必须绑定到
  `<sink>.monitor` 源，否则直接失败（不允许把麦克风静默当成系统输出）；
- 采集线程按 10ms（480 帧）分块，interleaved → L/R 平面 float 回调；
- 与 Windows loopback 相同的并发约束：启动前 `detachHolderCallback()`，
  停止后 `attachHolderCallback()`，保证同一时刻只有一路在写 `AnalyserHub`。

> **关键坑**：`<sink>.monitor` 只是 pulse 兼容层的虚拟源名字，**不是原生
> PipeWire 节点**。直接 `pw-record --target <sink>.monitor` 时 pw-record 解析
> 不到目标会**静默回退到默认 source（麦克风）**，不报任何错 —— 表现就是
> “Output 里混进了麦克风声音；静音麦克风后 Output 完全没声音”。
> 正确写法是 target 用 sink 的 node name 并显式 `stream.capture.sink=true`。

切换语义与上游一致：下拉里仍只有 `Microphone` / `Output` 两项，
`Output` 现在直接抓系统混音输出，不需要 BlackHole/VB-Cable 之类的虚拟声卡。

### 5.3 自带验证方式

```bash
# 播放一段测试音，应用切到 Output，电平表应跟随系统输出跳动
ffplay -f lavfi -i "sine=frequency=1000:duration=5" -volume 60
# 或
pw-play /path/to/tone.wav
```

已知坑（来自实测）：

- `pw-record` 不加 `--container raw` 会在 stdout 前带 24 字节 AU 头，导致
  f32 流整体错位；实现里已固定加该参数；
- `<sink>.monitor` 不能直接当 `pw-record --target`，会静默回退麦克风
  （见 5.2 的关键坑）；现在实现会交叉校验绑定目标并报错；
- `parec` 的 stdout 有缓冲，外部 `timeout`/SIGTERM 杀掉时可能一行都没写出；
  这是测试脚本的假象，不是采集失败。应用内部走管道实时读，不受影响；
- 若把某个应用的录音流手动在 mixer 里改到别的 source，会与实现抢占；
  Y2Kmeter 每次切换 Output 都会重新建立自己的采集流。

---

## 六、预设 Save / Load（.settings）

- Save：把当前 settings 物理文件复制到用户选择的路径；
- Load：覆盖 `~/.config/Y2Kmeter.settings` 后自重启；
- 本次改动：失败不再静默（弹 "Save preset failed" 并带目标路径），成功仍保持
  静默；目标若是目录会自动补 `Y2Kmeter.settings`；复制后校验目标文件非空。

---

## 七、已知限制 / 后续可做

1. **Milkdrop**：需要系统安装 libprojectM 4.x（Arch: `sudo pacman -S libprojectm`），
   运行时通过 `dlopen("libprojectM-4.so.4")` 加载；也可把 `libprojectM-4.so`
   放到可执行文件同目录。

   **预设/纹理来源**：仓库本身自带 `assets/milkdrop_presets/`（9921 个 `.milk`）
   与 `assets/milkdrop_textures/`（66 个纹理），另外还各有一份同名 `.zip`。
   Linux 运行时**不直接读仓库目录**，而是读用户数据目录：
   `~/.local/share/Y2Kmeter/milkdrop_presets` 与 `.../milkdrop_textures`。
   `scripts/linux_install.sh` 会按"目录 → zip"的优先级把资源 seed 过去并打印数量；
   目标目录已存在且非空时跳过（不覆盖用户自己增删的预设）。

   **如果只手动拷贝了二进制、没有 seed 资源**：Milkdrop 模块本身仍能创建，
   但预设列表为空/只能显示默认画面 —— 这不是预设"丢失"，而是资源目录没建立。
   重新跑一次安装脚本，或手动执行：
   ```bash
   mkdir -p ~/.local/share/Y2Kmeter
   cp -r assets/milkdrop_presets assets/milkdrop_textures ~/.local/share/Y2Kmeter/
   ```
2. **Stick 不是真占位**：见 4.3，属于 Wayland 平台能力差异。
3. **多显示器/混合缩放**：坐标换算按"窗口所在显示器的 scale"算；多显示器
   各自不同缩放组合未做过实机验证。
4. **VST3 构建**：Linux 下 `Y2Kmeter_VST3` 目标理论上可编（未安装到 `~/.vst3`
   验证），本次只交付 Standalone。
5. 上游 Windows 专属的 Direct2D 规避、AppBar、TCC 等代码在 Linux 上不会编译
   （平台宏隔离），无需移植。

---

## 八、改动文件清单（Linux 适配）

新增：

- `source/standalone/LinuxWindowSupport.{h,cpp}` —— Hyprland 浮动/置顶/移动
- `source/standalone/LinuxMonitorCapture.{h,cpp}` —— 系统输出采集
- `scripts/linux_install.sh` —— 安装二进制 + seed Milkdrop 资源 + 启动项

修改：

- `CMakeLists.txt` —— `JUCE_WEB_BROWSER=0`、`NEEDS_CURL TRUE`、新增 Linux 源文件
- `CMakePresets.json` —— `linux-release` / `linux-debug` 预设
- `source/perf/PerformanceCounterSystem.cpp` —— GCC 下 `jmax<int64>` 编译错误
- `source/standalone/Y2KStandaloneApp.cpp` —— 窗口收养/几何、bounds 恢复策略、
  Linux 系统输出采集接线、Save 健壮性
- `PluginEditor.{h,cpp}` —— Linux 窗口几何抽象、Stick 启用、对话框置顶挂起
- `source/ui/ModuleWorkspace.{h,cpp}` —— Linux 下显示 Stick 按钮、Save 对话框
  parent 与打开/关闭通知、拼写与注释
- `source/ui/modules/ProjectMApi.cpp` —— Linux dlopen `libprojectM-4.so` 分支
- `source/ui/modules/MilkdropModule.cpp` —— Linux 优先从 `~/.local/share` 读资源
