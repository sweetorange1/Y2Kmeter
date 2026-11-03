#include "source/analysis/music/RealFft.h"

#include <algorithm>
#include <cmath>

namespace y2k::music
{

void RealFft::prepare (int fftSize)
{
    int s = 8;
    int ord = 3;

    while (s < fftSize && s < (1 << 18))
    {
        s <<= 1;
        ++ord;
    }

    if (s == n && n > 0)
        return;

    n     = s;
    order = ord;

    bitReverse.resize ((size_t) n);

    for (int i = 0; i < n; ++i)
    {
        int x = i;
        int r = 0;

        for (int b = 0; b < order; ++b)
        {
            r = (r << 1) | (x & 1);
            x >>= 1;
        }

        bitReverse[(size_t) i] = r;
    }

    cosTable.resize ((size_t) (n / 2));
    sinTable.resize ((size_t) (n / 2));

    constexpr double kPi = 3.14159265358979323846;

    for (int k = 0; k < n / 2; ++k)
    {
        // e^{-j2πk/N}
        const double angle = -2.0 * kPi * (double) k / (double) n;
        cosTable[(size_t) k] = (float) std::cos (angle);
        sinTable[(size_t) k] = (float) std::sin (angle);
    }

    re.assign ((size_t) n, 0.0f);
    im.assign ((size_t) n, 0.0f);
}

void RealFft::transform()
{
    for (int i = 0; i < n; ++i)
    {
        const int j = bitReverse[(size_t) i];

        if (j > i)
        {
            std::swap (re[(size_t) i], re[(size_t) j]);
            std::swap (im[(size_t) i], im[(size_t) j]);
        }
    }

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len >> 1;
        const int step = n / len;

        for (int base = 0; base < n; base += len)
        {
            for (int j = 0; j < half; ++j)
            {
                const int k = j * step;
                const float wr = cosTable[(size_t) k];
                const float wi = sinTable[(size_t) k];

                const int a = base + j;
                const int b = a + half;

                const float tr = re[(size_t) b] * wr - im[(size_t) b] * wi;
                const float ti = re[(size_t) b] * wi + im[(size_t) b] * wr;

                re[(size_t) b] = re[(size_t) a] - tr;
                im[(size_t) b] = im[(size_t) a] - ti;
                re[(size_t) a] += tr;
                im[(size_t) a] += ti;
            }
        }
    }
}

void RealFft::magnitude (const float* input, float* outMagnitude)
{
    if (! isPrepared() || input == nullptr || outMagnitude == nullptr)
        return;

    for (int i = 0; i < n; ++i)
    {
        re[(size_t) i] = input[i];
        im[(size_t) i] = 0.0f;
    }

    transform();

    const float scale = 2.0f / (float) n;

    for (int k = 0; k <= n / 2; ++k)
    {
        const float r = re[(size_t) k];
        const float i = im[(size_t) k];

        outMagnitude[k] = scale * std::sqrt (r * r + i * i);
    }
}

} // namespace y2k::music
