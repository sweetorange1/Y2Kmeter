#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"

// ==========================================================
// FPS 按钮专用的 mini LookAndFeel
//   只覆盖 getTextButtonFont，把"30FPS / 60FPS"的字号下调几 pt，
//   让 52px 宽的按钮里三个字母不再挤在一起。其它绘制（背景/文字颜色/
//   hover/pressed 等）完全沿用 PinkXP 风格。
// ==========================================================
namespace
{
    class FpsMiniLookAndFeel : public PinkXPLookAndFeel
    {
    public:
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            // 基线：min(14, h*0.55)；这里再 × 0.78 得到一个更紧凑的字号
            const float base = juce::jmin (14.0f, buttonHeight * 0.55f);
            return PinkXP::getFont (base * 0.78f, juce::Font::bold);
        }
    };

    // ======================================================
    // AddMenuItemComponent —— "添加模块"弹出菜单的自定义菜单项
    //   · JUCE 的 PopupMenu 本身没有 hover 回调，但它的 CustomComponent
    //     机制会在 item 被鼠标高亮时调用 setHighlighted(true)；切走时
    //     再调用 setHighlighted(false) —— 这正是我们想捕捉的 hover 事件。
    //   · 本类还承担原 item 的视觉绘制：调用当前 LookAndFeel 的
    //     drawPopupMenuItemWithOptions 完整复刻 PinkXP 风格的菜单条目；
    //     点击由基类 CustomComponent 的默认逻辑负责（点击 → dismiss +
    //     返回 item id 给 showMenuAsync 的回调）。
    // ======================================================
    class AddMenuItemComponent : public juce::PopupMenu::CustomComponent
    {
    public:
        AddMenuItemComponent (ModuleWorkspace& ws, ModuleType t, juce::String name)
            : juce::PopupMenu::CustomComponent (true /*triggeredAutomatically*/),
              workspace (ws), moduleType (t), displayName (std::move (name)) {}

        void getIdealSize (int& idealWidth, int& idealHeight) override
        {
            // 沿用当前 LookAndFeel 给普通菜单项的理想尺寸（保持与原生菜单视觉一致）
            int w = 180, h = 22;
            getLookAndFeel().getIdealPopupMenuItemSize (displayName, false, /*standardHeight*/ 22, w, h);
            idealWidth  = juce::jmax (w, 160);
            idealHeight = juce::jmax (h, 22);
        }

        void paint (juce::Graphics& g) override
        {
            // 构造一个等价的 PopupMenu::Item，用于让 LookAndFeel 绘制。
            //   · 保留原 item id / 文本；不需要填 customComponent（本类即是）。
            juce::PopupMenu::Item item (displayName);
            item.itemID = getItem() != nullptr ? getItem()->itemID : 0;

            getLookAndFeel().drawPopupMenuItem (g, getLocalBounds(),
                                                /*isSeparator*/ false,
                                                /*isActive*/    true,
                                                /*isHighlighted*/ isItemHighlighted(),
                                                /*isTicked*/    false,
                                                /*hasSubMenu*/  false,
                                                displayName,
                                                /*shortcutKey*/ juce::String(),
                                                /*icon*/        nullptr,
                                                /*textColour*/  nullptr);
        }

        // JUCE 在鼠标进入/离开此 item 时会调用 Component::mouseEnter/mouseExit。
        // 我们借此通知 workspace 更新 hover 预览状态。
        //   注：PopupMenu::CustomComponent::setHighlighted 是 @internal 的
        //   非 virtual 方法，不能重写；mouseEnter/Exit 才是可靠的 hover 信号。
        void mouseEnter (const juce::MouseEvent&) override
        {
            workspace.setAddMenuHoverPreview (true, moduleType);
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            // 切到别的 item 时，别项的 mouseEnter 会先触发并覆盖 hoverPreviewType；
            // 仅当 hover 离开整张菜单（鼠标到菜单外）时此分支有效清除预览。
            // 我们这里只在"离开时仍然是 me"的情况下关闭，避免和 enter 竞争。
            workspace.setAddMenuHoverPreview (false, moduleType);
        }

    private:
        ModuleWorkspace& workspace;
        ModuleType       moduleType;
        juce::String     displayName;
    };
}

// ==========================================================
// 类型 ↔ 字符串（布局持久化用）
// ==========================================================
juce::String moduleTypeToString(ModuleType t)
{
    switch (t)
    {
        case ModuleType::eq:                return "eq";
        case ModuleType::loudness:          return "loudness";
        case ModuleType::oscilloscope:      return "oscilloscope";
        case ModuleType::spectrum:          return "spectrum";
        case ModuleType::phase:             return "phase";
        case ModuleType::dynamics:          return "dynamics";

        case ModuleType::lufsRealtime:      return "lufs_realtime";
        case ModuleType::truePeak:          return "true_peak";
        case ModuleType::oscilloscopeLeft:  return "oscilloscope_left";
        case ModuleType::oscilloscopeRight: return "oscilloscope_right";
        case ModuleType::phaseCorrelation:  return "phase_correlation";
        case ModuleType::phaseBalance:      return "phase_balance";
        case ModuleType::dynamicsMeters:    return "dynamics_meters";
        case ModuleType::dynamicsDr:        return "dynamics_dr";
        case ModuleType::dynamicsCrest:     return "dynamics_crest";
        case ModuleType::waveform:          return "waveform";
        case ModuleType::vuMeter:           return "vu_meter";
        case ModuleType::spectrogram:       return "spectrogram";
    }
    return "eq";
}

ModuleType stringToModuleType(const juce::String& s, bool* ok)
{
    struct KV { const char* k; ModuleType v; };
    static const KV kv[] = {
        { "eq",                 ModuleType::eq },
        { "loudness",           ModuleType::loudness },
        { "oscilloscope",       ModuleType::oscilloscope },
        { "spectrum",           ModuleType::spectrum },
        { "phase",              ModuleType::phase },
        { "dynamics",           ModuleType::dynamics },

        { "lufs_realtime",      ModuleType::lufsRealtime },
        { "true_peak",          ModuleType::truePeak },
        { "oscilloscope_left",  ModuleType::oscilloscopeLeft },
        { "oscilloscope_right", ModuleType::oscilloscopeRight },
        { "phase_correlation",  ModuleType::phaseCorrelation },
        { "phase_balance",      ModuleType::phaseBalance },
        { "dynamics_meters",    ModuleType::dynamicsMeters },
        { "dynamics_dr",        ModuleType::dynamicsDr },
        { "dynamics_crest",     ModuleType::dynamicsCrest },
        { "waveform",           ModuleType::waveform },
        { "vu_meter",           ModuleType::vuMeter },
        { "spectrogram",        ModuleType::spectrogram },
    };
    for (auto& p : kv)
        if (s == p.k)
        {
            if (ok) *ok = true;
            return p.v;
        }
    if (ok) *ok = false;
    return ModuleType::eq;
}

// ==========================================================
// PerlerImageLayer —— 每张拼豆贴画对应一个子 Component
//   · 与 ModulePanel 平级参与 z-order，从而实现"图片与模块平级的图层关系"
//   · 鼠标事件由 layer 拦截（setInterceptsMouseClicks(true, false)），
//     然后统一转发回 ModuleWorkspace 处理 —— 这样即使图片盖在模块之上，
//     点击图片 / 装饰区也**不会**被下方的模块子组件抢走；同时 workspace
//     里原有的 hit-test / drag / resize / focus 逻辑无需任何改动。
//   · bounds 在非聚焦态下 = 图片矩形；聚焦态下 = 装饰外包框（含左侧
//     cellSize 滑条、右侧 opacity 滑条、下方复选框、手柄、×按钮等）；
//     由 workspace 通过 syncPerlerLayerBounds / updateFocusedPerlerLayerBounds
//     动态维护。
//   · paint 时用 g.setOrigin(-getX(), -getY()) 把 Graphics 原点对齐到
//     workspace 坐标系，从而复用 workspace 里既有的绘制代码，不用改算法。
// ==========================================================
class ModuleWorkspace::PerlerImageLayer : public juce::Component
{
public:
    PerlerImageLayer (ModuleWorkspace& ws, PerlerImage& imgRef) noexcept
        : workspace (ws), img (&imgRef)
    {
        // 拦截点击（但不向子组件派发 —— 我们没有子组件）
        //   这是修复"点击图片时事件被下方模块抢走"的关键：layer 自己消费
        //   mouseDown，然后 forwardMouseXxx 转给 workspace。
        setInterceptsMouseClicks (true, false);
        setWantsKeyboardFocus (false);
        setOpaque (false);
    }

    // 数据指针可能在批量重建时被重设（目前未使用，但保留接口以防后用）
    void retarget (PerlerImage& imgRef) noexcept { img = &imgRef; }

    void paint (juce::Graphics& g) override
    {
        if (img == nullptr || img->pixelImage.isNull()) return;

        // 把 Graphics 原点临时平移到 workspace 坐标系（layer 在父坐标系里
        //   的位置等于 getX()/getY()；setOrigin(-x,-y) 后 g 的 (0,0)
        //   就对应 workspace 的 (0,0)）→ 复用 workspace 已有的绘制代码。
        juce::Graphics::ScopedSaveState save (g);
        g.setOrigin (-getX(), -getY());

        // 全局不透明度（右侧 opacity 滑条控制；默认 1.0）
        const float alpha = juce::jlimit (0.0f, 1.0f, img->opacity);

        if (img->perlerBeadsMode)
        {
            // 圆环模式：opacity 必须乘进每个 bead 的颜色 alpha，
            //   因为 drawPerlerImageAsBeads 内部会调 g.setColour(col)，
            //   这会覆盖 g.setOpacity 的效果，导致透明度调节无效。
            workspace.drawPerlerImageAsBeads (g, *img, alpha);
        }
        else
        {
            // 实心模式：g.setOpacity 对 drawImageAt 有效
            g.setOpacity (alpha);
            g.drawImageAt (img->pixelImage, img->topLeft.x, img->topLeft.y);
        }
    }

    // --------------------------------------------------------
    // 鼠标事件：转发到 workspace（把位置换到 workspace 坐标系）
    //   · JUCE 的 MouseEvent::getEventRelativeTo 即可完成坐标转换
    //   · workspace 原有 mouseDown/Drag/Up/Move/Exit 实现完全不用改
    // --------------------------------------------------------
    void mouseDown       (const juce::MouseEvent& e) override { workspace.mouseDown       (e.getEventRelativeTo (&workspace)); }
    void mouseDrag       (const juce::MouseEvent& e) override { workspace.mouseDrag       (e.getEventRelativeTo (&workspace)); }
    void mouseUp         (const juce::MouseEvent& e) override { workspace.mouseUp         (e.getEventRelativeTo (&workspace)); }
    void mouseMove       (const juce::MouseEvent& e) override { workspace.mouseMove       (e.getEventRelativeTo (&workspace)); }
    void mouseExit       (const juce::MouseEvent& e) override { workspace.mouseExit       (e.getEventRelativeTo (&workspace)); }
    // 注：mouseDoubleClick 不转发 —— workspace 的双击会走 showAddMenu 弹"添加
    //   模块"菜单，在图片/装饰区双击没必要触发这个行为。

private:
    ModuleWorkspace& workspace;
    PerlerImage*     img = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerlerImageLayer)
};

// ==========================================================
// ModuleWorkspace
// ==========================================================
ModuleWorkspace::ModuleWorkspace()
{
    // 工作区自身接收鼠标事件（用于双击/右键添加模块）
    setInterceptsMouseClicks(true, true);
    // 支持键盘焦点：聚焦拼豆图片后按 Delete 删除
    setWantsKeyboardFocus(true);

    // 底部工具栏：主题选择器（XP 画图调色板样式）
    addAndMakeVisible(themeBar);

    // 底部工具栏中部：音频信号源下拉（默认只放一个占位项 "System Output (Loopback)"，
    // 由 Standalone App 在启动时通过 setAudioSourceItems() 替换为真实设备列表）
    audioSourceLabel.setText ("Source", juce::dontSendNotification);
    audioSourceLabel.setJustificationType (juce::Justification::centredRight);
    audioSourceLabel.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
    audioSourceLabel.setColour (juce::Label::textColourId, PinkXP::ink);
    addAndMakeVisible (audioSourceLabel);

    audioSourceItems.clear();
    audioSourceItems.add ({ "System Output (Loopback)", "loopback:default", true });
    audioSourceBox.clear (juce::dontSendNotification);
    audioSourceBox.addItem (audioSourceItems[0].displayName, 1);
    audioSourceBox.setSelectedId (1, juce::dontSendNotification);
    audioSourceBox.setTooltip ("Audio source: defaults to system master output (Loopback).");

    // 显式挂 Pink XP LookAndFeel —— 保证下拉框 / 弹出菜单的配色与主题统一
    //（否则 ComboBox 会使用 LookAndFeel_V4 的默认浅蓝色，跟粉色主题格格不入）
    audioSourceBox.setLookAndFeel (&getPinkXPLookAndFeel());

    audioSourceBox.onChange = [this]()
    {
        const int idx = audioSourceBox.getSelectedItemIndex();
        if (idx < 0 || idx >= audioSourceItems.size()) return;
        const auto sourceId  = audioSourceItems.getReference (idx).sourceId;
        const auto isLoopback = audioSourceItems.getReference (idx).isLoopback;

        // 异步派发切换：让下拉框先完成关闭动画 / 下一帧重绘，再在消息队列
        // 下一个 tick 里执行重建音频设备的重活（WASAPI loopback init /
        // setAudioDeviceSetup 都是阻塞调用，耗时 50~200ms，同步在
        // onChange 里执行会让 UI 卡一下，用户点一下感觉"僵住"）。
        juce::Component::SafePointer<ModuleWorkspace> safe (this);
        juce::MessageManager::callAsync ([safe, sourceId, isLoopback]()
        {
            if (safe == nullptr) return;
            if (safe->onAudioSourceChanged)
                safe->onAudioSourceChanged (sourceId, isLoopback);
        });
    };
    addAndMakeVisible (audioSourceBox);

    // 右下角 Hide 按钮：默认"显示态"不透明
    hideBtn.setButtonText("Hide");
    hideBtn.setDimWhenIdle(false);
    hideBtn.onClick = [this]() { setChromeVisible(! chromeVisible); };
    addAndMakeVisible(hideBtn);

    // 左下角：FPS 限制按钮 + 实时 FPS 标签
    //   · 按钮文案："30 FPS" / "60 FPS"（点击切换）
    //   · 标签文案："-- fps" / "29.8 fps" 之类，由 Editor 定期计算后 setMeasuredFps
    fpsBtn.setButtonText ("30FPS");
    fpsBtn.setTooltip    ("Click to toggle target frame rate (30 / 60 FPS)");
    fpsBtn.onClick = [this]()
    {
        setFpsLimit (fpsLimit == 30 ? 60 : 30);
        if (onFpsLimitChanged) onFpsLimitChanged (fpsLimit);
    };
    // 专用 mini LookAndFeel：把 "30FPS/60FPS" 渲染得更紧凑（仅影响此按钮）
    fpsMiniLnf = std::make_unique<FpsMiniLookAndFeel>();
    fpsBtn.setLookAndFeel (fpsMiniLnf.get());
    addAndMakeVisible (fpsBtn);

    fpsLabel.setText ("-- fps", juce::dontSendNotification);
    fpsLabel.setJustificationType (juce::Justification::centredLeft);
    fpsLabel.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
    fpsLabel.setColour (juce::Label::textColourId, PinkXP::ink);
    addAndMakeVisible (fpsLabel);

    // Grid 显示切换按钮：在画布上叠加 8px 网格线（方便对齐拼豆贴画/模块）
    //   · 默认关闭（gridOverlayVisible=false）；通过 layout tree 持久化
    //   · 使用 setClickingTogglesState 让按钮保持"按下"视觉状态
    gridBtn.setButtonText ("Grid");
    gridBtn.setTooltip    ("Toggle 8px alignment grid overlay");
    gridBtn.setClickingTogglesState (true);
    gridBtn.setToggleState (gridOverlayVisible, juce::dontSendNotification);
    gridBtn.onClick = [this]()
    {
        gridOverlayVisible = gridBtn.getToggleState();
        repaint();
        notifyLayoutChanged();
    };
    addAndMakeVisible (gridBtn);

    // 布局预设下拉框（位于 Grid 按钮左侧）
    //   · 样式与音频源下拉完全一致：显式挂 PinkXP LookAndFeel，ComboBox 箭头 + 条目
    //     颜色跟随主题；nothing-selected 文本作为占位，选中后触发 onLayoutPresetChanged
    layoutPresetBox.clear (juce::dontSendNotification);
    layoutPresetBox.setTextWhenNothingSelected ("preasent");
    layoutPresetBox.setTooltip ("Choose a layout preset");
    layoutPresetBox.addItem ("Default",             (int) LayoutPreset::defaultGrid);
    layoutPresetBox.addItem ("Horizontal Bar(T)",      (int) LayoutPreset::horizontalFull);
    layoutPresetBox.addItem ("Horizontal Bar(B)", (int) LayoutPreset::horizontalBottom);
    layoutPresetBox.addItem ("Tiled",               (int) LayoutPreset::tiled);
    layoutPresetBox.setLookAndFeel (&getPinkXPLookAndFeel());
    layoutPresetBox.onChange = [this]()
    {
        const int id = layoutPresetBox.getSelectedId();
        if (id <= 0) return;
        const auto preset = (LayoutPreset) id;

        // 选中后立刻清回 "nothing selected"，这样下一次点同一个预设也能
        // 触发 onChange（ComboBox 默认的"已选中同一项不再回调"语义会让 Preset 1
        // 变成一次性按钮，用户重新点无响应）。用 dontSendNotification 避免递归。
        juce::Component::SafePointer<ModuleWorkspace> safe (this);
        juce::MessageManager::callAsync ([safe, preset]()
        {
            if (safe == nullptr) return;
            if (safe->onLayoutPresetChanged)
                safe->onLayoutPresetChanged (preset);
            safe->layoutPresetBox.setSelectedId (0, juce::dontSendNotification);
        });
    };
    addAndMakeVisible (layoutPresetBox);

    // 布局预设 Save/Load 按钮（紧邻 layoutPresetBox 左侧）
    //   · 按钮样式跟随全局 LookAndFeel（与 gridBtn / fpsBtn 一致，像素化 TextButton）
    //   · 点击 Save：弹 FileChooser 让用户选择"另存为"位置，默认后缀 .settings；
    //               选定后通过 onSavePresetRequested 把 File 传给外层去复制。
    //   · 点击 Load：弹 FileChooser 让用户挑选之前保存的 .settings 文件；选定后
    //               通过 onLoadPresetRequested 把 File 传给外层去覆盖当前 settings
    //               并触发重启。
    //   · 用 std::shared_ptr<juce::FileChooser> 持有（launchAsync 要求 chooser 生命
    //     周期覆盖到回调触发；lambda 捕获 shared_ptr 保证不析构）。
    savePresetBtn.setButtonText ("Save");
    savePresetBtn.setTooltip    ("Save current settings (layout + theme + window) as a preset file");
    savePresetBtn.onClick = [this]()
    {
        auto chooser = std::make_shared<juce::FileChooser> (
            "Save current settings as preset",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getChildFile (juce::String (JucePlugin_Name) + ".settings"),
            "*.settings");

        const auto flags = juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectFiles
                         | juce::FileBrowserComponent::warnAboutOverwriting;

        juce::Component::SafePointer<ModuleWorkspace> safe (this);
        chooser->launchAsync (flags, [safe, chooser](const juce::FileChooser& fc)
        {
            if (safe == nullptr) return;
            auto result = fc.getResult();
            if (result == juce::File{}) return;                 // 用户取消
            if (result.getFileExtension().isEmpty())
                result = result.withFileExtension ("settings");  // 补全后缀
            if (safe->onSavePresetRequested)
                safe->onSavePresetRequested (result);
        });
    };
    addAndMakeVisible (savePresetBtn);

    loadPresetBtn.setButtonText ("Load");
    loadPresetBtn.setTooltip    ("Load a previously saved preset file (reloads the app)");
    loadPresetBtn.onClick = [this]()
    {
        auto chooser = std::make_shared<juce::FileChooser> (
            "Load preset",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
            "*.settings");

        const auto flags = juce::FileBrowserComponent::openMode
                         | juce::FileBrowserComponent::canSelectFiles;

        juce::Component::SafePointer<ModuleWorkspace> safe (this);
        chooser->launchAsync (flags, [safe, chooser](const juce::FileChooser& fc)
        {
            if (safe == nullptr) return;
            const auto result = fc.getResult();
            if (result == juce::File{} || ! result.existsAsFile()) return;   // 用户取消/无效
            if (safe->onLoadPresetRequested)
                safe->onLoadPresetRequested (result);
        });
    };
    addAndMakeVisible (loadPresetBtn);

    // 订阅主题变更：切换主题后 PinkXP::ink 已改变，但 Label 的 textColour
    //   是缓存值（JUCE Label 用存储的 Colour 绘制），必须显式刷新，否则
    //   在暗色主题下 Label 仍显示"bubblegum 的深色 ink"，在黑底上看不清。
    //   同理 audioSourceBox 的 label 颜色由 LookAndFeel 缓存决定，这里
    //   用 sendLookAndFeelChange() 触发 JUCE 的 colourChanged() 从 LookAndFeel
    //   重新取色（PinkXPStyle::applyTheme 已经刷新了 LnF 的 ColourScheme 缓存）。
    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        fpsLabel.setColour (juce::Label::textColourId, PinkXP::ink);
        fpsLabel.repaint();

        audioSourceLabel.setColour (juce::Label::textColourId, PinkXP::ink);
        audioSourceLabel.repaint();

        // 让 ComboBox 的 label 颜色 / 箭头从 LookAndFeel 最新 ColourScheme 重新取
        audioSourceBox.sendLookAndFeelChange();
        audioSourceBox.repaint();

        // layoutPresetBox 同理刷新
        layoutPresetBox.sendLookAndFeelChange();
        layoutPresetBox.repaint();

        // hover 预览快照使用旧主题配色渲染，主题变更后必须清空重建
        hoverPreviewCache.clear();
    });
    // 构造时全局调色板可能尚未被 PluginEditor 的 applyTheme 覆盖，
    //   先按当前值设一次；即便日后再切主题，上面的订阅也会刷新。
    fpsLabel.setColour (juce::Label::textColourId, PinkXP::ink);
    audioSourceLabel.setColour (juce::Label::textColourId, PinkXP::ink);
}

ModuleWorkspace::~ModuleWorkspace()
{
    // 显式解绑 LookAndFeel：audioSourceBox 生命周期随本对象，
    // 但 getPinkXPLookAndFeel() 是进程级单例；在某些宿主卸载顺序下
    // （先析构 Component 再释放 LookAndFeel）不解绑也行，但显式解绑
    // 能消除 JUCE 在 assertAllComponentsMustReleaseLF 的兼容性隐患。
    audioSourceBox.setLookAndFeel (nullptr);

    // layoutPresetBox 同 audioSourceBox 一样挂了 PinkXP LookAndFeel 单例，这里显式解绑
    layoutPresetBox.setLookAndFeel (nullptr);

    // fpsBtn 绑定的是本对象持有的 fpsMiniLnf（unique_ptr）。
    // 必须先解绑，再让 unique_ptr 自行析构，否则 Button 在析构时会访问已销毁的 LnF。
    fpsBtn.setLookAndFeel (nullptr);

    // 解绑主题订阅，避免析构后 gThemeSubs 仍回调到已销毁的 this
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }
}

// ----------------------------------------------------------
// 区域计算
// ----------------------------------------------------------
juce::Rectangle<int> ModuleWorkspace::getCanvasArea() const
{
    // chrome 隐藏时，canvas 满铺（不再留 toolbar 空间）
    auto r = getLocalBounds();
    if (chromeVisible)
        r = r.withTrimmedBottom(toolbarHeight);
    return r.reduced(margin);
}

juce::Rectangle<int> ModuleWorkspace::getToolbarArea() const
{
    auto r = getLocalBounds();
    return r.removeFromBottom(toolbarHeight);
}

// ----------------------------------------------------------
// 网格吸附
// ----------------------------------------------------------
juce::Rectangle<int> ModuleWorkspace::snapRect(juce::Rectangle<int> r) const
{
    auto snap = [](int v) { return ((v + gridSize / 2) / gridSize) * gridSize; };
    const int x = snap(r.getX());
    const int y = snap(r.getY());
    const int w = juce::jmax(gridSize, snap(r.getWidth()));
    const int h = juce::jmax(gridSize, snap(r.getHeight()));
    return juce::Rectangle<int>(x, y, w, h);
}

void ModuleWorkspace::snapToGrid(ModulePanel& panel)
{
    auto canvas = getCanvasArea();

    // 1) 先做网格对齐
    auto snapped = snapRect(panel.getBounds());

    // 2) 与邻居吸附（边缘贴齐），guides 在这里不用，仅最终 commit
    juce::Array<int> dummyV, dummyH;
    snapped = snapToNeighbours(snapped, &panel, dummyV, dummyH);

    // 3) 不越界
    snapped.setX(juce::jlimit(canvas.getX(), juce::jmax(canvas.getX(),
                              canvas.getRight()  - snapped.getWidth()),  snapped.getX()));
    snapped.setY(juce::jlimit(canvas.getY(), juce::jmax(canvas.getY(),
                              canvas.getBottom() - snapped.getHeight()), snapped.getY()));

    panel.setBounds(snapped);
}

// ----------------------------------------------------------
// Phase E —— 邻居吸附（snap-to-edge）
//   对 r 的每条边（左/右/上/下）与其他模块的对应边做距离判断，
//   若 <= snapThreshold，则对齐并把对齐坐标塞入 outGuides。
// ----------------------------------------------------------
juce::Rectangle<int> ModuleWorkspace::snapToNeighbours(juce::Rectangle<int> r,
                                                       const ModulePanel* except,
                                                       juce::Array<int>& outVGuides,
                                                       juce::Array<int>& outHGuides) const
{
    constexpr int snapThreshold = 6;

    const int rLeft  = r.getX();
    const int rRight = r.getRight();
    const int rTop   = r.getY();
    const int rBot   = r.getBottom();

    int bestDxLeft  = snapThreshold + 1, bestXShift = 0, bestVGuide = 0; bool hitV = false;
    int bestDyTop   = snapThreshold + 1, bestYShift = 0, bestHGuide = 0; bool hitH = false;

    auto tryXAlign = [&](int candidate, int targetGuide)
    {
        const int dL = std::abs(candidate - rLeft);
        if (dL < bestDxLeft)       { bestDxLeft = dL; bestXShift = candidate - rLeft;       bestVGuide = targetGuide; hitV = true; }
        const int dR = std::abs(candidate - rRight);
        if (dR < bestDxLeft)       { bestDxLeft = dR; bestXShift = candidate - rRight;      bestVGuide = targetGuide; hitV = true; }
    };
    auto tryYAlign = [&](int candidate, int targetGuide)
    {
        const int dT = std::abs(candidate - rTop);
        if (dT < bestDyTop)        { bestDyTop = dT; bestYShift = candidate - rTop;         bestHGuide = targetGuide; hitH = true; }
        const int dB = std::abs(candidate - rBot);
        if (dB < bestDyTop)        { bestDyTop = dB; bestYShift = candidate - rBot;         bestHGuide = targetGuide; hitH = true; }
    };

    // 工作区边界也作为对齐目标
    const auto canvas = getCanvasArea();
    tryXAlign(canvas.getX(),     canvas.getX());
    tryXAlign(canvas.getRight(), canvas.getRight());
    tryYAlign(canvas.getY(),      canvas.getY());
    tryYAlign(canvas.getBottom(), canvas.getBottom());

    // 邻居模块的边
    for (auto* other : modules)
    {
        if (other == except) continue;
        const auto b = other->getBounds();
        tryXAlign(b.getX(),      b.getX());
        tryXAlign(b.getRight(),  b.getRight());
        tryYAlign(b.getY(),      b.getY());
        tryYAlign(b.getBottom(), b.getBottom());
    }

    if (hitV && bestDxLeft <= snapThreshold)
    {
        r.translate(bestXShift, 0);
        outVGuides.add(bestVGuide);
    }
    if (hitH && bestDyTop <= snapThreshold)
    {
        r.translate(0, bestYShift);
        outHGuides.add(bestHGuide);
    }
    return r;
}

// ----------------------------------------------------------
// 找空位置（简单流式：从左到右，行高取当前行最高）
// ----------------------------------------------------------
juce::Rectangle<int> ModuleWorkspace::findNextSlot(int w, int h) const
{
    auto canvas = getCanvasArea();
    if (w > canvas.getWidth())  w = canvas.getWidth();
    if (h > canvas.getHeight()) h = canvas.getHeight();

    int curX = canvas.getX();
    int curY = canvas.getY();
    int rowMaxBottom = curY;

    // 粗糙的无重叠贪心：对已有模块按 Y 升序扫描，每放一个移动 curX
    // 这里采用更简单的策略：从左上到右下以 gridSize 步长搜索首个不重叠矩形
    for (int y = canvas.getY(); y + h <= canvas.getBottom(); y += gridSize)
    {
        for (int x = canvas.getX(); x + w <= canvas.getRight(); x += gridSize)
        {
            juce::Rectangle<int> test(x, y, w, h);
            bool overlap = false;
            for (int i = 0; i < modules.size(); ++i)
            {
                if (test.intersects(modules.getUnchecked(i)->getBounds()))
                {
                    overlap = true;
                    break;
                }
            }
            if (!overlap)
                return test;
        }
    }

    // 实在找不到，返回左上角（允许重叠）
    juce::ignoreUnused(curX, curY, rowMaxBottom);
    return juce::Rectangle<int>(canvas.getX(), canvas.getY(), w, h);
}

// ----------------------------------------------------------
// 添加 / 移除模块
// ----------------------------------------------------------
void ModuleWorkspace::hookPanel(ModulePanel& panel)
{
    panel.onBoundsChangedByUser = [this](ModulePanel& p)
    {
        snapToGrid(p);
        clearDragPreview();
        notifyLayoutChanged();
    };
    panel.onBoundsDragging = [this](ModulePanel& p)
    {
        updateDragPreview(p);
    };
    panel.onCloseClicked   = [this](ModulePanel& p) { removeModule(p); };
    // bug2：点击任何模块（无论标题栏 / 内容区 / 缩放边缘）都取消图片聚焦
    //   · ModulePanel::mouseDown 最开头就会 toFront(true) + onBroughtToFront(*this)，
    //     因此这里能拦住"点击子模块"这条事件路径（父 ModuleWorkspace 的 mouseDown
    //     本来只响应 canvas 空白区，点到子模块时根本收不到 mouseDown）
    //   · 仅在 focusedPerlerIdx 有效时执行 repaint，避免无聚焦时的无谓刷新
    panel.onBroughtToFront = [this](ModulePanel&)
    {
        if (focusedPerlerIdx >= 0)
        {
            const int oldFocus = focusedPerlerIdx;
            focusedPerlerIdx    = -1;
            draggingPerlerIdx   = -1;
            resizingPerlerIdx   = -1;
            cellSizeDraggingIdx = -1;
            pendingCellSize     = -1;
            opacityDraggingIdx  = -1;
            activeResizeHandle  = ResizeHandle::none;
            deleteBtnHovered    = false;
            deleteBtnPressed    = false;
            // 收回旧聚焦 layer 的 bounds（从装饰外包框收回到图片本体矩形）
            syncPerlerLayerBounds (oldFocus);
            repaint();
        }
    };
}

ModulePanel& ModuleWorkspace::addModule(std::unique_ptr<ModulePanel> panel, bool autoPosition)
{
    jassert(panel != nullptr);
    auto* raw = panel.get();

    if (raw->getModuleId().isEmpty())
        raw->setModuleId(juce::String(nextIdCounter++));

    addAndMakeVisible(*raw);
    hookPanel(*raw);

    if (autoPosition || raw->getBounds().isEmpty())
    {
        // 优先使用具体模块在构造函数里声明的默认大小
        //   (setDefaultSize)，而非全局统一的 320×220；这样每种模块第一次
        //   被加入就能回复到用户期望的尺寸（比如 EQ 6×4 大格→384×256）。
        const int w = juce::jmax(raw->getMinWidth(),  raw->getDefaultWidth());
        const int h = juce::jmax(raw->getMinHeight(), raw->getDefaultHeight());
        auto slot = findNextSlot(w, h);
        raw->setBounds(snapRect(slot));
    }

    modules.add(panel.release());
    raw->toFront(false);
    notifyLayoutChanged();
    return *raw;
}

ModulePanel* ModuleWorkspace::addModuleByType(ModuleType t)
{
    if (factory == nullptr)
    {
        jassertfalse;
        return nullptr;
    }
    auto panel = factory(t);
    if (panel == nullptr)
        return nullptr;
    return &addModule(std::move(panel));
}

void ModuleWorkspace::removeModule(ModulePanel& panel)
{
    for (int i = modules.size(); --i >= 0;)
    {
        if (modules.getUnchecked(i) == &panel)
        {
            modules.remove(i);  // OwnedArray 会自动 delete
            repaint();
            notifyLayoutChanged();
            return;
        }
    }
}

// ----------------------------------------------------------
// 清空所有模块 + 拼豆贴画（用于布局预设切换）
//   · 不触发 onLayoutChanged：切换过程由外部负责重新填充，避免中间态
//     把"空布局"错误写回 Processor 的 savedLayoutXml
// ----------------------------------------------------------
void ModuleWorkspace::clearAllModules()
{
    // OwnedArray 析构时会自动 delete 每个 ModulePanel
    for (int i = modules.size(); --i >= 0;)
        modules.remove (i);

    // 同步清理拼豆贴画：先清 layer（子 Component）再清数据结构，避免 layer->img 野指针
    perlerLayers.clear();
    perlerImages.clear();
    focusedPerlerIdx   = -1;
    draggingPerlerIdx  = -1;
    resizingPerlerIdx  = -1;
    activeResizeHandle = ResizeHandle::none;
    repaint();
}

// ----------------------------------------------------------
// 自动流式布局（按 2 列）
// ----------------------------------------------------------
void ModuleWorkspace::autoLayout()
{
    auto canvas = getCanvasArea();
    const int cols = 2;
    const int colGap = gridSize;
    const int rowGap = gridSize;
    const int cellW = (canvas.getWidth() - colGap * (cols - 1)) / cols;

    int row = 0;
    int col = 0;
    int rowTop = canvas.getY();
    int currentRowHeight = 0;

    for (auto* m : modules)
    {
        const int w = juce::jmax(m->getMinWidth(), cellW);
        // 模块高度优先走自己的 default，既保留各模块的设计比例
        //  （如 Phase 6×3 vs EQ 6×4），又避免统一高度造成视觉高矮不一。
        const int h = juce::jmax(m->getMinHeight(), m->getDefaultHeight());

        if (col >= cols)
        {
            col = 0;
            ++row;
            rowTop += currentRowHeight + rowGap;
            currentRowHeight = 0;
        }

        int x = canvas.getX() + col * (cellW + colGap);
        int y = rowTop;
        m->setBounds(snapRect({ x, y, w, h }));

        currentRowHeight = juce::jmax(currentRowHeight, h);
        ++col;
    }
}

// ----------------------------------------------------------
// 添加模块菜单（双击 / 右键空白区触发）
// ----------------------------------------------------------
void ModuleWorkspace::showAddMenu(juce::Point<int> anchorScreenPos,
                                   juce::Point<int> placeAtCanvasPos)
{
    juce::PopupMenu m;
    m.setLookAndFeel(&getLookAndFeel());

    // ------------------------------------------------------------
    // Bug2：预览框位置 vs. 菜单位置的智能避让
    //   · 原行为：预览框左上角 = 鼠标点；菜单也弹在鼠标右下 → 两者重叠
    //   · 新策略：先判断 canvas 右侧剩余空间是否放得下"默认模块"尺寸的预览框：
    //        - 放得下 → 预览在鼠标右下；菜单锚到预览框左边（出现在左下）
    //        - 放不下 → 预览改到鼠标左下；菜单锚到预览框右边（出现在右下）
    //     这样预览与菜单始终分立于鼠标点两侧，永不互相遮挡。
    //   · 把最终计算出的预览框左上角写入 hoverPreviewPos，paintOverChildren
    //     直接用这个位置画预览，与菜单的 target 保持一致。
    // ------------------------------------------------------------
    const bool  hasPlacement = (placeAtCanvasPos.x >= 0 && placeAtCanvasPos.y >= 0);
    const auto  canvas       = getCanvasArea();
    // 预览框大小：用"当前 hover 的模块类型"的默认尺寸；菜单刚弹出时还没
    //   hover 某项，用 EQ 作为稳妥兜底（不影响菜单摆位计算）。
    const auto  prevSize     = getDefaultSizeForType (hoverPreviewActive ? hoverPreviewType
                                                                         : ModuleType::eq);
    // 自动缩小：窗口过小时把预览框夹到 canvas 的实际尺寸，保持和真实插入
    //   路径（showMenuAsync lambda 中）的"模块不越过下方控制区"逻辑一致，
    //   用户看到的 ghost 与最终放下来的模块大小相符。
    const int   prevW        = juce::jmin (prevSize.x, juce::jmax (1, canvas.getWidth()));
    const int   prevH        = juce::jmin (prevSize.y, juce::jmax (1, canvas.getHeight()));

    juce::Rectangle<int> previewBoxLocal; // workspace 本地坐标系下的预览框
    bool previewOnRight = true;           // 预览框在鼠标的右下（true）或左下（false）

    if (hasPlacement)
    {
        // 1) 尝试预览放在鼠标右下
        int rx = placeAtCanvasPos.x;
        int ry = placeAtCanvasPos.y;
        const int rightSpace = canvas.getRight() - rx;

        if (rightSpace >= prevW)
        {
            // 右侧放得下 → 预览在右下，菜单在左下
            previewOnRight = true;
        }
        else
        {
            // 右侧放不下 → 预览改到鼠标左下（以鼠标点为预览框右上角）
            previewOnRight = false;
            rx = placeAtCanvasPos.x - prevW;
        }

        // 夹紧到 canvas（与 paintOverChildren 的逻辑保持一致）
        rx = juce::jlimit (canvas.getX(),
                           juce::jmax (canvas.getX(), canvas.getRight()  - prevW), rx);
        ry = juce::jlimit (canvas.getY(),
                           juce::jmax (canvas.getY(), canvas.getBottom() - prevH), ry);

        // 8px 网格吸附（与 paintOverChildren 一致）
        previewBoxLocal = snapRect ({ rx, ry, prevW, prevH });
        hoverPreviewPos = previewBoxLocal.getTopLeft();
    }
    else
    {
        // 非落点触发：预览画在 canvas 中心（保留旧行为）
        hoverPreviewPos = canvas.getCentre();
        previewBoxLocal = snapRect ({ hoverPreviewPos.x, hoverPreviewPos.y, prevW, prevH });
    }

    // 预测量菜单宽度：沿用 AddMenuItemComponent::getIdealSize 的 LookAndFeel 路径，
    //   对每个条目算一次理想宽度取最大，再加上 popup 左右 border 与 minimumWidth 兜底。
    //   这样我们就能精准把菜单的某个角钉在鼠标点上。
    auto& lfRef            = getLookAndFeel();
    const int menuMinWidth = 160;
    int maxItemW = menuMinWidth;
    for (int i = 0; i < availableTypes.size(); ++i)
    {
        const auto t = availableTypes[i];
        // 用 CustomComponent 替换普通 addItem，借 setHighlighted 捕捉 hover
        juce::PopupMenu::Item item (getModuleDisplayName (t));
        item.itemID = i + 1;
        item.customComponent = new AddMenuItemComponent (*this, t, getModuleDisplayName (t));
        m.addItem (std::move (item));

        int iw = 180, ih = 22;
        lfRef.getIdealPopupMenuItemSize (getModuleDisplayName (t), false, 22, iw, ih);
        maxItemW = juce::jmax (maxItemW, iw);
    }

    auto options = juce::PopupMenu::Options()
        .withMinimumWidth (menuMinWidth);

    // Popup 左右边框（LookAndFeel 会把它加在菜单宽度两侧）
    const int popupBorder = lfRef.getPopupMenuBorderSizeWithOptions (options);
    // 估算菜单总宽度（保守地多留 4px，避免极端字体下四舍五入偏差）
    const int estimatedMenuW = juce::jmax (menuMinWidth, maxItemW + popupBorder * 2) + 4;

    // Bug2：根据"预览框在鼠标左下 / 右下"决定菜单的哪一个角对齐到鼠标点击处。
    //   JUCE 的 PopupMenu 在 targetArea 非空时，菜单左上角 ≈ (target.getX(), target.getBottom())，
    //   下方空间不足时会自动翻到上方。我们利用这一规律：
    //     · 预览在鼠标右下 → 菜单右上角对齐鼠标点 → target.x = mouseX - menuW
    //     · 预览在鼠标左下 → 菜单左上角对齐鼠标点 → target.x = mouseX
    //   target 高度取 1，让 target.getBottom() = mouseScreenY，菜单顶边即鼠标点。
    juce::Rectangle<int> targetArea;
    if (hasPlacement)
    {
        const int mx = anchorScreenPos.x;
        const int my = anchorScreenPos.y;
        if (previewOnRight)
            targetArea = { mx - estimatedMenuW, my - 1, estimatedMenuW, 1 };
        else
            targetArea = { mx, my - 1, estimatedMenuW, 1 };
    }
    else
    {
        // 兜底：用 1×1 锚点回到旧行为
        targetArea = { anchorScreenPos.x, anchorScreenPos.y, 1, 1 };
    }

    options = options.withTargetScreenArea (targetArea);

    const auto placePos = placeAtCanvasPos;

    m.showMenuAsync(options, [this, hasPlacement, placePos](int result)
    {
        // 无论用户是选中了某项还是取消菜单，都清除 hover 预览
        if (hoverPreviewActive)
        {
            hoverPreviewActive = false;
            repaint();
        }

        if (result <= 0)
            return;
        const int idx = result - 1;
        if (idx < 0 || idx >= availableTypes.size())
            return;

        if (! hasPlacement)
        {
            addModuleByType(availableTypes[idx]);
            return;
        }

        // 把新模块放到点击位置（canvas 内部坐标 = ModuleWorkspace 组件坐标）
        if (factory == nullptr) { jassertfalse; return; }
        auto panel = factory(availableTypes[idx]);
        if (panel == nullptr) return;

        // 使用该模块自己声明的 default 大小
        int w = juce::jmax(panel->getMinWidth(),  panel->getDefaultWidth());
        int h = juce::jmax(panel->getMinHeight(), panel->getDefaultHeight());

        auto canvas = getCanvasArea();

        // 自动缩小：当窗口被用户调到很小、default 尺寸装不进 canvas 时，
        //   把 w/h 夹到 canvas 的实际可用宽高，避免模块溢出 canvas 下边界
        //   覆盖底部控制区（toolbar：Hide/Source/FPS/ThemeBar 等）。
        //   · 下限仍由 minW/minH 保证，极端窄窗口下模块可能比 canvas 还小但
        //     不会比模块自身最小尺寸小，这是我们能接受的视觉降级（总好过遮挡）。
        w = juce::jmax (panel->getMinWidth(),  juce::jmin (w, canvas.getWidth()));
        h = juce::jmax (panel->getMinHeight(), juce::jmin (h, canvas.getHeight()));

        int x = placePos.x;
        int y = placePos.y;
        x = juce::jlimit(canvas.getX(), juce::jmax(canvas.getX(), canvas.getRight()  - w), x);
        y = juce::jlimit(canvas.getY(), juce::jmax(canvas.getY(), canvas.getBottom() - h), y);

        panel->setBounds(snapRect({ x, y, w, h }));

        auto* raw = panel.get();
        if (raw->getModuleId().isEmpty())
            raw->setModuleId(juce::String(nextIdCounter++));
        addAndMakeVisible(*raw);
        hookPanel(*raw);
        modules.add(panel.release());
        raw->toFront(false);
        notifyLayoutChanged();
    });
}

// ----------------------------------------------------------
// getDefaultSizeForType —— 按模块类型取它在构造器里通过 setDefaultSize
//   声明的默认尺寸。因为 paintOverChildren / hover 脏区计算等场景只有
//   ModuleType（没有 panel 实例），这里通过 factory 懒造临时 panel
//   读取 getDefaultWidth/Height 并缓存。
//   · factory 未绑定 / 构造失败 → 回退为 ModulePanel 基类默认 320×220
// ----------------------------------------------------------
juce::Point<int> ModuleWorkspace::getDefaultSizeForType (ModuleType t)
{
    const int key = (int) t;

    auto it = hoverPreviewSizeCache.find (key);
    if (it != hoverPreviewSizeCache.end())
        return it->second;

    juce::Point<int> sz { 320, 220 }; // 回退值：与 ModulePanel 基类 default 一致

    if (factory != nullptr)
    {
        if (auto panel = factory (t))
            sz = { panel->getDefaultWidth(), panel->getDefaultHeight() };
    }

    hoverPreviewSizeCache.emplace (key, sz);
    return sz;
}

// ----------------------------------------------------------
// getHoverPreviewImage —— 按需懒构造指定模块类型的"空态快照"
//   · 首次 hover 某 type（或缓存被主题切换清空后）：
//       1) 通过 factory 创建一个临时 ModulePanel
//       2) setBounds 到 panel->getDefaultWidth()×getDefaultHeight()，让 resized()/
//          layoutContent() 把子组件放好
//       3) createComponentSnapshot 递归绘制自身 + 子组件到 Image
//       4) panel 随 unique_ptr 出域销毁（hub.release / removeFrameListener
//          等清理动作已在各模块的析构函数里实现，无长期副作用）
//   · 渲染出的 Image 会被缓存，后续 hover 直接返回
//   · 各模块在无音频输入 / 无历史数据时的 paint 本身就是"空态样式"
//     （比如空的波形区、无数值的仪表），正是我们想要的预览外观
// ----------------------------------------------------------
const juce::Image& ModuleWorkspace::getHoverPreviewImage (ModuleType t)
{
    static const juce::Image emptyImg;

    if (factory == nullptr) return emptyImg;

    const int key = (int) t;

    // 缓存命中
    auto it = hoverPreviewCache.find (key);
    if (it != hoverPreviewCache.end() && it->second.isValid())
        return it->second;

    // 懒构造
    auto panel = factory (t);
    if (panel == nullptr) return emptyImg;

    // 尺寸：优先走每个模块自己的 default（与正式加入 Workspace 时的初始尺寸一致，
    //   保证 hover 预览看到的大小 ≈ 放下后的大小）。
    const int w = panel->getDefaultWidth();
    const int h = panel->getDefaultHeight();
    // 顺手写入 size 缓存（节省下次 getDefaultSizeForType 重新 factory 一次）
    hoverPreviewSizeCache[(int) t] = { w, h };
    // 给它一个临时 id（避免部分模块在 id 未设时的 assert），但不 addAndMakeVisible
    if (panel->getModuleId().isEmpty())
        panel->setModuleId ("preview");

    // ⚠ 关键修复：临时 panel 没有父组件链，Component::getLookAndFeel() 会回退到
    //   JUCE 进程级默认 LookAndFeel（LookAndFeel_V4），于是所有 TextButton 会被
    //   画成浅蓝渐变的默认样式，与实际模块（继承父链拿到的 PinkXPLookAndFeel）不一致。
    //   这里显式给临时 panel 挂上 PinkXPLookAndFeel，并通过 sendLookAndFeelChange()
    //   让它的所有子组件（按钮 / ComboBox / ...）也重新查询并应用该 LookAndFeel。
    panel->setLookAndFeel (&getPinkXPLookAndFeel());
    panel->setBounds (0, 0, w, h);
    panel->sendLookAndFeelChange();
    // 确保 resized/layoutContent 已跑；setBounds 会触发 resized()。

    // 用 createComponentSnapshot 递归绘制（含子组件）到 Image
    auto snapshot = panel->createComponentSnapshot ({ 0, 0, w, h }, /*clipImageToSrc*/ true);

    // 显式解绑 LookAndFeel，防止 panel 析构期间 LookAndFeel 单例被访问的潜在时序问题。
    panel->setLookAndFeel (nullptr);

    auto [ins, ok] = hoverPreviewCache.emplace (key, std::move (snapshot));
    return ins->second;
}

// ----------------------------------------------------------
// 添加模块菜单 hover 预览状态更新
//   由 AddMenuItemComponent::setHighlighted 调用。
//   · active=true  → 画 hoverPreviewType 的占位框（见 paint()）
//   · active=false → 清除预览
//   只对显示状态做 repaint 脏区处理，避免每次 hover 导致全窗口重绘。
// ----------------------------------------------------------
void ModuleWorkspace::setAddMenuHoverPreview (bool active, ModuleType t)
{
    const bool wasActive       = hoverPreviewActive;
    const ModuleType prevType  = hoverPreviewType;

    if (active)
    {
        hoverPreviewActive = true;
        hoverPreviewType   = t;
    }
    else
    {
        // 仅在"要清除的类型正是当前预览类型"时才关闭；避免与下一个 item
        // 的 mouseEnter 竞争（先 Enter 新项 → 再 Exit 旧项，会错误清除预览）。
        if (! hoverPreviewActive || hoverPreviewType != t)
            return;
        hoverPreviewActive = false;
    }

    // 状态无变化：无需重绘
    if (wasActive == hoverPreviewActive && prevType == hoverPreviewType)
        return;

    // 预览框的脏区 = 基于 hoverPreviewPos 的"当前/前一 hover 类型默认尺寸"矩形，
    //   稍加 expand 以防边框阴影被裁
    const auto sz = getDefaultSizeForType (hoverPreviewActive ? hoverPreviewType : prevType);
    auto canvas = getCanvasArea();
    // 同 paintOverChildren / showAddMenu lambda 的"自动缩小"策略，
    //   保证脏区矩形与真正的预览 / 插入尺寸一致，避免 hover 切换时残影。
    const int w = juce::jmin (sz.x, juce::jmax (1, canvas.getWidth()));
    const int h = juce::jmin (sz.y, juce::jmax (1, canvas.getHeight()));
    int x = hoverPreviewPos.x;
    int y = hoverPreviewPos.y;
    x = juce::jlimit (canvas.getX(), juce::jmax (canvas.getX(), canvas.getRight()  - w), x);
    y = juce::jlimit (canvas.getY(), juce::jmax (canvas.getY(), canvas.getBottom() - h), y);
    juce::Rectangle<int> box { x, y, w, h };
    repaint (box.expanded (4));
}

// ----------------------------------------------------------
// 双击 / 右键空白区 → 弹出添加菜单
// ----------------------------------------------------------
void ModuleWorkspace::mouseDown(const juce::MouseEvent& e)
{
    // 仅在画布内的空白处响应（点到子模块上时事件会被子组件吞掉，不会走到这里）
    if (! getCanvasArea().contains(e.getPosition()))
        return;

    // 事件来源区分：workspace 原生 / layer 转发
    //   · layer 转发过来的事件指向图片 / 装饰区域，此时右键不应弹"添加模块"菜单；
    //     直接落到下面的"图片命中 / 装饰命中"分支去聚焦/操作图片即可。
    const bool fromLayer = (e.eventComponent != nullptr && e.eventComponent != this);

    // 1) 右键：优先弹出"添加模块"菜单（保留原有行为；但不响应来自 layer 的右键）
    if (e.mods.isPopupMenu() && ! fromLayer)
    {
        showAddMenu(e.getScreenPosition(), e.getPosition());
        return;
    }

    // 2) 左键：优先检查是否在已聚焦图片的 × 按钮/缩放手柄上
    if (e.mods.isLeftButtonDown())
    {
        if (focusedPerlerIdx >= 0 && focusedPerlerIdx < perlerImages.size())
        {
            auto* fimg = perlerImages.getUnchecked (focusedPerlerIdx);
            const auto fb = fimg->getBounds();

            // 2a) 右上角 × → 删除
            if (getDeleteBtnRect (fb).contains (e.getPosition()))
            {
                deleteBtnPressed = true;
                deleteFocusedPerlerImage();
                return;
            }

            // 2a'') 图片下方 "PerlerBeads" 复选框 → 切换圆环渲染模式
            //   · 整行命中（方框 + 文字均可点）
            //   · 只切 flag + repaint 图片范围 + 复选框行范围，不触发重量化
            if (getPerlerBeadsCheckboxBounds (fb).contains (e.getPosition()))
            {
                fimg->perlerBeadsMode = ! fimg->perlerBeadsMode;
                auto dirty = fb;
                dirty = dirty.getUnion (getPerlerBeadsCheckboxBounds (fb).expanded (2));
                repaint (dirty);
                notifyLayoutChanged();
                return;
            }

            // 2a') 左侧 cellSize 滑块 → 进入 cellSize 拖动态
            //   Bug4：拖动过程中只记录 pendingCellSize，mouseUp 才真正 rebuild
            //   —— rebuild 很重（重量化整张图），拖动时重算会卡
            if (getCellSizeSliderBounds (fb).contains (e.getPosition()))
            {
                cellSizeDraggingIdx = focusedPerlerIdx;
                pendingCellSize     = cellSizeFromSliderY (fb, e.getPosition().y);
                // 仅重绘聚焦装饰范围（滑块 + 单位标签）
                const auto sliderArea = getCellSizeSliderBounds (fb)
                                            .expanded (cellSizeSliderW + 42, 8);
                repaint (sliderArea);
                return;
            }

            // 2a''') 右侧 opacity 滑块 → 进入 opacity 拖动态
            //   · 直接写入 PerlerImage::opacity（无 rebuild 负担），实时反馈
            if (getOpacitySliderBounds (fb).contains (e.getPosition()))
            {
                opacityDraggingIdx = focusedPerlerIdx;
                fimg->opacity = opacityFromSliderY (fb, e.getPosition().y);
                // 重绘：装饰滑条区域 + 图片本体（透明度改变会影响贴画外观）
                const auto sliderArea = getOpacitySliderBounds (fb).expanded (cellSizeSliderW + 4, 8);
                repaint (sliderArea.getUnion (fb));
                if (auto* layer = perlerLayers[focusedPerlerIdx]) layer->repaint();
                return;
            }

            // 2b) 缩放手柄→进入缩放态
            const auto handle = hitTestHandle (fb, e.getPosition());
            if (handle != ResizeHandle::none)
            {
                resizingPerlerIdx   = focusedPerlerIdx;
                activeResizeHandle  = handle;
                resizeStartRect     = fb;
                resizeStartCellsW   = fimg->cellsW;
                resizeStartCellsH   = fimg->cellsH;
                resizePreviewRect   = fb;
                resizePreviewCellsW = fimg->cellsW;
                resizePreviewCellsH = fimg->cellsH;
                // Bug2：新一轮缩放开始，清空累积脏区
                resizeDirtyUnion    = {};

                switch (handle)
                {
                    case ResizeHandle::topLeft:     resizeAnchorPos = fb.getBottomRight(); break;
                    case ResizeHandle::top:         resizeAnchorPos = fb.getBottomLeft();  break;
                    case ResizeHandle::topRight:    resizeAnchorPos = fb.getBottomLeft();  break;
                    case ResizeHandle::left:        resizeAnchorPos = fb.getTopRight();    break;
                    case ResizeHandle::right:       resizeAnchorPos = fb.getTopLeft();     break;
                    case ResizeHandle::bottomLeft:  resizeAnchorPos = fb.getTopRight();    break;
                    case ResizeHandle::bottom:      resizeAnchorPos = fb.getTopLeft();     break;
                    case ResizeHandle::bottomRight: resizeAnchorPos = fb.getTopLeft();     break;
                    default: break;
                }
                return;
            }
        }

        // 2c) 命中图片：进入平移拖动态 + 设为聚焦
        const int idx = hitTestPerlerImageAt (e.getPosition());
        if (idx >= 0)
        {
            draggingPerlerIdx = idx;
            auto* img = perlerImages.getUnchecked (idx);
            perlerDragOffset   = e.getPosition() - img->topLeft;
            perlerDragStartRect = img->getBounds();

            // 提到最前（让最后点击的图片视觉位于顶层）
            //   · 先同步 perlerImages + perlerLayers 数组的顺序（保证索引对应）
            //   · 再调 layer->toFront(true) 把子 Component 冒到 JUCE z-order 最上层
            //     —— 这是"图片与模块平级 z-order，聚焦图片冒到最上"的关键
            if (idx != perlerImages.size() - 1)
            {
                perlerImages.move (idx, perlerImages.size() - 1);
                perlerLayers.move (idx, perlerLayers.size() - 1);
                draggingPerlerIdx = perlerImages.size() - 1;
                repaint (perlerDragStartRect);
            }
            if (auto* focusedLayer = perlerLayers[draggingPerlerIdx])
                focusedLayer->toFront (true);
            // 设为聚焦 + 请求键盘焦点，以监听 Delete
            const int oldFocus = focusedPerlerIdx;
            focusedPerlerIdx = draggingPerlerIdx;
            // 聚焦切换：旧聚焦 layer 收回 bounds，新聚焦 layer 扩大 bounds
            //   · syncPerlerLayerBounds 内部按 focusedPerlerIdx 决定用
            //     decoratedBounds 还是 imgBounds。必须先赋 focusedPerlerIdx 再调用
            if (oldFocus >= 0 && oldFocus != focusedPerlerIdx)
                syncPerlerLayerBounds (oldFocus);
            syncPerlerLayerBounds (focusedPerlerIdx);
            grabKeyboardFocus();
            if (oldFocus != focusedPerlerIdx) repaint();

            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        }
        else
        {
            // 点击空白区 → 取消聚焦
            //   · 但要区分事件来源：若事件从 layer 转发而来（fromLayer），说明
            //     鼠标实际落在装饰外包框的空白区，此时**不应**失焦（误触保护）；
            //     只有真正点到 canvas 空白时才失焦。
            if (! fromLayer && focusedPerlerIdx >= 0)
            {
                const int oldFocus = focusedPerlerIdx;
                focusedPerlerIdx = -1;
                // 旧聚焦 layer 的 bounds 收回到图片本体矩形
                syncPerlerLayerBounds (oldFocus);
                repaint();
            }
        }
    }
}

void ModuleWorkspace::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (! getCanvasArea().contains(e.getPosition()))
        return;
    showAddMenu(e.getScreenPosition(), e.getPosition());
}

// ----------------------------------------------------------
// 绘制 & 布局
// ----------------------------------------------------------
void ModuleWorkspace::paint(juce::Graphics& g)
{
    if (chromeVisible)
    {
        // 工作区背景：凹陷 + 半透明内容色（70% 不透明，让桌面纹理透过来）
        auto canvasOuter = getLocalBounds().withTrimmedBottom(toolbarHeight);
        PinkXP::drawSunken(g, canvasOuter, PinkXP::content.withAlpha(0.70f));

        // 网格点（8px 间距，低透明度淡粉点阵）
        g.setColour(PinkXP::pink200.withAlpha(0.35f));
        auto canvas = getCanvasArea();
        for (int y = canvas.getY(); y < canvas.getBottom(); y += gridSize)
        {
            for (int x = canvas.getX(); x < canvas.getRight(); x += gridSize * 2)
                g.fillRect(x, y, 1, 1);
        }

        // Grid 叠加：按下 toolbar 的 Grid 按钮后，在 canvas 上叠加 8px 对齐网格线。
        //   · 主网格（8px）用较淡的粉线；每 8 条（即 64px）画一条稍深的"主分隔线"，
        //     方便用户在拖动拼豆贴画时做大致的距离估算。
        //   · 网格绘制在 canvas 底图上、拼豆贴画之前，模块与贴画仍会自然覆盖它。
        if (gridOverlayVisible)
        {
            const int major = gridSize * 8; // 64px
            // 次网格：较淡的粉线
            g.setColour (PinkXP::pink300.withAlpha (0.22f));
            for (int x = canvas.getX(); x <= canvas.getRight(); x += gridSize)
                g.fillRect (x, canvas.getY(), 1, canvas.getHeight());
            for (int y = canvas.getY(); y <= canvas.getBottom(); y += gridSize)
                g.fillRect (canvas.getX(), y, canvas.getWidth(), 1);
            // 主网格：每 64px 一条稍深的线
            g.setColour (PinkXP::pink500.withAlpha (0.30f));
            for (int x = canvas.getX(); x <= canvas.getRight(); x += major)
                g.fillRect (x, canvas.getY(), 1, canvas.getHeight());
            for (int y = canvas.getY(); y <= canvas.getBottom(); y += major)
                g.fillRect (canvas.getX(), y, canvas.getWidth(), 1);
        }

        // 底部工具栏：凸起面板（深色主题下也保持浅色底，让文字/色票可读）
        auto tb = getToolbarArea();
        PinkXP::drawRaised(g, tb, PinkXP::btnFace);

        // Toolbar 内的小竖线分隔符（XP 风格：1px 暗色 + 1px 高光色构成凹槽效果）
        //   分隔 FPS 区 / Source 区 / Hide 区
        auto drawDivider = [&](int x)
        {
            if (x < 0) return;
            // 竖线高度略小于 toolbar 高度（上下各留 6px 呼吸空间）
            const int y1 = tb.getY()      + 6;
            const int y2 = tb.getBottom() - 6;
            g.setColour (PinkXP::shdw);
            g.fillRect (x,     y1, 1, y2 - y1);
            g.setColour (PinkXP::hl);
            g.fillRect (x + 1, y1, 1, y2 - y1);
        };
        drawDivider (toolbarDividerX0);
        drawDivider (toolbarDividerX1);
        drawDivider (toolbarDividerX2);
        drawDivider (toolbarDividerXLayout);
    }
    // else: chrome 隐藏 —— 不画任何背景/工具栏，让 Editor 的桌面底图透过来

    // ------------------------------------------------------
    // 拼豆像素画：在工作区背景之后、子组件（模块）之前绘制。
    //   · JUCE 会在 paint() 调用结束后再绘制子组件，所以这里画的图片
    //     会被任何 ModulePanel 子组件完整覆盖（满足"在其他元素之下"）。
    //   · 仅裁剪到 canvas 区域内绘制，避免溢出到 toolbar。
    // ------------------------------------------------------
    // 引导提示：当 chrome 可见、且用户尚未拖入任何图片时，在 canvas 右下角
    // 给出 PNG/JPG/BMP/GIF 拖拽提示。一旦有图或用户点击了 Hide，此提示隐藏。
    if (chromeVisible && perlerImages.isEmpty())
    {
        auto canvas = getCanvasArea();
        // 右下角留 12px 边距；上下两行文案；使用 ink 色弱化透明度，不抢视觉
        const int padding = 12;
        const int lineGap = 2;
        auto fontMain = PinkXP::getFont (11.0f, juce::Font::bold);
        auto fontSub  = PinkXP::getFont (10.0f, juce::Font::plain);

        const juce::String line1 = "Drag an image here to pixelate it"; // →
        const juce::String line2 = "Supported: PNG / JPG / JPEG / BMP / GIF";

        const int h1 = (int) std::ceil (fontMain.getHeight());
        const int h2 = (int) std::ceil (fontSub .getHeight());

        const int totalH = h1 + lineGap + h2;
        const int bottom = canvas.getBottom() - padding;
        const int right  = canvas.getRight()  - padding;

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (canvas);

        g.setColour (PinkXP::ink.withAlpha (0.55f));
        g.setFont   (fontMain);
        g.drawText  (line1,
                     juce::Rectangle<int> (right - 600, bottom - totalH, 600, h1),
                     juce::Justification::centredRight, false);

        g.setColour (PinkXP::ink.withAlpha (0.40f));
        g.setFont   (fontSub);
        g.drawText  (line2,
                     juce::Rectangle<int> (right - 600, bottom - h2, 600, h2),
                     juce::Justification::centredRight, false);
    }

    if (! perlerImages.isEmpty())
    {
        // 图片本体 & 聚焦装饰已迁移：
        //   · 图片本体 → 由 PerlerImageLayer 子组件在各自 paint() 中绘制，
        //                 与 ModulePanel 平级参与 z-order（模块/图片互相遮挡）。
        //   · 聚焦装饰（选中框 / 手柄 / × 删除 / cellSize 滑条 / PerlerBeads
        //                 复选框 / 缩放预览框）→ 迁移到 paintOverChildren()
        //                 （在所有子组件之上绘制，保证聚焦图片的装饰永远
        //                  可见，不被其他模块遮住）。
        //   · 仅保留"聚焦图片冒到最上层"的 z-order 语义见 mouseDown 里
        //     的 perlerLayers[idx]->toFront(true) 调用。
    }

    // Phase E —— 拖拽吸附预览：绘制对齐指示线 + 半透明粉色覆盖
    if (hasDragPreview)
    {
        auto canvas = getCanvasArea();
        g.setColour(PinkXP::pink400.withAlpha(0.75f));
        for (int x : dragPreviewVGuides)
            g.fillRect(x, canvas.getY(), 1, canvas.getHeight());
        for (int y : dragPreviewHGuides)
            g.fillRect(canvas.getX(), y, canvas.getWidth(), 1);

        if (! dragPreview.isEmpty())
        {
            g.setColour(PinkXP::pink300.withAlpha(0.25f));
            g.fillRect(dragPreview);
            g.setColour(PinkXP::pink500.withAlpha(0.75f));
            g.drawRect(dragPreview, 1);
        }
    }
}

// ----------------------------------------------------------
// paintOverChildren —— 在所有模块子组件之上再绘制一层"添加模块"的
//   hover 预览框。这样预览始终显示在最上层，不会被任何已存在的模块遮挡。
// ----------------------------------------------------------
void ModuleWorkspace::paintOverChildren (juce::Graphics& g)
{
    // ------------------------------------------------------
    // 拼豆贴画的聚焦装饰（原在 paint() 里，迁移至此）
    //   · 迁移原因：v2 把图片本体变成 PerlerImageLayer 子组件参与 z-order，
    //     `paint()` 画的东西会被子组件覆盖；聚焦装饰必须画在**所有**子组件
    //     之上才能始终可见。
    //   · 只在 focusedPerlerIdx 有效时绘制；装饰跟随图片的 topLeft 自然移动
    //     （因为 getBounds() / getHandleRect 等都是基于 workspace 坐标系）。
    // ------------------------------------------------------
    if (focusedPerlerIdx >= 0 && focusedPerlerIdx < perlerImages.size())
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (getCanvasArea());

        auto* fimg = perlerImages.getUnchecked (focusedPerlerIdx);
        if (fimg != nullptr)
        {
            const auto fb = fimg->getBounds();
            const float dashes[] = { 4.0f, 3.0f };

            // 选中框：黑实线 + 白虚线叠加（任意背景上都可见）
            {
                juce::Path frame;
                frame.addRectangle (fb.toFloat());
                g.setColour (juce::Colours::black);
                g.strokePath (frame, juce::PathStrokeType (1.0f));
                juce::Path dashed;
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, frame, dashes, 2);
                g.setColour (juce::Colours::white);
                g.strokePath (dashed, juce::PathStrokeType (1.0f));
            }

            // 8 个手柄：白底黑框小方块
            auto drawHandle = [&] (const juce::Rectangle<int>& r)
            {
                g.setColour (juce::Colours::white);
                g.fillRect (r);
                g.setColour (juce::Colours::black);
                g.drawRect (r, 1);
            };
            for (auto h : { ResizeHandle::topLeft, ResizeHandle::top, ResizeHandle::topRight,
                            ResizeHandle::left,    ResizeHandle::right,
                            ResizeHandle::bottomLeft, ResizeHandle::bottom, ResizeHandle::bottomRight })
                drawHandle (getHandleRect (fb, h));

            // 右上角 × 删除按钮（Pink XP 凸起风格）
            {
                const auto xr = getDeleteBtnRect (fb);
                if (deleteBtnPressed)
                    PinkXP::drawPressed (g, xr, PinkXP::pink100);
                else
                    PinkXP::drawRaised (g, xr, deleteBtnHovered ? PinkXP::pink200 : PinkXP::btnFace);

                g.setColour (PinkXP::ink);
                g.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
                auto xrText = xr;
                xrText.translate (-1, -1);
                if (deleteBtnPressed) xrText.translate (1, 1);
                g.drawText ("x", xrText, juce::Justification::centred, false);
            }

            // 左侧 cellSize 滑块
            {
                const auto slider = getCellSizeSliderBounds (fb);
                if (! slider.isEmpty())
                {
                    PinkXP::drawSunken (g, slider, PinkXP::content);

                    const int curCS = juce::jlimit (minCellSize, maxCellSize,
                                                    (pendingCellSize > 0 && cellSizeDraggingIdx == focusedPerlerIdx)
                                                        ? pendingCellSize : fimg->cellSize);
                    const auto thumb = getCellSizeThumbBounds (fb, curCS);
                    PinkXP::drawRaised (g, thumb, (cellSizeDraggingIdx == focusedPerlerIdx)
                                                     ? PinkXP::pink300 : PinkXP::pink200);

                    // 值提示：随主题的 ink 色，浅/深色主题下都可读
                    g.setColour (PinkXP::ink);
                    g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
                    const juce::String label = juce::String (curCS) + "px";
                    const int labelW = 36;
                    const int labelX = slider.getX() - labelW - 2;
                    g.drawText (label,
                                labelX, thumb.getCentreY() - 8,
                                labelW, 16,
                                juce::Justification::centredRight, false);
                }
            }

            // 右侧 opacity 滑块（样式与左侧 cellSize 滑块对称）
            //   · 上 = 不透明(1.0)，下 = 完全透明(0.0)
            //   · 数字标签在拇指右侧（0~100%）
            {
                const auto slider = getOpacitySliderBounds (fb);
                if (! slider.isEmpty())
                {
                    PinkXP::drawSunken (g, slider, PinkXP::content);

                    const float curOp = juce::jlimit (0.0f, 1.0f, fimg->opacity);
                    const auto thumb  = getOpacityThumbBounds (fb, curOp);
                    PinkXP::drawRaised (g, thumb, (opacityDraggingIdx == focusedPerlerIdx)
                                                     ? PinkXP::pink300 : PinkXP::pink200);

                    // 百分比提示在拇指右侧
                    g.setColour (PinkXP::ink);
                    g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
                    const juce::String label = juce::String ((int) std::round (curOp * 100.0f)) + "%";
                    const int labelW = 36;
                    const int labelX = slider.getRight() + 2;
                    g.drawText (label,
                                labelX, thumb.getCentreY() - 8,
                                labelW, 16,
                                juce::Justification::centredLeft, false);
                }
            }

            // 图片下方的 "PerlerBeads" 复选框（凹陷方框 + 勾号 + 右侧文字）
            {
                const auto boxRect = getPerlerBeadsCheckboxBoxRect (fb);
                const auto rowRect = getPerlerBeadsCheckboxBounds   (fb);

                PinkXP::drawSunken (g, boxRect, juce::Colours::white);

                if (fimg->perlerBeadsMode)
                {
                    juce::Path tick;
                    const float bx = (float) boxRect.getX();
                    const float by = (float) boxRect.getY();
                    const float bw = (float) boxRect.getWidth();
                    const float bh = (float) boxRect.getHeight();
                    tick.startNewSubPath (bx + bw * 0.20f, by + bh * 0.55f);
                    tick.lineTo          (bx + bw * 0.45f, by + bh * 0.78f);
                    tick.lineTo          (bx + bw * 0.82f, by + bh * 0.25f);
                    g.setColour (PinkXP::pink500);
                    g.strokePath (tick, juce::PathStrokeType (2.0f,
                                                              juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));
                }

                g.setColour (PinkXP::ink);
                g.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
                const int textX = boxRect.getRight() + 4;
                const int textW = rowRect.getRight() - textX;
                g.drawText ("PerlerBeads",
                            textX, rowRect.getY(),
                            juce::jmax (0, textW), rowRect.getHeight(),
                            juce::Justification::centredLeft, false);
            }

            // 缩放拖动中的预览框（粉色虚线 + 尺寸提示）
            if (resizingPerlerIdx == focusedPerlerIdx && ! resizePreviewRect.isEmpty())
            {
                juce::Path prev;
                prev.addRectangle (resizePreviewRect.toFloat());
                juce::Path dashed;
                juce::PathStrokeType (2.0f).createDashedStroke (dashed, prev, dashes, 2);
                g.setColour (PinkXP::pink500.withAlpha (0.90f));
                g.strokePath (dashed, juce::PathStrokeType (2.0f));

                g.setColour (juce::Colours::black);
                g.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
                const auto tip = juce::String (resizePreviewCellsW)
                                    + " × " + juce::String (resizePreviewCellsH);
                g.drawText (tip,
                            resizePreviewRect.getX(),
                            resizePreviewRect.getBottom() + 2,
                            200, 14,
                            juce::Justification::topLeft, false);
            }
        }
    }

    if (! hoverPreviewActive)
        return;

    // 计算预览框位置 + 尺寸（落点已在 showAddMenu 中记录）
    const auto sz = getDefaultSizeForType (hoverPreviewType);
    auto canvas = getCanvasArea();
    // 自动缩小：窗口过小装不下 default 时，把预览夹到 canvas 尺寸，
    //   与 showMenuAsync 插入 lambda 中的"不越过下方控制区"逻辑保持一致，
    //   用户看到的 ghost 与真实放下来的模块大小完全相符。
    const int w = juce::jmin (sz.x, juce::jmax (1, canvas.getWidth()));
    const int h = juce::jmin (sz.y, juce::jmax (1, canvas.getHeight()));
    int x = hoverPreviewPos.x;
    int y = hoverPreviewPos.y;
    // 以触发位置为左上角，夹紧到 canvas 内
    x = juce::jlimit (canvas.getX(), juce::jmax (canvas.getX(), canvas.getRight()  - w), x);
    y = juce::jlimit (canvas.getY(), juce::jmax (canvas.getY(), canvas.getBottom() - h), y);
    // 与模块添加时一致：snap 到 8px 网格
    auto box = snapRect ({ x, y, w, h });

    // 裁剪到 canvas 区域，避免预览覆盖 toolbar
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (canvas);

    // 优先：用真实模块的空态快照（含标题栏 / 仪表骨架 / 按钮等子组件）
    //   · createComponentSnapshot 已经渲染了子组件，但模块内部还没收到任何
    //     分析帧，所以波形/数值区保持空态 —— 正好是"预览样式"
    const juce::Image& snap = getHoverPreviewImage (hoverPreviewType);
    if (snap.isValid())
    {
        g.setOpacity (0.55f);
        g.drawImage (snap,
                     box.toFloat(),
                     juce::RectanglePlacement::stretchToFit);
        g.setOpacity (1.0f);
    }
    else
    {
        // Fallback：factory 未绑定 / 渲染失败时，退回简易占位样式
        g.setOpacity (0.55f);
        PinkXP::drawRaised (g, box, PinkXP::content);

        const int titleH = 20;
        const juce::Rectangle<int> titleBar { box.getX(), box.getY(), box.getWidth(), titleH };
        PinkXP::drawPinkTitleBar (g, titleBar, getModuleDisplayName (hoverPreviewType), 11.0f);

        const auto body = box.withTrimmedTop (titleH).reduced (1);
        g.setColour (PinkXP::ink.withAlpha (0.35f));
        g.setFont   (PinkXP::getFont (28.0f, juce::Font::bold));
        g.drawText  ("+", body, juce::Justification::centred, false);
        g.setOpacity (1.0f);
    }

    // 虚线外框强调（粉色，带些 alpha），让用户一眼看出这是"待放置"而非"已有模块"
    g.setColour (PinkXP::pink500.withAlpha (0.85f));
    const float dashes[] = { 5.0f, 4.0f };
    juce::Path frame;
    frame.addRectangle (box.toFloat());
    juce::Path dashed;
    juce::PathStrokeType (1.5f).createDashedStroke (dashed, frame, dashes, 2);
    g.strokePath (dashed, juce::PathStrokeType (1.5f));
}

void ModuleWorkspace::resized()
{
    // ---- 按钮尺寸（常量）----
    constexpr int btnW = 52;
    constexpr int btnH = 22;
    constexpr int btnMargin = 6;

    if (chromeVisible)
    {
        // 底部工具栏：从右到左依次放 hideBtn、音频源下拉 (+Label)、FPS 控件，余下给 themeBar
        auto tb = getToolbarArea().reduced(4, 4);

        // 1) 最右侧：Hide 按钮
        auto btnArea = tb.removeFromRight(btnW + 4);
        hideBtn.setBounds(btnArea.withSizeKeepingCentre(btnW, btnH));

        // 2 + 3) Source 区：分隔线 #2 + 音频源下拉 + 前缀标签
        //   · 仅 Standalone 下可见（DAW 插件模式由宿主提供音频，无需选择设备）；
        //     插件模式下由 Editor 调 setAudioSourceUiVisible(false) 关闭整段，
        //     剩余宽度自动让给后续 FPS / 主题栏。
        if (audioSourceUiVisible)
        {
            // 2) 分隔线 #2（Source 区 与 Hide 区 之间）：占 1px 宽 + 两侧各 4px 视觉间距
            tb.removeFromRight (4);
            toolbarDividerX2 = tb.removeFromRight (1).getX();
            tb.removeFromRight (4);

            // 3) 紧邻分隔线 #2 的左侧：音频源下拉 + 前缀标签
            constexpr int sourceBoxW   = 180;
            constexpr int sourceLabelW = 52;
            auto sourceArea = tb.removeFromRight (sourceBoxW);
            audioSourceBox.setBounds (sourceArea.withSizeKeepingCentre (sourceBoxW, btnH));
            auto labelArea = tb.removeFromRight (sourceLabelW);
            audioSourceLabel.setBounds (labelArea.withSizeKeepingCentre (sourceLabelW, btnH));

            audioSourceLabel.setVisible (true);
            audioSourceBox.setVisible (true);
        }
        else
        {
            // 隐藏态：控件不可见 + 分隔线 #2 不画（x<0 时 drawDivider 跳过）
            audioSourceLabel.setVisible (false);
            audioSourceBox.setVisible (false);
            toolbarDividerX2 = -1;
        }

        // 4) 分隔线 #1（FPS 区 与 Source 区 之间）：占 1px 宽 + 两侧各 4px 视觉间距
        tb.removeFromRight (4);
        toolbarDividerX1 = tb.removeFromRight (1).getX();
        tb.removeFromRight (4);

        // 5) 紧邻分隔线 #1 的左侧：FPS 按钮 + FPS 标签（FPS 标签靠近下拉，按钮更靠左）
        //   · FPS 按钮与 Hide 按钮同宽（btnW=52），文字用紧凑格式避免被截断
        constexpr int fpsLabelW = 64;
        auto fpsLblArea = tb.removeFromRight (fpsLabelW);
        fpsLabel.setBounds (fpsLblArea.withSizeKeepingCentre (fpsLabelW, btnH));
        // FPS 按钮单独比通用 btnW 再宽 6px，容纳"30FPS/60FPS"文字不挤；
        //   文字靠 withSizeKeepingCentre 自然居中（JUCE 的 TextButton 默认居中绘制）
        constexpr int fpsBtnW = btnW + 6; // 52 + 6 = 58
        auto fpsBtnArea = tb.removeFromRight (fpsBtnW + 4);
        fpsBtn.setBounds (fpsBtnArea.withSizeKeepingCentre (fpsBtnW, btnH));

        // 5a) 分隔线 #0（Grid 与 FPS 之间）：与其他区域分隔符同样的视觉
        tb.removeFromRight (4);
        toolbarDividerX0 = tb.removeFromRight (1).getX();
        tb.removeFromRight (4);

        // 5b) 分隔线 #0 的左侧：Grid 切换按钮（与 Hide/FPS 同大小）
        auto gridBtnArea = tb.removeFromRight (btnW + 4);
        gridBtn.setBounds (gridBtnArea.withSizeKeepingCentre (btnW, btnH));
        gridBtn.setVisible (true);

        // 5c) 紧邻 Grid 左侧：与 Grid 之间用一条竖直分割线隔开，再放布局预设下拉
        //   · 布局预设下拉仅 Standalone 下显示（插件宿主模式由 Editor 调
        //     setLayoutPresetUiVisible(false) 关闭，因为切换预设涉及
        //     改顶层窗口尺寸/位置，和宿主窗口会打架）。
        //   · Save/Load 两个按钮由独立开关 saveLoadUiVisible 控制：
        //     Standalone 与 VST3 插件模式下都显示（VST3 下虽然没有布局预设下拉，
        //     两按钮仍然独立贴在 Grid 左侧，用于导入导出 .settings 预设文件）。
        const bool layoutVisible   = layoutPresetUiVisible;
        const bool saveLoadVisible = saveLoadUiVisible;

        if (layoutVisible || saveLoadVisible)
        {
            // 分隔线：Grid 与右侧下一块（Save/Load 或布局预设）之间
            tb.removeFromRight (4);
            toolbarDividerXLayout = tb.removeFromRight (1).getX();
            tb.removeFromRight (4);
        }
        else
        {
            toolbarDividerXLayout = -1;
        }

        if (layoutVisible)
        {
            // 布局预设下拉框：与音频源下拉同高。
            //   · 弹出菜单的宽度由 PinkXPLookAndFeel::getIdealPopupMenuItemSize
            //     按实际文字宽度自适应撑开，所以这里 ComboBox 本体保持紧凑即可。
            constexpr int layoutBoxW = 150;
            auto layoutArea = tb.removeFromRight (layoutBoxW);
            layoutPresetBox.setBounds (layoutArea.withSizeKeepingCentre (layoutBoxW, btnH));
            layoutPresetBox.setVisible (true);
        }
        else
        {
            layoutPresetBox.setVisible (false);
        }

        if (saveLoadVisible)
        {
            // Save / Load 两个按钮（与 gridBtn 同宽高），贴在布局预设下拉左侧
            //   若下拉不可见，则两按钮直接贴在 Grid 分隔线左侧。
            tb.removeFromRight (4); // 与上一块之间的视觉间距
            auto saveArea = tb.removeFromRight (btnW);
            savePresetBtn.setBounds (saveArea.withSizeKeepingCentre (btnW, btnH));
            savePresetBtn.setVisible (true);

            tb.removeFromRight (4); // 两按钮之间的间距
            auto loadArea = tb.removeFromRight (btnW);
            loadPresetBtn.setBounds (loadArea.withSizeKeepingCentre (btnW, btnH));
            loadPresetBtn.setVisible (true);

            tb.removeFromRight (6); // 与主题栏之间的视觉间距
        }
        else
        {
            savePresetBtn.setVisible (false);
            loadPresetBtn.setVisible (false);
            // 若下拉可见但 Save/Load 不可见，补一点收尾间距让视觉整齐
            if (layoutVisible)
                tb.removeFromRight (6);
        }

        fpsBtn.setVisible   (true);
        fpsLabel.setVisible (true);

        // 6) 剩余区域：主题栏
        themeBar.setBounds(tb);
        themeBar.setVisible(true);
    }
    else
    {
        // 隐藏态：themeBar / 音频源下拉 / FPS 控件统统藏起；hideBtn 悬浮在右下角
        themeBar.setVisible(false);
        audioSourceLabel.setVisible (false);
        audioSourceBox.setVisible (false);
        fpsBtn.setVisible   (false);
        fpsLabel.setVisible (false);
        gridBtn.setVisible  (false);
        layoutPresetBox.setVisible (false);
        savePresetBtn.setVisible   (false);
        loadPresetBtn.setVisible   (false);
        toolbarDividerX0 = -1;
        toolbarDividerX1 = -1;
        toolbarDividerX2 = -1;
        toolbarDividerXLayout = -1;
        auto r = getLocalBounds();
        hideBtn.setBounds(r.getRight()  - btnW - btnMargin,
                          r.getBottom() - btnH - btnMargin,
                          btnW, btnH);
    }

    // 模块自身保留各自 bounds；但要保证不越界（窗口缩小时裁剪）
    //
    // ⚠ bug1 修复：原先无条件 jlimit 会在"加载布局时 canvas 尚未恢复到用户
    //   保存时的尺寸"场景下，把所有模块挤进左上角。现在只在"真的需要"时夹紧：
    //     · 宽/高大于 canvas 时先把尺寸缩下来（避免 jlimit 的 UB 区间）；
    //     · 只有当模块完全跑到 canvas 右/下侧之外（起点不可见）时才回拉；
    //       部分超出 canvas 的模块保持原坐标不动 —— 等窗口放大后自然可见，
    //       避免"先打开插件 → canvas 暂时小 → 模块被强夹 → 后面窗口放大也不回来"。
    auto canvas = getCanvasArea();
    for (auto* m : modules)
    {
        auto b = m->getBounds();

        // 1) 尺寸超 canvas：缩到 canvas 内（否则接下来 jlimit 会把 upper < lower，挤到原点）
        if (b.getWidth()  > canvas.getWidth())  b.setWidth (canvas.getWidth());
        if (b.getHeight() > canvas.getHeight()) b.setHeight(canvas.getHeight());

        // 2) 位置：仅在"完全离开 canvas 可视区"时回拉；否则保持用户保存的坐标
        //    - 左上超出 canvas（负向）   → 拉回到 canvas 左/上边
        //    - 起点越过 canvas 右/下边缘 → 让起点等于 canvas.right/bottom - size
        if (b.getX() < canvas.getX())            b.setX (canvas.getX());
        if (b.getY() < canvas.getY())            b.setY (canvas.getY());
        if (b.getX() > canvas.getRight()  - 1)   b.setX (canvas.getRight()  - b.getWidth());
        if (b.getY() > canvas.getBottom() - 1)   b.setY (canvas.getBottom() - b.getHeight());

        if (b != m->getBounds())
            m->setBounds(b);
    }

    // ⚠ bug1 三次修复（2026-04-23）：
    //   这里曾经对 perlerImages 做过 clamp（先无条件 jlimit，后改成"完全越界才回拉"），
    //   但两种策略都在"启动瞬间 canvas 尚未恢复到用户上次保存时的尺寸"这一过渡期
    //   会有损地改写 topLeft，导致"把图片放右下角 → 关闭 → 重开 → 图片永久缩到
    //   默认小窗口的右下（看起来是左上方向移动）"这一不可逆 bug。
    //
    //   最终结论：resized() 里**不再**改写任何 perlerImage 的坐标。
    //   其他真正需要 clamp 的路径（addPerlerImageFromFile / mouseDrag /
    //   rebuildPerlerImage* / shiftAllPerlerImagesY 的调用处）都已各自做好
    //   边界保护。resized() 只是一个随窗口尺寸抖动频繁触发的钩子，它不应该
    //   永久改写用户数据——就算图片暂时完全不可见，用户把窗口放大就能重新看到。

    // 按钮始终保持在最上层（避免被模块窗口遮挡）
    hideBtn.toFront(false);
}

// ----------------------------------------------------------
// Chrome（白色底框 + 底部控制区）显隐控制
// ----------------------------------------------------------
void ModuleWorkspace::setChromeVisible(bool shouldBeVisible)
{
    if (chromeVisible == shouldBeVisible) return;
    chromeVisible = shouldBeVisible;

    // 按钮文案 + 空闲透明行为切换
    hideBtn.setButtonText(chromeVisible ? "Hide" : "Show");
    hideBtn.setDimWhenIdle(! chromeVisible);

    resized();
    repaint();

    // 通知外部（Editor 会据此把顶部 TitleBar 和 × 按钮切到半透明/不透明）
    if (onChromeVisibleChanged)
        onChromeVisibleChanged (chromeVisible);
}

// ----------------------------------------------------------
// Hit-test 挖洞：让 Editor 的顶部浮动按钮/标题文字事件能冒泡到父组件
// ----------------------------------------------------------
void ModuleWorkspace::setHitTestHoles (const juce::Array<juce::Rectangle<int>>& holes)
{
    hitTestHoles = holes;
}

bool ModuleWorkspace::hitTest (int x, int y)
{
    // 关键：JUCE 的 getComponentAt 会先对当前组件做 hitTest，
    //   若返回 false 则直接返回 nullptr，根本不会继续下钻子组件！
    //   因此这里必须在返回 false 之前，先确认该坐标点上没有任何可见的模块子组件覆盖
    //   —— 如果有模块覆盖，则返回 true，JUCE 会继续向下分发事件给模块，
    //       从而满足"模块遮挡浮层时独占鼠标"的需求；
    //   —— 如果确实是空白区且落在挖洞矩形内，再返回 false 让事件冒泡给父组件（Editor）。
    const juce::Point<int> p { x, y };

    bool insideHole = false;
    for (const auto& h : hitTestHoles)
    {
        if (h.contains (p))
        {
            insideHole = true;
            break;
        }
    }

    if (! insideHole)
        return true;

    // 在挖洞区域内：检查是否有任何可见 ModulePanel 覆盖该点。
    //   倒序遍历（与 getComponentAt 的 z-order 顺序一致：数组尾部为最前 child）。
    for (int i = modules.size(); --i >= 0;)
    {
        if (auto* m = modules.getUnchecked (i))
        {
            if (m->isVisible() && m->getBounds().contains (p))
                return true; // 有模块覆盖 → 让 JUCE 继续下钻，事件会被模块独占
        }
    }

    // 挖洞区且无模块覆盖 → 事件冒泡给 Editor
    return false;
}

// ==========================================================
// 音频信号来源下拉框 —— 外部填充 / 查询接口
// ==========================================================
void ModuleWorkspace::setAudioSourceItems (const juce::Array<AudioSourceItem>& items,
                                           const juce::String& selectedSourceId)
{
    audioSourceItems = items;
    audioSourceBox.clear (juce::dontSendNotification);

    int selectIdx = -1;
    for (int i = 0; i < audioSourceItems.size(); ++i)
    {
        const auto& it = audioSourceItems.getReference (i);
        audioSourceBox.addItem (it.displayName, i + 1);
        if (it.sourceId == selectedSourceId)
            selectIdx = i;
    }

    // 若外部指定了 selectedSourceId 则按它选；否则默认选第 0 项（不触发回调，避免启动抖动）
    if (selectIdx < 0 && ! audioSourceItems.isEmpty())
        selectIdx = 0;

    if (selectIdx >= 0)
        audioSourceBox.setSelectedId (selectIdx + 1, juce::dontSendNotification);
}

juce::String ModuleWorkspace::getSelectedAudioSourceId() const
{
    const int idx = audioSourceBox.getSelectedItemIndex();
    if (idx < 0 || idx >= audioSourceItems.size()) return {};
    return audioSourceItems.getReference (idx).sourceId;
}

void ModuleWorkspace::setAudioSourceUiVisible (bool shouldBeVisible)
{
    if (audioSourceUiVisible == shouldBeVisible) return;
    audioSourceUiVisible = shouldBeVisible;

    // 立刻同步控件可见性（resized 里还会再做一次，这里是"显式显隐"的即时反馈）
    if (! shouldBeVisible)
    {
        audioSourceBox.setVisible (false);
        audioSourceLabel.setVisible (false);
        // 让分隔线 #2 自动不画（drawDivider 会在 x<0 时跳过）
        toolbarDividerX2 = -1;
    }

    // 触发 toolbar 重新分配剩余空间（剩下的给 themeBar）
    resized();
    repaint();
}

void ModuleWorkspace::setLayoutPresetUiVisible (bool shouldBeVisible)
{
    if (layoutPresetUiVisible == shouldBeVisible) return;
    layoutPresetUiVisible = shouldBeVisible;

    if (! shouldBeVisible)
    {
        layoutPresetBox.setVisible (false);
        // 注意：Save/Load 按钮的显隐由独立开关 saveLoadUiVisible 决定，
        //       这里不再一起强制隐藏 —— VST3 插件模式下允许"隐藏下拉但保留 Save/Load"。
        // 若 saveLoadUiVisible 也是 false，resized() 分支会负责真正隐藏两按钮。
        // 让分隔线 Layout 自动不画（drawDivider 会在 x<0 时跳过）
        if (! saveLoadUiVisible)
            toolbarDividerXLayout = -1;
    }

    resized();
    repaint();
}

void ModuleWorkspace::setSaveLoadUiVisible (bool shouldBeVisible)
{
    if (saveLoadUiVisible == shouldBeVisible) return;
    saveLoadUiVisible = shouldBeVisible;

    if (! shouldBeVisible)
    {
        savePresetBtn.setVisible (false);
        loadPresetBtn.setVisible (false);
        // 若布局预设下拉也是隐藏态，此时分隔线也不需要
        if (! layoutPresetUiVisible)
            toolbarDividerXLayout = -1;
    }

    resized();
    repaint();
}

// ==========================================================
// FPS 限制按钮 / 实时 FPS 标签 —— 对外 API
// ==========================================================
void ModuleWorkspace::setFpsLimit (int hz)
{
    // 仅允许 30 / 60 两档（其他值归一到离得近的那个，避免越权设置）
    const int clamped = (hz >= 45 ? 60 : 30);
    if (fpsLimit == clamped) return;
    fpsLimit = clamped;
    fpsBtn.setButtonText (juce::String (fpsLimit) + "FPS");
}

void ModuleWorkspace::setMeasuredFps (float fps)
{
    // 显示保留 1 位小数，如 "29.8 fps"；未启动时 fps<=0 显示占位 "-- fps"
    if (fps <= 0.0f)
        fpsLabel.setText ("-- fps", juce::dontSendNotification);
    else
        fpsLabel.setText (juce::String (fps, 1) + " fps", juce::dontSendNotification);
}

// ==========================================================
// Phase E —— 吸附预览
// ==========================================================
void ModuleWorkspace::repaintDragPreviewPieces(juce::Rectangle<int> preview,
                                               const juce::Array<int>& vGuides,
                                               const juce::Array<int>& hGuides)
{
    const auto canvas = getCanvasArea();

    for (int x : vGuides)
        repaint(juce::Rectangle<int>(x, canvas.getY(), 1, canvas.getHeight()));

    for (int y : hGuides)
        repaint(juce::Rectangle<int>(canvas.getX(), y, canvas.getWidth(), 1));

    if (! preview.isEmpty())
        repaint(preview.expanded(2));
}

void ModuleWorkspace::updateDragPreview(ModulePanel& movingPanel)
{
    const auto oldPreview = dragPreview;
    juce::Array<int> oldVGuides(dragPreviewVGuides);
    juce::Array<int> oldHGuides(dragPreviewHGuides);
    const bool hadOldPreview = hasDragPreview;

    auto r = snapRect(movingPanel.getBounds());

    dragPreviewVGuides.clearQuick();
    dragPreviewHGuides.clearQuick();
    r = snapToNeighbours(r, &movingPanel, dragPreviewVGuides, dragPreviewHGuides);

    // 裁剪到工作区
    auto canvas = getCanvasArea();
    r.setX(juce::jlimit(canvas.getX(), juce::jmax(canvas.getX(),
            canvas.getRight()  - r.getWidth()),  r.getX()));
    r.setY(juce::jlimit(canvas.getY(), juce::jmax(canvas.getY(),
            canvas.getBottom() - r.getHeight()), r.getY()));

    dragPreview    = r;
    hasDragPreview = true;

    if (hadOldPreview)
        repaintDragPreviewPieces(oldPreview, oldVGuides, oldHGuides);

    repaintDragPreviewPieces(dragPreview, dragPreviewVGuides, dragPreviewHGuides);
}

void ModuleWorkspace::clearDragPreview()
{
    if (! hasDragPreview) return;

    const auto oldPreview = dragPreview;
    juce::Array<int> oldVGuides(dragPreviewVGuides);
    juce::Array<int> oldHGuides(dragPreviewHGuides);

    hasDragPreview = false;
    dragPreview = {};
    dragPreviewVGuides.clearQuick();
    dragPreviewHGuides.clearQuick();

    repaintDragPreviewPieces(oldPreview, oldVGuides, oldHGuides);
}

void ModuleWorkspace::notifyLayoutChanged()
{
    if (suspendNotifications) return;
    if (onLayoutChanged) onLayoutChanged();
}

// ==========================================================
// Phase E —— 布局持久化（ValueTree）
//
// <PBEQ_Layout>
//   <Module type="eq"         id="1" x="0" y="0" w="320" h="220" />
//   <Module type="loudness"   id="2" x="..." .../>
//   ...
// </PBEQ_Layout>
// ==========================================================
static const juce::Identifier kLayoutRoot  ("PBEQ_Layout");
static const juce::Identifier kLayoutModule ("Module");
static const juce::Identifier kLayoutPerler ("Perler");
static const juce::Identifier kPropType ("type");
static const juce::Identifier kPropId   ("id");
static const juce::Identifier kPropX    ("x");
static const juce::Identifier kPropY    ("y");
static const juce::Identifier kPropW    ("w");
static const juce::Identifier kPropH    ("h");
static const juce::Identifier kPropPath     ("path");
static const juce::Identifier kPropCellsW   ("cellsW");
static const juce::Identifier kPropCellsH   ("cellsH");
static const juce::Identifier kPropCellSize ("cellSize");
static const juce::Identifier kPropPerlerBeads ("perlerBeads");
static const juce::Identifier kPropOpacity  ("perlerOpacity");

juce::ValueTree ModuleWorkspace::saveLayoutTree() const
{
    juce::ValueTree tree(kLayoutRoot);
    // 全局开关：网格叠加层可见性（toolbar 上 Grid 按钮）
    tree.setProperty ("gridVisible", gridOverlayVisible, nullptr);

    // ==============================================================
    // 按 JUCE 真实 z-order 混合写入（模块 + 拼豆贴画）
    //   · getChildren() 返回顺序：index 0 = 最底层，index N-1 = 最顶层。
    //     ModulePanel::mouseDown 会 toFront(true) 把自己移到 child 列表末尾；
    //     PerlerImageLayer 被点击聚焦时也会 toFront(true)。因此 getChildren()
    //     的顺序就是当前屏幕上真实可见的叠放顺序。
    //   · 遍历时同时识别 ModulePanel 和 PerlerImageLayer，把对应的 <Module>
    //     / <Perler> 节点按 z-order 追加 —— 加载时按此顺序重建，依次
    //     addAndMakeVisible（每次新加入的组件自然在最前），于是恢复出的
    //     child 序列和保存时完全一致，层级关系（某张图片压在某模块下）得以保留。
    //   · 其他 chrome 子组件（hideBtn / layoutPresetBox / themeBar / fpsBtn 等）
    //     在此跳过，因为它们不参与持久化。
    // ==============================================================
    for (auto* child : getChildren())
    {
        if (child == nullptr) continue;

        // 模块节点
        if (auto* m = dynamic_cast<ModulePanel*> (child))
        {
            juce::ValueTree node(kLayoutModule);
            node.setProperty(kPropType, moduleTypeToString(m->getModuleType()), nullptr);
            node.setProperty(kPropId,   m->getModuleId(),                       nullptr);
            const auto b = m->getBounds();
            node.setProperty(kPropX, b.getX(),      nullptr);
            node.setProperty(kPropY, b.getY(),      nullptr);
            node.setProperty(kPropW, b.getWidth(),  nullptr);
            node.setProperty(kPropH, b.getHeight(), nullptr);
            tree.appendChild(node, nullptr);
            continue;
        }

        // 拼豆贴画节点：通过 layer 反查 perlerImages 索引
        if (auto* layer = dynamic_cast<PerlerImageLayer*> (child))
        {
            const int idx = perlerLayers.indexOf (layer);
            if (idx < 0) continue;
            auto* pimg = perlerImages.getUnchecked (idx);
            if (pimg == nullptr || pimg->sourcePath.isEmpty()) continue;

            juce::ValueTree node (kLayoutPerler);
            node.setProperty (kPropPath,     pimg->sourcePath,   nullptr);
            node.setProperty (kPropX,        pimg->topLeft.x,    nullptr);
            node.setProperty (kPropY,        pimg->topLeft.y,    nullptr);
            node.setProperty (kPropCellsW,   pimg->cellsW,       nullptr);
            node.setProperty (kPropCellsH,   pimg->cellsH,       nullptr);
            node.setProperty (kPropCellSize, pimg->cellSize,     nullptr);
            if (pimg->perlerBeadsMode)
                node.setProperty (kPropPerlerBeads, true, nullptr);
            if (std::abs (pimg->opacity - 1.0f) > 1.0e-4f)
                node.setProperty (kPropOpacity, (double) pimg->opacity, nullptr);
            tree.appendChild (node, nullptr);
            continue;
        }
        // 其他 chrome 子组件（按钮 / 下拉 / themeBar / 浮层等）不持久化
    }

    return tree;
}

bool ModuleWorkspace::loadLayoutFromTree(const juce::ValueTree& tree)
{
    if (! tree.hasType(kLayoutRoot))
        return false;
    if (factory == nullptr)
    {
        jassertfalse;
        return false;
    }

    // 批量操作期间静默 onLayoutChanged，避免反复回写 Processor
    const juce::ScopedValueSetter<bool> guard(suspendNotifications, true);

    // 恢复全局：网格叠加层可见性（若属性不存在则保持默认 false）
    if (tree.hasProperty ("gridVisible"))
    {
        gridOverlayVisible = (bool) tree.getProperty ("gridVisible");
        gridBtn.setToggleState (gridOverlayVisible, juce::dontSendNotification);
    }

    // 清空现有模块 + 拼豆贴画
    for (int i = modules.size(); --i >= 0;)
        modules.remove(i);
    // 同步清理拼豆贴画：先清 layer（子 Component）再清数据结构
    perlerLayers.clear();
    perlerImages.clear();
    focusedPerlerIdx   = -1;
    draggingPerlerIdx  = -1;
    resizingPerlerIdx  = -1;
    activeResizeHandle = ResizeHandle::none;
    repaint();

    int maxId = 0;
    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        const auto node = tree.getChild(i);

        // 拼豆贴画节点：路径仍在则重建
        if (node.hasType (kLayoutPerler))
        {
            const auto path = node.getProperty (kPropPath).toString();
            if (path.isEmpty()) continue;
            const juce::File f (path);
            if (! f.existsAsFile())
                continue; // 源文件已删除/搜移——静默跳过

            const int  cw  = (int) node.getProperty (kPropCellsW,   0);
            const int  ch  = (int) node.getProperty (kPropCellsH,   0);
            // cellSize：旧版存档可能为 0 或 10；为 0 时走默认（4）
            const int  cs  = (int) node.getProperty (kPropCellSize, 0);
            const int  px  = (int) node.getProperty (kPropX,        0);
            const int  py  = (int) node.getProperty (kPropY,        0);
            const bool beads = (bool) node.getProperty (kPropPerlerBeads, false);
            // opacity：旧存档无此属性时默认 1.0 = 完全不透明
            const float op   = (float) (double) node.getProperty (kPropOpacity, 1.0);
            const juce::Point<int> tl { px, py };
            addPerlerImageFromFile (f, { 0, 0 }, cw, ch, &tl, cs);
            // 恢复 PerlerBeads 渲染模式 + opacity：addPerlerImageFromFile 内部会 append 到末尾
            if (perlerImages.size() > 0)
            {
                auto* last = perlerImages.getLast();
                if (beads) last->perlerBeadsMode = true;
                last->opacity = juce::jlimit (0.0f, 1.0f, op);
            }
            continue;
        }

        if (! node.hasType(kLayoutModule)) continue;

        bool ok = false;
        const auto typeStr = node.getProperty(kPropType).toString();
        const auto type    = stringToModuleType(typeStr, &ok);
        if (! ok) continue;

        auto panel = factory(type);
        if (panel == nullptr) continue;

        const auto idStr = node.getProperty(kPropId).toString();
        if (idStr.isNotEmpty())
        {
            panel->setModuleId(idStr);
            maxId = juce::jmax(maxId, idStr.getIntValue());
        }

        const int x = (int) node.getProperty(kPropX, 0);
        const int y = (int) node.getProperty(kPropY, 0);
    const int w = juce::jmax(panel->getMinWidth(),  (int) node.getProperty(kPropW, panel->getDefaultWidth()));
        const int h = juce::jmax(panel->getMinHeight(), (int) node.getProperty(kPropH, panel->getDefaultHeight()));

        auto* raw = panel.get();
        addAndMakeVisible(*raw);
        hookPanel(*raw);
        raw->setBounds({ x, y, w, h });

        modules.add(panel.release());
    }

    nextIdCounter = juce::jmax(nextIdCounter, maxId + 1);

    // ⚠ bug1 修复：这里原本调用 resized() 做一次"整体夹紧"。但在插件首次恢复
    //   布局时，顶层窗口还没被 restoreMainWindowBounds() 放大到用户保存的尺寸，
    //   canvas 只是 Editor 构造阶段的默认 960×640。这会把所有在大 canvas 里
    //   摆放的模块强制 jlimit 进左上很小的区域，而且坐标一旦被改就回不去。
    //
    //   修复：加载完成后只做 repaint；夹紧留给后续真正的 resized() 回调
    //   （此时模块自己的 bounds 已是原始保存值，canvas 也已经是最终尺寸）。
    //   并且新版 resized() 的夹紧已改为"仅越界时回拉"，不会再误伤用户布局。
    repaint();
    return true;
}

juce::String ModuleWorkspace::saveLayoutAsXml() const
{
    const auto tree = saveLayoutTree();
    if (auto xml = tree.createXml())
        return xml->toString(juce::XmlElement::TextFormat{}.singleLine());
    return {};
}

bool ModuleWorkspace::loadLayoutFromXml(const juce::String& xmlString)
{
    if (xmlString.isEmpty()) return false;
    if (auto xml = juce::parseXML(xmlString))
    {
        const auto tree = juce::ValueTree::fromXml(*xml);
        return loadLayoutFromTree(tree);
    }
    return false;
}

// ==========================================================
// ThemeSwatchBar —— XP 画图底部调色板样式的主题选择器
// ==========================================================

namespace
{
    constexpr int kPreviewW   = 30;   // 左侧预览方块宽
    constexpr int kSwatchW    = 18;   // 单个主题色票宽
    constexpr int kSwatchH    = 18;   // 单个主题色票高
    constexpr int kSwatchGap  = 3;    // 色票间距
    constexpr int kGroupGap   = 10;   // 预览与色票组之间的间距
    constexpr int kBarLeftPad = 6;    // 整体左内边距
}

ThemeSwatchBar::ThemeSwatchBar()
{
    setInterceptsMouseClicks(true, false);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

ThemeSwatchBar::~ThemeSwatchBar() = default;

juce::Rectangle<int> ThemeSwatchBar::getPreviewBounds() const
{
    // 预览方块统一使用正方形：边长 = min(getHeight()-6, kPreviewW)
    // 旧版本宽=kPreviewW(30) 而高 = min(getHeight()-6, 30)，
    //   在 toolbar 只有 22-24px 高时出现"宽 30 × 高 18"的长方形。
    const int side = juce::jmin(getHeight() - 6, kPreviewW);
    const int y = (getHeight() - side) / 2;
    return { kBarLeftPad, y, side, side };
}

juce::Rectangle<int> ThemeSwatchBar::getSwatchBounds(int index) const
{
    const auto prev = getPreviewBounds();
    const int startX = prev.getRight() + kGroupGap;
    const int y = (getHeight() - kSwatchH) / 2;
    return { startX + index * (kSwatchW + kSwatchGap), y, kSwatchW, kSwatchH };
}

int ThemeSwatchBar::hitTestSwatch(juce::Point<int> p) const
{
    const auto& themes = PinkXP::getAllThemes();
    for (int i = 0; i < (int) themes.size(); ++i)
        if (getSwatchBounds(i).contains(p))
            return i;
    return -1;
}

void ThemeSwatchBar::paint(juce::Graphics& g)
{
    // 左侧预览方块（复刻 XP 画图的前景/背景色预览块：双色对角斜分）
    {
        const auto prev = getPreviewBounds();
        const auto& th  = PinkXP::getCurrentTheme();
        PinkXP::drawSunken(g, prev, juce::Colours::white);
        auto inner = prev.reduced(3);

        // 底色 = 主色（swatch）
        g.setColour(th.swatch);
        g.fillRect(inner);

        // 左上三角 = hl（高光/珍珠色），斜分
        juce::Path tri;
        tri.startNewSubPath((float) inner.getX(),     (float) inner.getY());
        tri.lineTo        ((float) inner.getRight(),  (float) inner.getY());
        tri.lineTo        ((float) inner.getX(),     (float) inner.getBottom());
        tri.closeSubPath();
        g.setColour(th.hl);
        g.fillPath(tri);
    }

    // 右侧色票
    const auto& themes = PinkXP::getAllThemes();
    for (int i = 0; i < (int) themes.size(); ++i)
    {
        const auto r = getSwatchBounds(i);
        if (r.getRight() > getWidth()) break;

        const auto& th = themes[(size_t) i];
        const bool isCurrent = (th.id == PinkXP::getCurrentThemeId());
        const bool isHover   = (i == hoverIndex);

        // 色票底：用 drawSunken 的外 2px 浅高光 + 深阴影，模拟 XP 色块凹槽
        g.setColour(PinkXP::dark);
        g.fillRect(r.getX(), r.getY(), r.getWidth(), 1);
        g.fillRect(r.getX(), r.getY(), 1, r.getHeight());
        g.setColour(PinkXP::hl);
        g.fillRect(r.getX() + 1, r.getBottom() - 1, r.getWidth() - 1, 1);
        g.fillRect(r.getRight() - 1, r.getY() + 1, 1, r.getHeight() - 1);

        // 色票主体
        g.setColour(th.swatch);
        g.fillRect(r.reduced(1));

        // 选中态：外围 2px 反色框 + 打勾
        if (isCurrent)
        {
            auto sel = r.expanded(2);
            g.setColour(PinkXP::ink);
            g.drawRect(sel, 2);

            // 小打勾像素点（右下角 3x3 像素 check）
            const int cx = r.getRight() - 4;
            const int cy = r.getBottom() - 4;
            g.setColour(juce::Colours::white);
            g.fillRect(cx,     cy - 1, 1, 2);
            g.fillRect(cx - 1, cy,     1, 2);
            g.fillRect(cx - 2, cy - 1, 1, 2);
        }
        else if (isHover)
        {
            g.setColour(PinkXP::ink.withAlpha(0.55f));
            g.drawRect(r.expanded(1), 1);
        }
    }

    // 右侧显示当前主题名（Y2K 小标签感）
    {
        const auto& th = PinkXP::getCurrentTheme();
        const int lastSwatchRight = (int) themes.size() > 0
            ? getSwatchBounds((int) themes.size() - 1).getRight()
            : getPreviewBounds().getRight();
        const int tx = lastSwatchRight + kGroupGap;
        const int tw = juce::jmax(0, getWidth() - tx - 4);
        if (tw > 10)
        {
            g.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
            g.setColour(PinkXP::ink);
            g.drawText(juce::String(th.displayName),
                       tx, 0, tw, getHeight(),
                       juce::Justification::centredLeft, true);
        }
    }
}

void ThemeSwatchBar::resized() {}

void ThemeSwatchBar::mouseMove(const juce::MouseEvent& e)
{
    const int idx = hitTestSwatch(e.getPosition());
    if (idx != hoverIndex)
    {
        hoverIndex = idx;
        if (idx >= 0)
        {
            const auto& th = PinkXP::getAllThemes()[(size_t) idx];
            setTooltip(juce::String(th.displayName) + "  --  " + juce::String(th.keywordHint));
        }
        else
        {
            setTooltip({});
        }
        repaint();
    }
}

void ThemeSwatchBar::mouseExit(const juce::MouseEvent&)
{
    if (hoverIndex != -1) { hoverIndex = -1; repaint(); }
}

void ThemeSwatchBar::mouseDown(const juce::MouseEvent& e)
{
    const int idx = hitTestSwatch(e.getPosition());
    if (idx >= 0)
    {
        const auto& themes = PinkXP::getAllThemes();
        PinkXP::applyTheme(themes[(size_t) idx].id);
        // 回调里一般会 repaint 顶层；此处也自己 repaint 一下以同步
        if (auto* top = getTopLevelComponent()) top->repaint();
        else                                    repaint();
    }
}

// ==========================================================
// HideChromeButton —— 右下角的隐藏/显示按钮
// ==========================================================
namespace
{
    constexpr float kHideBtnDimAlpha    = 0.35f;  // 隐藏态空闲透明度
    constexpr float kHideBtnActiveAlpha = 1.0f;   // 悬停/显示态透明度
}

HideChromeButton::HideChromeButton()
    : juce::Button("HideChromeButton")
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus(false);
    // 初始：不 dim（显示态）
    setAlpha(kHideBtnActiveAlpha);
}

void HideChromeButton::setDimWhenIdle(bool shouldDim)
{
    dimWhenIdle = shouldDim;
    updateAlpha();
    repaint();
}

void HideChromeButton::mouseEnter(const juce::MouseEvent& e)
{
    juce::Button::mouseEnter(e);
    updateAlpha();
    repaint();
}

void HideChromeButton::mouseExit(const juce::MouseEvent& e)
{
    juce::Button::mouseExit(e);
    updateAlpha();
    repaint();
}

void HideChromeButton::updateAlpha()
{
    // dimWhenIdle=false：始终不透明
    // dimWhenIdle=true ：鼠标悬停/按下时不透明；否则半透明
    if (! dimWhenIdle)
    {
        setAlpha(kHideBtnActiveAlpha);
        return;
    }

    const bool active = isMouseOver(true) || isDown();
    setAlpha(active ? kHideBtnActiveAlpha : kHideBtnDimAlpha);
}

void HideChromeButton::paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
    auto r = getLocalBounds();

    // Pink XP 风小按钮：凸起 / 按下凹陷（用 btnFace 保证深色主题下也可读）
    if (isButtonDown)
        PinkXP::drawPressed(g, r, PinkXP::btnFace);
    else
        PinkXP::drawRaised(g, r, isMouseOver ? PinkXP::pink100 : PinkXP::btnFace);

    // 按钮文字
    g.setFont(PinkXP::getFont(10.0f, juce::Font::bold));
    g.setColour(PinkXP::ink);
    auto textArea = r.reduced(4, 2);
    if (isButtonDown) textArea.translate(1, 1);
    g.drawText(getButtonText(), textArea,
               juce::Justification::centred, false);
}

// ==========================================================
// 拼豆像素画（PerlerImage）—— 拖入图片 / 像素化 / 颜色量化 / 拖动
// ==========================================================
namespace
{
    // --------------------------------------------------
    // sRGB <-> Linear <-> CIE Lab（用于"感知一致"的颜色最近邻匹配）
    // --------------------------------------------------
    struct Lab { float L, a, b; };

    inline float srgbToLinear (float c)
    {
        c = juce::jlimit (0.0f, 1.0f, c);
        return (c <= 0.04045f) ? (c / 12.92f)
                               : std::pow ((c + 0.055f) / 1.055f, 2.4f);
    }

    inline Lab rgbToLab (juce::Colour col)
    {
        const float r = srgbToLinear (col.getFloatRed());
        const float g = srgbToLinear (col.getFloatGreen());
        const float b = srgbToLinear (col.getFloatBlue());

        // sRGB -> XYZ (D65)
        const float X = r * 0.4124564f + g * 0.3575761f + b * 0.1804375f;
        const float Y = r * 0.2126729f + g * 0.7151522f + b * 0.0721750f;
        const float Z = r * 0.0193339f + g * 0.1191920f + b * 0.9503041f;

        // Normalize by D65 white
        const float xn = X / 0.95047f;
        const float yn = Y / 1.00000f;
        const float zn = Z / 1.08883f;

        auto f = [] (float v)
        {
            return (v > 0.008856f) ? std::cbrt (v) : (7.787f * v + 16.0f / 116.0f);
        };

        const float fx = f (xn);
        const float fy = f (yn);
        const float fz = f (zn);

        Lab out;
        out.L = 116.0f * fy - 16.0f;
        out.a = 500.0f * (fx - fy);
        out.b = 200.0f * (fy - fz);
        return out;
    }

    inline float labDistSq (const Lab& x, const Lab& y)
    {
        const float dL = x.L - y.L;
        const float da = x.a - y.a;
        const float db = x.b - y.b;
        return dL * dL + da * da + db * db;
    }
} // namespace

// 内嵌 144 色拼豆色表（与 G:/PerlerBeadsEQ/color.txt 中的十六进制一一对应）
const juce::Array<juce::Colour>& ModuleWorkspace::getPerlerPalette()
{
    static const juce::Array<juce::Colour> palette = []
    {
        static const char* const hexCodes[] = {
            // 白/灰
            "FFFFFF","F0F0F0","FFFDD0","FFFFF0","D3D3D3","A9A9A9","808080","555555",
            "333333","000000","C0C0C0","708090",
            // 棕/米
            "F5F5DC","F5DEB3","C3B091","D2B48C","C2B280","C19A6B","A0522D","7B3F00",
            "5C4033","3E2723","CC7722","D2691E",
            // 红
            "FF0000","ED1B24","8B0000","DE3163","E30B5C","722F37","CD5C5C","FF007F",
            "FF6666","FF4D40","A52A2A","B22222",
            // 橙/珊瑚
            "F26522","FF7518","FF3B30","FFC324","FBBF77","F88379","FA8072","FFCBA4",
            "F28500","FA5F3C","ED9121","FFAE42",
            // 黄/奶油
            "FFF44F","FFFF00","FFD700","FFFFE0","FFFACD","F7E7CE","FFFDD0","F0C24B",
            "E1AD01","FFD700","FFE135","FFF1B5",
            // 绿
            "00A651","50C878","228B22","8DB600","7CFC00","6B8E23","98FB98","90EE90",
            "006400","004B23","008080","32CD32",
            // 蓝绿/青
            "00FFFF","008B8B","B0E0E6","48D1CC","0ABAB5","AFEEEE","006D6D","4FE0B0",
            "4C9F9F","B2DFEE","70DBDB","D0F0F0",
            // 蓝
            "87CEEB","ADD8E6","4169E1","000080","4B0082","1E3A8A","0047AB","00008B",
            "0000CD","007FFF","003153","6699CC",
            // 紫/紫罗兰
            "800080","8F00FF","E6E6FA","D8BFD8","C9A0DC","C8A2C8","4B0082","DDA0DD",
            "8B00FF","C154C1","6F2DA8","CCCCFF",
            // 粉
            "FFC0CB","FFB6C1","FF69B4","FF1493","FF66CC","FF007F","FADADD","F88379",
            "FC5A8D","FFB7C5","FFB3DE","F4C2C2",
            // 金属/闪光/透明
            "FFD700","DAA520","C0C0C0","B87333","CD7F32","434B4D","E8E8E8","F8F8F8",
            "F0EAD6","FCF5E5","D8D8D8","FFDF00",
            // 荧光/霓虹
            "CCFF00","39FF14","FF5E00","FF1493","00FFFF","BF00FF","FF0038","CCFF00",
            "00FF7F","FFAE42","FF6EB4","FF00FF"
        };

        juce::Array<juce::Colour> arr;
        arr.ensureStorageAllocated ((int) (sizeof (hexCodes) / sizeof (hexCodes[0])));
        for (const char* hex : hexCodes)
        {
            const auto hexStr = juce::String ("FF") + juce::String (hex); // FF = 不透明
            const auto argb   = (juce::uint32) hexStr.getHexValue32();
            arr.add (juce::Colour (argb));
        }
        return arr;
    }();
    return palette;
}

// ----------------------------------------------------------
// 把原图降采样到 (cellsW × cellsH) 格，每格取平均色并量化到 144 色调色板。
//   · maxCellsOnLongSide：限制长边最多多少个 cell，避免一张大图生成
//     几百万像素的贴画（例如 1080 / 10 = 108 格；这里给默认 80 就够用）。
//   · cellSize：最终每个格子在屏幕上占多少像素（默认 10）。
// ----------------------------------------------------------
juce::Image ModuleWorkspace::buildPerlerImage (const juce::Image& source,
                                               int cellSize,
                                               int maxCellsOnLongSide,
                                               int& outCellsW,
                                               int& outCellsH)
{
    outCellsW = outCellsH = 0;
    if (source.isNull() || cellSize <= 0) return {};

    const int srcW = source.getWidth();
    const int srcH = source.getHeight();
    if (srcW <= 0 || srcH <= 0) return {};

    // 决定网格尺寸：长边 cell 数 = min(maxCellsOnLongSide, 原图长边 / cellSize)
    //   短边按比例缩放（保持原图宽高比）
    const int srcLong = juce::jmax (srcW, srcH);
    int longCells = juce::jmin (maxCellsOnLongSide, juce::jmax (1, srcLong / cellSize));
    longCells = juce::jmax (1, longCells);

    int cellsW, cellsH;
    if (srcW >= srcH)
    {
        cellsW = longCells;
        cellsH = juce::jmax (1, (int) std::round ((double) longCells * srcH / srcW));
    }
    else
    {
        cellsH = longCells;
        cellsW = juce::jmax (1, (int) std::round ((double) longCells * srcW / srcH));
    }

    // 预计算每个 cell 在原图中对应的像素矩形
    //   · 使用 double 精度步长，避免长宽偏差累积
    const double stepX = (double) srcW / (double) cellsW;
    const double stepY = (double) srcH / (double) cellsH;

    // 预缓存调色板的 Lab 值（避免每格都重算 144 次）
    const auto& palette = getPerlerPalette();
    juce::Array<Lab> paletteLab;
    paletteLab.ensureStorageAllocated (palette.size());
    for (const auto& c : palette)
        paletteLab.add (rgbToLab (c));

    // 用 BitmapData 读原图（仅读）和写目标图（读写）
    juce::Image outImage (juce::Image::ARGB, cellsW * cellSize, cellsH * cellSize, true);

    juce::Image::BitmapData srcBits (source,  juce::Image::BitmapData::readOnly);

    // Bug2：Graphics 的创建较重，放在循环外（原本在内层每个 cell 都 new，会拖慢大量格子场景）
    juce::Graphics g (outImage);

    for (int cy = 0; cy < cellsH; ++cy)
    {
        const int y0 = (int) std::floor (cy       * stepY);
        const int y1 = juce::jmin (srcH, (int) std::floor ((cy + 1) * stepY));
        const int yh = juce::jmax (1, y1 - y0);

        for (int cx = 0; cx < cellsW; ++cx)
        {
            const int x0 = (int) std::floor (cx       * stepX);
            const int x1 = juce::jmin (srcW, (int) std::floor ((cx + 1) * stepX));
            const int xw = juce::jmax (1, x1 - x0);

            // --- 平均颜色（按像素数加权；忽略透明度 < 16/255 的像素作为背景）---
            juce::uint64 sumR = 0, sumG = 0, sumB = 0;
            int count = 0;
            for (int yy = y0; yy < y0 + yh; ++yy)
            {
                for (int xx = x0; xx < x0 + xw; ++xx)
                {
                    const auto px = srcBits.getPixelColour (xx, yy);
                    if (px.getAlpha() < 16) continue;
                    sumR += px.getRed();
                    sumG += px.getGreen();
                    sumB += px.getBlue();
                    ++count;
                }
            }

            if (count <= 0) continue; // 完全透明格：不绘制

            juce::Colour avg ((juce::uint8) (sumR / (juce::uint64) count),
                              (juce::uint8) (sumG / (juce::uint64) count),
                              (juce::uint8) (sumB / (juce::uint64) count));

            // --- 匹配 144 色中最近颜色 ---
            const auto avgLab = rgbToLab (avg);
            int bestIdx = 0;
            float bestDist = std::numeric_limits<float>::max();
            for (int i = 0; i < paletteLab.size(); ++i)
            {
                const float d = labDistSq (avgLab, paletteLab.getReference (i));
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }

            // --- 把这一格填充为拼豆色 ---
            g.setColour (palette.getReference (bestIdx));
            g.fillRect (cx * cellSize, cy * cellSize, cellSize, cellSize);
        }
    }

    outCellsW = cellsW;
    outCellsH = cellsH;
    return outImage;
}

// ----------------------------------------------------------
// 从文件加载图片并加入 perlerImages（失败返回 false）
// ----------------------------------------------------------
bool ModuleWorkspace::addPerlerImageFromFile (const juce::File& file,
                                              juce::Point<int> dropCanvasPos,
                                              int fixedCellsW,
                                              int fixedCellsH,
                                              const juce::Point<int>* topLeftOverride,
                                              int forcedCellSize)
{
    if (! file.existsAsFile()) return false;

    auto src = juce::ImageFileFormat::loadFrom (file);
    if (src.isNull()) return false;

    // 默认像素块大小：4×4（聚焦图片后可经左侧滑块在 [1..15] 切换）
    //   · 恢复布局时（forcedCellSize>0）使用存档里的 cellSize，避免"大小跳变"
    const int cellSize = (forcedCellSize > 0)
                             ? juce::jlimit (minCellSize, maxCellSize, forcedCellSize)
                             : defaultCellSize;

    // 功能3：根据当前 canvas 大小动态推导"最大 cell 数"——
    //   让生成的贴画占 canvas 长边不超过 ~70%，短边不超过 ~70%，避免大图撑爆界面。
    //   对于新拖入（未指定 fixedCells）的图片生效；从持久化恢复时走 fixedCellsW/H 分支。
    const auto canvas = getCanvasArea();
    const int  srcLong = juce::jmax (src.getWidth(), src.getHeight());
    const int  srcW    = src.getWidth();
    const int  srcH    = src.getHeight();
    const int  canvasLong  = juce::jmax (1, juce::jmax (canvas.getWidth(), canvas.getHeight()));
    const int  canvasShort = juce::jmax (1, juce::jmin (canvas.getWidth(), canvas.getHeight()));

    //   贴画长边最大像素 ≈ canvas 长边 * 0.7 → 长边格子数 = maxPx / cellSize
    const int  maxPxLong   = juce::jmax (cellSize * 4, (int) (canvasLong  * 0.7));
    const int  maxPxShort  = juce::jmax (cellSize * 4, (int) (canvasShort * 0.7));
    // 同时还要兼顾"短边也不能超过 canvas 短边 * 0.7"——对窄长条图不出大块
    //   srcW >= srcH 时：长边=W  短边=H；否则反过来
    const bool wIsLong = (srcW >= srcH);
    const int  maxLongCellsByLong  = juce::jmax (1, maxPxLong  / cellSize);
    const int  shortByRatio = wIsLong
                                ? juce::jmax (1, (int) std::round ((double) maxLongCellsByLong * srcH / juce::jmax (1, srcW)))
                                : juce::jmax (1, (int) std::round ((double) maxLongCellsByLong * srcW / juce::jmax (1, srcH)));
    const int  maxShortCellsAllowed = juce::jmax (1, maxPxShort / cellSize);
    int maxCellsOnLongSide = maxLongCellsByLong;
    if (shortByRatio > maxShortCellsAllowed)
    {
        // 按短边反推长边 cell 数（保持原图比例）
        const double ratio = wIsLong
                                ? ((double) srcW / juce::jmax (1, srcH))
                                : ((double) srcH / juce::jmax (1, srcW));
        maxCellsOnLongSide = juce::jmax (1, (int) std::round (maxShortCellsAllowed * ratio));
    }
    // 再夹一个硬性上限，防止极端大图（例如 cellSize=1 时）生成上千格
    maxCellsOnLongSide = juce::jlimit (1, 400, maxCellsOnLongSide);
    // 不让生成的格子数超过"原图长边 / cellSize"（像素粒度上限）
    maxCellsOnLongSide = juce::jmin (maxCellsOnLongSide, juce::jmax (1, srcLong / juce::jmax (1, cellSize)));

    int cellsW = 0, cellsH = 0;
    juce::Image pixel;
    if (fixedCellsW > 0 && fixedCellsH > 0)
    {
        pixel  = buildPerlerImageFixed (src, cellSize, fixedCellsW, fixedCellsH);
        cellsW = fixedCellsW;
        cellsH = fixedCellsH;
    }
    else
    {
        pixel = buildPerlerImage (src, cellSize, maxCellsOnLongSide, cellsW, cellsH);

        // 功能1：新拖入的图片默认大小也吸附到 8px 网格（仅当未用 fixedCells 指定时）
        //   · 把屏幕像素宽高 snap 到 gridSize 的整数倍，反推新 cellsN
        //   · 若 snap 后值与原值不同，重新 buildPerlerImageFixed 一次得到最终贴画
        if (! pixel.isNull())
        {
            auto snapCells = [cellSize] (int cells)
            {
                const int rawPx     = juce::jmax (cellSize, cells * cellSize);
                const int snappedPx = juce::jmax (gridSize, ((rawPx + gridSize / 2) / gridSize) * gridSize);
                return juce::jmax (1, (int) std::round ((double) snappedPx / (double) cellSize));
            };
            const int snappedW = snapCells (cellsW);
            const int snappedH = snapCells (cellsH);
            if (snappedW != cellsW || snappedH != cellsH)
            {
                auto snappedPixel = buildPerlerImageFixed (src, cellSize, snappedW, snappedH);
                if (! snappedPixel.isNull())
                {
                    pixel  = snappedPixel;
                    cellsW = snappedW;
                    cellsH = snappedH;
                }
            }
        }
    }
    if (pixel.isNull()) return false;

    auto img = std::make_unique<PerlerImage>();
    img->pixelImage = pixel;
    img->cellsW     = cellsW;
    img->cellsH     = cellsH;
    img->cellSize   = cellSize;
    img->sourcePath = file.getFullPathName();

    // 位置：topLeftOverride 优先（恢复场景）；否则以鼠标落点为中心
    //   （canvas 已在函数上半段声明，此处直接复用）
    int x, y;
    const bool isRestoreCase = (topLeftOverride != nullptr);
    if (isRestoreCase)
    {
        x = topLeftOverride->x;
        y = topLeftOverride->y;
    }
    else
    {
        x = dropCanvasPos.x - pixel.getWidth() / 2;
        y = dropCanvasPos.y - pixel.getHeight() / 2;
        // 新拖入：吸附到 8px 网格（与模块保持一致）
        auto snapAxis = [this] (int v, int origin)
        {
            const int off = v - origin;
            const int snapped = ((off + gridSize / 2) / gridSize) * gridSize;
            return origin + snapped;
        };
        x = snapAxis (x, canvas.getX());
        y = snapAxis (y, canvas.getY());
    }

    // ⚠ bug1 三次修复（2026-04-23）：
    //   仅在"新拖入图片"时才 clamp 到当前 canvas；
    //   "恢复场景"（启动加载布局）下必须**原样**保留保存的 topLeft，
    //   因为 Editor 构造时窗口还没恢复到用户上次的真实尺寸（默认 960×640），
    //   此时 clamp 会把原本在大窗口右下角的图片永久缩到默认小窗口的右下
    //   （关闭再重开后图片逐次往左上漂移，无法恢复）。
    //   新拖入场景的 clamp 保留不变：鼠标落在 canvas 内，clamp 是无损的。
    if (! isRestoreCase)
    {
        x = juce::jlimit (canvas.getX(), juce::jmax (canvas.getX(), canvas.getRight()  - pixel.getWidth()),  x);
        y = juce::jlimit (canvas.getY(), juce::jmax (canvas.getY(), canvas.getBottom() - pixel.getHeight()), y);
    }
    img->topLeft = { x, y };

    const auto bounds = img->getBounds();

    // release 前先拿 raw 指针：add 之后 OwnedArray 拥有 ptr；layer 持有 ref
    auto* rawImg = img.release();
    perlerImages.add (rawImg);

    // 同步创建子组件 layer（与 perlerImages 平行；索引一致）
    //   · addAndMakeVisible 会把 layer 加入 workspace 子组件，参与 z-order；
    //     默认最新添加的在最前（后画盖在前画之上 —— 和模块 z-order 模型一致）
    //   · bounds 即图片在 workspace 坐标系的矩形
    auto layer = std::make_unique<PerlerImageLayer> (*this, *rawImg);
    layer->setBounds (bounds);
    addAndMakeVisible (*layer);
    perlerLayers.add (layer.release());

    repaint (bounds);
    notifyLayoutChanged();
    return true;
}

int ModuleWorkspace::hitTestPerlerImageAt (juce::Point<int> p) const
{
    // 倒序遍历：后加入的图片在最前方（与 drawImageAt 顺序一致）
    for (int i = perlerImages.size(); --i >= 0;)
    {
        if (auto* img = perlerImages.getUnchecked (i))
            if (img->getBounds().contains (p))
                return i;
    }
    return -1;
}

void ModuleWorkspace::clearPerlerImages()
{
    if (perlerImages.isEmpty()) return;
    // 同步清理子组件（避免 layer->img 野指针）
    perlerLayers.clear();
    perlerImages.clear();
    draggingPerlerIdx  = -1;
    resizingPerlerIdx  = -1;
    focusedPerlerIdx   = -1;
    activeResizeHandle = ResizeHandle::none;
    repaint();
    notifyLayoutChanged();
}

int ModuleWorkspace::getNumPerlerImages() const noexcept
{
    return perlerImages.size();
}

// ----------------------------------------------------------
// Bug3：chrome 隐藏/显示时，workspace 自身 y 位移补偿（Editor 调用）
//   · Editor 已对所有 ModulePanel 做反向平移以保持"屏幕绝对位置不变"，
//     这里对 perlerImages 做同样的平移，修复"hide 后图片上移"的 bug。
// ----------------------------------------------------------
void ModuleWorkspace::shiftAllPerlerImagesY (int dy)
{
    if (dy == 0 || perlerImages.isEmpty()) return;
    for (int i = 0; i < perlerImages.size(); ++i)
    {
        auto* img = perlerImages.getUnchecked (i);
        if (img == nullptr) continue;
        img->topLeft.y += dy;
        syncPerlerLayerBounds (i);   // layer 子组件同步跟随
    }
    repaint();
}

// ----------------------------------------------------------
// 把 perlerImages[idx] 的位置/尺寸同步到 perlerLayers[idx]->setBounds
//   · 任何修改 topLeft / pixelImage / 聚焦态的路径都必须调用一次；
//     否则 Component 在 z-order 中的位置和实际图片不匹配，会出现
//     "图片没动、bounds 没跟上"导致的鼠标 hit / 绘制区域错位问题。
//   · 聚焦图片：bounds = 装饰外包框（图片 + 左 cellSize 滑条 + 右
//     opacity 滑条 + 下 PerlerBeads 复选框 + 四周手柄 / × 按钮），
//     这样装饰区的点击也能被 layer 拦截，避免被下方模块抢走。
//   · 非聚焦图片：bounds = 图片本体矩形
// ----------------------------------------------------------
void ModuleWorkspace::syncPerlerLayerBounds (int idx)
{
    if (idx < 0 || idx >= perlerImages.size() || idx >= perlerLayers.size()) return;
    auto* img   = perlerImages.getUnchecked (idx);
    auto* layer = perlerLayers [idx];
    if (img == nullptr || layer == nullptr) return;

    const auto imgBounds = img->getBounds();
    const auto layerBounds = (idx == focusedPerlerIdx) ? getDecoratedBounds (imgBounds)
                                                       : imgBounds;
    layer->setBounds (layerBounds);
    layer->repaint();
}

// ----------------------------------------------------------
// FileDragAndDropTarget 接口实现
// ----------------------------------------------------------
bool ModuleWorkspace::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        const auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg"
            || ext == ".bmp" || ext == ".gif")
            return true;
    }
    return false;
}

void ModuleWorkspace::fileDragEnter (const juce::StringArray&, int, int) {}
void ModuleWorkspace::fileDragMove  (const juce::StringArray&, int, int) {}
void ModuleWorkspace::fileDragExit  (const juce::StringArray&)            {}

void ModuleWorkspace::filesDropped (const juce::StringArray& files, int x, int y)
{
    const juce::Point<int> dropPos { x, y };
    bool any = false;
    for (const auto& f : files)
    {
        const auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg"
            && ext != ".bmp" && ext != ".gif")
            continue;
        if (addPerlerImageFromFile (juce::File (f), dropPos))
            any = true;
    }
    if (any) repaint();
}

// ----------------------------------------------------------
// 鼠标拖动 / 抬起 —— 图片拖动态处理
// ----------------------------------------------------------
void ModuleWorkspace::mouseDrag (const juce::MouseEvent& e)
{
    // 0) cellSize 左侧滑块拖动态：仅更新 pendingCellSize，mouseUp 才真正 rebuild
    //    （rebuild 很重；拖动过程中只做"拇指位置"的 repaint 以给用户反馈）
    if (cellSizeDraggingIdx >= 0 && cellSizeDraggingIdx < perlerImages.size()
        && focusedPerlerIdx == cellSizeDraggingIdx)
    {
        auto* img = perlerImages.getUnchecked (cellSizeDraggingIdx);
        if (img == nullptr) return;

        const auto fb = img->getBounds();
        const int newCellSize = cellSizeFromSliderY (fb, e.getPosition().y);

        // 只在上次 pending 值真的变了才重绘
        if (newCellSize != pendingCellSize)
        {
            pendingCellSize = newCellSize;
            // 只重绘聚焦图片左侧的滑块装饰范围
            //   · X：滑块左侧数字标签 (labelW=36)、右侧 1px cushion
            //   · Y：上下各加 8px，避免死来死去的接缝脑残影
            const auto sliderArea = getCellSizeSliderBounds (fb)
                                        .expanded (cellSizeSliderW + 42, 8);
            repaint (sliderArea);
        }
        return;
    }

    // 0') 右侧 opacity 滑块拖动态：实时写 opacity + 重绘滑条和图片本体
    if (opacityDraggingIdx >= 0 && opacityDraggingIdx < perlerImages.size()
        && focusedPerlerIdx == opacityDraggingIdx)
    {
        auto* img = perlerImages.getUnchecked (opacityDraggingIdx);
        if (img == nullptr) return;

        const auto fb = img->getBounds();
        const float newAlpha = opacityFromSliderY (fb, e.getPosition().y);
        if (std::abs (newAlpha - img->opacity) > 1.0e-3f)
        {
            img->opacity = newAlpha;
            const auto sliderArea = getOpacitySliderBounds (fb).expanded (cellSizeSliderW + 4, 8);
            repaint (sliderArea);
            if (auto* layer = perlerLayers[opacityDraggingIdx]) layer->repaint();
        }
        return;
    }

    // 1) 缩放拖动态：更新预览框（不重量化）
    if (resizingPerlerIdx >= 0 && resizingPerlerIdx < perlerImages.size()
        && activeResizeHandle != ResizeHandle::none)
    {
        auto* img = perlerImages.getUnchecked (resizingPerlerIdx);
        if (img == nullptr) return;

        const int cellSize = juce::jmax (1, img->cellSize);
        const auto canvas  = getCanvasArea();
        const auto mouse   = e.getPosition();
        const auto h       = activeResizeHandle;

        const bool isCorner = (h == ResizeHandle::topLeft
                               || h == ResizeHandle::topRight
                               || h == ResizeHandle::bottomLeft
                               || h == ResizeHandle::bottomRight);

        const bool dragW = (h == ResizeHandle::left || h == ResizeHandle::right || isCorner);
        const bool dragH = (h == ResizeHandle::top  || h == ResizeHandle::bottom || isCorner);

        int newX = resizeStartRect.getX();
        int newY = resizeStartRect.getY();
        int newW = resizeStartRect.getWidth();
        int newH = resizeStartRect.getHeight();

        if (isCorner)
        {
            // ====== 功能1：角落 handle → 按起始矩形的宽高比等比缩放 ======
            const bool left = (h == ResizeHandle::topLeft   || h == ResizeHandle::bottomLeft);
            const bool top  = (h == ResizeHandle::topLeft   || h == ResizeHandle::topRight);

            // 以鼠标坐标 vs 锚点（起始时记下的对角点）推出原始矩形
            const auto anchor = resizeAnchorPos;

            // 先按鼠标计算裸宽高（无比例约束）
            int rawW = std::abs (mouse.x - anchor.x);
            int rawH = std::abs (mouse.y - anchor.y);

            // 再按起始比例把 rawW/rawH 约束为等比（取较大的那个维度主导）
            const double startAspect = (resizeStartRect.getHeight() > 0)
                                          ? (double) resizeStartRect.getWidth() / (double) resizeStartRect.getHeight()
                                          : 1.0;

            if (startAspect > 0.0)
            {
                // 若按 rawW 推出的 H 比 rawH 大，则以 W 为主导；反之以 H 为主导
                const double hFromW = (double) rawW / startAspect;
                if (hFromW >= (double) rawH)
                {
                    rawH = (int) std::round (hFromW);
                }
                else
                {
                    rawW = (int) std::round ((double) rawH * startAspect);
                }
            }

            // 量化到 cellSize 的整数倍，且 ≥ cellSize
            int cellsAcross = juce::jmax (1, (rawW + cellSize / 2) / cellSize);
            int cellsDown   = juce::jmax (1, (rawH + cellSize / 2) / cellSize);

            // 再次锁比：以 cellsW/cellsH 的起始比例把某一个维度重算
            const double cellsAspect = (resizeStartCellsH > 0)
                                          ? (double) resizeStartCellsW / (double) resizeStartCellsH
                                          : 1.0;
            if (cellsAspect > 0.0)
            {
                const double hFromW = (double) cellsAcross / cellsAspect;
                if (hFromW >= (double) cellsDown)
                    cellsDown = juce::jmax (1, (int) std::round (hFromW));
                else
                    cellsAcross = juce::jmax (1, (int) std::round ((double) cellsDown * cellsAspect));
            }

            newW = cellsAcross * cellSize;
            newH = cellsDown   * cellSize;
            newX = left ? (anchor.x - newW) : anchor.x;
            newY = top  ? (anchor.y - newH) : anchor.y;

            // 夹到 canvas 内：若超出则按能容纳的最大比例缩回
            //   先算出允许的最大 cells（按比例同步收缩）
            int maxCellsW = juce::jmax (1, (canvas.getWidth())  / cellSize);
            int maxCellsH = juce::jmax (1, (canvas.getHeight()) / cellSize);
            if (cellsAcross > maxCellsW || cellsDown > maxCellsH)
            {
                const double sx = (double) maxCellsW / cellsAcross;
                const double sy = (double) maxCellsH / cellsDown;
                const double s  = juce::jmin (sx, sy);
                cellsAcross = juce::jmax (1, (int) std::floor (cellsAcross * s));
                cellsDown   = juce::jmax (1, (int) std::floor (cellsDown   * s));
                newW = cellsAcross * cellSize;
                newH = cellsDown   * cellSize;
                newX = left ? (anchor.x - newW) : anchor.x;
                newY = top  ? (anchor.y - newH) : anchor.y;
            }
            // 若偏移后超出 canvas，再整体平移回来（保持尺寸不变）
            if (newX < canvas.getX())      newX = canvas.getX();
            if (newY < canvas.getY())      newY = canvas.getY();
            if (newX + newW > canvas.getRight())
                newX = juce::jmax (canvas.getX(), canvas.getRight() - newW);
            if (newY + newH > canvas.getBottom())
                newY = juce::jmax (canvas.getY(), canvas.getBottom() - newH);
        }
        else
        {
            // ====== 四边 handle → 非等比，保持原有行为 ======
            if (dragW)
            {
                const bool left = (h == ResizeHandle::left);
                if (left)
                {
                    const int proposedX = juce::jlimit (canvas.getX(), resizeStartRect.getRight() - cellSize, mouse.x);
                    const int cellsAcross = juce::jmax (1, (resizeStartRect.getRight() - proposedX + cellSize / 2) / cellSize);
                    newW = cellsAcross * cellSize;
                    newX = resizeStartRect.getRight() - newW;
                }
                else
                {
                    const int proposedR = juce::jlimit (resizeStartRect.getX() + cellSize, canvas.getRight(), mouse.x);
                    const int cellsAcross = juce::jmax (1, (proposedR - resizeStartRect.getX() + cellSize / 2) / cellSize);
                    newW = cellsAcross * cellSize;
                    newX = resizeStartRect.getX();
                }
            }

            if (dragH)
            {
                const bool top = (h == ResizeHandle::top);
                if (top)
                {
                    const int proposedY = juce::jlimit (canvas.getY(), resizeStartRect.getBottom() - cellSize, mouse.y);
                    const int cellsDown  = juce::jmax (1, (resizeStartRect.getBottom() - proposedY + cellSize / 2) / cellSize);
                    newH = cellsDown * cellSize;
                    newY = resizeStartRect.getBottom() - newH;
                }
                else
                {
                    const int proposedB = juce::jlimit (resizeStartRect.getY() + cellSize, canvas.getBottom(), mouse.y);
                    const int cellsDown  = juce::jmax (1, (proposedB - resizeStartRect.getY() + cellSize / 2) / cellSize);
                    newH = cellsDown * cellSize;
                    newY = resizeStartRect.getY();
                }
            }
        }

        newW = juce::jmax (cellSize, newW);
        newH = juce::jmax (cellSize, newH);

        const auto oldPreview = resizePreviewRect;
        resizePreviewRect   = juce::Rectangle<int> (newX, newY, newW, newH);
        resizePreviewCellsW = newW / cellSize;
        resizePreviewCellsH = newH / cellSize;

        // Bug2 修复：脏区需覆盖"预览框 + 8 手柄 + 下方文字提示"完整区域，
        //   且累积合并所有历史预览区，避免底图残留导致文字乱码叠影。
        // Bug1 修复：还需把左侧 cellSize 滑块（含 42px 数字标签）的区域算进脏区，
        //   否则 resize 过程中滑块会随图片位置移动而残留旧影。
        const int textPadBelow  = 20;   // 下方文字区域预留高度（14 文字 + 4 边距 + 2 冗余）
        const int textPadRight  = 220;  // 文字最多可能 200px 宽 + 余量
        const int handlePad     = handleSize + 2;
        const int leftPadSlider = cellSizeSliderW + cellSizeSliderGap + 42 + 4; // 滑块 + 标签

        auto dirtyFrom = [handlePad, textPadBelow, textPadRight, leftPadSlider]
                         (const juce::Rectangle<int>& r)
        {
            if (r.isEmpty()) return juce::Rectangle<int>();
            auto d = r.expanded (handlePad);
            // 向左扩展包含 cellSize 滑块区域
            d.setX (d.getX() - leftPadSlider);
            d.setWidth (d.getWidth() + leftPadSlider);
            // 下方文字提示（W × H）
            d = d.getUnion (juce::Rectangle<int> (r.getX(), r.getBottom() + 2,
                                                  textPadRight, textPadBelow));
            return d;
        };

        const auto oldDirty = dirtyFrom (oldPreview);
        const auto newDirty = dirtyFrom (resizePreviewRect);

        // 合并到全局累积脏区（本轮拖动开始时清空）
        resizeDirtyUnion = resizeDirtyUnion.getUnion (oldDirty).getUnion (newDirty);

        // 本帧重绘：旧预览区 + 新预览区（无需每次都 repaint 整个累积区）
        if (! oldDirty.isEmpty()) repaint (oldDirty);
        if (! newDirty.isEmpty()) repaint (newDirty);
        return;
    }

    // 2) 平移拖动态
    if (draggingPerlerIdx < 0 || draggingPerlerIdx >= perlerImages.size())
        return;

    auto* img = perlerImages.getUnchecked (draggingPerlerIdx);
    if (img == nullptr) return;

    const auto canvas = getCanvasArea();
    int nx = e.getPosition().x - perlerDragOffset.x;
    int ny = e.getPosition().y - perlerDragOffset.y;

    // 吸附到 8px 网格（与模块拖动行为保持一致）
    //   · 以 canvas 原点为基准，四舍五入到 gridSize 的整数倍
    //   · 这样贴画边缘能与模块边缘、以及 Grid 叠加层的网格线对齐
    auto snapAxis = [this] (int v, int origin)
    {
        const int off = v - origin;
        const int snapped = ((off + gridSize / 2) / gridSize) * gridSize;
        return origin + snapped;
    };
    nx = snapAxis (nx, canvas.getX());
    ny = snapAxis (ny, canvas.getY());

    nx = juce::jlimit (canvas.getX(), juce::jmax (canvas.getX(), canvas.getRight()  - img->pixelImage.getWidth()),  nx);
    ny = juce::jlimit (canvas.getY(), juce::jmax (canvas.getY(), canvas.getBottom() - img->pixelImage.getHeight()), ny);

    if (nx == img->topLeft.x && ny == img->topLeft.y)
        return;

    // 脏区必须涵盖聚焦图片的"所有装饰"（左 cellSize 滑条 / 右 opacity 滑条 /
    //   上手柄 + × / 下 PerlerBeads 复选框 / 两侧的数字标签），否则任何一个
    //   装饰留在旧位置都会有拖影。
    //   · 统一使用成员函数 getDecoratedBounds —— 它已精确覆盖所有装饰（包括
    //     opacity 滑条右侧的百分比标签），与 layer bounds、syncPerlerLayerBounds
    //     完全对齐，单一真相源。
    const auto oldBounds = getDecoratedBounds (img->getBounds());
    img->topLeft = { nx, ny };
    syncPerlerLayerBounds (draggingPerlerIdx);
    const auto newBounds = getDecoratedBounds (img->getBounds());
    repaint (oldBounds.getUnion (newBounds));
}

void ModuleWorkspace::mouseUp (const juce::MouseEvent&)
{
    // 0) cellSize 滑块拖动结束：此时才真正 rebuild（Bug4）
    if (cellSizeDraggingIdx >= 0)
    {
        const int idx = cellSizeDraggingIdx;
        const int cs  = pendingCellSize;
        cellSizeDraggingIdx = -1;
        pendingCellSize     = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);

        if (cs > 0 && idx >= 0 && idx < perlerImages.size())
        {
            auto* img = perlerImages.getUnchecked (idx);
            if (img != nullptr && cs != img->cellSize)
            {
                rebuildPerlerImageCellSize (idx, cs);
                repaint(); // 整区刷新，避免旧贴画残影
            }
            else
            {
                // 未发生值变化：仅清除滑块区域（拇指位置重置）
                if (img != nullptr)
                {
                    const auto sliderArea = getCellSizeSliderBounds (img->getBounds())
                                                .expanded (cellSizeSliderW + 42, 8);
                    repaint (sliderArea);
                }
            }
        }
        notifyLayoutChanged();
        return;
    }

    // 0') opacity 滑块拖动结束：仅清状态并持久化新值（无 rebuild）
    if (opacityDraggingIdx >= 0)
    {
        opacityDraggingIdx = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        notifyLayoutChanged();   // 透明度作为 perlerImage 状态的一部分需要保存
        return;
    }

    // 1) 缩放拖动结束：重新量化图片
    if (resizingPerlerIdx >= 0 && resizingPerlerIdx < perlerImages.size())
    {
        const int idx  = resizingPerlerIdx;
        int newW = resizePreviewCellsW;
        int newH = resizePreviewCellsH;
        auto newRect = resizePreviewRect;

        auto* img = perlerImages.getUnchecked (idx);
        if (img != nullptr && newW > 0 && newH > 0)
        {
            // 功能1：图片缩放后把"屏幕像素尺寸"吸附到 8px 网格
            //   · 算出目标屏幕像素宽高 = cellsN * cellSize，snap 到 gridSize 的整数倍
            //   · 反推新的 cellsN = round(targetPx / cellSize)，至少 1 格
            //   · 这样不同 cellSize 都能尽量让图片整体边缘对齐 8px 网格；
            //     cellSize 与 gridSize 不整除时也尽量接近
            const int cs = juce::jmax (1, img->cellSize);
            auto snapCells = [cs] (int cells)
            {
                const int rawPx     = juce::jmax (cs, cells * cs);
                const int snappedPx = juce::jmax (gridSize, ((rawPx + gridSize / 2) / gridSize) * gridSize);
                return juce::jmax (1, (int) std::round ((double) snappedPx / (double) cs));
            };
            newW = snapCells (newW);
            newH = snapCells (newH);
            // 同步更新预览矩形的尺寸（仅用于脏区 / 下方提示），位置保留
            newRect.setWidth  (newW * cs);
            newRect.setHeight (newH * cs);

            const auto oldBounds = img->getBounds().expanded (handleSize + 2);
            const auto oldCheck  = getPerlerBeadsCheckboxBounds (img->getBounds()).expanded (2);
            img->topLeft = newRect.getPosition();
            rebuildPerlerImage (idx, newW, newH);
            syncPerlerLayerBounds (idx);   // rebuild 后 pixelImage / topLeft 均可能变动
            const auto newBounds = img->getBounds().expanded (handleSize + 2);
            const auto newCheck  = getPerlerBeadsCheckboxBounds (img->getBounds()).expanded (2);
            // Bug2：把整个缩放过程中累积的脏区都刷一遍，彻底清除底图文字残影
            //      （同时把新/旧的 PerlerBeads 复选框行合并进去，防止复选框位置变化留下残影）
            auto refresh = oldBounds.getUnion (newBounds).getUnion (resizeDirtyUnion);
            if (! oldCheck.isEmpty()) refresh = refresh.getUnion (oldCheck);
            if (! newCheck.isEmpty()) refresh = refresh.getUnion (newCheck);
            if (! refresh.isEmpty()) repaint (refresh);
            notifyLayoutChanged();
        }

        resizingPerlerIdx   = -1;
        activeResizeHandle  = ResizeHandle::none;
        resizePreviewRect   = {};
        resizePreviewCellsW = resizePreviewCellsH = 0;
        resizeDirtyUnion    = {};
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    // 2) 平移拖动结束
    if (draggingPerlerIdx >= 0)
    {
        draggingPerlerIdx = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        notifyLayoutChanged();
    }
}

// ----------------------------------------------------------
// 鼠标移动 / 离开 —— 聚焦图片的 × 按钮 hover 效果 + 光标切换
// ----------------------------------------------------------
void ModuleWorkspace::mouseMove (const juce::MouseEvent& e)
{
    if (focusedPerlerIdx < 0 || focusedPerlerIdx >= perlerImages.size())
    {
        if (deleteBtnHovered)
        {
            deleteBtnHovered = false;
            repaint();
        }
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    auto* fimg = perlerImages.getUnchecked (focusedPerlerIdx);
    if (fimg == nullptr) return;

    const auto fb  = fimg->getBounds();
    const auto xr  = getDeleteBtnRect (fb);
    const bool hoverX = xr.contains (e.getPosition());
    if (hoverX != deleteBtnHovered)
    {
        deleteBtnHovered = hoverX;
        repaint (xr.expanded (2));
    }

    // 光标：在滑块/手柄/x/图片体上切换
    if (hoverX)
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }
    if (getCellSizeSliderBounds (fb).contains (e.getPosition()))
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    if (getOpacitySliderBounds (fb).contains (e.getPosition()))
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    const auto h = hitTestHandle (fb, e.getPosition());
    switch (h)
    {
        case ResizeHandle::topLeft:
        case ResizeHandle::bottomRight:
            setMouseCursor (juce::MouseCursor::TopLeftCornerResizeCursor);
            return;
        case ResizeHandle::topRight:
        case ResizeHandle::bottomLeft:
            setMouseCursor (juce::MouseCursor::TopRightCornerResizeCursor);
            return;
        case ResizeHandle::top:
        case ResizeHandle::bottom:
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        case ResizeHandle::left:
        case ResizeHandle::right:
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            return;
        default: break;
    }
    if (fb.contains (e.getPosition()))
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void ModuleWorkspace::mouseExit (const juce::MouseEvent&)
{
    if (deleteBtnHovered)
    {
        deleteBtnHovered = false;
        if (focusedPerlerIdx >= 0 && focusedPerlerIdx < perlerImages.size())
            if (auto* fimg = perlerImages.getUnchecked (focusedPerlerIdx))
                repaint (getDeleteBtnRect (fimg->getBounds()).expanded (2));
    }
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

// ----------------------------------------------------------
// 聚焦框几何帮助函数
// ----------------------------------------------------------
juce::Rectangle<int> ModuleWorkspace::getHandleRect (juce::Rectangle<int> b, ResizeHandle h) const
{
    const int s = handleSize;
    const int hs = s / 2;
    // 手柄放在选中框的"边上"，中心点落在边 / 角上
    const int xL = b.getX()      - hs;
    const int xR = b.getRight()  - hs;
    const int xC = b.getCentreX() - hs;
    const int yT = b.getY()       - hs;
    const int yB = b.getBottom()  - hs;
    const int yC = b.getCentreY() - hs;

    switch (h)
    {
        case ResizeHandle::topLeft:     return { xL, yT, s, s };
        case ResizeHandle::top:         return { xC, yT, s, s };
        case ResizeHandle::topRight:    return { xR, yT, s, s };
        case ResizeHandle::left:        return { xL, yC, s, s };
        case ResizeHandle::right:       return { xR, yC, s, s };
        case ResizeHandle::bottomLeft:  return { xL, yB, s, s };
        case ResizeHandle::bottom:      return { xC, yB, s, s };
        case ResizeHandle::bottomRight: return { xR, yB, s, s };
        default:                        return {};
    }
}

juce::Rectangle<int> ModuleWorkspace::getDeleteBtnRect (juce::Rectangle<int> b) const
{
    // 与 ModulePanel 标题栏右上角的 × 按钮风格一致：
    //   ·  18×18 凸起按钮
    //   ·  右上角外侧"半露"，让用户更容易从框外目视到并点击
    const int s = deleteBtnSize;
    return { b.getRight() - s / 2 - 2, b.getY() - s / 2 + 2, s, s };
}

// ----------------------------------------------------------
// cellSize 左侧滑块几何（仅在图片聚焦态可见/可交互）
// ----------------------------------------------------------
juce::Rectangle<int> ModuleWorkspace::getCellSizeSliderBounds (juce::Rectangle<int> imgBounds) const
{
    // 滑槽高度随图片高度自适应（但有合理的上下限，避免过小/过长）
    const int targetH = juce::jlimit (80, 260, imgBounds.getHeight());
    const int sliderH = targetH;
    const int y = imgBounds.getCentreY() - sliderH / 2;
    const int x = imgBounds.getX() - cellSizeSliderGap - cellSizeSliderW;

    juce::Rectangle<int> r { x, y, cellSizeSliderW, sliderH };

    // 不允许超出 canvas 左边界（极端情况下图片贴在 canvas 左侧）
    const auto canvas = getCanvasArea();
    if (r.getX() < canvas.getX())
    {
        // canvas 左侧空间不够 → 把滑块贴到图片左侧外沿最近的可用位置
        r.setX (canvas.getX());
    }
    return r;
}

juce::Rectangle<int> ModuleWorkspace::getCellSizeThumbBounds (juce::Rectangle<int> imgBounds, int cellSize) const
{
    const auto track = getCellSizeSliderBounds (imgBounds);
    if (track.isEmpty()) return {};

    const int cs = juce::jlimit (minCellSize, maxCellSize, cellSize);
    // cellSize 越大 → 拇指越靠上（语义：滑块向上代表更大的像素块）
    //   反之若觉得反直觉，未来可翻转；这里按"上 = 大"默认
    const double t = (double) (cs - minCellSize) / (double) juce::jmax (1, (maxCellSize - minCellSize));
    const int thumbH = cellSizeSliderThumb;
    // 内边距 2px
    const int yTop    = track.getY()      + 2;
    const int yBottom = track.getBottom() - 2 - thumbH;
    const int ty      = (int) std::round (yBottom - t * (yBottom - yTop));

    return { track.getX() - 2, ty, track.getWidth() + 4, thumbH };
}

int ModuleWorkspace::cellSizeFromSliderY (juce::Rectangle<int> imgBounds, int y) const
{
    const auto track = getCellSizeSliderBounds (imgBounds);
    if (track.isEmpty()) return defaultCellSize;

    const int thumbH = cellSizeSliderThumb;
    const int yTop    = track.getY()      + 2;
    const int yBottom = track.getBottom() - 2 - thumbH;
    const int range   = juce::jmax (1, yBottom - yTop);

    // yBottom 对应 minCellSize，yTop 对应 maxCellSize
    const double t = (double) (yBottom - juce::jlimit (yTop, yBottom, y)) / (double) range;
    const int cs = (int) std::round (t * (maxCellSize - minCellSize)) + minCellSize;
    return juce::jlimit (minCellSize, maxCellSize, cs);
}

// ----------------------------------------------------------
// 聚焦态"右侧"opacity 滑块几何（样式与左侧 cellSize 滑块对称）
//   · x = 图片右边外沿 + cellSizeSliderGap
//   · 上 = 不透明(1.0)，下 = 完全透明(0.0)
// ----------------------------------------------------------
juce::Rectangle<int> ModuleWorkspace::getOpacitySliderBounds (juce::Rectangle<int> imgBounds) const
{
    const int targetH = juce::jlimit (80, 260, imgBounds.getHeight());
    const int sliderH = targetH;
    const int y = imgBounds.getCentreY() - sliderH / 2;
    const int x = imgBounds.getRight() + cellSizeSliderGap;

    juce::Rectangle<int> r { x, y, cellSizeSliderW, sliderH };

    // 不越过 canvas 右边界（极端情况下图片贴在 canvas 右侧）
    const auto canvas = getCanvasArea();
    if (r.getRight() > canvas.getRight())
        r.setX (juce::jmax (canvas.getX(), canvas.getRight() - cellSizeSliderW));
    return r;
}

juce::Rectangle<int> ModuleWorkspace::getOpacityThumbBounds (juce::Rectangle<int> imgBounds, float opacity) const
{
    const auto track = getOpacitySliderBounds (imgBounds);
    if (track.isEmpty()) return {};

    const float a = juce::jlimit (0.0f, 1.0f, opacity);
    const int thumbH = cellSizeSliderThumb;
    const int yTop    = track.getY()      + 2;
    const int yBottom = track.getBottom() - 2 - thumbH;
    // opacity = 1 → 最顶（yTop）；opacity = 0 → 最底（yBottom）
    const int ty = (int) std::round (yBottom - a * (double) (yBottom - yTop));

    return { track.getX() - 2, ty, track.getWidth() + 4, thumbH };
}

float ModuleWorkspace::opacityFromSliderY (juce::Rectangle<int> imgBounds, int y) const
{
    const auto track = getOpacitySliderBounds (imgBounds);
    if (track.isEmpty()) return 1.0f;

    const int thumbH = cellSizeSliderThumb;
    const int yTop    = track.getY()      + 2;
    const int yBottom = track.getBottom() - 2 - thumbH;
    const int range   = juce::jmax (1, yBottom - yTop);
    // yBottom 对应 0.0，yTop 对应 1.0
    const float t = (float) (yBottom - juce::jlimit (yTop, yBottom, y)) / (float) range;
    return juce::jlimit (0.0f, 1.0f, t);
}

// ----------------------------------------------------------
// 装饰外包框：图片矩形 + 左 cellSize 滑条 + 右 opacity 滑条 + 上手柄/×
//   + 下 PerlerBeads 复选框。用于：
//     · 聚焦态下 layer 子组件的 bounds（让装饰区点击能被 layer 拦截）
//     · repaint 脏区计算
// ----------------------------------------------------------
juce::Rectangle<int> ModuleWorkspace::getDecoratedBounds (juce::Rectangle<int> imgBounds) const
{
    // 左侧：cellSize 滑条 + 间距 + 数字标签（~42px "Npx"）+ 一点 cushion
    const int leftPad  = cellSizeSliderW + cellSizeSliderGap + 42 /* label */ + 4;
    // 右侧：opacity 滑条 + 间距 + 百分比标签（labelW=36，在滑条右侧画）+ cushion
    //   · 百分比标签必须包含在脏区内，否则拖动图片时数字会有"拖影"（残留绘制）
    //   · 同时还要保证包含缩放手柄 / × 按钮（虽然它们通常在图片内侧，rightPad
    //     不影响它们，但保留 max 以便未来布局变化时兜底）
    const int rightPad = juce::jmax (handleSize, deleteBtnSize)
                         + cellSizeSliderW + cellSizeSliderGap
                         + 36 /* opacity % label */ + 2 + 4;
    // 上：缩放手柄 + × 按钮
    const int topPad   = juce::jmax (handleSize, deleteBtnSize) + 4;
    // 下：缩放手柄 + PerlerBeads 复选框行
    const int botPad   = juce::jmax (handleSize + 4,
                                     perlerBeadsRowGap + perlerBeadsRowH + 4);
    return { imgBounds.getX() - leftPad,
             imgBounds.getY() - topPad,
             imgBounds.getWidth()  + leftPad + rightPad,
             imgBounds.getHeight() + topPad  + botPad };
}

// ----------------------------------------------------------
// PerlerBeads 复选框几何（仅在图片聚焦态可见 / 可交互）
// ----------------------------------------------------------
juce::Rectangle<int> ModuleWorkspace::getPerlerBeadsCheckboxBounds (juce::Rectangle<int> imgBounds) const
{
    // 整行命中区：位于图片正下方，行高 perlerBeadsRowH，
    //   宽度 = 方框 + 4px 间距 + "PerlerBeads" 文字宽度（近似 90px），整体左对齐图片左边
    const int rowW = perlerBeadsBoxSize + 4 + 90;
    const int rowH = perlerBeadsRowH;
    const int x    = imgBounds.getX();
    const int y    = imgBounds.getBottom() + perlerBeadsRowGap;
    juce::Rectangle<int> r { x, y, rowW, rowH };

    // 不越过 canvas 下边界：若空间不足，回到图片底边内侧（让用户仍能看到复选框）
    const auto canvas = getCanvasArea();
    if (r.getBottom() > canvas.getBottom())
        r.setY (juce::jmax (canvas.getY(), imgBounds.getBottom() - rowH - 2));
    return r;
}

juce::Rectangle<int> ModuleWorkspace::getPerlerBeadsCheckboxBoxRect (juce::Rectangle<int> imgBounds) const
{
    const auto row = getPerlerBeadsCheckboxBounds (imgBounds);
    const int s    = perlerBeadsBoxSize;
    // 方框垂直居中于整行，水平贴在行首
    return { row.getX(), row.getCentreY() - s / 2, s, s };
}

// ----------------------------------------------------------
// 以 "圆环模式" 渲染一张拼豆贴画（见 PerlerImage::perlerBeadsMode）
//   · 读取 pixelImage 每个 cell 的中心像素颜色
//   · 外圆填充 cell，内圆镂空 → 使用非零环绕填充得到环形
//   · 与 EqModule::drawEq 中的 "珠子" 视觉比例一致（内径 ≈ cell * 0.2）
// ----------------------------------------------------------
void ModuleWorkspace::drawPerlerImageAsBeads (juce::Graphics& g, const PerlerImage& img, float opacity) const
{
    if (img.pixelImage.isNull()) return;

    const int cs = juce::jmax (1, img.cellSize);
    const int cw = juce::jmax (0, img.cellsW);
    const int ch = juce::jmax (0, img.cellsH);
    const int x0 = img.topLeft.x;
    const int y0 = img.topLeft.y;

    // opacity 夹紧到 [0,1]，用于乘进每个 bead 的颜色 alpha
    const float alpha = juce::jlimit (0.0f, 1.0f, opacity);

    // 在“只读”上下文中读取像素颜色
    const juce::Image::BitmapData pixels (img.pixelImage, juce::Image::BitmapData::readOnly);

    const float outerR = (float) cs * 0.5f;
    // 与 EqModule 一致的"珠子"内径比例；但当 cellSize 很小时保证至少 1px 以产生可见镂空
    float innerR = (float) cs * 0.20f;
    if (cs <= 2) innerR = 0.0f;                    // 太小直接画实心点
    else         innerR = juce::jmax (innerR, 1.0f);

    for (int cy = 0; cy < ch; ++cy)
    {
        for (int cx = 0; cx < cw; ++cx)
        {
            // 取 cell 中心像素颜色
            const int sx = juce::jlimit (0, pixels.width  - 1, cx * cs + cs / 2);
            const int sy = juce::jlimit (0, pixels.height - 1, cy * cs + cs / 2);
            const auto col = pixels.getPixelColour (sx, sy);
            if (col.getAlpha() == 0) continue;    // 保留透明

            const float fcx = (float) (x0 + cx * cs) + (float) cs * 0.5f;
            const float fcy = (float) (y0 + cy * cs) + (float) cs * 0.5f;

            // 把全局 opacity 乘进颜色 alpha：
            //   不能用 g.setOpacity，因为 setColour 会完整覆盖它的效果。
            g.setColour (alpha < 1.0f ? col.withMultipliedAlpha (alpha) : col);

            if (innerR <= 0.0f)
            {
                g.fillEllipse (fcx - outerR, fcy - outerR, outerR * 2.0f, outerR * 2.0f);
            }
            else
            {
                juce::Path ring;
                ring.addEllipse (fcx - outerR, fcy - outerR, outerR * 2.0f, outerR * 2.0f);
                ring.addEllipse (fcx - innerR, fcy - innerR, innerR * 2.0f, innerR * 2.0f);
                ring.setUsingNonZeroWinding (false);
                g.fillPath (ring);
            }
        }
    }
}

ModuleWorkspace::ResizeHandle ModuleWorkspace::hitTestHandle (juce::Rectangle<int> b, juce::Point<int> p) const
{
    for (auto h : { ResizeHandle::topLeft, ResizeHandle::top, ResizeHandle::topRight,
                    ResizeHandle::left,    ResizeHandle::right,
                    ResizeHandle::bottomLeft, ResizeHandle::bottom, ResizeHandle::bottomRight })
    {
        if (getHandleRect (b, h).contains (p))
            return h;
    }
    return ResizeHandle::none;
}

// ----------------------------------------------------------
// 按指定格子数量化（与 buildPerlerImage 主体相同，只是 cells 固定不推导）
// ----------------------------------------------------------
juce::Image ModuleWorkspace::buildPerlerImageFixed (const juce::Image& source,
                                                    int cellSize,
                                                    int targetCellsW,
                                                    int targetCellsH)
{
    if (source.isNull() || cellSize <= 0 || targetCellsW <= 0 || targetCellsH <= 0)
        return {};

    const int srcW = source.getWidth();
    const int srcH = source.getHeight();
    if (srcW <= 0 || srcH <= 0) return {};

    const double stepX = (double) srcW / (double) targetCellsW;
    const double stepY = (double) srcH / (double) targetCellsH;

    const auto& palette = getPerlerPalette();
    juce::Array<Lab> paletteLab;
    paletteLab.ensureStorageAllocated (palette.size());
    for (const auto& c : palette)
        paletteLab.add (rgbToLab (c));

    juce::Image outImage (juce::Image::ARGB, targetCellsW * cellSize, targetCellsH * cellSize, true);
    juce::Image::BitmapData srcBits (source, juce::Image::BitmapData::readOnly);

    // Bug2：Graphics 的创建/析构较重，原实现在内层循环每个格都 new，
    //   当格数大时（例如 400×400）会卡死 0.5~2s 且业务线程卷旋。此处只创建一次。
    juce::Graphics g (outImage);

    for (int cy = 0; cy < targetCellsH; ++cy)
    {
        const int y0 = (int) std::floor (cy       * stepY);
        const int y1 = juce::jmin (srcH, (int) std::floor ((cy + 1) * stepY));
        const int yh = juce::jmax (1, y1 - y0);

        for (int cx = 0; cx < targetCellsW; ++cx)
        {
            const int x0 = (int) std::floor (cx       * stepX);
            const int x1 = juce::jmin (srcW, (int) std::floor ((cx + 1) * stepX));
            const int xw = juce::jmax (1, x1 - x0);

            juce::uint64 sumR = 0, sumG = 0, sumB = 0;
            int count = 0;
            for (int yy = y0; yy < y0 + yh; ++yy)
            {
                for (int xx = x0; xx < x0 + xw; ++xx)
                {
                    const auto px = srcBits.getPixelColour (xx, yy);
                    if (px.getAlpha() < 16) continue;
                    sumR += px.getRed();
                    sumG += px.getGreen();
                    sumB += px.getBlue();
                    ++count;
                }
            }

            if (count <= 0) continue;

            const juce::Colour avg ((juce::uint8) (sumR / (juce::uint64) count),
                                    (juce::uint8) (sumG / (juce::uint64) count),
                                    (juce::uint8) (sumB / (juce::uint64) count));

            const auto avgLab = rgbToLab (avg);
            int bestIdx = 0;
            float bestDist = std::numeric_limits<float>::max();
            for (int i = 0; i < paletteLab.size(); ++i)
            {
                const float d = labDistSq (avgLab, paletteLab.getReference (i));
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }

            g.setColour (palette.getReference (bestIdx));
            g.fillRect (cx * cellSize, cy * cellSize, cellSize, cellSize);
        }
    }
    return outImage;
}

// ----------------------------------------------------------
// 缩放结束后，用新格子数重新量化指定图片
// ----------------------------------------------------------
void ModuleWorkspace::rebuildPerlerImage (int idx, int newCellsW, int newCellsH)
{
    if (idx < 0 || idx >= perlerImages.size()) return;
    auto* img = perlerImages.getUnchecked (idx);
    if (img == nullptr) return;

    const int cellSize = juce::jmax (1, img->cellSize);
    newCellsW = juce::jmax (1, newCellsW);
    newCellsH = juce::jmax (1, newCellsH);

    juce::Image newPixel;

    // 优先用源图重新量化（画质最佳）
    if (img->sourcePath.isNotEmpty())
    {
        const juce::File f (img->sourcePath);
        if (f.existsAsFile())
        {
            const auto src = juce::ImageFileFormat::loadFrom (f);
            if (! src.isNull())
                newPixel = buildPerlerImageFixed (src, cellSize, newCellsW, newCellsH);
        }
    }

    // 源图不可用：基于当前 pixelImage 再量化（作为降级方案，画质会有累积损失）
    if (newPixel.isNull())
        newPixel = buildPerlerImageFixed (img->pixelImage, cellSize, newCellsW, newCellsH);

    if (newPixel.isNull()) return;

    img->pixelImage = newPixel;
    img->cellsW     = newCellsW;
    img->cellsH     = newCellsH;

    // 不越出 canvas（缩放后可能溢出右/下边缘）
    const auto canvas = getCanvasArea();
    int nx = juce::jlimit (canvas.getX(), juce::jmax (canvas.getX(), canvas.getRight()  - newPixel.getWidth()),  img->topLeft.x);
    int ny = juce::jlimit (canvas.getY(), juce::jmax (canvas.getY(), canvas.getBottom() - newPixel.getHeight()), img->topLeft.y);
    img->topLeft = { nx, ny };
    syncPerlerLayerBounds (idx);   // pixelImage / topLeft 变动，同步 layer
}

// ----------------------------------------------------------
// ----------------------------------------------------------
// Bug5: Modify cellSize (clarity) -- lock the pixel size on screen,
//   only change the size of each perler block (thus changing cell count).
//   oldSize  = cellsW_old * cellSize_old x cellsH_old * cellSize_old
//   newCells = round(screenW / newCellSize), nearest multiple
//   final screen size may deviate +/-(newCellSize-1)px, visually negligible.
//   restore topLeft anchored to image center -> "size unchanged, pixels finer/coarser".
// ----------------------------------------------------------
void ModuleWorkspace::rebuildPerlerImageCellSize (int idx, int newCellSize)
{
    if (idx < 0 || idx >= perlerImages.size()) return;
    auto* img = perlerImages.getUnchecked (idx);
    if (img == nullptr) return;

    newCellSize = juce::jlimit (minCellSize, maxCellSize, newCellSize);
    if (newCellSize == img->cellSize) return;

    // Key: lock screen pixel size (keep image size unchanged as user expects)
    const int screenW = juce::jmax (1, img->cellsW) * juce::jmax (1, img->cellSize);
    const int screenH = juce::jmax (1, img->cellsH) * juce::jmax (1, img->cellSize);

    // Compute new cell count by new cellSize (round to nearest)
    int newCellsW = juce::jmax (1, (int) std::round ((double) screenW / (double) newCellSize));
    int newCellsH = juce::jmax (1, (int) std::round ((double) screenH / (double) newCellSize));

    // Bug2: hard upper bound -- avoid freeze from excessive cells (max 400 per side)
    const int hardMax = 400;
    if (newCellsW > hardMax || newCellsH > hardMax)
    {
        const double sx = (double) hardMax / juce::jmax (1, newCellsW);
        const double sy = (double) hardMax / juce::jmax (1, newCellsH);
        const double s  = juce::jmin (sx, sy);
        newCellsW = juce::jmax (1, (int) std::floor (newCellsW * s));
        newCellsH = juce::jmax (1, (int) std::floor (newCellsH * s));
    }

    juce::Image newPixel;

    // Prefer requantize from source image (best quality)
    if (img->sourcePath.isNotEmpty())
    {
        const juce::File f (img->sourcePath);
        if (f.existsAsFile())
        {
            const auto src = juce::ImageFileFormat::loadFrom (f);
            if (! src.isNull())
                newPixel = buildPerlerImageFixed (src, newCellSize, newCellsW, newCellsH);
        }
    }

    // Source unavailable: requantize from current pixelImage (fallback)
    if (newPixel.isNull())
        newPixel = buildPerlerImageFixed (img->pixelImage, newCellSize, newCellsW, newCellsH);

    if (newPixel.isNull()) return;

    // Anchor to original image center -> visually keep size unchanged
    const auto oldCenter = img->getBounds().getCentre();

    img->pixelImage = newPixel;
    img->cellsW     = newCellsW;
    img->cellsH     = newCellsH;
    img->cellSize   = newCellSize;

    int nx = oldCenter.x - newPixel.getWidth()  / 2;
    int ny = oldCenter.y - newPixel.getHeight() / 2;

    // Clamp to canvas
    const auto canvas = getCanvasArea();
    nx = juce::jlimit (canvas.getX(), juce::jmax (canvas.getX(), canvas.getRight()  - newPixel.getWidth()),  nx);
    ny = juce::jlimit (canvas.getY(), juce::jmax (canvas.getY(), canvas.getBottom() - newPixel.getHeight()), ny);
    img->topLeft = { nx, ny };
    syncPerlerLayerBounds (idx);   // cellSize 更改后 pixelImage 尺寸和 topLeft 都变，同步 layer
}

// ----------------------------------------------------------
// 删除聚焦图片
// ----------------------------------------------------------
void ModuleWorkspace::deleteFocusedPerlerImage()
{
    if (focusedPerlerIdx < 0 || focusedPerlerIdx >= perlerImages.size()) return;

    // Bug1：脏区必须同时覆盖图片本体 + 8 个缩放手柄 + 左侧 cellSize 滑条
    //       + 图片下方的 PerlerBeads 复选框行
    //       （滑条几何位于图片左外部约 20~60px，原来仅 repaint(bounds) 不够大，
    //        导致图片删除后滑条/数字 label / 复选框的像素残留在 canvas 上）
    const auto imgBounds   = perlerImages.getUnchecked (focusedPerlerIdx)->getBounds();
    const auto sliderRect  = getCellSizeSliderBounds (imgBounds);
    const auto checkRect   = getPerlerBeadsCheckboxBounds (imgBounds);
    // 滑条左侧还有 "Npx" 数字 label（见 paintOverChildren：labelX = slider.getX() - 36 - 2，
    // 宽 36），这里直接把左扩量开大到 ~64px 足以覆盖 label + 预期拇指
    auto dirty = imgBounds.expanded (handleSize + 4);
    if (! sliderRect.isEmpty())
        dirty = dirty.getUnion (sliderRect.expanded (64, 4));
    if (! checkRect.isEmpty())
        dirty = dirty.getUnion (checkRect.expanded (4));

    // 同步删除对应的 layer 子组件
    //   · 先删 layer（OwnedArray 会自动 delete Component，也会从 workspace
    //     的子组件列表移除），再删数据结构，避免 layer->img 的悬垂指针
    perlerLayers.remove (focusedPerlerIdx);
    perlerImages.remove (focusedPerlerIdx);
    focusedPerlerIdx    = -1;
    draggingPerlerIdx   = -1;
    resizingPerlerIdx   = -1;
    cellSizeDraggingIdx = -1;
    pendingCellSize     = -1;
    activeResizeHandle  = ResizeHandle::none;
    deleteBtnHovered    = false;
    deleteBtnPressed    = false;
    repaint (dirty);
    notifyLayoutChanged();
}

// ----------------------------------------------------------
// 键盘：Delete 删除聚焦图片
// ----------------------------------------------------------
bool ModuleWorkspace::keyPressed (const juce::KeyPress& key)
{
    if (focusedPerlerIdx >= 0
        && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey))
    {
        deleteFocusedPerlerImage();
        return true;
    }
    return juce::Component::keyPressed (key);
}

void ModuleWorkspace::updatePerlerCursorFor (juce::Point<int> /*canvasPos*/)
{
    // 保留占位：未来可根据命中的 handle 设置 LeftRight/UpDown/Diagonal 光标
}
