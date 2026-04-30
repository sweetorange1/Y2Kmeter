#pragma once

#include <JuceHeader.h>
#include <juce_opengl/juce_opengl.h>
#include "PluginProcessor.h"
#include "BinaryData.h"

// 前向声明（把 ModuleWorkspace/ModulePanel/ModuleType 的头文件下沉到 .cpp，
// 以规避 MSVC 多文件编译时 include guard 串扰的问题）
class ModuleWorkspace;
class ModulePanel;
enum class ModuleType;

// ==========================================================
// Y2KmeterAudioProcessorEditor —— 多模块框架版
// 顶层职责：
//   1. Pink XP 像素风外壳（桌面棋盘格 + 凸起主窗口 + 玫瑰粉标题栏）
//   2. 内嵌 ModuleWorkspace，承载所有可拖拽/停靠的分析模块
//   3. 注入模块工厂（类型 -> 具体 ModulePanel 派生类）
// 历史 getCustomFont 静态接口保留，内部转发到 PinkXP::getFont。
// ==========================================================

class Y2KmeterAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    Y2KmeterAudioProcessorEditor(Y2KmeterAudioProcessor&);
    ~Y2KmeterAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

    // 鼠标事件：右上角关闭按钮 + 空白区域拖拽窗口（仅 Standalone 模式生效）
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

    // 兼容旧接口：返回自定义字体
    static juce::Font getCustomFont(float height, int styleFlags = juce::Font::plain);

    // ======================================================
    // 音频源下拉（对外 API —— 供 Standalone App 对接）
    //   · AudioSourceItem 结构体别名直接转自 ModuleWorkspace，避免重复声明
    //   · 通过 Editor 转交给 Workspace 的下拉框
    // ======================================================
    struct AudioSourceEntry
    {
        juce::String displayName;
        juce::String sourceId;
        bool         isLoopback = false;
    };

    void setAudioSourceItems (const juce::Array<AudioSourceEntry>& items,
                              const juce::String& selectedSourceId = {});

    // 订阅下拉切换（sourceId 为添加时传入的 ID）
    std::function<void(const juce::String& sourceId, bool isLoopback)> onAudioSourceChanged;

    // ======================================================
    // 预设 Save/Load 透传回调（由 ModuleWorkspace 的 Save/Load 按钮触发）
    //   · Editor 本身不处理 settings 文件读写 —— 仅把 workspace 的
    //     onSavePresetRequested/onLoadPresetRequested 转发到这两个回调。
    //   · Standalone App 在 createEditor 后订阅这两个回调来真正执行：
    //       save  = 把当前 PropertiesFile 物理文件另存到用户选定路径
    //       load  = 把选定文件覆盖到 PropertiesFile 物理位置并重启 App
    //   · VST3/AU 插件模式下 workspace 的预设 UI 已被隐藏，这两个回调
    //     不会被触发；Standalone App 订阅与否对插件模式无副作用。
    // ======================================================
    std::function<void(juce::File dest)> onSaveSettingsRequested;
    std::function<void(juce::File src)>  onLoadSettingsRequested;

    // ======================================================
    // 持久化辅助接口（Standalone App 在 save/restore 时使用）
    //   · chrome（顶部标题栏 + 底部 Toolbar）可见性
    //   · alwaysOnTop（抬头右侧"固定"按钮的当前状态）
    // ======================================================
    bool isChromeVisible() const;
    void setChromeVisible (bool shouldBeVisible);

    bool isAlwaysOnTopActive() const noexcept { return alwaysOnTopActive; }
    // 设置"固定置顶"状态。内部会：
    //   · 同步按钮视觉态（alwaysOnTopActive）
    //   · 若处于 Standalone 且顶层窗口可用，则对顶层窗口调用 setAlwaysOnTop
    //     （为绕开 JUCE Component::setAlwaysOnTop 的"flag 未变则 no-op"早退，
    //      强制先 false 再 true 推一次，确保系统层级真的切到 TOPMOST）
    //   · 重绘标题栏以刷新按钮按下/抬起外观
    void setAlwaysOnTopActive (bool shouldBeOnTop);

private:
    // 初始化字体、LookAndFeel、ModuleWorkspace
    void initLookAndFeel();
    void initWorkspace();           // 兼容旧调用；内部转发到 loadInitialModules
    void loadInitialModules();      // 加载默认模块（XP 风层叠瀑布）或恢复保存布局

    // 应用布局预设：
    //   presetId = 1 (LayoutPreset::defaultGrid)    = 默认布局（七个默认模块 + 默认窗口 960×640 大小）
    //   presetId = 2 (LayoutPreset::horizontalFull) = 横向铺满当前屏幕宽度（默认模块横向等分 canvas）
    //   · 由 ModuleWorkspace::onLayoutPresetChanged 触发（参数用 int 承载枚举值，
    //     以避免在此头文件里 include 完整的 ModuleWorkspace.h）
    void applyLayoutPreset (int presetId);

    // 按默认层叠瀑布布局加载七个默认模块（loadInitialModules 的"默认分支"提取）
    //   · 不检查 savedXml，只做填充；由调用方负责先清空 workspace
    void seedDefaultModules();

    // 顶部抬头：Y2K 风格标题栏（参考 XP 应用窗口顶栏）
    juce::Rectangle<int> getTitleBarBounds() const;

    // 标题栏左侧"可点击标题文字"区域（含软件名 + 版本号），点击后跳转官网
    juce::Rectangle<int> getTitleTextBounds() const;

    // 右上角三个按钮的几何（从右到左：关闭 × / 最大化固定 ★ / 最小化 _ ）
    //   样式完全复用 ModulePanel 的 × 按钮（drawRaised + pink200 hover）
    juce::Rectangle<int> getCloseButtonBounds()    const;
    juce::Rectangle<int> getPinButtonBounds()      const; // 固定（置顶）
    juce::Rectangle<int> getMinimiseButtonBounds() const; // 最小化

    // chrome 隐藏态下独立悬浮在右上角的"小关闭浮标"
    //   · 不参与标题栏的按钮组（pin / minimise 在隐藏态下不显示）
    //   · 鼠标未悬停时半透明（与底部 Hide 按钮的 dim-when-idle 语义一致）
    juce::Rectangle<int> getFloatingCloseButtonBounds() const;

    void handleCloseClicked();
    void handlePinClicked();       // 切换 alwaysOnTop
    void handleMinimiseClicked();  // 最小化顶层窗口

    // chrome 隐藏态下同步 workspace 的 hit-test 挖洞矩形
    //   · 在"浮动关闭按钮 + 标题文字"位置告诉 workspace"此区域事件别吃"，
    //     事件会冒泡回 Editor，我们在 mouseMove/Down/Up 中自行处理。
    //   · 如果模块当前覆盖了这些区域，JUCE 会优先派发给模块子组件，
    //     满足"模块可以遮挡浮层并独占鼠标"的用户需求。
    void updateWorkspaceHitTestHoles();

    // 把标题栏 / 关闭按钮按 chromeAlpha 进行半透明绘制（Hide 按钮隐藏 chrome 时触发）
    float getChromeAlpha() const;

    // 给所有模块统一下发 CPU 占用：仅依赖 Editor 的一个内部 Timer，
    //   避免 N 个模块各自每帧查一次 getCpuLoad()。
    void timerCallback() override;

    // 根据实际 FPS 与宿主模式动态调整 FrameDispatcher 频率。
    //   · 插件宿主（VST3/AU）下上限 48Hz，measuredFps 低于目标分档下调；
    //   · Standalone 下贴近用户设定，仅在 <60% 的重负载瞬间轻微降帧。
    void applyAdaptiveFrameRate (float measuredFps);

    // 模块工厂（仅 Phase A：支持 EQ；其他类型为 Phase B-D 预留）
    std::unique_ptr<ModulePanel> createModule(ModuleType type);

    // ======================================================
    // VST3 / AU 等插件模式下的预设 Save/Load 实现
    //   · Standalone 模式下这两个方法不会被调用（onSave/LoadSettingsRequested 由
    //     Y2KStandaloneApp 自己订阅做 PropertiesFile 物理拷贝 + 重启）。
    //   · 插件模式下 Editor 就地处理，不重启宿主；详见构造函数里的说明。
    //   · saveStateAsSettingsFile：把 Processor::getStateInformation 的 binary
    //     base64 化成 Standalone 兼容的 <PROPERTIES> XML 写入目标文件；
    //   · loadStateFromSettingsFile：容错读取三种格式（Standalone .settings、
    //     裸 PBEQ_State XML、裸 PBEQ_Layout XML），把布局还原到当前 processor。
    // ======================================================
    void saveStateAsSettingsFile  (const juce::File& dest);
    void loadStateFromSettingsFile (const juce::File& src);

    // 字体 typeface（每个 Editor 实例持有一份；避免 static 存储期
    // 与插件 DLL 生命周期错配导致宿主卸载插件时崩溃）
    juce::Typeface::Ptr customTypeface;

    // Logo 图片（从 BinaryData::logo_png 解码并缓存到实例成员；
    // 不使用 ImageCache 以避免跨 DLL 卸载时的悬垂引用）
    juce::Image logoImage;

    Y2KmeterAudioProcessor& processor;

    // true = 被 DAW 宿主加载的插件模式（VST3 / AU / AAX / LV2 等）；
    // false = Standalone 模式。
    //   · 插件模式下：宿主提供音窗口边框，因此**不画**自画伪标题栏、不接管
    //     窗口拖拽 / 关闭 / 置顶 / 最小化按钮；底部 toolbar 里的"信号源"下拉
    //     也隐藏（宿主直接给音频，不需要选择设备）。
    //   · 由构造函数一次性根据 processor.wrapperType 初始化后不再变化。
    const bool isPluginHost;

    // pimpl ：头里只用前向声明的指针，完整类型只在 .cpp 中可见
    std::unique_ptr<ModuleWorkspace> workspace;

    // 主题变更订阅 token
    int themeSubToken = 0;

    // —— 顶部标题栏 + 右上角三个按钮（样式与 ModulePanel 的 × 一致）——
    static constexpr int titleBarHeight     = 26;   // XP 风标题栏高度
    static constexpr int closeButtonSize    = 18;   // 按钮尺寸（嵌入标题栏右侧）
    static constexpr int closeButtonMargin  = 4;    // 最右按钮距标题栏右边缘
    static constexpr int titleButtonGap     = 3;    // 三个按钮之间的水平间距
    bool closeButtonHovered   = false;
    bool closeButtonPressed   = false;
    bool pinButtonHovered     = false;
    bool pinButtonPressed     = false;
    bool minButtonHovered     = false;
    bool minButtonPressed     = false;
    bool alwaysOnTopActive    = true;  // 固定按钮当前状态：默认启用（置顶不被遮挡）
    bool initialAlwaysOnTopApplied = false; // 首次 visibilityChanged 时把默认置顶应用到顶层窗口的一次性 flag
    bool titleTextHovered     = false; // 左侧标题文字 hover 态（hover 时显示下划线 + 手型光标）

    // 标题文字实际绘制的总像素宽度（由 paint() 计算后回写，供 mouseMove / mouseDown
    //   做精确的命中测试，避免把整条 titleTextBounds 都当作可点击热区）
    mutable int cachedTitleTextW = 0;

    // —— chrome（标题栏 + 关闭按钮）可见性（订阅 workspace 的 chrome 状态）——
    //   chrome 隐藏时 chromeDim=true → 标题栏 + × 以 15% alpha 绘制；
    //   鼠标悬停到 Editor 上自动恢复不透明。
    bool chromeDim = false;
    bool mouseInsideEditor = false;

    // —— 无边框窗口拖拽支持（仅在 Standalone 下起作用，VST3 宿主中无害）——
    //   拖拽只允许从标题栏发起（而不是任意空白区）
    juce::ComponentDragger  windowDragger;
    bool                    draggingWindow = false;

    // —— FPS 统计（通过内部 FrameListener 辅助类订阅 AnalyserHub）——
    //   为绕开 MSVC include guard 串扰问题，把 AnalyserHub 完整头封装在 .cpp
    //   中，这里只前向声明 + std::unique_ptr pimpl。
    class FpsFrameListener;
    std::unique_ptr<FpsFrameListener> fpsListener;

    // —— Chrome 隐藏态浮层（顶层 child，z-order 高于 workspace）——
    //   chrome 隐藏时负责：① 底图上打印软件名/版本号/官网文字；
    //                     ② 右上角悬浮半透明关闭按钮（hover 时恢复不透明并可点击）。
    //   通过独立 Component 实现，避免 workspace（setInterceptsMouseClicks true）
    //   覆盖掉 Editor 上的鼠标事件。
    class ChromeHiddenOverlay;
    std::unique_ptr<ChromeHiddenOverlay> chromeHiddenOverlay;

    std::atomic<juce::int64> frameCounter { 0 };
    juce::int64              lastFrameCounterSample = 0;
    double                   lastFpsTimeMs          = 0.0;

    // 自适应 FPS 调度状态：
    //   · userRequestedFpsLimit —— workspace 顶部 FPS 切换按钮设定的目标上限
    //   · adaptiveDispatchHz    —— 当前让 AnalyserHub 实际跑的频率（自适应降降升升的目标值）
    //   · adaptiveRecoverTicks  —— 测标持续达标后的连续计数，避免单帧抖动回升
    int                      userRequestedFpsLimit  = 30;
    int                      adaptiveDispatchHz     = 30;
    int                      adaptiveRecoverTicks   = 0;

    // Tamagotchi 信号保活：仅当工作区存在 Tamagotchi 模块时，
    // 才临时 retain(Loudness) 以驱动孵化/行为状态机；无 Tamagotchi 时立刻 release。
    bool tamagotchiSignalRetained = false;

    // —— GPU 合成层（统一迁移绘制到 GPU，Standalone + VST3 共用）——
    //   · 在 Editor 构造末尾 attachTo(*this)，析构最开始 detach()。
    //   · attach 后 JUCE 会把所有子组件的 paint() 命令翻译成 OpenGL 批次，
    //     底层把软光栅的 fillCGRect / drawGlyphs / fillPath 全部交给 GPU 执行。
    //   · 插件宿主（VST3/AU）下也安全：JUCE 会为 Editor 的顶层 NSView/HWND 自动
    //     创建一个子 GL 子层，不会影响宿主窗口其余部分；DAW 卸载 Editor 时
    //     detach 会清理 GL 资源。
    //   · 本成员**必须**在类的末尾（所有子组件之后）声明，保证析构时最先执行
    //     （反向声明顺序），而我们在 ~Editor 起始处已显式 detach 一次兜底。
    juce::OpenGLContext openGLContext;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Y2KmeterAudioProcessorEditor)
};