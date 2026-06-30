// Offline DSP smoke-test for SpectralEngine. No host, no GUI.
// Checks: stability (no NaN/Inf), silence -> silence, a fed sine produces
// bounded, non-trivial output, and that the held tone sustains after input stops.
#include "SpectralEngine.h"
#include <cstdio>
#include <cmath>

#ifndef M_PI
constexpr double M_PI = 3.14159265358979323846; // MSVC doesn't define it
#endif

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

    // 5) brush edit: boosting a held band raises its level, cutting lowers it
    {
        eng.setOrder (12);
        eng.reset();
        double ph = 0.0, w = 2.0 * M_PI * 1000.0 / sr;
        std::vector<float> buf (block);
        // deposit a 1 kHz tone, then stop input and let it hold
        for (int b = 0; b < (int) (0.4 * sr / block); ++b)
        {
            for (int i = 0; i < block; ++i) { buf[i] = 0.5f * (float) std::sin (ph); ph += w; }
            eng.process (buf.data(), buf.data(), block, p);
        }
        auto holdRms = [&]
        {
            // flush past the STFT latency (fftSize = 4096 = 16 blocks) so the output
            // reflects the *current* held state and any drained edits, then measure.
            for (int b = 0; b < 24; ++b) { std::fill (buf.begin(), buf.end(), 0.0f); eng.process (buf.data(), buf.data(), block, p); }
            return rms (buf.data(), block);
        };
        float before = holdRms();
        for (int i = 0; i < 60; ++i)  eng.queueBrush (1000.0f, +1.0f, 0.6f); // boost @ 1 kHz
        float afterUp = holdRms();
        for (int i = 0; i < 120; ++i) eng.queueBrush (1000.0f, -1.0f, 0.6f); // cut @ 1 kHz
        float afterDn = holdRms();
        bool ok = std::isfinite (afterUp) && std::isfinite (afterDn)
                  && afterUp > before * 1.3f && afterDn < afterUp * 0.5f;
        printf ("[%s] brush edit: before=%.4f up=%.4f down=%.4f\n",
                ok ? "PASS" : "FAIL", before, afterUp, afterDn);
        fails += ok ? 0 : 1;
    }

    // 6) compress: +expand widens the loud/quiet ratio, -homogenise narrows it
    {
        const int B500 = (int) std::lround (500.0  * 4096.0 / sr);  // ~bin 43
        const int B2k  = (int) std::lround (2000.0 * 4096.0 / sr);  // ~bin 171
        std::vector<float> m, ph, buf2 (block);

        auto deposit = [&]
        {
            eng.setOrder (12); eng.reset();
            double a = 0.0, b = 0.0, wa = 2.0 * M_PI * 500.0 / sr, wb = 2.0 * M_PI * 2000.0 / sr;
            for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
            {
                for (int i = 0; i < block; ++i) { buf2[i] = 0.5f * (float) std::sin (a) + 0.1f * (float) std::sin (b); a += wa; b += wb; }
                eng.process (buf2.data(), buf2.data(), block, p);
            }
        };
        auto ratioAfter = [&] (float compress, int blocks)
        {
            SpectralEngine::Params h; h.feed = 0.0f; h.loss = 0.0f; h.compress = compress;
            for (int blk = 0; blk < blocks; ++blk) { std::fill (buf2.begin(), buf2.end(), 0.0f); eng.process (buf2.data(), buf2.data(), block, h); }
            int n = 0; for (int t = 0; t < 8 && n == 0; ++t) { std::fill (buf2.begin(), buf2.end(), 0.0f); eng.process (buf2.data(), buf2.data(), block, h); n = eng.copyDisplay (m, ph); }
            return m[(size_t) B500] / juce::jmax (1.0e-9f, m[(size_t) B2k]);
        };

        deposit(); float base = ratioAfter (0.0f, 8);
        deposit(); float expanded = ratioAfter (+0.9f, 120);
        deposit(); float homogen  = ratioAfter (-0.9f, 120);
        bool ok = std::isfinite (expanded) && std::isfinite (homogen)
                  && expanded > base * 1.3f && homogen < base * 0.8f;
        printf ("[%s] compress: base=%.2f expand=%.2f homogenise=%.2f\n",
                ok ? "PASS" : "FAIL", base, expanded, homogen);
        fails += ok ? 0 : 1;
    }

    // 7) feed=0 freezes: output must be independent of the input (no phase leak)
    {
        auto run = [&] (bool driveInput, std::vector<float>& outTail)
        {
            SpectralEngine e2; e2.prepare (sr, 13); e2.setOrder (12); e2.reset();
            std::vector<float> b (block);
            double a = 0.0, w = 2.0 * M_PI * 500.0 / sr;
            // identical deposit at 500 Hz with feed
            for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
            {
                for (int i = 0; i < block; ++i) { b[i] = 0.5f * (float) std::sin (a); a += w; }
                e2.process (b.data(), b.data(), block, p);
            }
            // freeze (feed=0); optionally drive a loud, unrelated 2 kHz input
            SpectralEngine::Params f; f.feed = 0.0f; f.loss = 0.0f;
            double a2 = 0.0, w2 = 2.0 * M_PI * 2000.0 / sr;
            outTail.assign (block, 0.0f);
            for (int blk = 0; blk < 60; ++blk)
            {
                for (int i = 0; i < block; ++i) b[i] = driveInput ? 0.8f * (float) std::sin (a2) : 0.0f, a2 += w2;
                e2.process (b.data(), b.data(), block, f);
            }
            outTail.assign (b.begin(), b.end());
        };
        std::vector<float> silentDriven, inputDriven;
        run (false, silentDriven);
        run (true,  inputDriven);
        float maxDiff = 0.0f;
        for (int i = 0; i < block; ++i) maxDiff = juce::jmax (maxDiff, std::abs (silentDriven[i] - inputDriven[i]));
        bool ok = maxDiff < 1.0e-4f;
        printf ("[%s] feed=0 ignores input: maxDiff=%.2e\n", ok ? "PASS" : "FAIL", maxDiff);
        fails += ok ? 0 : 1;
    }

    // 8) phase noise: when on, it perturbs the frozen output but stays finite & bounded
    {
        auto runNoise = [&] (bool noise, std::vector<float>& tail)
        {
            SpectralEngine e2; e2.prepare (sr, 13); e2.setOrder (12); e2.reset();
            std::vector<float> b (block);
            double a = 0.0, w = 2.0 * M_PI * 500.0 / sr;
            for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
            {
                for (int i = 0; i < block; ++i) { b[i] = 0.5f * (float) std::sin (a); a += w; }
                e2.process (b.data(), b.data(), block, p);
            }
            SpectralEngine::Params f; f.feed = 0.0f; f.loss = 0.0f; f.phaseNoise = noise;
            float mx = 0.0f;
            for (int blk = 0; blk < 60; ++blk)
            {
                std::fill (b.begin(), b.end(), 0.0f);
                e2.process (b.data(), b.data(), block, f);
                for (int i = 0; i < block; ++i) mx = juce::jmax (mx, std::abs (b[i]));
            }
            tail.assign (b.begin(), b.end());
            return mx;
        };
        std::vector<float> off, on;
        runNoise (false, off);
        float mxOn = runNoise (true, on);
        float diff = 0.0f;
        for (int i = 0; i < block; ++i) diff = juce::jmax (diff, std::abs (off[i] - on[i]));
        bool ok = std::isfinite (mxOn) && mxOn < 5.0f && diff > 1.0e-4f; // changed, not exploded
        printf ("[%s] phase noise: changes=%.2e maxAbsOn=%.3f\n", ok ? "PASS" : "FAIL", diff, mxOn);
        fails += ok ? 0 : 1;
    }

    // audio-state save/restore: a held sound survives a serialize -> fresh engine -> restore
    {
        SpectralEngine src; src.prepare (sr, 13); src.setOrder (12); src.reset();
        std::vector<float> b (block);
        double a = 0.0, w = 2.0 * M_PI * 750.0 / sr;
        for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
        {
            for (int i = 0; i < block; ++i) { b[i] = 0.5f * (float) std::sin (a); a += w; }
            src.process (b.data(), b.data(), block, p);
        }
        juce::MemoryBlock mb;
        { juce::MemoryOutputStream os (mb, false); src.writeAudioState (os); }

        SpectralEngine dst; dst.prepare (sr, 13); dst.setOrder (12); dst.reset();
        { juce::MemoryInputStream is (mb, false); dst.readAudioState (is); }

        SpectralEngine::Params f; f.feed = 0.0f; f.loss = 0.0f;
        float r = 0.0f;
        for (int blk = 0; blk < 20; ++blk) { std::fill (b.begin(), b.end(), 0.0f); dst.process (b.data(), b.data(), block, f); r = juce::jmax (r, rms (b.data(), block)); }
        bool ok = std::isfinite (r) && r > 1.0e-3f; // restored engine sustains the held tone
        printf ("[%s] audio-state save/restore: tailRMS=%.4f\n", ok ? "PASS" : "FAIL", r);
        fails += ok ? 0 : 1;
    }

    printf ("\n%s (%d failure%s)\n", fails == 0 ? "ALL PASS" : "FAILURES",
            fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
