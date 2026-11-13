#pragma once

#include <JuceHeader.h>
#include <vector>
#include "source/ui/GlDirectModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoldSlowMo.h"
#include "source/analysis/AnalyserHub.h"
#include "source/analysis/OscHistory.h"
#include "source/analysis/BandSplit.h"

// ==========================================================
// OscilloscopeModule —— Pink XP 像素风立体声示波器（v3.1：示波器模块）
//
//   合并了旧的 Oscilloscope / Oscilloscope Wave（含 oscilloscope_left / _right）：
//   同一路 Kind::Oscilloscope 的 2048 样本窗口，用「显示类型」+ 参数还原每一种旧外观。
//
//   显示类型（displayMode）：
//     · Waveform  —— 时域波形；声道 L / R / 双（waveChannel），配色跟随主题 chartRamp
//     · X-Y       —— 李萨如 XY（L→X，R→Y，点阵）
//     · Lissajous —— Mid/Side（旋转 45° 的 XY）
//   X-Y / Lissajous 可开启「频段染色」：L/R 各滤成低 / 中 / 高三路，三路各自的
//   点云按固定色叠画（低 = 红 #E60045 / 中 = 蓝紫 #665AFF / 高 = 青 #00FFFF，
//   同 MiniMeters Stereometer）；分频点可调（默认 200 / 2000 Hz，见 BandSplit.h）。
//   散点走持久残影通道，点大小 / 消失速度在控制面板可调。
//
//   v3.1 交互变化：
//     · 顶部工具栏（模式按钮 + Freeze 按钮）取消，整块内容区都是画布；
//     · 旧的 Freeze 按钮取消，改为「按住画布左键 = 慢放」：时间被拉长 50 倍，
//       曲线混向残影色，松手直接回到实时（参照频谱的子弹时间，见 HoldSlowMo.h）；
//     · 慢放期间从 OscHistory 读取更长的历史，所以画面是真的在慢速滚动，而不是定格。
//
//   旧存档兼容：displayMode 键不变；frozen 键被忽略（按未冻结加载）；
//   oscilloscope_wave 的 channelMode 键作为 waveChannel 的别名读取。
// ==========================================================

class OscilloscopeModule : public GlDirectModuleBase,
                           public AnalyserHub::FrameListener
{
public:
    explicit OscilloscopeModule(AnalyserHub& hub);
    ~OscilloscopeModule() override;

    // Phase F：Hub 分发器回调
    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    enum class DisplayMode { waveform, xy, lissajous };

    void setDisplayMode(DisplayMode m);
    DisplayMode getDisplayMode() const noexcept { return displayMode; }

    // 按住慢放：按住画布（非标题栏 / 抽屉）左键开始，松手回到实时
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;

    // ---- Toybox：拍立得快照（反馈 C1，GL 代表 L1 效果）----
    juce::Rectangle<int> getToyCanvasBounds() const { return getCanvasBounds (getContentBounds()); }
    bool toyHitTest (juce::Point<int> panelPos, ToyHit& out) const override;
    void toyPoke    (const ToyHit& hit, ToyContext& ctx) override;
    double lastToyPokeTime_ = 0.0;   // 防连点冷却

    // ---- 持久化 + 控制面板 ----
    juce::ValueTree saveModuleSpecificState() const override;
    void restoreModuleSpecificState(const juce::ValueTree& state) override;
    void   buildParamDefs (std::vector<ParamDef>& out) const override;
    double getParamValue  (const juce::String& id) const override;
    void   setParamValue  (const juce::String& id, double v) override;

protected:
    void layoutContent(juce::Rectangle<int> contentBounds) override;

    // ---- GlDirectModuleBase 钩子（v2.9.4 迁移自 v2.9.3 直渲染样板）----
    void buildGlSnapshot() override;
    juce::Rectangle<int> getGlCanvasBounds (juce::Rectangle<int> contentBounds) override;
    void paintGlOverlay (juce::Graphics& g, juce::Rectangle<int> canvas) override;
    void paintLegacyContent (juce::Graphics& g, juce::Rectangle<int> contentBounds) override;

private:
    // 一个 XY 散点（GL 与 CPU 回退路径共用同一份几何/配色）
    // layer：频段染色时 0/1/2 = 低/中/高（CPU 回退按它分组合并绘制）
    struct XYPoint { float x, y, alpha; juce::Colour col; int layer = 0; };

    // 波形声道配色（glow / main）
    struct WaveColours { juce::Colour lGlow, lMain, rGlow, rMain; };

    // ---- 各模式绘制 ----
    void drawBackground(juce::Graphics& g, juce::Rectangle<int> canvas) const;
    void drawWaveform  (juce::Graphics& g, juce::Rectangle<int> canvas);
    void drawXY        (juce::Graphics& g, juce::Rectangle<int> canvas, bool rotate45);
    juce::Path buildWaveformPath(const juce::Array<float>& samples,
                                 juce::Rectangle<int> inner,
                                 float yCenter,
                                 float halfHeight) const;

    void collectXYPoints (juce::Rectangle<int> canvas, bool rotate45, std::vector<XYPoint>& out) const;
    WaveColours waveColours() const;
    bool drawsL() const noexcept { return waveChannel == 0 || waveChannel == 2; }
    bool drawsR() const noexcept { return waveChannel == 1 || waveChannel == 2; }
    bool xyBandActive() const noexcept { return bandColour && displayMode != DisplayMode::waveform; }

    void rebuildStaticLayerIfNeeded(juce::Rectangle<int> contentBounds);
    void drawStaticLayer(juce::Graphics& g, juce::Rectangle<int> contentBounds) const;
    void invalidateStaticLayer();

    // 方案 A：动态波形 / XY 点图缓存层
    void redrawDynamicLayerIfNeeded(juce::Rectangle<int> canvas);
    void invalidateDynamicLayer();
    bool snapshotChangedSinceLastDraw() const noexcept;

    juce::Rectangle<int> getCanvasBounds  (juce::Rectangle<int> content) const;

    // 慢放
    void beginHold (const AnalyserHub::FrameSnapshot& frame);
    void endHold();

    // ---- 成员 ----
    AnalyserHub& hub;

    // 参数（构造默认 = 旧 Oscilloscope 外观）
    DisplayMode displayMode = DisplayMode::waveform;
    int   waveChannel = 2;        // 0 L / 1 R / 2 双
    bool  bandColour  = false;    // X-Y / Lissajous 频段染色
    float crossLowHz  = y2k::bands::BandSplitter::kDefaultLowCross;    // 200 Hz（MiniMeters 默认）
    float crossHighHz = y2k::bands::BandSplitter::kDefaultHighCross;   // 2 kHz
    bool  autoGain    = true;     // X-Y / Lissajous 自动缩放
    bool  showRings   = true;     // X-Y / Lissajous 标尺环
    float dotSize     = 2.0f;     // 散点边长（px，1–6）
    float fadeSpeed   = 1.0f;     // 残影消失速度倍率（1.0 = 经典拖尾）

    // 快照（UI 线程独占使用）：实时窗口，或慢放时从历史读出的窗口
    juce::Array<float> snapshotL;
    juce::Array<float> snapshotR;
    // snapshotL[0] 的绝对样本索引：抽点渲染按它对齐相位，慢放时画面不会在
    //   几种抽点形状之间来回闪（见 buildGlSnapshot 的 pushWave）
    juce::int64 snapshotBaseIndex = 0;

    // 三频段分离结果（与 snapshot 同长；仅 xyBandActive 时有效）
    y2k::bands::BandSplitter splitter;
    std::vector<float> lowL, lowR, midL, midR, highL, highR;
    double splitterRate = 0.0;

    // 散点残影：累计 dt 用于按帧率无关的衰减（与 StereoField 同策略）
    static constexpr float kTrailDecayPerFrame = 0.92f;
    float trailDtAccumSec = 0.0f;
    bool  trailNeedsClear = true;   // 切换模式 / 开关染色 / 改分频后要求清一次 trail

    // 按住慢放
    HoldSlowMo hold;
    OscHistory history;
    bool   holdPendingStart = false;
    double holdPlayPos = 0.0;      // 读指针（绝对样本位置，窗口末端）
    std::vector<float> readBufL, readBufR;

    juce::Image staticLayer;
    juce::Rectangle<int> staticLayerContentBounds;
    int themeSubToken = -1;

    // 方案 A：动态层缓存
    juce::Image dynamicLayer;
    juce::Rectangle<int> dynamicLayerCanvasBounds; // 以 canvas 坐标系存
    bool        dynamicLayerDirty = true;

    // 粗粒度指纹：采样长度 + 首/中/尾元素 —— 用来判断 snapshot 是否真的变了
    int   lastDrawnSampleCount = -1;
    float lastDrawnFingerprint[6] { 0, 0, 0, 0, 0, 0 };
    DisplayMode lastDrawnMode = DisplayMode::waveform;

    // XY / Lissajous 自动缩放（v1.8.5）
    float autoGainFactor              = 1.0f;    // 当前增益系数 [0.1, 10]
    float periodicMaxAccum            = 0.0f;    // 当前周期内累积的样本最大距离
    float prevPeriodicMax             = 1.0f;
    float lastDrawnAutoGain           = 1.0f;    // 上次绘制时的 autoGainFactor，用于动态层脏标记
    double lastPeriodicGainUpdateMs   = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OscilloscopeModule)
};
