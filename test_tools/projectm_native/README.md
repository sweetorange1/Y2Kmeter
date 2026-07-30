# projectM Native Test Tool

Y2Kmeter 内置 Milkdrop 模块的原生对照测试工具。

## 用途

当 Y2Kmeter 中某个 Milkdrop 预设渲染效果不对时，可以用此工具配合**原生 projectM 引擎**渲染同一预设，
对比效果，判断是预设本身的问题还是 Y2Kmeter 集成层的问题。

## 架构关系

```
┌─────────────────────────────────────────┐
│  Y2Kmeter MilkdropModule                │
│  ├─ juce::OpenGLContext                 │
│  ├─ ProjectMApi (LoadLibrary 加载 DLL)   │
│  └─ projectM-4.dll (v4.1.4)            │
│     └─ glew32.dll                       │
└─────────────────────────────────────────┘
          对比
┌─────────────────────────────────────────┐
│  projectMSDL (原生前端)                  │
│  ├─ SDL2 窗口 + 音频 Loopback 采集       │
│  └─ projectM-4.dll (同引擎，原生调用)     │
│     └─ glew32.dll                       │
└─────────────────────────────────────────┘
```

## 快速开始

### 方案 A：下载预编译包（推荐，最快）

```powershell
# 在 PowerShell 中运行
cd I:\Y2KMeter\test_tools\projectm_native
.\download_setup.ps1
```

脚本会自动：
1. 从 GitHub 下载 `projectMSDL-2.0-windows-x64-pre3.zip`
2. 解压到 `projectMSDL\` 目录
3. 将 Y2Kmeter 预设目录 ( `assets\milkdrop_presets\` ) 软链接过来
4. 生成启动脚本

### 方案 B：从源码编译（获取最新版本）

```powershell
.\build_from_source.ps1
```

前置条件：Git、CMake、VS 2022。脚本会自动克隆 vcpkg 并编译所有依赖。

## 启动测试

```powershell
# 双击运行
.\run_test.bat

# 或在 PowerShell 中
.\run_test.ps1
```

## 首次配置

1. 启动 projectMSDL 后，按 **ESC** 打开设置面板
2. 在 **Settings → General → Preset Paths** 中添加预设目录：
   ```
   presets\y2kmeter_presets\
   ```
   或使用绝对路径：
   ```
   I:\Y2KMeter\assets\milkdrop_presets\
   ```
3. 选择音频输入设备（默认会自动采集系统音频 Loopback）
4. 关闭设置面板，开始测试

## 键盘快捷键

| 按键 | 功能 |
|------|------|
| `ESC` | 打开/关闭设置 UI |
| `N` / `P` | 下一个 / 上一个预设 |
| `R` | 随机预设 |
| `SPACE` | 锁定当前预设（不切换） |
| `Y` | 开关随机播放模式 |
| `Shift+N/P` | 带平滑过渡的预设切换 |
| `Ctrl+F` | 全屏切换 |
| `Ctrl+Q` | 退出 |
| `F1` | 显示帮助 |
| `鼠标滚轮` | 切换预设 |

## 测试工作流

1. **在 Y2Kmeter 中找到有问题的预设**：记下预设文件名（如 `$$$ Royal - Mashup (574).milk`）
2. **在 projectMSDL 中加载同一预设**：按 `N`/`P` 翻到同一预设，或使用搜索
3. **播放相同音频**：确保两个工具接收相同的音频输入
4. **对比渲染效果**：
   - 颜色是否正确？
   - 形状/波形是否一致？
   - 动画行为是否相同？
5. **判断结论**：
   - 如果 projectMSDL 正常 → 问题在 Y2Kmeter 集成层（OpenGL context、像素回读、预设加载等）
   - 如果 projectMSDL 也有问题 → 这是 libprojectM 本身对该预设的兼容性问题

## 文件结构

```
test_tools/projectm_native/
├── README.md                 ← 本文件
├── download_setup.ps1        ← 下载并设置预编译包
├── build_from_source.ps1     ← 从源码编译
├── run_test.bat              ← 快速启动（批处理）
├── run_test.ps1              ← 快速启动（PowerShell）
├── projectMSDL/              ← 解压后的预编译工具
│   └── presets/
│       └── y2kmeter_presets/ ← 软链接到 assets/milkdrop_presets/
├── src/                      ← (仅方案B) 克隆的源码
└── install/                  ← (仅方案B) 编译产物安装目录
```

## 注意事项

- **音频源**：projectMSDL 默认采集系统音频 Loopback，播放系统声音即可驱动可视化。在设置中可以选择特定输入设备。
- **预设路径**：projectMSDL 在启动时会扫描配置的预设目录。Y2Kmeter 的 1114 个预设加载可能需要几秒。
- **版本差异**：预编译包使用 libprojectM 4.1.3，Y2Kmeter 使用 4.1.4。两个版本非常接近，差异极小。如需精确匹配版本，使用方案 B 切换到对应 tag。
- **DPI 缩放**：如果界面显示异常，尝试在兼容性设置中调整 DPI 缩放。
