#include "source/ui/toys/ToyKit.h"

// ==========================================================
// ToyFx_Oscilloscope —— 拍电视（§8.3，GL 代表模块，纯 L1）
//   一次白闪（alpha ≤ 0.35、≤120ms）、滚动暗带、水平撕裂线、
//   雪花点、左下角 "TRACKING" 闪烁约 1.2 秒。
//   只在 JUCE 组件层叠加，不碰 GL。
// ==========================================================

namespace
{
    struct TvStaticActor : public ToyActor
    {
        juce::Point<float>     at;
        juce::Rectangle<int>   canvas;
        juce::String           readout;   // 真实读数（E.4.3）

        TvStaticActor (juce::Point<float> p, juce::Rectangle<int> c, const juce::String& r)
            : at (p), canvas (c), readout (r) { ttl = 1.2f; }

        void step (float dt, ToyDirector&) override
        {
            age += dt;
            if (age >= ttl) alive = false;
        }

        void paint (juce::Graphics& g, ToyDirector&) override
        {
            const float t = age / ttl;
            juce::Random rng ((juce::int64) (juce::Time::getMillisecondCounter() / 40));

            // 白闪（前 120ms）
            if (age < 0.12f)
            {
                const float f = 1.0f - age / 0.12f;
                g.setColour (ToyPalette::glow().withAlpha (0.35f * f));
                g.fillRect (canvas);
            }

            // 滚动暗带（自上而下）
            const float bandY = canvas.getY() + std::fmod ((float) age * 180.0f, (float) canvas.getHeight());
            g.setColour (ToyPalette::shadow().withAlpha (0.28f * (1.0f - t)));
            g.fillRect (canvas.getX(), (int) bandY, canvas.getWidth(), 26);

            // 水平撕裂线
            g.setColour (ToyPalette::accent().withAlpha (0.5f * (1.0f - t)));
            for (int i = 0; i < 4; ++i)
            {
                const int y = canvas.getY() + rng.nextInt (canvas.getHeight());
                g.fillRect (canvas.getX() + rng.nextInt (canvas.getWidth() / 2),
                            y, canvas.getWidth() / 2, 1);
            }

            // 雪花点
            g.setColour (ToyPalette::glow().withAlpha (0.6f * (1.0f - t)));
            const int n = 60;
            for (int i = 0; i < n; ++i)
            {
                const int x = canvas.getX() + rng.nextInt (canvas.getWidth());
                const int y = canvas.getY() + rng.nextInt (canvas.getHeight());
                g.fillRect (x, y, 1, 1);
            }

            // 左下角真实读数（取代 "TRACKING"，E.4.3）
            const float blink = (std::fmod ((float) age, 0.4f) < 0.2f) ? 1.0f : 0.2f;
            g.setFont (PinkXP::getFont (11.0f, juce::Font::bold));
            g.setColour (ToyPalette::ok().withAlpha (0.9f * blink));
            g.drawText (readout,
                        juce::Rectangle<int> (canvas.getX() + 4, canvas.getBottom() - 18,
                                              juce::jmin (220, canvas.getWidth() - 8), 16),
                        juce::Justification::centredLeft, false);
        }

        juce::Rectangle<float> getBounds() const override
        {
            return canvas.toFloat();
        }
    };
}

std::unique_ptr<ToyActor> makeTvStaticActor (juce::Point<float> at, juce::Rectangle<int> canvas,
                                             const juce::String& readout)
{
    return std::make_unique<TvStaticActor> (at, canvas, readout);
}
