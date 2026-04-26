#ifndef PBEQ_MODULE_WORKSPACE_H_INCLUDED
#define PBEQ_MODULE_WORKSPACE_H_INCLUDED

#include <JuceHeader.h>
#include <unordered_map>

// ============================================================
// 为绕过 MSVC 同进程多文件编译时 include guard 串扰问题，
// 将 ModuleType / ModulePanel / ModuleWorkspace 合并到同一个头文件。
// 实现分别写在 ModulePanel.cpp 与 ModuleWorkspace.cpp 中。
// ============================================================

class ModuleWorkspace; // 前向声明（给 ModulePanel 的回调使用）

enum class ModuleType
{
    eq,
    loudness,
    oscilloscope,
    spectrum,
    phase,
    dynamics,

    // 细粒度拆分模块
    lufsRealtime,
    truePeak,
    oscilloscopeLeft,
    oscilloscopeRight,
    phaseCorrelation,
    phaseBalance,
    dynamicsMeters,
    dynamicsDr,
    dynamicsCrest,

    // 持续滚动瀑布波形（复用 Oscilloscope 原始样本，后端零新增计算）
    waveform,

    // 模拟 VU 指针表（复用 Loudness 路的 RMS L/R，后端零新增计算）
    vuMeter,

    // 像素式实时频谱瀑布图（复用 Spectrum 路的 FFT 幅度，后端零新增计算）
    spectrogram
};

juce::String getModuleDisplayName(ModuleType t);

// 类型 ↔ 字符串（用于布局持久化）
juce::String moduleTypeToString(ModuleType t);
ModuleType   stringToModuleType(const juce::String& s, bool* ok = nullptr);

// ------------------------------------------------------------
// ThemeSwatchBar —— 参考 Win XP 画图底部调色板样式的主题选择器
//   左侧：当前主题预览方块（双色斜分，复刻画图的前景/背景色预览）
//   右侧：N 个主题色票（点击即切换主题）
// ------------------------------------------------------------
class ThemeSwatchBar : public juce::Component,
                       public juce::SettableTooltipClient
{
public:
    ThemeSwatchBar();
    ~ThemeSwatchBar() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    juce::Rectangle<int> getPreviewBounds() const;
    juce::Rectangle<int> getSwatchBounds(int index) const;
    int hitTestSwatch(juce::Point<int> p) const;

    int hoverIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ThemeSwatchBar)
};

// ------------------------------------------------------------
// HideChromeButton —— 隐藏/显示"白色底框+底部控制区"的按钮
//   - 显示态（chrome 可见）：按钮常驻 toolbar 右侧，完全不透明
//   - 隐藏态（chrome 隐藏）：鼠标离开按钮 → 半透明；悬停 → 不透明
// ------------------------------------------------------------
class HideChromeButton : public juce::Button
{
public:
    HideChromeButton();
    ~HideChromeButton() override = default;

    // dimWhenIdle=true 时：鼠标离开变半透明；鼠标悬停/按下变不透明
    // dimWhenIdle=false：始终完全不透明
    void setDimWhenIdle(bool shouldDim);
    bool getDimWhenIdle() const noexcept { return dimWhenIdle; }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;

    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

private:
    void updateAlpha();

    bool dimWhenIdle = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HideChromeButton)
};

// ------------------------------------------------------------
// ModulePanel —— 所有分析模块的 Pink XP 像素窗口基类
// ------------------------------------------------------------
class ModulePanel : public juce::Component
{
public:
    explicit ModulePanel(ModuleType type);
    ~ModulePanel() override;

    ModuleType   getModuleType() const noexcept { return moduleType; }
    juce::String getModuleId()   const noexcept { return moduleId; }
    void setModuleId(const juce::String& id) { moduleId = id; }

    juce::String getTitleText() const { return titleText; }
    void setTitleText(const juce::String& s);

    // 下发当前插件 CPU 占用率（[0..1]），UI 线程调用；
    //   改变足够大时会触发右下角小字区域的局部 repaint。
    void setCpuLoad (float load01) noexcept;

    void setMinSize(int w, int h) noexcept { minW = w; minH = h; }
    int  getMinWidth()  const noexcept { return minW; }
    int  getMinHeight() const noexcept { return minH; }

    // ------------------------------------------------------------------
    // 每个模块的"默认大小"（用于首次加入到 Workspace / 拖入预览 / 预设 1
    // 默认布局 seed）。派生类在构造里调用 setDefaultSize(w,h) 给出自己
    // 期望的出生尺寸；若不调用则沿用下方默认值（320×220，与历史行为一致）。
    // 这个尺寸与 minSize 是独立的两个概念：
    //   · minSize  = 用户能把模块缩到多小（统一 50×50）
    //   · default  = 模块刚生成时的开局尺寸（每个模块各自按网格给定）
    // ------------------------------------------------------------------
    void setDefaultSize (int w, int h) noexcept { defaultW = w; defaultH = h; }
    int  getDefaultWidth()  const noexcept { return defaultW; }
    int  getDefaultHeight() const noexcept { return defaultH; }

    std::function<void(ModulePanel&)> onBoundsChangedByUser;
    std::function<void(ModulePanel&)> onBoundsDragging;   // 拖拽过程中持续回调（吸附预览用）
    std::function<void(ModulePanel&)> onCloseClicked;
    std::function<void(ModulePanel&)> onBroughtToFront;

    void paint(juce::Graphics& g) override;
    // CPU 小字绘制在所有子组件之上（子组件如 EQ 的 PixelEqGraph 会填满整个
    // 内容区，paint() 里绘制的文本会被子组件后续绘制覆盖）。
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;

protected:
    virtual void paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds);
    virtual void layoutContent(juce::Rectangle<int> contentBounds) { juce::ignoreUnused(contentBounds); }

    juce::Rectangle<int> getContentBounds() const;

private:
    enum class Edge { none, right, bottom, bottomRight };

    Edge detectEdge(juce::Point<int> pos) const;
    void updateCursorFor(Edge e);

    ModuleType   moduleType;
    juce::String moduleId;
    juce::String titleText;

    // 模块最小尺寸默认 64×64（= 1 个大格 / 8×8 个小方格，每小方格 8 像素）。
    //   · 之前默认 160×120 会在 HorizontalStrip 预设下把窗口的可用 canvas
    //     撑得比实际给出的 slot 还大，导致模块越过底部 toolbar，把
    //     Hide / Source / FPS / ThemeBar 等控件盖住。
    //   · 统一降到 64×64 后，任何子模块只要自己不再显式 setMinSize 到更大，
    //     都能被缩到一个大格；牺牲是极小尺寸下文字会挤压甚至溢出，
    //     这是用户明确接受的代价（换取"永远不遮挡 toolbar"的确定性）。
    int minW = 64;
    int minH = 64;

    // 默认出生尺寸（每个派生模块通过 setDefaultSize 覆盖）。
    // 320×220 是历史上 Workspace 使用的共享默认值，此处作为
    // "派生类没显式声明" 时的后备兜底。
    int defaultW = 320;
    int defaultH = 220;

    static constexpr int titleBarHeight  = 22;
    static constexpr int closeButtonSize = 16;
    static constexpr int edgeHotSize     = 8;

    juce::Rectangle<int> getCloseButtonBounds() const;
    juce::Rectangle<int> getTitleBarBounds()   const;

    bool closeButtonHovered = false;
    bool closeButtonPressed = false;

    // 右下角 CPU 小字（单位：%，已从 [0..1] 乘 100）
    float cpuPercent = 0.0f;
    juce::Rectangle<int> getCpuLabelBounds() const;

    enum class DragMode { none, move, resize };
    DragMode dragMode = DragMode::none;
    Edge     resizeEdge = Edge::none;

    juce::Point<int>     dragStartMouse;
    juce::Rectangle<int> dragStartBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModulePanel)
};

// ------------------------------------------------------------
// ModuleWorkspace —— 模块拖拽/停靠/吸附工作区
// ------------------------------------------------------------
class ModuleWorkspace : public juce::Component,
                        public juce::FileDragAndDropTarget
{
public:
    ModuleWorkspace();
    ~ModuleWorkspace() override;

    using ModuleFactory = std::function<std::unique_ptr<ModulePanel>(ModuleType)>;
    void setModuleFactory(ModuleFactory f) { factory = std::move(f); }

    void setAvailableModuleTypes(juce::Array<ModuleType> types) { availableTypes = std::move(types); }

    ModulePanel& addModule(std::unique_ptr<ModulePanel> panel, bool autoPosition = true);
    ModulePanel* addModuleByType(ModuleType t);

    void removeModule(ModulePanel& panel);

    int getNumModules() const noexcept { return modules.size(); }
    ModulePanel* getModule(int idx) const noexcept { return modules[idx]; }

    // 清空所有模块（含 OwnedArray 自动 delete）与拼豆贴画。
    //   · 用于布局预设切换：先清空再由外部重新填充。
    //   · 不触发 onLayoutChanged 回调，避免中间态把"空布局"写回 Processor。
    void clearAllModules();

    void autoLayout();

    // ======================================================
    // Phase E —— 布局持久化 + 吸附指示 + 外部订阅
    // ======================================================

    // 保存 / 加载布局到 ValueTree
    juce::ValueTree saveLayoutTree() const;
    bool            loadLayoutFromTree(const juce::ValueTree& tree);

    // XML 便捷接口（用于 Processor::getStateInformation）
    juce::String saveLayoutAsXml() const;
    bool         loadLayoutFromXml(const juce::String& xmlString);

    // 用户操作布局（添加/删除/移动/缩放）后回调，由 Editor 订阅 → 写回 Processor
    std::function<void()> onLayoutChanged;

    // 隐藏/显示"白色底框 + 底部控制区"（右下角 Hide 按钮驱动，也可外部调用）
    void setChromeVisible(bool shouldBeVisible);
    bool isChromeVisible() const noexcept { return chromeVisible; }

    // chrome 可见性变化回调（例如 Editor 订阅 → 顶部 TitleBar 做半透明）
    std::function<void(bool visible)> onChromeVisibleChanged;

    // ======================================================
    // 音频信号来源下拉框（底部工具栏中部，主题栏与 Hide 之间）
    // ======================================================
    struct AudioSourceItem
    {
        juce::String displayName;    // 显示给用户看的名字
        juce::String sourceId;       // 稳定 ID（回调携带，用于宿主 App 识别）
        bool         isLoopback = false; // 是否"系统输出 Loopback"占位项
    };

    // 由 Editor/Standalone App 调用来填充下拉项
    //   · 调用时会保留当前选择（若 sourceId 仍存在则不触发回调）
    void setAudioSourceItems (const juce::Array<AudioSourceItem>& items,
                              const juce::String& selectedSourceId = {});

    // 取当前选中的 sourceId（空串表示未选择）
    juce::String getSelectedAudioSourceId() const;

    // 用户在下拉里选择后的回调（sourceId 为 setAudioSourceItems 时提供的 ID）
    std::function<void(const juce::String& sourceId, bool isLoopback)> onAudioSourceChanged;

    // 控制"信号源"下拉 + 前缀标签 + 其左侧分隔线的可见性。
    //   · 用于 VST3 / AU 等插件模式：宿主 DAW 直接给音频，不需要选择信号源，
    //     Editor 构造时会调 setAudioSourceUiVisible(false)。
    //   · false 时 resized() 跳过 Source 区的布局（不占宽、不画分隔线 #2），
    //     剩余空间自动让给 themeBar。
    //   · 默认 true（Standalone 模式保持原行为）。
    void setAudioSourceUiVisible (bool shouldBeVisible);

    // 控制"布局预设"下拉 + Grid 按钮左侧分隔线的可见性。
    //   · 用于 VST3 / AU 等插件模式：宿主窗口由 DAW 决定大小，切换布局预设
    //     （尤其是"横向铺满屏幕"这类改变顶层窗口尺寸/位置的预设）对宿主无意义
    //     且会打架；Editor 构造时会调 setLayoutPresetUiVisible(false) 关闭。
    //   · false 时 resized() 跳过布局预设区（不占宽、不画分隔线 Layout），
    //     剩余空间让给 themeBar；Grid 按钮不受影响保留。
    //   · 默认 true（Standalone 模式保持原行为）。
    void setLayoutPresetUiVisible (bool shouldBeVisible);

    // ======================================================
    // FPS 限制按钮（底部工具栏左下角）
    //   · 点击可在 30 / 60 FPS 之间切换（默认 30）
    //   · 回调由 Editor 订阅，真正修改 AnalyserHub 的 FrameDispatcher 频率
    //   · 旁边显示实时帧率（由 Editor 统计后通过 setMeasuredFps 下发）
    // ======================================================
    int  getFpsLimit() const noexcept { return fpsLimit; }
    void setFpsLimit (int hz);                 // 外部可调用（例如恢复上次设置）
    void setMeasuredFps (float fps);           // Editor 每秒更新一次显示

    // 用户点击 FPS 按钮切换后的回调
    std::function<void(int hz)> onFpsLimitChanged;

    // ======================================================
    // 布局预设下拉（toolbar 上 Grid 按钮左侧）
    //   · Preset 1 = 默认布局（七个默认模块 + 默认窗口大小）
    //   · Preset 2 = 上方横向铺满屏幕宽度（默认模块横向等分 canvas）
    //   · Preset 3 = 下方横向铺满屏幕宽度（同 Preset 2 但窗口贴屏幕底部）
    //   · Preset 4 = Tiled：按用户 settings 的 1346×1087@(89,134)
    //                快照平铺布局（硬编码 XML）
    //   · 选中某项时调用 onLayoutPresetChanged(index)，由 Editor
    //     负责真正的布局/窗口尺寸变更（ModuleWorkspace 本身不持有
    //     顶层窗口的引用，也不了解模块工厂细节，故把具体策略委托给 Editor）
    // ======================================================
    enum class LayoutPreset
    {
        defaultGrid      = 1, // 默认布局（七个默认模块 + 默认窗口大小）
        horizontalFull   = 2, // 横向铺满屏幕宽度（贴屏幕顶部）
        horizontalBottom = 3, // 横向铺满屏幕宽度（贴屏幕底部）
        tiled            = 4  // Tiled：按 settings 快照还原 1346×1087 平铺布局
    };
    std::function<void(LayoutPreset)> onLayoutPresetChanged;

    // ======================================================
    // 预设导出 / 导入回调（Save/Load 按钮触发）
    //   · onSavePresetRequested(juce::File dest)：用户在 Save 点击后弹出的
    //     FileChooser 中选定目标文件，Workspace 会把 File 原样回调给外层；
    //     外层（Y2KStandaloneApp）负责把当前 settings 物理文件复制过去。
    //   · onLoadPresetRequested(juce::File src)：用户在 Load 点击后弹出的
    //     FileChooser 中选定源文件，Workspace 会把 File 原样回调给外层；
    //     外层负责把源文件复制到 settings 物理位置并触发重启以应用。
    //   · 空 File 表示用户取消了 FileChooser（外层可忽略）。
    // ======================================================
    std::function<void(juce::File)> onSavePresetRequested;
    std::function<void(juce::File)> onLoadPresetRequested;

    // 被 ModulePanel 在 drag 过程中调用，更新吸附预览线
    void updateDragPreview(ModulePanel& movingPanel);
    void clearDragPreview();

    // 仅重绘拖拽预览相关脏区（预览框 + 指示线），避免整画布重绘
    void repaintDragPreviewPieces(juce::Rectangle<int> preview,
                                  const juce::Array<int>& vGuides,
                                  const juce::Array<int>& hGuides);

    void paint(juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;

    // 双击 / 右键空白区 → 弹出 "添加模块" 菜单
    void mouseDown       (const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    // ======================================================
    // Hit-test 挖洞：Editor 在 chrome 隐藏态下为"悬浮关闭按钮/可点击标题文字"
    //   指定一组矩形（workspace 坐标系），这些区域的鼠标事件不会被本组件消化，
    //   而是冒泡给父组件（Editor）处理。
    //   · 仅当空矩形数组时恢复默认 hit-test 行为。
    //   · 注意：任何落在模块子组件（ModulePanel）内部的事件，JUCE 会先派发给模块；
    //     只有落在 workspace 空白处 + 挖洞范围时才会冒泡到 Editor，从而自然地满足
    //     "模块可以遮挡并屏蔽顶部按钮"的用户需求。
    // ======================================================
    void setHitTestHoles (const juce::Array<juce::Rectangle<int>>& holes);
    bool hitTest (int x, int y) override;

    // ======================================================
    // 文件拖放（FileDragAndDropTarget） —— 拖入图片生成"拼豆像素画"
    //   · 仅接收常见图片格式（png/jpg/jpeg/bmp/gif）
    //   · 落入后自动：① 按 10×10 像素块降采样 ② 每块颜色匹配到内置 144 色拼豆色
    //                ③ 作为一张"贴画"绘制到 canvas 底图上（模块/网格线之下，被模块遮挡）
    //   · 允许多张图片叠放；用户可以像拖模块一样拖动它们（左键按住拖动）
    //   · 点击图片进入"聚焦"态：绘制选中框 + 8 个缩放手柄 + 右上角 ×；
    //     拖动手柄缩放网格数（像素块尺寸不变），松开后重新量化颜色；
    //     聚焦态下按 Delete / 单击 × 则删除该图片。
    // ======================================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped           (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter          (const juce::StringArray& files, int x, int y) override;
    void fileDragMove           (const juce::StringArray& files, int x, int y) override;
    void fileDragExit           (const juce::StringArray& files) override;

    // 清空所有拼豆贴画（外部可调用；目前未提供 UI 按钮，保留给未来使用）
    void clearPerlerImages();
    int  getNumPerlerImages() const noexcept;

    // Bug3：chrome 隐藏/显示时，workspace 自身在父 Editor 内的 y 位置会有 titleBarHeight
    //   的平移差。Editor 已对所有 ModulePanel 做了反向补偿来保持"屏幕绝对位置不变"；
    //   本方法让 Editor 对 perlerImages 同样做这种反向补偿，修复"hide 后图片上移"的 bug。
    //   · dy > 0：图片整体在 workspace 坐标系下下移 dy 像素
    //   · dy < 0：整体上移 |dy| 像素
    void shiftAllPerlerImagesY (int dy);

    // 把索引 idx 对应的 PerlerImageLayer 子组件的 bounds 同步到当前
    //   PerlerImage 的 getBounds()，并 repaint 它自己。任何修改图片
    //   topLeft / pixelImage 的路径都必须调用一次此方法以保证 z-order
    //   容器里的子组件位置和渲染与数据一致。
    void syncPerlerLayerBounds (int idx);

    // 鼠标事件：在已有 mouseDown/mouseDoubleClick 的基础上，新增 mouseDrag/mouseUp
    //   · 用于实现"点击空白 → 命中拼豆图片 → 拖动移动"的交互
    //   · 若没命中图片则走原有的"空白区右键弹添加菜单"逻辑
    void mouseDrag       (const juce::MouseEvent& e) override;
    void mouseUp         (const juce::MouseEvent& e) override;
    // 聚焦图片时：监听鼠标移动以更新 × 按钮 hover 态 + 光标
    void mouseMove       (const juce::MouseEvent& e) override;
    void mouseExit       (const juce::MouseEvent& e) override;

    // ==================================================================
    // canvas / toolbar 区域查询（public）
    //   · 外层 PluginEditor 在 applyLayoutPreset 时需要根据扣除了 toolbar 的
    //     "有效作画区域"计算模块位置，避免直接用 getLocalBounds() 导致模块
    //     从 y=0 一直铺到 workspace 底部，把底部 toolbar 的 Hide/Source/FPS 等
    //     控件整个盖住（之前就出现过这种预设 2 模块遮挡控制台的 bug）。
    // ==================================================================
    juce::Rectangle<int> getCanvasArea()  const;
    juce::Rectangle<int> getToolbarArea() const;

private:
    static constexpr int gridSize       = 8;
    static constexpr int margin         = 0;   // 0 = 模块可以紧贴窗口边框
    static constexpr int toolbarHeight  = 36;
    // 注：原先这里还有 defaultModuleW/H 两个 workspace 级常量，现已废弃。
    //   每个模块的"初始大小"由派生类在构造里调用 ModulePanel::setDefaultSize
    //   自行声明，Workspace 通过 panel->getDefaultWidth/Height 或
    //   getDefaultSizeForType(ModuleType) 取值，避免一刀切。

    juce::Rectangle<int> snapRect(juce::Rectangle<int> r) const;
    void snapToGrid(ModulePanel& panel);

    // Phase E —— 边缘/邻居吸附：对 r 结合 except 之外的已有模块做吸附对齐，并把
    // 命中的对齐坐标填入 outGuides（水平线 Y 坐标 / 竖直线 X 坐标）
    juce::Rectangle<int> snapToNeighbours(juce::Rectangle<int> r,
                                          const ModulePanel* except,
                                          juce::Array<int>& outVGuides,
                                          juce::Array<int>& outHGuides) const;

    void notifyLayoutChanged();

    juce::Rectangle<int> findNextSlot(int w, int h) const;

    // 弹出添加模块菜单：
    //   anchorScreenPos 控制弹出位置（屏幕坐标）
    //   placeAtCanvasPos 非 (-1,-1) 时，新模块将放置到 canvas 内该坐标（左上角）
    void showAddMenu(juce::Point<int> anchorScreenPos,
                     juce::Point<int> placeAtCanvasPos = { -1, -1 });
    void hookPanel(ModulePanel& panel);

    juce::OwnedArray<ModulePanel> modules;

    // 底部工具栏里的主题选择器（XP 画图调色板样式）
    ThemeSwatchBar themeBar;

    // 底部工具栏里的音频信号源下拉（位于主题栏与 Hide 按钮之间）
    juce::ComboBox                 audioSourceBox;
    juce::Array<AudioSourceItem>   audioSourceItems;
    juce::Label                    audioSourceLabel;    // "Source:" 前缀标签

    // 信号源下拉在底部 toolbar 中的可见性。true = 显示（Standalone 默认）；
    //   false = 插件模式下隐藏整个 Source 区（下拉 + 标签 + 左侧分隔线）。
    bool                           audioSourceUiVisible = true;

    // 布局预设下拉在底部 toolbar 中的可见性。true = 显示（Standalone 默认）；
    //   false = 插件模式下隐藏整个"布局预设"区（下拉 + Grid 按钮与之间的分隔线）。
    bool                           layoutPresetUiVisible = true;

    // 右下角的 Hide/Show 按钮（隐藏白色底框 + 控制区）
    HideChromeButton hideBtn;
    bool             chromeVisible = true;

    // FPS 限制按钮 + 实时 FPS 标签（右下角，Hide 按钮左侧）
    juce::TextButton fpsBtn;
    juce::Label      fpsLabel;
    int              fpsLimit = 30;  // 默认 30 FPS

    // FPS 按钮专用的 mini LookAndFeel：仅重写 getTextButtonFont 把"30FPS / 60FPS"
    //   渲染得比全局按钮字号更小一点（52px 宽 + 像素字体下不显挤）。
    //   具体类型在 .cpp 里定义（轻量封装 juce::LookAndFeel_V4），这里用 pimpl
    //   前向指针避免把实现细节暴露到头文件。
    std::unique_ptr<juce::LookAndFeel> fpsMiniLnf;

    // 主题订阅 token：切换主题时重新 setColour(fpsLabel) 以跟随 PinkXP::ink
    //   （暗色主题下 ink 是浅色，必须在主题切换后刷新缓存过的 Label colour，
    //    否则 fpsLabel 一直显示构造时的深色文字，在黑底上看不清。）
    int themeSubToken = -1;

    // Grid 显示切换按钮（toolbar 中，位于 FPS 按钮左侧 / themeBar 右侧）
    //   · 点击切换 gridOverlayVisible；paint() 会据此在 canvas 上绘制 8px 网格线
    //   · 持久化到 layout tree，以便恢复上次打开时的状态
    juce::TextButton gridBtn;
    bool             gridOverlayVisible = false;

    // 布局预设下拉（位于 Grid 按钮左侧，中间用一条竖直分割线和 Grid 分开）
    //   · 样式：与 audioSourceBox 一致 —— PinkXP LookAndFeel 下的 ComboBox
    //   · 选中后通过 onLayoutPresetChanged 通知 Editor 执行布局变更
    juce::ComboBox   layoutPresetBox;
    // Grid 与 layoutPresetBox 之间的竖直分割线 x 坐标（-1 = chrome 隐藏态未布局）
    int              toolbarDividerXLayout = -1;

    // 预设 Save/Load 按钮（紧邻 layoutPresetBox 左侧）
    //   · Save：用户把当前 settings 文件另存为到指定位置（自定义文件名，后缀 .settings）
    //   · Load：用户选择一个之前 Save 过的 .settings 文件覆盖当前 settings，然后
    //           由外层（Y2KStandaloneApp）触发重启以应用所有持久化项。
    //   · 仅在 layoutPresetUiVisible==true（Standalone 模式）时可见。
    //   · 下拉弹出 FileChooser 后通过 onSavePresetRequested / onLoadPresetRequested
    //     把选中的 File 传给 Editor → Standalone App 去做真正的复制 / 重载。
    juce::TextButton savePresetBtn;
    juce::TextButton loadPresetBtn;

    // 添加模块菜单（双击 / 右键空白区）hover 预览状态
    //   · 当用户在 PopupMenu 里悬停在某一模块名上时，在 canvas 的 hoverPreviewPos
    //     处绘制一个半透明的"模块占位框"（标题栏 + 模块名），给用户一个
    //     "放下去后大概长这样"的直觉提示。
    //   · 一旦菜单关闭（用户选中 / 取消）就清空，不残留在画面上。
    bool          hoverPreviewActive = false;
    ModuleType    hoverPreviewType   = ModuleType::eq;
    juce::Point<int> hoverPreviewPos { 0, 0 };

    // hover 预览 —— 每个 ModuleType 缓存一张"空态快照"（含完整子组件），
    //   首次 hover 时通过 factory 创建临时 ModulePanel → createComponentSnapshot
    //   渲染成 Image，之后直接半透明贴图，避免反复构造模块实例。
    //   主题切换后缓存全部失效（新主题的配色下必须重新渲染）。
    std::unordered_map<int, juce::Image> hoverPreviewCache;
    int hoverPreviewCacheThemeTag = -1;

    // hover 预览 —— 每个 ModuleType 的"默认大小"缓存（随 factory 懒查询）
    //   · getDefaultSizeForType 会 factory 构造一个临时 ModulePanel，读取其
    //     setDefaultSize 声明的 getDefaultWidth/Height 并缓存，之后 paintOverChildren
    //     / 脏区计算 / 预览快照等场景直接命中缓存。
    //   · 主题切换不影响默认尺寸，故此缓存不随主题切换失效。
    std::unordered_map<int, juce::Point<int>> hoverPreviewSizeCache;

    // 取（必要时构造）指定 type 的预览快照。返回的 Image 可能为空（factory 未绑定 /
    //   构造失败等异常场景），调用方需做空判断。
    const juce::Image& getHoverPreviewImage (ModuleType t);

    // 取（必要时构造）指定 type 的"默认大小"。factory 未绑定或构造失败时
    //   回退到 ModulePanel 基类默认（320×220）。
    juce::Point<int> getDefaultSizeForType (ModuleType t);

public:
    // 内部使用：AddMenuItemComponent 在 hover 变化时调用，更新预览状态并触发重绘。
    //   · t 为当前被高亮的模块类型；active=false 表示 hover 离开（菜单关闭/切别项前）
    void setAddMenuHoverPreview (bool active, ModuleType t);

private:

    // Toolbar 中"FPS 区 | Source 区 | Hide 区"之间的两条竖线分隔符 X 坐标
    //   · resized() 里根据控件实际位置写入；paint() 读取后绘制
    //   · 值为 -1 表示未初始化 / 不绘制（如 chrome 隐藏时）
    int toolbarDividerX0 = -1; // grid 与 fps 之间
    int toolbarDividerX1 = -1; // fps 与 source 之间
    int toolbarDividerX2 = -1; // source 与 hide 之间

    ModuleFactory factory;
    juce::Array<ModuleType> availableTypes {
        ModuleType::eq, ModuleType::loudness, ModuleType::lufsRealtime, ModuleType::truePeak,
        ModuleType::vuMeter,
        ModuleType::oscilloscope, ModuleType::oscilloscopeLeft, ModuleType::oscilloscopeRight,
        ModuleType::waveform,
        ModuleType::spectrum, ModuleType::spectrogram,
        ModuleType::phase, ModuleType::phaseCorrelation, ModuleType::phaseBalance,
        ModuleType::dynamics, ModuleType::dynamicsMeters, ModuleType::dynamicsDr, ModuleType::dynamicsCrest
    };

    int nextIdCounter = 1;

    // Phase E —— 吸附指示（由 ModulePanel drag 时写入）
    juce::Rectangle<int> dragPreview;
    juce::Array<int>     dragPreviewVGuides; // 竖线 X 坐标
    juce::Array<int>     dragPreviewHGuides; // 横线 Y 坐标
    bool                 hasDragPreview = false;

    // 正在从 ValueTree 批量恢复布局，此期间静默通知避免递归
    bool suspendNotifications = false;

    // Editor 在 chrome 隐藏态下指定的 hit-test 挖洞矩形（workspace 坐标系）
    //   · 落在这些矩形内的鼠标事件会被忽略（本组件 hitTest 返回 false），
    //     从而冒泡到父组件（Editor）去处理"浮动关闭按钮/标题文字点击"等顶部交互。
    //   · 空数组 = 无挖洞，默认响应所有像素（与 JUCE Component 默认行为一致）。
    juce::Array<juce::Rectangle<int>> hitTestHoles;

    // ======================================================
    // 拼豆像素画（PerlerImage） —— 拖入图片后的"画布贴画"
    //   · 每张贴画 = 一张像素化（每个 cell 10×10）+ 144 色量化后的 Image
    //   · 从 v2 起：每张贴画对应一个 PerlerImageLayer 子 Component，与 ModulePanel
    //     平级参与 JUCE 的 z-order；模块与图片之间按"后添加的在上"或显式 toFront
    //     的顺序互相遮挡。点击图片聚焦 → 调 layer->toFront(true) 让它冒到最上层。
    //   · 聚焦装饰（删除按钮 / cellSize 滑条 / PerlerBeads 复选框 / 缩放手柄）
    //     仍由 ModuleWorkspace::paintOverChildren 绘制在所有子组件之上 —— 因为
    //     它们只在"当前聚焦图片"周围出现，而聚焦图片已经 toFront 到最上，装饰
    //     不会视觉穿透到别的模块上。
    //   · 通过 mouseDown/mouseDrag/mouseUp 支持左键拖动（layer 鼠标穿透，
    //     事件直达 workspace，由 workspace 统一处理）
    // ======================================================
    struct PerlerImage
    {
        juce::Image    pixelImage;              // 已像素化 + 量化后的位图（cellsW*cellSize × cellsH*cellSize）
        juce::Point<int> topLeft { 0, 0 };      // canvas 坐标系左上角
        int            cellsW   = 0;            // 网格宽（列数）
        int            cellsH   = 0;            // 网格高（行数）
        int            cellSize = 10;           // 每个块的屏幕像素（固定 10）

        // 源图绝对路径（用于下次打开重建；空串 → 仅在内存存活，不持久化）
        juce::String   sourcePath;

        // "PerlerBeads" 渲染模式开关：
        //   · false（默认）→ 按原样绘制 pixelImage（实心方块贴画）
        //   · true          → 把每个像素块渲染为一个小圆环（仿 EQ 模块的
        //                    "珠子"视觉；颜色仍取 pixelImage 的 cell 颜色）
        //   该 flag 会被序列化到布局存档中，跨会话保留。
        bool           perlerBeadsMode = false;

        // 图片的全局透明度（0.0 = 完全透明；1.0 = 完全不透明，默认）
        //   · 由聚焦态图片右侧的"透明度"滑条控制
        //   · 同时作用于像素贴画模式与 PerlerBeads 圆环模式
        //   · 序列化到 XML（属性名 perlerOpacity）；兼容旧档默认回退 1.0
        float          opacity = 1.0f;

        juce::Rectangle<int> getBounds() const noexcept
        {
            return { topLeft.x, topLeft.y,
                     pixelImage.getWidth(), pixelImage.getHeight() };
        }
    };

    // 前置声明：PerlerImageLayer 定义在 .cpp 里（私有实现，仅负责绘制单张图片）
    class PerlerImageLayer;

    juce::OwnedArray<PerlerImage>     perlerImages;
    // 平行数组：每个索引 i 对应 perlerImages[i] 的子 Component。
    //   · OwnedArray 保证 workspace 析构时自动销毁所有 layer Component
    //   · 所有涉及 perlerImages 增/删/重排的位置都必须同步维护这个数组，
    //     即 perlerLayers[i] 永远与 perlerImages[i] 对应同一张图片
    juce::OwnedArray<juce::Component> perlerLayers;

    // 聚焦态：当前被选中的图片索引（-1 = 未选中）
    int                 focusedPerlerIdx     = -1;

    // 删除按钮 hover/press 状态（与 ModulePanel 风格一致）
    bool                deleteBtnHovered     = false;
    bool                deleteBtnPressed     = false;

    // 图片拖动态（左键按住命中图片后进入）
    int                 draggingPerlerIdx    = -1;   // 正在拖动的图片索引（-1 = 未拖）
    juce::Point<int>    perlerDragOffset;            // 鼠标落点到图片左上角的偏移
    juce::Rectangle<int> perlerDragStartRect;        // 拖动起始时的矩形（用于 repaint 脏区）

    // 缩放手柄（顶点 8 个：上中/下中/左中/右中 + 四个角）
    enum class ResizeHandle
    {
        none,
        topLeft,    top,    topRight,
        left,               right,
        bottomLeft, bottom, bottomRight
    };

    // 缩放拖动态
    int                  resizingPerlerIdx  = -1;    // 正在缩放的图片索引
    ResizeHandle         activeResizeHandle = ResizeHandle::none;
    juce::Rectangle<int> resizeStartRect;            // 起始矩形（平移框用，不含量化）
    int                  resizeStartCellsW  = 0;
    int                  resizeStartCellsH  = 0;
    juce::Point<int>     resizeAnchorPos;            // 锤点固定的对角（画布坐标）
    juce::Rectangle<int> resizePreviewRect;          // 拖动中的预览框（临时，没量化）
    int                  resizePreviewCellsW = 0;
    int                  resizePreviewCellsH = 0;
    // 缩放拖动覆盖的最大脏区（合并历次 preview，用于彻底清除底图残留）
    juce::Rectangle<int> resizeDirtyUnion;

    // cellSize 竖直滑块拖动态
    int                  cellSizeDraggingIdx = -1;   // -1 = 未拖动 cellSize 滑块

    // Bug4：拖动 cellSize 滑块过程中只记录"目标 cellSize"，mouseUp 时才真正 rebuild。
    //   · 拖动时 paint() 用 pendingCellSize 绘制拇指位置，给用户即时反馈
    //   · -1 = 无待处理值（非拖动态）
    int                  pendingCellSize      = -1;

    // opacity 竖直滑块拖动态（图片右侧）
    //   · 与 cellSize 滑块对称；拖动过程中直接更新 PerlerImage::opacity
    //     + repaint（opacity 只影响绘制，改值代价低，不需要 "pending" 缓存）
    //   · -1 = 未在拖动 opacity 滑块
    int                  opacityDraggingIdx   = -1;

    // 聚焦框的几何帮助函数。handleSize 固定 8px。
    static constexpr int handleSize     = 8;
    static constexpr int deleteBtnSize  = 18;  // 与 ModulePanel 右上角 × 按钮同尺寸

    // 默认初始 cellSize（拖入图片时使用）；聚焦态可经左侧滑块在 [1..15] 内切换
    static constexpr int defaultCellSize = 4;
    static constexpr int minCellSize     = 1;
    static constexpr int maxCellSize     = 15;

    // 聚焦态图片左侧的 cellSize 竖直滑块（样式 Pink XP 像素凹槽 + 凸起拇指）
    static constexpr int cellSizeSliderW     = 14;   // 滑槽宽度
    static constexpr int cellSizeSliderGap   = 6;    // 滑块到图片左边的间距
    static constexpr int cellSizeSliderThumb = 10;   // 拇指高度

    juce::Rectangle<int> getHandleRect  (juce::Rectangle<int> imgBounds, ResizeHandle h) const;
    juce::Rectangle<int> getDeleteBtnRect (juce::Rectangle<int> imgBounds) const;
    ResizeHandle         hitTestHandle (juce::Rectangle<int> imgBounds, juce::Point<int> p) const;

    // 聚焦态左侧 cellSize 滑块几何
    juce::Rectangle<int> getCellSizeSliderBounds (juce::Rectangle<int> imgBounds) const;
    juce::Rectangle<int> getCellSizeThumbBounds  (juce::Rectangle<int> imgBounds, int cellSize) const;
    // 根据滑槽内的 y 坐标反算 cellSize（会 clamp 到 [minCellSize..maxCellSize]）
    int                  cellSizeFromSliderY     (juce::Rectangle<int> imgBounds, int y) const;

    // 聚焦态图片"右侧"opacity 滑块几何（样式与左侧 cellSize 滑块对称）
    //   · 语义：上 = 完全不透明 (1.0)，下 = 完全透明 (0.0)
    //   · 只在 focusedPerlerIdx >= 0 且对应图片存在时绘制 / hit-test
    juce::Rectangle<int> getOpacitySliderBounds  (juce::Rectangle<int> imgBounds) const;
    juce::Rectangle<int> getOpacityThumbBounds   (juce::Rectangle<int> imgBounds, float opacity) const;
    // 根据滑槽内的 y 坐标反算 opacity（会 clamp 到 [0, 1]）
    float                opacityFromSliderY      (juce::Rectangle<int> imgBounds, int y) const;

    // 聚焦态装饰外包框（图片 + 四周所有装饰：左 cellSize 滑条 / 右 opacity
    // 滑条 / 上手柄+× / 下 PerlerBeads 复选框）。用于：
    //   · 聚焦图片时把 layer 子组件 bounds 扩大到此区域，使所有装饰区的
    //     鼠标事件都能被 layer 拦截（而不会冒泡到下方被遮挡的模块上）
    //   · repaint 脏区计算统一
    juce::Rectangle<int> getDecoratedBounds      (juce::Rectangle<int> imgBounds) const;

    // 聚焦态图片下方的 "PerlerBeads" 复选框几何（整行可点击 = 方框 + 文字）
    //   · 仅在 focusedPerlerIdx >= 0 时可见/可交互
    //   · 返回矩形为整行命中区；内部方框位于行首
    juce::Rectangle<int> getPerlerBeadsCheckboxBounds (juce::Rectangle<int> imgBounds) const;
    juce::Rectangle<int> getPerlerBeadsCheckboxBoxRect (juce::Rectangle<int> imgBounds) const;

    // 聚焦态复选框尺寸常量
    static constexpr int perlerBeadsRowH      = 18;   // 整行高度
    static constexpr int perlerBeadsBoxSize   = 14;   // 内部方框尺寸
    static constexpr int perlerBeadsRowGap    =  6;   // 图片底部到复选框顶部的间距

    // 把一张拼豆贴画按 "圆环模式" 渲染到画布（替代 drawImageAt）：
    //   · 每个 cell 读取 pixelImage 的中心像素颜色作为该圆环颜色
    //   · 圆环外径 ≈ cellSize/2，内径 ≈ cellSize * 0.2（复用 EqModule 的"珠子"比例）
    //   · alpha==0 的 cell 跳过，保留贴画原始透明区域
    void drawPerlerImageAsBeads (juce::Graphics& g, const PerlerImage& img, float opacity = 1.0f) const;

    // 从 HEX 颜色字符串解析出的 144 色拼豆色调色板（内嵌常量在 .cpp 中定义）
    static const juce::Array<juce::Colour>& getPerlerPalette();

    // 对指定原图执行：1) 按 cellSize×cellSize 平均降采样；2) 每格在 144 色中查找
    //   最近颜色（LAB 距离）；3) 返回"最终贴画"的 juce::Image（尺寸 = cellsW*cellSize × cellsH*cellSize）。
    //   · maxCellsOnLongSide 用于限制长边格子数（避免超大图卡顿）
    static juce::Image buildPerlerImage (const juce::Image& source,
                                         int cellSize,
                                         int maxCellsOnLongSide,
                                         int& outCellsW,
                                         int& outCellsH);

    // 按指定格子数量化（聚焦态缩放后使用；返回的 Image 尺寸 = targetCellsW*cellSize × targetCellsH*cellSize）
    static juce::Image buildPerlerImageFixed (const juce::Image& source,
                                              int cellSize,
                                              int targetCellsW,
                                              int targetCellsH);

    // 根据 canvas 坐标返回命中图片的索引（从最前 z-order 开始找，-1 = 未命中）
    int hitTestPerlerImageAt (juce::Point<int> p) const;

    // 加载并加入一张图片（file → buildPerlerImage → append）；失败返回 false
    //   fixedCellsW/H > 0 时：按指定格子数量化（用于从持久化恢复）
    //   topLeftOverride 非 nullptr 时：直接使用该位置作为左上角（用于恢复）；否则以 dropCanvasPos 为中心居中放置
    bool addPerlerImageFromFile (const juce::File& file,
                                 juce::Point<int> dropCanvasPos,
                                 int fixedCellsW = 0,
                                 int fixedCellsH = 0,
                                 const juce::Point<int>* topLeftOverride = nullptr,
                                 int forcedCellSize = 0);

    // 对指定图片重新按新格子数量化（缩放手柄松开时调用）
    //   · 优先用 sourcePath 重新加载原图重量化；源图不可用则基于当前 pixelImage 降级量化
    void rebuildPerlerImage (int idx, int newCellsW, int newCellsH);

    // 重载：同时修改 cellSize（聚焦态左侧像素大小滑块拖动时调用）
    //   · newCellSize 范围 [1..15]
    //   · 网格数 cellsW/cellsH 保持不变，仅像素尺寸变化；贴画屏幕像素尺寸同比缩放
    void rebuildPerlerImageCellSize (int idx, int newCellSize);

    // 删除当前聚焦图片（由 Delete 键 / 右上角 × 触发）
    void deleteFocusedPerlerImage();

    // 刷新鼠标指针形状（根据聚焦图片 + 当前鼠标位置下的 handle）
    void updatePerlerCursorFor (juce::Point<int> canvasPos);

    // 键盘：监听 Delete 删除聚焦图片
    bool keyPressed (const juce::KeyPress& key) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModuleWorkspace)
};

#endif // PBEQ_MODULE_WORKSPACE_H_INCLUDED