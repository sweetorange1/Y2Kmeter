# Project Knowledge（项目知识速览）

## 1. 项目定位

- 产品名：`Y2Kmeter`
- 形态：音频分析插件 + Standalone 应用
- 技术栈：C++17、JUCE 8.0.12、CMake ≥ 3.22
- 平台：Windows / macOS
- Windows 默认使用静态 CRT，减少运行时依赖

## 2. 核心分层

- `PluginProcessor.*`：顶层音频处理与状态持久化入口
- `PluginEditor.*`：顶层编辑器外壳、标题栏、窗口交互、工作区承载
- `source/analysis/**`：分析层，`AnalyserHub` 是中心调度器
- `source/ui/**`：UI 框架层，`ModuleWorkspace` 与 `ModulePanel` 是核心
- `source/ui/modules/**`：各分析模块的具体 UI 实现
- `source/standalone/**`：Standalone 应用、Loopback 采集、平台差异处理

## 3. 重要架构事实

### 3.1 数据流

- 插件态：`processBlock -> AnalyserHub::pushStereo -> FrameSnapshot -> 各模块 onFrame`
- Standalone：系统音频经 `WasapiLoopbackCapture` / `MacDesktopAudioCapture` 直接推入 `AnalyserHub`
- 模块一般通过 `ModulePanel + AnalyserHub::FrameListener` 双继承接收 UI 帧

### 3.2 头文件与类组织

以下头文件包含多个强关联类，这是**有意为之**：

- `source/analysis/AnalyserHub.h`
- `source/ui/ModuleWorkspace.h`

原因是历史上为规避 MSVC include-guard / 多 TU 编译串扰而采用的合并头策略。维护时不要随意拆散。

### 3.3 pimpl 与前向声明

- `Y2KmeterAudioProcessor` 对 `AnalyserHub` 使用前向声明 + 指针隐藏实现
- `Y2KmeterAudioProcessorEditor` 对 `ModuleWorkspace` 也采用类似方式
- 涉及这些类时，尽量不要把完整依赖重新塞回公共头文件

### 3.4 OpenGL 与顶层窗口

- `PluginEditor` 侧存在 `juce::OpenGLContext` 生命周期约束
- 若调整挂载 / 分离顺序，必须考虑析构顺序、顶层窗口可见性与平台差异
- Standalone 与插件态的标题栏、全屏、窗口拖拽行为不同，不能混为一谈

## 4. 常见维护点

### 4.1 新增或删除模块类型

除了实现模块本身，还要同步检查：

- `ModuleType` 枚举
- 模块类型字符串映射
- 工厂创建逻辑
- 默认布局 / 预设布局
- 持久化与反序列化
- `CMakeLists.txt` 的源文件列表
- 如有性能计数映射，也要同步补齐

### 4.2 布局与持久化

- `ModuleWorkspace` 负责模块布局、主题、FPS、拼豆图等状态
- 布局写回经过 debounce / flush 机制，修改保存链路时要保证对称恢复
- 修改布局锁定、隐藏 chrome、窗口边界恢复等逻辑时，优先核对构造期与 `visibilityChanged` 时序

### 4.3 资源与打包

- 字体、图标、安装资源复制都受 CMake 控制
- Windows 与 macOS 的资源分发路径不同，改动 post-build 逻辑时要看双平台
- `BUNDLE_ID = cn.iisaacbeats.Y2Kmeter` 不应随意修改

## 5. 历史经验提炼

- 头文件改了但行为不变，常常不是代码没写对，而是构建缓存没失效
- 顶层窗口相关问题通常要同时检查 `PluginEditor`、Standalone 壳和平台差异代码
- 对音频线程路径的改动必须格外保守：避免锁、避免动态分配、避免阻塞
- 若需要修改主题、标题栏、全屏与窗口状态，必须先确认插件态与 Standalone 态各自预期

## 6. 推荐阅读顺序

1. 先看 `PluginProcessor.*` 和 `PluginEditor.*`
2. 再看 `AnalyserHub.*`
3. 再看 `ModuleWorkspace.*` / `ModulePanel.*`
4. 最后进入具体模块或平台代码

这样能最快建立“数据从哪里来、状态写到哪里去、窗口由谁控制”的整体心智模型。