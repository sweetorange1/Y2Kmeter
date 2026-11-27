#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"

// ==========================================================
// MeterWidgets —— 响度 / 动态模块共享的像素风绘制件（header-only）
//
//   v3.1 参数系统重构前，这些函数各有 2~3 份拷贝：
//     · FineSplitModules.cpp 匿名 namespace（meterColour / lufsColour / drawReadout …）
//     · LoudnessModule::dbToColour / lufsToColour
//     · DynamicsModule::dbToColour / drawSingleBar
//   合并为一份。数值、配色分区、格子尺寸与旧实现逐项一致（旧外观零回归）。
// ==========================================================
namespace y2k::meter
{
    inline float mapDbToNorm (float db, float minDb = -60.0f, float maxDb = 0.0f)
    {
        return juce::jlimit (0.0f, 1.0f, juce::jmap (db, minDb, maxDb, 0.0f, 1.0f));
    }

    // dBFS / dBTP 分区色：> -3 警示，> -9 临界，其余安全
    inline juce::Colour dbColour (float db)
    {
        if (db > -3.0f) return PinkXP::sel;
        if (db > -9.0f) return PinkXP::pink500;
        return PinkXP::pink200;
    }

    // LUFS 分区色：> -9 过响，> -14 适中，其余正常
    inline juce::Colour lufsColour (float lufs)
    {
        if (lufs > -9.0f)  return PinkXP::sel;
        if (lufs > -14.0f) return PinkXP::pink500;
        return PinkXP::pink200;
    }

    // 横向像素进度条：skin 凹槽 + 分段 LED 格 + 末格心跳高光
    inline void drawReadoutBar (juce::Graphics& g, juce::Rectangle<int> bar, float tNorm,
                                float pulsePhase = 0.0f)
    {
        if (bar.getWidth() <= 4 || bar.getHeight() <= 3) return;
        tNorm = juce::jlimit (0.0f, 1.0f, tNorm);

        const auto& tokens = PinkXP::getSkinTokens();
        PinkXP::Craft::drawPanelWell (g, bar, PinkXP::content.darker (0.04f), PinkXP::WellKind::trough);

        auto inner = bar.reduced (2);
        if (inner.getWidth() <= 2 || inner.getHeight() <= 1) return;

        const int cellW = 5;
        const int gap   = (inner.getWidth() >= 36) ? juce::jlimit (1, 2, tokens.defaultCellGapPx) : 1;
        const int step  = cellW + gap;
        const int totalCells = juce::jmax (1, (inner.getWidth() + gap) / step);
        const int litCells   = juce::jlimit (0, totalCells,
                                             (int) std::round (tNorm * (float) totalCells));

        for (int i = 0; i < totalCells; ++i)
        {
            const int x = inner.getX() + i * step;
            const float t = (float) i / (float) juce::jmax (1, totalCells - 1);
            juce::Colour base;
            if (t < 0.7f)       base = PinkXP::pink300.interpolatedWith (PinkXP::pink600, t / 0.7f);
            else if (t < 0.9f)  base = PinkXP::pink500;
            else                base = PinkXP::sel;

            const bool lit = (i < litCells);
            const float pulse = (lit && i == litCells - 1)
                                    ? (0.75f + 0.25f * std::sin (pulsePhase))
                                    : 0.0f;

            PinkXP::Craft::drawMeterCell (g, juce::Rectangle<int> (x, inner.getY(), cellW, inner.getHeight()),
                                          base, lit, pulse);
        }
    }

    // 带进度条的读数面板（标签 + 呼吸灯 + 20pt 大数字 + 单位 + 进度条）
    //   tNorm < 0 表示不画条；flash ∈ [0,1] 数值刚变化时的闪白；pulsePhase 心跳相位
    inline void drawReadout (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label,
                             float v, const juce::String& unit, float tNorm,
                             float flash = 0.0f, float pulsePhase = 0.0f)
    {
        // 绿幕：底板 / 呼吸灯不画；标签 + 数字 + 单位 + 点亮的电平格作为"数值读数"保留
        const auto& tokens   = PinkXP::getSkinTokens();
        const bool chromaKey = tokens.isChromaKey;
        const bool isChrome  = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
        const bool isGlass   = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
        const bool isNeon    = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
        const bool isCrt     = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
        PinkXP::Craft::drawReadoutCard (g, r, PinkXP::content);
        auto inner = r.reduced (6);

        juce::Rectangle<int> barArea;
        if (tNorm >= 0.0f && inner.getHeight() >= 24)
        {
            barArea = inner.removeFromBottom (8);
            inner.removeFromBottom (2);
        }

        auto titleRow = inner.removeFromTop (12);
        const float labelAlpha = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio) ? 0.72f
                               : isGlass                                                 ? 0.78f
                               : isChrome                                                ? 0.80f
                               : (isNeon || isCrt)                                       ? 0.88f
                                                                                         : 0.85f;
        const float unitAlpha  = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio) ? 0.58f
                               : isGlass                                                 ? 0.64f
                               : isChrome                                                ? 0.66f
                                                                                         : 0.70f;
        g.setColour (PinkXP::ink.withAlpha (labelAlpha));
        g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (9.0f, juce::Font::bold), (float) titleRow.getHeight()));
        PinkXP::drawCachedText (g, label, titleRow, juce::Justification::centredLeft,
                                9.0f, juce::Font::bold, PinkXP::ink.withAlpha (labelAlpha));

        if (! chromaKey)
        {
            const int sz = (isChrome || isGlass || isNeon) ? 7 : 6;
            auto dot = juce::Rectangle<int> (titleRow.getRight() - sz - 2,
                                             titleRow.getY() + (titleRow.getHeight() - sz) / 2, sz, sz);
            const bool active = (tNorm > 0.005f);
            const auto ledCol = active
                                    ? PinkXP::pink500.withAlpha (0.35f + 0.65f * juce::jlimit (0.0f, 1.0f, tNorm))
                                    : PinkXP::pink500;
            PinkXP::Craft::drawStatusLed (g, dot, ledCol, active, true);

            if (inner.getHeight() >= 24)
            {
                const bool isDark = PinkXP::content.getPerceivedBrightness() < 0.45f;
                const int ruleY = titleRow.getBottom();
                if (isChrome)
                {
                    // Millennium Chrome：标题行下方刻蚀 2px 金属细沟槽（1px 暗槽 + 1px 镜面高光）
                    g.setColour (PinkXP::dark.withAlpha (isDark ? 0.50f : 0.30f));
                    g.fillRect (inner.getX(), ruleY, inner.getWidth(), 1);
                    g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.55f)
                                           .withAlpha (isDark ? 0.22f : 0.62f));
                    g.fillRect (inner.getX(), ruleY + 1, inner.getWidth(), 1);
                }
                else if (isGlass)
                {
                    g.setColour (isDark ? juce::Colours::white.withAlpha (0.14f)
                                        : juce::Colours::black.withAlpha (0.09f));
                    g.fillRect (inner.getX(), ruleY, inner.getWidth(), 1);
                    g.setColour (juce::Colours::white.withAlpha (isDark ? 0.05f : 0.45f));
                    g.fillRect (inner.getX(), ruleY + 1, inner.getWidth(), 1);
                }
                else if (isNeon)
                {
                    g.setColour (PinkXP::sel.withAlpha (0.18f));
                    g.fillRect (inner.getX(), ruleY - 1, inner.getWidth(), 3);
                    g.setColour (PinkXP::sel.withAlpha (0.58f));
                    g.fillRect (inner.getX(), ruleY, inner.getWidth(), 1);
                }
                else if (isCrt)
                {
                    g.setColour (PinkXP::sel.withAlpha (0.42f));
                    g.fillRect (inner.getX(), ruleY, inner.getWidth(), 1);
                }
            }
        }

        g.setColour (PinkXP::pink600.interpolatedWith (juce::Colour (0xffffffff),
                                                       juce::jlimit (0.0f, 1.0f, flash) * 0.65f));
        g.setFont (PinkXP::fitFontToHeight (PinkXP::getFont (20.0f, juce::Font::bold),
                                            (float) juce::jmax (10, inner.getHeight())));
        const juce::String numStr = (v <= -60.0f) ? juce::String ("-144") : juce::String (v, 1);
        g.drawText (y2k::tr (numStr), inner, juce::Justification::centredLeft, false);

        g.setColour (PinkXP::ink.withAlpha (unitAlpha));
        g.setFont (PinkXP::getFont (10.0f, juce::Font::plain));
        PinkXP::drawCachedText (g, unit, inner, juce::Justification::centredRight,
                                10.0f, juce::Font::plain, PinkXP::ink.withAlpha (unitAlpha));

        if (! barArea.isEmpty())
            drawReadoutBar (g, barArea, tNorm, pulsePhase);

        PinkXP::Craft::drawCanvasFinishOverlay (g, r.reduced (1));
    }

    // 4px 格 + 1px 间隙的纵向像素柱（原 DynamicsModule::drawSingleBar / DynamicsMeters 的同一份实现）
    //   norm ∈ [0,1] 点亮高度；peakNorm ≤ 0 不画保持线
    inline void drawSegmentBar (juce::Graphics& g, juce::Rectangle<int> area,
                                float norm, float peakNorm, float peakDb, int cell = 4, int gap = 1)
    {
        const auto& tokens = PinkXP::getSkinTokens();
        PinkXP::Craft::drawPanelWell (g, area, PinkXP::content.darker (0.05f), PinkXP::WellKind::trough);

        auto inner = area.reduced (3);
        if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

        const int effGap  = (gap == 1 && tokens.defaultCellGapPx > 1 && inner.getHeight() >= 64)
                                ? tokens.defaultCellGapPx
                                : gap;
        const int step    = cell + effGap;
        const int numRows = juce::jmax (1, (inner.getHeight() + effGap) / step);
        const int litRows = (int) std::round (norm * (float) numRows);
        const int totalH  = numRows * step - effGap;
        const int offsetY = inner.getY() + (inner.getHeight() - totalH) / 2;
        const int colW    = juce::jmax (1, inner.getWidth() - 4);
        const int offsetX = inner.getX() + (inner.getWidth() - colW) / 2;

        for (int row = 0; row < numRows; ++row)
        {
            const int y = offsetY + row * step;
            const bool lit = (numRows - 1 - row) < litRows;
            const float rowNorm = 1.0f - (float) row / (float) juce::jmax (1, numRows - 1);
            const auto base = dbColour (juce::jmap (rowNorm, -60.0f, 0.0f));
            PinkXP::Craft::drawMeterCell (g, juce::Rectangle<int> (offsetX, y, colW, cell), base, lit);
        }

        const int peakRow = (int) std::round (peakNorm * (float) numRows);
        if (peakRow > 0 && peakRow <= numRows)
        {
            const auto peakCol = dbColour (peakDb).brighter (0.2f);
            const juce::Rectangle<int> peakRect (offsetX - 1, offsetY + (numRows - peakRow) * step, colW + 2, 2);
            if (tokens.peakCapGlow && ! PinkXP::isGreenScreenBackdrop())
            {
                g.setColour (peakCol.withAlpha (0.28f));
                g.fillRect (peakRect.expanded (1));
            }
            g.setColour (peakCol);
            if (! tokens.hardPixelSteps && tokens.barCellRadius > 0.5f)
            {
                g.fillRoundedRectangle (peakRect.toFloat(), juce::jmin (1.0f, tokens.barCellRadius * 0.5f));
                if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass && ! PinkXP::isGreenScreenBackdrop())
                {
                    g.setColour (juce::Colours::white.withAlpha (0.50f));
                    g.fillRect ((float) peakRect.getX() + 0.5f, (float) peakRect.getY(),
                                juce::jmax (1.0f, (float) peakRect.getWidth() - 1.0f), 1.0f);
                }
                else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
                {
                    g.setColour (peakCol.interpolatedWith (juce::Colours::white, 0.58f));
                    g.fillRect (peakRect.getX() + 1, peakRect.getY(), juce::jmax (1, peakRect.getWidth() - 2), 1);
                }
            }
            else
            {
                g.fillRect (peakRect);
                if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome
                    || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
                {
                    g.setColour (peakCol.interpolatedWith (juce::Colours::white, 0.55f));
                    g.fillRect (peakRect.getX(), peakRect.getY(), peakRect.getWidth(), 1);
                }
            }
        }

        PinkXP::Craft::drawCanvasFinishOverlay (g, area.reduced (2));
    }
}
