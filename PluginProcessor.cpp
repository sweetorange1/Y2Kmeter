#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "source/analysis/AnalyserHub.h"
#include "source/perf/PerformanceCounterSystem.h"
#include "source/network/TelemetryClient.h"
#include "source/network/UpdateChecker.h"

// macOS Standalone 专属：音频转储调试器（Windows 构建下此头文件不被编译，
// 且 CMakeLists 中 AudioDumpRecorder.cpp 仅在 APPLE 分支加入 target_sources）
#if JUCE_MAC
 #include "source/standalone/AudioDumpRecorder.h"
#endif

namespace
{
inline float clampGainDb (float db) noexcept
{
    return juce::jlimit (-10.0f, 36.0f, db);
}
}

// ==========================================================
// Y2KmeterAudioProcessor
// ==========================================================
Y2KmeterAudioProcessor::Y2KmeterAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    ),
      analyserHub(std::make_unique<AnalyserHub>())
{
    // ================================================================
    // VST3 遥测 + 更新检查（进程级去重，避免 DAW 扫描期重复触发）
    //
    // 策略：
    //   · 使用 static atomic 确保同一进程内最多触发一次；
    //   · 延迟 5 秒执行（DAW 扫描通常在 3 秒内完成加载+销毁）；
    //   · 授权状态由安装包写入注册表，LoadFromRegistry() 读取；
    //     默认未授权（不发送数据），VST3 无独立安装程序，
    //     直接复用同一注册表键值；
    //   · client_id 持久化到 %APPDATA%/Y2Kmeter.telemetry 文件，
    //     作为 PropertiesFile 单 key 存根，与 Standalone 共享同一 UUID。
    //   · sTelemetryProps 为 static 确保其生命周期覆盖所有异步回调
    //     （callback / ShowUpdateDialog 中的 settings 指针不会悬空）。
    // ================================================================
    static std::atomic<bool> telemetryOnceFlag{false};
    if (!telemetryOnceFlag.exchange(true, std::memory_order_acquire))
    {
        // ApplicationProperties 必须为 static，否则 callAfterDelay
        // lambda 返回后被析构，导致 background thread / dialog
        // callback 中 settings 指针 Use-After-Free 崩溃。
        static juce::ApplicationProperties sTelemetryProps;
        {
            juce::PropertiesFile::Options opts;
            opts.applicationName = "Y2Kmeter";
            opts.filenameSuffix  = ".telemetry";
            opts.folderName      = "";
            opts.osxLibrarySubFolder = "Application Support";
            opts.storageFormat   = juce::PropertiesFile::storeAsXML;
            sTelemetryProps.setStorageParameters(opts);
        }

        juce::Timer::callAfterDelay(5000, []() {
            auto* telemetrySettings =
                sTelemetryProps.getUserSettings();

            auto& tc = y2k::network::TelemetryClient::GetInstance();
            tc.LoadFromRegistry();

            tc.SendStartupPing(telemetrySettings, /*isPlugin=*/true);

#if JUCE_WINDOWS
            const juce::String platform = "win-x64";
#elif JUCE_MAC
            const juce::String platform = "macos";
#elif JUCE_LINUX
            const juce::String platform = "linux";
#else
            const juce::String platform = "unknown";
#endif
            y2k::network::CheckForUpdatesAsync(
                juce::String(JucePlugin_VersionString),
                platform,
                telemetrySettings,
                [telemetrySettings](
                    const y2k::network::UpdateInfo& info) {
                    if (info.has_update) {
                        y2k::network::ShowUpdateDialog(
                            info, telemetrySettings);
                    }
                });
        });
    }
}

Y2KmeterAudioProcessor::~Y2KmeterAudioProcessor() {}

bool Y2KmeterAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    if (layouts.getMainOutputChannelSet() == juce::AudioChannelSet::disabled())
        return false;

#if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
#endif
}

void Y2KmeterAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    analyserHub->prepare(sampleRate, samplesPerBlock);

    // CPU 占用率测量器：重置每个 block 的目标时长（= samplesPerBlock / sampleRate）
    // loadMeasurer 内部会用 start/stop 测出实际耗时 / 目标时长，得出占比。
    loadMeasurer.reset(sampleRate, samplesPerBlock);

    // P2-2：音频线程分析增益临时缓冲预分配，避免每个 block 的
    //   juce::AudioBuffer 构造 / HeapBlock::malloc 开销。
    const int cap = juce::jmax (32, samplesPerBlock);
    analysisGainBufferStereo.setSize (2, cap, /*keepExistingContent=*/ false,
                                      /*clearExtraSpace=*/ false,
                                      /*avoidReallocating=*/ false);
    if (analysisGainBufferMonoCapacity < cap)
    {
        analysisGainBufferMono.allocate ((size_t) cap, false);
        analysisGainBufferMonoCapacity = cap;
    }
}

void Y2KmeterAudioProcessor::releaseResources() {}

void Y2KmeterAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer&)
{
#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::PerformanceCounterSystem::instance().markCurrentThreadRole(y2k::perf::ThreadRole::audio, "Audio-ProcessBlock");
    y2k::perf::ScopedPerfTimer perfTimer(y2k::perf::FunctionId::processBlockTotal,
                                         y2k::perf::Partition::audioAnalysis,
                                         y2k::perf::ThreadRole::audio);
#endif

    // CPU 负载采样：包住整个 processBlock 的有效工作范围。
    // AudioProcessLoadMeasurer 是无锁的，安全在音频线程调用。
    juce::AudioProcessLoadMeasurer::ScopedTimer loadScope (loadMeasurer,
                                                           buffer.getNumSamples());

    juce::ScopedNoDenormals noDenormals;

    // 前置增益变化后，在音频线程安全重置 loudness（含 LUFS-I 积分）
    if (pendingLoudnessReset.exchange (false, std::memory_order_relaxed))
        analyserHub->resetLoudness();

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();

    // ------------------------------------------------------------------
    // macOS Standalone 调试采样：当 Y2KM_AUDIO_DUMP 系列环境变量开启时，
    // 把当前 block 的输入通道复制一份落盘。仅编入 macOS 构建，
    // Windows 下该整段不参与编译，零运行时开销与风险。
    // ------------------------------------------------------------------
#if JUCE_MAC
    if (wrapperType == wrapperType_Standalone)
    {
        auto& dump = AudioDumpRecorder::instance();
        dump.configureFromEnvironment();
        if (dump.isEnabled())
        {
            const float* L = (totalIn >= 1 ? buffer.getReadPointer (0) : nullptr);
            const float* R = (totalIn >= 2 ? buffer.getReadPointer (1) : L);
            if (L != nullptr && R != nullptr)
                dump.push (AudioDumpRecorder::Route::microphone,
                           L,
                           R,
                           buffer.getNumSamples(),
                           getSampleRate());
        }
    }
#endif

    // 1) 先采分析：UI 不可见时跳过
    if (analysisActive.load(std::memory_order_relaxed))
    {
        if (totalIn >= 2)
        {
            const float gainLin = analysisInputGainLin.load (std::memory_order_relaxed);

            if (std::abs (gainLin - 1.0f) < 0.0001f)
            {
                // 真正的立体声
                analyserHub->pushStereo(buffer.getReadPointer(0),
                                        buffer.getReadPointer(1),
                                        buffer.getNumSamples());
            }
            else
            {
                const int n = buffer.getNumSamples();
                // P2-2：用预分配缓冲，避免音频线程构造 juce::AudioBuffer
                //   触发的堆分配。容量在 prepareToPlay 里按 samplesPerBlock 开足，
                //   极少见的 n 超容量场景下实时 setSize 只众之一次，不影响稳态。
                if (analysisGainBufferStereo.getNumSamples() < n)
                    analysisGainBufferStereo.setSize (2, n, false, false, false);

                analysisGainBufferStereo.copyFrom (0, 0, buffer, 0, 0, n);
                analysisGainBufferStereo.copyFrom (1, 0, buffer, 1, 0, n);
                analysisGainBufferStereo.applyGain (0, 0, n, gainLin);
                analysisGainBufferStereo.applyGain (1, 0, n, gainLin);

                analyserHub->pushStereo (analysisGainBufferStereo.getReadPointer (0),
                                         analysisGainBufferStereo.getReadPointer (1),
                                         n);
            }
        }
        else if (totalIn == 1)
        {
            const float gainLin = analysisInputGainLin.load (std::memory_order_relaxed);
            const int n = buffer.getNumSamples();

            if (std::abs (gainLin - 1.0f) < 0.0001f)
            {
                // Mono 降级：L/R 使用同一指针
                analyserHub->pushStereo(buffer.getReadPointer(0),
                                        buffer.getReadPointer(0),
                                        n);
            }
            else
            {
                // P2-2：用预分配的 mono 缓冲，避免 HeapBlock::malloc 在音频线程的堆分配。
                if (analysisGainBufferMonoCapacity < n)
                {
                    analysisGainBufferMono.allocate ((size_t) n, false);
                    analysisGainBufferMonoCapacity = n;
                }

                const auto* src = buffer.getReadPointer (0);
                auto* dst = analysisGainBufferMono.get();
                for (int i = 0; i < n; ++i)
                    dst[i] = src[i] * gainLin;

                analyserHub->pushStereo (dst, dst, n);
            }
        }
    }

    // 2) 输出处理（分 wrapper 区分）：
    //   · Standalone：清零输出。
    //     原因是我们在 Standalone 外壳里通过 WASAPI loopback 抓取"系统输出"做分析，
    //     若此时 processBlock 又把缓冲回写到声卡输出，会形成"系统输出 → 我们采 →
    //     我们又写回输出 → 下一帧又采到自己写入的回环"的反馈循环。
    //     同时 Standalone 壳本就是"纯分析工具"，输出不该发声。
    //   · VST3 及其它插件格式（wrapperType_VST3 / wrapperType_VST / wrapperType_AU
    //     / wrapperType_AAX / wrapperType_LV2 等）：**原样透传**（输入=输出）。
    //     原因是在 DAW 轨道上本插件被串联时，下游效果 / 监听不能被"吃掉"音频。
    //     分析已经在 step 1 完成，输入缓冲没有被我们动过（pushStereo 只读不写），
    //     因此无需任何操作即可实现完美透传。
    if (wrapperType == wrapperType_Standalone)
    {
        for (int i = 0; i < totalOut; ++i)
            buffer.clear (i, 0, buffer.getNumSamples());
    }
    // else（插件模式）：保留 buffer 内容不动，输入 = 输出，分析+透传。
}

// ---- 兼容旧接口 ----
AnalyserHub& Y2KmeterAudioProcessor::getAnalyserHub() noexcept
{
    return *analyserHub;
}

void Y2KmeterAudioProcessor::setAnalysisActive(bool shouldBeActive) noexcept
{
    analysisActive.store(shouldBeActive, std::memory_order_relaxed);
#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::PerformanceCounterSystem::instance().recordEvent(
        y2k::perf::FunctionId::lowFreqThemeOrUiStateChange,
        y2k::perf::Partition::dataCommunication,
        y2k::perf::ThreadRole::unknown,
        1);
#endif
}

bool Y2KmeterAudioProcessor::isAnalysisActive() const noexcept
{
    return analysisActive.load(std::memory_order_relaxed);
}

double Y2KmeterAudioProcessor::getCpuLoad() const noexcept
{
    // JUCE API 返回 [0..1]；非 const 接口 → const_cast 包一下。
    // getLoadAsProportion() 内部读原子变量，线程安全。
    return const_cast<juce::AudioProcessLoadMeasurer&>(loadMeasurer).getLoadAsProportion();
}

void Y2KmeterAudioProcessor::registerLoopbackRenderTime (double millisecondsTaken,
                                                         int   numSamples,
                                                         double sampleRate) noexcept
{
    if (numSamples <= 0 || sampleRate <= 0.0) return;

    // 如果 Standalone 场景下 prepareToPlay 还没被调用（或 sampleRate / blockSize
    // 与 Loopback 实际值不一致），这里懒初始化一次。之后 sampleRate/numSamples
    // 若有变化，registerRenderTime() 内部会自己做 target-ms 归一。
    if (! loopbackMeasurerPrimed
        || std::abs (loopbackLastSampleRate - sampleRate) > 0.5
        || loopbackLastBlockSize != numSamples)
    {
        loadMeasurer.reset (sampleRate, numSamples);
        loopbackLastSampleRate = sampleRate;
        loopbackLastBlockSize  = numSamples;
        loopbackMeasurerPrimed = true;
    }

    loadMeasurer.registerRenderTime (millisecondsTaken, numSamples);
}

void Y2KmeterAudioProcessor::setAnalysisInputGainDb (float db) noexcept
{
    const float clamped = clampGainDb (db);
    const float oldDb   = analysisInputGainDb.load (std::memory_order_relaxed);

    analysisInputGainDb.store (clamped, std::memory_order_relaxed);
    analysisInputGainLin.store (juce::Decibels::decibelsToGain (clamped),
                                std::memory_order_relaxed);

    // 用户修改前置增益时，请求在音频线程重置 Loudness 积分（LUFS-I 自动归零重算）。
    if (std::abs (clamped - oldDb) > 0.0001f)
        pendingLoudnessReset.store (true, std::memory_order_relaxed);
}

float Y2KmeterAudioProcessor::getAnalysisInputGainDb() const noexcept
{
    return analysisInputGainDb.load (std::memory_order_relaxed);
}

float Y2KmeterAudioProcessor::getAnalysisInputGainLinear() const noexcept
{
    return analysisInputGainLin.load (std::memory_order_relaxed);
}

juce::File Y2KmeterAudioProcessor::exportPerfCountersNow()
{
#if Y2K_ENABLE_PERF_COUNTERS
    return y2k::perf::PerformanceCounterSystem::instance().exportNow();
#else
    return {};
#endif
}

void Y2KmeterAudioProcessor::setPerfAutoExportEnabled(bool enabled) noexcept
{
#if Y2K_ENABLE_PERF_COUNTERS
    y2k::perf::PerformanceCounterSystem::instance().setAutoExportEnabled(enabled);
    y2k::perf::PerformanceCounterSystem::instance().recordEvent(
        y2k::perf::FunctionId::lowFreqThemeOrUiStateChange,
        y2k::perf::Partition::dataCommunication,
        y2k::perf::ThreadRole::unknown,
        1);
#else
    juce::ignoreUnused(enabled);
#endif
}

bool Y2KmeterAudioProcessor::isPerfAutoExportEnabled() const noexcept
{
#if Y2K_ENABLE_PERF_COUNTERS
    return y2k::perf::PerformanceCounterSystem::instance().isAutoExportEnabled();
#else
    return false;
#endif
}

double Y2KmeterAudioProcessor::getCurrentSampleRate() const noexcept
{
    return analyserHub->getSampleRate();
}

void Y2KmeterAudioProcessor::getOscilloscopeSnapshot(juce::Array<float>& dest)
{
    juce::Array<float> dummyR;
    analyserHub->getOscilloscopeSnapshot(dest, dummyR);
}

void Y2KmeterAudioProcessor::getSpectrumSnapshot(juce::Array<float>& dest)
{
    analyserHub->getSpectrumSnapshot(dest);
}

// ---- AudioProcessor 标准接口 ----
juce::AudioProcessorEditor* Y2KmeterAudioProcessor::createEditor()
{
    return new Y2KmeterAudioProcessorEditor(*this);
}
bool Y2KmeterAudioProcessor::hasEditor() const { return true; }

const juce::String Y2KmeterAudioProcessor::getName() const { return "Y2Kmeter"; }
bool Y2KmeterAudioProcessor::acceptsMidi()  const { return false; }
bool Y2KmeterAudioProcessor::producesMidi() const { return false; }
bool Y2KmeterAudioProcessor::isMidiEffect() const { return false; }
double Y2KmeterAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int Y2KmeterAudioProcessor::getNumPrograms()    { return 1; }
int Y2KmeterAudioProcessor::getCurrentProgram() { return 0; }
void Y2KmeterAudioProcessor::setCurrentProgram(int) {}
const juce::String Y2KmeterAudioProcessor::getProgramName(int) { return {}; }
void Y2KmeterAudioProcessor::changeProgramName(int, const juce::String&) {}

void Y2KmeterAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // P4：如果 Editor 在运行，debounce 合并中的 workspace 布局变更可能
    //   还没落到 savedLayoutXml。先 flush，保证下面序列化的是最新布局。
    //   非 UI 打开状态下此回调为空，no-op。
    if (flushPendingUiStateBeforeSave)
        flushPendingUiStateBeforeSave();

    // 顶层 <PBEQ_State>
    //   <Layout>...</Layout>        ← UI 布局
    juce::ValueTree root("PBEQ_State");
    root.setProperty("version", 1, nullptr);
    root.setProperty("analysisInputGainDb",
                     (double) analysisInputGainDb.load (std::memory_order_relaxed),
                     nullptr);

    // 插件 Editor 最近一次窗口尺寸：仅在非零时写入（0=从未被 Editor 写过）
    if (savedEditorWidth > 0 && savedEditorHeight > 0)
    {
        root.setProperty ("editorW", savedEditorWidth,  nullptr);
        root.setProperty ("editorH", savedEditorHeight, nullptr);
    }

    // 布局锁定态（v1.8.3）：仅 true 时写入，避免旧 state 无意义地多一个 false 属性。
    if (savedLayoutLocked)
        root.setProperty ("layoutLocked", true, nullptr);

    // 新手引导完成状态：始终写入，区分"未完成"与"旧存档中缺失"
    root.setProperty ("tutorialCompleted", tutorialCompleted, nullptr);

    // Milkdrop 整体染色 + 效果（全局状态）
    root.setProperty ("milkdropTintR",  (double) savedMilkdropVisualState_.tint_r,  nullptr);
    root.setProperty ("milkdropTintG",  (double) savedMilkdropVisualState_.tint_g,  nullptr);
    root.setProperty ("milkdropTintB",  (double) savedMilkdropVisualState_.tint_b,  nullptr);
    root.setProperty ("milkdropBrightness", (double) savedMilkdropVisualState_.brightness, nullptr);
    root.setProperty ("milkdropInvert",  savedMilkdropVisualState_.invert,  nullptr);
    root.setProperty ("milkdropShadows", savedMilkdropVisualState_.shadows, nullptr);
    root.setProperty ("milkdropSolarize", savedMilkdropVisualState_.solarize, nullptr);
    root.setProperty ("milkdropSplit",    savedMilkdropVisualState_.split,    nullptr);
    root.setProperty ("milkdropZoom",     savedMilkdropVisualState_.zoom,     nullptr);
    root.setProperty ("milkdropMulti",    savedMilkdropVisualState_.multi,    nullptr);
    root.setProperty ("milkdropRainbow",  savedMilkdropVisualState_.rainbow,  nullptr);
    root.setProperty ("milkdropBlow",     savedMilkdropVisualState_.blow,     nullptr);
    root.setProperty ("milkdropBurn",     savedMilkdropVisualState_.burn,     nullptr);
    root.setProperty ("milkdropKaleidoscope", savedMilkdropVisualState_.kaleidoscope, nullptr);
    root.setProperty ("milkdropSwirl",        savedMilkdropVisualState_.swirl,        nullptr);
    root.setProperty ("milkdropPinch",        savedMilkdropVisualState_.pinch,        nullptr);
    root.setProperty ("milkdropPixelate",     savedMilkdropVisualState_.pixelate,     nullptr);
    root.setProperty ("milkdropGlitch",       savedMilkdropVisualState_.glitch,       nullptr);
    root.setProperty ("milkdropPosterize",    savedMilkdropVisualState_.posterize,    nullptr);
    root.setProperty ("milkdropSepia",        savedMilkdropVisualState_.sepia,        nullptr);
    root.setProperty ("milkdropGrayscale",    savedMilkdropVisualState_.grayscale,    nullptr);
    root.setProperty ("milkdropEdge",         savedMilkdropVisualState_.edge,         nullptr);
    root.setProperty ("milkdropVignette",     savedMilkdropVisualState_.vignette,     nullptr);
    root.setProperty ("milkdropTunnel",       savedMilkdropVisualState_.tunnel,       nullptr);
    root.setProperty ("milkdropRipple",       savedMilkdropVisualState_.ripple,       nullptr);
    root.setProperty ("milkdropMelt",         savedMilkdropVisualState_.melt,         nullptr);
    root.setProperty ("milkdropFisheye",      savedMilkdropVisualState_.fisheye,      nullptr);
    root.setProperty ("milkdropNoiseWarp",    savedMilkdropVisualState_.noise_warp,   nullptr);
    root.setProperty ("milkdropMirrorMaze",   savedMilkdropVisualState_.mirror_maze,  nullptr);
    root.setProperty ("milkdropFragment",     savedMilkdropVisualState_.fragment,     nullptr);
    root.setProperty ("milkdropSpiral",       savedMilkdropVisualState_.spiral,       nullptr);
    root.setProperty ("milkdropTwist",        savedMilkdropVisualState_.twist,        nullptr);
    root.setProperty ("milkdropColorShift",   savedMilkdropVisualState_.color_shift,  nullptr);
    root.setProperty ("milkdropNeon",         savedMilkdropVisualState_.neon,         nullptr);
    root.setProperty ("milkdropThermal",      savedMilkdropVisualState_.thermal,      nullptr);
    root.setProperty ("milkdropAcid",         savedMilkdropVisualState_.acid,         nullptr);
    root.setProperty ("milkdropVhs",          savedMilkdropVisualState_.vhs,          nullptr);
    root.setProperty ("milkdropCrt",          savedMilkdropVisualState_.crt,          nullptr);
    root.setProperty ("milkdropDuotone",      savedMilkdropVisualState_.duotone,      nullptr);
    root.setProperty ("milkdropBloom",        savedMilkdropVisualState_.bloom,        nullptr);
    root.setProperty ("milkdropBinary",       savedMilkdropVisualState_.binary,       nullptr);
    root.setProperty ("milkdropPrismatic",    savedMilkdropVisualState_.prismatic,    nullptr);

    // Milkdrop 简单波形样式覆盖（全局状态）
    root.setProperty ("milkdropWaveEnabled",  savedMilkdropWaveState_.enabled,  nullptr);
    root.setProperty ("milkdropWaveMode",     savedMilkdropWaveState_.mode,     nullptr);
    root.setProperty ("milkdropWaveX",        (double) savedMilkdropWaveState_.x, nullptr);
    root.setProperty ("milkdropWaveY",        (double) savedMilkdropWaveState_.y, nullptr);
    root.setProperty ("milkdropWaveR",        (double) savedMilkdropWaveState_.r, nullptr);
    root.setProperty ("milkdropWaveG",        (double) savedMilkdropWaveState_.g, nullptr);
    root.setProperty ("milkdropWaveB",        (double) savedMilkdropWaveState_.b, nullptr);
    root.setProperty ("milkdropWaveA",        (double) savedMilkdropWaveState_.a, nullptr);
    root.setProperty ("milkdropWaveMystery",  (double) savedMilkdropWaveState_.mystery, nullptr);
    root.setProperty ("milkdropWaveUsedots",  savedMilkdropWaveState_.usedots,  nullptr);
    root.setProperty ("milkdropWaveThick",    savedMilkdropWaveState_.thick,    nullptr);
    root.setProperty ("milkdropWaveAdditive", savedMilkdropWaveState_.additive, nullptr);
    root.setProperty ("milkdropWaveBrighten", savedMilkdropWaveState_.brighten, nullptr);

    // Milkdrop 后处理 uv 几何畸变偏移（全局状态，随 visual state 持久化）
    root.setProperty ("milkdropOffsetZoom", (double) savedMilkdropVisualState_.offset.value[0], nullptr);
    root.setProperty ("milkdropOffsetRot",  (double) savedMilkdropVisualState_.offset.value[1], nullptr);
    root.setProperty ("milkdropOffsetWarp", (double) savedMilkdropVisualState_.offset.value[2], nullptr);
    root.setProperty ("milkdropOffsetDx",   (double) savedMilkdropVisualState_.offset.value[3], nullptr);
    root.setProperty ("milkdropOffsetDy",   (double) savedMilkdropVisualState_.offset.value[4], nullptr);
    root.setProperty ("milkdropOffsetSx",   (double) savedMilkdropVisualState_.offset.value[5], nullptr);
    root.setProperty ("milkdropOffsetSy",   (double) savedMilkdropVisualState_.offset.value[6], nullptr);
    root.setProperty ("milkdropOffsetKaleido", savedMilkdropVisualState_.offset.ivalue[0], nullptr);
    root.setProperty ("milkdropOffsetFoldX",   savedMilkdropVisualState_.offset.ivalue[1], nullptr);
    root.setProperty ("milkdropOffsetFoldY",   savedMilkdropVisualState_.offset.ivalue[2], nullptr);

    if (savedLayoutXml.isNotEmpty())
    {
        if (auto layoutXml = juce::parseXML(savedLayoutXml))
        {
            const auto layoutTree = juce::ValueTree::fromXml(*layoutXml);
            if (layoutTree.isValid())
                root.appendChild(layoutTree, nullptr);
        }
    }

    if (auto xml = root.createXml())
        juce::AudioProcessor::copyXmlToBinary(*xml, destData);
}

void Y2KmeterAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = juce::AudioProcessor::getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr) return;

    const auto root = juce::ValueTree::fromXml(*xml);
    if (! root.isValid() || ! root.hasType("PBEQ_State")) return;

    if (root.hasProperty ("analysisInputGainDb"))
        setAnalysisInputGainDb ((float) (double) root.getProperty ("analysisInputGainDb", 0.0));

    // 插件 Editor 最近一次窗口尺寸（可能为 0 = 未保存；后续 Editor 构造时兜底到默认值）
    if (root.hasProperty ("editorW") && root.hasProperty ("editorH"))
    {
        const int w = (int) root.getProperty ("editorW", 0);
        const int h = (int) root.getProperty ("editorH", 0);
        if (w > 0 && h > 0)
        {
            savedEditorWidth  = w;
            savedEditorHeight = h;
        }
    }

    // 布局锁定态（v1.8.3）：旧 state 无此属性时默认 false，保持向后兼容。
    savedLayoutLocked = (bool) root.getProperty ("layoutLocked", false);

    // 新手引导完成状态：旧存档中无此属性 → 视为老用户，默认已完成
    if (root.hasProperty ("tutorialCompleted"))
        tutorialCompleted = (bool) root.getProperty ("tutorialCompleted");
    else
        tutorialCompleted = true;   // old settings file → user already has experience

    // Milkdrop 整体染色 + 效果（旧存档缺失时保持默认值）
    if (root.hasProperty ("milkdropTintR"))
        savedMilkdropVisualState_.tint_r = (float) (double) root.getProperty ("milkdropTintR", 1.0);
    if (root.hasProperty ("milkdropTintG"))
        savedMilkdropVisualState_.tint_g = (float) (double) root.getProperty ("milkdropTintG", 1.0);
    if (root.hasProperty ("milkdropTintB"))
        savedMilkdropVisualState_.tint_b = (float) (double) root.getProperty ("milkdropTintB", 1.0);
    if (root.hasProperty ("milkdropBrightness"))
        savedMilkdropVisualState_.brightness = (float) (double) root.getProperty ("milkdropBrightness", 1.0);
    if (root.hasProperty ("milkdropInvert"))
        savedMilkdropVisualState_.invert = (bool) root.getProperty ("milkdropInvert", false);
    if (root.hasProperty ("milkdropShadows"))
        savedMilkdropVisualState_.shadows = (bool) root.getProperty ("milkdropShadows", false);
    if (root.hasProperty ("milkdropSolarize"))
        savedMilkdropVisualState_.solarize = (bool) root.getProperty ("milkdropSolarize", false);
    if (root.hasProperty ("milkdropSplit"))
        savedMilkdropVisualState_.split = (bool) root.getProperty ("milkdropSplit", false);
    if (root.hasProperty ("milkdropZoom"))
        savedMilkdropVisualState_.zoom = (bool) root.getProperty ("milkdropZoom", false);
    if (root.hasProperty ("milkdropMulti"))
        savedMilkdropVisualState_.multi = (bool) root.getProperty ("milkdropMulti", false);
    if (root.hasProperty ("milkdropRainbow"))
        savedMilkdropVisualState_.rainbow = (bool) root.getProperty ("milkdropRainbow", false);
    if (root.hasProperty ("milkdropBlow"))
        savedMilkdropVisualState_.blow = (bool) root.getProperty ("milkdropBlow", false);
    if (root.hasProperty ("milkdropBurn"))
        savedMilkdropVisualState_.burn = (bool) root.getProperty ("milkdropBurn", false);
    if (root.hasProperty ("milkdropKaleidoscope"))
        savedMilkdropVisualState_.kaleidoscope = (bool) root.getProperty ("milkdropKaleidoscope", false);
    if (root.hasProperty ("milkdropSwirl"))
        savedMilkdropVisualState_.swirl = (bool) root.getProperty ("milkdropSwirl", false);
    if (root.hasProperty ("milkdropPinch"))
        savedMilkdropVisualState_.pinch = (bool) root.getProperty ("milkdropPinch", false);
    if (root.hasProperty ("milkdropPixelate"))
        savedMilkdropVisualState_.pixelate = (bool) root.getProperty ("milkdropPixelate", false);
    if (root.hasProperty ("milkdropGlitch"))
        savedMilkdropVisualState_.glitch = (bool) root.getProperty ("milkdropGlitch", false);
    if (root.hasProperty ("milkdropPosterize"))
        savedMilkdropVisualState_.posterize = (bool) root.getProperty ("milkdropPosterize", false);
    if (root.hasProperty ("milkdropSepia"))
        savedMilkdropVisualState_.sepia = (bool) root.getProperty ("milkdropSepia", false);
    if (root.hasProperty ("milkdropGrayscale"))
        savedMilkdropVisualState_.grayscale = (bool) root.getProperty ("milkdropGrayscale", false);
    if (root.hasProperty ("milkdropEdge"))
        savedMilkdropVisualState_.edge = (bool) root.getProperty ("milkdropEdge", false);
    if (root.hasProperty ("milkdropVignette"))
        savedMilkdropVisualState_.vignette = (bool) root.getProperty ("milkdropVignette", false);
    if (root.hasProperty ("milkdropTunnel"))
        savedMilkdropVisualState_.tunnel = (bool) root.getProperty ("milkdropTunnel", false);
    if (root.hasProperty ("milkdropRipple"))
        savedMilkdropVisualState_.ripple = (bool) root.getProperty ("milkdropRipple", false);
    if (root.hasProperty ("milkdropMelt"))
        savedMilkdropVisualState_.melt = (bool) root.getProperty ("milkdropMelt", false);
    if (root.hasProperty ("milkdropFisheye"))
        savedMilkdropVisualState_.fisheye = (bool) root.getProperty ("milkdropFisheye", false);
    if (root.hasProperty ("milkdropNoiseWarp"))
        savedMilkdropVisualState_.noise_warp = (bool) root.getProperty ("milkdropNoiseWarp", false);
    if (root.hasProperty ("milkdropMirrorMaze"))
        savedMilkdropVisualState_.mirror_maze = (bool) root.getProperty ("milkdropMirrorMaze", false);
    if (root.hasProperty ("milkdropFragment"))
        savedMilkdropVisualState_.fragment = (bool) root.getProperty ("milkdropFragment", false);
    if (root.hasProperty ("milkdropSpiral"))
        savedMilkdropVisualState_.spiral = (bool) root.getProperty ("milkdropSpiral", false);
    if (root.hasProperty ("milkdropTwist"))
        savedMilkdropVisualState_.twist = (bool) root.getProperty ("milkdropTwist", false);
    if (root.hasProperty ("milkdropColorShift"))
        savedMilkdropVisualState_.color_shift = (bool) root.getProperty ("milkdropColorShift", false);
    if (root.hasProperty ("milkdropNeon"))
        savedMilkdropVisualState_.neon = (bool) root.getProperty ("milkdropNeon", false);
    if (root.hasProperty ("milkdropThermal"))
        savedMilkdropVisualState_.thermal = (bool) root.getProperty ("milkdropThermal", false);
    if (root.hasProperty ("milkdropAcid"))
        savedMilkdropVisualState_.acid = (bool) root.getProperty ("milkdropAcid", false);
    if (root.hasProperty ("milkdropVhs"))
        savedMilkdropVisualState_.vhs = (bool) root.getProperty ("milkdropVhs", false);
    if (root.hasProperty ("milkdropCrt"))
        savedMilkdropVisualState_.crt = (bool) root.getProperty ("milkdropCrt", false);
    if (root.hasProperty ("milkdropDuotone"))
        savedMilkdropVisualState_.duotone = (bool) root.getProperty ("milkdropDuotone", false);
    if (root.hasProperty ("milkdropBloom"))
        savedMilkdropVisualState_.bloom = (bool) root.getProperty ("milkdropBloom", false);
    if (root.hasProperty ("milkdropBinary"))
        savedMilkdropVisualState_.binary = (bool) root.getProperty ("milkdropBinary", false);
    if (root.hasProperty ("milkdropPrismatic"))
        savedMilkdropVisualState_.prismatic = (bool) root.getProperty ("milkdropPrismatic", false);

    // Milkdrop 简单波形样式覆盖（旧存档缺失时保持默认）
    if (root.hasProperty ("milkdropWaveEnabled"))
        savedMilkdropWaveState_.enabled = (bool) root.getProperty ("milkdropWaveEnabled", false);
    if (root.hasProperty ("milkdropWaveMode"))
        savedMilkdropWaveState_.mode = (int) root.getProperty ("milkdropWaveMode", 6);
    if (root.hasProperty ("milkdropWaveX"))
        savedMilkdropWaveState_.x = (float) (double) root.getProperty ("milkdropWaveX", 0.5);
    if (root.hasProperty ("milkdropWaveY"))
        savedMilkdropWaveState_.y = (float) (double) root.getProperty ("milkdropWaveY", 0.5);
    if (root.hasProperty ("milkdropWaveR"))
        savedMilkdropWaveState_.r = (float) (double) root.getProperty ("milkdropWaveR", 1.0);
    if (root.hasProperty ("milkdropWaveG"))
        savedMilkdropWaveState_.g = (float) (double) root.getProperty ("milkdropWaveG", 1.0);
    if (root.hasProperty ("milkdropWaveB"))
        savedMilkdropWaveState_.b = (float) (double) root.getProperty ("milkdropWaveB", 1.0);
    if (root.hasProperty ("milkdropWaveA"))
        savedMilkdropWaveState_.a = (float) (double) root.getProperty ("milkdropWaveA", 1.0);
    if (root.hasProperty ("milkdropWaveMystery"))
        savedMilkdropWaveState_.mystery = (float) (double) root.getProperty ("milkdropWaveMystery", 0.0);
    if (root.hasProperty ("milkdropWaveUsedots"))
        savedMilkdropWaveState_.usedots = (bool) root.getProperty ("milkdropWaveUsedots", false);
    if (root.hasProperty ("milkdropWaveThick"))
        savedMilkdropWaveState_.thick = (bool) root.getProperty ("milkdropWaveThick", false);
    if (root.hasProperty ("milkdropWaveAdditive"))
        savedMilkdropWaveState_.additive = (bool) root.getProperty ("milkdropWaveAdditive", false);
    if (root.hasProperty ("milkdropWaveBrighten"))
        savedMilkdropWaveState_.brighten = (bool) root.getProperty ("milkdropWaveBrighten", false);

    // Milkdrop 后处理 uv 几何畸变偏移（旧存档缺失时保持默认 0）
    if (root.hasProperty ("milkdropOffsetZoom"))
        savedMilkdropVisualState_.offset.value[0] = (float) (double) root.getProperty ("milkdropOffsetZoom", 0.0);
    if (root.hasProperty ("milkdropOffsetRot"))
        savedMilkdropVisualState_.offset.value[1] = (float) (double) root.getProperty ("milkdropOffsetRot", 0.0);
    if (root.hasProperty ("milkdropOffsetWarp"))
        savedMilkdropVisualState_.offset.value[2] = (float) (double) root.getProperty ("milkdropOffsetWarp", 0.0);
    if (root.hasProperty ("milkdropOffsetDx"))
        savedMilkdropVisualState_.offset.value[3] = (float) (double) root.getProperty ("milkdropOffsetDx", 0.0);
    if (root.hasProperty ("milkdropOffsetDy"))
        savedMilkdropVisualState_.offset.value[4] = (float) (double) root.getProperty ("milkdropOffsetDy", 0.0);
    if (root.hasProperty ("milkdropOffsetSx"))
        savedMilkdropVisualState_.offset.value[5] = (float) (double) root.getProperty ("milkdropOffsetSx", 0.0);
    if (root.hasProperty ("milkdropOffsetSy"))
        savedMilkdropVisualState_.offset.value[6] = (float) (double) root.getProperty ("milkdropOffsetSy", 0.0);
    if (root.hasProperty ("milkdropOffsetKaleido"))
        savedMilkdropVisualState_.offset.ivalue[0] = (int) root.getProperty ("milkdropOffsetKaleido", 0);
    if (root.hasProperty ("milkdropOffsetFoldX"))
        savedMilkdropVisualState_.offset.ivalue[1] = (int) root.getProperty ("milkdropOffsetFoldX", 0);
    if (root.hasProperty ("milkdropOffsetFoldY"))
        savedMilkdropVisualState_.offset.ivalue[2] = (int) root.getProperty ("milkdropOffsetFoldY", 0);

    const auto layoutTree = root.getChildWithName("PBEQ_Layout");
    if (layoutTree.isValid())
    {
        if (auto layoutXml = layoutTree.createXml())
            savedLayoutXml = layoutXml->toString(juce::XmlElement::TextFormat{}.singleLine());
    }
}

// ---- Phase E —— UI 布局字符串桥接 ----
juce::String Y2KmeterAudioProcessor::getSavedLayoutXml() const
{
    return savedLayoutXml;
}

void Y2KmeterAudioProcessor::setSavedLayoutXml(const juce::String& xml)
{
    savedLayoutXml = xml;
}

// 插件入口
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Y2KmeterAudioProcessor();
}