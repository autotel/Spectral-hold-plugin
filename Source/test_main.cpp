// Offline DSP smoke-test for SpectralEngine and PlateReverb. No host, no GUI.
// Checks: stability (no NaN/Inf), silence -> silence, a fed sine produces
// bounded, non-trivial output, and that the held tone sustains after input stops.
#include "SpectralEngine.h"
#include "PlateReverb.h"
#include "DryDelay.h"
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
    p.feed = 1.0f; p.loss = 0.2f; // shaper defaults to inactive (shapeLevel = 0)

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

    // 6) shaper Level shape (width=0.5, count=1, mode=1 permanent): +expand widens the
    // loud/quiet ratio, -homogenise narrows it -- same math as the old Compress knob.
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
        auto ratioAfter = [&] (float shapeLevel, int blocks)
        {
            SpectralEngine::Params h; h.feed = 0.0f; h.loss = 0.0f;
            h.shapeAmt = 1.0f; h.shapeMode = 1.0f; h.shape = 0.0f;
            h.shapeWidth = 0.5f; h.shapeCount = 1.0f; h.shapeLevel = shapeLevel;
            for (int blk = 0; blk < blocks; ++blk) { std::fill (buf2.begin(), buf2.end(), 0.0f); eng.process (buf2.data(), buf2.data(), block, h); }
            int n = 0; for (int t = 0; t < 8 && n == 0; ++t) { std::fill (buf2.begin(), buf2.end(), 0.0f); eng.process (buf2.data(), buf2.data(), block, h); n = eng.copyDisplay (m, ph); }
            return m[(size_t) B500] / juce::jmax (1.0e-9f, m[(size_t) B2k]);
        };

        deposit(); float base = ratioAfter (0.0f, 8);
        deposit(); float expanded = ratioAfter (+0.9f, 120);
        deposit(); float homogen  = ratioAfter (-0.9f, 120);
        bool ok = std::isfinite (expanded) && std::isfinite (homogen)
                  && expanded > base * 1.3f && homogen < base * 0.8f;
        printf ("[%s] shaper level: base=%.2f expand=%.2f homogenise=%.2f\n",
                ok ? "PASS" : "FAIL", base, expanded, homogen);
        fails += ok ? 0 : 1;
    }

    // 6b) shaper Sine shape as a filter (replaces the old Filter knob): a tone parked
    // at shapeFreq is strongly cut momentarily, and restored once amount goes to 0.
    {
        eng.setOrder (12); eng.reset();
        double ph = 0.0, w = 2.0 * M_PI * 1000.0 / sr;
        std::vector<float> buf2 (block);
        for (int b = 0; b < (int) (0.4 * sr / block); ++b)
        {
            for (int i = 0; i < block; ++i) { buf2[i] = 0.5f * (float) std::sin (ph); ph += w; }
            eng.process (buf2.data(), buf2.data(), block, p);
        }
        auto holdRms = [&] (const SpectralEngine::Params& hp)
        {
            for (int b = 0; b < 24; ++b) { std::fill (buf2.begin(), buf2.end(), 0.0f); eng.process (buf2.data(), buf2.data(), block, hp); }
            return rms (buf2.data(), block);
        };
        SpectralEngine::Params cut; cut.feed = 0.0f; cut.loss = 0.0f;
        cut.shapeAmt = 1.0f; cut.shapeMode = 0.0f; cut.shape = 4.0f; // Sine is index 4 (after Harmonics)
        cut.shapeFreq = 1000.0f; cut.shapeWidth = 0.35f; cut.shapeCount = 0.0f; cut.shapeLevel = -1.0f;
        float cutRms = holdRms (cut);

        SpectralEngine::Params restored = cut; restored.shapeAmt = 0.0f;
        float restoredRms = holdRms (restored);

        SpectralEngine::Params flat; flat.feed = 0.0f; flat.loss = 0.0f;
        float flatRms = holdRms (flat);

        bool ok = std::isfinite (cutRms) && cutRms < flatRms * 0.3f
                  && restoredRms > cutRms * 1.5f;
        printf ("[%s] shaper sine-as-filter: cut=%.4f restored=%.4f flat=%.4f\n",
                ok ? "PASS" : "FAIL", cutRms, restoredRms, flatRms);
        fails += ok ? 0 : 1;
    }

    // 6c) shaper Spikes shape is subtractive band-select: a single spike (count=0) at
    // shapeFreq. level>0 rejects the peak tone (keeps the off-peak tone); level<0 passes
    // only the peak tone (cuts the off-peak tone). Momentary (mode=0), read per-bin.
    {
        const int B1k = (int) std::lround (1000.0 * 4096.0 / sr);
        const int B4k = (int) std::lround (4000.0 * 4096.0 / sr);
        std::vector<float> m, ph, buf2 (block);
        eng.setOrder (12); eng.reset();
        double a = 0.0, b = 0.0, wa = 2.0 * M_PI * 1000.0 / sr, wb = 2.0 * M_PI * 4000.0 / sr;
        for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
        {
            for (int i = 0; i < block; ++i) { buf2[i] = 0.4f * (float) std::sin (a) + 0.4f * (float) std::sin (b); a += wa; b += wb; }
            eng.process (buf2.data(), buf2.data(), block, p);
        }
        auto measure = [&] (float level, float& at1k, float& at4k)
        {
            SpectralEngine::Params h; h.feed = 0.0f; h.loss = 0.0f;
            h.shapeAmt = 1.0f; h.shapeMode = 0.0f; h.shape = 2.0f;
            h.shapeFreq = 1000.0f; h.shapeWidth = 0.5f; h.shapeCount = 0.0f; h.shapeLevel = level;
            // run several frames so the display snapshot reflects the shaped output (mode=0
            // is non-destructive, so S is unchanged and each measure sees the same held tones)
            for (int blk = 0; blk < 24; ++blk) { std::fill (buf2.begin(), buf2.end(), 0.0f); eng.process (buf2.data(), buf2.data(), block, h); }
            int n = 0; for (int t = 0; t < 8 && n == 0; ++t) { std::fill (buf2.begin(), buf2.end(), 0.0f); eng.process (buf2.data(), buf2.data(), block, h); n = eng.copyDisplay (m, ph); }
            at1k = m[(size_t) B1k]; at4k = m[(size_t) B4k];
        };
        float f1, f4, r1, r4, p1, p4;
        measure (0.0f,  f1, f4);  // flat reference
        measure (+1.0f, r1, r4);  // reject peaks -> 1k cut, 4k kept
        measure (-1.0f, p1, p4);  // pass only peaks -> 4k cut, 1k kept
        bool ok = r1 < f1 * 0.3f && r4 > f4 * 0.7f   // rejected the peak tone, kept the other
                  && p4 < f4 * 0.3f && p1 > f1 * 0.7f; // passed only the peak tone
        printf ("[%s] shaper spikes subtractive: reject(1k=%.4f,4k=%.4f) pass(1k=%.4f,4k=%.4f)\n",
                ok ? "PASS" : "FAIL", r1, r4, p1, p4);
        fails += ok ? 0 : 1;
    }

    // 6d) shaper Harmonics shape is subtractive overtone/undertone band-select: tones at
    // 1 kHz (the fundamental) and 2.5 kHz (NOT in 1 kHz's harmonic series). level>0
    // rejects the harmonic series (cuts 1k, keeps 2.5k); level<0 passes only the series
    // (keeps 1k, cuts 2.5k). count=0.3 -> ~3.6 harmonics/side, plenty to cover 2k/3k
    // without touching 2.5k. Momentary (mode=0), read per-bin.
    {
        const int B1k   = (int) std::lround (1000.0 * 4096.0 / sr);
        const int B2500 = (int) std::lround (2500.0 * 4096.0 / sr);
        std::vector<float> m, ph, buf2 (block);
        eng.setOrder (12); eng.reset();
        double a = 0.0, b = 0.0, wa = 2.0 * M_PI * 1000.0 / sr, wb = 2.0 * M_PI * 2500.0 / sr;
        for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
        {
            for (int i = 0; i < block; ++i) { buf2[i] = 0.4f * (float) std::sin (a) + 0.4f * (float) std::sin (b); a += wa; b += wb; }
            eng.process (buf2.data(), buf2.data(), block, p);
        }
        auto measure = [&] (float level, float shapeParam, float& at1k, float& at2500)
        {
            SpectralEngine::Params h; h.feed = 0.0f; h.loss = 0.0f;
            h.shapeAmt = 1.0f; h.shapeMode = 0.0f; h.shape = shapeParam;
            h.shapeFreq = 1000.0f; h.shapeWidth = 0.5f; h.shapeCount = 0.3f; h.shapeLevel = level;
            // run several frames so the display snapshot reflects the shaped output (mode=0
            // is non-destructive, so S is unchanged and each measure sees the same held tones)
            for (int blk = 0; blk < 24; ++blk) { std::fill (buf2.begin(), buf2.end(), 0.0f); eng.process (buf2.data(), buf2.data(), block, h); }
            int n = 0; for (int t = 0; t < 8 && n == 0; ++t) { std::fill (buf2.begin(), buf2.end(), 0.0f); eng.process (buf2.data(), buf2.data(), block, h); n = eng.copyDisplay (m, ph); }
            at1k = m[(size_t) B1k]; at2500 = m[(size_t) B2500];
        };
        float f1, f2, r1, r2, p1, p2, x1, x2;
        measure (0.0f,  3.0f, f1, f2);  // flat reference
        measure (+1.0f, 3.0f, r1, r2);  // reject series -> 1k cut, 2.5k kept
        measure (-1.0f, 3.0f, p1, p2);  // pass only series -> 2.5k cut, 1k kept
        measure (-1.0f, 2.5f, x1, x2);  // mid crossfade (Spikes<->Harmonics) stays finite
        bool ok = r1 < f1 * 0.3f && r2 > f2 * 0.7f    // rejected the fundamental, kept 2.5k
                  && p2 < f2 * 0.3f && p1 > f1 * 0.7f  // passed only the harmonic series
                  && std::isfinite (x1) && std::isfinite (x2);
        printf ("[%s] shaper harmonics subtractive: reject(1k=%.4f,2.5k=%.4f) pass(1k=%.4f,2.5k=%.4f)\n",
                ok ? "PASS" : "FAIL", r1, r2, p1, p2);
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

    // 9) harmonize: with two close tones held, it perturbs the output but stays bounded
    {
        auto runHarm = [&] (float amount, std::vector<float>& tail)
        {
            SpectralEngine e2; e2.prepare (sr, 13); e2.setOrder (12); e2.reset();
            std::vector<float> b (block);
            double a = 0.0, c = 0.0, wa = 2.0 * M_PI * 400.0 / sr, wc = 2.0 * M_PI * 520.0 / sr;
            for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
            {
                for (int i = 0; i < block; ++i) { b[i] = 0.4f * (float) std::sin (a) + 0.4f * (float) std::sin (c); a += wa; c += wc; }
                e2.process (b.data(), b.data(), block, p);
            }
            SpectralEngine::Params h; h.feed = 0.0f; h.loss = 0.0f; h.harmonize = amount; h.harmWidth = 1.0f;
            float mx = 0.0f;
            for (int blk = 0; blk < 80; ++blk)
            {
                std::fill (b.begin(), b.end(), 0.0f);
                e2.process (b.data(), b.data(), block, h);
                for (int i = 0; i < block; ++i) mx = juce::jmax (mx, std::abs (b[i]));
            }
            tail.assign (b.begin(), b.end());
            return mx;
        };
        std::vector<float> off, on;
        runHarm (0.0f, off);
        float mxOn = runHarm (1.0f, on);
        float diff = 0.0f;
        for (int i = 0; i < block; ++i) diff = juce::jmax (diff, std::abs (off[i] - on[i]));
        bool ok = std::isfinite (mxOn) && mxOn < 5.0f && diff > 1.0e-4f;
        printf ("[%s] harmonize: changes=%.2e maxAbsOn=%.3f\n", ok ? "PASS" : "FAIL", diff, mxOn);
        fails += ok ? 0 : 1;
    }

    // 10) energy migration: a tone pulled toward a far harmonic crosses bins (> omega-only
    //     reach ~2 bins / ~23 Hz at 4096/48k), proving the energy actually moved.
    {
        SpectralEngine e2; e2.prepare (sr, 13); e2.setOrder (12); e2.reset();
        std::vector<float> b (block);
        // dominant fixed anchor at 1000, weak tone at 1400 (-> nearest ratio 4/3 -> 1333 Hz)
        double a = 0.0, c = 0.0, wa = 2.0 * M_PI * 1400.0 / sr, wc = 2.0 * M_PI * 1000.0 / sr;
        for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
        {
            for (int i = 0; i < block; ++i) { b[i] = 0.1f * (float) std::sin (a) + 0.9f * (float) std::sin (c); a += wa; c += wc; }
            e2.process (b.data(), b.data(), block, p);
        }
        SpectralEngine::Params h; h.feed = 0.0f; h.loss = 0.0f; h.harmonize = 1.0f; h.harmonic = 1.0f; h.harmWidth = 1.5f;
        for (int blk = 0; blk < 600; ++blk) { std::fill (b.begin(), b.end(), 0.0f); e2.process (b.data(), b.data(), block, h); }

        std::vector<float> pf, pw, pd; int n = 0;
        for (int t = 0; t < 8 && n <= 0; ++t) { std::fill (b.begin(), b.end(), 0.0f); e2.process (b.data(), b.data(), block, h); n = e2.copyPeaks (pf, pw, pd); }
        // the weak tone tracks the anchor's harmonic (~4/3). It must drop below the
        // omega-only floor (~1383 Hz for its home bin) to prove energy actually migrated.
        float weak = 1400.0f;
        for (int i = 0; i < n; ++i) if (pf[(size_t) i] > 1250.0f && pf[(size_t) i] < 1395.0f) weak = pf[(size_t) i];
        bool ok = n > 0 && weak < 1382.0f;
        printf ("[%s] energy migration: weak tone 1400 -> %.0f Hz (past omega-only floor)\n",
                ok ? "PASS" : "FAIL", weak);
        fails += ok ? 0 : 1;
    }

    // 11) unison lock: two close tones entrain, lock to one frequency, and stop beating
    //     (block-RMS becomes steady instead of pulsing).
    {
        auto beatDepth = [&] (float harmonize)
        {
            SpectralEngine e2; e2.prepare (sr, 13); e2.setOrder (12); e2.reset();
            std::vector<float> b (block);
            double a = 0.0, c = 0.0, wa = 2.0 * M_PI * 500.0 / sr, wc = 2.0 * M_PI * 560.0 / sr;
            for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
            {
                for (int i = 0; i < block; ++i) { b[i] = 0.45f * (float) std::sin (a) + 0.45f * (float) std::sin (c); a += wa; c += wc; }
                e2.process (b.data(), b.data(), block, p);
            }
            SpectralEngine::Params h; h.feed = 0.0f; h.loss = 0.0f; h.harmonize = harmonize; h.harmWidth = 1.0f;
            for (int blk = 0; blk < 600; ++blk) { std::fill (b.begin(), b.end(), 0.0f); e2.process (b.data(), b.data(), block, h); }
            float mn = 1.0e9f, mx = 0.0f;
            for (int blk = 0; blk < 120; ++blk)
            {
                std::fill (b.begin(), b.end(), 0.0f); e2.process (b.data(), b.data(), block, h);
                float r = rms (b.data(), block);
                mn = juce::jmin (mn, r); mx = juce::jmax (mx, r);
            }
            return mx > 1.0e-6f ? (mx - mn) / mx : 0.0f; // 0 = steady, ->1 = strong beating
        };
        float off = beatDepth (0.0f);   // no harmonize: tones beat
        float on  = beatDepth (1.0f);   // harmonize: should lock -> steady
        bool ok = on < 0.5f * off || on < 0.1f;
        printf ("[%s] unison lock: beat depth off=%.2f on=%.2f\n", ok ? "PASS" : "FAIL", off, on);
        fails += ok ? 0 : 1;
    }

    // 12) peak detection: a close pair is resolved as TWO peaks, and a quiet tone is included
    {
        SpectralEngine e2; e2.prepare (sr, 13); e2.setOrder (12); e2.reset();
        std::vector<float> b (block);
        double p1 = 0, p2 = 0, p3 = 0, p4 = 0;
        const double w1 = 2.0 * M_PI * 600.0 / sr,  w2 = 2.0 * M_PI * 635.0 / sr;   // close pair (~3 bins)
        const double w3 = 2.0 * M_PI * 1000.0 / sr, w4 = 2.0 * M_PI * 1500.0 / sr;  // loud + quiet (-26 dB)
        for (int blk = 0; blk < (int) (0.4 * sr / block); ++blk)
        {
            for (int i = 0; i < block; ++i)
            {
                b[i] = 0.4f * (float) std::sin (p1) + 0.4f * (float) std::sin (p2)
                     + 0.8f * (float) std::sin (p3) + 0.04f * (float) std::sin (p4);
                p1 += w1; p2 += w2; p3 += w3; p4 += w4;
            }
            e2.process (b.data(), b.data(), block, p);
        }
        SpectralEngine::Params h; h.feed = 0.0f; h.loss = 0.0f; h.harmonize = 0.01f; h.harmWidth = 1.0f;
        std::vector<float> pf, pw, pd; int n = 0;
        for (int t = 0; t < 10 && n <= 0; ++t) { std::fill (b.begin(), b.end(), 0.0f); e2.process (b.data(), b.data(), block, h); n = e2.copyPeaks (pf, pw, pd); }
        int pair = 0, quiet = 0;
        for (int i = 0; i < n; ++i)
        {
            if (pf[(size_t) i] > 580.0f && pf[(size_t) i] < 660.0f) ++pair;
            if (pf[(size_t) i] > 1440.0f && pf[(size_t) i] < 1560.0f) ++quiet;
        }
        bool ok = pair == 2 && quiet >= 1;
        printf ("[%s] peak detection: close pair=%d (expect 2), quiet tone=%d (expect >=1)\n",
                ok ? "PASS" : "FAIL", pair, quiet);
        fails += ok ? 0 : 1;
    }

    // ---- PlateReverb (agent-wiki/plan-reverb.md §6) ----

    auto rmsRange = [] (const std::vector<float>& x, int from, int to)
    {
        return rms (x.data() + from, to - from);
    };

    // 2) tail exists & decays: impulse -> wet output; RMS around 1.0-1.5s nonzero,
    // RMS around 4.0-4.5s smaller (decay=0.5, size=1)
    {
        PlateReverb rv; rv.prepare (sr);
        rv.setParams (0.5f, 1.0f, 0.3f, 0.02f);
        const int n = (int) (5.0 * sr);
        std::vector<float> in (n, 0.0f), wl (n), wr (n);
        in[0] = 1.0f;
        rv.process (in.data(), wl.data(), wr.data(), n);
        bool finite = finiteAll (wl.data(), n) && finiteAll (wr.data(), n);
        float early = rmsRange (wl, (int) (1.0 * sr), (int) (1.5 * sr));
        float late  = rmsRange (wl, (int) (4.0 * sr), (int) (4.5 * sr));
        bool ok = finite && early > 1.0e-5f && late < early;
        printf ("[%s] reverb tail decays: early=%.2e late=%.2e\n", ok ? "PASS" : "FAIL", early, late);
        fails += ok ? 0 : 1;
    }

    // 3) decay knob is monotonic: RMS at t=2s with decay=0.8 > decay=0.3
    {
        auto rmsAt2s = [&] (float decay)
        {
            PlateReverb rv; rv.prepare (sr);
            rv.setParams (decay, 1.0f, 0.3f, 0.02f);
            const int n = (int) (2.2 * sr);
            std::vector<float> in (n, 0.0f), wl (n), wr (n);
            in[0] = 1.0f;
            rv.process (in.data(), wl.data(), wr.data(), n);
            return rmsRange (wl, (int) (2.0 * sr), (int) (2.2 * sr));
        };
        float lo = rmsAt2s (0.3f), hi = rmsAt2s (0.8f);
        bool ok = std::isfinite (lo) && std::isfinite (hi) && hi > lo;
        printf ("[%s] reverb decay monotonic: decay0.3=%.2e decay0.8=%.2e\n",
                ok ? "PASS" : "FAIL", lo, hi);
        fails += ok ? 0 : 1;
    }

    // 4) stability at extremes: decay=1.0 (clamped 0.98), size=2.0 -- 2s noise then
    // 30s silence, every sample finite and bounded
    {
        PlateReverb rv; rv.prepare (sr);
        rv.setParams (1.0f, 2.0f, 0.3f, 0.02f);
        juce::Random rng (1234);
        const int nNoise = (int) (2.0 * sr);
        std::vector<float> in (nNoise), wl (nNoise), wr (nNoise);
        for (int i = 0; i < nNoise; ++i) in[i] = rng.nextFloat() * 2.0f - 1.0f;
        rv.process (in.data(), wl.data(), wr.data(), nNoise);

        const int nSil = (int) (30.0 * sr);
        std::vector<float> sil (nSil, 0.0f), sl (nSil), sr2 (nSil);
        rv.process (sil.data(), sl.data(), sr2.data(), nSil);

        bool finite = finiteAll (wl.data(), nNoise) && finiteAll (wr.data(), nNoise)
                      && finiteAll (sl.data(), nSil) && finiteAll (sr2.data(), nSil);
        float maxAbs = 0.0f;
        for (float v : wl) maxAbs = juce::jmax (maxAbs, std::abs (v));
        for (float v : sl)  maxAbs = juce::jmax (maxAbs, std::abs (v));
        bool ok = finite && maxAbs < 10.0f;
        printf ("[%s] reverb stability at extremes: finite=%d maxAbs=%.3f\n",
                ok ? "PASS" : "FAIL", (int) finite, maxAbs);
        fails += ok ? 0 : 1;
    }

    // 5) stereo decorrelation: impulse tail, normalized cross-correlation of L vs R
    // over the first 2s < 0.9
    {
        PlateReverb rv; rv.prepare (sr);
        rv.setParams (0.6f, 1.0f, 0.3f, 0.02f);
        const int n = (int) (2.0 * sr);
        std::vector<float> in (n, 0.0f), wl (n), wr (n);
        in[0] = 1.0f;
        rv.process (in.data(), wl.data(), wr.data(), n);
        double dot = 0.0, el = 0.0, er = 0.0;
        for (int i = 0; i < n; ++i) { dot += (double) wl[i] * wr[i]; el += (double) wl[i] * wl[i]; er += (double) wr[i] * wr[i]; }
        float corr = (float) (dot / std::sqrt (juce::jmax (1.0e-20, el * er)));
        bool ok = std::isfinite (corr) && corr < 0.9f;
        printf ("[%s] reverb stereo decorrelation: corr=%.3f\n", ok ? "PASS" : "FAIL", corr);
        fails += ok ? 0 : 1;
    }

    // 6) damping darkens: HF proxy (energy of first-difference) at damp=0.9 must be
    // less than at damp=0.1
    {
        auto hfEnergy = [&] (float damp)
        {
            PlateReverb rv; rv.prepare (sr);
            rv.setParams (0.6f, 1.0f, damp, 0.02f);
            const int n = (int) (1.0 * sr);
            std::vector<float> in (n, 0.0f), wl (n), wr (n);
            in[0] = 1.0f;
            rv.process (in.data(), wl.data(), wr.data(), n);
            double e = 0.0;
            for (int i = 1; i < n; ++i) { float d = wl[i] - wl[i - 1]; e += (double) d * d; }
            return e;
        };
        double bright = hfEnergy (0.1f), dark = hfEnergy (0.9f);
        bool ok = std::isfinite (bright) && std::isfinite (dark) && dark < bright;
        printf ("[%s] reverb damping darkens: bright=%.3e dark=%.3e\n", ok ? "PASS" : "FAIL", bright, dark);
        fails += ok ? 0 : 1;
    }

    // 7) size change doesn't explode: sweep size 0.5 -> 2.0 over 1s while feeding noise
    {
        PlateReverb rv; rv.prepare (sr);
        juce::Random rng (5678);
        const int n = (int) (1.0 * sr);
        std::vector<float> wl (n), wr (n);
        bool finite = true;
        float maxAbs = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            float size = juce::jmap ((float) i / (float) n, 0.5f, 2.0f);
            rv.setParams (0.6f, size, 0.3f, 0.02f);
            float x = rng.nextFloat() * 2.0f - 1.0f;
            rv.process (&x, &wl[(size_t) i], &wr[(size_t) i], 1);
            if (! std::isfinite (wl[(size_t) i]) || ! std::isfinite (wr[(size_t) i])) finite = false;
            maxAbs = juce::jmax (maxAbs, std::abs (wl[(size_t) i]));
        }
        bool ok = finite && maxAbs < 10.0f;
        printf ("[%s] reverb size sweep stable: finite=%d maxAbs=%.3f\n",
                ok ? "PASS" : "FAIL", (int) finite, maxAbs);
        fails += ok ? 0 : 1;
    }

    // ---- integration branch (agent-wiki/plan-integration.md §7) ----

    // dryWet alignment: DryDelay must delay by exactly the requested amount (this is
    // what time-aligns the dry path with the engine's fftSize latency in the processor)
    {
        DryDelay dd; dd.prepare (8192, block);
        const int delay = 4096;
        std::vector<float> src ((size_t) (delay + 4 * block)), got (src.size());
        for (size_t i = 0; i < src.size(); ++i) src[i] = std::sin (0.01 * (double) i) * 0.7f;
        for (size_t off = 0; off < src.size(); off += (size_t) block)
            dd.process (src.data() + off, got.data() + off, block, delay);
        float maxErr = 0.0f;
        for (size_t i = (size_t) delay; i < src.size(); ++i)
            maxErr = juce::jmax (maxErr, std::abs (got[i] - src[i - (size_t) delay]));
        // and the pre-delay region must be silent (cleared ring)
        float pre = rms (got.data(), delay);
        bool ok = maxErr == 0.0f && pre < 1.0e-9f;
        printf ("[%s] dry delay alignment: maxErr=%.2e preRMS=%.2e\n", ok ? "PASS" : "FAIL", maxErr, pre);
        fails += ok ? 0 : 1;
    }

    // everything-on stability (in == out aliasing path, house rule): shaper permanent +
    // harmonize + phase noise all active on a noise feed -> finite, bounded
    {
        SpectralEngine e2; e2.prepare (sr, 13); e2.setOrder (12); e2.reset();
        SpectralEngine::Params a;
        a.feed = 1.0f; a.loss = 0.1f; a.phaseNoise = true;
        a.shapeAmt = 1.0f; a.shapeMode = 1.0f; a.shape = 2.5f; // mid-crossfade Spikes/Harmonics
        a.shapeFreq = 800.0f; a.shapeWidth = 0.6f; a.shapeCount = 0.5f; a.shapeLevel = 0.7f;
        a.harmonize = 0.05f; a.harmWidth = 1.0f; a.harmonic = 1.0f;
        juce::Random rng (99);
        std::vector<float> b (block);
        bool finite = true;
        float mx = 0.0f;
        for (int blk = 0; blk < (int) (30.0 * sr / block); ++blk)
        {
            for (int i = 0; i < block; ++i) b[(size_t) i] = rng.nextFloat() * 1.6f - 0.8f;
            e2.process (b.data(), b.data(), block, a); // in == out
            if (! finiteAll (b.data(), block)) { finite = false; break; }
            for (int i = 0; i < block; ++i) mx = juce::jmax (mx, std::abs (b[(size_t) i]));
        }
        bool ok = finite && mx < 50.0f;
        printf ("[%s] everything-on stability: finite=%d maxAbs=%.3f\n",
                ok ? "PASS" : "FAIL", (int) finite, mx);
        fails += ok ? 0 : 1;
    }

    // revFeed: engine + reverb + feedback loop composed exactly like the processor
    // (mono engine, wet re-injected next block scaled by 0.5 * revFeed).
    // (a) integration: with revFeed the reverb tail is re-captured by the hold, so
    //     held energy after input stops is higher than without.
    // (b) worst-case stability: feed=1, loss=0, revFeed=1, revDecay=1 -> 2 s noise
    //     then 30 s silence stays finite and bounded (the processor's limiter isn't
    //     in this loop, so the loop itself must not run away).
    {
        auto runLoop = [&] (float revFeed, float loss, float revDecay, bool noiseIn,
                            double holdSeconds, float& heldRms, float& maxAbs)
        {
            SpectralEngine e2; e2.prepare (sr, 13); e2.setOrder (12); e2.reset();
            PlateReverb rv; rv.prepare (sr);
            rv.setParams (revDecay, 1.0f, 0.3f, 0.02f);
            SpectralEngine::Params fp; fp.feed = 1.0f; fp.loss = loss;
            juce::Random rng (7);
            std::vector<float> b (block), wl (block, 0.0f), wr (block, 0.0f);
            maxAbs = 0.0f; heldRms = 0.0f;
            double ph = 0.0, w = 2.0 * M_PI * 660.0 / sr;
            const int feedBlocks = (int) (2.0 * sr / block);
            const int holdBlocks = (int) (holdSeconds * sr / block);
            for (int blk = 0; blk < feedBlocks + holdBlocks; ++blk)
            {
                for (int i = 0; i < block; ++i)
                {
                    float x = 0.0f;
                    if (blk < feedBlocks)
                        x = noiseIn ? rng.nextFloat() * 1.6f - 0.8f
                                    : 0.5f * (float) std::sin (ph);
                    ph += w;
                    // inject last wet, clamped to +/-1 exactly like the processor does
                    b[(size_t) i] = x + 0.5f * revFeed * juce::jlimit (-1.0f, 1.0f, wl[(size_t) i]);
                }
                e2.process (b.data(), b.data(), block, fp);
                rv.process (b.data(), wl.data(), wr.data(), block); // wet of the output
                for (int i = 0; i < block; ++i) maxAbs = juce::jmax (maxAbs, std::abs (b[(size_t) i]));
                if (! finiteAll (b.data(), block)) { maxAbs = 1.0e30f; return; }
                if (blk == feedBlocks + holdBlocks - 1) heldRms = rms (b.data(), block);
            }
        };

        float rmsOff, rmsOn, mxOff, mxOn;
        runLoop (0.0f, 0.4f, 0.8f, false, 2.0, rmsOff, mxOff);
        runLoop (1.0f, 0.4f, 0.8f, false, 2.0, rmsOn,  mxOn);
        bool okInt = std::isfinite (rmsOn) && rmsOn > rmsOff * 1.5f;
        printf ("[%s] revFeed integrates tail: heldRMS off=%.2e on=%.2e\n",
                okInt ? "PASS" : "FAIL", rmsOff, rmsOn);
        fails += okInt ? 0 : 1;

        // At loss=0 the held state accumulates the clamped injection linearly (the
        // documented eternal-hold behavior; the processor's limiter caps the output).
        // The assertion is "no exponential runaway": unclamped feedback hit ~1e32 here.
        float rmsWc, mxWc;
        runLoop (1.0f, 0.0f, 1.0f, true, 30.0, rmsWc, mxWc);
        bool okStab = mxWc < 500.0f;
        printf ("[%s] revFeed worst-case stability: maxAbs=%.3f\n", okStab ? "PASS" : "FAIL", mxWc);
        fails += okStab ? 0 : 1;
    }

    printf ("\n%s (%d failure%s)\n", fails == 0 ? "ALL PASS" : "FAILURES",
            fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
