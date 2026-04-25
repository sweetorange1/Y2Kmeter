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

    // ---- Phase E —— UI 布局持久化（Processor 作为唯一 state owner）----
    // 由 Editor 在 workspace->onLayoutChanged 里写回；Editor 启动时读取并装载。
    juce::String getSavedLayoutXml() const;
    void         setSavedLayoutXml(const juce::String& xml);

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

    // processBlock 时间占比测量器（JUCE 内置，读写原子，实时线程友好）
    juce::AudioProcessLoadMeasurer loadMeasurer;

    // Loopback 路径下的 measurer 状态：首次喂样时懒初始化、采样率/块长变化时重置。
    bool   loopbackMeasurerPrimed = false;
    double loopbackLastSampleRate = 0.0;
    int    loopbackLastBlockSize  = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Y2KmeterAudioProcessor)
};