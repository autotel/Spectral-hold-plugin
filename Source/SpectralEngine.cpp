#include "SpectralEngine.h"
#include "ShapeCurves.h"

namespace
{
    constexpr int   kOverlap   = 4;      // 75% overlap, hop = fftSize/4
    constexpr float kCompFloor = 0.05f;  // clamp on 1/momentary-gain to avoid blow-up
    constexpr float kDecayFloor = 1.0e-4f;
    constexpr float kInjFloor  = 0.05f;  // min injection scale (so capture works at loss=0)
    constexpr float kPermScale = 0.23f;  // permanent-mode per-frame strength (coupled with
                                          // ShapeCurves' 4.6 normaliser -- see plan doc's
                                          // "compress equivalence": don't change independently)
    constexpr float kLn4       = 1.386294361f; // momentary boost ceiling: +12 dB at L=1
    constexpr float kPhaseNoise   = 0.15f; // rad of per-frame phase jitter when noise is on
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

    // --- shaper: per-bin signed change L[k] in [-1..+1] from a cross-faded math curve
    // (ShapeCurves.h), applied as a permanent multiply on S (compounding, replaces
    // Compress) and/or a momentary output gain (non-destructive, replaces Filter),
    // cross-faded by shapeMode. The Level shape needs the pivot mean of the *active*
    // bins (as Compress did); the other shapes are purely positional and ignore it.
    const bool shaperActive = p.shapeAmt > 1.0e-4f && std::abs (p.shapeLevel) > 1.0e-4f;
    float shapeMean = 0.0f, activeThresh = 0.0f;
    if (shaperActive)
    {
        // pivot = mean level of the *active* bins (ignore the empty noise floor, else the
        // pivot collapses to ~0 and every real tone saturates the same way).
        float maxMag = 0.0f;
        for (int k = 1; k < numBins; ++k) maxMag = juce::jmax (maxMag, std::abs (S[(size_t) k]));
        activeThresh = maxMag * 1.0e-3f;
        double sum = 0.0; int cnt = 0;
        for (int k = 1; k < numBins; ++k)
        {
            float a = std::abs (S[(size_t) k]);
            if (a > activeThresh) { sum += a; ++cnt; }
        }
        shapeMean = (float) (sum / juce::jmax (1, cnt));
    }
    const float invShapeMean = 1.0f / juce::jmax (1.0e-9f, shapeMean);
    const float shapeX0 = std::log2 (juce::jmax (20.0f, p.shapeFreq));

    // --- per-hop scalars
    // loss -> decay multiplier per hop. loss=0 -> 1 (eternal), loss=1 -> fast.
    const float lossDecay = std::exp (-p.loss * (float) hopSize / (float) sampleRate * 6.0f);
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

        // --- shaper: L[k] in [-1..+1]. ratio=1 makes the Level-shape component a
        // no-op (used for bin 0 and inactive/noise-floor bins, mirroring the old
        // Compress leaving them untouched).
        float L = 0.0f;
        if (shaperActive && k > 0)
        {
            float ratio = 1.0f;
            if (shapeMean > 1.0e-9f)
            {
                float a = std::abs (S[(size_t) k]);
                if (a > activeThresh)
                    ratio = juce::jlimit (0.01f, 100.0f, a * invShapeMean);
            }
            const float x0 = std::log2 (juce::jmax (20.0f, binFreq));
            L = juce::jlimit (-1.0f, 1.0f,
                    ShapeCurves::shapeL (p.shape, x0, shapeX0, p.shapeWidth, p.shapeCount,
                                        p.shapeLevel, ratio)) * p.shapeAmt;
        }

        const bool hasL = std::abs (L) > 1.0e-9f;

        // permanent: gently reshape the held state itself (compounds over frames, like
        // the old Compress/brush).
        if (hasL && p.shapeMode > 1.0e-4f)
            S[(size_t) k] *= std::exp (L * p.shapeMode * kPermScale);

        // momentary: non-destructive output gain (replaces the old filter). Only cuts
        // are compensated at injection, so live input isn't attenuated by boosts.
        float gOut = 1.0f;
        if (hasL && p.shapeMode < 0.9999f)
        {
            const float tm = L * (1.0f - p.shapeMode);
            gOut = (tm >= 0.0f) ? std::exp (tm * kLn4)
                                 : (1.0f + tm) * (1.0f + tm); // smooth to 0 at tm = -1
        }
        const float comp  = 1.0f / juce::jmax (kCompFloor, juce::jmin (1.0f, gOut));
        const float decay = juce::jmax (kDecayFloor, lossDecay); // loss only

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

        std::complex<float>& xs = Xs[(size_t) k];
        xs = x; // attack was removed; input is fed in unsmoothed

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

        // output is the held state shaped by the momentary gain (non-destructive)
        const std::complex<float> outBin = sk * gOut;
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

int SpectralEngine::copyDisplay (std::vector<float>& mag, std::vector<float>& phase)
{
    const juce::ScopedTryLock stl (displayLock);
    if (! stl.isLocked())
        return 0;

    mag.assign   (dispMag.begin(),   dispMag.begin()   + numBins);
    phase.assign (dispPhase.begin(), dispPhase.begin() + numBins);
    return numBins;
}

void SpectralEngine::writeAudioState (juce::MemoryOutputStream& os) const
{
    os.writeInt (order);
    os.writeInt (numBins);
    for (int k = 0; k < numBins; ++k)
    {
        os.writeFloat (S[(size_t) k].real());
        os.writeFloat (S[(size_t) k].imag());
    }
    for (int k = 0; k < numBins; ++k) os.writeFloat (omega[(size_t) k]);
    for (int k = 0; k < numBins; ++k) os.writeFloat (prevPhase[(size_t) k]);
    for (int k = 0; k < numBins; ++k)
    {
        os.writeFloat (Xs[(size_t) k].real());
        os.writeFloat (Xs[(size_t) k].imag());
    }
}

void SpectralEngine::readAudioState (juce::MemoryInputStream& is)
{
    const int ord = is.readInt();
    const int nb  = is.readInt();
    configure (juce::jlimit (1, maxOrder, ord));
    reset(); // clears rings + S/Xs and sets omega = expectedAdv

    const int n = juce::jmin (nb, numBins);
    for (int k = 0; k < nb; ++k)
    {
        float re = is.readFloat(), im = is.readFloat();
        if (k < n) S[(size_t) k] = { re, im };
    }
    for (int k = 0; k < nb; ++k) { float v = is.readFloat(); if (k < n) omega[(size_t) k]     = v; }
    for (int k = 0; k < nb; ++k) { float v = is.readFloat(); if (k < n) prevPhase[(size_t) k] = v; }
    for (int k = 0; k < nb; ++k)
    {
        float re = is.readFloat(), im = is.readFloat();
        if (k < n) Xs[(size_t) k] = { re, im };
    }
}
