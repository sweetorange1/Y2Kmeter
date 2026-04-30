#include "PluginEditor.h"
#include <JuceHeader.h>
#include "BinaryData.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/modules/EqModule.h"
#include "source/ui/modules/LoudnessModule.h"
#include "source/ui/modules/OscilloscopeModule.h"
#include "source/ui/modules/SpectrumModule.h"
#include "source/ui/modules/PhaseModule.h"
#include "source/ui/modules/DynamicsModule.h"
#include "source/ui/modules/FineSplitModules.h"
#include "source/ui/modules/WaveformModule.h"
#include "source/ui/modules/SpectrogramModule.h"
#include "source/ui/modules/TamagotchiModule.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// FpsFrameListener——嵌套实现（封装在 .cpp 以绕开 AnalyserHub 完整头
//   在 PluginEditor.h 里暴露时导致的 MSVC include guard 串扰问题）
// ==========================================================
class Y2KmeterAudioProcessorEditor::FpsFrameListener : public AnalyserHub::FrameListener
{
public:
    explicit FpsFrameListener (Y2KmeterAudioProcessorEditor& ownerRef) : owner (ownerRef) {}

    void onFrame (const AnalyserHub::FrameSnapshot& /*frame*/) override
    {
        // UI 线程回调：仅原子计数器 +1（轻量、不做其它处理）
        owner.frameCounter.fetch_add (1, std::memory_order_relaxed);
    }

private:
    Y2KmeterAudioProcessorEditor& owner;
};

// ==========================================================
// ChromeHiddenOverlay —— chrome 隐藏态的"纯视觉"浮层
//
// 职责：
//   · 底图上打印软件名 / 版本号 / 官网链接（低对比度）
//   · 右上角半透明关闭按钮（视觉）
//
// 关键设计：
//   · 整体宽度与 Editor 同宽、高度 = titleBarHeight。
//   · z-order 最底层（在 workspace 之下）→ 模块可以自然遮挡 overlay 的文字和按钮。
//   · 本组件不处理任何鼠标事件：setInterceptsMouseClicks(false, false)。
//     顶部按钮的点击由 Editor 在 mouseMove/Down/Up 中统一处理（workspace 在对应
//     矩形区域做 hit-test 挖洞，让事件冒泡到 Editor）。
//   · 视觉状态（closeButtonHovered / titleTextHovered 等）由 Editor 在鼠标事件中
//     通过 setCloseButtonHovered / setTitleTextHovered 下发。
// ==========================================================
class Y2KmeterAudioProcessorEditor::ChromeHiddenOverlay : public juce::Component
{
public:
    explicit ChromeHiddenOverlay (Y2KmeterAudioProcessorEditor& ownerRef)
        : owner (ownerRef)
    {
        // 纯视觉层：完全不拦截鼠标事件（事件全部透传到下方的 workspace / Editor）
        setInterceptsMouseClicks (false, false);

        // 预先计算标题文字像素宽度，这样 Editor 首次调用 updateWorkspaceHitTestHoles
        //   就能拿到精确的文字矩形，而不用等第一次 paint 写入。
        //   （与 paint() 里完全相同的字体/文本/间距计算）
        const juce::Font nameFont    = PinkXP::getFont (12.0f, juce::Font::bold);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);
        const int nameW    = juce::GlyphArrangement::getStringWidthInt (nameFont, "Y2Kmeter");
        const int versionW = juce::GlyphArrangement::getStringWidthInt (versionFont, "v1.5.0");
        const int urlW     = juce::GlyphArrangement::getStringWidthInt (urlFont, "iisaacbeats.cn");
        constexpr int gap1 = 6;
        constexpr int gap2 = 10;
        cachedTitleTextW = nameW + gap1 + versionW + gap2 + urlW;
    }

    // 由 Editor 更新视觉状态（hover/pressed）
    void setCloseButtonHovered (bool h)
    {
        if (h != closeButtonHovered) { closeButtonHovered = h; repaint (getFloatingCloseButtonRect()); }
    }
    void setCloseButtonPressed (bool p)
    {
        if (p != closeButtonPressed) { closeButtonPressed = p; repaint (getFloatingCloseButtonRect()); }
    }
    void setTitleTextHovered (bool h)
    {
        if (h != titleTextHovered) { titleTextHovered = h; repaint(); }
    }

    // 几何（Editor 需要用来做 hit-test 与冒泡处理）
    juce::Rectangle<int> getFloatingCloseButtonRect() const
    {
        constexpr int margin = 4;
        constexpr int size   = 18; // 与 Editor::closeButtonSize 保持一致
        return { getWidth() - margin - size, margin, size, size };
    }

    // 标题文字整体命中矩形（宽度取实际像素宽度，由 paint 写入）
    juce::Rectangle<int> getTitleTextHitRect() const
    {
        const int x = 28;
        const int w = juce::jmax (0, cachedTitleTextW);
        return { x, 0, w, juce::jmin (26, getHeight()) };
    }

    void paint (juce::Graphics& g) override
    {
        // ------- 1) 顶部抬头文字：软件名 + 版本号 + 官网（低对比度，贴在底图上）-------
        const juce::String nameText    = "Y2Kmeter";
const juce::String versionText = "v1.6";
        const juce::String urlText     = "iisaacbeats.cn";

        const juce::Font nameFont    = PinkXP::getFont (12.0f, juce::Font::bold);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);

        const int nameW    = juce::GlyphArrangement::getStringWidthInt (nameFont, nameText);
        const int versionW = juce::GlyphArrangement::getStringWidthInt (versionFont, versionText);
        const int urlW     = juce::GlyphArrangement::getStringWidthInt (urlFont, urlText);

        constexpr int gap1 = 6;
        constexpr int gap2 = 10;

        const int x0 = 28;
        const int y  = 0;
        const int h  = juce::jmin (26, getHeight());

        // chrome 隐藏态：文字淡淡贴在底图上，hover 时略亮并加下划线
        const float textAlpha = titleTextHovered ? 0.95f : 0.55f;

        // 文字描边（1px 阴影），帮助文字在任何底图上都可读
        g.setColour (juce::Colours::white.withAlpha (textAlpha * 0.6f));
        g.setFont (nameFont);
        g.drawText (nameText, x0 + 1, y + 1, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1 + 1, y + 1, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2 + 1, y + 1, urlW, h, juce::Justification::centredLeft, false);

        // 主文字（墨色）
        g.setColour (PinkXP::ink.withAlpha (textAlpha));
        g.setFont (nameFont);
        g.drawText (nameText, x0, y, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1, y, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2, y, urlW, h, juce::Justification::centredLeft, false);

        if (titleTextHovered)
        {
            const int lineY = y + h - 3;
            const int totalW = nameW + gap1 + versionW + gap2 + urlW;
            g.setColour (PinkXP::ink.withAlpha (0.95f));
            g.fillRect (x0, lineY, totalW, 1);
        }

        // 缓存实际绘制宽度，供 Editor 做精确命中测试
        cachedTitleTextW = nameW + gap1 + versionW + gap2 + urlW;

        // ------- 2) 右上角浮动关闭按钮（15% / 100% 两档透明度）-------
        juce::Graphics::ScopedSaveState save (g);
        g.setOpacity (closeButtonHovered ? 1.0f : 0.15f);

        auto cb = getFloatingCloseButtonRect();
        if (closeButtonPressed)
            PinkXP::drawPressed (g, cb, PinkXP::pink100);
        else
            PinkXP::drawRaised  (g, cb, closeButtonHovered ? PinkXP::pink200 : PinkXP::btnFace);

        g.setColour (PinkXP::ink);
        g.setFont   (PinkXP::getFont (12.0f, juce::Font::bold));
        auto cbText = cb;
        cbText.translate (-1, -1);
        if (closeButtonPressed) cbText.translate (1, 1);
        g.drawText ("x", cbText, juce::Justification::centred, false);
    }

    int getCachedTitleTextWidth() const noexcept { return cachedTitleTextW; }

private:
    Y2KmeterAudioProcessorEditor& owner;
    bool closeButtonHovered = false;
    bool closeButtonPressed = false;
    bool titleTextHovered   = false;
    mutable int cachedTitleTextW = 0;
};

// ==========================================================
// Y2KmeterAudioProcessorEditor —— 多模块框架外壳
// 窗口：960 × 640（Pink XP 桌面 + 凸起主窗口 + 粉色标题栏）
// 中部放置 ModuleWorkspace，所有分析模块都作为子模块存在
// ==========================================================

juce::Font Y2KmeterAudioProcessorEditor::getCustomFont(float height, int styleFlags)
{
    return PinkXP::getFont(height, styleFlags);
}

Y2KmeterAudioProcessorEditor::Y2KmeterAudioProcessorEditor(Y2KmeterAudioProcessor& p)
    : juce::AudioProcessorEditor(&p),
      processor(p),
      // 根据 AudioProcessor::wrapperType 一次性判定是否插件宿主模式。
      // JUCE 在 AudioProcessor 构造时会把本次加载的 wrapper 类型填好
      // （VST3 / AU / AAX / LV2 / Standalone / Undefined 等），对于
      // Standalone 以外的一切情况我们都视为"插件宿主模式"。
      isPluginHost (p.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
{
    // 0) 先加载 typeface / logo 到实例成员（避免 static 跨 DLL 卸载导致崩溃）
    customTypeface = PinkXP::loadActiveTypeface();
    logoImage = juce::ImageFileFormat::loadFrom(BinaryData::logo_png,
                                                 (size_t) BinaryData::logo_pngSize);

    initLookAndFeel();

    // 1) 先创建 ChromeHiddenOverlay 并以 invisible 状态加入 —— 让它成为"最底层"子组件
    //    （后续 workspace / 其它组件 addAndMakeVisible 时都会排到它之上）。
    //    这样 chrome 隐藏态下，overlay 的抬头文字和浮动关闭按钮会被模块自然遮挡。
    chromeHiddenOverlay = std::make_unique<ChromeHiddenOverlay> (*this);
    addChildComponent (*chromeHiddenOverlay);

    // 2) 创建 workspace 并挂到编辑器上（z-order 高于 overlay）
    workspace = std::make_unique<ModuleWorkspace>();
    workspace->setModuleFactory([this](ModuleType t) -> std::unique_ptr<ModulePanel>
    {
        return createModule(t);
    });
    workspace->setAvailableModuleTypes({
        ModuleType::eq,
        ModuleType::loudness,
        ModuleType::lufsRealtime,
        ModuleType::truePeak,

        // 模拟 VU 指针表（复用 Loudness 路的 RMS L/R，后端零新增计算）
        ModuleType::vuMeter,

        ModuleType::oscilloscope,
        ModuleType::oscilloscopeLeft,
        ModuleType::oscilloscopeRight,

        ModuleType::spectrum,

        ModuleType::phase,
        ModuleType::phaseCorrelation,
        ModuleType::phaseBalance,

        ModuleType::dynamics,
        ModuleType::dynamicsMeters,
        ModuleType::dynamicsDr,
        ModuleType::dynamicsCrest,

        // 持续滚动瀑布波形（复用 Oscilloscope 原始样本，后端零新增计算）
        ModuleType::waveform,

        // 实时频谱瀑布图（复用 Spectrum 路的 FFT 幅度，后端零新增计算）
        ModuleType::spectrogram,

        // 独立小宠物模块（右键/双击空白区添加）
        ModuleType::tamagotchi
    });

    addAndMakeVisible(*workspace);

    // 2) 先给编辑器设尺寸 —— 触发 resized() 让 workspace 拿到实际 bounds
    //    这样后续 addModuleByType 走 findNextSlot 时 canvas 不再是 0x0
    //
    //   尺寸上限分模式：
    //     · Standalone：1600×1100 —— 默认预设下的合理上限；切换到"横向铺满"
    //       类预设时 applyLayoutPreset 会临时把上限抬到屏幕宽度，预设回 1 时
    //       会再夹回 1600×1100，避免用户手动拖得过大错位。
    //     · VST3 / AU 插件宿主：8192×8192 —— 远大于常规显示器分辨率。插件模式
    //       下布局预设下拉被隐藏（不会再有地方动态抬高上限），如果仍用 1600×1100
    //       会被 JUCE 的 ComponentBoundsConstrainer 死死夹住；用户在 DAW 里
    //       拉宽窗口到 1600 就拉不动了。给一个足够大的上限让宿主自由 resize。
    //
    //   尺寸下限同样分模式：
    //     · Standalone：640×420 —— 保证伪标题栏 + 底部 toolbar + 至少一行模块
    //       能完整显示，避免用户误缩到按钮挤压错位。
    //     · VST3 / AU 插件宿主：320×240 —— 插件模式下不画伪标题栏（chrome 由宿主
    //       提供），底部 toolbar 里大量 UI 也被隐藏（Source 下拉、布局预设下拉等），
    //       实际需要的最小可用宽度远小于 Standalone；若仍沿用 640×420，用户在 DAW
    //       里就会"缩到一半缩不动了"。给到 320×240 让宿主充分自由。
    setResizable(true, true);
    if (isPluginHost)
        setResizeLimits(50, 50, 8192, 8192);
    else
        setResizeLimits(640, 420, 1600, 1100);
    setSize(960, 640);

    // 3) 再装载默认/已保存模块
    loadInitialModules();

    // 4) 用户布局变更 → 立即写回 Processor state
    workspace->onLayoutChanged = [this]()
    {
        processor.setSavedLayoutXml(workspace->saveLayoutAsXml());
    };

    // 4.05) 布局预设切换：Preset 1 = 默认布局 + 默认窗口大小；
    //       Preset 2 = 把顶层窗口宽度拉到当前屏幕宽，并让默认模块横向等分 canvas。
    //       实现细节：
    //         · 先清空 workspace 的所有现有模块 / 拼豆贴画；
    //         · Preset 1：恢复 Editor 尺寸到 960×640，然后调 loadInitialModules()
    //           的"默认分支"（强制传空 XML 以绕过恢复路径）；
    //         · Preset 2：取当前所在屏幕 userArea 的宽度，调整顶层窗口宽度到该值，
    //           顶层窗口 x 移到屏幕左边；workspace 的 canvas 拿到后按横向等分
    //           7 个默认模块。
    //         · 切换后手动 notifyLayoutChanged 以把新布局回写 Processor。
    workspace->onLayoutPresetChanged = [this](ModuleWorkspace::LayoutPreset preset)
    {
        applyLayoutPreset ((int) preset);
    };

    // 4.05b) 预设 Save/Load —— 仅做透传：workspace 已经弹完 FileChooser，
    //        Editor 没有 settings 文件的写入能力（PropertiesFile 归 Standalone App
    //        持有），所以把 File 原样转发给外层订阅者（Y2KStandaloneApp）。
    //        外层未订阅时 onSave/LoadSettingsRequested 为空回调 → 点击按钮无效
    //        但不崩溃（VST3/AU 插件模式下即使按钮被误显也安全）。
    workspace->onSavePresetRequested = [this](juce::File dest)
    {
        if (onSaveSettingsRequested) onSaveSettingsRequested (dest);
    };
    workspace->onLoadPresetRequested = [this](juce::File src)
    {
        if (onLoadSettingsRequested) onLoadSettingsRequested (src);
    };

    // 4.1) chrome 可见性变化 → 隐藏/显示顶部 TitleBar
    //     切换时需要 resized() 重新布局（隐藏后 workspace 应占满整个窗口），
    //     并整体 repaint 而不仅仅 repaint 标题栏矩形。
    //
    //     注：workspace 在 chrome 可见时从 y=titleBarHeight 开始，chrome 隐藏时从 y=0 开始，
    //     自身位置有 titleBarHeight 的差值。为了让用户看到的模块视觉位置完全不变，
    //     我们在切换瞬间对所有模块的相对 y 坐标做反向补偿平移：
    //       · 切到 hide（workspace 上移 26 px）→ 模块 y += 26，屏幕位置不变
    //       · 切回 show（workspace 下移 26 px）→ 模块 y -= 26，屏幕位置还原
    workspace->onChromeVisibleChanged = [this](bool visible)
    {
        const bool toHide = ! visible;           // 本次切换是"进入隐藏态"吗？
        chromeDim = toHide;

        // 切换态时重置 hover/pressed，避免残留视觉
        closeButtonHovered = closeButtonPressed = false;
        pinButtonHovered   = pinButtonPressed   = false;
        minButtonHovered   = minButtonPressed   = false;

        // 平移所有模块相对 workspace 的 y 坐标，保持"屏幕绝对位置"完全不变
        //   · 向隐藏态切换：workspace 的 top 从 titleBarHeight → 0，模块需要 +titleBarHeight
        //   · 向显示态切换：workspace 的 top 从 0 → titleBarHeight，模块需要 -titleBarHeight
        if (workspace != nullptr)
        {
            const int dy = toHide ? titleBarHeight : -titleBarHeight;
            const int n = workspace->getNumModules();
            for (int i = 0; i < n; ++i)
            {
                if (auto* m = workspace->getModule (i))
                {
                    auto b = m->getBounds();
                    b.translate (0, dy);
                    m->setBounds (b);
                }
            }
            // Bug3：对画布上的"拼豆贴画"做同样的 y 补偿，保持视觉位置不变
            workspace->shiftAllPerlerImagesY (dy);
        }

        // 切换 overlay 可见性：仅在 hide 态下显示抬头文字 + 浮动按钮
        if (chromeHiddenOverlay != nullptr)
            chromeHiddenOverlay->setVisible (toHide);

        resized();
        repaint();

        // 切换后同步 workspace 的 hit-test 挖洞：让 overlay 按钮/文字区的鼠标事件
        //   冒泡到 Editor，从而 Editor 能直接接管浮动按钮的 hover/press 逻辑，
        //   同时任何压在按钮之上的模块都能正常独占鼠标事件（JUCE 先派发给子组件）。
        updateWorkspaceHitTestHoles();
    };

    // 4.2) 音频源下拉变化 → 透传给外部（Standalone App 订阅后会真正切换音频设备）
    workspace->onAudioSourceChanged = [this](const juce::String& sourceId, bool isLoopback)
    {
        if (onAudioSourceChanged) onAudioSourceChanged (sourceId, isLoopback);
    };

    // 4.3) FPS 限制按钮切换 → 修改 AnalyserHub 的 FrameDispatcher 频率
    workspace->onFpsLimitChanged = [this](int hz)
    {
        userRequestedFpsLimit = juce::jlimit (15, 120, hz);
        adaptiveDispatchHz    = isPluginHost ? juce::jmin (48, userRequestedFpsLimit)
                                             : userRequestedFpsLimit;
        adaptiveRecoverTicks  = 0;

        processor.getAnalyserHub().startFrameDispatcher (adaptiveDispatchHz);
        // 立即重置统计起点，避免切换后站显示跨区间的均值
        frameCounter.store (0, std::memory_order_relaxed);
        lastFrameCounterSample = 0;
        lastFpsTimeMs = juce::Time::getMillisecondCounterHiRes();
    };

    // 订阅主题变更 → 切主题后刷新全局重绘
    themeSubToken = PinkXP::subscribeThemeChanged([this]()
    {
        if (auto* lnf = dynamic_cast<juce::LookAndFeel_V4*>(&getLookAndFeel()))
            juce::ignoreUnused(lnf);
        repaint();
    });

    // 应用默认主题（会把全局配色重写一次，确保状态一致）
    PinkXP::applyTheme(PinkXP::getCurrentThemeId());

    // 构造阶段 isShowing() 可能仍为 false，先确保分析链启动；
    // 后续由 visibilityChanged() 继续做可见性联动。
    processor.setAnalysisActive(true);

    // Editor 自身 10Hz 轮询：拉 Processor 的 CPU 占比并广播到所有模块的
    //   右下角标签，同时计算实际 FPS 下发给 workspace 的 FPS 标签
    startTimerHz (10);

    // Phase F：启动全局 FrameDispatcher（默认 30Hz 统一滚 UI 分发、模块后续订阅）
    //   模块构造中的 retain() 已经让 refCounts 就绪，这里开 Timer 即可开始工作。
    userRequestedFpsLimit = juce::jlimit (15, 120, workspace->getFpsLimit());
    adaptiveDispatchHz    = isPluginHost ? juce::jmin (48, userRequestedFpsLimit)
                                         : userRequestedFpsLimit;
    adaptiveRecoverTicks  = 0;
    processor.getAnalyserHub().startFrameDispatcher (adaptiveDispatchHz);

    // 订阅帧分发，以计算实际 FPS（内部辅助类，避免 header 对 AnalyserHub 的硬依赖）
    fpsListener = std::make_unique<FpsFrameListener> (*this);
    processor.getAnalyserHub().addFrameListener (fpsListener.get());
    lastFpsTimeMs = juce::Time::getMillisecondCounterHiRes();

    // ------------------------------------------------------
    // 插件宿主（VST3 / AU / AAX / LV2）模式下的一次性差异化配置
    //   · 隐藏底部 Toolbar 中的"Source"下拉（宿主直接提供音频）
    //   · 不绘制自画伪标题栏 / 右上三按钮 / 窗口拖拽（由宿主窗口边框负责）
    //   · 不启用"默认置顶"行为（对宿主无意义，且多数宿主会忽略该调用）
    //   · 关闭 chromeHiddenOverlay（插件下永远不会进入 Hide 态的浮层）
    // 注：paint/resized/mouseXxx 中会再次基于 isPluginHost 做绕行，
    //     此处只是一次性的初始状态设置。
    // ------------------------------------------------------
    if (isPluginHost)
    {
        if (workspace != nullptr)
        {
            workspace->setAudioSourceUiVisible (false);
            // 布局预设下拉在插件模式下同样隐藏：
            //   切换预设会改顶层窗口尺寸 / 位置（横向铺满屏幕等），和宿主窗口会打架；
            //   插件下始终使用默认预设即可，无需用户选择。
            workspace->setLayoutPresetUiVisible (false);

            // 但 Save/Load 两个按钮仍然保留 —— 用户依然需要在 DAW 里导入/导出
            //   预设文件（与 Standalone Save 出的 .settings 互通）。按钮独立贴在
            //   Grid 左侧显示。
            workspace->setSaveLoadUiVisible (true);
        }

        // 插件模式下 Save/Load 不经过 Standalone App，直接由 Editor 就地处理：
        //   · Save：把 Processor::getStateInformation 的数据包装成与 Standalone
        //     完全兼容的 .settings 文件（<PROPERTIES> 外壳 + base64 编码的
        //     filterState 条目），Standalone 下一次打开该文件也能正常恢复。
        //   · Load：容错读取三种格式
        //       a) Standalone .settings —— 解析 <PROPERTIES> 里 filterState 条目，
        //          base64 解码 + copyXmlToBinary 反序列化为 MemoryBlock，再喂给
        //          Processor::setStateInformation；
        //       b) 裸 PBEQ_State XML（Processor state 原生 XML 形态）；
        //       c) 裸 PBEQ_Layout XML（仅布局，不含其它）—— 直接灌给 workspace。
        //     成功后调用 workspace->loadLayoutFromXml 热重载，无需重启宿主。
        //
        //   注：插件模式下"主题/窗口尺寸/音频源"等 runtime 状态由 DAW 或 Editor 自身
        //   管理，Load 过来的 .settings 里的那些 key 在这里被有意忽略；我们只抽取
        //   和 Processor state 相关的部分 —— 这与"预设 = 模块布局"的用户心智一致。
        onSaveSettingsRequested = [this](juce::File dest)
        {
            if (dest == juce::File{}) return;
            saveStateAsSettingsFile (dest);
        };
        onLoadSettingsRequested = [this](juce::File src)
        {
            if (src == juce::File{} || ! src.existsAsFile()) return;
            loadStateFromSettingsFile (src);
        };

        // 插件模式下不画伪标题栏 / chrome 浮层
        chromeHiddenOverlay.reset();

        // 插件下不主动置顶（让宿主完全掌控窗口行为）
        alwaysOnTopActive             = false;
        initialAlwaysOnTopApplied     = true; // 抑制 visibilityChanged 里的首次推送

        // 触发一次重排：workspace 在插件模式下从 y=0 起铺满整个 Editor
        resized();
    }

    // ==================================================================
    // GPU 合成层挂载（Standalone + VST3 共用此入口）
    //
    //   · attachTo(*this) 之后，本 Editor 及其所有子组件（workspace、各
    //     ModulePanel、chromeHiddenOverlay）的 paint() 命令都会被 JUCE 翻译
    //     成 OpenGL 批次在 GPU 上执行。软光栅路径（CoreGraphicsContext::
    //     fillCGRect / drawGlyphs / fillPath / strokePath）彻底绕开，主线程
    //     只剩 draw 命令组装与提交，CPU 负载大幅下降。
    //
    //   · setContinuousRepainting(false)：**关键**。默认为 true 时，GL 上下文
    //     会按屏幕刷新率不停重绘，等于强制 60~120Hz，不受我们 FrameDispatcher
    //     的 adaptiveDispatchHz 控制。设为 false 后只在子组件调用 repaint()
    //     时才触发一次 GL 重绘，和现有的节流/脏区策略完美配合。
    //
    //   · setComponentPaintingEnabled(true) 是默认值，显式写出来只是强调
    //     "让 GL 上下文负责普通 JUCE 组件的 paint"，不是手动 glDraw。
    //
    //   · 插件宿主（VST3/AU）场景：
    //       - macOS：JUCE 为 Editor 顶层 NSView 创建一个 NSOpenGLContext 子层，
    //         宿主窗口其余部分不受影响。ProTools/Logic/Cubase/Live 等主流 DAW 均支持。
    //       - Windows：JUCE 为 HWND 创建一个子 GL 子窗口，同理。
    //     极少数老版 ProTools/AU-Hosts 对子 GL 上下文兼容性差；若将来发现某宿主
    //     花屏/黑屏，可在此处加 isPluginHost 保护把插件模式下回退到软光栅。
    //     当前按"两种模式都走 GPU"的需求开启。
    //
    //   · 析构顺序：openGLContext 声明在 Editor 类末尾 → 成员反向析构会最先析构
    //     GL 上下文（JUCE 会自动 detach），但为了防止 GL 资源释放时 child
    //     components 已被部分销毁的 UB，~Editor 起始处仍显式 detach() 一次兜底。
    // ==================================================================
    openGLContext.setContinuousRepainting (false);
    openGLContext.setComponentPaintingEnabled (true);
    openGLContext.attachTo (*this);
}

Y2KmeterAudioProcessorEditor::~Y2KmeterAudioProcessorEditor()
{
    // -1) 最先 detach GPU 上下文，确保随后 workspace / chromeHiddenOverlay 等
    //     子组件析构时，GL 资源（Texture/VBO/FBO）已经从上下文解绑。
    //     若依赖成员反向析构顺序隐式 detach，会触发"GL 资源释放时子组件已部分销毁"的
    //     未定义行为（JUCE GL context 在析构里会 flush 队列，需要组件树依然完整）。
    openGLContext.detach();

    // 0) 先停掉 Editor 自身的 timer，避免 workspace.reset() 中途被调
    stopTimer();

    // 兜底：若曾为 Tamagotchi 临时保活 Loudness，析构前配平 release。
    if (tamagotchiSignalRetained)
    {
        processor.getAnalyserHub().release (AnalyserHub::Kind::Loudness);
        tamagotchiSignalRetained = false;
    }

    // 取消订阅帧分发（在 stopFrameDispatcher 之前取消，更严谨）
    if (fpsListener != nullptr)
    {
        processor.getAnalyserHub().removeFrameListener (fpsListener.get());
    }

    // Phase F：停掉 FrameDispatcher，避免 workspace/模块离场后
    //   还有 UI Timer 回调到已析构的 FrameListener。
    processor.getAnalyserHub().stopFrameDispatcher();

    if (themeSubToken != 0)
    {
        PinkXP::unsubscribeThemeChanged(themeSubToken);
        themeSubToken = 0;
    }

    // 1) 释放 workspace（内部会停止所有 Timer、清空子组件）
    //    必须在清理 LookAndFeel / Typeface 之前完成
    workspace.reset();

    // Editor 销毁后关闭分析，避免后台继续进行无意义计算
    processor.setAnalysisActive(false);

    // 2) 解绑 LookAndFeel（本组件 + 自身）
    setLookAndFeel(nullptr);

    // 3) 关键：清空全局 Typeface 缓存，防止插件 DLL 卸载时
    //    BinaryData 内存已被释放而全局 gTypeface 仍持有悬垂引用导致宿主卡死
    PinkXP::initCustomTypeface(nullptr);

    // 4) 清空 ImageCache（保险；我们自己不用它，但有些 JUCE 默认路径可能写入）
    juce::ImageCache::releaseUnusedImages();

    // 5) 实例成员 customTypeface / logoImage 会在随后自动析构，
    //    此时已没有任何 LookAndFeel / Font / Image 引用 BinaryData，安全
}

// ----------------------------------------------------------
// 初始化
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::initLookAndFeel()
{
    // 把字体注入全局 PinkXP，供所有模块使用
    PinkXP::initCustomTypeface(customTypeface);
    setLookAndFeel(&getPinkXPLookAndFeel());
}

void Y2KmeterAudioProcessorEditor::initWorkspace()
{
    // 已合并到构造器 + loadInitialModules()。保留空实现以兼容旧声明（若 header 仍声明）
    loadInitialModules();
}

void Y2KmeterAudioProcessorEditor::loadInitialModules()
{
    // 1) 优先恢复已保存布局
    const auto savedXml = processor.getSavedLayoutXml();
    if (savedXml.isNotEmpty() && workspace->loadLayoutFromXml(savedXml))
        return;

    // 2) 默认状态：首次打开时按默认层叠瀑布填充七个核心模块
    seedDefaultModules();
}

// ----------------------------------------------------------
// 按默认层叠瀑布布局加载七个默认模块（eq / loudness / oscilloscope /
// spectrum / phase / dynamics / waveform）。
//   · 调用方需先清空 workspace（首次启动由构造器保证）
//   · 每个模块相对前一个偏移 (stepX, stepY)，形成老式"弹出大量窗口"的层叠感
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::seedDefaultModules()
{
    static const ModuleType defaultOrder[] = {
        ModuleType::eq,
        ModuleType::loudness,
        ModuleType::oscilloscope,
        ModuleType::spectrum,
        ModuleType::phase,
        ModuleType::dynamics,
        ModuleType::waveform
    };

    // workspace 的 canvas 原点（此时 setSize 已触发 resized，canvas 有效）
    const auto canvas = workspace->getLocalBounds();

    // 瀑布布局的行列偏移 / 起点（与模块默认大小无关，保持固定）
    constexpr int stepX          = 28;
    constexpr int stepY          = 28;
    constexpr int startX         = 16;
    constexpr int startY         = 16;

    // 当瀑布到达右/下边缘时，换一列重新从顶部开始（像 XP 那样）
    int x = startX;
    int y = startY;
    int column = 0;

    for (auto type : defaultOrder)
    {
        auto panel = createModule(type);
        if (panel == nullptr) continue;

        // 尺寸：优先使用每个模块自己声明的"默认大小"（setDefaultSize），
        //   再用 minW/minH 做下限保护；这样 EQ / Loudness / Spectrum 等
        //   各自拿到 384×256 / 320×256 / 384×256 …而不是一刀切 340×240。
        const int w = juce::jmax(panel->getMinWidth(),  panel->getDefaultWidth());
        const int h = juce::jmax(panel->getMinHeight(), panel->getDefaultHeight());

        // 如果越界，换到下一列的顶部
        if (x + w > canvas.getRight() - 8 || y + h > canvas.getBottom() - 8)
        {
            ++column;
            x = startX + column * (stepX * 4);
            y = startY;
            // 还是越界就截取到边缘以内
            if (x + w > canvas.getRight() - 8)
                x = juce::jmax(0, canvas.getRight()  - w - 8);
            if (y + h > canvas.getBottom() - 8)
                y = juce::jmax(0, canvas.getBottom() - h - 8);
        }

        panel->setBounds(x, y, w, h);

        // autoPosition=false 让 workspace 保留我们设定的 bounds
        workspace->addModule(std::move(panel), /*autoPosition*/ false);

        x += stepX;
        y += stepY;
    }
}

// ----------------------------------------------------------
// 应用布局预设（由 ModuleWorkspace 的布局下拉框触发）
//   · presetId = 1 (defaultGrid)    → 恢复默认布局 + 默认窗口大小 960×640
//   · presetId = 2 (horizontalFull) → 拉伸顶层窗口到当前屏幕宽，默认模块横向
//                                      等分 canvas，高度撑满 canvas
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::applyLayoutPreset (int presetId)
{
    if (workspace == nullptr) return;

    // 先清空现有模块 / 拼豆贴画（clearAllModules 不触发 onLayoutChanged）
    workspace->clearAllModules();

    if (presetId == 1)
    {
        // Preset 1: 默认布局 + 默认窗口 960×640
        //   · 通过 setSize 触发 resized，workspace 会拿到对应的 canvas 尺寸后
        //     seedDefaultModules 才能正确计算默认瀑布位置。
        //   · 如果当前已经是 960×640（用户刚刚打开），setSize 不会有副作用。
        setSize (960, 640);
        seedDefaultModules();
    }
    else if (presetId == 2 || presetId == 3)
    {
        // Preset 2 / 3: 横向铺满屏幕宽度（高 250px）
        //   · Preset 2 = 贴屏幕顶部 (Y = userArea.getY())
        //   · Preset 3 = 贴屏幕底部 (Y = userArea.getBottom() - targetH)
        //
        //   其余逻辑（宽度取屏幕 userArea 宽、固定高 250、横向等分 7 个默认模块、
        //   放开 resizeLimits）完全相同。之前 Preset 2 累积 *2/3 导致高度不断缩水
        //   的问题已在上一轮改成常量修复。
        //
        //   Y 必须基于 userArea 而不是 top->getY()：Editor 被用户拖到副屏后，
        //   top->getY() 是跨屏绝对坐标（可能是 1440 或负数），而要的是"当前屏的
        //   顶/底边在屏幕坐标中的 Y"——这正是 userArea.getY() / getBottom()。
        const bool bottomAligned = (presetId == 3);

        auto* top = getTopLevelComponent();
        if (top == nullptr) top = this;

        const auto display = juce::Desktop::getInstance()
                                 .getDisplays()
                                 .getDisplayForRect (top->getScreenBounds());
        const auto userArea = (display != nullptr) ? display->userArea
                                                   : juce::Rectangle<int> (1280, 720);
        const int screenW = userArea.getWidth();

        // ------------------------------------------------------------
        // 目标高度自动对齐到 8px 网格整数倍 canvas：
        //   期望高度 kHorizontalStripHeight = 250，但 Editor 内 = Y2K 标题栏(26) +
        //   workspace；workspace 内 = canvas + 底部 toolbar(36)。因此：
        //       canvas.height = targetH - 26 - 36 = targetH - 62
        //   250 - 62 = 188，188 % 8 = 4 → canvas 不是整格，7 个等分模块
        //   floor 到 8 倍数后底部会留 4px 空白。
        //
        //   这里不想硬编码"26+36"这类依赖 ModuleWorkspace 内部常量的数值，
        //   改为"先试探一次布局，读取实际 canvas 高度，反推 overheadH"——
        //   这样无论未来 chrome/toolbar 高度怎么变都能自洽。
        //     1) 先按期望高度 setSize，触发 resized() → workspace 拿到 canvas
        //     2) overheadH = targetH - canvas.getHeight()（Editor 里非 canvas 的部分）
        //     3) 调整 targetH 使 (targetH - overheadH) 是 8 的倍数，并尽量靠近 250
        // ------------------------------------------------------------
        constexpr int kHorizontalStripHeight = 250;
        int targetH = kHorizontalStripHeight;

        // 关键前置：先把 resizeLimits 放到一个足够宽松的区间，确保后面的
        //   试探 setSize(screenW, 250) 不会被夹回（Editor 默认
        //   setResizeLimits(640, 420, 1600, 1100)：高度下限 420、宽度上限 1600）。
        //   如果试探被夹，probeCanvasH 会变成夹后 Editor 高度对应的 canvas，
        //   反推出来的 overheadH 是错的 → 最终 canvas 高度不是 8 的倍数 →
        //   模块 floor 后底部留 4~6px 空白。
        //   · 下限给一个极小值（例如 kHorizontalStripHeight 本身 250），
        //     上限至少覆盖 screenW × 1200（单屏宽 × 超出极限的高度）。
        if (auto* cbc0 = getConstrainer())
        {
            setResizeLimits (juce::jmin (cbc0->getMinimumWidth(),  screenW),
                             juce::jmin (cbc0->getMinimumHeight(), kHorizontalStripHeight),
                             juce::jmax (cbc0->getMaximumWidth(),  screenW),
                             juce::jmax (cbc0->getMaximumHeight(), 1200));
        }

        // 第 1 步：试探布局，让 workspace 计算出当前 chrome 状态下的 canvas 高。
        //   · Standalone 模式（top != this）：直接对顶层窗口 setBounds，
        //     这样 probe 反推的 overhead 天然包含"ResizableBorder 4px 上下边框"
        //     （setResizable(true, false) 下 DocumentWindow 会给 content 挖上下各 4px，
        //     setBoundsInset 压缩 Editor 高度 8px）。若 probe 只对 Editor setSize，
        //     反推的 overheadH 少算这 8px，最终 top->setBounds 走的才是"扣边框"路径，
        //     真实 canvas 比预期少 8px → 模块底部出现 8px 空白。
        //   · 插件模式（top == this）：无法搬动顶层窗口，直接 setSize。
        if (top == this)
        {
            setSize (screenW, targetH);
        }
        else
        {
            top->setBounds (userArea.getX(), userArea.getY(), screenW, targetH);
        }

        // 第 2 步：根据试探结果反推"顶层窗口高度里不属于 canvas 的那部分"。
        //   probeContainerH 是 probe 时真正被我们"占用"的高度：
        //     · standalone：top 的 height（=250，不被 border 吃掉，因为 border 从
        //       top 高度里内扣分给 Editor）
        //     · 插件：Editor 自身 height（=250）
        const int probeContainerH = (top == this) ? getHeight() : top->getHeight();
        const int probeCanvasH    = workspace->getCanvasArea().getHeight();
        const int overheadH       = probeContainerH - probeCanvasH;

        // 第 3 步：取最接近 250-overheadH 的"8 的倍数"作为期望 canvas 高，
        //          再加回 overheadH 得到对齐后的 targetH
        constexpr int kGridForH = 8;
        const int desiredCanvasRaw = kHorizontalStripHeight - overheadH;
        const int desiredCanvasH   = juce::jmax (kGridForH,
                                                 ((desiredCanvasRaw + kGridForH / 2) / kGridForH) * kGridForH);
        targetH = desiredCanvasH + overheadH;

        // 第 4 步：按最终 targetH 锁定 resizeLimits（上下限收紧到 screenW × targetH）
        if (auto* cbc = getConstrainer())
        {
            setResizeLimits (juce::jmin (cbc->getMinimumWidth(),  screenW),
                             juce::jmin (cbc->getMinimumHeight(), targetH),
                             juce::jmax (cbc->getMaximumWidth(),  screenW),
                             juce::jmax (cbc->getMaximumHeight(), targetH));
        }

        // 目标 Y：按贴顶/贴底切换
        const int targetY = bottomAligned ? (userArea.getBottom() - targetH)
                                          :  userArea.getY();

        if (top == this)
        {
            // 插件模式（嵌在宿主里）：无法移动顶层窗口，只调整尺寸
            setSize (screenW, targetH);
        }
        else
        {
            // Standalone 模式：连同外层窗口一起搬到目标位置
            top->setBounds (userArea.getX(), targetY, screenW, targetH);
        }

        // setSize / setBounds 会触发 resized()，workspace 随之拿到新 canvas。
        //   此时按横向等分的方式铺默认 7 个模块。
        static const ModuleType horizOrder[] = {
            ModuleType::eq,
            ModuleType::spectrogram,
            ModuleType::oscilloscope,
            ModuleType::spectrum,
            ModuleType::phase,
            ModuleType::dynamics,
            ModuleType::waveform
        };

        // 使用 workspace->getCanvasArea() 而不是 getLocalBounds()，
        //   后者包含底部 toolbarHeight（36px）与 chrome 控件区。
        const auto canvas = workspace->getCanvasArea();
        const int count    = (int) (sizeof (horizOrder) / sizeof (horizOrder[0]));

        // ============================================================
        // 网格对齐：必须与 ModuleWorkspace::gridSize 保持一致（8 像素）。
        //   原实现直接用 canvas.getWidth()/count 作为 slotW，不是 8 的倍数，
        //   canvas 原点也未必对齐 8；模块落点和大小都偏离网格。
        //   → 一旦用户之后拖动/缩放任意模块，snapToGrid 会把它吸附到最近
        //     的 8 像素位，立刻与相邻模块拉开空隙或错位，"密排列"被破坏。
        //
        //   修复思路：
        //     1) 起点 x0/y0 按 gridSize 向上取整，保证第一个模块左上角在网格上。
        //     2) 可用宽度 usableW 从 canvas.getRight() 向下取整到 gridSize
        //        的倍数后，减去 x0；高度 slotH 同理。
        //     3) 把 usableW 切成 count 份，每份都是 gridSize 的整倍数：
        //        · baseCells = usableW / gridSize / count 个小格
        //        · 剩余 leftoverCells 分摊到前若干个模块各 +1 小格。
        //        这样 7 个模块宽度之和 == usableW，全部在网格上且密排无缝。
        // ============================================================
        constexpr int kGrid = 8;
        auto ceilToGrid  = [kGrid] (int v) { return ((v + kGrid - 1) / kGrid) * kGrid; };
        auto floorToGrid = [kGrid] (int v) { return (v / kGrid) * kGrid; };

        const int x0 = ceilToGrid  (canvas.getX());
        const int y0 = ceilToGrid  (canvas.getY());
        const int xR = floorToGrid (canvas.getRight());
        const int yB = floorToGrid (canvas.getBottom());

        const int usableW = juce::jmax (kGrid * count, xR - x0);
        const int usableH = juce::jmax (kGrid,         yB - y0);

        const int totalCells    = usableW / kGrid;              // 小格数量（8px/格）
        const int baseCells     = totalCells / count;           // 每个模块的基础格数
        const int leftoverCells = totalCells - baseCells * count; // 需要 +1 格的模块数量

        const int slotH = juce::jmax (80, usableH);             // 高度占满 canvas 的整网格

        int curX = x0;
        for (int i = 0; i < count; ++i)
        {
            auto panel = createModule (horizOrder[i]);
            if (panel == nullptr) continue;

            // 前 leftoverCells 个模块多拿一格 gridSize，消化 usableW 不能整除 count 的余数
            const int cellsForThis = baseCells + (i < leftoverCells ? 1 : 0);
            const int slotW        = cellsForThis * kGrid;

            const int w = juce::jmax (panel->getMinWidth(),  slotW);
            const int h = juce::jmax (panel->getMinHeight(), slotH);
            panel->setBounds (curX, y0, w, h);
            workspace->addModule (std::move (panel), /*autoPosition*/ false);

            curX += slotW;
        }
    }
    else if (presetId == 4)
    {
        // Preset 4 "Tiled": 按用户 Y2Kmeter.settings 里的快照还原
        //   · 顶层窗口：1346×1087（屏幕坐标 (89,134)；Standalone 下会真实移动窗口，
        //     插件模式下仅设尺寸，位置由宿主决定）
        //   · 模块布局：使用下方硬编码的 PBEQ_Layout XML（直接从 settings 的
        //     filterState 二进制解码得到），loadLayoutFromXml 内部会自行清空
        //     workspace 旧模块 / 拼豆贴画，不需要额外 clear。

        // ------------------------------------------------------------------
        // 预设 4 的模块快照 XML（根节点 = PBEQ_Layout，loadLayoutFromXml 要求）。
        //   · 来源：C:/Users/echotcxu/AppData/Roaming/Y2Kmeter/Y2Kmeter.settings
        //     的 filterState 经 MemoryBlock::fromBase64Encoding + copyXmlToBinary
        //     反序列化得到。
        //   · 14 个模块：eq/loudness/dynamics/lufs_realtime/true_peak/spectrum/
        //     phase/phase_correlation/phase_balance/dynamics_meters/dynamics_dr/
        //     oscilloscope/dynamics_crest/waveform，铺满 1344×1024 canvas（waveform
        //     横贯底部）。
        //   · 将来如果用户需要"再增加一个类似预设"，可以用项目根目录的
        //     decode_filterstate.py 把新 settings 解码成 XML，再把内层
        //     PBEQ_Layout 拷到这里。
        // ------------------------------------------------------------------
        static const juce::String kTiledPresetLayoutXml = R"TILED(<PBEQ_Layout gridVisible="1"><Module type="eq" id="78" x="0" y="0" w="384" h="256"/><Module type="loudness" id="79" x="384" y="0" w="320" h="256"/><Module type="dynamics" id="83" x="0" y="512" w="384" h="256"/><Module type="lufs_realtime" id="85" x="704" y="0" w="320" h="256"/><Module type="true_peak" id="86" x="1024" y="0" w="256" h="192"/><Module type="spectrum" id="88" x="384" y="256" w="384" h="256"/><Module type="phase" id="89" x="768" y="256" w="320" h="192"/><Module type="phase_correlation" id="90" x="1088" y="256" w="192" h="128"/><Module type="phase_balance" id="91" x="1088" y="384" w="192" h="128"/><Module type="dynamics_meters" id="93" x="384" y="512" w="256" h="256"/><Module type="dynamics_dr" id="94" x="640" y="512" w="320" h="256"/><Module type="oscilloscope" id="97" x="0" y="256" w="384" h="256"/><Module type="dynamics_crest" id="113" x="960" y="512" w="384" h="256"/><Module type="waveform" id="115" x="0" y="768" w="1344" h="256"/></PBEQ_Layout>)TILED";

        constexpr int kTiledW = 1346;
        constexpr int kTiledH = 1087;
        constexpr int kTiledX = 89;
        constexpr int kTiledY = 134;

        auto* top = getTopLevelComponent();
        if (top == nullptr) top = this;

        // 放开 resizeLimits 上限，否则 1346×1087 会被旧约束（默认 1600×1100）限制
        //   下限保留现值，避免影响用户以后拉小窗口
        if (auto* cbc = getConstrainer())
        {
            setResizeLimits (juce::jmin (cbc->getMinimumWidth(),  kTiledW),
                             juce::jmin (cbc->getMinimumHeight(), kTiledH),
                             juce::jmax (cbc->getMaximumWidth(),  kTiledW),
                             juce::jmax (cbc->getMaximumHeight(), kTiledH));
        }

        if (top == this)
        {
            // 插件模式：无法移动顶层窗口位置，仅调尺寸；宿主会决定实际位置
            setSize (kTiledW, kTiledH);
        }
        else
        {
            // Standalone：连同外层 Y2KMainWindow 一起搬到 settings 记录的屏幕位置
            top->setBounds (kTiledX, kTiledY, kTiledW, kTiledH);
        }

        // 注入快照布局（loadLayoutFromXml 内部会先清空再恢复）
        workspace->loadLayoutFromXml (kTiledPresetLayoutXml);
    }

    // 手动回写布局到 Processor（clearAllModules + 批量 addModule 会触发多次
    // onLayoutChanged，我们在这里统一再写一次确保最终态被持久化）
    processor.setSavedLayoutXml (workspace->saveLayoutAsXml());

    // ------------------------------------------------------------------
    // 修复：切换预设（特别是预设 2）后顶层窗口的 alwaysOnTop 会"显示按下但
    // 实际不置顶"的 bug。根因：
    //   · 预设 2 会通过 top->setBounds(...) 大幅改变顶层窗口，Windows 在某些
    //     情况下会把 HWND_TOPMOST 丢掉；
    //   · 但 JUCE Component::setAlwaysOnTop 有早退优化：
    //       if (shouldStayOnTop != flags.alwaysOnTopFlag) { ... }
    //     当我们再次调用 setAlwaysOnTop(true) 时，内部 flag 仍是 true，
    //     方法会直接 return，OS 层的 TopMost 位就不会被重新打上。
    //   · 用户"按两下 pin 按钮"能恢复，就是先 false 再 true 绕开了早退。
    // 解决：这里在布局变更后显式做一次 false→true（仅在当前是置顶态时），
    //      强制让 peer 重新应用 HWND_TOPMOST。
    // ------------------------------------------------------------------
    if (alwaysOnTopActive)
        setAlwaysOnTopActive (true);
}

// ----------------------------------------------------------
// 工厂方法
// ----------------------------------------------------------
std::unique_ptr<ModulePanel> Y2KmeterAudioProcessorEditor::createModule(ModuleType type)
{
    switch (type)
    {
        case ModuleType::eq:
            return std::make_unique<EqModule>(processor);
        case ModuleType::loudness:
            return std::make_unique<LoudnessModule>(processor.getAnalyserHub());
        case ModuleType::oscilloscope:
            return std::make_unique<OscilloscopeModule>(processor.getAnalyserHub());
        case ModuleType::spectrum:
            return std::make_unique<SpectrumModule>(processor.getAnalyserHub());
        case ModuleType::phase:
            return std::make_unique<PhaseModule>(processor.getAnalyserHub());
        case ModuleType::dynamics:
            return std::make_unique<DynamicsModule>(processor.getAnalyserHub());

        case ModuleType::lufsRealtime:
            return std::make_unique<LufsRealtimeModule>(processor.getAnalyserHub());
        case ModuleType::truePeak:
            return std::make_unique<TruePeakModule>(processor.getAnalyserHub());
        case ModuleType::oscilloscopeLeft:
            return std::make_unique<OscilloscopeChannelModule>(processor.getAnalyserHub(), true);
        case ModuleType::oscilloscopeRight:
            return std::make_unique<OscilloscopeChannelModule>(processor.getAnalyserHub(), false);
        case ModuleType::phaseCorrelation:
            return std::make_unique<PhaseCorrelationModule>(processor.getAnalyserHub());
        case ModuleType::phaseBalance:
            return std::make_unique<PhaseBalanceModule>(processor.getAnalyserHub());
        case ModuleType::dynamicsMeters:
            return std::make_unique<DynamicsMetersModule>(processor.getAnalyserHub());
        case ModuleType::dynamicsDr:
            return std::make_unique<DynamicsDrModule>(processor.getAnalyserHub());
        case ModuleType::dynamicsCrest:
            return std::make_unique<DynamicsCrestModule>(processor.getAnalyserHub());

        case ModuleType::waveform:
            return std::make_unique<WaveformModule>(processor.getAnalyserHub());

        case ModuleType::vuMeter:
            return std::make_unique<VuMeterModule>(processor.getAnalyserHub());

        case ModuleType::spectrogram:
            return std::make_unique<SpectrogramModule>(processor.getAnalyserHub());

        case ModuleType::tamagotchi:
            return std::make_unique<TamagotchiModule>();

        default:
            jassertfalse; // 暂未实现
            return nullptr;

    }
}

// ============================================================
// 插件模式（VST3 / AU / AAX）下的预设 Save —— 把当前 Processor state
// 打包成一个与 Standalone 完全兼容的 <PROPERTIES> XML 文件
//
// 文件结构（与 JUCE PropertiesFile 在磁盘上的 XML 格式对齐）：
//   <?xml version="1.0" ...?>
//   <PROPERTIES>
//     <VALUE name="filterState" val="BASE64(processor state binary)"/>
//   </PROPERTIES>
//
// 这样：
//   · 用户在 VST3 里 Save 出来的 .settings，Standalone 下次启动时 Load
//     能原样恢复（Standalone 的 reloadPluginState 会读 filterState）；
//   · Standalone 里 Save 出来的 .settings，VST3 这边 Load 也能正确解析
//     （loadStateFromSettingsFile 会识别同样的 <PROPERTIES>/filterState 结构）。
//
// 注：插件模式下我们**有意不**写入主题 / 窗口 bounds / 音频源 等 Standalone
// 专属字段 —— 这些在 VST3 场景下的语义与 Standalone 完全不同（窗口由宿主管、
// 主题由插件内独立持久化）。如果用户用 VST3 存的文件里缺这些键，Standalone
// 首次 Load 时会按其默认值兜底，不会报错。
// ============================================================
void Y2KmeterAudioProcessorEditor::saveStateAsSettingsFile (const juce::File& dest)
{
    // 1) 先把"当前 workspace 的实时布局"回写到 Processor，保证 getStateInformation
    //    拿到的是用户眼睛看到的最新状态（onLayoutChanged 是异步节流的，不能指望
    //    点击 Save 的那一瞬间刚好已经被刷到 Processor）
    if (workspace != nullptr)
        processor.setSavedLayoutXml (workspace->saveLayoutAsXml());

    // 2) 导出 Processor state（内部是 XML 的二进制化形式）
    juce::MemoryBlock stateBlock;
    processor.getStateInformation (stateBlock);

    // 3) 构造 <PROPERTIES> 根节点 + 一个 filterState VALUE 子节点（base64）
    juce::XmlElement props ("PROPERTIES");
    auto* v = props.createNewChildElement ("VALUE");
    v->setAttribute ("name", "filterState");
    v->setAttribute ("val",  stateBlock.toBase64Encoding());

    // 4) 原子化写入目标文件（先写 TempFile，成功后 rename；避免半截文件）
    dest.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp (dest);
    if (! props.writeTo (tmp.getFile()))
    {
        DBG ("saveStateAsSettingsFile: write temp failed");
        return;
    }
    if (! tmp.overwriteTargetFileWithTemporary())
    {
        DBG ("saveStateAsSettingsFile: replace failed → " + dest.getFullPathName());
    }
}

// ============================================================
// 插件模式下的预设 Load —— 把用户选择的 .settings 文件解析出 Processor state
// 并就地热重载（不重启宿主）。容错三种来源：
//   a) Standalone / 本 Save 写出的 <PROPERTIES> + filterState (base64) 结构；
//   b) 裸 <PBEQ_State ...> XML（即 Processor::getStateInformation 的原生 XML 形态）；
//   c) 裸 <PBEQ_Layout ...> XML（仅包含布局，无其他元信息）。
// 优先级 a > b > c。任一成功即可。
// ============================================================
void Y2KmeterAudioProcessorEditor::loadStateFromSettingsFile (const juce::File& src)
{
    if (! src.existsAsFile()) return;

    auto xml = juce::parseXML (src);
    if (xml == nullptr)
    {
        DBG ("loadStateFromSettingsFile: not a valid XML → " + src.getFullPathName());
        return;
    }

    // ---- 分支 a：<PROPERTIES> 外壳，含 filterState base64 -------
    if (xml->hasTagName ("PROPERTIES"))
    {
        // 在 VALUE 子节点里找 name="filterState" 的那一个
        const juce::XmlElement* filterVal = nullptr;
        for (auto* child : xml->getChildWithTagNameIterator ("VALUE"))
        {
            if (child != nullptr && child->getStringAttribute ("name") == "filterState")
            {
                filterVal = child;
                break;
            }
        }
        if (filterVal != nullptr)
        {
            const auto b64 = filterVal->getStringAttribute ("val");
            juce::MemoryBlock block;
            if (block.fromBase64Encoding (b64) && block.getSize() > 0)
            {
                // 反序列化回 Processor —— 内部会更新 savedLayoutXml
                processor.setStateInformation (block.getData(), (int) block.getSize());
                // 立刻把新布局灌到 workspace（不用等宿主再开关一次 Editor）
                if (workspace != nullptr)
                {
                    const auto layoutXml = processor.getSavedLayoutXml();
                    if (layoutXml.isNotEmpty())
                        workspace->loadLayoutFromXml (layoutXml);
                }
                return;
            }
        }
        DBG ("loadStateFromSettingsFile: <PROPERTIES> without valid filterState");
        // 不 return，允许继续尝试把整文件当作其它格式（极少见）
    }

    // ---- 分支 b：裸 PBEQ_State XML -------
    if (xml->hasTagName ("PBEQ_State"))
    {
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (*xml, block);
        if (block.getSize() > 0)
        {
            processor.setStateInformation (block.getData(), (int) block.getSize());
            if (workspace != nullptr)
            {
                const auto layoutXml = processor.getSavedLayoutXml();
                if (layoutXml.isNotEmpty())
                    workspace->loadLayoutFromXml (layoutXml);
            }
            return;
        }
    }

    // ---- 分支 c：裸 PBEQ_Layout XML -------
    if (xml->hasTagName ("PBEQ_Layout"))
    {
        const auto layoutXml = xml->toString (juce::XmlElement::TextFormat{}.singleLine());
        // 同步写回 Processor 以便下次 getStateInformation 能拿到
        processor.setSavedLayoutXml (layoutXml);
        if (workspace != nullptr)
            workspace->loadLayoutFromXml (layoutXml);
        return;
    }

    DBG ("loadStateFromSettingsFile: unknown root tag = " + xml->getTagName());
}

// ----------------------------------------------------------
// 绘制：方案乙（完全铺满）
//   · 顶部 TitleBar（26px，Pink XP 风格抬头；含 Logo + 标题文字 + 右侧 × 按钮）
//   · 中部 Workspace（半透明），下面铺桌面棋盘纹理 + 中央 logo，作为"透过来的纹理背景"
//   · 底部 Toolbar（由 ModuleWorkspace 自己画）
//   关闭按钮嵌入 TitleBar 最右侧；chrome 隐藏时 TitleBar + × 均按 chromeAlpha 半透明
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::paint(juce::Graphics& g)
{
    // 1) 桌面纹理底图：只在 workspace 矩形范围内绘制
    //    workspace 是半透明的，纹理会作为模块背后的视觉肌理透出来
    if (workspace != nullptr)
    {
        const auto wsBounds = workspace->getBounds();
        if (! wsBounds.isEmpty())
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (wsBounds);
            PinkXP::drawDesktop (g, getLocalBounds());
            PinkXP::drawLogo    (g, wsBounds, logoImage);
        }
    }

    // 2) 顶部 TitleBar + 右上角按钮组
    //    · chrome 可见：正常绘制完整标题栏 + 三个按钮
    //    · chrome 隐藏：整个标题栏消失，只在右上角绘制一个半透明小关闭浮标
    //      （样式参照底部 Hide 按钮：未悬停时 15%、悬停时 100%）
    //    · 插件宿主模式（VST3 等）：也画"精简抬头"——只绘制背景 + Logo +
    //      "Y2Kmeter v1.1 iisaacbeats.cn"文字；不画右侧最小化/固定/关闭按钮
    //      （宿主窗口已经提供系统级边框/关闭按钮，伪按钮会产生两套 UI 打架）。
    if (! chromeDim)
    {
        auto tb = getTitleBarBounds();

        // 标题栏背景（Pink XP 风格 —— 粉色渐变 + 像素高光/阴影）
        //   文字留给我们自己画，这样"软件名 + 版本号 + 官网"可以拥有不同字号 / 下划线
        PinkXP::drawPinkTitleBar (g, tb, juce::String(), 12.0f);

        // 左侧 Logo 图标：drawPinkTitleBar 已保留最左侧 24px 给 icon 区域，
        //   我们继续在 icon 右侧顺延绘制标题文字，所以 textArea 的 x 从 28 开始。
        //   标题布局：[Y2Kmeter] [空格] [v1.1 小字] [空格-空格] [iisaacbeats.cn 小字]
        auto textArea = getTitleTextBounds();

        // 主标题 "Y2Kmeter"
        const juce::String nameText    = "Y2Kmeter";
const juce::String versionText = "v1.6";
        const juce::String urlText     = "iisaacbeats.cn";

        const juce::Font nameFont    = PinkXP::getFont (12.0f, juce::Font::bold);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);

        const int nameW    = juce::GlyphArrangement::getStringWidthInt (nameFont, nameText);
        const int versionW = juce::GlyphArrangement::getStringWidthInt (versionFont, versionText);
        const int urlW     = juce::GlyphArrangement::getStringWidthInt (urlFont, urlText);

        constexpr int gap1 = 6;   // name ↔ version 之间
        constexpr int gap2 = 10;  // version ↔ url 之间

        const int y  = textArea.getY();
        const int h  = textArea.getHeight();
        const int x0 = textArea.getX();

        // 文字阴影（1,1）
        g.setColour (PinkXP::sel.darker(0.55f));
        g.setFont (nameFont);
        g.drawText (nameText, x0 + 1, y + 1, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1 + 1, y + 1, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2 + 1, y + 1, urlW, h, juce::Justification::centredLeft, false);

        // 主文字（白色）
        g.setColour (PinkXP::selInk);
        g.setFont (nameFont);
        g.drawText (nameText, x0, y, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1, y, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2, y, urlW, h, juce::Justification::centredLeft, false);

        // hover 时在可点击区域（name + version + url 整段）画下划线
        if (titleTextHovered)
        {
            const int lineY = y + h - 3;
            const int totalW = nameW + gap1 + versionW + gap2 + urlW;
            g.setColour (PinkXP::selInk);
            g.fillRect (x0, lineY, totalW, 1);
        }

        // 缓存实际绘制宽度，供 mouseMove / mouseDown 精确命中（参考 titleTextHovered）
        cachedTitleTextW = nameW + gap1 + versionW + gap2 + urlW;

        // 标题栏下沿 1px 深色分割线（与 ModulePanel 视觉一致）
        g.setColour (PinkXP::dark);
        g.fillRect (tb.getX(), tb.getBottom(), tb.getWidth(), 1);

        // 3) 右上角三个按钮（从右到左：关闭 × / 固定 ★ / 最小化 _ ）
        //    · 插件宿主模式（VST3 等）：完全不画这三个按钮 —— 宿主自带系统级
        //      最小化/关闭按钮，我们只保留"软件名 + 版本号 + 官网"文字抬头。
        //    样式完全参考 XP：drawRaised + hover 粉色 + pressed 凹陷
        if (! isPluginHost)
        {
            auto drawTitleBtn = [&] (juce::Rectangle<int> rc,
                                     bool hovered, bool pressed, bool activeLatched,
                                     const juce::String& glyph,
                                     float fontHeight,
                                     int glyphDxWhenIdle,
                                     int glyphDyWhenIdle)
            {
                // Pin 按钮激活（alwaysOnTop=true）时呈现"凹陷锁定"视觉
                if (pressed || activeLatched)
                    PinkXP::drawPressed (g, rc, activeLatched ? PinkXP::pink300 : PinkXP::pink100);
                else
                    PinkXP::drawRaised  (g, rc, hovered ? PinkXP::pink200 : PinkXP::btnFace);

                g.setColour (PinkXP::ink);
                g.setFont   (PinkXP::getFont (fontHeight, juce::Font::bold));
                auto txt = rc;
                txt.translate (glyphDxWhenIdle, glyphDyWhenIdle);
                if (pressed || activeLatched) txt.translate (1, 1);
                g.drawText (glyph, txt, juce::Justification::centred, false);
            };

            // 3a) 最小化按钮："_"（字体里下划线位置偏低，y=-3 让底横线略高一点更像传统 _ 位置）
            drawTitleBtn (getMinimiseButtonBounds(),
                          minButtonHovered, minButtonPressed, false,
                          "_", 12.0f, -1, -3);

            // 3b) 固定（置顶）按钮：未激活 "*"  / 激活 "*"(凹陷) —— 使用易识别的星号符号
            //     字体里 '*' 位置偏下，y=-2 做居中微调（相对最小化 y=-3 略低 1 像素更对眼）
            drawTitleBtn (getPinButtonBounds(),
                          pinButtonHovered, pinButtonPressed, alwaysOnTopActive,
                          "*", 14.0f, -1, -2);

            // 3c) 关闭按钮："x"（字体里 'x' 位置正常，仅 y=-1 做微调）
            drawTitleBtn (getCloseButtonBounds(),
                          closeButtonHovered, closeButtonPressed, false,
                          "x", 12.0f, -1, -1);
        }
    }
    // chrome 隐藏态的浮动关闭按钮 + 抬头文字已交由 ChromeHiddenOverlay 处理
    //   （作为独立 child 组件，z-order 高于 workspace，避免被覆盖导致无法交互）
}

// ----------------------------------------------------------
// 布局：方案乙（完全铺满）
//   titleBar (26px) | workspace（自带底部 toolbar 36px） —— 三段占满整个 Editor
//   workspace 内部的 toolbar 会自动占据最底部 36px（由 ModuleWorkspace 处理）
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::resized()
{
    auto r = getLocalBounds();
    // chrome 可见时顶部让给 TitleBar；chrome 隐藏时让 workspace 占满整个窗口，
    //   浮层 overlay 作为"最底层 child"铺在顶部 titleBarHeight 区域，会被模块自然遮挡。
    // 插件宿主模式（VST3 等）：也为标题栏预留 titleBarHeight —— 我们会画一个
    //   "精简抬头"（只有软件名 + 版本号 + 官网文字，无右侧最小化/固定/关闭按钮），
    //   宿主窗口已提供自己的系统标题栏和边框，不会与此抬头冲突。
    if (! chromeDim)
        r.removeFromTop (titleBarHeight);
    workspace->setBounds (r);

    // 浮层固定在顶部，与 TitleBar 同尺寸（Editor 同宽 × titleBarHeight）。
    if (chromeHiddenOverlay != nullptr)
        chromeHiddenOverlay->setBounds (0, 0, getWidth(), titleBarHeight);

    // 同步 workspace 的 hit-test 挖洞（按钮位置依赖 getWidth()，resize 后必须重新计算）
    updateWorkspaceHitTestHoles();
}

void Y2KmeterAudioProcessorEditor::visibilityChanged()
{
    processor.setAnalysisActive(true);

    // 默认启用"固定窗口置顶"（alwaysOnTopActive 初始 true）：
    //   · 仅在 Standalone 下应用（插件模式下顶层是宿主窗口，setAlwaysOnTop
    //     会影响宿主行为，且多数 DAW 会忽略该调用，但为避免副作用，限定
    //     Standalone 场景）。
    //   · visibilityChanged 是 getTopLevelComponent() 已指向实际顶层 Y2KMainWindow
    //     的最早时机；构造器里调 getTopLevelComponent() 可能还是 Editor 自己。
    //   · 只在首次 visibilityChanged 做一次，避免每次隐藏/显示时反复强推。
    //   · 通过 setAlwaysOnTopActive() 强推（内部做 false→true 绕开 JUCE 早退），
    //     防止"按钮显按下但实际未置顶"的 bug。
    if (! initialAlwaysOnTopApplied
        && juce::JUCEApplicationBase::isStandaloneApp())
    {
        if (auto* top = getTopLevelComponent())
        {
            if (top != this)
            {
                setAlwaysOnTopActive (alwaysOnTopActive);
            }
        }
    }
}

// ==========================================================
// 标题栏 / 关闭按钮几何
// ==========================================================
juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getTitleBarBounds() const
{
    return { 0, 0, getWidth(), titleBarHeight };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getTitleTextBounds() const
{
    // 标题栏左侧可点击文字区域：从 x=28 开始（drawPinkTitleBar 已为左侧 icon 保留 6..25 的 19px）
    //   · Standalone：右边界到最小化按钮左侧留 8px 间隔，避免误点到按钮
    //   · 插件宿主模式：没有右侧按钮，右边界直接贴到 Editor 右缘，留 8px 边距
    auto tb = getTitleBarBounds();
    const int x = tb.getX() + 28;
    const int right = isPluginHost ? (tb.getRight() - 8)
                                    : (getMinimiseButtonBounds().getX() - 8);
    return { x, tb.getY(), juce::jmax (0, right - x), tb.getHeight() };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getCloseButtonBounds() const
{
    auto tb = getTitleBarBounds();
    const int y = tb.getY() + (tb.getHeight() - closeButtonSize) / 2;
    return { tb.getRight() - closeButtonMargin - closeButtonSize, y,
             closeButtonSize, closeButtonSize };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getPinButtonBounds() const
{
    // 关闭按钮左侧（中间那一个）：固定（置顶）按钮
    auto cb = getCloseButtonBounds();
    return { cb.getX() - titleButtonGap - closeButtonSize, cb.getY(),
             closeButtonSize, closeButtonSize };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getMinimiseButtonBounds() const
{
    // Pin 按钮左侧（最左那一个）：最小化按钮
    auto pb = getPinButtonBounds();
    return { pb.getX() - titleButtonGap - closeButtonSize, pb.getY(),
             closeButtonSize, closeButtonSize };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getFloatingCloseButtonBounds() const
{
    // chrome 隐藏态下的悬浮关闭按钮：右上角，距边距 4px
    constexpr int margin = 4;
    return { getWidth()  - margin - closeButtonSize, margin,
             closeButtonSize, closeButtonSize };
}

// chrome 可见时恒为 1.0；chrome 隐藏时本函数已废弃（整个标题栏直接不绘制，
// 只留悬浮关闭按钮按自己的 hover 态切换透明度）。保留仅为兼容旧调用点。
float Y2KmeterAudioProcessorEditor::getChromeAlpha() const
{
    return 1.0f;
}

void Y2KmeterAudioProcessorEditor::handleCloseClicked()
{
    // Standalone 模式：请求应用退出（JUCE 会调用 StandaloneApp::systemRequestedQuit）
    // VST3 模式：顶层是 Editor 本身，调用 quit 无效；改为发 WM_CLOSE 到宿主无意义，
    //           直接忽略即可（关闭按钮在插件模式下仅视觉存在，宿主自带窗口 × 会关闭）。
    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();
    }
}

// 固定（置顶）：切换顶层窗口的 alwaysOnTop 属性
//   · Standalone：生效于自定义的 Y2KMainWindow（继承 DocumentWindow）
//   · 插件模式：顶层通常是宿主窗口，setAlwaysOnTop 会被 JUCE 向上调用；
//     宿主有时会忽略，但不会有负作用。
void Y2KmeterAudioProcessorEditor::handlePinClicked()
{
    setAlwaysOnTopActive (! alwaysOnTopActive);
}

// 直接设置"固定置顶"状态（供 handlePinClicked 与 StandaloneApp 恢复使用）
//   为什么要显式 false→true：JUCE Component::setAlwaysOnTop 有早退优化
//   （flag 与当前相同则 no-op）。Editor 首次 visibilityChanged 可能在
//   top==this 的瞬间被调用过一次（flag 已置 true 但实际未落到真正的顶层窗口），
//   之后再推 setAlwaysOnTop(true) 就被早退吃掉，出现"按钮按下但实际未置顶"的
//   bug。这里总是先 false 再 true 强推一次，彻底避开早退。
void Y2KmeterAudioProcessorEditor::setAlwaysOnTopActive (bool shouldBeOnTop)
{
    alwaysOnTopActive = shouldBeOnTop;

    if (auto* top = getTopLevelComponent())
    {
        if (top != this)
        {
            // 先清一次再置目标值，绕开 flag 相同时的 no-op 早退
            top->setAlwaysOnTop (false);
            top->setAlwaysOnTop (shouldBeOnTop);
        }
        else
        {
            top->setAlwaysOnTop (shouldBeOnTop);
        }
    }

    initialAlwaysOnTopApplied = true;
    repaint (getTitleBarBounds());
}

// 最小化：仅在 Standalone 下有意义；调用顶层窗口的 peer 将其最小化
void Y2KmeterAudioProcessorEditor::handleMinimiseClicked()
{
    if (auto* top = getTopLevelComponent())
    {
        if (auto* peer = top->getPeer())
            peer->setMinimised (true);
    }
}

// 同步 workspace 的 hit-test 挖洞。
//   · chrome 可见时：无挖洞（workspace 占据 y=titleBarHeight 以下区域，顶部 26px 是
//     Editor 的标题栏范围，workspace 根本不覆盖那里，自然不需要挖洞）。
//   · chrome 隐藏时：workspace 扩到全窗口，顶部 26px 中存在"浮动关闭按钮矩形 +
//     标题文字矩形"两块区域需要冒泡给 Editor 处理；挖洞坐标已转换成 workspace
//     坐标系（workspace 此时 y=0，与 Editor 坐标系一致）。
void Y2KmeterAudioProcessorEditor::updateWorkspaceHitTestHoles()
{
    if (workspace == nullptr) return;

    // 插件宿主模式：没有自画浮动关闭按钮 / 标题文字，强制清空挖洞列表即可
    if (isPluginHost)
    {
        workspace->setHitTestHoles ({});
        return;
    }

    if (! chromeDim)
    {
        workspace->setHitTestHoles ({});
        return;
    }

    juce::Array<juce::Rectangle<int>> holes;

    // 1) 浮动关闭按钮矩形（固定位置，与 ChromeHiddenOverlay::getFloatingCloseButtonRect 保持一致）
    constexpr int floatMargin = 4;
    juce::Rectangle<int> closeRect (getWidth() - floatMargin - closeButtonSize,
                                     floatMargin,
                                     closeButtonSize, closeButtonSize);
    holes.add (closeRect);

    // 2) 标题文字矩形（实际像素宽度由 overlay paint 后回写；首次未 paint 时宽度为 0 不添加）
    if (chromeHiddenOverlay != nullptr)
    {
        const int textW = chromeHiddenOverlay->getCachedTitleTextWidth();
        if (textW > 0)
        {
            juce::Rectangle<int> textRect (28, 0, textW, titleBarHeight);
            holes.add (textRect);
        }
    }

    workspace->setHitTestHoles (holes);
}

// ==========================================================
// 鼠标事件：关闭按钮 hover/press/click + TitleBar 区拖动顶层窗口
//   · chromeDim 模式下，mouseEnter/Exit 控制 TitleBar 恢复/淡化
// ==========================================================
void Y2KmeterAudioProcessorEditor::mouseMove(const juce::MouseEvent& e)
{
    // 插件宿主模式：不处理按钮 hover 与 chromeDim 分支；但仍要处理"标题文字 hover"，
    //   让鼠标悬停到 "Y2Kmeter v1.1 iisaacbeats.cn" 上时出现手型光标和下划线，
    //   以便点击打开官网。
    if (isPluginHost)
    {
        mouseInsideEditor = true;

        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        const bool hovered = textW > 0 && hotspot.contains (e.getPosition());
        if (hovered != titleTextHovered)
        {
            titleTextHovered = hovered;
            setMouseCursor (hovered ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
            repaint (getTitleBarBounds());
        }
        return;
    }

    mouseInsideEditor = true;

    auto updateHover = [&] (bool& state, juce::Rectangle<int> rc)
    {
        const bool h = rc.contains (e.getPosition());
        if (h != state) { state = h; repaint (rc); }
    };

    if (chromeDim)
    {
        // chrome 隐藏态：Editor 负责处理"浮动关闭按钮 / 标题文字"的 hover，
        //   workspace 已通过 hit-test 挖洞让这些事件冒泡过来；视觉状态由 overlay 负责绘制。
        if (chromeHiddenOverlay == nullptr)
            return;

        const auto p = e.getPosition();
        const auto closeRc = chromeHiddenOverlay->getFloatingCloseButtonRect()
                                .translated (chromeHiddenOverlay->getX(),
                                             chromeHiddenOverlay->getY());
        const bool overClose = closeRc.contains (p);
        chromeHiddenOverlay->setCloseButtonHovered (overClose);
        closeButtonHovered = overClose; // 复用 Editor 的状态跟踪 press/click 流转

        // 标题文字 hover（宽度按 overlay 缓存的实际像素宽度计算，避免整条横条命中）
        const int textW = chromeHiddenOverlay->getCachedTitleTextWidth();
        juce::Rectangle<int> textRc (28 + chromeHiddenOverlay->getX(),
                                      chromeHiddenOverlay->getY(),
                                      juce::jmax (0, textW),
                                      titleBarHeight);
        const bool overText = textW > 0 && textRc.contains (p);
        chromeHiddenOverlay->setTitleTextHovered (overText);
        if (overText != titleTextHovered)
        {
            titleTextHovered = overText;
            setMouseCursor (overText ? juce::MouseCursor::PointingHandCursor
                                     : juce::MouseCursor::NormalCursor);
        }

        // 隐藏态下不再有 pin/minimise 按钮；强制清零以免切换回显示态时有残留 hover 视觉
        if (pinButtonHovered) pinButtonHovered = false;
        if (minButtonHovered) minButtonHovered = false;
    }
    else
    {
        updateHover (closeButtonHovered, getCloseButtonBounds());
        updateHover (pinButtonHovered,   getPinButtonBounds());
        updateHover (minButtonHovered,   getMinimiseButtonBounds());

        // 标题文字 hover 检测（实际文字像素矩形，宽度由 paint 回写）
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        const bool hovered = textW > 0 && hotspot.contains (e.getPosition());
        if (hovered != titleTextHovered)
        {
            titleTextHovered = hovered;
            setMouseCursor (hovered ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
            repaint (getTitleBarBounds());
        }
    }
}

void Y2KmeterAudioProcessorEditor::mouseExit(const juce::MouseEvent&)
{
    // 插件宿主模式：没有按钮 hover 要清理，但标题文字 hover 状态仍要复位
    //   （否则鼠标移出顶部抬头后，下划线和手型光标不会消失）。
    if (isPluginHost)
    {
        if (titleTextHovered)
        {
            titleTextHovered = false;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            repaint (getTitleBarBounds());
        }
        return;
    }

    auto clearHover = [&] (bool& state, juce::Rectangle<int> rc)
    {
        if (state) { state = false; repaint (rc); }
    };
    // 统一清理（两套 rect 都 repaint 一下开销可忽略）
    clearHover (closeButtonHovered, getCloseButtonBounds());
    clearHover (pinButtonHovered,   getPinButtonBounds());
    clearHover (minButtonHovered,   getMinimiseButtonBounds());

    if (titleTextHovered)
    {
        titleTextHovered = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint (getTitleBarBounds());
    }

    // 隐藏态：同步清空 overlay 的 hover 视觉
    if (chromeHiddenOverlay != nullptr)
    {
        chromeHiddenOverlay->setCloseButtonHovered (false);
        chromeHiddenOverlay->setTitleTextHovered (false);
    }

    mouseInsideEditor = false;
}

void Y2KmeterAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    // 插件宿主模式：不处理右侧按钮（因为根本没画）和窗口拖拽（宿主负责），
    //   但允许"标题文字"点击打开官网。其他区域点击继续冒泡给子组件。
    if (isPluginHost)
    {
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        if (textW > 0 && hotspot.contains (e.getPosition()))
        {
            juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
        }
        return;
    }

    // chrome 隐藏态：仅处理浮动关闭按钮 + 标题文字点击；其他区域不处理（也不支持从顶部拖窗口）
    if (chromeDim)
    {
        if (chromeHiddenOverlay == nullptr) return;

        const auto p = e.getPosition();
        const auto closeRc = chromeHiddenOverlay->getFloatingCloseButtonRect()
                                .translated (chromeHiddenOverlay->getX(),
                                             chromeHiddenOverlay->getY());
        if (closeRc.contains (p))
        {
            closeButtonPressed = true;
            chromeHiddenOverlay->setCloseButtonPressed (true);
            return;
        }

        const int textW = chromeHiddenOverlay->getCachedTitleTextWidth();
        juce::Rectangle<int> textRc (28 + chromeHiddenOverlay->getX(),
                                      chromeHiddenOverlay->getY(),
                                      juce::jmax (0, textW),
                                      titleBarHeight);
        if (textW > 0 && textRc.contains (p))
        {
            juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
        }
        return;
    }

    // 1) 命中标题栏右侧三个按钮之一
    if (getCloseButtonBounds().contains (e.getPosition()))
    {
        closeButtonPressed = true;
        repaint (getCloseButtonBounds());
        return;
    }
    if (getPinButtonBounds().contains (e.getPosition()))
    {
        pinButtonPressed = true;
        repaint (getPinButtonBounds());
        return;
    }
    if (getMinimiseButtonBounds().contains (e.getPosition()))
    {
        minButtonPressed = true;
        repaint (getMinimiseButtonBounds());
        return;
    }

    // 2) 只有 TitleBar 区域才允许拖拽顶层窗口（模块 / toolbar 区不参与窗口拖拽）
    if (! getTitleBarBounds().contains(e.getPosition()))
        return;

    // 2.1) 命中标题文字热区 → 打开官网（iisaacbeats.cn），不启动窗口拖拽
    {
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        if (textW > 0 && hotspot.contains (e.getPosition()))
        {
            juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
            return;
        }
    }

    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        if (auto* top = getTopLevelComponent())
        {
            windowDragger.startDraggingComponent(top, e.getEventRelativeTo(top));
            draggingWindow = true;
        }
    }
}

void Y2KmeterAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e)
{
    // 插件宿主模式：无 chrome 拖窗需求
    if (isPluginHost) return;

    if (draggingWindow)
    {
        if (auto* top = getTopLevelComponent())
            windowDragger.dragComponent(top, e.getEventRelativeTo(top), nullptr);
    }
}

void Y2KmeterAudioProcessorEditor::mouseUp(const juce::MouseEvent& e)
{
    // 插件宿主模式：无 chrome 按钮点击处理
    if (isPluginHost) return;

    draggingWindow = false;

    // 按下与抬起都落在同一按钮内才触发 click —— XP 风按钮的标准行为
    if (closeButtonPressed)
    {
        closeButtonPressed = false;

        // 根据当前 chrome 状态决定使用"浮动关闭按钮"还是"标题栏内的关闭按钮"的几何
        juce::Rectangle<int> rc;
        if (chromeDim && chromeHiddenOverlay != nullptr)
        {
            rc = chromeHiddenOverlay->getFloatingCloseButtonRect()
                    .translated (chromeHiddenOverlay->getX(),
                                 chromeHiddenOverlay->getY());
            chromeHiddenOverlay->setCloseButtonPressed (false);
        }
        else
        {
            rc = getCloseButtonBounds();
        }

        const bool stillOn = rc.contains (e.getPosition());
        repaint (rc);
        if (stillOn) handleCloseClicked();
        return;
    }
    if (pinButtonPressed)
    {
        pinButtonPressed = false;
        const bool stillOn = getPinButtonBounds().contains (e.getPosition());
        repaint (getPinButtonBounds());
        if (stillOn) handlePinClicked();
        return;
    }
    if (minButtonPressed)
    {
        minButtonPressed = false;
        const bool stillOn = getMinimiseButtonBounds().contains (e.getPosition());
        repaint (getMinimiseButtonBounds());
        if (stillOn) handleMinimiseClicked();
        return;
    }
}

// ==========================================================
// 音频源下拉：Editor 对外 API → 转发到 Workspace
// ==========================================================
void Y2KmeterAudioProcessorEditor::setAudioSourceItems (
    const juce::Array<AudioSourceEntry>& items,
    const juce::String& selectedSourceId)
{
    if (workspace == nullptr) return;

    juce::Array<ModuleWorkspace::AudioSourceItem> ws;
    ws.ensureStorageAllocated (items.size());
    for (const auto& it : items)
        ws.add ({ it.displayName, it.sourceId, it.isLoopback });

    workspace->setAudioSourceItems (ws, selectedSourceId);
}

// ==========================================================
// 持久化辅助：chrome 可见性（Standalone 在 save/restore 时使用）
// ==========================================================
bool Y2KmeterAudioProcessorEditor::isChromeVisible() const
{
    return workspace != nullptr ? workspace->isChromeVisible() : true;
}

void Y2KmeterAudioProcessorEditor::setChromeVisible (bool shouldBeVisible)
{
    if (workspace != nullptr)
        workspace->setChromeVisible (shouldBeVisible);
}

// ==========================================================
// timerCallback —— 10Hz 拉 Processor 的 CPU 占用，广播给所有模块；
//                  同时按 1s 窗口换算实际 FPS，下发给 workspace 的 FPS 标签
// ==========================================================
void Y2KmeterAudioProcessorEditor::timerCallback()
{
    if (workspace == nullptr) return;

    // 仅当存在 Tamagotchi 模块时，才保活/计算对应的音频信号。
    const int n = workspace->getNumModules();
    juce::Array<TamagotchiModule*> tamagotchiModules;
    tamagotchiModules.ensureStorageAllocated (n);

    for (int i = 0; i < n; ++i)
        if (auto* tamagotchi = dynamic_cast<TamagotchiModule*> (workspace->getModule (i)))
            tamagotchiModules.add (tamagotchi);

    const bool hasTamagotchi = ! tamagotchiModules.isEmpty();
    if (hasTamagotchi != tamagotchiSignalRetained)
    {
        if (hasTamagotchi)
            processor.getAnalyserHub().retain (AnalyserHub::Kind::Loudness);
        else
            processor.getAnalyserHub().release (AnalyserHub::Kind::Loudness);

        tamagotchiSignalRetained = hasTamagotchi;
    }

    float signal01 = 0.0f;
    if (hasTamagotchi)
    {
        if (auto frame = processor.getAnalyserHub().getLatestFrame())
        {
            if (frame->has (AnalyserHub::Kind::Loudness))
            {
                const float rmsDb = juce::jmax (frame->loudness.rmsL, frame->loudness.rmsR);
                signal01 = juce::jlimit (0.0f, 1.0f, juce::Decibels::decibelsToGain (rmsDb, -144.0f));
            }
            else if (frame->has (AnalyserHub::Kind::Dynamics))
            {
                const float rmsDb = juce::jmax (frame->dynamics.rmsL, frame->dynamics.rmsR);
                signal01 = juce::jlimit (0.0f, 1.0f, juce::Decibels::decibelsToGain (rmsDb, -144.0f));
            }
            else if (frame->has (AnalyserHub::Kind::Oscilloscope))
            {
                double sumSqL = 0.0;
                double sumSqR = 0.0;
                for (int i = 0; i < AnalyserHub::oscilloscopeBufferSize; ++i)
                {
                    const double l = (double) frame->oscL[(size_t) i];
                    const double r = (double) frame->oscR[(size_t) i];
                    sumSqL += l * l;
                    sumSqR += r * r;
                }

                const double invN = 1.0 / (double) AnalyserHub::oscilloscopeBufferSize;
                const float rmsLinear = (float) std::sqrt (juce::jmax (0.0, juce::jmax (sumSqL * invN, sumSqR * invN)));
                signal01 = juce::jlimit (0.0f, 1.0f, rmsLinear);
            }
        }
    }

    const float cpu01 = (float) processor.getCpuLoad();
    for (int i = 0; i < n; ++i)
        if (auto* m = workspace->getModule (i))
            m->setCpuLoad (cpu01);

    for (auto* tamagotchi : tamagotchiModules)
        tamagotchi->setSignalLevel01 (signal01);

    // FPS 统计：每秒更新一次显示（使用高精度时戳防止 wall-clock 跨日问题）
    const double nowMs   = juce::Time::getMillisecondCounterHiRes();
    const double deltaMs = nowMs - lastFpsTimeMs;
    if (deltaMs >= 1000.0)
    {
        const juce::int64 cur = frameCounter.load (std::memory_order_relaxed);
        const juce::int64 diff = cur - lastFrameCounterSample;
        const float fps = (float) (diff * 1000.0 / deltaMs);

        workspace->setMeasuredFps (fps);
        applyAdaptiveFrameRate (fps);

        lastFrameCounterSample = cur;
        lastFpsTimeMs          = nowMs;
    }
}

void Y2KmeterAudioProcessorEditor::applyAdaptiveFrameRate (float measuredFps)
{
    auto& hub = processor.getAnalyserHub();

    const int requested = juce::jlimit (15, 120, userRequestedFpsLimit);
    int targetHz = adaptiveDispatchHz;

    // 插件宿主里更容易受 UI 消息循环节流，优先保证稳定感而非硬追目标帧。
    if (isPluginHost)
    {
        const int requestedCap = juce::jmin (48, requested);

        if (measuredFps < (float) requestedCap * 0.50f)
        {
            targetHz = juce::jmax (15, requestedCap - 16);
            adaptiveRecoverTicks = 0;
        }
        else if (measuredFps < (float) requestedCap * 0.65f)
        {
            targetHz = juce::jmax (16, requestedCap - 10);
            adaptiveRecoverTicks = 0;
        }
        else if (measuredFps < (float) requestedCap * 0.80f)
        {
            targetHz = juce::jmax (18, requestedCap - 6);
            adaptiveRecoverTicks = 0;
        }
        else if (measuredFps > (float) requestedCap * 0.96f)
        {
            ++adaptiveRecoverTicks;
            if (adaptiveRecoverTicks >= 4)
            {
                targetHz = requestedCap;
                adaptiveRecoverTicks = 0;
            }
        }
        else
        {
            adaptiveRecoverTicks = 0;
        }
    }
    else
    {
        // standalone 优先贴近用户设定，但在重负载瞬间允许轻微降帧。
        if (measuredFps < (float) requested * 0.60f)
        {
            targetHz = juce::jmax (20, requested - 6);
            adaptiveRecoverTicks = 0;
        }
        else if (measuredFps > (float) requested * 0.94f)
        {
            ++adaptiveRecoverTicks;
            if (adaptiveRecoverTicks >= 2)
            {
                targetHz = requested;
                adaptiveRecoverTicks = 0;
            }
        }
        else
        {
            adaptiveRecoverTicks = 0;
        }
    }

    const int maxAllowedHz = isPluginHost ? juce::jmin (48, requested) : requested;
    targetHz = juce::jlimit (15, maxAllowedHz, targetHz);

    if (targetHz != adaptiveDispatchHz || hub.getFrameDispatcherHz() != targetHz)
    {
        adaptiveDispatchHz = targetHz;
        hub.startFrameDispatcher (adaptiveDispatchHz);
    }
}

// FrameListener 的实际实现在文件顶端的 FpsFrameListener 嵌套类中定义，
// 这里不再重复。