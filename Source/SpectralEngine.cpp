#include "SpectralEngine.h"
#include "ShapeCurves.h"
#include <numeric> // std::gcd
#include <algorithm> // std::copy

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
    constexpr float kPermCeilNorm = 1.0f; // permanent-mode boost self-limits toward this
                                          // NORMALISED bin magnitude (|S|*2/fftSize, i.e. ~a
                                          // full-scale sine); keeps the bipolar shapes from
                                          // compounding upward without a hard clamp
    constexpr float kPhaseNoiseMax = 0.5f; // rad of per-frame phase jitter at phaseNoise=1
                                           // (legacy bool "on" was 0.15 rad == amt 0.3)
    // harmonize
    constexpr float kEntRate   = 0.12f;  // per-frame fraction toward the entrainment target
    constexpr float kHarmRate  = 0.12f;  // per-frame fraction toward the harmonic target
    constexpr float kHarmStep  = 0.05f;  // clamp on per-frame omega shift (rad/hop)
    constexpr float kPeakFloor = 0.03f;  // peak threshold as a fraction of the max magnitude.
                                         // ~-30 dB: just above Hann's ~-31 dB first sidelobe
                                         // (so leakage isn't mistaken for a tone) but low
                                         // enough to include fairly quiet tones in harmonizing.
    constexpr int   kMaxDen    = 6;      // largest harmonic ratio denominator/numerator
    // East<->West continuous tone locations (agent-wiki/plan-eastwest.md, PLAN v2)
    constexpr float kLocSigma   = 0.35f; // room size: gaussian attenuation width along E<->W.
                                         // d=0.5 -> ~-18 dB, d=1 -> ~-71 dB. Absolute (NOT
                                         // normalised across tones) so gains can't jump.
    constexpr float kInjLocFloor = 1.0e-7f; // ignore near-zero injections when pulling loc
                                            // (silence must not drag tones toward the knob)

    // Location layers (agent-wiki/plan-loclayers.md): the injection-routing decision.
    // "Near enough" (att >= kClaimAtt) drags the nearest layer's loc toward the knob, same
    // as the single-layer model. Further than that, dragging would smear a tone recorded
    // elsewhere; instead CLAIM a fresh/quietest layer at this bin, leaving the far one
    // untouched. Matches ParticleEngine's kMatchAttFloor so both paradigms agree on where
    // "elsewhere" starts.
    constexpr float kClaimAtt = 0.1f;
    constexpr float kClaimClearFloor = 1.0e-4f; // |S| above which a claimed layer-bin's
                                                // stale residual is hard-reset before the
                                                // fresh recording claims it (avoids phase-
                                                // mushing old content with the new tone)

    // playback gain / edit weight at distance d along the E<->W line
    inline float ewAtt (float d)
    {
        const float t = d / kLocSigma;
        return std::exp (-t * t);
    }
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
    auxRing.assign    ((size_t) maxFftSize, 0.0f);
    auxFftData.assign ((size_t) (2 * maxFftSize), 0.0f);

    maxBins = maxFftSize / 2 + 1;
    const size_t layeredSize = (size_t) kNumLayers * (size_t) maxBins;
    S.assign      (layeredSize, {});
    omega.assign  (layeredSize, 0.0f);
    binLoc.assign (layeredSize, 0.0f); // all tones start at East (legacy position)
    transAcc.assign (layeredSize, 0.0f);
    synthScratch.assign ((size_t) maxBins, {});
    Xs.assign  ((size_t) maxBins, {});
    expectedAdv.assign  ((size_t) maxBins, 0.0f);
    prevPhase.assign    ((size_t) maxBins, 0.0f);
    mixAbsScratch.assign ((size_t) maxBins, 0.0f);
    dispMag.assign   ((size_t) maxBins, 0.0f);
    dispPhase.assign ((size_t) maxBins, 0.0f);
    dispLayerMag.assign (layeredSize, 0.0f);
    dispLayerLoc.assign (layeredSize, 0.0f);

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
    std::fill (auxRing.begin(), auxRing.end(), 0.0f);
    std::fill (S.begin(),  S.end(),  std::complex<float> {});
    std::fill (Xs.begin(), Xs.end(), std::complex<float> {});
    std::fill (prevPhase.begin(), prevPhase.end(), 0.0f);
    std::fill (binLoc.begin(), binLoc.end(), 0.0f); // tones re-home to East (legacy)
    std::fill (transAcc.begin(), transAcc.end(), 0.0f);
    // start every layer at bin centre until the input is measured
    for (int l = 0; l < kNumLayers; ++l)
        std::copy (expectedAdv.begin(), expectedAdv.begin() + maxBins, omega.begin() + (long) li (l, 0));
    inWrite = outRead = hopCount = 0;
}

void SpectralEngine::process (const float* in, const float* aux, float* out, int numSamples, const Params& p)
{
    for (int n = 0; n < numSamples; ++n)
    {
        // latch input first: in/aux and out may alias (in-place processing).
        const float x = in[n];
        const float a = (aux != nullptr) ? aux[n] : 0.0f;

        // pop output (latency = fftSize)
        out[n] = outRing[(size_t) outRead] * winNorm;
        outRing[(size_t) outRead] = 0.0f;
        outRead = (outRead + 1) % fftSize;

        // push input (aux shares inWrite -- sample-aligned with the analysis frame)
        inRing[(size_t) inWrite] = x;
        auxRing[(size_t) inWrite] = a;
        inWrite = (inWrite + 1) % fftSize;

        if (++hopCount >= hopSize)
        {
            hopCount = 0;
            processFrame (p);
            applyPendingOrder(); // safe only between frames
            applyPendingHold();  // safe only between frames, see queueHoldRestore()
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

    // Aux (reverb-feedback) analysis: a second, Feed-independent input path (see
    // agent-wiki/plan-roadmap.md Part A). Only computed when revFeed is in use so the
    // default costs nothing.
    const bool auxActive = p.revFeed > 1.0e-4f;
    if (auxActive)
    {
        for (int i = 0; i < fftSize; ++i)
            auxFftData[(size_t) i] = auxRing[(size_t) ((inWrite + i) % fftSize)] * window[(size_t) i];
        fft->performRealOnlyForwardTransform (auxFftData.data());
    }

    // E<->W (agent-wiki/plan-loclayers.md): the knob is a listener/recorder position on a
    // continuous line. Each layer/bin carries its own loc[k]; attenuation att(|L-loc[k]|)
    // is ABSOLUTE (never normalised across tones), so output gains are smooth in both L
    // and time. kNumLayers parallel held states per bin let the same frequency coexist at
    // multiple locations instead of one recording dragging/smearing another (§0-§2).
    const float ewL = juce::jlimit (0.0f, 1.0f, p.ewLocation);

    drainBrush (p); // pending GUI brush edits, per layer, distance-weighted
    for (int l = 0; l < kNumLayers; ++l)
        applyHarmonize (p, l, l == 0); // coupled-oscillator pitch drift, per layer, independent

    // shaper pivot: "shape what you hear" -- the listener-mix magnitude per bin,
    // mixAbs[k] = Sum_l |S_l[k]|*att_l[k], computed once and reused for both the mean scan
    // below and the per-bin ratio in the main loop (agent-wiki/plan-loclayers.md §3).
    const bool shaperActive = p.shapeAmt > 1.0e-4f && std::abs (p.shapeLevel) > 1.0e-4f;
    float shapeMean = 0.0f, activeThresh = 0.0f;
    if (shaperActive)
    {
        for (int k = 1; k < numBins; ++k)
        {
            float m = 0.0f;
            for (int l = 0; l < kNumLayers; ++l)
                m += std::abs (S[li (l, k)]) * ewAtt (std::abs (ewL - binLoc[li (l, k)]));
            mixAbsScratch[(size_t) k] = m;
        }
        float maxMag = 0.0f;
        for (int k = 1; k < numBins; ++k) maxMag = juce::jmax (maxMag, mixAbsScratch[(size_t) k]);
        activeThresh = maxMag * 1.0e-3f;
        double sum = 0.0; int cnt = 0;
        for (int k = 1; k < numBins; ++k)
        {
            float a = mixAbsScratch[(size_t) k];
            if (a > activeThresh) { sum += a; ++cnt; }
        }
        shapeMean = (float) (sum / juce::jmax (1, cnt));
    }
    const float invShapeMean = 1.0f / juce::jmax (1.0e-9f, shapeMean);
    const float shapeX0 = std::log2 (juce::jmax (20.0f, p.shapeFreq));

    // --- per-hop scalars
    // loss -> decay multiplier per hop. loss=0 -> 1 (eternal), loss=1 -> fast.
    const float lossRate  = p.loss * (float) hopSize / (float) sampleRate * 6.0f;
    const float lossDecay = std::exp (-lossRate);
    // Injection scale: with the (now phase-coherent) feedback loop, feeding integrates.
    // Scaling by (1-lossDecay) makes the steady-state held level ~= feed * input level
    // instead of building up by 1/(1-lossDecay). Floored so capture still works at loss=0.
    // (Uses the un-localised rate: freshly-fed bins sit at the listener, att~1.)
    const float injScale = juce::jmax (kInjFloor, 1.0f - lossDecay);
    const float feed    = p.feed * injScale;
    const float trackW  = p.feed; // input's influence on the held *frequency* follows
                                  // Feed, so feed=0 fully freezes (no input phase leak)
    const float refFreq   = (float) sampleRate / (float) fftSize; // freq of bin 1
    const float twoPi     = juce::MathConstants<float>::twoPi;
    constexpr float kTrackThresh = 1.0e-3f;

    // Transpose (agent-wiki/plan-roadmap.md B3): pitch-shift the OUTPUT of the held sound
    // without touching the held state (S/omega stay untouched -- non-destructive). See the
    // per-layer transAcc accumulation below and the bin remap at the end of this loop.
    const float transRatio = std::exp2 (p.transpose / 12.0f);
    const bool  transposing = std::abs (p.transpose) > 1.0e-3f;
    if (transposing)
        std::fill (synthScratch.begin(), synthScratch.begin() + numBins, std::complex<float> {});

    // --- spectral update
    for (int k = 0; k < numBins; ++k)
    {
        // shaper L[k] in [-1..+1], computed once per bin from the listener mix. ratio=1
        // makes the Level-shape component a no-op (bin 0 and inactive/noise-floor bins,
        // mirroring the old Compress).
        float L = 0.0f;
        if (shaperActive && k > 0)
        {
            float ratio = 1.0f;
            if (shapeMean > 1.0e-9f)
            {
                float a = mixAbsScratch[(size_t) k];
                if (a > activeThresh)
                    ratio = juce::jlimit (0.01f, 100.0f, a * invShapeMean);
            }
            const float x0 = std::log2 (juce::jmax (20.0f, (float) k * refFreq));
            L = juce::jlimit (-1.0f, 1.0f,
                    ShapeCurves::shapeL (p.shape, x0, shapeX0, p.shapeWidth, p.shapeCount,
                                        p.shapeLevel, ratio)) * p.shapeAmt;
        }
        const bool hasL = std::abs (L) > 1.0e-9f;

        // momentary shaper / comp: bin-level, applied to the mixed output (unchanged from
        // the single-layer model -- it shapes what you hear, not any one layer).
        float gOut = 1.0f;
        if (hasL && p.shapeMode < 0.9999f)
        {
            const float tm = L * (1.0f - p.shapeMode);
            gOut = (tm >= 0.0f) ? std::exp (tm * kLn4)
                                 : (1.0f + tm) * (1.0f + tm); // smooth to 0 at tm = -1
        }
        const float comp = 1.0f / juce::jmax (kCompFloor, juce::jmin (1.0f, gOut));

        std::complex<float> x { fftData[(size_t) (2 * k)], fftData[(size_t) (2 * k + 1)] };
        const std::complex<float> inj = feed * x * comp;

        // Aux (reverb-feedback) injection: Feed-independent, uses the same injScale/comp
        // as the live path. See agent-wiki/plan-roadmap.md Part A / dsp-design.md.
        std::complex<float> injAux {};
        if (auxActive)
        {
            const std::complex<float> xa { auxFftData[(size_t) (2 * k)], auxFftData[(size_t) (2 * k + 1)] };
            injAux = p.revFeed * injScale * xa * comp;
        }
        const float aInj = std::abs (inj + injAux);

        // --- instantaneous-frequency tracking: measure the true per-hop phase advance of
        // the input (shared across layers -- a property of the analysis). Held at *that*
        // rate so leakage bins stay phase coherent and the freeze is smooth, not grainy.
        const float phi = std::arg (x);
        bool haveMeasured = false; float measured = 0.0f;
        if (trackW > 1.0e-4f && std::abs (x) > kTrackThresh)
        {
            float dev = (phi - prevPhase[(size_t) k]) - expectedAdv[(size_t) k];
            dev -= twoPi * std::round (dev / twoPi); // wrap to [-pi, pi]
            measured = expectedAdv[(size_t) k] + dev; // input's true advance/hop
            haveMeasured = true;
        }
        prevPhase[(size_t) k] = phi;
        Xs[(size_t) k] = x;

        // --- injection routing (agent-wiki/plan-loclayers.md §2): find the layer nearest
        // the knob at this bin (tie-break: whichever already holds energy here, then
        // lowest index -- keeps the knob-at-0 legacy case landing in layer 0 always).
        int nearestLayer = 0; float nearestAtt = -1.0f;
        for (int l = 0; l < kNumLayers; ++l)
        {
            const float al = ewAtt (std::abs (ewL - binLoc[li (l, k)]));
            if (al > nearestAtt + 1.0e-6f
                || (al > nearestAtt - 1.0e-6f
                    && std::abs (S[li (l, k)]) > std::abs (S[li (nearestLayer, k)])))
            {
                nearestAtt = al;
                nearestLayer = l;
            }
        }
        int injectLayer = nearestLayer;
        bool claimed = false;
        if (nearestAtt < kClaimAtt)
        {
            // near enough at NO layer: claim the quietest layer instead of dragging a far
            // one (which would smear a tone recorded elsewhere) -- leaves every other
            // layer's held content untouched.
            int quietest = 0; float qAbs = std::abs (S[li (0, k)]);
            for (int l = 1; l < kNumLayers; ++l)
            {
                const float a = std::abs (S[li (l, k)]);
                if (a < qAbs) { qAbs = a; quietest = l; }
            }
            injectLayer = quietest;
            claimed = true;
        }

        // --- per-layer update: permanent shaper, rotate, (routed) inject, localised decay
        std::complex<float> mixOut {};
        std::complex<float> mixOutT {}; // transposed-output accumulator (see "transposing" below)
        for (int l = 0; l < kNumLayers; ++l)
        {
            const size_t idx = li (l, k);
            const float locL = binLoc[idx];
            const float attL = ewAtt (std::abs (ewL - locL));

            // permanent shaper: reshape this layer's held state, scaled by its OWN
            // distance (a tone at the knob gets the full edit, a far one barely changes).
            // The bipolar shapes can BOOST, which would compound without bound; the boost
            // self-limits as a bin approaches kPermCeil (equilibrium there), cuts are
            // unbounded-down as before.
            if (hasL && p.shapeMode > 1.0e-4f)
            {
                float e = L * p.shapeMode * kPermScale * attL;
                if (e > 0.0f)
                {
                    const float permCeil = kPermCeilNorm * 0.5f * (float) fftSize; // |S| units
                    e *= juce::jmax (0.0f, 1.0f - std::abs (S[idx]) / permCeil);
                }
                S[idx] *= std::exp (e);
            }

            // Transpose (agent-wiki/plan-roadmap.md B3): accumulate an extra per-layer phase
            // offset so the OUTPUT advances at omega*ratio while the held state S/omega is
            // untouched (non-destructive). Only tracked while transposing to avoid drift
            // building up unnoticed at ratio=1.
            if (transposing)
            {
                transAcc[idx] += omega[idx] * (transRatio - 1.0f);
                transAcc[idx] -= twoPi * std::round (transAcc[idx] / twoPi); // wrap to [-pi,pi]
            }

            // free-run phasor at the tracked frequency (+ optional phase noise)
            float w = omega[idx];
            if (p.phaseNoise > 1.0e-4f)
                w += (rng.nextFloat() * 2.0f - 1.0f) * kPhaseNoiseMax * p.phaseNoise;
            std::complex<float> sk = S[idx] * std::polar (1.0f, w);

            if (l == injectLayer)
            {
                // frequency tracking follows Feed, targets only the receiving layer --
                // otherwise recording elsewhere would silently retune a far tone's pitch.
                if (haveMeasured)
                    omega[idx] += (measured - omega[idx]) * trackW;

                if (aInj > kInjLocFloor)
                {
                    // a claimed layer's stale residual (if any) is cleared first so the
                    // fresh recording doesn't phase-mush with old content; a dragged
                    // layer keeps integrating as before.
                    if (claimed && std::abs (sk) > kClaimClearFloor)
                        sk = {};
                    const float aHeld = std::abs (sk);
                    binLoc[idx] = claimed ? ewL
                                           : (aHeld * locL + aInj * ewL) / (aHeld + aInj);
                    sk += inj;
                    if (auxActive)
                    {
                        // feedback-loop safety: unlike live input, aux closes a real loop
                        // (engine -> reverb -> engine); soft-ceiling it the same way the
                        // permanent shaper's boost self-limits, so gain>1 loops saturate
                        // instead of blowing up. Live input stays uncapped as before.
                        const float permCeil = kPermCeilNorm * 0.5f * (float) fftSize;
                        const float g = juce::jmax (0.0f, 1.0f - std::abs (sk) / permCeil);
                        sk += injAux * g;
                    }
                }
            }

            // loss is localised: a layer at the knob decays at the set rate; a far one is
            // spared (decay -> 1). att==1 everywhere at knob==0 -> legacy single-buffer rate.
            const float decayL = juce::jmax (kDecayFloor, std::exp (-lossRate * attL));
            sk *= decayL;
            S[idx] = sk;

            // output: this layer's held tone heard from the listener position
            mixOut += sk * attL;
            if (transposing)
                mixOutT += sk * attL * std::polar (1.0f, transAcc[idx]);
        }

        // Stereo spread (agent-wiki/plan-roadmap.md B5): per-bin complementary channel
        // gain, deterministic hash of the bin index so L/R vary in a fixed, repeatable
        // pattern (gL^2+gR^2 = 2, equal-power). Output-only -- never feeds back into S.
        // Skipped entirely at spread<=0 so the default stays bit-exact.
        float spreadGain = 1.0f;
        if (p.spread > 1.0e-4f)
        {
            const uint32_t u = (uint32_t) k * 2654435761u;
            const float h = (float) ((u >> 16) & 0xFFFFu) / 32767.5f - 1.0f; // [-1,1]
            spreadGain = std::sqrt (juce::jmax (0.0f, 1.0f + (float) p.spreadSign * p.spread * h));
        }

        // momentary-shaped mix of all layers -- the audible output. Non-transposing path
        // is bit-exact unchanged (writes straight into fftData/dispScratch); transposing
        // path accumulates into synthScratch at the shifted bin and is copied out below.
        if (! transposing)
        {
            const std::complex<float> outBin = mixOut * gOut * spreadGain;
            fftData[(size_t) (2 * k)]     = outBin.real();
            fftData[(size_t) (2 * k + 1)] = outBin.imag();
            dispScratch[(size_t) k] = outBin;
        }
        else
        {
            const std::complex<float> outBinT = mixOutT * gOut * spreadGain;
            const int kPrime = (int) std::lround ((double) k * (double) transRatio);
            if (kPrime >= 1 && kPrime < numBins)
                synthScratch[(size_t) kPrime] += outBinT;
        }
    }

    if (transposing)
    {
        for (int k = 0; k < numBins; ++k)
        {
            fftData[(size_t) (2 * k)]     = synthScratch[(size_t) k].real();
            fftData[(size_t) (2 * k + 1)] = synthScratch[(size_t) k].imag();
            dispScratch[(size_t) k]       = synthScratch[(size_t) k];
        }
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

        // Location strip (agent-wiki/plan-roadmap.md B7): per-layer magnitude/location, same
        // norm as dispMag so the strip's alpha and the main view's brightness read the same.
        for (int l = 0; l < kNumLayers; ++l)
            for (int k = 0; k < numBins; ++k)
            {
                const size_t idx = li (l, k);
                dispLayerMag[idx] = std::abs (S[idx]) * norm;
                dispLayerLoc[idx] = binLoc[idx];
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

void SpectralEngine::drainBrush (const Params& p)
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
    const float ewL = juce::jlimit (0.0f, 1.0f, p.ewLocation);

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
            // gain = factor^(strength * w * rate), distance-weighted along E<->W so the
            // brush edits the tones near the listener and barely reaches far ones -- per
            // layer, each scaled by its own distance (agent-wiki/plan-loclayers.md §3).
            for (int l = 0; l < kNumLayers; ++l)
            {
                const size_t idx = li (l, k);
                const float att = ewAtt (std::abs (ewL - binLoc[idx]));
                const float g = std::exp (op.strength * w * kBrushRate * lnFactor * att);
                S[idx] *= g;
            }
        }
    }
    brushScratch.clear();
}

void SpectralEngine::applyHarmonize (const Params& p, int layer, bool isFirstLayer)
{
    // Harmonize runs independently per layer (agent-wiki/plan-loclayers.md §4): peaks,
    // drift, unison merge and migration all stay within one layer's S/omega/binLoc slice.
    // Cross-layer entrainment (tones held in different layers coupling to each other) is
    // a known limitation, not attempted here. The influence overlay snapshot is the UNION
    // across layers (isFirstLayer resets the running count; later layers append).

    // Harmonize is the master amount; Harmonic only blends the *character* (0 = averaging /
    // entrainment, 1 = harmonic attraction). So Harmonic does nothing while Harmonize is 0.
    if (p.harmonize <= 1.0e-4f)
    {
        if (isFirstLayer)
        {
            const juce::ScopedTryLock stl (displayLock);
            if (stl.isLocked()) dispPeakN = 0; // clear the influence overlay
        }
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

    // 1. extract significant spectral peaks (local maxima) within THIS layer
    float maxMag = 0.0f;
    for (int k = 1; k < numBins; ++k) maxMag = juce::jmax (maxMag, std::abs (S[li (layer, k)]));
    if (maxMag < 1.0e-9f) return;
    const float floor = maxMag * kPeakFloor;

    int P = 0;
    for (int k = 4; k < numBins - 4 && P < kMaxPeaks; ++k)
    {
        const float a = std::abs (S[li (layer, k)]);
        if (a <= floor) continue;
        // prominence: local max over +/-1 bin only (the floor rejects Hann sidelobes). A wider
        // window would suppress the quieter of two close tones, counting them as one peak so
        // they never entrain against each other; +/-1 resolves tones down to ~2 bins apart
        // while a single tone's mainlobe is still a single peak.
        bool isPeak = true;
        for (int o = -1; o <= 1 && isPeak; ++o)
            if (o != 0 && std::abs (S[li (layer, k + o)]) > a) isPeak = false;
        if (isPeak)
        {
            peakBin[(size_t) P]   = k;
            peakAmp[(size_t) P]   = a;
            peakFreq[(size_t) P]  = omega[li (layer, k)] * fScale;
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
            const float wi = std::abs (S[li (layer, bi)]), wj = std::abs (S[li (layer, bj)]);
            const float mo = (wi * omega[li (layer, bi)] + wj * omega[li (layer, bj)]) / (wi + wj + 1.0e-12f);
            const float mloc = (wi * binLoc[li (layer, bi)] + wj * binLoc[li (layer, bj)]) / (wi + wj + 1.0e-12f);
            const std::complex<float> combined = S[li (layer, bi)] + S[li (layer, bj)];
            const int target = juce::jlimit (1, numBins - 2, (int) std::lround (mo * fScale / binHz));

            for (int base : { bi, bj })
                for (int o = -kPad; o <= kPad; ++o)
                {
                    const int idx = base + o;
                    if (idx >= 1 && idx < numBins)
                    {
                        S[li (layer, idx)]     = std::complex<float> {};
                        omega[li (layer, idx)] = expectedAdv[(size_t) idx];
                    }
                }
            S[li (layer, target)]      = combined;
            omega[li (layer, target)]  = mo;
            binLoc[li (layer, target)] = mloc; // location merges like omega (amplitude-weighted)

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
        // distance-weighted: harmonize pulls hardest on tones near the listener position
        const float att       = ewAtt (std::abs (juce::jlimit (0.0f, 1.0f, p.ewLocation)
                                                 - binLoc[li (layer, peakBin[(size_t) i])]));
        const float df        = p.harmonize * kHarmRate * blended * att;

        peakDelta[(size_t) i] = juce::jlimit (-kHarmStep, kHarmStep, df / fScale); // -> rad/hop
    }

    // snapshot peaks for the influence overlay (freq, weight, drift in Hz): UNION across
    // layers -- isFirstLayer resets the running count, every layer's call appends.
    if (const juce::ScopedTryLock stl (displayLock); stl.isLocked())
    {
        if (isFirstLayer) dispPeakN = 0;
        const int n = juce::jmin (P, kMaxPeaks - dispPeakN);
        for (int i = 0; i < n; ++i)
        {
            dispPeakF[(size_t) (dispPeakN + i)] = peakFreq[(size_t) i];
            dispPeakA[(size_t) (dispPeakN + i)] = peakAmp[(size_t) i];
            dispPeakD[(size_t) (dispPeakN + i)] = peakDelta[(size_t) i] * fScale; // rad/hop -> Hz drift
        }
        dispPeakN += n;
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
            omega[li (layer, kk)] = juce::jlimit (lo, hi, omega[li (layer, kk)] + d);
        }
    }

    // 4. energy migration across bins. A single bin's phasor only represents a frequency
    // within ~half a bin of its centre. When a peak's centre drifts past half a bin, shift
    // its whole packet (centre +/- kPad bins) RIGIDLY by one bin, carrying each bin's complex
    // value, omega (frequency-absolute) and phase. Shifting the packet as a unit keeps the
    // peak coherent (moving bins independently tears it apart); omega is preserved so the
    // pitch is continuous, and the tone can travel any distance one bin at a time.
    // (prevPhase is NOT moved: it's shared analysis-only state -- see the class-level note
    // in SpectralEngine.h -- and gets unconditionally overwritten from the input every
    // frame's main loop regardless, so migrating it was already a no-op before this branch.)
    const float binW = juce::MathConstants<float>::twoPi * (float) hopSize / (float) fftSize;
    const float half = binW * 0.5f;
    for (int i = 0; i < P; ++i)
    {
        const int k = peakBin[(size_t) i];
        const float rel = omega[li (layer, k)] - expectedAdv[(size_t) k];
        const int s = (rel > half) ? +1 : (rel < -half) ? -1 : 0;
        if (s == 0) continue;
        if (k - kPad < 1 || k + kPad >= numBins - 1) continue; // near edges: don't migrate

        // collision avoidance: only skip when a neighbour is close enough that the packets
        // heavily overlap (would trample each other). Use kPad (not 2*kPad) so lightly
        // overlapping tones still re-centre instead of being stranded off their bin centre
        // (which leaves the energy and the actual pitch/marker visibly misaligned).
        bool nearNeighbour = false;
        for (int j = 0; j < P && ! nearNeighbour; ++j)
            if (j != i && std::abs (peakBin[(size_t) j] - k) <= kPad)
                nearNeighbour = true;
        if (nearNeighbour) continue;

        // move src -> src+s for the whole packet, ordered so we never overwrite a not-yet-moved
        // bin. loc travels with the packet (any code moving energy between bins moves loc too).
        if (s > 0)
            for (int b = k + kPad; b >= k - kPad; --b)
            {
                S[li (layer, b + 1)]     = S[li (layer, b)];
                omega[li (layer, b + 1)] = omega[li (layer, b)];
                binLoc[li (layer, b + 1)] = binLoc[li (layer, b)];
            }
        else
            for (int b = k - kPad; b <= k + kPad; ++b)
            {
                S[li (layer, b - 1)]     = S[li (layer, b)];
                omega[li (layer, b - 1)] = omega[li (layer, b)];
                binLoc[li (layer, b - 1)] = binLoc[li (layer, b)];
            }

        // clear the bin vacated at the trailing edge and neutralise its omega
        const int vac = (s > 0) ? (k - kPad) : (k + kPad);
        S[li (layer, vac)] = std::complex<float> {};
        omega[li (layer, vac)] = expectedAdv[(size_t) vac];
    }
}

// Hold serialization (agent-wiki/plan-roadmap.md B1): S/omega/binLoc for every layer, so
// the held sound can survive a session save/reload (or an undo snapshot, see B6). Format
// (all little-endian, via juce::OutputStream/InputStream): int32 version=1, int32 order,
// int32 kNumLayers, int32 numBins, then per layer: numBins*(re,im) floats for S, numBins
// floats for omega, numBins floats for binLoc.
void SpectralEngine::writeHold (juce::MemoryOutputStream& out) const
{
    out.writeInt (1); // version
    out.writeInt (order);
    out.writeInt (kNumLayers);
    out.writeInt (numBins);
    for (int l = 0; l < kNumLayers; ++l)
    {
        for (int k = 0; k < numBins; ++k)
        {
            const auto& s = S[li (l, k)];
            out.writeFloat (s.real());
            out.writeFloat (s.imag());
        }
        for (int k = 0; k < numBins; ++k)
            out.writeFloat (omega[li (l, k)]);
        for (int k = 0; k < numBins; ++k)
            out.writeFloat (binLoc[li (l, k)]);
    }
}

void SpectralEngine::queueHoldRestore (const void* data, size_t size)
{
    const juce::ScopedLock sl (holdLock);
    pendingHold.setSize (0);
    pendingHold.append (data, size);
    holdPending.store (true);
}

void SpectralEngine::applyPendingHold()
{
    if (! holdPending.load())
        return;

    juce::MemoryBlock blob;
    {
        const juce::ScopedTryLock stl (holdLock);
        if (! stl.isLocked())
            return; // never block the audio thread; try again next frame
        blob.swapWith (pendingHold);
        holdPending.store (false);
    }

    constexpr size_t kHeaderBytes = 4 * sizeof (int32_t);
    if (blob.getSize() < kHeaderBytes)
        return; // malformed/short: discard

    juce::MemoryInputStream in (blob, false);
    const int32_t version     = in.readInt();
    const int32_t blobOrder   = in.readInt();
    const int32_t blobLayers  = in.readInt();
    const int32_t blobBins    = in.readInt();

    if (version != 1 || blobLayers != kNumLayers || blobBins <= 0)
        return; // format mismatch: discard

    if (blobOrder != order)
    {
        // Order mismatch: if a fresh setOrder() is still queued (applied just before this
        // call, next frame), keep the blob pending for when it lands; otherwise it's stale
        // (or was never going to match), discard it.
        if (pendingOrder.load() >= 0)
        {
            const juce::ScopedLock sl (holdLock);
            pendingHold.swapWith (blob);
            holdPending.store (true);
        }
        return;
    }
    if (blobBins != numBins)
        return; // order matched but bin count didn't -- shouldn't happen; guard anyway

    const size_t perLayerBytes = (size_t) blobBins * 4 * sizeof (float); // re,im,omega,loc
    const size_t totalNeeded = kHeaderBytes + (size_t) blobLayers * perLayerBytes;
    if (blob.getSize() < totalNeeded)
        return; // malformed/short: discard

    for (int l = 0; l < blobLayers; ++l)
    {
        for (int k = 0; k < blobBins; ++k)
        {
            const float re = in.readFloat();
            const float im = in.readFloat();
            S[li (l, k)] = { re, im };
        }
        for (int k = 0; k < blobBins; ++k)
            omega[li (l, k)] = in.readFloat();
        for (int k = 0; k < blobBins; ++k)
            binLoc[li (l, k)] = in.readFloat();
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

int SpectralEngine::copyLayers (std::vector<float>& mag, std::vector<float>& loc)
{
    const juce::ScopedTryLock stl (displayLock);
    if (! stl.isLocked())
        return 0;

    // dispLayerMag/dispLayerLoc are stored with stride maxBins (li()'s layout); the caller
    // gets a tightly-packed stride-numBins copy instead (maxBins >= numBins always, and
    // differs whenever the FFT order isn't the max).
    mag.resize ((size_t) kNumLayers * (size_t) numBins);
    loc.resize ((size_t) kNumLayers * (size_t) numBins);
    for (int l = 0; l < kNumLayers; ++l)
    {
        std::copy (dispLayerMag.begin() + li (l, 0), dispLayerMag.begin() + li (l, 0) + numBins,
                   mag.begin() + (size_t) l * (size_t) numBins);
        std::copy (dispLayerLoc.begin() + li (l, 0), dispLayerLoc.begin() + li (l, 0) + numBins,
                   loc.begin() + (size_t) l * (size_t) numBins);
    }
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

