#include "SpectralEngine.h"
#include <numeric> // std::gcd

namespace
{
    constexpr int   kOverlap   = 4;      // 75% overlap, hop = fftSize/4
    constexpr float kCompFloor = 0.05f;  // clamp on 1/filterGain to avoid blow-up
    constexpr float kDecayFloor = 1.0e-4f;
    constexpr float kInjFloor  = 0.05f;  // min injection scale (so capture works at loss=0)
    constexpr float kCompressRate = 0.05f; // per-frame strength of the compress reshaping
    constexpr float kPhaseNoise   = 0.15f; // rad of per-frame phase jitter when noise is on
    // harmonize
    constexpr float kEntRate   = 0.12f;  // per-frame fraction toward the entrainment target
    constexpr float kHarmRate  = 0.12f;  // per-frame fraction toward the harmonic target
    constexpr float kHarmStep  = 0.05f;  // clamp on per-frame omega shift (rad/hop)
    constexpr float kPeakFloor = 0.03f;  // peak threshold as a fraction of the max magnitude.
                                         // ~-30 dB: just above Hann's ~-31 dB first sidelobe
                                         // (so leakage isn't mistaken for a tone) but low
                                         // enough to include fairly quiet tones in harmonizing.
    constexpr int   kMaxDen    = 6;      // largest harmonic ratio denominator/numerator
}

void SpectralEngine::prepare (double sr, int maxFftOrder)
{
    sampleRate = sr;
    maxOrder   = maxFftOrder;
    maxFftSize = 1 << maxFftOrder;

    // Preallocate everything at the maximum size so setOrder() never allocates.
    fftData.assign ((size_t) (2 * maxFftSize), 0.0f);
    dispScratch.assign ((size_t) (maxFftSize / 2 + 1), {});
    brushPending.reserve (256);
    brushScratch.reserve (256);
    peakBin.assign   ((size_t) kMaxPeaks, 0);
    peakFreq.assign  ((size_t) kMaxPeaks, 0.0f);
    peakAmp.assign   ((size_t) kMaxPeaks, 0.0f);
    peakDelta.assign ((size_t) kMaxPeaks, 0.0f);
    dispPeakF.assign ((size_t) kMaxPeaks, 0.0f);
    dispPeakA.assign ((size_t) kMaxPeaks, 0.0f);
    dispPeakD.assign ((size_t) kMaxPeaks, 0.0f);
    inRing.assign  ((size_t) maxFftSize, 0.0f);
    outRing.assign ((size_t) maxFftSize, 0.0f);
    window.assign  ((size_t) maxFftSize, 0.0f);
    S.assign   ((size_t) (maxFftSize / 2 + 1), {});
    Xs.assign  ((size_t) (maxFftSize / 2 + 1), {});
    expectedAdv.assign ((size_t) (maxFftSize / 2 + 1), 0.0f);
    omega.assign       ((size_t) (maxFftSize / 2 + 1), 0.0f);
    prevPhase.assign   ((size_t) (maxFftSize / 2 + 1), 0.0f);
    dispMag.assign   ((size_t) (maxFftSize / 2 + 1), 0.0f);
    dispPhase.assign ((size_t) (maxFftSize / 2 + 1), 0.0f);

    configure (maxFftOrder); // default to max; processor overrides via setOrder()
    reset();
}

void SpectralEngine::configure (int fftOrder)
{
    order   = juce::jlimit (1, maxOrder, fftOrder);
    fftSize = 1 << order;
    hopSize = fftSize / kOverlap;
    numBins = fftSize / 2 + 1;

    fft = std::make_unique<juce::dsp::FFT> (order);

    // Hann window, applied on both analysis and synthesis.
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                     * (float) i / (float) fftSize);

    // Overlap-add normalisation for a Hann^2 window at 75% overlap = 1.5.
    winNorm = 1.0f / 1.5f;

    // Bin-centre phase advance per hop = 2pi * k * hop / fftSize. This is only the
    // starting estimate; omega[] is refined per frame from the input's instantaneous
    // frequency so held partials free-run at their true pitch (smooth, not grainy).
    for (int k = 0; k < numBins; ++k)
        expectedAdv[(size_t) k] = juce::MathConstants<float>::twoPi
                                  * (float) k * (float) hopSize / (float) fftSize;
}

float SpectralEngine::filterGain (float freqHz, float toneHz, float amt)
{
    const float oct  = std::log2 (juce::jmax (20.0f, freqHz));
    const float cent = std::log2 (juce::jmax (20.0f, toneHz));
    const float d    = oct - cent;
    const float bell = std::exp (-(d * d) / (2.0f * kSigmaOct * kSigmaOct));
    return 1.0f - amt * (1.0f - bell);
}

void SpectralEngine::setOrder (int fftOrder)
{
    fftOrder = juce::jlimit (1, maxOrder, fftOrder);
    if (fftOrder != order)
        pendingOrder.store (fftOrder); // applied at next frame boundary on audio thread
}

void SpectralEngine::applyPendingOrder()
{
    int req = pendingOrder.exchange (-1);
    if (req < 0 || req == order)
        return;

    const juce::ScopedLock sl (displayLock);
    configure (req);
    reset();
}

void SpectralEngine::reset()
{
    std::fill (inRing.begin(),  inRing.end(),  0.0f);
    std::fill (outRing.begin(), outRing.end(), 0.0f);
    std::fill (S.begin(),  S.end(),  std::complex<float> {});
    std::fill (Xs.begin(), Xs.end(), std::complex<float> {});
    std::fill (prevPhase.begin(), prevPhase.end(), 0.0f);
    omega = expectedAdv; // start at bin centre until the input is measured
    inWrite = outRead = hopCount = 0;
}

void SpectralEngine::process (const float* in, float* out, int numSamples, const Params& p)
{
    for (int n = 0; n < numSamples; ++n)
    {
        // latch input first: in and out may alias (in-place processing).
        const float x = in[n];

        // pop output (latency = fftSize)
        out[n] = outRing[(size_t) outRead] * winNorm;
        outRing[(size_t) outRead] = 0.0f;
        outRead = (outRead + 1) % fftSize;

        // push input
        inRing[(size_t) inWrite] = x;
        inWrite = (inWrite + 1) % fftSize;

        if (++hopCount >= hopSize)
        {
            hopCount = 0;
            processFrame (p);
            applyPendingOrder(); // safe only between frames
        }
    }
}

void SpectralEngine::processFrame (const Params& p)
{
    // --- analysis: windowed copy of the last fftSize input samples (oldest first)
    for (int i = 0; i < fftSize; ++i)
    {
        float s = inRing[(size_t) ((inWrite + i) % fftSize)];
        fftData[(size_t) i] = s * window[(size_t) i];
    }
    fft->performRealOnlyForwardTransform (fftData.data());

    drainBrush(); // apply any pending GUI edits to the held spectrum

    // --- compress: reshape each tone's level relative to the average level.
    // >0 expands (louder tones get louder, quiet ones quieter -> purify); <0 homogenises
    // (quiet tones rise, loud ones drop). Applied gently per frame so it acts over time.
    if (std::abs (p.compress) > 1.0e-4f)
    {
        // pivot = mean level of the *active* bins (ignore the empty noise floor, else the
        // pivot collapses to ~0 and every real tone saturates the same way).
        float maxMag = 0.0f;
        for (int k = 1; k < numBins; ++k) maxMag = juce::jmax (maxMag, std::abs (S[(size_t) k]));
        if (maxMag > 1.0e-9f)
        {
            const float active = maxMag * 1.0e-3f; // activity threshold
            double sum = 0.0; int cnt = 0;
            for (int k = 1; k < numBins; ++k)
            {
                float a = std::abs (S[(size_t) k]);
                if (a > active) { sum += a; ++cnt; }
            }
            const float mean = (float) (sum / juce::jmax (1, cnt));
            const float e    = p.compress * kCompressRate;
            const float invM = 1.0f / juce::jmax (1.0e-9f, mean);
            for (int k = 1; k < numBins; ++k)
            {
                float a = std::abs (S[(size_t) k]);
                if (a <= active) continue; // leave the noise floor alone
                float ratio = juce::jlimit (0.01f, 100.0f, a * invM);
                S[(size_t) k] *= std::pow (ratio, e); // louder-than-mean up (e>0), quieter down
            }
        }
    }

    // --- harmonize: tones pull on each other's pitch (coupled oscillators)
    applyHarmonize (p);

    // --- per-hop scalars
    // loss -> decay multiplier per hop. loss=0 -> 1 (eternal), loss=1 -> fast.
    const float lossDecay = std::exp (-p.loss * (float) hopSize / (float) sampleRate * 6.0f);
    // attack -> input smoothing coefficient. attack=0 -> 1 (instant).
    const float aCoef = std::exp (-p.attack * 9.0f);
    // Injection scale: with the (now phase-coherent) feedback loop, feeding integrates.
    // Scaling by (1-lossDecay) makes the steady-state held level ~= feed * input level
    // instead of building up by 1/(1-lossDecay). Floored so capture still works at loss=0.
    const float injScale = juce::jmax (kInjFloor, 1.0f - lossDecay);
    const float feed    = p.feed * injScale;
    const float trackW  = p.feed; // input's influence on the held *frequency* follows Feed,
                                  // so feed=0 fully freezes (no input phase leak)

    const float refFreq   = (float) sampleRate / (float) fftSize; // freq of bin 1
    const float twoPi     = juce::MathConstants<float>::twoPi;
    constexpr float kTrackThresh = 1.0e-3f; // only re-estimate freq where input has energy

    // --- spectral update
    for (int k = 0; k < numBins; ++k)
    {
        const float binFreq = (float) k * refFreq;
        // The filter is now a NON-DESTRUCTIVE output shaping: it never enters the held
        // state's decay, so turning it off restores the held waves intact. Input is still
        // compensated by 1/gFilt so fed tones come out as if there were no filter.
        const float gFilt = (k > 0) ? filterGain (binFreq, p.filterTone, p.filterAmt) : 1.0f;
        const float comp  = 1.0f / juce::jmax (kCompFloor, gFilt); // input compensation
        const float decay = juce::jmax (kDecayFloor, lossDecay);   // loss only (no filter)

        std::complex<float> x { fftData[(size_t) (2 * k)], fftData[(size_t) (2 * k + 1)] };

        // --- instantaneous-frequency tracking: measure the true per-hop phase advance
        // of the input and hold the partial at *that* rate, so leakage bins stay phase
        // coherent and the freeze is a smooth continuous tone (not a bin-centre grain).
        const float phi  = std::arg (x);
        if (trackW > 1.0e-4f && std::abs (x) > kTrackThresh)
        {
            float dev = (phi - prevPhase[(size_t) k]) - expectedAdv[(size_t) k];
            dev -= twoPi * std::round (dev / twoPi);              // wrap to [-pi, pi]
            const float measured = expectedAdv[(size_t) k] + dev; // input's true advance/hop
            // Feed controls how strongly the input retunes the held tone (1 = snap, smooth).
            omega[(size_t) k] += (measured - omega[(size_t) k]) * trackW;
        }
        prevPhase[(size_t) k] = phi; // always tracked (bookkeeping; does not reach audio)

        // attack: smooth the fed input spectrum toward x
        std::complex<float>& xs = Xs[(size_t) k];
        xs += (x - xs) * aCoef;

        // free-run phasor at the tracked frequency (+ optional phase noise injected into
        // the frequency tracking), inject compensated input, decay (loss). The noise is
        // per-frame and non-accumulating, so it shimmers rather than detuning permanently.
        float w = omega[(size_t) k];
        if (p.phaseNoise)
            w += (rng.nextFloat() * 2.0f - 1.0f) * kPhaseNoise;
        std::complex<float> sk = S[(size_t) k] * std::polar (1.0f, w);
        sk += feed * xs * comp;
        sk *= decay;
        S[(size_t) k] = sk;

        // output is the held state shaped by the filter (non-destructive)
        const std::complex<float> outBin = sk * gFilt;
        fftData[(size_t) (2 * k)]     = outBin.real();
        fftData[(size_t) (2 * k + 1)] = outBin.imag();

        // snapshot the *output* spectrum so the display matches what is heard
        dispScratch[(size_t) k] = outBin;
    }

    // --- snapshot for the GUI (try-lock; skip if the editor is reading)
    if (const juce::ScopedTryLock stl (displayLock); stl.isLocked())
    {
        const float norm = 2.0f / (float) fftSize;
        for (int k = 0; k < numBins; ++k)
        {
            dispMag[(size_t) k]   = std::abs (dispScratch[(size_t) k]) * norm;
            dispPhase[(size_t) k] = std::arg (dispScratch[(size_t) k]);
        }
    }

    // --- synthesis: inverse transform, window, overlap-add
    fft->performRealOnlyInverseTransform (fftData.data());
    for (int i = 0; i < fftSize; ++i)
    {
        int idx = (outRead + i) % fftSize;
        outRing[(size_t) idx] += fftData[(size_t) i] * window[(size_t) i];
    }
}

void SpectralEngine::queueBrush (float centreFreqHz, float strength, float sigmaOct)
{
    const juce::ScopedLock sl (brushLock);
    brushPending.push_back ({ centreFreqHz, juce::jlimit (-1.0f, 1.0f, strength),
                              juce::jmax (0.05f, sigmaOct) });
}

void SpectralEngine::drainBrush()
{
    {
        const juce::ScopedTryLock stl (brushLock); // never block the audio thread
        if (! stl.isLocked() || brushPending.empty())
            return;
        brushScratch.swap (brushPending);
        brushPending.clear();
    }

    const float refFreq = (float) sampleRate / (float) fftSize;
    const float lnFactor = std::log (kBrushMaxFactor);

    for (const auto& op : brushScratch)
    {
        const float centreOct = std::log2 (juce::jmax (20.0f, op.centreFreq));
        const float invSig2   = 1.0f / (2.0f * op.sigmaOct * op.sigmaOct);
        for (int k = 1; k < numBins; ++k)
        {
            const float oct = std::log2 ((float) k * refFreq);
            const float d   = oct - centreOct;
            const float w   = std::exp (-(d * d) * invSig2); // blurred-brush falloff
            if (w < 1.0e-3f)
                continue;
            // gain = factor^(strength * w * rate); >1 boosts, <1 cuts, smoothly with w
            const float g = std::exp (op.strength * w * kBrushRate * lnFactor);
            S[(size_t) k] *= g;
        }
    }
    brushScratch.clear();
}

void SpectralEngine::applyHarmonize (const Params& p)
{
    // Harmonize is the master amount; Harmonic only blends the *character* (0 = averaging /
    // entrainment, 1 = harmonic attraction). So Harmonic does nothing while Harmonize is 0.
    if (p.harmonize <= 1.0e-4f)
    {
        const juce::ScopedTryLock stl (displayLock);
        if (stl.isLocked()) dispPeakN = 0; // clear the influence overlay
        return;
    }

    // low-denominator harmonic ratios (built once): value, log2(value), weight 1/(n*m)
    struct Ratio { float l2, invDen; };
    static const std::vector<Ratio> ratios = []
    {
        std::vector<Ratio> r;
        for (int n = 1; n <= kMaxDen; ++n)
            for (int m = 1; m <= kMaxDen; ++m)
                if (std::gcd (n, m) == 1)
                    r.push_back ({ std::log2 ((float) n / (float) m), 1.0f / (float) (n * m) });
        return r;
    }();

    // omega (rad/hop) <-> frequency (Hz): f = omega * fScale
    const float fScale = (float) sampleRate / ((float) hopSize * juce::MathConstants<float>::twoPi);

    // 1. extract significant spectral peaks (local maxima)
    float maxMag = 0.0f;
    for (int k = 1; k < numBins; ++k) maxMag = juce::jmax (maxMag, std::abs (S[(size_t) k]));
    if (maxMag < 1.0e-9f) return;
    const float floor = maxMag * kPeakFloor;

    int P = 0;
    for (int k = 4; k < numBins - 4 && P < kMaxPeaks; ++k)
    {
        const float a = std::abs (S[(size_t) k]);
        if (a <= floor) continue;
        // prominence: local max over +/-1 bin only (the floor rejects Hann sidelobes). A wider
        // window would suppress the quieter of two close tones, counting them as one peak so
        // they never entrain against each other; +/-1 resolves tones down to ~2 bins apart
        // while a single tone's mainlobe is still a single peak.
        bool isPeak = true;
        for (int o = -1; o <= 1 && isPeak; ++o)
            if (o != 0 && std::abs (S[(size_t) (k + o)]) > a) isPeak = false;
        if (isPeak)
        {
            peakBin[(size_t) P]   = k;
            peakAmp[(size_t) P]   = a;
            peakFreq[(size_t) P]  = omega[(size_t) k] * fScale;
            peakDelta[(size_t) P] = 0.0f;
            ++P;
        }
    }
    if (P < 2) return;

    // 1b. merge near-unison peaks into one centred phasor. Two tones that have entrained to
    // within ~a bin still sit a hair mistuned and beat forever. Just equalising their frequency
    // is not enough: a phasor stored away from its bin centre produces an amplitude ripple from
    // the overlap-add (itself a beat). So we sum their energy into the single bin nearest the
    // common frequency and clear both packets — one centred phasor = clean, steady, no beat.
    constexpr int kPad = 3;                                    // packet half-width (shared with migration)
    const float binHz = (float) sampleRate / (float) fftSize;
    const float lockTolHz = binHz * 2.0f;
    for (int i = 0; i < P; ++i)
        for (int j = i + 1; j < P; )
        {
            if (std::abs (peakFreq[(size_t) i] - peakFreq[(size_t) j]) >= lockTolHz) { ++j; continue; }

            const int bi = peakBin[(size_t) i], bj = peakBin[(size_t) j];
            const float wi = std::abs (S[(size_t) bi]), wj = std::abs (S[(size_t) bj]);
            const float mo = (wi * omega[(size_t) bi] + wj * omega[(size_t) bj]) / (wi + wj + 1.0e-12f);
            const std::complex<float> combined = S[(size_t) bi] + S[(size_t) bj];
            const int target = juce::jlimit (1, numBins - 2, (int) std::lround (mo * fScale / binHz));

            for (int base : { bi, bj })
                for (int o = -kPad; o <= kPad; ++o)
                {
                    const int idx = base + o;
                    if (idx >= 1 && idx < numBins)
                    {
                        S[(size_t) idx]     = std::complex<float> {};
                        omega[(size_t) idx] = expectedAdv[(size_t) idx];
                    }
                }
            S[(size_t) target]     = combined;
            omega[(size_t) target] = mo;

            peakBin[(size_t) i]  = target;
            peakAmp[(size_t) i]  = std::abs (combined);
            peakFreq[(size_t) i] = mo * fScale;
            --P; // swap-remove peak j
            peakBin[(size_t) j]  = peakBin[(size_t) P];
            peakAmp[(size_t) j]  = peakAmp[(size_t) P];
            peakFreq[(size_t) j] = peakFreq[(size_t) P];
            peakDelta[(size_t) j] = peakDelta[(size_t) P];
        }
    if (P < 1) return;

    const float invSig2 = 1.0f / (2.0f * juce::jmax (0.005f, p.harmWidth) * p.harmWidth);

    // 2. reciprocal pull: compute every peak's drift from the same snapshot
    for (int i = 0; i < P; ++i)
    {
        const float fi = peakFreq[(size_t) i];
        if (fi <= 0.0f) continue;
        double entNum = 0.0, entDen = 0.0, harmNum = 0.0, harmDen = 0.0;

        for (int j = 0; j < P; ++j)
        {
            if (j == i) continue;
            const float fj = peakFreq[(size_t) j];
            if (fj <= 0.0f) continue;

            const float doct = std::log2 (fj / fi);
            const float w    = std::exp (-doct * doct * invSig2);
            const float aw   = w * peakAmp[(size_t) j];

            // entrainment: drift toward neighbours (amplitude-weighted average)
            entNum += aw * (fj - fi);
            entDen += aw;

            // harmonic: drift toward fj * (nearest low-denominator ratio)
            if (p.harmonic > 1.0e-4f)
            {
                const float lr = std::log2 (fi / fj);
                float bestL2 = 0.0f, bestInv = 0.0f, bestDist = 1.0e9f;
                for (const auto& r : ratios)
                {
                    const float d = std::abs (r.l2 - lr);
                    if (d < bestDist) { bestDist = d; bestL2 = r.l2; bestInv = r.invDen; }
                }
                const float target = fj * std::exp2 (bestL2);
                const float hw = aw * bestInv;
                harmNum += hw * (target - fi);
                harmDen += hw;
            }
        }

        // blend the two characters: Harmonic = 0 pure entrainment, 1 pure harmonic.
        const float entDrift  = (entDen  > 0.0) ? (float) (entNum  / entDen)  : 0.0f;
        const float harmDrift = (harmDen > 0.0) ? (float) (harmNum / harmDen) : 0.0f;
        const float blended   = (1.0f - p.harmonic) * entDrift + p.harmonic * harmDrift;
        const float df        = p.harmonize * kHarmRate * blended;

        peakDelta[(size_t) i] = juce::jlimit (-kHarmStep, kHarmStep, df / fScale); // -> rad/hop
    }

    // snapshot peaks for the influence overlay (freq, weight, drift in Hz)
    if (const juce::ScopedTryLock stl (displayLock); stl.isLocked())
    {
        dispPeakN = juce::jmin (P, kMaxPeaks);
        for (int i = 0; i < dispPeakN; ++i)
        {
            dispPeakF[(size_t) i] = peakFreq[(size_t) i];
            dispPeakA[(size_t) i] = peakAmp[(size_t) i];
            dispPeakD[(size_t) i] = peakDelta[(size_t) i] * fScale; // rad/hop -> Hz drift
        }
    }

    // 3. apply the shift to each peak's bin and its immediate leakage neighbours
    for (int i = 0; i < P; ++i)
    {
        const int   k = peakBin[(size_t) i];
        const float d = peakDelta[(size_t) i];
        if (d == 0.0f) continue;
        for (int o = -2; o <= 2; ++o)
        {
            const int kk = k + o;
            if (kk < 1 || kk >= numBins) continue;
            const float lo = expectedAdv[(size_t) kk] - juce::MathConstants<float>::pi;
            const float hi = expectedAdv[(size_t) kk] + juce::MathConstants<float>::pi;
            omega[(size_t) kk] = juce::jlimit (lo, hi, omega[(size_t) kk] + d);
        }
    }

    // 4. energy migration across bins. A single bin's phasor only represents a frequency
    // within ~half a bin of its centre. When a peak's centre drifts past half a bin, shift
    // its whole packet (centre +/- kPad bins) RIGIDLY by one bin, carrying each bin's complex
    // value, omega (frequency-absolute) and phase. Shifting the packet as a unit keeps the
    // peak coherent (moving bins independently tears it apart); omega is preserved so the
    // pitch is continuous, and the tone can travel any distance one bin at a time.
    const float binW = juce::MathConstants<float>::twoPi * (float) hopSize / (float) fftSize;
    const float half = binW * 0.5f;
    for (int i = 0; i < P; ++i)
    {
        const int k = peakBin[(size_t) i];
        const float rel = omega[(size_t) k] - expectedAdv[(size_t) k];
        const int s = (rel > half) ? +1 : (rel < -half) ? -1 : 0;
        if (s == 0) continue;
        if (k - kPad < 1 || k + kPad >= numBins - 1) continue; // near edges: don't migrate

        // collision avoidance: if another peak is within two packet widths, don't migrate.
        // Overlapping packets would trample each other (smearing the energy). Such pairs are
        // already frequency-locked above, so leaving the energy put gives a steady sum.
        bool nearNeighbour = false;
        for (int j = 0; j < P && ! nearNeighbour; ++j)
            if (j != i && std::abs (peakBin[(size_t) j] - k) <= 2 * kPad)
                nearNeighbour = true;
        if (nearNeighbour) continue;

        // move src -> src+s for the whole packet, ordered so we never overwrite a not-yet-moved bin
        if (s > 0)
            for (int b = k + kPad; b >= k - kPad; --b)
            {
                S[(size_t) (b + 1)]        = S[(size_t) b];
                omega[(size_t) (b + 1)]    = omega[(size_t) b];
                prevPhase[(size_t) (b + 1)] = prevPhase[(size_t) b];
            }
        else
            for (int b = k - kPad; b <= k + kPad; ++b)
            {
                S[(size_t) (b - 1)]        = S[(size_t) b];
                omega[(size_t) (b - 1)]    = omega[(size_t) b];
                prevPhase[(size_t) (b - 1)] = prevPhase[(size_t) b];
            }

        // clear the bin vacated at the trailing edge and neutralise its omega
        const int vac = (s > 0) ? (k - kPad) : (k + kPad);
        S[(size_t) vac] = std::complex<float> {};
        omega[(size_t) vac] = expectedAdv[(size_t) vac];
    }
}

int SpectralEngine::copyDisplay (std::vector<float>& mag, std::vector<float>& phase)
{
    const juce::ScopedTryLock stl (displayLock);
    if (! stl.isLocked())
        return 0;

    mag.assign   (dispMag.begin(),   dispMag.begin()   + numBins);
    phase.assign (dispPhase.begin(), dispPhase.begin() + numBins);
    return numBins;
}

int SpectralEngine::copyPeaks (std::vector<float>& freq, std::vector<float>& weight,
                               std::vector<float>& drift)
{
    const juce::ScopedTryLock stl (displayLock);
    if (! stl.isLocked())
        return -1; // busy: keep last frame

    const int n = dispPeakN;
    freq.assign   (dispPeakF.begin(), dispPeakF.begin() + n);
    weight.assign (dispPeakA.begin(), dispPeakA.begin() + n);
    drift.assign  (dispPeakD.begin(), dispPeakD.begin() + n);
    return n;
}
