#pragma once

#include <JuceHeader.h>
#include <atomic>

// 前向声明（AnalyserHub 的完整头下沉到 .cpp，规避 MSVC 多文件同进程编译时
// include guard 跨 TU 串扰问题）
class AnalyserHub;

// ==========================================================
// Y2KmeterAudioProcessor
//
// Phase B 重构：
//   - 原 osc/spectrum 成员迁移到 AnalyserHub
//   - processBlock 改为立体声 L/R 推送
//   - 对外接口通过 getAnalyserHub() 暴露给 UI 模块
// ==========================================================

class Y2KmeterAudioProcessor : public juce::AudioProcessor
{
public:
    Y2KmeterAudioProcessor();
    ~Y2KmeterAudioProcessor() override;

    // AudioProcessor overrides
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---- 分析中心（UI 模块通过此接口获取数据）----
    // 实现在 .cpp 中（头里只有前向声明）
    AnalyserHub& getAnalyserHub() noexcept;

    // UI 可见性驱动的分析开关：false 时 processBlock 跳过全部分析计算
    void setAnalysisActive(bool shouldBeActive) noexcept;
    bool isAnalysisActive() const noexcept;

    // ---- CPU 负载监测 ----
    // 返回 [0..1]：processBlock 在上一周期内的占用比
    // （1.0 ≈ 整个 audio block 的时间全部花在了 processBlock）
    // UI 每帧轮询一次，用于显示到各模块右下角
    double getCpuLoad() const noexcept;

    // Standalone 的 Loopback 路径不会走 processBlock，为了让 CPU 负载仪表
    // 从第一个音频回调开始就有值（而不是一直 0 直到 DAW 场景下 processBlock
    // 触发），Loopback 采集线程在每次喂完样本后调用此接口记录本次耗时。
    //
    // 用法（Loopback 回调里）：
    //   const auto t0 = juce::Time::getMillisecondCounterHiRes();
    //   hub.pushStereo(l, r, numSamples);
    //   const auto elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
    //   proc.registerLoopbackRenderTime(elapsed, numSamples, sampleRate);
    void registerLoopbackRenderTime (double millisecondsTaken,
                                     int   numSamples,
                                     double sampleRate) noexcept;

    // ---- 分析输入增益（仅作用于分析链路，不改变插件音频透传输出）----
    void  setAnalysisInputGainDb (float db) noexcept;
    float getAnalysisInputGainDb() const noexcept;
    float getAnalysisInputGainLinear() const noexcept;

    // ---- 全局性能计数器控制 ----
    juce::File exportPerfCountersNow();
    void setPerfAutoExportEnabled(bool enabled) noexcept;
    bool isPerfAutoExportEnabled() const noexcept;

    // ---- Phase E —— UI 布局持久化（Processor 作为唯一 state owner）----
    // 由 Editor 在 workspace->onLayoutChanged 里写回；Editor 启动时读取并装载。
    juce::String getSavedLayoutXml() const;
    void         setSavedLayoutXml(const juce::String& xml);

    // ---- 布局锁定态持久化（v1.8.3 新增）----
    //   · 抵锁后用户无法拖动/resize 窗口，模块 tile 、贴画也不能拖动/缩放；
    //   · 锁定态本身与具体布局 XML 同下位 - 由不同字段制控，避免影响 loadLayoutFromXml。
    //   · 序列化为 <PBEQ_State layoutLocked="1|0" ...> 属性；
    //     未保存时默认 false。
    bool getLayoutLocked() const noexcept { return savedLayoutLocked; }
    void setLayoutLocked (bool locked) noexcept { savedLayoutLocked = locked; }

    // 新手引导完成状态
    bool isTutorialCompleted() const noexcept { return tutorialCompleted; }
    void setTutorialCompleted (bool completed) noexcept { tutorialCompleted = completed; }

    // ---- 插件 Editor 窗口尺寸持久化 ----
    //   · 由 Editor::resized() 实时写回，保存到 host 的 state 中；
    //   · VST3 / AU 等插件宿主在关闭→重开插件窗口时，宿主不会记住尺寸
    //     （尤其 FL Studio Windows 版），因此我们在 Editor 构造时读取
    //     这里保存的尺寸并 setSize，实现"上次关闭时多大，再次打开就多大"。
    //   · 默认 0 表示未保存；Editor 会在此情况下使用硬编码默认 960×640。
    //   · Standalone 下不使用此值（Standalone 自己走 PropertiesFile 保存位置+尺寸）。
    int getSavedEditorWidth()  const noexcept { return savedEditorWidth; }
    int getSavedEditorHeight() const noexcept { return savedEditorHeight; }
    void setSavedEditorSize (int w, int h) noexcept
    {
        savedEditorWidth  = w;
        savedEditorHeight = h;
    }

    // P4：host 调用 getStateInformation 前，Processor 会先触发此钩子，
    //   让 Editor 端 flush 掉 ModuleWorkspace 里处于 debounce 合并中的
    //   布局变更通知，保证保存到 host 的布局 XML 永远是最新版本。
    //   Editor 构造时注册，析构时清空。
    std::function<void()> flushPendingUiStateBeforeSave;

    // ---- 兼容旧接口（供 EqModule 使用，内部转发到 AnalyserHub）----
    double getCurrentSampleRate() const noexcept;
    void getOscilloscopeSnapshot(juce::Array<float>& dest);   // 返回 L 声道
    void getSpectrumSnapshot(juce::Array<float>& dest);

    bool bypassed = false;

private:
    // pimpl：头里只用前向声明的指针，完整类型只在 .cpp 中可见
    std::unique_ptr<AnalyserHub> analyserHub;

    std::atomic<bool> analysisActive { true };

    // Phase E —— 最近一次保存的布局（由 Editor 同步过来；序列化到 host 状态）
    juce::String savedLayoutXml;

    // 布局锁定态（v1.8.3）：true = 窗口/模块/贴画 均不可拖动/缩放。
    //   · Editor 里的锁定按钮切换时写到这里；
    //   · 本身不影响音频处理，仅作为 UI-only state。
    bool savedLayoutLocked = false;

    // 新手引导完成状态（持久化到 host state / settings 文件中）
    //   · true = 用户已完成新手引导（STEP1→STEP2→complete），后续启动不再触发
    //   · false = 尚未完成；仅在 Standalone 下生效，插件模式忽略
    //   · 序列化为 <PBEQ_State tutorialCompleted="1|0" ...>
    bool tutorialCompleted = false;

    // 插件 Editor 最近一次窗口尺寸（0 = 未保存）
    int savedEditorWidth  = 0;
    int savedEditorHeight = 0;

    // processBlock 时间占比测量器（JUCE 内置，读写原子，实时线程友好）
    juce::AudioProcessLoadMeasurer loadMeasurer;

    // Loopback 路径下的 measurer 状态：首次喂样时懒初始化、采样率/块长变化时重置。
    bool   loopbackMeasurerPrimed = false;
    double loopbackLastSampleRate = 0.0;
    int    loopbackLastBlockSize  = 0;

    // 分析输入增益（dB 与线性值原子缓存，供音频线程无锁读取）
    std::atomic<float> analysisInputGainDb  { 0.0f };
    std::atomic<float> analysisInputGainLin { 1.0f };

    // 当前置增益发生变化时置位；在音频线程 processBlock 开头消费并 reset loudness，
    // 以实现 LUFS-I 自动重置（避免跨线程直接触碰 LoudnessMeter 内部状态）。
    std::atomic<bool>  pendingLoudnessReset { false };

    // P2-2：预分配的分析增益临时缓冲，避免 processBlock 在 gain ≠ 0dB 时
    //   频繁的音频线程堆分配（juce::AudioBuffer 构造 / HeapBlock::malloc）。
    //   · analysisGainBufferStereo 用于立体声路径（1024 帧⨥用，容量不足时
    //     在 prepareToPlay 里由实际 samplesPerBlock 拓宽；
    //   · analysisGainBufferMono   用于 mono 降级路径；
    //   · 两者都只被音频线程读写，不需额外同步。
    juce::AudioBuffer<float> analysisGainBufferStereo;
    juce::HeapBlock<float>   analysisGainBufferMono;
    int                      analysisGainBufferMonoCapacity = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Y2KmeterAudioProcessor)
};