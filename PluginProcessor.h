#pragma once

#include <JuceHeader.h>
#include <atomic>

#include "source/ui/modules/MilkdropVisualState.h"
#include "source/ui/modules/MilkdropWaveState.h"
#include "source/ui/MilkdropParamIds.h"

// 前向声明（AnalyserHub 的完整头下沉到 .cpp，规避 MSVC 多文件同进程编译时
// include guard 跨 TU 串扰问题）
class AnalyserHub;

// ==========================================================
// HostPlayheadSnapshot —— 宿主播放位置快照（音频线程写，UI 线程读）
//
//   由 processBlock 在每个音频块开头从 AudioPlayHead 读取并缓存到
//   原子字段，供 Milkdrop 模块的 BPM 同步自动切换模式在 UI 线程
//   轮询读取（无跨线程锁）。
// ==========================================================
struct HostPlayheadSnapshot
{
    bool   valid       = false;  // 是否有可用的播放位置信息（无 playhead / 宿主未报告时为 false）
    bool   isPlaying   = false;  // 宿主是否正在播放
    double bpm         = 0.0;    // 当前 BPM（0 = 未知）
    double ppqPosition = 0.0;    // 当前播放位置（四分音符）
};

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

    // 消费挂起的 Loudness 重置请求（LUFS-I 归零重算）。
    //   · 常规插件/设备输入模式下由 processBlock 自动调用；
    //   · 当音频不经过 processBlock 时（macOS ScreenCaptureKit 桌面采集、
    //     Windows WASAPI loopback），必须由采集回调显式调用一次，
    //     否则改增益后 LUFS-I 积分不会重新开始。
    void consumePendingLoudnessReset() noexcept;

    // ---- 全局性能计数器控制 ----
    juce::File exportPerfCountersNow();
    void setPerfAutoExportEnabled(bool enabled) noexcept;
    bool isPerfAutoExportEnabled() const noexcept;

    // ---- Phase E —— UI 布局持久化（Processor 作为唯一 state owner）----
    // 由 Editor 在 workspace->onLayoutChanged 里写回；Editor 启动时读取并装载。
    //
    // v2.8.5：布局按"窗口形态"分三槽保存 —— 非 stick / stick 顶(T) / stick 底(B)。
    //   · 三种形态各自保存互不覆盖：用户在 T/B 之间来回切换不会丢掉任何一侧
    //     调好的布局；stick 期间改动也会落盘（重启后仍在）。
    //   · 旧存档只有 <PBEQ_Layout>（= 非 stick 槽），读取时自动视为 normal。
    enum class LayoutSlot { normal = 0, stickTop = 1, stickBottom = 2 };

    juce::String getSavedLayoutXml() const;                     // = normal 槽（兼容旧调用）
    void         setSavedLayoutXml (const juce::String& xml);   // = normal 槽
    juce::String getSavedLayoutXml (LayoutSlot slot) const;
    void         setSavedLayoutXml (LayoutSlot slot, const juce::String& xml);

    // 脱离态模块独立存档（v2.8.5）：
    //   · 与 normal / stickT / stickB 三个布局槽完全解耦，槽切换不会改动它；
    //   · 记录 moduleId、类型、dock 回位 bounds、浮动窗口屏幕坐标、模块业务状态；
    //   · 删除 / 停靠浮动模块后由下一次 persist 覆盖写回。
    juce::String getSavedFloatingModulesXml() const { return savedFloatingModulesXml; }
    void         setSavedFloatingModulesXml (const juce::String& xml) { savedFloatingModulesXml = xml; }

    // ---- stick 会话持久化（Windows 专属语义；其它平台仅原样保存字段）----
    //   active : 上次退出时是否处于 AppBar 驻留态 → 重启后自动恢复
    //   bottom : 驻留边（false = 顶 T，true = 底 B）
    bool isSavedStickActive() const noexcept { return savedStickActive; }
    bool isSavedStickBottom() const noexcept { return savedStickBottom; }
    void setSavedStickSession (bool active, bool bottom) noexcept
    {
        savedStickActive = active;
        savedStickBottom = bottom;
    }

    // stick 驻留条的逻辑高度（Editor 高度），按 T/B 分别记录。
    //   · 顶/底两侧的条高互不影响：在 T 侧拉高后切到 B，B 仍是它自己上次的高度。
    int  getSavedStickEditorHeight (bool bottom) const noexcept
    {
        return bottom ? savedStickEditorHeightBottom : savedStickEditorHeightTop;
    }
    void setSavedStickEditorHeight (bool bottom, int h) noexcept
    {
        if (bottom)
            savedStickEditorHeightBottom = juce::jmax (0, h);
        else
            savedStickEditorHeightTop    = juce::jmax (0, h);
    }

    // 进入 stick 前的"非 stick"窗口 bounds（退出 stick / 重启后再退出时还原）
    juce::Rectangle<int> getSavedStickRestoreBounds() const noexcept
    {
        return { savedStickRestoreX, savedStickRestoreY, savedStickRestoreW, savedStickRestoreH };
    }
    void setSavedStickRestoreBounds (juce::Rectangle<int> r) noexcept
    {
        savedStickRestoreX = r.getX();
        savedStickRestoreY = r.getY();
        savedStickRestoreW = r.getWidth();
        savedStickRestoreH = r.getHeight();
    }

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

    // ---- Toybox（v3.0.0）档位持久化 ----
    //   · 序列化为 <PBEQ_State toyLevel="…" ...>；-1 = 未保存（Editor 按形态默认值兜底）。
    int  getSavedToyLevel() const noexcept { return savedToyLevel; }
    void setSavedToyLevel (int level) noexcept { savedToyLevel = level; }

    // P4：host 调用 getStateInformation 前，Processor 会先触发此钩子，
    //   让 Editor 端 flush 掉 ModuleWorkspace 里处于 debounce 合并中的
    //   布局变更通知，保证保存到 host 的布局 XML 永远是最新版本。
    //   Editor 构造时注册，析构时清空。
    std::function<void()> flushPendingUiStateBeforeSave;

    // ---- Milkdrop 整体染色 + 效果持久化（全局状态，所有 Milkdrop 模块共享）----
    //   · tint_r/g/b 为加性偏移系数（master output colors），默认 1.0 = 中性；
    //   · brightness 为软 gamma 亮度系数，默认 1.0；
    //   · invert / shadows 为效果开关；
    //   · Editor 每次 SetMilkdropVisualState 时同步写回，序列化到 host state
    //     顶层属性；Editor 构造时读回，实现关闭→重开软件后复原。
    void setSavedMilkdropVisualState (const MilkdropVisualState& state) noexcept
    {
        savedMilkdropVisualState_ = state;
    }
    const MilkdropVisualState& getSavedMilkdropVisualState() const noexcept
    {
        return savedMilkdropVisualState_;
    }

    // ---- Milkdrop 简单波形样式覆盖持久化（全局状态，所有 Milkdrop 模块共享）----
    //   · 与 visual state 同构：Editor 每次 SetMilkdropWaveState 时同步写回，
    //     序列化到 host state 顶层属性；Editor 构造时读回。
    void setSavedMilkdropWaveState (const MilkdropWaveState& state) noexcept
    {
        savedMilkdropWaveState_ = state;
    }
    const MilkdropWaveState& getSavedMilkdropWaveState() const noexcept
    {
        return savedMilkdropWaveState_;
    }

    // ---- Milkdrop 收藏库切换状态持久化（全局状态，仅一个 Milkdrop 模块）----
    //   · true = 当前浏览的是收藏库 milkdrop_presets_like；
    //   · false = 浏览内置库 milkdrop_presets（默认）。
    //   · Editor 每次切换时同步写回，序列化到 host state 顶层属性；
    //     Editor 构造时读回，实现关闭→重开软件后仍停留在上次浏览的库。
    void setSavedMilkdropUseLikeLibrary (bool useLike) noexcept
    {
        savedMilkdropUseLikeLibrary_.store (useLike);
    }
    bool getSavedMilkdropUseLikeLibrary() const noexcept
    {
        return savedMilkdropUseLikeLibrary_.load();
    }

    // ---- 宿主播放位置快照（供 Milkdrop BPM 同步模式读取）----
    //   · 返回 processBlock 最近一次从 AudioPlayHead 缓存的播放信息；
    //   · UI 线程调用，读原子字段，无锁安全。
    HostPlayheadSnapshot getHostPlayheadSnapshot() const noexcept;

    // ---- 兼容旧接口（供 EqModule 使用，内部转发到 AnalyserHub）----
    double getCurrentSampleRate() const noexcept;
    void getOscilloscopeSnapshot(juce::Array<float>& dest);   // 返回 L 声道
    void getSpectrumSnapshot(juce::Array<float>& dest);

    bool bypassed = false;

    // ---- 宿主自动化参数树（milkdrop-only 与完整版插件共用）----
    // 暴露 Milkdrop 渲染参数（color/effects/wave/tweak/auto + 预设切换开关），
    // 供宿主通过 MIDI CC / automation 控制。所有形态都创建参数树：插件宿主
    // 可枚举并自动化，Standalone 无宿主枚举但无副作用（仍用于参数持久化）。
    juce::AudioProcessorValueTreeState& getApvts() noexcept { return *apvts; }

private:
    // pimpl：头里只用前向声明的指针，完整类型只在 .cpp 中可见
    std::unique_ptr<AnalyserHub> analyserHub;

    std::atomic<bool> analysisActive { true };

    // Phase E —— 最近一次保存的布局（由 Editor 同步过来；序列化到 host 状态）
    //   · savedLayoutXml            = 非 stick 槽（旧存档里的 <PBEQ_Layout>）
    //   · savedLayoutXmlStickTop    = stick 顶(T) 槽
    //   · savedLayoutXmlStickBottom = stick 底(B) 槽
    juce::String savedLayoutXml;
    juce::String savedLayoutXmlStickTop;
    juce::String savedLayoutXmlStickBottom;

    // 脱离态模块独立存档（根节点 PBEQ_FloatingModules；不隶属于任何布局槽）
    juce::String savedFloatingModulesXml;

    // stick 会话状态（v2.8.5）：重启后据此自动恢复驻留态与方向
    bool savedStickActive = false;
    bool savedStickBottom = false;
    // 驻留条逻辑高度（按侧独立；0 = 未保存，用默认布局高度）
    int  savedStickEditorHeightTop    = 0;
    int  savedStickEditorHeightBottom = 0;
    int  savedStickRestoreX = 0, savedStickRestoreY = 0,
         savedStickRestoreW = 0, savedStickRestoreH = 0;

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

    // Toybox 档位（0=Off / 1=Chill / 2=Normal / 3=Party）；-1 = 未保存
    int savedToyLevel = -1;

    // Milkdrop 整体染色 + 效果（全局状态，持久化到 host state）
    MilkdropVisualState savedMilkdropVisualState_;

    // Milkdrop 简单波形样式覆盖（全局状态，持久化到 host state）
    MilkdropWaveState savedMilkdropWaveState_;

    // Milkdrop 收藏库切换状态（全局状态，持久化到 host state）。
    // 用 atomic 消除 GL 线程（删空回退）与 UI/host 线程之间的读写竞争。
    std::atomic<bool> savedMilkdropUseLikeLibrary_ { false };

    // ---- 宿主播放位置缓存（音频线程写，UI 线程读，原子无锁）----
    //   · processBlock 每个音频块开头从 AudioPlayHead 读取并缓存；
    //   · Standalone 形态无 playhead，valid 恒为 false。
    std::atomic<bool>   hostPlayheadValid   { false };
    std::atomic<bool>   hostPlayheadPlaying { false };
    std::atomic<double> hostPlayheadBpm     { 0.0 };
    std::atomic<double> hostPlayheadPpq     { 0.0 };

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

    // 宿主自动化参数树（用 unique_ptr 延迟构造，APVTS 需绑定 *this）。
    std::unique_ptr<juce::AudioProcessorValueTreeState> apvts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Y2KmeterAudioProcessor)
};