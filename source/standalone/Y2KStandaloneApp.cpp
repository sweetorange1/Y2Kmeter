// ==========================================================
// Y2KStandaloneApp.cpp
//   自定义 Standalone 入口，替换 JUCE 默认的 StandaloneFilterApp / StandaloneFilterWindow，
//   目的：摆脱 JUCE 自带的 Options 菜单栏 + 原生标题栏，改为完全无边框的 Y2K 风格窗口。
//
//   工作原理：
//     1) CMake 定义 JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
//        → juce_audio_plugin_client_Standalone.cpp 不再编译它的默认 App/Window
//        → 但仍然保留 JUCE_CREATE_APPLICATION_DEFINE 等宏依赖（见下方）
//     2) 我们自己实现一个 juce::JUCEApplication，并用 START_JUCE_APPLICATION 注册为入口
//     3) 内部复用官方的 StandalonePluginHolder —— 它负责：
//          · 实例化 Y2KmeterAudioProcessor（通过 createPluginFilter）
//          · 打开 AudioDeviceManager
//          · 把 processor 接到 AudioProcessorPlayer → 实现实时音频
//     4) 我们自己创建一个 DocumentWindow（setUsingNativeTitleBar(false) + titleBarHeight=0）
//        把 processor->createEditor() 返回的 Editor 作为 contentOwned 放进去
//        → 由 Editor 内部的 Y2K 关闭按钮 + 标题区拖拽来控制窗口
// ==========================================================

#include <juce_core/system/juce_TargetPlatform.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

#include <juce_audio_plugin_client/detail/juce_CheckSettingMacros.h>
#include <juce_audio_plugin_client/detail/juce_IncludeSystemHeaders.h>
#include <juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h>
#include <juce_audio_plugin_client/detail/juce_PluginUtilities.h>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

// 我们需要直接访问 Y2KmeterAudioProcessorEditor（填音频源下拉 + 订阅回调）
#include "../../PluginEditor.h"
#include "../../PluginProcessor.h"
#include "../analysis/AnalyserHub.h"

// 主题持久化：从 settings 读取/写回 PinkXP 全局主题 id
#include "../ui/PinkXPStyle.h"

// WASAPI Loopback 采集器（捕捉系统混音输出）
#include "WasapiLoopbackCapture.h"

// macOS 桌面音频采集器（ScreenCaptureKit）
#include "MacDesktopAudioCapture.h"
#include "AudioDumpRecorder.h"

namespace y2k
{

// ----------------------------------------------------------
// Y2KMainWindow
//   无原生标题栏、无边框；内容直接是 AudioProcessorEditor。
//   关闭请求统一走 JUCEApplication::quit()。
// ----------------------------------------------------------
class Y2KMainWindow  : public juce::DocumentWindow
{
public:
    Y2KMainWindow (const juce::String& name, juce::Colour bg)
        : juce::DocumentWindow (name,
                                bg,
                                juce::DocumentWindow::minimiseButton, // 按钮并不会画出，因为下面关闭了原生标题栏
                                true /*addToDesktop*/)
    {
        // 关键 1：关掉原生标题栏，不再显示系统标题/关闭/Options
        setUsingNativeTitleBar (false);
        // 关键 2：把 JUCE 自己的标题条高度设为 0 —— 这样 contentComponent 能占满整个窗口
        setTitleBarHeight (0);

        // 允许窗口缩放（若你希望完全固定尺寸，把 true 改成 false 即可）
        setResizable (true, false);

        // 关闭系统投影（我们自己画 Y2K 风格）
        setDropShadowEnabled (false);
    }

    // 用户点击系统任务栏"关闭"/ Alt+F4 / Editor 上的关闭按钮最终都会走这里
    void closeButtonPressed() override
    {
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Y2KMainWindow)
};

// ----------------------------------------------------------
// Y2KStandaloneApp
//   自定义 JUCEApplication：
//     - 构造 StandalonePluginHolder（复用 JUCE 的音频设备/播放管线）
//     - 创建 Y2KMainWindow 并把 Editor setContentOwned 进去
// ----------------------------------------------------------
class Y2KStandaloneApp  : public juce::JUCEApplication,
                         public juce::ChangeListener
{
public:
    Y2KStandaloneApp()
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = juce::CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName          = "~/.config";
       #else
        options.folderName          = "";
       #endif

        appProperties.setStorageParameters (options);
    }

    const juce::String getApplicationName()    override { return juce::CharPointer_UTF8 (JucePlugin_Name); }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed()          override { return true; }
    void anotherInstanceStarted (const juce::String&) override {}

    // --------------------------------------------------
    // 启动
    // --------------------------------------------------
    void initialise (const juce::String&) override
    {
        AudioDumpRecorder::instance().configureFromEnvironment();

        {
            auto& dump = AudioDumpRecorder::instance();
            if (dump.isEnabled())
            {
                const auto dir = dump.getSessionDirectory();
                if (dir != juce::File())
                {
                    const auto logFile = dir.getChildFile ("runtime.log");
                    runtimeFileLogger.reset (new juce::FileLogger (logFile,
                                                                    "Y2Kmeter runtime log",
                                                                    0));
                    juce::Logger::setCurrentLogger (runtimeFileLogger.get());
                    juce::Logger::writeToLog ("[Y2KStandaloneApp] runtime log file=" + logFile.getFullPathName());
                }
            }
        }

        // 1) 创建音频 + 插件宿主
        pluginHolder = std::make_unique<juce::StandalonePluginHolder> (

            appProperties.getUserSettings(),   // 设置持久化
            false,                             // takeOwnershipOfSettings
            juce::String{},                    // preferredDefaultDeviceName
            nullptr,                           // preferredSetupOptions
            juce::Array<juce::StandalonePluginHolder::PluginInOuts>{},
            false                              // shouldAutoOpenMidiDevices
        );

        // 1.0) 关键：关闭 JUCE Standalone 默认的"静音输入"开关
        //      StandalonePluginHolder 为了"防新手把麦克风 → 扬声器的反馈"，首次启动时把
        //      shouldMuteInput 默认置 true（桌面端 isInterAppAudioConnected() == false → !false = true）。
        //      后果：即便你选了任何输入设备，JUCE audio callback 在 line 563 会把
        //      inputChannelData 硬替换成零 buffer → processBlock 始终收到全 0 →
        //      AnalyserHub 没有信号，所有模块像死的一样。
        //      我们这边 Processor 已经在 processBlock 里强制清空输出 buffer（杜绝回放），
        //      不再需要这层"保险"，所以这里强制关掉。
        //      同时显式写原子 muteInput（shouldMuteInput 是 UI Value，通过 valueChanged
        //      异步写 muteInput；在第一次回调到来前我们直接写原子位以绝后患）。
        pluginHolder->shouldMuteInput.setValue (false);
        pluginHolder->muteInput.store (false);

        // 1.1) 防止 processor 的输出回路造成反馈：Holder 默认把 processor 的输出直接送回
        //      默认扬声器。我们用 muteInput=true 让 Holder 静音"送进 processor 的输入"，
        //      同时 Processor::processBlock 里也已显式清空所有输出 buffer → 双保险：
        //      扬声器/耳机不会再接收到 processor 产生的任何声音（防止 loopback 模式下的循环）。
        //      注：这里 shouldMuteInput 影响的是"喂给 processor 的 input buffer"，
        //         我们真正依赖的其实是 Processor::processBlock 的输出清零；muteInput 同时
        //         开启可避免 AudioDeviceManager 的硬件输入被 Player 回灌到 processor。
        //         —— 但普通模式下我们仍要走硬件输入 → 不能全局 mute。
        //      折中：保持 shouldMuteInput=false（默认），依赖 Processor 输出清零来断反馈。

        // 1.15) 恢复上次选择的 UI 主题（PinkXP ThemeId）
        //       · 必须在 createEditor() 之前应用：否则 Editor 构造里所有子组件会先
        //         用默认 bubblegum 配色构建一遍，再被主题订阅回调刷新——中间会有一
        //         帧明显的"先粉后变色"，且部分缓存（LookAndFeel ColourScheme 等）
        //         以构造时的配色固化后需要额外 sendLookAndFeelChange 才能彻底刷新。
        //       · settings 里存整数（强转 ThemeId），缺键/越界兜底为 bubblegum。
        //       · ThemeId 枚举的数值顺序不会被动（新主题追加到末尾），因此存值稳定。
        {
            const int savedThemeRaw = getUserSettings()
                .getIntValue ("ui.themeId", (int) PinkXP::ThemeId::bubblegum);
            const auto& themes = PinkXP::getAllThemes();
            PinkXP::ThemeId targetId = PinkXP::ThemeId::bubblegum;
            for (const auto& t : themes)
            {
                if ((int) t.id == savedThemeRaw) { targetId = t.id; break; }
            }
            PinkXP::applyTheme (targetId);
        }

        // 1.16) 读取上次命中的虚拟回环设备名（非 Windows 下 Output 模式优先复用）
        lastVirtualLoopbackInputName = getUserSettings().getValue ("audio.virtualLoopbackInputName", {});

        // 1.2) 关键：在 createEditor 之前把上一次保存的 plugin state 恢复到 Processor。

        //      StandalonePluginHolder::reloadPluginState() 会读 settings 里的 "filterState"
        //      并调 processor->setStateInformation() → 反序列化 savedLayoutXml。
        //      只有这样 Editor 构造里的 loadInitialModules() 才能读到 savedLayoutXml，
        //      走"恢复上次布局"分支而不是默认瀑布布局。
        pluginHolder->reloadPluginState();

        // 2) 构建无边框窗口
        //    背景色用 LookAndFeel 默认的窗口底色（实际看不到，因为 Editor 会占满）
        const auto bg = juce::LookAndFeel::getDefaultLookAndFeel()
                             .findColour (juce::ResizableWindow::backgroundColourId);

        mainWindow = std::make_unique<Y2KMainWindow> (getApplicationName(), bg);

        // 3) 取插件自己的 Editor 作为窗口内容
        //    关键：使用 setContentNonOwned —— 由本 App 管理 editor 的生命周期。
        //    因为 AudioProcessorEditor 析构时会断言 processor.getActiveEditor() != this，
        //    必须在 delete editor 之前显式调用 processor->editorBeingDeleted(editor)
        //    让 processor 清掉它内部的 activeEditor 指针。
        if (auto* proc = pluginHolder->processor.get())
        {
            if (auto* editor = proc->createEditorIfNeeded())
            {
                pluginEditor.reset (editor);
                mainWindow->setContentNonOwned (editor, /*resizeToFit*/ true);

                // 4) 填充 Editor 底部的"音频源"下拉框
                //    · 首项为 System Output (Loopback) 占位（真实切换后续任务实现）
                //    · 其后枚举所有输入设备（名称 + 设备类型名）
                if (auto* y2kEditor = dynamic_cast<Y2KmeterAudioProcessorEditor*> (editor))
                {
                    cachedEditor = y2kEditor;
                    populateAudioSources (*y2kEditor);

                    y2kEditor->onAudioSourceChanged = [this](const juce::String& sourceId,
                                                             bool isLoopback)
                    {
                        handleAudioSourceChanged (sourceId, isLoopback);
                    };

                    // 预设 Save/Load —— 由 ModuleWorkspace 的 Save/Load 按钮触发，
                    //   Editor 透传过来。真正的 settings 文件读写只能在 Standalone App
                    //   这边做（PropertiesFile 的 storage 参数归这里持有）。
                    y2kEditor->onSaveSettingsRequested = [this](juce::File dest)
                    {
                        handleExportSettings (dest);
                    };
                    y2kEditor->onLoadSettingsRequested = [this](juce::File src)
                    {
                        handleImportSettings (src);
                    };

                    // 监听 AudioDeviceManager 变化（包括外部 Options 面板、
                    //   热插拔等）→ 自动刷新下拉框内容与选中项
                    if (pluginHolder != nullptr)
                        pluginHolder->deviceManager.addChangeListener (this);

                    // 4.1) 恢复上次的 audio source。
                    //      不在这里预先过滤 loopback：
                    //      · 让下拉框始终保留 Output 语义选项（尤其是 macOS）；
                    //      · 真正切换时由 handleAudioSourceChanged 内部判定可用性，
                    //        不可用则静默回退到 Microphone。
                    auto savedSource = getUserSettings()
                        .getValue ("audio.sourceId", "loopback:default");

                    const bool savedLoopback = savedSource.startsWith ("loopback");
                    handleAudioSourceChanged (savedSource, savedLoopback);
                    syncDropdownToCurrentDevice (*y2kEditor);

                    // 4.2) 恢复 chrome（标题栏+底部 toolbar）可见性（默认可见）
                    const bool savedChrome = getUserSettings()
                        .getBoolValue ("ui.chromeVisible", true);
                    y2kEditor->setChromeVisible (savedChrome);

                    // 4.3) 恢复"固定置顶"（pin）按钮状态（默认 true — 首次启动就置顶）
                    //     · 用 setAlwaysOnTopActive 统一接口，内部会显式强推一次
                    //       setAlwaysOnTop(false)→setAlwaysOnTop(true) 绕开 JUCE 的
                    //       flag 早退，保证按钮视觉态与系统 HWND_TOPMOST 真实一致。
                    //     · 此时 editor 尚未 setVisible(true)，顶层是 mainWindow，
                    //       getTopLevelComponent() 能返回到 Y2KMainWindow。
                    const bool savedAlwaysOnTop = getUserSettings()
                        .getBoolValue ("ui.alwaysOnTop", true);
                    y2kEditor->setAlwaysOnTopActive (savedAlwaysOnTop);
                }
            }
        }

        // 5) 恢复窗口 bounds（若 settings 里有合法记录且落在当前桌面范围内）
        if (! restoreMainWindowBounds())
            mainWindow->centreWithSize (mainWindow->getWidth(), mainWindow->getHeight());

        mainWindow->setVisible (true);
    }

    // --------------------------------------------------
    // 退出
    // --------------------------------------------------
    void shutdown() override
    {
        // -2) 先把所有可持久化的 UI/音频状态写入 settings
        //     persistAllSettings 带幂等守卫：若 systemRequestedQuit 已经跑过一次，
        //     这里就是 no-op；否则（例如直接 quit 路径）兜底保存一次。
        //     绝对不能在这里只 sanitize 不 save —— 否则会把上一次写好的 filterState
        //     擦掉却没人补写，导致下次启动恢复到默认布局。
        persistAllSettings();

        // -1) 停掉 Loopback / mac 桌面采集线程（若在跑）
        stopLoopbackCapture();
        stopMacDesktopAudioCapture();

        AudioDumpRecorder::instance().flushSummary();

        // 0) 先取消对 AudioDeviceManager 的监听（它可能比 this 先析构）

        if (pluginHolder != nullptr)
            pluginHolder->deviceManager.removeChangeListener (this);
        cachedEditor = nullptr;

        // 1) 先让窗口不再持有 editor 的引用（setContentNonOwned 不拥有，这里只是解绑显示）
        if (mainWindow != nullptr)
            mainWindow->clearContentComponent();

        // 2) 在 delete editor 之前，通知 processor 清空 activeEditor 指针
        //    否则会触发 jassert(processor.getActiveEditor() != this)
        if (pluginEditor != nullptr && pluginHolder != nullptr && pluginHolder->processor != nullptr)
            pluginHolder->processor->editorBeingDeleted (pluginEditor.get());

        pluginEditor = nullptr;   // 此时 delete editor 安全

        // 3) 关窗口
        mainWindow = nullptr;

        // 4) 停止音频 + 销毁 processor
        pluginHolder = nullptr;

        juce::Logger::setCurrentLogger (nullptr);
        runtimeFileLogger.reset();

        appProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        // 用户点 × 时的体验优化：立刻让窗口"消失"，后台异步做清理
        //   关闭真实卡顿来自 pluginHolder 析构里的 closeAudioDevice()（WASAPI 50~200ms），
        //   以及可能的 Loopback 线程 join。把这些耗时放到 shutdown() 里慢慢做，
        //   但在 user 点击的这一刻先把窗口 hide，让感官上 0 延迟。

        // 1) 一次性把所有持久化项写入 settings（带幂等守卫）——
        //    顺序：sanitize 清 → 窗口 bounds → UI/音频 → plugin state → 立刻刷盘。
        //    必须在 setVisible(false) 之前，否则某些 Win 版本上 hide 后
        //    getBounds 会变成 minimized 的值。
        persistAllSettings();

        // 3) 立刻把窗口隐藏 / 移出 alwaysOnTop，让用户感觉关闭是瞬时的
        if (mainWindow != nullptr)
        {
            // 取消置顶（如果开了固定按钮），避免隐藏时被 z-order 问题卡住
            mainWindow->setAlwaysOnTop (false);
            mainWindow->setVisible (false);
        }

        // 4) 走标准退出流程（有模态窗口先关模态，100ms 后重试一次）
        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            juce::Timer::callAfterDelay (100, []()
            {
                if (auto app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        }
        else
        {
            quit();
        }
    }

private:
    // ----------------------------------------------------
    // settings 便捷访问
    // ----------------------------------------------------
    juce::PropertiesFile& getUserSettings()
    {
        // PropertiesFile 保证在 setStorageParameters 后始终返回有效指针
        return *appProperties.getUserSettings();
    }

    // ----------------------------------------------------
    // 恢复主窗口 bounds：
    //   读 settings 里的 window.{x,y,w,h}，若四项都存在且落在某个屏幕显示区内，
    //   则直接 setBounds 恢复；否则返回 false 让调用方走 centreWithSize 兜底。
    //   · 不保存 maximized / fullscreen 状态（按用户设计此软件不会最大化）。
    //   · 对多屏拔掉 / 分辨率变化的情况做越界修正：整窗至少 80% 面积在主屏内。
    // ----------------------------------------------------
    bool restoreMainWindowBounds()
    {
        if (mainWindow == nullptr) return false;

        auto& s = getUserSettings();
        if (! (s.containsKey ("window.x") && s.containsKey ("window.y")
            && s.containsKey ("window.w") && s.containsKey ("window.h")))
            return false;

        const int x = s.getIntValue ("window.x",  0);
        const int y = s.getIntValue ("window.y",  0);
        const int w = s.getIntValue ("window.w", -1);
        const int h = s.getIntValue ("window.h", -1);

        // 基础合法性检查：必须大于 Editor 最小尺寸下限
        if (w < 320 || h < 240) return false;

        juce::Rectangle<int> target (x, y, w, h);

        // 检查目标矩形是否与任一屏幕显示区有足够交集（>50%），否则判定为"屏幕已不在"
        const auto& disp = juce::Desktop::getInstance().getDisplays();
        bool onScreen = false;
        for (const auto& d : disp.displays)
        {
            const auto inter = d.userArea.getIntersection (target);
            if (inter.getWidth() * inter.getHeight()
                >= (target.getWidth() * target.getHeight()) / 2)
            {
                onScreen = true;
                break;
            }
        }
        if (! onScreen) return false;

        mainWindow->setBounds (target);
        return true;
    }

    // 关闭前保存窗口 bounds 到 settings
    void saveMainWindowBounds()
    {
        if (mainWindow == nullptr) return;

        // 只有当窗口当前不是最小化状态时才记录（避免记到最小化的怪异 bounds）
        if (mainWindow->isMinimised()) return;

        // 若窗口已经被 hide（shutdown 阶段的二次调用），跳过 —— 此时 getBounds
        // 在某些 Windows 版本上会返回错误值，而真正的 save 已经在
        // systemRequestedQuit() 里做过了
        if (! mainWindow->isVisible()) return;

        const auto b = mainWindow->getBounds();
        auto& s = getUserSettings();
        // 先显式 remove，再 setValue —— 等价于"用当前值覆盖"，彻底剔除任何
        //   由老版本留下的同名脏值（例如 Win32 下极少数情况下 PropertyFile
        //   把同一 key 写成了格式错误的字符串，再读时会得到 0）。
        s.removeValue ("window.x");
        s.removeValue ("window.y");
        s.removeValue ("window.w");
        s.removeValue ("window.h");
        s.setValue ("window.x", b.getX());
        s.setValue ("window.y", b.getY());
        s.setValue ("window.w", b.getWidth());
        s.setValue ("window.h", b.getHeight());
    }

    // ----------------------------------------------------
    // 清洗 settings：删除已被本版本淘汰的老 key + 清空本版本管理的所有 key，
    //   然后由 saveUiAndAudioState / savePluginState / saveMainWindowBounds
    //   用当前值重写一遍。这样每次关闭得到的 settings 都是一份干净的"本版本
    //   标准快照"，不会被老版本可能留下的脏数据 / 格式变更 / 残留字段影响。
    //
    //   注：JUCE 内部 StandalonePluginHolder 维护的 "audioSetup" 等字段是
    //   该框架自行管理的，我们**不碰**，避免重置用户的音频设备选择。
    // ----------------------------------------------------
    void sanitizeLegacySettings()
    {
        auto& s = getUserSettings();

        // ---- 1) 清除任何本版本已经不再使用（但可能在老版本存在）的旧 key ----
        //        将来版本如果需要淘汰新的 key，在下面追加 s.removeValue("...") 即可。
        //        MSVC 不支持零长度数组，所以这里用直列 removeValue 语句的形式（即
        //        使目前没有需要清的老 key，也不会产生任何运行时开销 —— PropertyFile
        //        对不存在的 key 调 removeValue 是 no-op）。
        //        示例：
        //          s.removeValue ("ui.oldFoo");
        //          s.removeValue ("audio.oldBar");
        // （本版本暂无需要淘汰的老 key）

        // ---- 2) 清除本版本管理的 key —— 随后会由保存函数写入最新值 ----
        const char* const managedKeys[] = {
            "settings.schemaVersion",
            "window.x", "window.y", "window.w", "window.h",
            "ui.chromeVisible",
            "ui.alwaysOnTop",
            "ui.themeId",
            "audio.sourceId",
            "filterState"     // 插件状态（EQ/布局 XML）— 保证用当前值重写
        };
        for (auto* k : managedKeys)
            s.removeValue (k);

        // ---- 3) 写入本版本 schema 标记（未来可据此识别老存档，做迁移/丢弃）
        s.setValue ("settings.schemaVersion", 1);
    }

    // ----------------------------------------------------
    // 把所有需要持久化的设置一次性落盘。带"已执行过"守卫 —— 无论走
    // systemRequestedQuit（用户点 ×）还是 shutdown（JUCE 应用退出）
    // 还是两条都走，本函数里的完整序列只会发生一次，保证：
    //   sanitize（清掉所有本版本管理的 key + 可能的老版本脏数据）
    //     → 窗口 bounds / UI / 音频源 / 主题 重写
    //     → plugin state（EQ/布局 XML）重写
    //     → 立刻 saveIfNeeded 刷盘
    // 是一组"原子的覆盖写入"，绝不会出现"sanitize 跑了两次却只 save 一次
    // filterState"这种中间态 —— 那正是上个版本导致"每次重开回到默认状态"的 bug。
    // ----------------------------------------------------
    void persistAllSettings()
    {
        if (alreadyPersisted) return;
        alreadyPersisted = true;

        flushCurrentStateToDisk();
    }

    // 不带幂等守卫的"立刻把当前运行时状态刷到磁盘" —— 用于 Save 预设等需要
    // 在程序仍在运行时把 settings 文件拷贝出去的场景。调完后用户还可以继续
    // 操作，随后的正常关闭路径仍能再刷一次。
    void flushCurrentStateToDisk()
    {
        sanitizeLegacySettings();
        saveMainWindowBounds();
        saveUiAndAudioState();

        if (pluginHolder != nullptr && pluginHolder->processor != nullptr)
            pluginHolder->savePluginState();

        appProperties.saveIfNeeded();
    }

    // ----------------------------------------------------
    // 预设导出：把当前 settings 文件"另存为"到用户指定路径
    //   · 完全静默 —— 不弹任何 AlertWindow（用户要求）。
    //   · 失败时仅通过 DBG 输出日志；成功时不做任何可见反馈。
    //   · 步骤：先把当前运行时状态刷到物理文件 → File::copyFileTo。
    // ----------------------------------------------------
    void handleExportSettings (juce::File dest)
    {
        if (dest == juce::File{}) return;

        // 1) 先把当前运行时状态全部落到 PropertiesFile 的物理文件，
        //    保证导出快照包含用户所有未落盘的最新修改
        flushCurrentStateToDisk();

        // 2) 拿到 PropertiesFile 的实际磁盘文件
        auto* props = appProperties.getUserSettings();
        const juce::File src = (props != nullptr ? props->getFile() : juce::File{});
        if (src == juce::File{} || ! src.existsAsFile())
        {
            DBG ("handleExportSettings: settings file does not exist on disk yet");
            return;
        }

        // 3) 复制到目标路径（父目录不存在则自动创建；同名覆盖由 FileChooser
        //    的 warnAboutOverwriting 确认过，这里直接覆盖）
        dest.getParentDirectory().createDirectory();
        const bool ok = src.copyFileTo (dest);
        if (! ok)
            DBG ("handleExportSettings: copy failed → " + dest.getFullPathName());
    }

    // ----------------------------------------------------
    // 预设导入：把用户指定的 .settings 文件覆盖当前 settings 并"自动重启"
    //   · 完全静默 —— 不弹任何 AlertWindow（用户要求）。
    //   · 采用"物理覆盖 + 自重启"方案：settings 里涉及的 runtime 状态
    //     （主题、布局 XML、窗口 bounds、chrome、alwaysOnTop、音频源、
    //      FPS 等）在 initialise() 里散落着十几条恢复路径，运行时热重载
    //     复杂且容易遗漏，直接"启动新实例 → quit 旧实例"最可靠 ——
    //     语义等价于"干净首次打开此 settings"。
    //   · 自重启实现：juce::Process::openDocument(currentExecutableFile, "")
    //     在 Windows 下走 ShellExecute，启动的新进程完全独立于当前进程，
    //     父进程 quit 后新进程照常存活。macOS/Linux 下 openDocument 同样
    //     是"后台启动、不阻塞、不继承生命周期"，跨平台一致。
    //   · 为防止 shutdown 里的 flushCurrentStateToDisk 把刚导入的文件
    //     又用"当前内存运行时状态"覆盖掉，复制完成后立即把
    //     alreadyPersisted=true，锁死 persistAllSettings 成 no-op。
    // ----------------------------------------------------
    void handleImportSettings (juce::File src)
    {
        if (src == juce::File{} || ! src.existsAsFile())
        {
            DBG ("handleImportSettings: selected file does not exist");
            return;
        }

        auto* props = appProperties.getUserSettings();
        const juce::File dest = (props != nullptr ? props->getFile() : juce::File{});
        if (dest == juce::File{})
        {
            DBG ("handleImportSettings: cannot locate settings file on disk");
            return;
        }

        // 用户选中的就是当前 settings 文件本身 —— 无需任何动作，也不重启
        if (src == dest)
        {
            DBG ("handleImportSettings: selected file is the current settings; nothing to do");
            return;
        }

        // 1) 先把 PropertiesFile 内存中的 pending save 刷一次（此时写入的
        //    还是"旧"的 key/value 到旧 dest 路径）—— 确保 needsToSave 被清零，
        //    后续我们要做的物理覆盖不会被 PropertiesFile 的析构 flush 抢跑。
        props->saveIfNeeded();

        // 2) 物理覆盖 settings 文件
        const bool ok = src.copyFileTo (dest);
        if (! ok)
        {
            DBG ("handleImportSettings: overwrite failed → " + dest.getFullPathName());
            return;
        }

        // 3) 锁死 persistAllSettings / 任何后续 flushCurrentStateToDisk ——
        //    不让关闭路径把当前内存运行时状态再写回去覆盖刚导入的文件。
        alreadyPersisted = true;

        // 4) 启动一个新的自己（独立进程），然后 quit 当前进程 ——
        //    新进程在 initialise() 里走完整的 settings 恢复路径。
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        if (exe.existsAsFile())
            juce::Process::openDocument (exe.getFullPathName(), {});
        else
            DBG ("handleImportSettings: cannot locate current executable; restart manually");

        juce::JUCEApplication::quit();
    }

    // 关闭前保存 UI（chrome 可见性 + 固定置顶状态）与音频源（上次选择的 sourceId）
    void saveUiAndAudioState()
    {
        auto& s = getUserSettings();

        if (cachedEditor != nullptr)
        {
            s.setValue ("ui.chromeVisible",  cachedEditor->isChromeVisible());
            // 保存"固定置顶"按钮当前状态，下次启动按此恢复（默认 true）
            s.setValue ("ui.alwaysOnTop",    cachedEditor->isAlwaysOnTopActive());
        }

        // getCurrentSourceId 已经封装了 loopback vs mic 的判定
        s.setValue ("audio.sourceId", getCurrentSourceId());
        s.setValue ("audio.virtualLoopbackInputName", lastVirtualLoopbackInputName);

        // 保存当前 UI 主题（PinkXP ThemeId，存整数枚举值）——下次启动时由

        //   initialise() 里的 1.15) 分支读取并 applyTheme 恢复。
        //   · 主题在运行时被 ThemeSwatchBar::mouseDown → PinkXP::applyTheme 修改；
        //     这里只是在退出前快照一次 gCurrentThemeId。
        s.setValue ("ui.themeId", (int) PinkXP::getCurrentThemeId());
    }

    // ----------------------------------------------------
    // 音频源下拉：只暴露语义化选项，避免把 N 个后端 × M 块物理设备的
    // 笛卡尔积全部罗列出来造成困扰。
    //
    //   1) Microphone            —— 系统当前默认输入设备
    //                              sourceId = "input:default"
    //   2) Output                —— 系统输出回环（Windows: WASAPI；
    //                              macOS/Linux: 虚拟回环输入设备）
    //                              sourceId = "loopback:default"
    //
    // 非 Windows 下也始终展示 Output；
    // 若系统当前没有可用虚拟回环输入设备，用户选择后会在切换逻辑里
    // 自动回退到 Microphone（不阻断 UI 入口）。
    // ----------------------------------------------------
    juce::String findPreferredVirtualLoopbackInputName() const
    {
       #if JUCE_WINDOWS
        return {};
       #else
        if (pluginHolder == nullptr) return {};
        auto& adm = pluginHolder->deviceManager;

        juce::StringArray inputNames;
        if (auto* type = adm.getCurrentDeviceTypeObject())
        {
            type->scanForDevices();
            inputNames = type->getDeviceNames (true /*wantInputNames*/);
        }
        if (inputNames.isEmpty())
            return {};

        auto hasInputName = [&inputNames] (const juce::String& name)
        {
            for (const auto& n : inputNames)
                if (n == name)
                    return true;
            return false;
        };

        // 1) 优先复用上一次成功命中的设备
        if (lastVirtualLoopbackInputName.isNotEmpty() && hasInputName (lastVirtualLoopbackInputName))
            return lastVirtualLoopbackInputName;

        // 2) 按主流软件常见虚拟回环设备关键词匹配（BlackHole/Loopback/Background Music/...）
        const juce::StringArray preferredKeywords {
            "blackhole", "loopback", "soundflower", "monitor", "vb-cable", "cable output", "ishowu",
            "background music", "sound siphon", "audiomovers", "virtual"
        };

        for (const auto& name : inputNames)
            for (const auto& key : preferredKeywords)
                if (name.containsIgnoreCase (key))
                    return name;

        // 3) 兜底：挑一个"不像物理麦克风"的输入，提升无关键词场景下的可用性
        const juce::StringArray likelyPhysicalMicKeywords {
            "microphone", "mic", "built-in", "macbook", "airpods", "headset", "webcam", "camera", "line in",
            "内建", "麦克风", "耳机", "摄像"
        };

        for (const auto& name : inputNames)
        {
            bool looksPhysicalMic = false;
            for (const auto& k : likelyPhysicalMicKeywords)
            {
                if (name.containsIgnoreCase (k))
                {
                    looksPhysicalMic = true;
                    break;
                }
            }

            if (! looksPhysicalMic)
                return name;
        }

        return {};
       #endif
    }

    bool hasAnyVirtualLoopbackInputDevice() const
    {
       #if JUCE_WINDOWS
        return true; // Windows 始终可尝试 WASAPI loopback
       #else
        return findPreferredVirtualLoopbackInputName().isNotEmpty();
       #endif
    }

    void populateAudioSources (Y2KmeterAudioProcessorEditor& editor)
    {
        juce::Array<Y2KmeterAudioProcessorEditor::AudioSourceEntry> items;

        // 选项 1：麦克风输入（系统默认输入设备）
        items.add ({ "Microphone",
                     "input:default",
                     false });

       #if JUCE_WINDOWS
        // 选项 2：系统输出回环（WASAPI）
        items.add ({ "Output",
                     "loopback:default",
                     true });
       #else
        // macOS / Linux：始终展示 Output，具体可用性在切换时判定并回退
        items.add ({ "Output",
                     "loopback:default",
                     true });
       #endif

        editor.setAudioSourceItems (items, getCurrentSourceId());
    }

    // 拼装"当前正在使用的音频源"的 sourceId（只会返回两种语义化值之一）
    juce::String getCurrentSourceId() const
    {
        if (loopbackActive.load (std::memory_order_acquire)
            || virtualLoopbackInputActive.load (std::memory_order_acquire))
            return "loopback:default";

        return "input:default";
    }

    // ----------------------------------------------------
    // 用户在下拉里选择了一个音频源：
    //   · "loopback:default" → 启动 WasapiLoopbackCapture 捕捉默认渲染端点；
    //                         同时关闭 AudioDeviceManager 的物理设备（避免双路 push）
    //   · "input:default"    → 停掉 Loopback（若在跑），打开系统默认输入设备
    //                         （让 AudioDeviceManager 自己挑当前 deviceType 下的默认）
    // ----------------------------------------------------
    void handleAudioSourceChanged (const juce::String& sourceId, bool isLoopback)
    {
        DBG (juce::String ("[Y2K] Audio source changed -> ") + sourceId);

        if (pluginHolder == nullptr) return;

        if (isLoopback)
        {
           #if JUCE_WINDOWS
            // 1) 先停硬件输入 → AudioDeviceManager 把输入通道清零
            //    （保留输出设备不变；setAudioDeviceSetup 的 err 我们不强求成功）
            disableHardwareInput();

            // 2) 启动 Loopback 采集线程
            if (! startLoopbackCapture())
            {
                juce::AlertWindow::showAsync (
                    juce::MessageBoxOptions()
                        .withIconType (juce::MessageBoxIconType::WarningIcon)
                        .withTitle ("System Loopback error")
                        .withMessage (juce::String ("Failed to start system output loopback:\n")
                                        + (loopback != nullptr ? loopback->getLastError() : juce::String ("(unknown)"))
                                        + "\n\nFalling back to Microphone.")
                        .withButton ("OK"),
                    nullptr);

                // 回退到"麦克风输入"
                enableDefaultInput();
                if (cachedEditor != nullptr)
                    syncDropdownToCurrentDevice (*cachedEditor);
                return;
            }

            DBG ("[Y2K] WASAPI Loopback capture started.");
            return;
           #else
            // macOS：优先使用 ScreenCaptureKit 直接采集系统输出音频（不依赖 BlackHole）。
            // Linux：暂不支持直接系统输出采集，继续使用虚拟回环输入设备方案。
            stopLoopbackCapture();

           #if JUCE_MAC
            if (startMacDesktopAudioCapture())
            {
                DBG ("[Y2K] macOS desktop audio capture started.");
                return;
            }

            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("System Output capture unavailable")
                    .withMessage (juce::String ("Failed to start macOS desktop audio capture:\n")
                                    + (macDesktopCapture != nullptr ? macDesktopCapture->getLastError() : juce::String ("(unknown)"))
                                    + "\n\nFalling back to Microphone.")
                    .withButton ("OK"),
                nullptr);

            enableDefaultInput();
            if (cachedEditor != nullptr)
                syncDropdownToCurrentDevice (*cachedEditor);
            return;
           #else
            // Linux：仍通过虚拟回环输入设备实现 Output。
            if (enablePreferredVirtualLoopbackInput())
            {
                DBG ("[Y2K] Virtual loopback input enabled.");
                return;
            }

            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("System Loopback unavailable")
                    .withMessage (
                        "A virtual loopback input device exists, but failed to open.\n\n"
                        "Please verify your loopback app routing and audio permissions, then try again.\n\n"
                        "Falling back to Microphone.")
                    .withButton ("OK"),
                nullptr);

            enableDefaultInput();
            if (cachedEditor != nullptr)
                syncDropdownToCurrentDevice (*cachedEditor);
            return;
           #endif
           #endif

        }

        // "Microphone Input" → 先停 Loopback，再启用默认输入设备
        stopLoopbackCapture();
        stopMacDesktopAudioCapture();
        virtualLoopbackInputActive.store (false, std::memory_order_release);
        enableDefaultInput();

    }

    // ----------------------------------------------------
    // 非 Windows 下的 "Output" 回退方案：选择虚拟回环输入设备
    //   典型设备名关键词：BlackHole / Loopback / Soundflower / Monitor / VB-Cable。
    // ----------------------------------------------------
    bool enablePreferredVirtualLoopbackInput()
    {
        if (pluginHolder == nullptr) return false;
        auto& adm = pluginHolder->deviceManager;

        attachHolderCallback();

        const juce::String picked = findPreferredVirtualLoopbackInputName();
        if (picked.isEmpty())
            return false;

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        adm.getAudioDeviceSetup (setup);
        setup.inputDeviceName         = picked;
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.inputChannels.setRange (0, 2, true);   // 先尝试 stereo

        auto err = adm.setAudioDeviceSetup (setup, true);
        if (err.isNotEmpty())
        {
            setup.inputChannels.clear();
            setup.inputChannels.setRange (0, 1, true); // mono 兜底
            err = adm.setAudioDeviceSetup (setup, true);
        }
        if (err.isNotEmpty())
        {
            setup.useDefaultInputChannels = true;       // 再兜底交给系统
            setup.inputChannels.clear();
            err = adm.setAudioDeviceSetup (setup, true);
        }
        if (err.isNotEmpty())
        {
            DBG (juce::String ("[Y2K] Failed to enable virtual loopback input: ") + err);
            return false;
        }

        pluginHolder->shouldMuteInput.setValue (false);
        pluginHolder->muteInput.store (false);

        lastVirtualLoopbackInputName = picked;
        virtualLoopbackInputActive.store (true, std::memory_order_release);

        DBG (juce::String ("[Y2K] Virtual loopback input device = ") + picked);
        return true;
    }

    // ----------------------------------------------------
    // 打开系统默认输入设备（当前 deviceType 下）——
    //   · 必须**显式**把 deviceType 下的默认输入设备名写进 setup.inputDeviceName
    //     否则 JUCE 的 setAudioDeviceSetup 在 inputDeviceName.isEmpty() 时会主动
    //     channels.clear()（见 juce_AudioDeviceManager::updateSetupChannels），
    //     导致我们指定的 stereo bitmask 被抹掉 → 设备确实打开了但 active input channels = 0
    //     → processBlock 收到全 0 → 模块一动不动。
    //   · 通道 bitmask 先试 stereo；若物理设备只有 1 路输入则退化回 mono。
    // ----------------------------------------------------
    void enableDefaultInput()

    {
        if (pluginHolder == nullptr) return;
        auto& adm = pluginHolder->deviceManager;

        // 如果刚才是 Loopback 模式，audio device 可能已被 closeAudioDevice() 关掉；
        // 这里先确保 holder callback 已恢复
        attachHolderCallback();

        // 1) 解析当前 deviceType 下的"默认输入设备名"
        juce::String defaultInputName;
        if (auto* type = adm.getCurrentDeviceTypeObject())
        {
            type->scanForDevices();
            const auto names = type->getDeviceNames (true /*wantInputNames*/);
            const int  idx   = type->getDefaultDeviceIndex (true);
            if (juce::isPositiveAndBelow (idx, names.size()))
                defaultInputName = names[idx];
            else if (! names.isEmpty())
                defaultInputName = names[0];   // 兜底：取列表第一个
        }

        if (defaultInputName.isEmpty())
        {
            DBG ("[Y2K] enableDefaultInput: no input device available for current deviceType");
            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("Audio input error")
                    .withMessage ("No input device available.\n"
                                  "Please connect a microphone / audio interface, "
                                  "or enable 'Stereo Mix' in Windows Sound settings.")
                    .withButton ("OK"),
                nullptr);
            return;
        }

        // 2) 把设备名 + 通道 bitmask 写进 setup
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        adm.getAudioDeviceSetup (setup);
        setup.inputDeviceName         = defaultInputName;
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.inputChannels.setRange (0, 2, true);   // 前 2 ch（stereo）

        auto err = adm.setAudioDeviceSetup (setup, true);
        if (err.isNotEmpty())
        {
            // 有些设备（例如 mono 麦克风）不支持 2 bit 通道 mask → 退回 1 ch
            DBG (juce::String ("[Y2K] enableDefaultInput stereo failed: ") + err + "; retrying mono");
            setup.inputChannels.clear();
            setup.inputChannels.setRange (0, 1, true);
            err = adm.setAudioDeviceSetup (setup, true);
        }
        if (err.isNotEmpty())
        {
            // 再退一步：彻底让 JUCE 自己挑（useDefaultInputChannels=true）
            DBG (juce::String ("[Y2K] enableDefaultInput mono failed: ") + err + "; retrying with default channels");
            setup.useDefaultInputChannels = true;
            setup.inputChannels.clear();
            err = adm.setAudioDeviceSetup (setup, true);
        }
        if (err.isNotEmpty())
        {
            DBG (juce::String ("[Y2K] Failed to enable default input: ") + err);
            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("Audio input error")
                    .withMessage (juce::String ("Failed to open the default input device:\n") + err)
                    .withButton ("OK"),
                nullptr);
            return;
        }

        // 3) 双保险：再次确保 muteInput 未被偷偷置位
        pluginHolder->shouldMuteInput.setValue (false);
        pluginHolder->muteInput.store (false);

        // 切回默认输入后清理虚拟回环状态
        virtualLoopbackInputActive.store (false, std::memory_order_release);

        // 4) 调试日志 —— 确认 active input channels > 0

        if (auto* dev = adm.getCurrentAudioDevice())
        {
            const int nin = dev->getActiveInputChannels().countNumberOfSetBits();
            DBG (juce::String ("[Y2K] Default input device = ") + dev->getName()
                  + ", active in channels = " + juce::String (nin)
                  + ", sr="                    + juce::String (dev->getCurrentSampleRate())
                  + ", buf="                   + juce::String (dev->getCurrentBufferSizeSamples()));

            if (nin <= 0)
            {
                DBG ("[Y2K] WARNING: input channels is 0 after open — device opened but no audio will flow.");
            }
        }
    }

    // 关闭 AudioDeviceManager 的硬件输入（仅清 inputChannels / inputDeviceName，不动输出）
    void disableHardwareInput()
    {
        if (pluginHolder == nullptr) return;
        auto& adm = pluginHolder->deviceManager;

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        adm.getAudioDeviceSetup (setup);
        if (setup.inputDeviceName.isEmpty() && setup.inputChannels.isZero())
            return; // 已经没有输入

        setup.inputDeviceName.clear();
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();

        const auto err = adm.setAudioDeviceSetup (setup, true);
        if (err.isNotEmpty())
            DBG (juce::String ("[Y2K] disableHardwareInput err: ") + err);
    }

    // ----------------------------------------------------
    // 断开 / 恢复 pluginHolder 参与的 audio callback 流
    //   · 为什么必须做：StandalonePluginHolder 在构造时调用
    //     deviceManager.addAudioCallback(this)，即使我们 disableHardwareInput() 清空了
    //     input endpoint，只要 output endpoint 还活着，device callback 就会继续被调，
    //     里面会把零输入喂给 processBlock → analyserHub->pushStereo。
    //     同一时刻 WASAPI loopback 后台线程也在 pushStereo，两个线程并发写
    //     LoudnessMeter 的 deque 必定 UAF。
    //   · 为什么不用 removeAudioCallback(pluginHolder.get())：
    //     StandalonePluginHolder : private AudioIODeviceCallback，外部无法做 upcast。
    //   · 采用等价手段：关闭 / 重启 AudioDeviceManager 的物理设备。
    //     closeAudioDevice() 会停止所有已注册 AudioIOCallback 的调用，且线程安全；
    //     restartLastAudioDevice() 按最后一次 setup 恢复。
    // ----------------------------------------------------
    void detachHolderCallback()
    {
        if (pluginHolder == nullptr || holderCallbackDetached) return;
        pluginHolder->deviceManager.closeAudioDevice();
        holderCallbackDetached = true;
    }

    void attachHolderCallback()
    {
        if (pluginHolder == nullptr || ! holderCallbackDetached) return;
        pluginHolder->deviceManager.restartLastAudioDevice();
        holderCallbackDetached = false;
    }

    // ----------------------------------------------------
    // WASAPI Loopback —— 启停
    // ----------------------------------------------------
    bool startLoopbackCapture()
    {
        if (pluginHolder == nullptr || pluginHolder->processor == nullptr)
            return false;

        if (loopback == nullptr)
            loopback = std::make_unique<WasapiLoopbackCapture>();

        auto* y2kProc = dynamic_cast<Y2KmeterAudioProcessor*> (pluginHolder->processor.get());
        if (y2kProc == nullptr) return false;

        captureTargetProcessor.store (y2kProc, std::memory_order_release);

        // 关键：先彻底断开 pluginHolder 对 deviceManager 的 audio callback，
        //       确保 loopback 运行期间只有采集线程在 push AnalyserHub，杜绝并发 UAF
        detachHolderCallback();

        // 重置采样率缓存 → 第一次回调会做一次 prepare
        loopbackLastSampleRate.store (0.0, std::memory_order_release);

        // 采集到的数据 → 直接喂给 processor 的 AnalyserHub
        //   · onAudio 全部在 loopback 采集线程里执行，不会与任何 JUCE audio callback 并发
        //   · 因此 prepare() 与 pushStereo() 串行调用，deque 的 push/pop 不会跟读并发
        loopback->onAudio = [this] (const float* L, const float* R, int n, double sr)
        {
            auto* proc = captureTargetProcessor.load (std::memory_order_acquire);
            if (proc == nullptr) return;

            // 采样率变化 → 重新 prepare（内部会 clear 所有 deque，现在无并发线程，安全）
            const double cached = loopbackLastSampleRate.load (std::memory_order_acquire);
            if (std::abs (cached - sr) > 0.5)
            {
                const int blk = juce::jmax (64, (int) (sr * 0.01));
                proc->getAnalyserHub().prepare (sr, blk);
                loopbackLastSampleRate.store (sr, std::memory_order_release);
            }

            if (! proc->isAnalysisActive()) return;

            // 度量 Loopback 路径的 CPU 占比（否则 processBlock 不跑，UI CPU 一直 0）
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            proc->getAnalyserHub().pushStereo (L, R, n);
            const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - t0;
            proc->registerLoopbackRenderTime (elapsedMs, n, sr);
        };

        if (! loopback->start())
        {
            // 启动失败：恢复 holder callback，避免普通设备模式下也丢输入
            attachHolderCallback();
            return false;
        }

        loopbackActive.store (true, std::memory_order_release);
        return true;
    }

    void stopLoopbackCapture()
    {
        captureTargetProcessor.store (nullptr, std::memory_order_release);

        if (loopback != nullptr)
        {
            loopback->onAudio = {};
            loopback->stop();   // 同步 join 采集线程，确保返回后不再有回调
        }
        loopbackActive.store (false, std::memory_order_release);
        loopbackLastSampleRate.store (0.0, std::memory_order_release);

        // 采集线程已停止 → 此时恢复 holder callback 是安全的：
        //   即便随后 pluginHolder 马上开始接受 audio callback 并调 pushStereo，
        //   loopback 侧已经没有线程在动 AnalyserHub 了，不会并发。
        attachHolderCallback();
    }

    bool startMacDesktopAudioCapture()
    {
       #if JUCE_MAC
        if (pluginHolder == nullptr || pluginHolder->processor == nullptr)
            return false;

        if (macDesktopCapture == nullptr)
            macDesktopCapture = std::make_unique<MacDesktopAudioCapture>();

        auto* y2kProc = dynamic_cast<Y2KmeterAudioProcessor*> (pluginHolder->processor.get());
        if (y2kProc == nullptr) return false;

        captureTargetProcessor.store (y2kProc, std::memory_order_release);

        detachHolderCallback();
        loopbackLastSampleRate.store (0.0, std::memory_order_release);

        macDesktopCapture->onAudio = [this] (const float* L, const float* R, int n, double sr)
        {
            auto* proc = captureTargetProcessor.load (std::memory_order_acquire);
            if (proc == nullptr) return;

            const double cached = loopbackLastSampleRate.load (std::memory_order_acquire);
            if (std::abs (cached - sr) > 0.5)
            {
                const int blk = juce::jmax (64, (int) (sr * 0.01));
                proc->getAnalyserHub().prepare (sr, blk);
                loopbackLastSampleRate.store (sr, std::memory_order_release);
            }

            auto& dump = AudioDumpRecorder::instance();
            if (dump.isEnabled())
                dump.push (AudioDumpRecorder::Route::output, L, R, n, sr);

            if (! proc->isAnalysisActive()) return;

            const double t0 = juce::Time::getMillisecondCounterHiRes();
            proc->getAnalyserHub().pushStereo (L, R, n);

            const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - t0;
            proc->registerLoopbackRenderTime (elapsedMs, n, sr);

        };

        if (! macDesktopCapture->start())
        {
            attachHolderCallback();
            return false;
        }

        loopbackActive.store (true, std::memory_order_release);
        virtualLoopbackInputActive.store (false, std::memory_order_release);
        return true;
       #else
        return false;
       #endif
    }

    void stopMacDesktopAudioCapture()
    {
       #if JUCE_MAC
        captureTargetProcessor.store (nullptr, std::memory_order_release);

        if (macDesktopCapture != nullptr)
        {
            macDesktopCapture->onAudio = {};
            macDesktopCapture->stop();
        }

        loopbackActive.store (false, std::memory_order_release);
        loopbackLastSampleRate.store (0.0, std::memory_order_release);
        attachHolderCallback();
       #endif
    }

    // 根据 AudioDeviceManager 当前状态，把下拉框选择回到"真实正在使用的输入设备"

    void syncDropdownToCurrentDevice (Y2KmeterAudioProcessorEditor& editor)
    {
        // 重新 populate 一次（它会以 getCurrentSourceId() 做 selected），
        // 这样设备列表与选中项同时刷新
        populateAudioSources (editor);
    }

    // AudioDeviceManager → ChangeBroadcaster → 我们：外部/热插拔导致设备状态变化时
    // 自动刷新下拉框
    void changeListenerCallback (juce::ChangeBroadcaster* src) override
    {
        if (pluginHolder == nullptr || cachedEditor == nullptr) return;
        if (src != &pluginHolder->deviceManager) return;
        syncDropdownToCurrentDevice (*cachedEditor);
    }

    // ----------------------------------------------------
    // 成员字段
    // ----------------------------------------------------
    juce::ApplicationProperties                     appProperties;
    std::unique_ptr<juce::StandalonePluginHolder>   pluginHolder;
    std::unique_ptr<Y2KMainWindow>                  mainWindow;
    // 独立持有 editor —— 与 DocumentWindow 解耦生命周期，
    // 以便在 delete 之前显式调用 processor->editorBeingDeleted()。
    std::unique_ptr<juce::AudioProcessorEditor>     pluginEditor;
    std::unique_ptr<juce::FileLogger>               runtimeFileLogger;

    // 裸指针缓存 —— 指向 pluginEditor.get() 动态转型后的具体类型
    //   · 用于 ChangeListener 回调里刷新下拉框；不拥有生命周期
    Y2KmeterAudioProcessorEditor*                   cachedEditor = nullptr;

    // WASAPI Loopback 采集器 —— 在"System Output (Loopback)"选项被选中时启用
    std::unique_ptr<WasapiLoopbackCapture>          loopback;
    std::atomic<bool>                               loopbackActive { false };
    std::atomic<double>                             loopbackLastSampleRate { 0.0 };
    std::atomic<Y2KmeterAudioProcessor*>            captureTargetProcessor { nullptr };

    // macOS：基于 ScreenCaptureKit 的桌面音频采集（不依赖 BlackHole）
    std::unique_ptr<MacDesktopAudioCapture>         macDesktopCapture;

    // 非 Windows 的 "Output" 语义通过虚拟回环输入设备实现（BlackHole/Loopback/...）
    std::atomic<bool>                               virtualLoopbackInputActive { false };
    juce::String                                    lastVirtualLoopbackInputName;

    // 标记 pluginHolder 当前是否已从 deviceManager 的 audio callback 列表里摘除
    //   · Loopback 运行期间 = true（避免 AnalyserHub 被两路线程并发 push 引发 UAF）
    //   · 其他时间 = false
    bool                                            holderCallbackDetached = false;

    // 关闭流程可能有两个入口都会走到 save：systemRequestedQuit → shutdown。
    // persistAllSettings 必须恰好跑一次，否则第二次会在第一次 save 好的
    // 基础上再 sanitize 一遍 filterState，而 pluginHolder 可能已经被析构
    // → 没人把 filterState 写回，settings 里就只剩空值，下次启动回到默认布局。
    bool                                            alreadyPersisted = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Y2KStandaloneApp)
};

} // namespace y2k

// ----------------------------------------------------------
// 与 juce_audio_plugin_client_Standalone.cpp 的约定：
//   当 JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 时，JUCE 期望外部提供
//   juce::JUCEApplicationBase* juce_CreateApplication();
//   以及 main()/WinMain()（由下面的 JUCE_MAIN_FUNCTION_DEFINITION 宏生成）
// ----------------------------------------------------------
// 注意：WinMain / main 不要在这里生成！
//   JUCE 官方的 juce_audio_plugin_client_Standalone.cpp 在
//   JUCE_USE_CUSTOM_PLUGIN_STANDALONE_ENTRYPOINT=0 时已经自带一份 JUCE_MAIN_FUNCTION_DEFINITION
//   它展开出来的 WinMain 会自动调用 juce_CreateApplication()。
//   如果我们这里再展开一次，会导致 LNK2005: WinMain 重定义。
//   因此本文件只负责提供 juce_CreateApplication() 的实现。
juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication()   { return new y2k::Y2KStandaloneApp(); }

#endif // JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP
