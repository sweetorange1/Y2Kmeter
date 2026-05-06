#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "source/analysis/AnalyserHub.h"
#include "source/perf/PerformanceCounterSystem.h"

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
                juce::AudioBuffer<float> analysisTemp (2, n);
                analysisTemp.copyFrom (0, 0, buffer, 0, 0, n);
                analysisTemp.copyFrom (1, 0, buffer, 1, 0, n);
                analysisTemp.applyGain (0, 0, n, gainLin);
                analysisTemp.applyGain (1, 0, n, gainLin);

                analyserHub->pushStereo (analysisTemp.getReadPointer (0),
                                         analysisTemp.getReadPointer (1),
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
                juce::HeapBlock<float> mono;
                mono.malloc ((size_t) n);
                const auto* src = buffer.getReadPointer (0);
                for (int i = 0; i < n; ++i)
                    mono[i] = src[i] * gainLin;

                analyserHub->pushStereo (mono.get(), mono.get(), n);
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
    // 顶层 <PBEQ_State>
    //   <Layout>...</Layout>        ← UI 布局
    juce::ValueTree root("PBEQ_State");
    root.setProperty("version", 1, nullptr);
    root.setProperty("analysisInputGainDb",
                     (double) analysisInputGainDb.load (std::memory_order_relaxed),
                     nullptr);

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