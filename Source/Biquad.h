#pragma once

#include <cmath>

// Enkel biquad (RBJ cookbook), én kanal-tilstand. Koeffisienter deles
// ikke mellom stemmer slik at hver stemme kan ha eget filter.
struct Biquad
{
    enum class Type { lowpass, highpass };

    void setCoefficients (Type type, double sampleRate, double freq, double q) noexcept
    {
        freq = std::fmin (std::fmax (freq, 20.0), sampleRate * 0.45);
        q = std::fmax (q, 0.1);

        const double w0 = 6.283185307179586 * freq / sampleRate;
        const double cosw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);

        double b0, b1, b2;

        if (type == Type::lowpass)
        {
            b0 = (1.0 - cosw) * 0.5;
            b1 = 1.0 - cosw;
            b2 = b0;
        }
        else
        {
            b0 = (1.0 + cosw) * 0.5;
            b1 = -(1.0 + cosw);
            b2 = b0;
        }

        const double a0 = 1.0 + alpha;
        c0 = (float) (b0 / a0);
        c1 = (float) (b1 / a0);
        c2 = (float) (b2 / a0);
        c3 = (float) (-2.0 * cosw / a0);
        c4 = (float) ((1.0 - alpha) / a0);
    }

    void reset() noexcept { z1 = z2 = 0.0f; }

    // Transposed direct form II
    float process (float x) noexcept
    {
        const float y = c0 * x + z1;
        z1 = c1 * x - c3 * y + z2;
        z2 = c2 * x - c4 * y;
        return y;
    }

    float c0 = 1.0f, c1 = 0.0f, c2 = 0.0f, c3 = 0.0f, c4 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};
