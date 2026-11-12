#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"

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

    // 横向像素进度条：sunken 槽 + 5px 格 + 1px 间隙 + 末格心跳高光
    inline void drawReadoutBar (juce::Graphics& g, juce::Rectangle<int> bar, float tNorm,
                                float pulsePhase = 0.0f)
    {
        if (bar.getWidth() <= 4 || bar.getHeight() <= 3) return;
        tNorm = juce::jlimit (0.0f, 1.0f, tNorm);

        PinkXP::drawSunken (g, bar, PinkXP::content.darker (0.04f));
        auto inner = bar.reduced (2);
        if (inner.getWidth() <= 2 || inner.getHeight() <= 1) return;

        const int cellW = 5, gap = 1, step = cellW + gap;
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

            if (i < litCells)
            {
                if (i == litCells - 1)
                {
                    const float pulse = 0.75f + 0.25f * std::sin (pulsePhase);
                    g.setColour (base.brighter (0.15f * pulse).withAlpha (1.0f));
                }
                else
                {
                    g.setColour (base);
                }
            }
            else
            {
                g.setColour (base.withAlpha (0.15f));
            }
            g.fillRect (x, inner.getY(), cellW, inner.getHeight());
        }
    }

    // 带进度条的读数面板（标签 + 呼吸灯 + 20pt 大数字 + 单位 + 进度条）
    //   tNorm < 0 表示不画条；flash ∈ [0,1] 数值刚变化时的闪白；pulsePhase 心跳相位
    inline void drawReadout (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label,
                             float v, const juce::String& unit, float tNorm,
                             float flash = 0.0f, float pulsePhase = 0.0f)
    {
        PinkXP::drawSunken (g, r, PinkXP::content);
        auto inner = r.reduced (6);

        juce::Rectangle<int> barArea;
        if (tNorm >= 0.0f && inner.getHeight() >= 24)
        {
            barArea = inner.removeFromBottom (8);
            inner.removeFromBottom (2);
        }

        auto titleRow = inner.removeFromTop (12);
        g.setColour (PinkXP::ink.withAlpha (0.85f));
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        PinkXP::drawCachedText (g, label, titleRow, juce::Justification::centredLeft,
                                9.0f, juce::Font::bold, PinkXP::ink.withAlpha (0.85f));

        {
            const int sz = 6;
            auto dot = juce::Rectangle<int> (titleRow.getRight() - sz - 2,
                                             titleRow.getY() + (titleRow.getHeight() - sz) / 2, sz, sz);
            if (tNorm > 0.005f)
                g.setColour (PinkXP::pink500.withAlpha (0.35f + 0.65f * juce::jlimit (0.0f, 1.0f, tNorm)));
            else
                g.setColour (PinkXP::pink500.withAlpha (0.08f));
            g.fillRect (dot);
            g.setColour (PinkXP::pink700.withAlpha (0.6f));
            g.drawRect (dot, 1);
        }

        g.setColour (PinkXP::pink600.interpolatedWith (juce::Colour (0xffffffff),
                                                       juce::jlimit (0.0f, 1.0f, flash) * 0.65f));
        g.setFont (PinkXP::getFont (20.0f, juce::Font::bold));
        const juce::String numStr = (v <= -60.0f) ? juce::String ("-144") : juce::String (v, 1);
        g.drawText (y2k::tr (numStr), inner, juce::Justification::centredLeft, false);

        g.setColour (PinkXP::ink.withAlpha (0.7f));
        g.setFont (PinkXP::getFont (10.0f, juce::Font::plain));
        PinkXP::drawCachedText (g, unit, inner, juce::Justification::centredRight,
                                10.0f, juce::Font::plain, PinkXP::ink.withAlpha (0.7f));

        if (! barArea.isEmpty())
            drawReadoutBar (g, barArea, tNorm, pulsePhase);
    }

    // 4px 格 + 1px 间隙的纵向像素柱（原 DynamicsModule::drawSingleBar / DynamicsMeters 的同一份实现）
    //   norm ∈ [0,1] 点亮高度；peakNorm ≤ 0 不画保持线
    inline void drawSegmentBar (juce::Graphics& g, juce::Rectangle<int> area,
                                float norm, float peakNorm, float peakDb, int cell = 4, int gap = 1)
    {
        PinkXP::drawSunken (g, area, PinkXP::content.darker (0.05f));
        auto inner = area.reduced (3);
        if (inner.getWidth() <= 0 || inner.getHeight() <= 0) return;

        const int step = cell + gap;
        const int numRows = juce::jmax (1, (inner.getHeight() + gap) / step);
        const int litRows = (int) std::round (norm * (float) numRows);
        const int totalH  = numRows * step - gap;
        const int offsetY = inner.getY() + (inner.getHeight() - totalH) / 2;
        const int colW    = juce::jmax (1, inner.getWidth() - 4);
        const int offsetX = inner.getX() + (inner.getWidth() - colW) / 2;

        for (int row = 0; row < numRows; ++row)
        {
            const int y = offsetY + row * step;
            const bool lit = (numRows - 1 - row) < litRows;
            const float rowNorm = 1.0f - (float) row / (float) juce::jmax (1, numRows - 1);
            const auto base = dbColour (juce::jmap (rowNorm, -60.0f, 0.0f));
            g.setColour (lit ? base : base.withAlpha (0.15f));
            g.fillRect (offsetX, y, colW, cell);
        }

        const int peakRow = (int) std::round (peakNorm * (float) numRows);
        if (peakRow > 0 && peakRow <= numRows)
        {
            g.setColour (dbColour (peakDb).brighter (0.2f));
            g.fillRect (offsetX - 1, offsetY + (numRows - peakRow) * step, colW + 2, 2);
        }
    }
}
