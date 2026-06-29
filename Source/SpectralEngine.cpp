#include "SpectralEngine.h"

namespace
{
    constexpr int   kOverlap   = 4;      // 75% overlap, hop = fftSize/4
    constexpr float kCompFloor = 0.05f;  // clamp on 1/filterGain to avoid blow-up
    constexpr float kDecayFloor = 1.0e-4f;
    constexpr float kInjFloor  = 0.05f;  // min injection scale (so capture works at loss=0)
    constexpr float kCompressRate = 0.05f; // per-frame strength of the compress reshaping
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

        // free-run phasor at the tracked frequency, inject compensated input, decay (loss)
        std::complex<float> sk = S[(size_t) k] * std::polar (1.0f, omega[(size_t) k]);
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

void SpectralEngine::queueBrush (float centreFreqHz, float strength)
{
    const juce::ScopedLock sl (brushLock);
    brushPending.push_back ({ centreFreqHz, juce::jlimit (-1.0f, 1.0f, strength) });
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
    const float invSig2  = 1.0f / (2.0f * kBrushSigmaOct * kBrushSigmaOct);

    for (const auto& op : brushScratch)
    {
        const float centreOct = std::log2 (juce::jmax (20.0f, op.centreFreq));
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
