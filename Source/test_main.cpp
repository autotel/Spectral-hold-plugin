// Offline DSP smoke-test for SpectralEngine. No host, no GUI.
// Checks: stability (no NaN/Inf), silence -> silence, a fed sine produces
// bounded, non-trivial output, and that the held tone sustains after input stops.
#include "SpectralEngine.h"
#include <cstdio>
#include <cmath>

static bool finiteAll (const float* x, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (x[i])) return false;
    return true;
}

static float rms (const float* x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += (double) x[i] * x[i];
    return (float) std::sqrt (s / juce::jmax (1, n));
}

int main()
{
    const double sr = 48000.0;
    const int block = 256;
    int fails = 0;

    SpectralEngine eng;
    eng.prepare (sr, 13);
    eng.setOrder (12); // 4096

    SpectralEngine::Params p;
    p.feed = 1.0f; p.loss = 0.2f; p.filterAmt = 0.0f; p.filterTone = 1000.0f; p.attack = 0.0f;

    std::vector<float> in (block), out (block);

    // 1) silence -> silence
    {
        std::fill (in.begin(), in.end(), 0.0f);
        bool ok = true;
        for (int b = 0; b < 200; ++b)
        {
            eng.process (in.data(), out.data(), block, p);
            if (! finiteAll (out.data(), block)) { ok = false; break; }
            if (rms (out.data(), block) > 1.0e-5f) { ok = false; break; }
        }
        printf ("[%s] silence -> silence\n", ok ? "PASS" : "FAIL");
        fails += ok ? 0 : 1;
    }

    eng.reset();

    // 2) feed a 1 kHz sine, then cut input; expect bounded output and a sustained tail
    {
        double ph = 0.0, w = 2.0 * M_PI * 1000.0 / sr;
        bool finite = true;
        float maxAbs = 0.0f;
        // feed for ~0.5 s
        for (int b = 0; b < (int) (0.5 * sr / block); ++b)
        {
            for (int i = 0; i < block; ++i) { in[i] = 0.5f * (float) std::sin (ph); ph += w; }
            eng.process (in.data(), out.data(), block, p);
            if (! finiteAll (out.data(), block)) finite = false;
            for (int i = 0; i < block; ++i) maxAbs = juce::jmax (maxAbs, std::abs (out[i]));
        }
        // silence input, measure tail
        std::fill (in.begin(), in.end(), 0.0f);
        float tail = 0.0f;
        for (int b = 0; b < 4; ++b)
        {
            eng.process (in.data(), out.data(), block, p);
            if (! finiteAll (out.data(), block)) finite = false;
            tail = juce::jmax (tail, rms (out.data(), block));
        }
        bool ok = finite && maxAbs > 0.01f && maxAbs < 50.0f && tail > 1.0e-4f;
        printf ("[%s] sine feed: finite=%d maxAbs=%.3f tailRMS=%.5f\n",
                ok ? "PASS" : "FAIL", (int) finite, maxAbs, tail);
        fails += ok ? 0 : 1;
    }

    // 3) order change mid-stream stays finite
    {
        eng.setOrder (10); // 1024
        std::fill (in.begin(), in.end(), 0.1f);
        bool ok = true;
        for (int b = 0; b < 100; ++b)
        {
            eng.process (in.data(), out.data(), block, p);
            if (! finiteAll (out.data(), block)) { ok = false; break; }
        }
        printf ("[%s] order change mid-stream\n", ok ? "PASS" : "FAIL");
        fails += ok ? 0 : 1;
    }

    // 4) in-place processing (in == out, as a host calls it) must still pass audio
    {
        eng.setOrder (12);
        eng.reset();
        double ph = 0.0, w = 2.0 * M_PI * 1000.0 / sr;
        std::vector<float> buf (block);
        float maxAbs = 0.0f;
        bool finite = true;
        for (int b = 0; b < (int) (0.5 * sr / block); ++b)
        {
            for (int i = 0; i < block; ++i) { buf[i] = 0.5f * (float) std::sin (ph); ph += w; }
            eng.process (buf.data(), buf.data(), block, p); // alias in == out
            if (! finiteAll (buf.data(), block)) finite = false;
            for (int i = 0; i < block; ++i) maxAbs = juce::jmax (maxAbs, std::abs (buf[i]));
        }
        bool ok = finite && maxAbs > 0.01f && maxAbs < 50.0f;
        printf ("[%s] in-place (in==out): finite=%d maxAbs=%.3f\n",
                ok ? "PASS" : "FAIL", (int) finite, maxAbs);
        fails += ok ? 0 : 1;
    }

    printf ("\n%s (%d failure%s)\n", fails == 0 ? "ALL PASS" : "FAILURES",
            fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
