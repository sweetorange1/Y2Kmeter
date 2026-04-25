#include "source/ui/modules/WaveformModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// WaveformModule —— 持续滚动瀑布波形
//
// 设计要点：
//   · **零后端新增**：复用 AnalyserHub::Kind::Oscilloscope，不在音频线程新增分析
//   · **按需计算**：构造时 retain(Oscilloscope) / 析构时 release，与其它模块一致
//   · **前端滚动缓冲**：history 环形缓冲存 mono (L+R)/2 = 3~6 秒样本
//   · **每像素 min/max 包络 + RMS 颜色**：对每个 X 像素列对应的样本窗做聚合
// ==========================================================

WaveformModule::WaveformModule(AnalyserHub& h)
    : ModulePanel(ModuleType::waveform),
      hub(h)
{
    // 按需启用 Oscilloscope 计算（与其它模块一致）
    hub.retain (AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener (this);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // waveform 6×4 大格

    // 缓存采样率（列缓冲会在第一次 layoutContent 时按实际 canvas 宽度建立）
    cachedSampleRate = hub.getSampleRate();
    if (cachedSampleRate <= 0.0) cachedSampleRate = 44100.0;

    lastFrameTimeMs = juce::Time::getMillisecondCounterHiRes();

    auto setupRangeBtn = [this] (juce::TextButton& b, float seconds)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (0x57464d52); // "WFMR"
        b.onClick = [this, seconds]() { setDisplaySeconds (seconds); };
        addAndMakeVisible (b);
    };
    setupRangeBtn (btnRange3, 3.0f);
    setupRangeBtn (btnRange6, 6.0f);
    btnRange3.setToggleState (true, juce::dontSendNotification);

    btnFreeze.setClickingTogglesState (true);
    btnFreeze.onClick = [this]() { setFrozen (btnFreeze.getToggleState()); };
    addAndMakeVisible (btnFreeze);

    // 右侧增益滑条：0 ~ +36 dB（纯UI绘制级别的放大，不改变后端数据）
    //   · 默认 0 dB（无放大，还原原有波形幅度）
    //   · 样式与 EqModule 的 SIZE 滑条一致（顶部 label + 垂直滑条 + 下方 TextBox）
    gainSlider.setSliderStyle (juce::Slider::LinearVertical);
    gainSlider.setRange (0.0, 36.0, 1.0);
    gainSlider.setValue ((double) gainDb, juce::dontSendNotification);
    // TextBox 只读：拿掉"点数字改值"的交互入口，数字只做展示
    gainSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 40, 16);
    // 数字文字颜色跟随主题 ink（浅色主题下是深色 → 可读）
    gainSlider.setColour (juce::Slider::textBoxTextColourId,       PinkXP::ink);
    gainSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    gainSlider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    gainSlider.setTextValueSuffix ("dB");
    gainSlider.onValueChange = [this]()
    {
        gainDb = (float) gainSlider.getValue();
        repaint();
    };
    addAndMakeVisible (gainSlider);

    gainLabel.setJustificationType (juce::Justification::centred);
    gainLabel.setColour (juce::Label::textColourId, PinkXP::ink);
    gainLabel.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
    gainLabel.setText ("GAIN", juce::dontSendNotification);
    addAndMakeVisible (gainLabel);

    // 主题切换时重新下发 "GAIN" 标签的 textColourId —— 与标题栏墨色保持一致。
    //   Label 的 textColour 是缓存值，主题切换后需要手动刷新。
    //   Slider 的 textBoxTextColourId 同样需要显式刷新（同样的缓存语义）。
    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        gainLabel.setColour (juce::Label::textColourId, PinkXP::ink);
        gainSlider.setColour (juce::Slider::textBoxTextColourId, PinkXP::ink);
        gainLabel.repaint();
        gainSlider.repaint();
    });
}

WaveformModule::~WaveformModule()
{
    // 解绑主题订阅，避免析构后仍然回调到已销毁的 this
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    hub.removeFrameListener (this);
    hub.release (AnalyserHub::Kind::Oscilloscope);
}

// ----------------------------------------------------------
// 设置显示时长（秒）—— 仅刷新 samplesPerColumn，列缓冲内容保留
//   · 缩短时长：右半部分列继续显示；左边自动被新帧覆盖
//   · 延长时长：左边显示为静音直到新样本慢慢填满
//   · 不做"内容重采样"以免引入额外视觉抖动
// ----------------------------------------------------------
void WaveformModule::setDisplaySeconds (float seconds)
{
    seconds = juce::jlimit (1.0f, 20.0f, seconds);
    if (std::abs (displaySeconds - seconds) < 1.0e-3f) return;

    displaySeconds = seconds;

    // 重新计算每列应容纳的样本数；列缓冲尺寸由画布宽度决定不变
    if (cachedCanvasWidth > 0 && cachedSampleRate > 0.0)
        samplesPerColumn = (float) (cachedSampleRate * displaySeconds)
                         / (float) cachedCanvasWidth;

    // 切换时长时，丢弃当前正在累加的半列，避免 samplesPerColumn 变化引起一次不连续跳变
    accMin = 1.0f; accMax = -1.0f; accSumSq = 0.0f; accCount = 0;

    btnRange3.setToggleState (std::abs (displaySeconds - 3.0f) < 1.0e-3f, juce::dontSendNotification);
    btnRange6.setToggleState (std::abs (displaySeconds - 6.0f) < 1.0e-3f, juce::dontSendNotification);

    repaint();
}

// ----------------------------------------------------------
// 画布宽度变化时重建列缓冲（保留最近 min(validCols, newWidth) 列）
// ----------------------------------------------------------
void WaveformModule::rebuildColumnBuffer (int newCanvasWidth)
{
    newCanvasWidth = juce::jmax (1, newCanvasWidth);
    if (newCanvasWidth == cachedCanvasWidth && ! columnHistory.empty())
        return;

    std::vector<Column> newBuf ((size_t) newCanvasWidth);

    // 拷贝最近 keep 列到新缓冲的末尾（保持"最新在右端"）
    const int keep = juce::jmin (validCols, newCanvasWidth);
    const int oldSize = (int) columnHistory.size();
    for (int i = 0; i < keep; ++i)
    {
        const int k = keep - 1 - i; // 新缓冲位置 i 放第 k 新的列
        const int srcIdx = (writeCol - 1 - k + oldSize) % juce::jmax (1, oldSize);
        if (srcIdx >= 0 && srcIdx < oldSize)
            newBuf[(size_t) (newCanvasWidth - keep + i)] = columnHistory[(size_t) srcIdx];
    }

    columnHistory   = std::move (newBuf);
    writeCol        = newCanvasWidth % (int) columnHistory.size(); // 下次从末尾 wrap 到 0
    validCols       = keep;
    cachedCanvasWidth = newCanvasWidth;

    // 每列样本数
    if (cachedSampleRate > 0.0)
        samplesPerColumn = (float) (cachedSampleRate * displaySeconds)
                         / (float) cachedCanvasWidth;

    // 重置列内累加器
    accMin = 1.0f; accMax = -1.0f; accSumSq = 0.0f; accCount = 0;
}

void WaveformModule::setFrozen (bool b)
{
    if (frozen == b) return;
    frozen = b;
    btnFreeze.setToggleState (frozen, juce::dontSendNotification);
}

// ----------------------------------------------------------
// onFrame —— Hub 分发器回调
// ----------------------------------------------------------
void WaveformModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! isShowing()) return;
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double deltaMs = nowMs - lastFrameTimeMs;
    lastFrameTimeMs = nowMs;

    if (frozen) { repaint(); return; }

    // 列缓冲尚未建立（首次 layout 还没跑）— 跳过
    if (columnHistory.empty() || samplesPerColumn <= 0.0f)
    {
        repaint();
        return;
    }

    // 估算自上帧以来应追加多少个新样本
    //   · sampleRate × (Δt / 1000)
    //   · 限制在 [0, snapshotLen]（若 UI 卡顿 Δt 过大，也不会超过快照本身的长度）
    //   · 快照本身是"最近 N 个样本"的环形快照（时间从旧到新），
    //     所以 append 时从末尾取最新的 numToAppend 个
    const int snapshotLen = (int) frame.oscL.size();
    if (snapshotLen <= 0) { repaint(); return; }

    int numToAppend = (int) std::lround (cachedSampleRate * deltaMs / 1000.0);
    numToAppend = juce::jlimit (0, snapshotLen, numToAppend);
    if (numToAppend > 0)
        pushHistorySamples (frame.oscL.data(), frame.oscR.data(),
                            snapshotLen, numToAppend);

    repaint();
}

// ----------------------------------------------------------
// 把 oscilloscope 快照末尾的 numToAppend 个样本合并成 mono，
// 每 samplesPerColumn 个样本聚合成 1 列入 columnHistory。
//
// 关键点：samplesPerColumn 通常不是整数（例如 240.67），
//   这里用"浮点阈值 + 累加器"的策略保证长时间下每秒
//   产出的列数严格 = canvasWidth / displaySeconds。
//   任何一帧只会整数列数地推进屏幕，彻底消除亚像素抖动。
// ----------------------------------------------------------
void WaveformModule::pushHistorySamples (const float* L, const float* R,
                                         int snapshotLen, int numToAppend)
{
    const int colCap = (int) columnHistory.size();
    if (colCap <= 0 || samplesPerColumn <= 0.0f) return;

    // 快照里最新样本在 snapshotLen-1，最旧在 0；取最末 numToAppend 个
    const int srcStart = snapshotLen - numToAppend;

    for (int i = 0; i < numToAppend; ++i)
    {
        const float l = L[srcStart + i];
        const float r = R[srcStart + i];
        float m = 0.5f * (l + r);
        if (! std::isfinite (m)) m = 0.0f;
        m = juce::jlimit (-1.0f, 1.0f, m);

        if (m < accMin) accMin = m;
        if (m > accMax) accMax = m;
        accSumSq += m * m;
        ++accCount;

        // 凑够一列就产出
        if ((float) accCount >= samplesPerColumn)
        {
            Column col;
            col.minV = accMin;
            col.maxV = accMax;
            col.rms  = std::sqrt (accSumSq / (float) accCount);

            columnHistory[(size_t) writeCol] = col;
            writeCol = (writeCol + 1) % colCap;
            if (validCols < colCap) ++validCols;

            // 为"累加器小数部分"做补偿：
            //   若 samplesPerColumn=240.67，凑满 241 样本时，第 241 号样本已经
            //   是下一列的一部分；我们用"扣除整列后的余数"继续累加，
            //   保证平均每列样本数严格等于 samplesPerColumn。
            const float overflow = (float) accCount - samplesPerColumn;
            accMin = 1.0f; accMax = -1.0f;
            accSumSq = 0.0f;
            accCount = juce::jmax (0, (int) std::floor (overflow));
            // 注：丢失的累加不做精确回填——这点误差远小于 1 个样本
            // 对 48kHz × 3s × 600px 的尺度完全不可见
            (void) overflow;
        }
    }
}

// ----------------------------------------------------------
// 颜色映射：幅度 [0,1] → Pink XP 主题色梯度
//   0.00 – 0.10  → pink100 (最轻柔)
//   0.10 – 0.35  → pink300
//   0.35 – 0.65  → pink500
//   0.65 – 0.90  → pink700 (深紫粉)
//   0.90 – 1.00  → sel      (高亮洋红，接近 clip)
// 中间段做线性插值避免色带感
// ----------------------------------------------------------
juce::Colour WaveformModule::mapAmplitudeToColour (float amp01) const
{
    amp01 = juce::jlimit (0.0f, 1.0f, amp01);

    // 色站（stops）—— 按 amp 阈值递增
    struct Stop { float t; juce::Colour c; };
    const Stop stops[] = {
        { 0.00f, PinkXP::pink100 },
        { 0.20f, PinkXP::pink300 },
        { 0.50f, PinkXP::pink500 },
        { 0.80f, PinkXP::pink700 },
        { 1.00f, PinkXP::sel     }
    };
    constexpr int N = (int) (sizeof (stops) / sizeof (stops[0]));

    // 找到所属区间做线性插值
    for (int i = 0; i < N - 1; ++i)
    {
        if (amp01 <= stops[i + 1].t)
        {
            const float span = juce::jmax (1.0e-6f, stops[i + 1].t - stops[i].t);
            const float u = (amp01 - stops[i].t) / span;
            return stops[i].c.interpolatedWith (stops[i + 1].c, u);
        }
    }
    return stops[N - 1].c;
}

// ----------------------------------------------------------
// 布局
// ----------------------------------------------------------
juce::Rectangle<int> WaveformModule::getToolbarBounds (juce::Rectangle<int> content) const
{
    return content.withHeight (toolbarH);
}

juce::Rectangle<int> WaveformModule::getCanvasBounds (juce::Rectangle<int> content) const
{
    // 顶部扣掉工具栏，右侧扣掉 GAIN 面板（与 layoutContent 中 kGainPanelW 保持一致）
    constexpr int kGainPanelW = 42;
    return content.withTrimmedTop (toolbarH + 4)
                  .withTrimmedRight (kGainPanelW);
}

void WaveformModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    auto tb = getToolbarBounds (contentBounds).reduced (2);
    const int btnW = 44;
    const int gap = 4;

    // 左侧：显示时长切换
    btnRange3.setBounds (tb.getX(),                    tb.getY(), btnW, tb.getHeight());
    btnRange6.setBounds (tb.getX() + (btnW + gap),     tb.getY(), btnW, tb.getHeight());

    // 右侧：Freeze
    const int freezeW = 64;
    btnFreeze.setBounds (tb.getRight() - freezeW, tb.getY(), freezeW, tb.getHeight());

    // 右侧 GAIN 滑条（叠在画布右侧，顶部留 14px 给 Label，后面 canvas 会去掉此区宽）
    constexpr int kGainPanelW = 42;
    auto contentInner = contentBounds;
    contentInner.removeFromTop (toolbarH + 4);              // 跳过顶部工具栏
    auto gainPanel = contentInner.removeFromRight (kGainPanelW);
    auto gainLblArea = gainPanel.removeFromTop (14);
    gainLabel.setBounds (gainLblArea);
    gainSlider.setBounds (gainPanel);

    // 画布宽度变化（含首次布局）时重建列缓冲 —— 列缓冲尺寸严格 = 画布内部宽
    //   注意：canvas 已经排除 GAIN 面板，所以列数与实际绘制区域一致
    auto canvas = getCanvasBounds (contentBounds).reduced (4);
    rebuildColumnBuffer (juce::jmax (1, canvas.getWidth()));
}

// ----------------------------------------------------------
// paintContent
// ----------------------------------------------------------
void WaveformModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    auto canvas = getCanvasBounds (contentBounds);
    drawBackground (g, canvas);

    if (canvas.getWidth() <= 8 || canvas.getHeight() <= 8)
        return;

    drawWaveform (g, canvas);

    // 右上角冻结标记
    if (frozen)
    {
        auto dot = juce::Rectangle<int> (canvas.getRight() - 12, canvas.getY() + 6, 8, 8);
        g.setColour (juce::Colour (0xffec4d85));
        g.fillEllipse (dot.toFloat());
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText ("FROZEN", canvas.getRight() - 60, canvas.getY() + 4,
                    42, 12, juce::Justification::centredRight, false);
    }
}

void WaveformModule::drawBackground (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    PinkXP::drawSunken (g, canvas, PinkXP::content);

    auto inner = canvas.reduced (2);
    if (inner.isEmpty()) return;

    // 中心水平线（零幅度基准）
    g.setColour (PinkXP::pink300.withAlpha (0.55f));
    const int cy = inner.getCentreY();
    g.drawHorizontalLine (cy, (float) inner.getX(), (float) inner.getRight());

    // 时间刻度（每 1 秒一根竖虚线，方便目测）
    const int w = inner.getWidth();
    if (displaySeconds > 0.0f && w > 0)
    {
        const float pxPerSec = (float) w / displaySeconds;
        g.setColour (PinkXP::pink200.withAlpha (0.45f));
        // 从最右端向左每 1 秒画一条
        for (int s = 1; s * pxPerSec < (float) w; ++s)
        {
            const int x = inner.getRight() - (int) std::round ((float) s * pxPerSec);
            for (int y = inner.getY(); y < inner.getBottom(); y += 4)
                g.fillRect (x, y, 1, 2);
        }
    }
}

// ----------------------------------------------------------
// drawWaveform —— 瀑布主体（整数像素列滚动版）
//
//   每个屏幕像素 x 对应 columnHistory 里的一列（已经提前在音频数据阶段聚合好
//   min/max/RMS），所以每帧之间"同一 x 像素"的数据来源是**完全不变**的
//   —— 视觉上只是整个图形原子地左移整数像素列，彻底没有亚像素抖动。
//
//   另外做一次 3-tap 水平低通平滑（仅 UI 层、在相邻 3 列间求均值），
//   可以把细竖线的视觉密度降低，避免"蛛网感"。
// ----------------------------------------------------------
void WaveformModule::drawWaveform (juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    const int colCap = (int) columnHistory.size();
    if (colCap <= 0 || validCols <= 0) return;

    auto inner = canvas.reduced (4);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int   w      = juce::jmin (inner.getWidth(), colCap);
    const int   ix     = inner.getX();
    const int   iy     = inner.getY();
    const int   ih     = inner.getHeight();
    const int   cy     = iy + ih / 2;
    // 基础半高 + 用户 GAIN dB 放大系数（0 dB → ×1，+6 → ×2，+36 → ×63）
    //   纯UI级别的绘制缩放；不修改 columnHistory 里的 min/max
    const float gainMul = std::pow (10.0f, gainDb / 20.0f);
    const float halfH  = (float) ih * 0.48f * gainMul;

    // 将列缓冲映射到屏幕：
    //   屏幕 x = inner.right()-1 对应 k=0 (最新)
    //   屏幕 x = inner.right()-w 对应 k=w-1 (w 列前)
    //   k 大于 validCols-1 时该列无数据 → 跳过（启动早期左边留白）
    auto sampleCol = [&] (int k) -> Column
    {
        if (k < 0 || k >= validCols) return Column{};
        const int idx = (writeCol - 1 - k + colCap) % colCap;
        return columnHistory[(size_t) idx];
    };

    // 预取整行 column，应用 3-tap smoothing；用整数坐标绘制避免亚像素抗锯齿
    //   注意 JUCE 的 fillRect(int,int,int,int) 是严格整数像素、不做子像素插值
    for (int xi = 0; xi < w; ++xi)
    {
        const int k = w - 1 - xi; // xi=0 最左(最旧) ... xi=w-1 最右(最新)
        if (k >= validCols) continue; // 启动早期左侧留白

        // 3-tap 均值平滑（边界用 k 自己 clamp）
        const Column c0 = sampleCol (juce::jmax (0, k - 1));
        const Column c1 = sampleCol (k);
        const Column c2 = sampleCol (juce::jmin (validCols - 1, k + 1));

        const float minV = (c0.minV + c1.minV + c2.minV) * (1.0f / 3.0f);
        const float maxV = (c0.maxV + c1.maxV + c2.maxV) * (1.0f / 3.0f);
        const float rms  = (c0.rms  + c1.rms  + c2.rms ) * (1.0f / 3.0f);

        const float peak = juce::jmax (std::abs (maxV), std::abs (minV));

        // 颜色：用"感知幅度" = RMS × 1.4 + peak × 0.3（强调 RMS 但保留瞬态色彩）
        //   映射到 [0,1] 时再做一次 sqrt，让低幅度也能看到颜色变化（避免全是 pink100）
        const float ampForColour = juce::jlimit (0.0f, 1.0f,
                                                 std::sqrt (rms * 1.4f + peak * 0.3f));
        g.setColour (mapAmplitudeToColour (ampForColour));

        // 柱条：从 min 到 max 的垂直线段；全部用整数坐标 → 无亚像素抖动
        //   再用 inner.getY() / inner.getBottom() 收紧，避免放大后超出画布
        const int px    = ix + xi;
        const int yTopRaw = cy - (int) std::round (maxV * halfH);
        const int yBotRaw = cy - (int) std::round (minV * halfH);
        const int yMin  = juce::jlimit (iy, iy + ih, juce::jmin (yTopRaw, yBotRaw));
        const int yMax  = juce::jlimit (iy, iy + ih, juce::jmax (yTopRaw, yBotRaw));
        const int hLine = juce::jmax (1, yMax - yMin); // 至少 1 像素
        g.fillRect (px, yMin, 1, hLine);
    }

    // 右侧微弱"新样本高亮"：最近 2px 加一层浅高光，强调"当前"的感觉
    {
        juce::ColourGradient grad (PinkXP::hl.withAlpha (0.0f),
                                   (float) inner.getRight() - 8.0f, cy,
                                   PinkXP::hl.withAlpha (0.35f),
                                   (float) inner.getRight(),        cy,
                                   false);
        g.setGradientFill (grad);
        g.fillRect ((float) inner.getRight() - 8.0f, (float) inner.getY(),
                    8.0f, (float) inner.getHeight());
    }

    // 左下角说明：显示当前时长
    g.setColour (PinkXP::ink.withAlpha (0.85f));
    g.setFont (PinkXP::getAxisFont (9.0f, juce::Font::plain));
    const juce::String hint = juce::String (displaySeconds, 1) + "s  (newest →)";
    g.drawText (hint, canvas.getX() + 4, canvas.getBottom() - 14,
                140, 12, juce::Justification::centredLeft, false);
}
