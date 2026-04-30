#include "source/ui/modules/EqModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"
#include <cmath>

// ==========================================================
// EqModule::PixelEqGraph —— 像素拼拼豆豆风 EQ 网格
// Y2K + Win95 像素装饰：四角 L 型像素、CRT 扫描线、像素星、像素标题条
// ==========================================================

EqModule::PixelEqGraph::PixelEqGraph(Y2KmeterAudioProcessor& p)
    : processor(p)
{
    // Phase F：本子组件依赖 Spectrum；时发粗粒度 specData[160]。
    //   注意：父模块 EqModule 已经 retain(Spectrum) 过一次，这里不再重复 retain。
    //   PixelEqGraph 生命周期与 EqModule 完全对齐（PixelEqGraph 是 EqModule 的成员）。
    processor.getAnalyserHub().addFrameListener(this);
}

EqModule::PixelEqGraph::~PixelEqGraph()
{
    processor.getAnalyserHub().removeFrameListener(this);
}

void EqModule::PixelEqGraph::visibilityChanged()
{
    // Phase F：Hub 分发器统一由 Editor 管理，不再起 Timer
}

void EqModule::PixelEqGraph::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Spectrum)) return;

    // Phase F 优化：std::array → juce::Array 改 memcpy 批量拷贝
    const int count = (int) frame.spectrumData.size();
    spectrumSnapshot.resize(count);
    std::memcpy (spectrumSnapshot.getRawDataPointer(), frame.spectrumData.data(),
                 (size_t) count * sizeof (float));

    if (smoothedSpectrum.size() != count)
    {
        smoothedSpectrum.resize(count);
        peakSpectrum.resize(count);
        for (int i = 0; i < count; ++i)
        {
            smoothedSpectrum.set(i, spectrumSnapshot.getUnchecked(i));
            peakSpectrum.set(i, spectrumSnapshot.getUnchecked(i));
        }
    }
    else
    {
        // Peak 线每帧向下衰减的像素速率（单位：归一化电平 / 帧）。
        //   timer = 30Hz；这里选 0.008 ≈ 每秒下落 24% 电平，和 Spectrum 的 peak
        //   缓降节奏一致，足够让"历史最高"逐步回落、但又不会快到看不清。
        constexpr float kPeakFallPerFrame = 0.008f;

        for (int i = 0; i < count; ++i)
        {
            const float raw = spectrumSnapshot.getUnchecked(i);
            const float prev = smoothedSpectrum.getUnchecked(i);
            const float alpha = (raw > prev) ? 0.35f : 0.08f;
            const float smoothed = prev + alpha * (raw - prev);
            smoothedSpectrum.set(i, smoothed);

            // Peak 线：上升瞬时跟随、下降匀速衰减（≥ 当前 smoothed 时才保留）
            const float prevPeak = peakSpectrum.getUnchecked(i);
            const float decayed  = juce::jmax(smoothed, prevPeak - kPeakFallPerFrame);
            peakSpectrum.set(i, juce::jlimit(0.0f, 1.0f,
                                             (smoothed > prevPeak) ? smoothed : decayed));
        }
    }

    // 性能优化（阶段1）：UI 侧 repaint 节流。
    //   Hub 以 60~100Hz 回调 onFrame，但像素 EQ 图肉眼 ~30Hz 已足够，
    //   高 DPI 屏幕上每帧全量重绘像素网格开销较大。
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float  scale  = (float) juce::jmax (1.0, (double) juce::Component::getApproximateScaleFactorForComponent (this));
    const double minRepaintIntervalMs = 20.0 * (double) juce::jmin (1.8f, scale);
    if ((nowMs - lastRepaintMs) < minRepaintIntervalMs)
        return;

    lastRepaintMs = nowMs;
    repaint();
}

void EqModule::PixelEqGraph::setActiveBand(Band newBand)
{
    if (activeBand == newBand) return;
    activeBand = newBand;
    repaint();
}

void EqModule::PixelEqGraph::setCellSize(int newSize)
{
    newSize = juce::jlimit(4, 24, newSize);
    if (cellSize == newSize) return;
    cellSize = newSize;
    repaint();
}

void EqModule::PixelEqGraph::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);

    PinkXP::drawSunken(g, bounds, PinkXP::content);

    const int padding = 6;
    const auto innerFull = bounds.reduced(padding);
    if (innerFull.getWidth() <= 0 || innerFull.getHeight() <= 0) return;

    const int scaleMarginLeft = 36;
    const auto inner = innerFull.withTrimmedLeft(scaleMarginLeft);
    if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

    const int cs = cellSize;
    const int gap = juce::jmax(1, cs / 8);
    const int step = cs + gap;

    const int numCols = juce::jmax(1, (inner.getWidth()  + gap) / step);
    const int numRows = juce::jmax(1, (inner.getHeight() + gap) / step);

    const int totalGridW = numCols * step - gap;
    const int totalGridH = numRows * step - gap;
    const int offsetX = inner.getX() + (inner.getWidth()  - totalGridW) / 2;
    const int offsetY = inner.getY() + (inner.getHeight() - totalGridH) / 2;

    const auto getBandRange = [this]()
    {
        switch (activeBand)
        {
            case Band::low:  return std::pair<float,float>{ 0.0f,          1.0f / 3.0f };
            case Band::mid:  return std::pair<float,float>{ 1.0f / 3.0f,   2.0f / 3.0f };
            case Band::high: return std::pair<float,float>{ 2.0f / 3.0f,   1.0f };
            default:         return std::pair<float,float>{ 0.0f,          1.0f };
        }
    };

    auto bandRange = getBandRange();
    float bandStart = bandRange.first;
    float bandEnd   = bandRange.second;

    {
        const double sr = processor.getCurrentSampleRate();
        const double nyquist = sr * 0.5;
        const float normLow  = (float) std::pow(20.0    / nyquist, 1.0 / 2.3);
        const float normHigh = (float) std::pow(20000.0 / nyquist, 1.0 / 2.3);
        bandStart = juce::jmax(bandStart, normLow);
        bandEnd   = juce::jmin(bandEnd,   normHigh);
    }
    if (bandEnd <= bandStart) return;

    juce::Colour litColour = PinkXP::pink400;
    if (activeBand == Band::low)  litColour = PinkXP::pink200;
    if (activeBand == Band::mid)  litColour = PinkXP::pink400;
    if (activeBand == Band::high) litColour = PinkXP::pink600;

    const juce::Colour dimColour = PinkXP::pink50;
    const int specCount = smoothedSpectrum.size();

    juce::Array<int> litCounts;
    litCounts.ensureStorageAllocated(numCols);

    for (int col = 0; col < numCols; ++col)
    {
        float level = 0.0f;
        if (specCount > 0)
        {
            const float colNorm = (float) col / (float) juce::jmax(1, numCols - 1);
            const float specPos = juce::jmap(colNorm, bandStart, bandEnd) * (float) (specCount - 1);
            const int idx = juce::jlimit(0, specCount - 1, (int) std::round(specPos));
            level = juce::jlimit(0.0f, 1.0f, smoothedSpectrum.getUnchecked(idx));
        }

        const int litCount = (int) std::round(level * (float) numRows);
        litCounts.add(litCount);

        for (int row = 0; row < numRows; ++row)
        {
            const int x = offsetX + col * step;
            const int y = offsetY + row * step;
            const bool lit = (numRows - 1 - row) < litCount;

            if (lit)
            {
                const float rowNorm = 1.0f - (float) row / (float) juce::jmax(1, numRows - 1);
                const auto ringColour = litColour.withAlpha(0.55f + 0.45f * rowNorm);

                const float cx = (float) x + (float) cs * 0.5f;
                const float cy = (float) y + (float) cs * 0.5f;
                const float outerRadius = (float) step * 0.5f;
                const float innerRadius = (float) cs * 0.2f;

                juce::Path ring;
                ring.addEllipse(cx - outerRadius, cy - outerRadius, outerRadius * 2.0f, outerRadius * 2.0f);
                ring.addEllipse(cx - innerRadius, cy - innerRadius, innerRadius * 2.0f, innerRadius * 2.0f);
                ring.setUsingNonZeroWinding(false);

                g.setColour(ringColour);
                g.fillPath(ring);
            }
            else
            {
                g.setColour(dimColour);
                g.fillRect(x, y, cs, cs);
            }
        }
    }

    auto calcDividerY = [&](int lc) -> float
    {
        if (lc <= 0)       return (float)(offsetY + numRows * step) - (float) gap * 0.5f;
        if (lc >= numRows) return (float) offsetY - (float) gap * 0.5f;
        return (float)(offsetY + (numRows - lc) * step) - (float) gap * 0.5f;
    };

    auto buildStepPath = [&](const juce::Array<int>& counts) -> juce::Path
    {
        juce::Path path;
        bool started = false;

        for (int col = 0; col < numCols; ++col)
        {
            const int lc = counts.getUnchecked(col);
            const float dy = calcDividerY(lc);

            const float colLeft  = (float)(offsetX + col * step) - (float) gap * 0.5f;
            const float colRight = (col < numCols - 1)
                ? (float)(offsetX + (col + 1) * step) - (float) gap * 0.5f
                : (float)(offsetX + col * step + cs);
            const float effectiveLeft = (col == 0) ? (float) offsetX : colLeft;

            if (!started)
            {
                path.startNewSubPath(effectiveLeft, dy);
                path.lineTo(colRight, dy);
                started = true;
            }
            else
            {
                const float prevY = path.getCurrentPosition().y;
                if (std::abs(dy - prevY) > 0.5f)
                {
                    path.lineTo(effectiveLeft, prevY);
                    path.lineTo(effectiveLeft, dy);
                }
                path.lineTo(colRight, dy);
            }
        }
        return path;
    };

    juce::Array<int> peakLitCounts;
    peakLitCounts.ensureStorageAllocated(numCols);
    const int peakSpecCount = peakSpectrum.size();
    for (int col = 0; col < numCols; ++col)
    {
        float peakLevel = 0.0f;
        if (peakSpecCount > 0)
        {
            const float colNorm = (float) col / (float) juce::jmax(1, numCols - 1);
            const float specPos = juce::jmap(colNorm, bandStart, bandEnd) * (float) (peakSpecCount - 1);
            const int idx = juce::jlimit(0, peakSpecCount - 1, (int) std::round(specPos));
            peakLevel = juce::jlimit(0.0f, 1.0f, peakSpectrum.getUnchecked(idx));
        }
        peakLitCounts.add((int) std::round(peakLevel * (float) numRows));
    }

    if (numCols >= 2)
    {
        const juce::Colour peakColour = PinkXP::pink300;
        const float lineThickness = 2.0f;
        juce::Path peakPath = buildStepPath(peakLitCounts);

        g.setColour(peakColour.withAlpha(0.25f));
        g.strokePath(peakPath, juce::PathStrokeType(lineThickness + 2.0f,
            juce::PathStrokeType::beveled, juce::PathStrokeType::square));
        g.setColour(peakColour.withAlpha(0.85f));
        g.strokePath(peakPath, juce::PathStrokeType(lineThickness,
            juce::PathStrokeType::beveled, juce::PathStrokeType::square));
    }

    if (numCols >= 2)
    {
        const juce::Colour dividerColour = PinkXP::pink600.withAlpha(0.95f);
        const float lineThickness = 2.0f;
        juce::Path dividerPath = buildStepPath(litCounts);

        g.setColour(dividerColour.withAlpha(0.3f));
        g.strokePath(dividerPath, juce::PathStrokeType(lineThickness + 3.0f,
            juce::PathStrokeType::beveled, juce::PathStrokeType::square));
        g.setColour(dividerColour);
        g.strokePath(dividerPath, juce::PathStrokeType(lineThickness,
            juce::PathStrokeType::beveled, juce::PathStrokeType::square));
    }

    // dB 刻度
    {
        const float dbMin = -50.0f;
        const float dbMax =  50.0f;
        const float dbRange = dbMax - dbMin;

        const float gridTop    = (float) offsetY;
        const float gridBottom = (float)(offsetY + totalGridH);
        const float gridHeight = gridBottom - gridTop;

        const float scaleRight = (float)(offsetX - 4);
        const float scaleLeft  = (float)(innerFull.getX() + 2);

        g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::plain));

        for (int db = (int) dbMin; db <= (int) dbMax; db += 10)
        {
            const float level = ((float) db - dbMin) / dbRange;
            const float y = gridBottom - level * gridHeight;

            if (db == 0)
            {
                g.setColour(PinkXP::pink300.withAlpha(0.55f));
                g.drawHorizontalLine((int) std::round(y), (float) offsetX, (float)(offsetX + totalGridW));
            }

            const float tickLen = (db % 20 == 0) ? 6.0f : 3.0f;
            g.setColour(PinkXP::pink600.withAlpha(0.7f));
            g.drawHorizontalLine((int) std::round(y), scaleRight - tickLen, scaleRight);

            if (db % 20 == 0)
            {
                juce::String label = (db > 0) ? ("+" + juce::String(db)) : juce::String(db);
                g.setColour(PinkXP::ink.withAlpha(0.85f));
                const int textH = 10;
                g.drawText(label, (int) scaleLeft, (int) std::round(y) - textH / 2,
                           (int)(scaleRight - tickLen - 1 - scaleLeft), textH,
                           juce::Justification::centredRight, false);
            }
        }
    }
}

// ==========================================================
// EqModule::BandSelector
// ==========================================================
EqModule::BandSelector::BandSelector()
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void EqModule::BandSelector::setSelectedIndex(int newIndex, bool notify)
{
    newIndex = juce::jlimit(0, 2, newIndex);
    if (selectedIndex == newIndex) return;
    selectedIndex = newIndex;
    repaint();
    if (notify && onSelectionChanged) onSelectionChanged(selectedIndex);
}

int EqModule::BandSelector::getIndexForX(float x) const
{
    const float segW = (float) getWidth() / 3.0f;
    int idx = (int)(x / segW);
    return juce::jlimit(0, 2, idx);
}

void EqModule::BandSelector::paint(juce::Graphics& g)
{
    const int w = getWidth();
    const int h = getHeight();
    const int segW = w / 3;

    for (int i = 0; i < 3; ++i)
    {
        const int x = i * segW;
        const int segWidth = (i == 2) ? (w - x) : segW;
        juce::Rectangle<int> btnRect(x, 0, segWidth, h);
        const bool isSelected = (i == selectedIndex);

        if (isSelected)
        {
            PinkXP::drawPressed(g, btnRect, PinkXP::btnFace);
            g.setColour(PinkXP::pink100);
            for (int py = btnRect.getY() + 3; py < btnRect.getBottom() - 2; py += 4)
                for (int px = btnRect.getX() + 3 + ((py / 4) % 2) * 2; px < btnRect.getRight() - 2; px += 4)
                    g.fillRect(px, py, 2, 2);
            g.setColour(PinkXP::ink);
            g.setFont(PinkXP::getFont(12.0f, juce::Font::bold));
        }
        else
        {
            PinkXP::drawRaised(g, btnRect, PinkXP::btnFace);
            g.setColour(PinkXP::ink);
            g.setFont(PinkXP::getFont(12.0f, juce::Font::plain));
        }
        g.drawText(labels[i], btnRect, juce::Justification::centred, false);
    }
}

void EqModule::BandSelector::mouseDown(const juce::MouseEvent& e)
{
    isDragging = true;
    setSelectedIndex(getIndexForX(e.position.x));
}

void EqModule::BandSelector::mouseDrag(const juce::MouseEvent& e)
{
    if (!isDragging) return;
    setSelectedIndex(getIndexForX(juce::jlimit(0.0f, (float) getWidth(), e.position.x)));
}

void EqModule::BandSelector::mouseUp(const juce::MouseEvent&)
{
    isDragging = false;
}

// ==========================================================
// EqModule 主体
// ==========================================================
EqModule::EqModule(Y2KmeterAudioProcessor& p)
    : ModulePanel(ModuleType::eq),
      processor(p),
      eqGraph(p)
{
    // Phase F：本模块只需要 Spectrum 一路
    processor.getAnalyserHub().retain(AnalyserHub::Kind::Spectrum);

    setMinSize(64, 64);
    setDefaultSize(384, 256); // eq 6×4 大格

    bandSelector.onSelectionChanged = [this](int idx)
    {
        PixelEqGraph::Band b = PixelEqGraph::Band::mid;
        if (idx == 0) b = PixelEqGraph::Band::low;
        if (idx == 2) b = PixelEqGraph::Band::high;
        eqGraph.setActiveBand(b);
    };

    cellSizeSlider.setSliderStyle(juce::Slider::LinearVertical);
    cellSizeSlider.setRange(4.0, 24.0, 1.0);
    cellSizeSlider.setValue(10.0, juce::dontSendNotification);
    // TextBox 只读：第 2 个参数 isReadOnly=true 让数字仅用于展示，
    //   不再接受点击编辑（需求：拿掉"点数字改值"的交互入口）
    cellSizeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 40, 16);
    // 数字文字颜色跟随主题 ink（浅色主题下是深色 → 可读；
    //   不再依赖 LookAndFeel 默认的白色 textBoxTextColourId）
    cellSizeSlider.setColour (juce::Slider::textBoxTextColourId,       PinkXP::ink);
    cellSizeSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    cellSizeSlider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    cellSizeSlider.onValueChange = [this]()
    {
        eqGraph.setCellSize((int) cellSizeSlider.getValue());
    };

    cellSizeLabel.setJustificationType(juce::Justification::centred);
    cellSizeLabel.setColour(juce::Label::textColourId, PinkXP::ink);
    cellSizeLabel.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    cellSizeLabel.setText("SIZE", juce::dontSendNotification);

    // 主题切换时重新下发 "SIZE" 标签的 textColourId —— 与标题栏墨色保持一致。
    //   JUCE Label 的 textColour 是在调用瞬间缓存到 Component 内部的 Colour 字段，
    //   切主题后 PinkXP::ink 已变，但旧值仍挂在 Label 上；必须显式刷新一次。
    //   Slider 的 textBoxTextColourId 同样需要显式刷新（理由同上）。
    themeSubToken = PinkXP::subscribeThemeChanged ([this]()
    {
        cellSizeLabel.setColour (juce::Label::textColourId, PinkXP::ink);
        cellSizeSlider.setColour (juce::Slider::textBoxTextColourId, PinkXP::ink);
        cellSizeLabel.repaint();
        cellSizeSlider.repaint();
    });

    addAndMakeVisible(bandSelector);
    addAndMakeVisible(eqGraph);
    addAndMakeVisible(cellSizeSlider);
    addAndMakeVisible(cellSizeLabel);

    bandSelector.setSelectedIndex(1, false);
    eqGraph.setActiveBand(PixelEqGraph::Band::mid);
}

EqModule::~EqModule()
{
    // 解绑主题订阅，避免析构后仍然回调到已销毁的 this
    if (themeSubToken >= 0)
    {
        PinkXP::unsubscribeThemeChanged (themeSubToken);
        themeSubToken = -1;
    }

    // Phase F：释放 Spectrum 引用
    processor.getAnalyserHub().release(AnalyserHub::Kind::Spectrum);
}

void EqModule::paintContent(juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    // 内容区自身不画背景（由 eqGraph 的凹陷画布+内部 UI 组件覆盖全区），
    // 但若内容区比 eqGraph 宽（顶部工具栏等区域）需要底色，用 btnFace 填充保持和谐。
    g.setColour(PinkXP::btnFace);
    g.fillRect(contentBounds);
}

void EqModule::layoutContent(juce::Rectangle<int> contentBounds)
{
    auto area = contentBounds.reduced(6);

    // 顶部工具栏（band 选择器整行）
    auto top = area.removeFromTop(26);
    bandSelector.setBounds(top);

    area.removeFromTop(6);

    // 右侧 SIZE 滑条
    const int sliderWidth = 42;
    auto rightPanel = area.removeFromRight(sliderWidth);
    area.removeFromRight(6);

    auto labelArea = rightPanel.removeFromTop(14);
    cellSizeLabel.setBounds(labelArea);
    cellSizeSlider.setBounds(rightPanel);

    eqGraph.setBounds(area);
}
