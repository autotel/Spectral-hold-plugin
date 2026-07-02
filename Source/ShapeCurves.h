#pragma once
#include <cmath>
#include <algorithm>

// Pure math for the spectral Shaper (see agent-wiki/plan-spectral-shaper.md and
// dsp-design.md). Every shape maps a bin's log-frequency position (and, for the
// Level shape, its level relative to the held spectrum's mean) to a signed change
// L in [-1..+1]. Header-only, no JUCE dependency, so it's shared verbatim by the
// engine (SpectralEngine.cpp), the GUI overlay (SpectrumDisplay.cpp) and tests --
// the display must never reimplement this math (it did once for the old filter
// bell; don't repeat that drift).
namespace ShapeCurves
{
    constexpr int kNumShapes = 4; // Level, Sigmoid, Spikes, Sine (shape param 0..3)

    inline float sign (float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

    // Gaussian window in log-frequency around x0, lerped to flat (1 everywhere) as
    // `count` approaches 1 -- shared shape for the Level and Sine "count" windows.
    inline float gaussianEnvLerp (float x, float x0, float sigmaOct, float count)
    {
        const float d = x - x0;
        const float win = std::exp (-(d * d) / (2.0f * sigmaOct * sigmaOct));
        float t = std::clamp ((count - 0.8f) / 0.2f, 0.0f, 1.0f);
        t = t * t * (3.0f - 2.0f * t); // smoothstep
        return win + (1.0f - win) * t;
    }

    // --- shape 0: Level (replaces Compress) -------------------------------------
    // ratio = |S[k]| / mean of the active bins, or exactly 1 for inactive/empty bins
    // (the caller is responsible for that -- it makes this a no-op there, same as
    // the old compress leaving the noise floor untouched).
    inline float levelShape (float ratio, float width, float level)
    {
        const float lr    = std::log (std::max (ratio, 1.0e-6f));
        const float u      = lr / 4.6f;                          // normalise to ~[-1..1]
        const float gamma  = std::pow (2.0f, (width - 0.5f) * 2.0f); // 0.5 -> 1 (1:1, compress-equivalent)
        return level * sign (u) * std::pow (std::abs (u), gamma);
    }

    inline float levelWindow (float x, float x0, float count)
    {
        const float sigma = 0.3f * std::pow (2.0f, 6.0f * count); // 0.3 .. ~19 octaves
        return gaussianEnvLerp (x, x0, sigma, count);
    }

    // --- shape 1: Sigmoid / tanh shelf (LP <-> HP) -------------------------------
    inline float sigmoidShape (float x, float x0, float width, float level)
    {
        const float wOct = 0.1f * std::pow (2.0f, width * 5.64f); // 0.1 .. 5 octaves
        const float s    = std::tanh ((x - x0) / wOct);
        // level>0: cuts below x0 (highpass); level<0: cuts above x0 (lowpass);
        // continuous through level=0 (flat, per spec).
        return level > 0.0f ? level * (s - 1.0f) * 0.5f
                             : level * (s + 1.0f) * 0.5f;
    }

    // --- shape 2: N-spikes (subtractive band-select) -----------------------------
    // Purely attenuating (L <= 0), sign-split like the Sigmoid: level>0 REJECTS the
    // peaks (notches), level<0 passes ONLY the peaks (cuts everything outside them),
    // flat no-op at level=0. `env` in [0..1] is the spike comb (1 at a peak, 0 between).
    inline float spikesShape (float x, float x0, float width, float count, float level)
    {
        const float d      = 0.2f * std::pow (2.0f, width * 4.6f); // spacing 0.2 .. 5 octaves
        const float sigma  = 0.25f * d;                            // spikes stay distinct
        const float nMaxF  = count * 12.0f;                        // side-pairs at full knob (slow ramp; ~old count=0.3)
        const float nCentF = (x - x0) / d;
        const int   nCent  = (int) std::lround (nCentF);
        float sum = 0.0f;
        for (int n = nCent - 1; n <= nCent + 1; ++n)
        {
            const float an = std::clamp (nMaxF - std::abs ((float) n) + 1.0f, 0.0f, 1.0f);
            if (an <= 0.0f) continue;
            const float dd = x - (x0 + (float) n * d);
            sum += an * std::exp (-(dd * dd) / (2.0f * sigma * sigma));
        }
        const float env = std::clamp (sum, 0.0f, 1.0f);
        return level >= 0.0f ? -level * env            // reject peaks (notch)
                              :  level * (1.0f - env);  // = -|level|*(1-env): pass only peaks
    }

    // --- shape 3: Sine (replaces Filter) -----------------------------------------
    inline float sineShape (float x, float x0, float width, float count, float level)
    {
        constexpr float kTwoPi = 6.283185307f;
        const float rho    = 0.25f * std::pow (2.0f, width * 4.6f);   // 0.25 .. 5.75 cycles/octave
        const float sigmaE = (0.5f / rho) * std::pow (2.0f, 7.0f * count);
        const float env    = gaussianEnvLerp (x, x0, sigmaE, count);
        return level * std::cos (kTwoPi * rho * (x - x0)) * env;
    }

    // Cross-fade between the two adjacent shapes named by `shapeParam` (0..3).
    inline float shapeLRaw (int idx, float x, float x0, float width, float count,
                             float level, float ratio)
    {
        switch (idx)
        {
            case 0:  return levelShape (ratio, width, level) * levelWindow (x, x0, count);
            case 1:  return sigmoidShape (x, x0, width, level);
            case 2:  return spikesShape (x, x0, width, count, level);
            default: return sineShape (x, x0, width, count, level);
        }
    }

    inline float shapeL (float shapeParam, float x, float x0, float width, float count,
                          float level, float ratio)
    {
        shapeParam = std::clamp (shapeParam, 0.0f, (float) (kNumShapes - 1));
        const int   i0 = (int) std::floor (shapeParam);
        const int   i1 = std::min (i0 + 1, kNumShapes - 1);
        const float t  = shapeParam - (float) i0;
        const float l0 = shapeLRaw (i0, x, x0, width, count, level, ratio);
        if (t <= 1.0e-6f || i0 == i1)
            return l0;
        const float l1 = shapeLRaw (i1, x, x0, width, count, level, ratio);
        return l0 + (l1 - l0) * t;
    }
}
