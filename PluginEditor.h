#pragma once

#include <JuceHeader.h>
#include <juce_opengl/juce_opengl.h>
#include "PluginProcessor.h"
#include "BinaryData.h"
#include "projectM-4/types.h"  // projectm_handle
#include <memory>

// 前向声明（把 ModuleWorkspace/ModulePanel/ModuleType 的头文件下沉到 .cpp，
// 以规避 MSVC 多文件编译时 include guard 串扰的问题）
class ModuleWorkspace;
class ModulePanel;
enum class ModuleType;
class MilkdropTintPass;  // 整体染色后处理着色器，完整定义在 MilkdropModule.h
class ToyDirector;       // Toybox 点击互动主管（完整定义在 source/ui/toys/ToyKit.h）

// ==========================================================
// Y2KmeterAudioProcessorEditor —— 多模块框架版
// 顶层职责：
//   1. Pink XP 像素风外壳（桌面棋盘格 + 凸起主窗口 + 玫瑰粉标题栏）
//   2. 内嵌 ModuleWorkspace，承载所有可拖拽/停靠的分析模块
//   3. 注入模块工厂（类型 -> 具体 ModulePanel 派生类）
// 历史 getCustomFont 静态接口保留，内部转发到 PinkXP::getFont。
// ==========================================================

class Y2KmeterAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     public juce::OpenGLRenderer,
                                     private juce::Timer
{
public:
    // ==============================================================
    // OpenGLRenderer —— projectM & GPU 模块在 Editor 上下文中渲染
    //   · newOpenGLContextCreated: GLEW init → 扫描预设/纹理路径 → create handle
    //   · renderOpenGL: projectM帧 + 未来 GPU 模块 → Editor CachedImage FBO
    //   · openGLContextClosing: destroy handle → reload DLL
    // ==============================================================
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    // 只销毁 Editor 侧 Milkdrop 渲染资源（projectM handle + FBO + 染色后处理），
    // 不触碰 GlDirectModule 直渲染模块的 VBO/FBO/shader。供 Milkdrop 脱离
    // （SuspendMilkdropEditorRendererForFloating）时挂起 Editor renderer 使用。
    // 必须在 GL 线程上调用。
    void destroyMilkdropEditorResources();

    // ---- projectM 桥接 API（供 MilkdropModule 调用）----
    // PCM 推送（UI 线程安全，内部加锁）
    void PushMilkdropPcm(const float* interleaved_lr, unsigned int frame_count);
    // 预设切换请求（原子，renderOpenGL 消费）
    void RequestMilkdropPresetDelta(int delta);
    void RequestMilkdropPresetJump(int index);
    void RequestMilkdropPresetRandom();
    // 渲染缩放（循环 1→2→4→1，来自 MilkdropModule Overlay 的 [1:n] 按钮）
    void RequestMilkdropRenderScale();
    int  GetMilkdropRenderScale() const noexcept { return milkdrop_render_scale_; }

    // ---- Milkdrop 收藏库（like library）桥接 ----
    // 当前是否浏览收藏库（全局共享，仅一个 Milkdrop 模块）。
    bool IsMilkdropUseLikeLibrary() const noexcept { return milkdrop_use_like_library_.load(); }
    // UI 线程调用：请求切换预设库（内置库 ↔ 收藏库）。GL 线程在 renderOpenGL 中
    // 消费该请求，重扫预设列表并加载切换后库的第一个预设。
    void RequestMilkdropToggleLibrary();
    // 仅翻转收藏库状态并持久化，不触发 Editor GL 线程消费。
    // 供浮动态 / macOS（GLView 本地渲染路径）使用：GLView 用自己的请求标志重扫，
    // 避免 Editor 的 toggle 标志在挂起期间残留、dock 回嵌入态时被误消费。
    void ToggleMilkdropLibraryState();
    // 直接设置收藏库状态并持久化（供删空收藏库后回退到内置库等场景使用）。
    void SetMilkdropUseLikeLibrary(bool use_like);
    // 取消收藏当前预设后请求重扫预设列表（嵌入态；renderOpenGL 消费）。
    // 当前浏览收藏库时，删除当前预设后列表少一项，需重扫并切换到下一个预设；
    // 若收藏库因此被删空，则自动回退到内置库。
    void RequestMilkdropUnlinkReload();
    // 收藏库是否至少含一个 .milk 预设（决定切换按钮是否显示）。
    bool HasMilkdropLikedPresets() const;
    // 当前预设的完整文件路径（嵌入态专用，UI 线程安全）。
    juce::String GetMilkdropCurrentPresetFilePath() const;
    // 诊断
    bool IsMilkdropRenderReady() const noexcept { return milkdrop_render_ready_; }
    juce::String GetMilkdropError() const;
    int  GetMilkdropCurrentPresetIndex() const noexcept;
    int  GetMilkdropTotalPresets() const noexcept;
    juce::String GetMilkdropCurrentPresetName() const;
    int64_t GetMilkdropLastPresetSwitchTimeMs() const noexcept;
    // 浮动窗口共享帧：renderOpenGL 抓取离线 FBO → 浮动 MilkdropModule 读取绘制
    const juce::Image& GetMilkdropSharedFrame() const noexcept { return milkdrop_shared_frame_; }
    // 浮动态 Milkdrop 使用自身 native OpenGL surface 渲染时，临时释放 Editor
    // 持有的 projectM handle，避免 Windows/GLEW 下跨上下文多 handle 崩溃。
    void SuspendMilkdropEditorRendererForFloating();
    void ResumeMilkdropEditorRendererAfterFloating();

    // 整体视觉状态（master output colors 染色 + bright/invert/shadows 等效果），
    // 全局共享（所有 Milkdrop 模块同一状态）。UI 线程调用 SetMilkdropVisualState 写，
    // GL 线程调用 GetMilkdropVisualState 读（内部加锁）；写时同步回 Processor 以便持久化。
    void SetMilkdropVisualState(const MilkdropVisualState& state);
    MilkdropVisualState GetMilkdropVisualState() const;

    // 简单波形样式覆盖（wave_mode/x/y/r/g/b/a/mystery/usedots/...），全局共享。
    // UI 线程写、GL 线程读（内部加锁），写时同步回 Processor 以便持久化。
    void SetMilkdropWaveState(const MilkdropWaveState& state);
    MilkdropWaveState GetMilkdropWaveState() const;

    // 把当前渲染状态（visual/wave/auto）回写到宿主参数。
    //   public 供 MilkdropModule 在 UI 修改 auto 轮播状态后调用，使宿主参数
    //   跟随 UI 状态，避免 10Hz pull 反向覆盖。Standalone 下为空操作。详见 .cpp。
    void pushMilkdropAutomationParams();

    Y2KmeterAudioProcessorEditor(Y2KmeterAudioProcessor&);
    ~Y2KmeterAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    void childrenChanged() override;

    // 鼠标事件：右上角关闭按钮 + 空白区域拖拽窗口（仅 Standalone 模式生效）
    void mouseMove  (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit  (const juce::MouseEvent&) override;
    void mouseDown  (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    // 双击标题栏空白区（非按钮、非标题文字热区）→ 切换顶层窗口全屏 / 还原
    //   · 仅在 Standalone 且非 chrome 隐藏态下生效；插件宿主模式忽略。
    void mouseDoubleClick (const juce::MouseEvent&) override;

    // 键盘快捷键（v2.5.7）：加载 Milkdrop 模块时，← → 切预设，空格随机预设。
    //   仅在软件有焦点且当前存在 Milkdrop 模块（嵌入态或脱离态）时生效，
    //   否则返回 false 让事件继续传播，避免干扰其他键盘交互。
    bool keyPressed(const juce::KeyPress& key) override;

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

    // 当窗口被程序性设置成超过编辑器 constrainer 上限的尺寸（如启动时恢复历史
    //   bounds、双击伪全屏）时，把 constrainer 的 max 放宽到能容纳 w×h（min 不变）。
    //   否则后续任何 setResizeLimits 都会经 setBoundsConstrained 把编辑器夹回 1600，
    //   再经 ResizableWindow 的 resizeToFitContent 反噬把窗口一并压小。
    void relaxConstrainerForWindowSize (int w, int h);

    bool isAlwaysOnTopActive() const noexcept { return alwaysOnTopActive; }
    // 设置"固定置顶"状态。内部会：
    //   · 同步按钮视觉态（alwaysOnTopActive）
    //   · 若处于 Standalone 且顶层窗口可用，则对顶层窗口调用 setAlwaysOnTop
    //     （为绕开 JUCE Component::setAlwaysOnTop 的"flag 未变则 no-op"早退，
    //      强制先 false 再 true 推一次，确保系统层级真的切到 TOPMOST）
    //   · 重绘标题栏以刷新按钮按下/抬起外观
    void setAlwaysOnTopActive (bool shouldBeOnTop);

    // 从持久化恢复 FPS 限制值（Standalone 启动时调用）
    void RestoreFpsLimit (int hz);

    // 返回用户选择的 FPS 限制档位（30/60/120/0，0=无上限）
    int  GetUserRequestedFpsLimit() const noexcept { return userRequestedFpsLimit; }

    // 从持久化恢复 FFT size 档位（反馈 #7，Standalone 启动时调用）
    void RestoreFftSize (int size);
    // 返回控制台当前的 FFT size 档位
    int  GetUserRequestedFftSize() const noexcept;

    // ======================================================
    // Toybox（v3.0.0）档位（0=Off / 1=Chill / 2=Normal / 3=Party）
    //   · setToyLevel：同步 ToyDirector + workspace TOY 按钮 + Processor 持久化。
    //   · getToyLevel：读取当前档位（供 Standalone 退出时保存 ui.toyLevel）。
    // ======================================================
    void setToyLevel (int level);
    int  getToyLevel() const noexcept;

    // 是否插件宿主模式（VST3/AU 等，宿主提供 playhead）；Standalone 返回 false。
    //   · Milkdrop BPM 同步自动切换模式仅在此为 true 时显示入口。
    bool isPluginHostMode() const noexcept { return isPluginHost; }

    // 转发 Processor 的宿主播放位置快照（供 Milkdrop BPM 同步模式读取）。
    HostPlayheadSnapshot getHostPlayheadSnapshot() const noexcept;

private:
    // 初始化字体、LookAndFeel、ModuleWorkspace
    void initLookAndFeel();
    void initWorkspace();           // 兼容旧调用；内部转发到 loadInitialModules
    void loadInitialModules();      // 加载默认模块（XP 风层叠瀑布）或恢复保存布局

    // 应用布局预设：
    //   presetId = 1 (defaultGrid)        = 默认布局（十个默认模块 + 默认窗口 960×640）
    //   presetId = 2 (horizontalFull)     = 横向铺满当前屏幕宽度（贴屏幕顶部）
    //   presetId = 3 (horizontalBottom)   = 横向铺满当前屏幕宽度（贴屏幕底部）
    //   presetId = 5 (mv)                 = 全屏 + 上方横向模块条 + 下方 Milkdrop
    //   presetId = 6 (studioMonitor)      = 双行监听（仪表行 + 波形行）
    //   presetId = 7 (focusSidecar)       = 主视觉 + 右侧仪表列
    //   presetId = 9 (broadcastLoudness)  = 广播响度合规
    //   presetId = 10 (onlyMilkdrop)      = 全屏 + 单个最大化 Milkdrop
    //   · 由 ModuleWorkspace::onLayoutPresetChanged 触发（参数用 int 承载枚举值，
    //     以避免在此头文件里 include 完整的 ModuleWorkspace.h）
    void applyLayoutPreset (int presetId);

    // stick 模式是否激活（Windows 专属；非 Windows 恒 false，paint 无需 #if 即可判断）
    bool isStickActive() const noexcept { return stickActive; }

    // ---- 布局三槽（v2.8.5）----
    // 当前活动槽：0 = 非 stick / 1 = stick 顶(T) / 2 = stick 底(B)。
    //   所有"保存当前布局"的调用都必须走 persistCurrentLayoutToProcessor()，
    //   否则 stick 期间的改动会写进非 stick 槽，把用户常驻布局污染掉。
    int  activeLayoutSlot() const noexcept;
    void persistCurrentLayoutToProcessor();

    // 把"当前实际 stick 状态"写回 Processor。
    //   用于导入存档后校正：存档里的 stickActive 描述的是导出方当时的状态，
    //   直接保留会让本机下次启动莫名进入 stick。
    void syncStickSessionFlagToProcessor();

#if JUCE_WINDOWS
    // ---- stick 模式（Windows 专属：AppBar 硬 stick 驻留）----
    //   进入：备份当前布局 + 窗口 bounds → 切换 Horizontal Bar(T) → AppBar 驻留
    //         （SHAppBarMessage 缩小屏幕工作区，其他窗口被挤走）→ 隐藏最小化/置顶按钮
    //   退出：取消 AppBar → 恢复备份布局 + 窗口 bounds → 恢复按钮
    //   T/B：在顶端（horizontalFull）/ 底端（horizontalBottom）之间切换
    //   三槽：非 stick / stick 顶 / stick 底 各自独立保存，互不覆盖。
    void enterStickMode();
    void exitStickMode();
    void toggleStickTopBottom();

    // 启动时恢复上次的 stick 会话（窗口上屏后由 timerCallback 调用一次）。
    //   · 存档里有 stickActive=true 时，loadInitialModules 已先装载对应 stick 槽；
    //     这里补做窗口高度 + AppBar 驻留 + 按钮组切换。
    void restoreSavedStickSession();
    // 从存档加载指定 stick 槽（1=顶 / 2=底）；槽为空或解析失败返回 false。
    bool loadSavedStickSlotLayout (int slot);

    // 不重建模块布局，只把窗口搬到"屏幕宽 × 指定 Editor 高"并贴顶/贴底。
    //   用于恢复存档槽（用户上次调好的条高）与启动恢复会话。
    void applyStickBarGeometry (bool bottom, int targetEditorHeight);

    // 存档用的 stick 高度：hide 态时折算回 show 态等效高度（与 chrome 展开一致）
    int  stickEditorHeightForSave() const noexcept;

    bool pendingStickRestore = false;   // 启动待恢复 stick 会话
    bool pendingStickBottom  = false;   // 待恢复方向（false=T / true=B）

    // 驻留期间窗口高度被改变（用户拖拽底边）后，延时重新 SETPOS，
    // 让系统工作区占位与窗口实际高度保持一致；否则会出现空档/压住其他窗口。
    //   · 只比较物理高度，10Hz 调用开销可忽略；
    //   · 需等高度稳定 ~200ms 再重排，避免拖拽过程中频繁 SETPOS。
    void updateStickDockGeometry();
#endif

    // 把顶层窗口调整到目标尺寸（居中于当前屏幕 userArea），处理 resizeLimits
    // 放开/锁定、standalone setBounds（含边框）与插件 setSize 的差异，并反推
    // overhead 使 canvas 高度对齐到 8px 网格。返回对齐后的 canvas 区域。
    juce::Rectangle<int> resizeToTargetCanvas (int targetW, int targetH);

    // 在 canvas 从 rowY 起、高 rowH 的横向条内，按加权宽度比例（ratios 为
    // nullptr 时均分）摆放 count 个模块，宽度对齐 8px 网格并占满 canvas 宽度。
    // 返回该行模块的实际底部 Y（供调用方在其下方继续布局）。
    int placeModuleRow (juce::Rectangle<int> canvas, int rowY, int rowH,
                        const ModuleType* order, int count, const float* ratios);

    // 按默认层叠瀑布布局加载十个默认模块（loadInitialModules 的"默认分支"提取）
    //   · 不检查 savedXml，只做填充；由调用方负责先清空 workspace
    void seedDefaultModules();

    // 顶部抬头：Y2K 风格标题栏（参考 XP 应用窗口顶栏）
    juce::Rectangle<int> getTitleBarBounds() const;

    // 标题栏左侧"可点击标题文字"区域（含软件名 + 版本号），点击后跳转官网
    juce::Rectangle<int> getTitleTextBounds() const;

    // 右上角四个按钮的几何（从右到左：关闭 × / 固定 ★ / 最小化 _ / 锁定 ⮌ ）
    //   样式完全复用 ModulePanel 的 × 按钮（drawRaised + pink200 hover）
    juce::Rectangle<int> getCloseButtonBounds()    const;
    juce::Rectangle<int> getPinButtonBounds()      const; // 固定（置顶）
    juce::Rectangle<int> getMinimiseButtonBounds() const; // 最小化
    juce::Rectangle<int> getLockButtonBounds()     const; // 布局锁定（v1.8.3 新增）

    // chrome 隐藏态下独立悬浮在右上角的"小关闭浮标"
    //   · 不参与标题栏的按钮组（pin / minimise 在隐藏态下不显示）
    //   · 鼠标未悬停时半透明（与底部 Hide 按钮的 dim-when-idle 语义一致）
    juce::Rectangle<int> getFloatingCloseButtonBounds() const;

    // 根据当前窗口所在显示器（或主显示器）的 userArea 计算并应用 UI 密度缩放。
    //   低分辨率屏幕会得到 <1.0 的缩放，让标题栏 / 工具栏 / 模块 chrome 更紧凑，
    //   为音频仪表显示区让出更多空间。缩放因子变化时触发一次 resized() 重新布局。
    void updateUiScaleForCurrentDisplay();
    void moved() override;

    // ---- 宿主自动化参数双向同步（pull：宿主 → 渲染）----
    //   在 timerCallback 里以 10Hz 轮询，检测参数变化并应用到 Milkdrop 状态。
    //   仅插件宿主形态生效（Standalone 无宿主参数，直接返回）。详见 .cpp。
    void pullMilkdropAutomationParams();

    // 只把宿主的 color/effects/tweak/wave 参数应用到渲染状态（不含 auto/preset、
    // 不刷新 UI 控件）。由参数 listener 在宿主改动参数时立即调用，避免 10Hz
    // pull 采样造成的"阶梯式"视觉变化（手动拖动是逐事件高频，宿主 automation
    // 则是逐音频块高频，10Hz 采样会漏掉中间值）。
    // 返回是否有状态发生变化（供 10Hz pull 据此决定是否 refreshStateFromEditor）。
    bool applyVisualAndWaveFromHostParams();

    void handleCloseClicked();
    void handlePinClicked();       // 切换 alwaysOnTop
    void handleMinimiseClicked();  // 最小化顶层窗口
    void handleLockClicked();      // 切换布局锁定（v1.8.3）

    // 伪全屏切换（v2.5.8：修复 setFullScreen 覆盖任务栏 + PopupMenu 不可见）
    //   · 背景：Y2KMainWindow 是无原生标题栏 DocumentWindow；JUCE 在 Windows 上
    //     setFullScreen(true) = ShowWindow(SW_SHOWMAXIMIZED)；对无 WS_CAPTION 的
    //     窗口而言，此调用会把窗口撑到 monitor 物理边界（覆盖任务栏），并且此时
    //     顶层是 alwaysOnTop=true，PopupMenu / ComboBox 弹窗与主窗口平级 topmost，
    //     会被主窗口 Z 序压住导致"看不见弹出内容"。
    //   · 修复：手动把顶层窗口 setBounds 到 Displays::getDisplayForRect().userArea
    //     （即当前显示器的"工作区"，Windows 已自动扣除任务栏），并暂时把 resize
    //     limits 放宽。退出时把 bounds 与 resize limits 都还原。
    //   · 与 layoutLocked 兼容：进入伪全屏前若已锁定，则临时放开 resize limits；
    //     退出后重新 apply。
    void toggleFakeFullScreen();

    // 将 layoutLocked 同步到 workspace / 顶层窗口的 setResizable / Processor 持久化。
    //   · shouldRepaintButton 为 true 时刷新 lock 按钮局部（仅在用户交互时需要）；
    //   · initial 为 true 时代表从持久化引入初值，不回写 Processor（避免未变同步）。
    void applyLayoutLocked (bool locked, bool initial = false);
    bool isLayoutLocked() const noexcept { return layoutLocked; }

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

    // 保持 FrameDispatcher 与用户设定上限一致。
    //   · 采用"封顶"语义：未触及上限前不做干预，超出上限时 FrameDispatcher 即起限制作用。
    //   · 插件（VST3/AU）与 Standalone 统一逻辑，不再区分宿主模式。
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

    // 桌面背景缓存：drawDesktop() 较重，主题/尺寸变化时重建一次，paint() 只贴图。
    void invalidateDesktopCache() noexcept;
    void rebuildDesktopCacheIfNeeded();

    // 字体 typeface（每个 Editor 实例持有一份；避免 static 存储期
    // 与插件 DLL 生命周期错配导致宿主卸载插件时崩溃）
    juce::Typeface::Ptr customTypeface;

    // Logo 图片（从 BinaryData::logo_png 解码并缓存到实例成员；
    // 不使用 ImageCache 以避免跨 DLL 卸载时的悬垂引用）
    juce::Image logoImage;

    juce::Image desktopCacheImage;
    juce::Rectangle<int> desktopCacheBounds;
    bool desktopCacheDirty = true;

    Y2KmeterAudioProcessor& processor;

    // pull 期间置位，抑制 SetMilkdropVisualState 内部触发的 push，
    // 避免 pull 中途用"尚未更新的状态"回滚宿主刚写入的参数。
    bool pullingParams_ = false;
    // 预设切换开关的上一次轮询值，用于检测宿主 off→on 上升沿（触发一次动作）。
    bool lastPresetNext_   = false;
    bool lastPresetPrev_   = false;
    bool lastPresetRandom_ = false;
    // 上一次已回写到 md_preset_index 的渲染索引（-1 = 尚未同步）。
    //   timerCallback 检测渲染索引变化 → push 回写参数（持久化当前预设）。
    int lastSyncedPresetIndex_ = -1;

    // 宿主参数变化监听器：宿主（automation/MIDI CC）改参数时，APVTS 可能在**音频
    // 线程**回调 parameterChanged（VST3 宿主在音频线程 setNormalized）。这里用
    // AsyncUpdater 把工作转到消息线程（触发即合并，每帧最多一次），既消除 10Hz
    // pull 采样造成的阶梯，又避免在音频线程触碰 UI / 非原子状态。
    class HostParamListener : public juce::AudioProcessorValueTreeState::Listener,
                              private juce::AsyncUpdater
    {
    public:
        explicit HostParamListener (Y2KmeterAudioProcessorEditor& e) : editor_ (e) {}
        ~HostParamListener() override { cancelPendingUpdate(); }
        void parameterChanged (const juce::String&, float) override
        {
            triggerAsyncUpdate();
        }
        void handleAsyncUpdate() override
        {
            const bool wasPulling = editor_.pullingParams_;
            editor_.pullingParams_ = true;
            editor_.applyVisualAndWaveFromHostParams();
            editor_.pullingParams_ = wasPulling;
        }
    private:
        Y2KmeterAudioProcessorEditor& editor_;
    };
    std::unique_ptr<HostParamListener> hostParamListener_;

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
    bool lockButtonHovered    = false;   // v1.8.3
    bool lockButtonPressed    = false;
    bool alwaysOnTopActive    = true;  // 固定按钮当前状态：默认启用（置顶不被遮挡）
    bool layoutLocked         = false; // v1.8.3：布局锁定态（抱锁后不可拖动/resize）

    // stick 模式是否激活（Windows 专属；非 Windows 恒 false，供 paint 判断隐藏按钮）
    bool stickActive = false;

#if JUCE_WINDOWS
    // ---- stick 模式状态（Windows 专属）----
    bool                 stickBottomAligned  = false;  // false=T(顶), true=B(底)
    juce::ValueTree      stickBackupLayout;            // 进入 stick 前的模块布局备份
    juce::Rectangle<int> stickBackupWindowBounds;       // 进入 stick 前的窗口 bounds 备份
    // 进入 stick 前 constrainer 的 resizeLimits（退出时恢复宽度锁定）
    int stickSavedMinW = 0, stickSavedMinH = 0, stickSavedMaxW = 0, stickSavedMaxH = 0;
    // 驻留态窗口的物理高度（px）：当前已提交给 AppBar 的厚度 / 正在等待稳定的新高度
    int stickDockedHeightPx  = 0;
    int stickPendingHeightPx = 0;
    int stickStableTicks     = 0;
    // AppBar 最终提交的物理矩形（px）；窗口若被 JUCE/系统挪走，timer 会据此搬回去
    juce::Rectangle<int> stickDockRectPx;
    // stick 期间把顶层 constrainer 的"最小可见量"清零：否则任何 setBoundsConstrained /
    //   边框拖拽都会把窗口夹进 userArea（= 被自己占位压缩后的工作区）→ 窗口被推下去。
    int  stickSavedOnscreenTop = 0, stickSavedOnscreenLeft = 0,
         stickSavedOnscreenBottom = 0, stickSavedOnscreenRight = 0;
    bool stickOnscreenSaved = false;
    void redockStickAfterGeometryChange();
    void setStickOnscreenRelaxed (bool relaxed);
#endif

    // v2.8.2：L 只是交互闸门，不再保存/改写 resizeLimits（旧 savedLock* / pendingLockApplyOnAttach 已移除）
    bool initialAlwaysOnTopApplied = false; // 首次 visibilityChanged 时把默认置顶应用到顶层窗口的一次性 flag

    // 伪全屏状态（v2.5.8）
    //   · pseudoFullScreen: 当前是否处于"伪最大化"（用户 double-click 标题栏触发）
    //   · preFullScreenBounds: 进入前顶层窗口 bounds，用于退出时还原
    //   · preFullScreenLockMin/Max*: 进入前 constrainer 的 resize limits，用于退出时还原
    //     （与 layoutLocked 的 savedLock*字段独立，避免互相污染）
    bool                 pseudoFullScreen = false;
    juce::Rectangle<int> preFullScreenBounds;
    int                  preFullScreenMinW = 0, preFullScreenMinH = 0;
    int                  preFullScreenMaxW = 0, preFullScreenMaxH = 0;
    bool                 preFullScreenLimitsValid = false;

    // 编辑器自身 constrainer 的进入前备份（v2.8.x 修复）：伪全屏若只放宽顶层窗口
    //   constrainer、漏掉编辑器自己的 1600×1100，则后续任何编辑器级 setResizeLimits
    //   都会把编辑器夹回 1600 宽，再经 ResizableWindow 的 resizeToFitContent 反噬
    //   把窗口一并压小。进入伪全屏时同步放宽编辑器 constrainer，退出时还原。
    int  preFullScreenEditorMinW = 0, preFullScreenEditorMinH = 0;
    int  preFullScreenEditorMaxW = 0, preFullScreenEditorMaxH = 0;
    bool preFullScreenEditorLimitsValid = false;

    // Windows 下强制关闭 Direct2D 渲染器的一次性 flag。
    //   背景：JUCE 8 默认在 Windows 上启用 Direct2D。VST3 DLL 被宿主
    //   FreeLibrary 卸载时，CRT 在持有 loader lock 的上下文里开始执行
    //   atexit 析构链；其中 JUCE 的 SharedResourcePointer<DxgiAdapters> 静态
    //   缓存会释放 ID3D11Device，触发 AMD 的 atidxx64.dll::CDevice::DestroyDriverInstance
    //   内部 Sleep 等 GPU fence——因 loader lock导致驱动 worker thread 无法推进，
    //   主线程无限 Sleep → FL Studio 刷新删除插件时卡死。
    //   解法：首次顶层 peer 可用时把渲染引擎切到软光栅/GDI（0 = software renderer），
    //   避免 DLL 生命周期内初始化 Direct2D 全局资源。
    bool renderingEngineConfigured = false;

    // Editor 析构流程已启动——用于在 resized() 中拒绝把析构末端
    //   宿主/JUCE 塞来的极小尺寸（实测 FL Studio 关闭插件窗口时曾传入 ~175×85）
    //   回写到 Processor.savedEditorSize，从而避免下次打开窗口恢复到异常小尺寸。
    bool editorBeingDestructed = false;
    bool titleTextHovered     = false; // 左侧标题文字 hover 态（hover 时显示下划线 + 手型光标）

    // 标题文字实际绘制的总像素宽度（由 paint() 计算后回写，供 mouseMove / mouseDown
    //   做精确的命中测试，避免把整条 titleTextBounds 都当作可点击热区）
    mutable int cachedTitleTextW = 0;

    // =====================================================================
    // 窗口行为模型（v2.8.2 重构）
    //   三个概念相互独立、职责单一：
    //   ① L（layoutLocked）：纯"交互闸门"——禁止拖动/缩放窗口、禁止编辑模块。
    //      不再改 resizeLimits、不再移动/缩放窗口；切换 L 永远不会让窗口跳动。
    //   ② Hide（autoHideMode）：标题栏 + 工具栏收起。
    //      · 进入/退出时窗口一次性收缩/展开 chrome 高度（上半屏顶边固定、下半屏
    //        底边固定；stick 时固定驻留边），基于"当前"窗口计算，不使用旧快照。
    //      · 悬停时 chrome 以浮层（overlay）形式盖在内容之上显示，窗口几何
    //        **完全不变** —— 不会再出现悬停闪现 / 拖动后被拉回原位。
    //   ③ Stick：AppBar 驻留，仅在几何真正改变（进入/退出/切边/Hide 切换/
    //      用户拖高度）时重新提交占位，悬停不会触发任何占位变化。
    // =====================================================================
    bool chromeDim = false;            // 标题栏不占位（== autoHideMode 的布局表现）
    bool mouseInsideEditor = false;

    bool autoHideMode   = false;       // Hide 已开启（持久化为 ui.chromeVisible=false）
    bool chromeRevealed = false;       // Hide 下鼠标悬停 → chrome 浮层显示中
    bool revealArmed    = false;       // Hide 刚开启时需鼠标先离开窗口一次才允许浮现
    juce::uint32 revealLastInsideMs = 0;

    // Hide 期间放宽 Editor 的最小高度（否则窗口收缩 chrome 高度后会被 limits 拉回）
    bool hideLimitsSaved = false;
    int  hideSavedMinW = 0, hideSavedMinH = 0, hideSavedMaxW = 0, hideSavedMaxH = 0;

    // Hide 悬停检测：仅在 Hide 期间以 30Hz 轮询鼠标位置（无需任何 MouseListener）
    struct LambdaTimer : public juce::Timer
    {
        std::function<void()> callback;
        void timerCallback() override { if (callback) callback(); }
    };
    LambdaTimer chromeRevealPoller;

    // 标题栏是否可交互：chrome 常驻，或 Hide 下浮层正在显示
    bool isTitleBarInteractive() const noexcept { return ! chromeDim || chromeRevealed; }

    void setAutoHideMode (bool shouldHide);
    void setChromeRevealed (bool shouldReveal);
    void updateChromeReveal();
    int  getChromeHeightPx() const noexcept;
    void drawTitleBar (juce::Graphics& g);

    // L 闸门：启用/禁用顶层窗口边框 + Editor 右下角的缩放手柄（不碰 resizeLimits）
    void applyUserResizeGate();

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

    // —— v2.9 真实出帧统计 ——
    //   frameCounter 统计的是"分发次数"，GL 线程跟不上时多个 repaint 会被合并成
    //   一帧，分发数仍是 120 但屏幕实际只换了更少的帧。glFrameCounter 在
    //   renderOpenGL()（GL 线程，每次真正出帧一次）里 +1，FPS 标签以它为准。
    std::atomic<juce::int64> glFrameCounter { 0 };
    juce::int64              lastGlFrameCounterSample = 0;

    // —— v2.9.1 vblank 对齐的帧时钟（wall-clock 判据 + 兜底 Timer）——
    //   VBlankAttachment 在 UI 线程按显示器 vblank 回调，只用来"对齐相位"；
    //   分发频率由墙钟判据保证：
    //     · vblank 入口：距上次分发 ≥ 0.85×目标间隔才分发（错过一个 vblank 后
    //       下一个回调立即追帧，不会因 JUCE AsyncUpdater 的合并语义被永久吞掉）；
    //     · 兜底 Timer：距上次分发 ≥ 1.0×目标间隔即补帧 —— vblank 回调被 GL
    //       持锁阻塞而大量丢弃时（JUCE Windows VBlankThread 在消息线程没处理完
    //       上一个回调时会丢事件），仍能维持用户设定的分发频率。
    //   vblank 停摆（显示器休眠/远程桌面）由 10Hz 看门狗切回 Hub 内部 Timer。
    void onDisplayVBlank (double timestampSec);
    void updateFrameClockWatchdog();
    void performFrameDispatch (double timestampSec);
    void tryDispatchFromVBlank (double nowWallMs, double timestampSec);
    void tryDispatchFromFallbackTimer (double nowWallMs);
    void updateFramePacingFallbackTimer();
    std::unique_ptr<juce::VBlankAttachment> vblankAttachment;

    class FramePacingFallbackTimer;
    std::unique_ptr<FramePacingFallbackTimer> framePacingFallback;

    double vblankLastWallMs     = 0.0;   // 上一次 vblank 回调的墙钟（看门狗用）
    double lastDispatchWallMs   = 0.0;   // 上一次实际分发的墙钟（vblank 与兜底共用去重）
    double vblankLastSec        = 0.0;   // 上一次 vblank 事件时间戳（credit 累加用）
    double vblankCreditMs       = 0.0;   // vblank 时间信用累加器（分数倍率节流）

    // FPS 上限控制状态：
    //   · userRequestedFpsLimit —— 用户通过 UI 按钮设定的目标上限
    //     （0=无上限，30/60/120=有上限），默认 120。
    //   · activeDispatchHz     —— 下发到 AnalyserHub FrameDispatcher 的实际频率，
    //     始终等于 userRequestedFpsLimit（0 对 120）。采用"封顶"语义：
    //     未触及上限时系统自由运行，仅超出时才被 FrameDispatcher 的频率限制。
    int                      userRequestedFpsLimit  = 120;
    int                      activeDispatchHz       = 120;

    // VirtuPet 信号保活：仅当工作区存在 VirtuPet 模块时，
    // 才临时 retain(Loudness) 以驱动孵化/行为状态机；无 VirtuPet 时立刻 release。
    bool virtuPetSignalRetained = false;

    // —— GPU 合成层（统一迁移绘制到 GPU，Standalone + VST3 共用）——
    //   · 在 Editor 构造末尾 attachTo(*this)，析构最开始 detach()。
    //   · attach 后 JUCE 会把所有子组件的 paint() 命令翻译成 OpenGL 批次，
    //     底层把软光栅的 fillCGRect / drawGlyphs / fillPath 全部交给 GPU 执行。
    //   · 插件宿主（VST3/AU）下也安全：JUCE 会为 Editor 的顶层 NSView/HWND 自动
    //     创建一个子 GL 子层，不会影响宿主窗口其余部分；DAW 卸载 Editor 时
    //     detach 会清理 GL 资源。
    // ================================================================
    // Toybox（v3.0.0）点击解压互动主管。
    //   · 声明位置在 openGLContext 之前（§11.2）：反向析构时 toyDirector
    //     在 openGLContext 之后、workspace 之前销毁。
    //   · 完整类型只在 .cpp 中可见（pimpl，避免把 AnalyserHub 拖进本头）。
    // ================================================================
    std::unique_ptr<ToyDirector> toyDirector;

    //   · 本成员**必须**在类的末尾（所有子组件之后）声明，保证析构时最先执行
    //     （反向声明顺序），而我们在 ~Editor 起始处已显式 detach 一次兜底。
    juce::OpenGLContext openGLContext;

    // —— v2.9.3 Spectrogram3D GL 直渲染 ——
    //   renderOpenGL 里用 VBO 直接画 S3D 条带 + 轮廓 + 凹陷背景，绕开 JUCE 的
    //   CPU 路径光栅化；组件缓存里 plot 区域保持透明（模块 paintContent 配合）。
    //   shader 编译失败时 s3dGlReady=false，模块自动回退旧软件渲染路径。
    // v2.9.3：按组件树 z 序统一绘制 GL 直渲染内容（Milkdrop projectM blit +
    //   Spectrogram3D 条带）—— 重叠区域由 z 序决定遮挡（聚焦模块覆盖另一个）。
    //   milkdropReady=false 时只画 S3D（Milkdrop 未就绪或挂起）。
    void renderGlDirectModulesZOrdered (float dpiScale, int editorLogicalHeight,
                                        int editorPhysHeight,
                                        bool milkdropReady, unsigned int milkdropBlitSourceFbo);
    std::unique_ptr<juce::OpenGLShaderProgram> s3dShader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> s3dTransformUniform;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> s3dNdcUniform;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> s3dSnapUniform;
    // v2.9.5：纹理 shader（纹理通道 quad 绘制 + trail FBO 合成）
    std::unique_ptr<juce::OpenGLShaderProgram> s3dTexShader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> s3dTexTransformUniform;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> s3dTexNdcUniform;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> s3dTexWindowUniform;
    int s3dTexPosAttrib = 0, s3dTexUvAttrib = 1;
    unsigned int s3dVao = 0;       // VAO：attrib 布局容器（VBO 为 per-module 懒创建）
    unsigned int s3dQuadVbo = 0;   // v2.9.5：临时 quad VBO（纹理 quad / trail 衰减 quad 专用 ——
                                    //   不能借模块 glVbo，否则 needUpload=false 帧会用残留的
                                    //   quad 数据当快照顶点画 → 全部模块画面污染）
    int s3dPosAttrib = 0, s3dColAttrib = 1;
    bool s3dShaderUseChecked = false;
    bool s3dGlReady = false;

    // ================================================================
    // 模块浮动窗口（Standalone 模式下的模块弹出/脱离功能）
    //   · FloatingModuleWindow 封装一个独立的 DocumentWindow，
    //     内部持有已脱离的 ModulePanel。
    //   · 关闭浮动窗口时自动将模块 dock 回 workspace 原始位置。
    //   · floatingWindows_ 由 Editor 统一管理生命周期。
    // ================================================================
    class FloatingModuleWindow : public juce::DocumentWindow,
                                 private juce::AsyncUpdater
    {
    public:
        FloatingModuleWindow(const juce::String& name,
                             ModulePanel& module,
                             juce::Rectangle<int> savedBounds,
                             std::function<void(ModulePanel&, juce::Rectangle<int>)> onDock,
                             Y2KmeterAudioProcessorEditor* editor);
        ~FloatingModuleWindow() override;
        void closeButtonPressed() override;
        void resized() override;
        juce::BorderSize<int> getBorderThickness() const override;
        juce::Rectangle<int> getSavedBounds() const noexcept { return savedBounds_; }
        void setLayoutLocked(bool locked);
        juce::ValueTree saveModuleSpecificState() const;

        // 键盘事件：浮动 Milkdrop 窗口需要将 ← → 空格转发给 Editor，
        // 否则浮动窗口吞掉键盘焦点后 Editor 收不到按键事件。
        bool keyPressed(const juce::KeyPress& key) override;

        // 取消 resize/move 触发的 pending async update（关闭窗口前调用）
        void cancelAsyncUpdates() { cancelPendingUpdate(); }

    private:
        void handleAsyncUpdate() override;
        ModulePanel& module_;
        juce::Rectangle<int> savedBounds_;
        std::function<void(ModulePanel&, juce::Rectangle<int>)> onDock_;
        Y2KmeterAudioProcessorEditor* editor_;  // 用于 Milkdrop resize 时触发 GL 重绘
        bool floatingLockLimitsValid_ = false;
        int floatingLockMinW_ = 0;
        int floatingLockMinH_ = 0;
        int floatingLockMaxW_ = 0;
        int floatingLockMaxH_ = 0;
    };

    std::vector<std::unique_ptr<FloatingModuleWindow>> floatingWindows_;

    // 将当前所有浮动窗口的真实屏幕 bounds 同步到 ModuleWorkspace 的持久化状态。
    void syncFloatingWindowStatesToWorkspace();

    // 由 workspace 的脱离态记录重建浮动窗口（启动恢复 / 退出 stick 恢复备份布局）。
    //   已存在同 moduleId 的浮动窗口会跳过，避免重复实例化。
    void restoreFloatingWindowsFromWorkspace();

    // 用 Processor 里的独立存档替换 workspace 的脱离态记录（不创建窗口）。
    //   用于导入 settings / 工程状态后重新对齐浮动模块。
    void reloadFloatingModuleRecordsFromProcessor();

    // ================================================================
    // v1.9.x：新手引导（仅 Standalone 模式生效，插件宿主模式忽略）
    //   · step_hidden:      不显示引导
    //   · step1_rightClick: 右键点击画布添加电子宠物
    //   · step2_playAudio:  播放音频孵化宠物蛋
    //   · tutorialWasSkipped: 用户在引导期间切换了非 default 预设 → 跳过
    // ================================================================
    enum class TutorialStep { hidden, step1_rightClick, step1_menuOpened, step2_playAudio };
    TutorialStep tutorialStep = TutorialStep::hidden;
    bool tutorialWasSkipped = false;  // 切换预设导致跳过，切回 default 时重新触发

    // 新手引导覆盖层（全屏半透明 + 聚光灯 + Y2K 气泡提示）
    //   完整定义在 PluginEditor.cpp 中（与 ChromeHiddenOverlay 同理）
    class TutorialOverlay;
    std::unique_ptr<TutorialOverlay> tutorialOverlay;

    // 新手引导流程控制
    void startTutorial();                           // 启动引导 STEP 1
    void advanceTutorialStep2();                    // STEP 1 → STEP 2
    void completeTutorial();                        // STEP 2 完成，标记 tutorialCompleted
    void skipTutorial();                            // 用户切换预设时跳过引导
    void dismissTutorialOverlay();                  // 只隐藏覆盖层，不改变完成状态
    void checkTutorialStep2Condition();             // timer 中轮询：蛋是否孵化

    // ==============================================================
    // projectM 状态（由 Editor GL 上下文持有）
    // ==============================================================
    projectm_handle        milkdrop_pm_handle_ = nullptr;
    bool                   milkdrop_render_ready_ = false;
    std::string            milkdrop_error_;

    // v2.3 GPU: projectM 渲染到独立 offscreen FBO → glBlitFramebuffer 搬运到
    //   各模块正确位置。关键：使用 openglRenderFrameFbo(fbo_id) 让 projectM
    //   渲染到独立 FBO（非 FBO 0），消除同一 FBO 上 blit 源/目重叠的未定义行为。
    //   分辨率控制：内部降采样渲染 → GL_LINEAR 上采样填满，产生模糊降功耗效果。
    GLuint milkdrop_render_fbo_   = 0;
    GLuint milkdrop_render_tex_   = 0;
    int    milkdrop_render_scale_ = 1;  // 1=全分辨率, 2=半分辨率, 4=1/4分辨率
    int    milkdrop_last_fbo_w_   = 0;
    int    milkdrop_last_fbo_h_   = 0;
    double milkdrop_last_render_sec_ = 0.0;  // 上次 projectM 渲染时刻（60fps 节流）

    // 整体染色后处理（master output colors）：projectM 渲染到 milkdrop_render_fbo_ 后，
    // 若染色增益非 (1,1,1)，先经着色器采样到 milkdrop_post_fbo_，再 blit 到各模块。
    std::unique_ptr<MilkdropTintPass> milkdrop_tint_pass_;
    GLuint milkdrop_post_fbo_ = 0;   ///< 染色后处理输出 FBO
    GLuint milkdrop_post_tex_ = 0;   ///< 染色后处理输出纹理
    int    milkdrop_post_w_   = 0;   ///< 当前 post FBO 宽度
    int    milkdrop_post_h_   = 0;   ///< 当前 post FBO 高度

    // 全局视觉状态（UI 线程写，GL 线程读，mutex 保护）
    mutable std::mutex milkdrop_visual_mutex_;
    MilkdropVisualState milkdrop_visual_state_;

    // 简单波形样式覆盖（UI 线程写，GL 线程读，mutex 保护）
    mutable std::mutex milkdrop_wave_mutex_;
    MilkdropWaveState milkdrop_wave_state_;

    // 浮动窗口共享帧：renderOpenGL 抓取离线 FBO 内容 → 浮动 Milkdrop 读取
    juce::Image       milkdrop_shared_frame_;
    std::mutex        milkdrop_shared_frame_mutex_;
    int               floating_milkdrop_count_ = 0;  // 浮动 Milkdrop 窗口计数
    std::unique_ptr<uint8_t[]> milkdrop_capture_buf_;      // 预分配 glReadPixels 缓冲区
    int                        milkdrop_capture_buf_size_ = 0;

    // 预设
    juce::StringArray      milkdrop_preset_paths_;
    int                    milkdrop_current_preset_ = -1;
    std::atomic<int>       milkdrop_current_preset_ui_{ -1 };
    std::atomic<int>       milkdrop_requested_preset_delta_{ 0 };
    std::atomic<bool>      milkdrop_requested_preset_random_{ false };
    std::atomic<int>       milkdrop_requested_preset_jump_{ -1 };
    std::atomic<int64_t>   milkdrop_last_preset_switch_ms_{ 0 };
    // 收藏库（like library）：当前是否浏览收藏库（全局共享，仅一个 Milkdrop 模块）。
    // UI 线程写、GL 线程读（RescanMilkdropPresetPaths），用 atomic 消除数据竞争；
    // 切换请求经独立原子标志异步消费。
    std::atomic<bool>      milkdrop_use_like_library_{ false };
    std::atomic<bool>      milkdrop_requested_library_toggle_{ false };
    // 双向索引记忆：切换库时记录各自库的当前预设索引（-1 = 未记录，用 0 兜底）。
    // 仅在 GL 线程消费切换请求时读写，无跨线程竞争。
    int                    milkdrop_builtin_preset_index_ = -1;
    int                    milkdrop_like_preset_index_ = -1;
    // 取消收藏触发的重扫请求（renderOpenGL 消费）。
    std::atomic<bool>      milkdrop_requested_rescan_{ false };
    // 脱离态（floating）期间置 true，阻止 Editor::newOpenGLContextCreated 异步
    // 创建 Editor projectM handle。Windows 下 libprojectM/GLEW 依赖进程全局指针表，
    // 若 Editor handle 与 GLView 的本地 handle 同时存在会互相干扰（表现为预设
    // shader 编译失败回退 Idle）。挂起期间跳过创建，dock 回来时再恢复。
    std::atomic<bool>      milkdrop_renderer_suspended_{ false };
    void LoadMilkdropPresetInternal();
    static juce::File FindMilkdropAssetsDir(const juce::String& subdir);
    // 重扫预设目录（根据 milkdrop_use_like_library_ 选择内置库或收藏库）。
    // 必须在 GL 线程调用（或 projectM handle 尚未创建的初始化阶段）。
    void RescanMilkdropPresetPaths();
    // PCM 缓冲（UI 线程写，GL 线程读）
    std::mutex             milkdrop_pcm_mutex_;
    std::vector<float>     milkdrop_pending_pcm_;
    unsigned int           milkdrop_pending_frames_ = 0;
    std::vector<float>     milkdrop_last_real_pcm_;
    unsigned int           milkdrop_last_real_frames_ = 0;
    bool                   milkdrop_has_ever_received_pcm_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Y2KmeterAudioProcessorEditor)
};