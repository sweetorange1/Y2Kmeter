#include "source/ui/HoverHelp.h"
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/UiPrefs.h"
#include "PluginEditor.h"   // 提示窗口的宿主是 Editor：按屏幕坐标定位模块需要 workspace

// ==========================================================
// HoverHelp.cpp —— 悬停提示：文案库 / 开关持久化 / TooltipWindow 解析逻辑
// ==========================================================

namespace y2k::help
{
    juce::String make (const juce::String& title, const juce::String& body)
    {
        if (title.isEmpty()) return body;
        if (body.isEmpty())  return title + "\n";
        return title + "\n" + body;
    }

    // ------------------------------------------------------
    // 开关持久化（走 UiPrefs，与界面语言共用一份偏好文件）
    //   只缓存一个 bool；读写频率极低（只在切换时写）。
    // ------------------------------------------------------
    namespace
    {
        bool gLoaded  = false;
        bool gEnabled = true;

        constexpr const char* kHoverTipKey = "hoverTips";
    }

    bool isEnabled()
    {
        if (! gLoaded)
        {
            gLoaded  = true;
            gEnabled = y2k::prefs::getBool (kHoverTipKey, true);
        }
        return gEnabled;
    }

    void setEnabled (bool shouldBeEnabled)
    {
        gLoaded  = true;
        gEnabled = shouldBeEnabled;
        y2k::prefs::setBool (kHoverTipKey, shouldBeEnabled);
    }

    // ------------------------------------------------------
    // 模块详细说明
    //   文案以英文为源语言（翻译键）；显示时由 PinkXPLookAndFeel::drawTooltip
    //   经 y2k::i18n 按"整段 → 逐行"查表翻译，因此这里保持纯英文 ASCII。
    // ------------------------------------------------------
    juce::String moduleHelp (ModuleType type)
    {
        const auto name = getModuleDisplayName (type);

        switch (type)
        {
            case ModuleType::eq:
                return make (name,
                    "Live spectrum drawn as a grid of pixel perler beads.\n"
                    "- Top slider: visible frequency range (drag both ends).\n"
                    "- SIZE: bead size in pixels.\n"
                    "Visual only - it never changes your audio.");

            case ModuleType::loudness:
                return make (name,
                    "ITU-R BS.1770-4 loudness at a glance.\n"
                    "- M: momentary LUFS (400 ms window)\n"
                    "- S: short-term LUFS (3 s window)\n"
                    "- I: integrated LUFS (gated, whole programme)\n"
                    "- Peak L / R: channel peak level\n"
                    "Integrated resets automatically after a long silence.");

            case ModuleType::oscilloscope:
                return make (name,
                    "Oscilloscope with three views:\n"
                    "- Wave: classic time-domain waveform\n"
                    "- X-Y: left vs right plot (stereo shape)\n"
                    "- Liss: Lissajous figure\n"
                    "X-Y and Liss auto-zoom to the signal peak. Freeze holds the picture.");

            case ModuleType::spectrum:
                return make (name,
                    "Real-time spectrum analyser on a log axis (20 Hz - 20 kHz).\n"
                    "- Hover the graph to read frequency and level.\n"
                    "- Press and hold the mouse on the graph for bullet time: the "
                    "fall-off slows to 1/50 speed (ghost colour) until you release.\n"
                    "- Peak / Sharp / curve style / channel view buttons on top.\n"
                    "Resolution follows FFT Size in the bottom toolbar.");

            case ModuleType::phase:
                return make (name,
                    "Stereo phase toolkit.\n"
                    "- Goniometer: mid / side vector scope of L and R\n"
                    "- Correlation: +1 mono-safe, 0 very wide, -1 out of phase\n"
                    "- Width and Balance bars\n"
                    "A needle that sits below 0 means parts will cancel in mono.");

            case ModuleType::dynamics:
                return make (name,
                    "Dynamics overview: Peak and RMS for L / R, crest factor "
                    "(peak minus RMS) and dynamic range (DR) with history.\n"
                    "Hover the graph to read values.");

            case ModuleType::lufsRealtime:
                return make (name,
                    "Large real-time loudness readout (LUFS).\n"
                    "- M: momentary loudness - 400 ms window, reacts fastest.\n"
                    "- S: short-term loudness - 3 s window, the usual main number.\n"
                    "- I: integrated loudness - gated average over the whole programme.\n"
                    "The loudness part of the Loudness module on its own, for compact layouts.");

            case ModuleType::truePeak:
                return make (name,
                    "True Peak and RMS for left and right.\n"
                    "- TP-L / TP-R: true peak in dBTP, 4x oversampled - catches "
                    "inter-sample peaks that sample meters miss.\n"
                    "- RMS-L / RMS-R: average signal level in dBFS.\n"
                    "Keep true peak under -1 dBTP for streaming platforms.");

            case ModuleType::oscilloscopeWave:
                return make (name,
                    "Pure waveform scope.\n"
                    "- L / R / Both: choose which channel to draw.");

            case ModuleType::phaseCorrelation:
                return make (name,
                    "Phase correlation meter.\n"
                    "+1 = mono compatible, 0 = very wide / unrelated, "
                    "-1 = out of phase (cancels when summed to mono).");

            case ModuleType::phaseBalance:
                return make (name,
                    "Left / right balance meter: shows which side is louder. "
                    "Centre means the mix is balanced.");

            case ModuleType::dynamicsMeters:
                return make (name,
                    "Peak and RMS meters for left and right side by side. "
                    "The gap between Peak and RMS shows how punchy the signal is.");

            case ModuleType::dynamicsDr:
                return make (name,
                    "Dynamic range (DR): short-term and integrated.\n"
                    "Higher = more dynamic, lower = more compressed / limited.");

            case ModuleType::dynamicsCrest:
                return make (name,
                    "Crest factor history (peak-to-RMS in dB) scrolling over time.\n"
                    "- SPAN: visible time window (1 - 35 s).\n"
                    "Hover the graph to read values.");

            case ModuleType::waveform:
                return make (name,
                    "Scrolling waveform history.\n"
                    "- Colour button: PITCH / LOUD / SIB colouring.\n"
                    "- Freeze: pause scrolling.\n"
                    "- SPEED: scroll speed in pixels per second.\n"
                    "Hover to read time and level.");

            case ModuleType::vuMeter:
                return make (name,
                    "Analogue-style VU needle (combined L + R RMS, dBFS).\n"
                    "Fast rise, slow mechanical fall - like a real VU.\n"
                    "LED: off = no signal, green = signal, red = hot.");

            case ModuleType::spectrogram:
                return make (name,
                    "Pixel spectrogram: frequency (vertical) over time (scrolling).\n"
                    "- Mode: - Classic / + Sharp / ++ Sharper.\n"
                    "- SPEED: scroll speed.\n"
                    "Hover to read frequency.");

            case ModuleType::spectrogram3d:
                return make (name,
                    "3D spectrum waterfall seen from above - recent spectra roll "
                    "back into the distance.\n"
                    "- TINT: colour peaks with the theme (off = blue-to-red heat map).\n"
                    "- SPEED: scroll speed.");

            case ModuleType::stereoField:
                return make (name,
                    "Stereo field radar. Direction = L / R balance, distance = level.\n"
                    "Auto-scales to the recent peak; the dB rings show the current scale.");

            case ModuleType::stereoMap:
                return make (name,
                    "Stereo image per frequency band.\n"
                    "- PAN: where each band sits (up = left, down = right).\n"
                    "- MONO: how much of each band survives a mono fold-down "
                    "(lower = more cancellation).\n"
                    "Hover for a ruler, click for a readout bubble.");

            case ModuleType::virtuPet:
                return make (name,
                    "An audio-powered pixel pet.\n"
                    "Play music to hatch the egg and keep it fed - long silence makes "
                    "it hungry. Drag it around to carry it.");

            case ModuleType::milkdrop:
                return make (name,
                    "MilkDrop visualiser (projectM) dancing to bass / mid / treble.\n"
                    "- Click it to show the control bar: presets, auto, color, "
                    "effects, wave, tweak.\n"
                    "- Keys: Left / Right = prev / next preset, Space = random.\n"
                    "- Heart (bottom right) saves the preset to your Likes.\n"
                    "- Switch button (bottom right) flips between all presets and your "
                    "Likes folder (Y2Kmeter/milkdrop_presets_like in your user data folder).");

            case ModuleType::tempoKey:
                return make (name,
                    "Real-time BPM and musical key detection.\n"
                    "- START / STOP, RESET the analysis window.\n"
                    "- Click BPM or KEY for a detail bubble.\n"
                    "The bar shows confidence. Runs only while this module exists.");

            case ModuleType::recorder:
                return make (name,
                    "Always-on recorder of the raw input (before gain), "
                    "32-bit float WAV.\n"
                    "- Red dot: start / stop a recording (up to 1 hour).\n"
                    "- SAVE LAST N s: grab the last N seconds from the 60 s buffer.\n"
                    "The folder opens after saving.");

            default:
                break;
        }

        return make (name, {});
    }

    juce::String moduleTitleUsage (bool isFloating, bool layoutLocked)
    {
        if (layoutLocked)
            return "Layout locked (L): moving, resizing and closing are disabled.";

        if (isFloating)
            return "Drag title: move window. Double-click title: full screen.";

        return "Drag title: move. Drag edges: resize.\n"
               "Double-click title: maximise / restore.\n"
               "Right-click: add modules or turn these tips off.";
    }
}

// ==========================================================
// Y2KTooltipWindow
// ==========================================================
Y2KTooltipWindow::Y2KTooltipWindow (juce::Component* parentComponent)
    : juce::TooltipWindow (parentComponent, kTipDelayMs)
{
    // 绘制交给 PinkXP LookAndFeel（drawTooltip / getTooltipBounds）
    setLookAndFeel (&getPinkXPLookAndFeel());

    // 提示框本身永不拦截鼠标：
    //   · 避免出现在光标下时抢走 hover（TooltipWindow::mouseEnter 会立即隐藏自己）；
    //   · getComponentUnderMouse 永远不会命中提示框本身。
    setInterceptsMouseClicks (false, false);

    // 低频回收"鼠标已离开本 peer"的旧提示（见 StaleTipGuard 注释）
    staleTipGuard.startTimerHz (8);
}

Y2KTooltipWindow::~Y2KTooltipWindow()
{
    staleTipGuard.stopTimer();
    setLookAndFeel (nullptr);
}

void Y2KTooltipWindow::StaleTipGuard::timerCallback()
{
    if (! owner.isVisible())
        return;

    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();

    // 鼠标在别的窗口（或已经不在任何组件上）：本提示窗口无法再被 JUCE
    //   的定时器更新，直接收起，避免留下上一个组件 / 模块的说明。
    if (under == nullptr || under->getPeer() != owner.getPeer())
        owner.hideTip();
}

juce::String Y2KTooltipWindow::getTipFor (juce::Component& c)
{
    if (! y2k::help::isEnabled())
        return {};

    // 拖拽 / 按下期间不出提示（与 JUCE 默认行为一致）
    if (juce::ModifierKeys::getCurrentModifiers().isAnyMouseButtonDown())
        return {};

    // Standalone 不在前台时不打扰（插件形态由宿主决定前后台，跳过此判断）
    if (juce::JUCEApplicationBase::isStandaloneApp() && ! juce::Process::isForegroundProcess())
        return {};

    if (c.isCurrentlyBlockedByAnotherModalComponent())
        return {};

    // 静止判定：仪表内容区的说明只在鼠标停住一会儿后出现
    const auto now   = juce::Time::getMillisecondCounter();
    const auto mouse = juce::Desktop::getMousePosition();
    if (mouse.getDistanceFrom (lastPollPos) > kStillTolerancePx)
    {
        lastPollPos  = mouse;
        stillSinceMs = now;
    }

    auto* host = getParentComponent();

    // 鼠标位置实际所在的模块：按屏幕坐标 + z-order 解析，**不依赖** JUCE 的
    //   "鼠标下组件"。macOS 上 Milkdrop 的控制栏是一个独立的子 NSWindow，鼠标
    //   落在它上面时主窗口收不到任何鼠标事件 → JUCE 的命中结果会一直停在
    //   上一个模块 / 控件上，于是"悬停 Milkdrop 的画面/颜色按钮却弹出别的
    //   模块介绍"（抬头的模块说明也一样）。
    //   浮动态各自的提示窗口没有 workspace 上下文，panelAtMouse 保持 nullptr，
    //   直接走组件链（浮动窗口本身就在鼠标下，命中是新鲜的）。
    ModulePanel* panelAtMouse = nullptr;
    if (auto* editor = dynamic_cast<Y2KmeterAudioProcessorEditor*> (host))
        if (auto* ws = editor->getWorkspace())
            panelAtMouse = ws->getModuleAtScreenPoint (mouse);

    auto tipForPanel = [&] (ModulePanel& panel) -> juce::String
    {
        // 模块自带原生提示窗口（macOS Milkdrop）：内容区（含控制栏）由模块
        //   自己的 overlay 轮询出提示 —— 这里再出只会重复，且 Editor 的提示
        //   绘制在原生 GL 视图之下本来就看不见。标题栏不在此范围（照常出提示）。
        if (panel.usesNativeOverlayTips()
            && panel.getContentBounds().contains (panel.getLocalPoint (nullptr, mouse)))
            return {};

        const auto info = panel.getHoverHelp (panel.getLocalPoint (nullptr, mouse));
        if (info.text.isEmpty())
            return {};

        if (info.contentZone && (now - stillSinceMs) < kContentStillMs)
            return {};

        return info.text;
    };

    for (auto* p = &c; p != nullptr; p = p->getParentComponent())
    {
        // 模块面板（含其任意子组件，如 Milkdrop 的 GLView、EQ 的像素图）
        //   注意：模块里的 juce 按钮 / 滑块本身是 TooltipClient，会在下面的分支
        //   先于面板命中；只有"没有文案的子组件 / 面板自身"才走到这里。
        if (auto* panel = dynamic_cast<ModulePanel*> (p))
        {
            // workspace 内有可信的坐标命中时以它为准（覆盖过期命中 / 模块重叠）
            if (panelAtMouse != nullptr)
                return tipForPanel (*panelAtMouse);

            // 浮动窗口：面板必须真的在鼠标下才采信
            if (panel->getScreenBounds().contains (mouse))
                return tipForPanel (*panel);

            return {};
        }

        if (auto* client = dynamic_cast<juce::TooltipClient*> (p))
        {
            // 过期命中保护：起点控件已经不在鼠标下时不能采信它的文案
            if (p->getScreenBounds().contains (mouse))
            {
                const auto tip = client->getTooltip();
                if (tip.isNotEmpty())
                    return tip;
            }
        }

        if (p == host)
            break;
    }

    // 组件链没走到模块（例如"鼠标下组件"仍停留在空白画布 / Editor 上）
    //   时，用屏幕坐标命中的模块兜底。
    if (panelAtMouse != nullptr)
        return tipForPanel (*panelAtMouse);

    return {};
}
