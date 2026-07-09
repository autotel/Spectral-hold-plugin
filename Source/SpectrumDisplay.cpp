#include "SpectrumDisplay.h"
#include "PluginProcessor.h"
#include "ShapeCurves.h"

SpectrumDisplay::SpectrumDisplay (SpectralHoldProcessor& p) : proc (p)
{
    pShapeAmt   = proc.apvts.getRawParameterValue ("shapeAmt");
    pShapeMode  = proc.apvts.getRawParameterValue ("shapeMode");
    pShape      = proc.apvts.getRawParameterValue ("shape");
    pShapeFreq  = proc.apvts.getRawParameterValue ("shapeFreq");
    pShapeWidth = proc.apvts.getRawParameterValue ("shapeWidth");
    pShapeCount = proc.apvts.getRawParameterValue ("shapeCount");
    pShapeLevel = proc.apvts.getRawParameterValue ("shapeLevel");
    pHarm      = proc.apvts.getRawParameterValue ("harmonize");
    pHarmWidth = proc.apvts.getRawParameterValue ("harmWidth");
    pEwLocation = proc.apvts.getRawParameterValue ("ewLocation");
    setMouseCursor (juce::MouseCursor::NoCursor); // we draw our own brush cursor
    startTimerHz (30);
}

SpectrumDisplay::~SpectrumDisplay() { stopTimer(); }

// --- coordinate helpers ----------------------------------------------------
float SpectrumDisplay::xToFreq (float x) const
{
    const float logMin = std::log (kMinHz);
    const float logMax = std::log (juce::jmin (kMaxHz, (float) sampleRate * 0.5f));
    const float t = juce::jlimit (0.0f, 1.0f, x / juce::jmax (1.0f, (float) getWidth() - 1.0f));
    return std::exp (logMin + t * (logMax - logMin));
}

float SpectrumDisplay::freqToX (float freq) const
{
    const float logMin = std::log (kMinHz);
    const float logMax = std::log (juce::jmin (kMaxHz, (float) sampleRate * 0.5f));
    const float t = (std::log (juce::jlimit (kMinHz, kMaxHz, freq)) - logMin) / (logMax - logMin);
    return t * ((float) getWidth() - 1.0f);
}

float SpectrumDisplay::yToStrength (float y) const
{
    const float c = (float) getHeight() * 0.5f;          // centre line
    return juce::jlimit (-1.0f, 1.0f, (c - y) / juce::jmax (1.0f, c)); // top=+1, bottom=-1
}

// --- editing ---------------------------------------------------------------
// The brush is TIME-based: holding the button at a point keeps reshaping the wave (applied
// each timer tick in timerCallback), so you don't have to keep moving the pointer. Mouse
// events only update the target position/strength; they don't apply on their own.
void SpectrumDisplay::updateBrushTarget (const juce::MouseEvent& e)
{
    mousePos = e.position;
    mouseInside = true;
    brushStrength = yToStrength (e.position.y);
    brushPressure = e.source.isPressureValid() ? juce::jlimit (0.05f, 1.0f, e.pressure) : 1.0f;
}

void SpectrumDisplay::mouseDown (const juce::MouseEvent& e)
{
    // Undo (agent-wiki/plan-roadmap.md B6): snapshot before the first edit of a stroke,
    // not on every drag tick -- one snapshot per stroke, so Undo reverts the whole gesture.
    proc.snapshotHold();
    brushHeld = true;
    updateBrushTarget (e);
    repaint();
}
void SpectrumDisplay::mouseDrag (const juce::MouseEvent& e) { updateBrushTarget (e); repaint(); }
void SpectrumDisplay::mouseUp   (const juce::MouseEvent&)   { brushHeld = false; repaint(); }

void SpectrumDisplay::mouseMove (const juce::MouseEvent& e)
{
    mousePos = e.position;
    mouseInside = true;
    brushStrength = yToStrength (e.position.y); // preview only, no edit on hover
    repaint();
}

void SpectrumDisplay::mouseEnter (const juce::MouseEvent& e)
{
    mouseInside = true;
    mousePos = e.position;
    repaint();
}

void SpectrumDisplay::mouseExit (const juce::MouseEvent&)
{
    mouseInside = false;
    repaint();
}

// --- snapshot --------------------------------------------------------------
void SpectrumDisplay::timerCallback()
{
    // time-based brush: while the button is held, keep applying at a steady rate
    if (brushHeld)
        proc.applySpectralBrush (xToFreq (mousePos.x), brushStrength * brushPressure, brushSigmaOct);

    double sr = sampleRate;
    int    sz = fftSize;
    int n = proc.getDisplaySnapshot (magL, phaseL, magR, phaseR, sr, sz);
    if (n <= 0)
        return; // engine busy; keep last frame

    sampleRate = sr;
    fftSize    = sz;

    if (smoothMagL.size() != magL.size())
        smoothMagL.assign (magL.size(), 0.0f);
    if (smoothMagR.size() != magR.size())
        smoothMagR.assign (magR.size(), 0.0f);

    // attack-fast / release-slow visual envelope, per channel
    for (size_t i = 0; i < magL.size(); ++i)
    {
        float m = magL[i];
        float& s = smoothMagL[i];
        s += (m - s) * (m > s ? 0.6f : 0.15f);
    }
    for (size_t i = 0; i < magR.size(); ++i)
    {
        float m = magR[i];
        float& s = smoothMagR[i];
        s += (m - s) * (m > s ? 0.6f : 0.15f);
    }
    // harmonize influence peaks (keep last frame if busy)
    int np = proc.getHarmonizePeaks (peakFreq, peakWeight, peakDrift);
    if (np >= 0)
        peakCount = np;

    // location strip (agent-wiki/plan-roadmap.md B7): keep last frame if busy, same as above
    int nLoc = proc.getLocationSnapshot (layerMag, layerLoc);
    if (nLoc > 0)
        locBins = nLoc;

    repaint();
}

// --- paint -----------------------------------------------------------------
void SpectrumDisplay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    const int W = getWidth();
    const int fullH = getHeight();
    // Reserve the location strip's band at the bottom (agent-wiki/plan-roadmap.md B7); H is
    // used for every main-view vertical extent below, so shrinking it here confines the
    // whole "lighting" view + overlays to the space above the strip, unchanged otherwise.
    const int H = juce::jmax (0, fullH - (int) kLocStripH);
    if (smoothMagL.empty() || fftSize <= 0 || W <= 0 || H <= 0)
        return;

    const float binToHz  = (float) sampleRate / (float) fftSize;
    const int   numBins  = (int) smoothMagL.size();
    const float logMin   = std::log (kMinHz);
    const float logMax   = std::log (juce::jmin (kMaxHz, (float) sampleRate * 0.5f));

    auto levelToBright = [] (float m)
    {
        float db = juce::Decibels::gainToDecibels (m + 1.0e-6f, -100.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f);
    };

    // --- spectrum "lighting" lines -------------------------------------------
    // Split stereo (agent-wiki/plan-uifix.md U1): left channel's gradient projects UP from
    // centre, right channel's DOWN. Skip the column only when BOTH channels are quiet, so a
    // tone alive in only one channel still draws. Identical L/R (mono, or just no stereo
    // difference right now) makes the two halves mirror into the old symmetric spike.
    for (int x = 0; x < W; ++x)
    {
        float t    = (float) x / (float) (W - 1);
        float freq = std::exp (logMin + t * (logMax - logMin));
        float fbin = freq / binToHz;
        if (fbin < 1.0f || fbin >= (float) (numBins - 1))
            continue;

        int   k  = (int) fbin;
        float fr = fbin - (float) k;
        float mL  = smoothMagL[(size_t) k] * (1.0f - fr) + smoothMagL[(size_t) (k + 1)] * fr;
        float mR  = smoothMagR[(size_t) k] * (1.0f - fr) + smoothMagR[(size_t) (k + 1)] * fr;
        float phL2 = phaseL.empty() ? 0.0f : phaseL[(size_t) k];
        float phR2 = phaseR.empty() ? 0.0f : phaseR[(size_t) k];

        float brightL = levelToBright (mL);
        float brightR = levelToBright (mR);
        if (brightL <= 0.01f && brightR <= 0.01f)
            continue;

        auto hueOf = [] (float ph)
        {
            float pt = (ph + juce::MathConstants<float>::pi) * (0.5f / juce::MathConstants<float>::pi);
            return 0.72f - 0.24f * pt;
        };
        auto baseL = juce::Colour::fromHSV (hueOf (phL2), 0.25f, 1.0f, 1.0f);
        auto baseR = juce::Colour::fromHSV (hueOf (phR2), 0.25f, 1.0f, 1.0f);

        juce::ColourGradient grad (juce::Colours::transparentBlack, (float) x, 0.0f,
                                   juce::Colours::transparentBlack, (float) x, (float) H, false);
        grad.addColour (0.28,  baseL.withAlpha (brightL * 0.35f));
        grad.addColour (0.49,  baseL.withAlpha (brightL));
        grad.addColour (0.51,  baseR.withAlpha (brightR));
        grad.addColour (0.72,  baseR.withAlpha (brightR * 0.35f));
        g.setGradientFill (grad);
        g.fillRect ((float) x, 0.0f, 1.0f, (float) H);
    }

    // --- shaper curve overlay (fades out as amount -> 0, no hard toggle) ----
    const float shAmt   = pShapeAmt   != nullptr ? pShapeAmt->load()   : 0.0f;
    const float shMode  = pShapeMode  != nullptr ? pShapeMode->load()  : 0.0f;
    const float shShape = pShape      != nullptr ? pShape->load()     : 0.0f;
    const float shFreq  = pShapeFreq  != nullptr ? pShapeFreq->load()  : 1000.0f;
    const float shWidth = pShapeWidth != nullptr ? pShapeWidth->load() : 0.5f;
    const float shCount = pShapeCount != nullptr ? pShapeCount->load() : 1.0f;
    const float shLevel = pShapeLevel != nullptr ? pShapeLevel->load() : 0.0f;
    // ramp alpha over [0 .. kFadeRange] instead of popping in/out at a fixed threshold
    constexpr float kFadeRange = 0.15f;
    const float fadeAlpha = juce::jlimit (0.0f, 1.0f, shAmt / kFadeRange);
    if (fadeAlpha > 0.001f)
    {
        // Level shape needs a rough pivot mean, approximated from the (already smoothed)
        // display magnitudes -- exact match isn't required, this is a preview only.
        float shMean = 0.0f;
        if (! smoothMagL.empty())
        {
            float maxMag = 0.0f;
            for (float mg : smoothMagL) maxMag = juce::jmax (maxMag, mg);
            const float activeThresh = maxMag * 1.0e-3f;
            double sum = 0.0; int cnt = 0;
            for (float mg : smoothMagL) if (mg > activeThresh) { sum += (double) mg; ++cnt; }
            shMean = (float) (sum / juce::jmax (1, cnt));
        }
        const float invShMean = 1.0f / juce::jmax (1.0e-9f, shMean);
        const float x0 = std::log2 (juce::jmax (20.0f, shFreq));

        auto gToY = [H] (float gain)
        {
            return (1.0f - juce::jlimit (0.0f, 2.0f, gain) * 0.5f) * (float) H;
        };

        juce::Path curve;
        for (int x = 0; x < W; ++x)
        {
            float t    = (float) x / (float) (W - 1);
            float freq = std::exp (logMin + t * (logMax - logMin));
            float fbin = freq / binToHz;

            float ratio = 1.0f;
            if (shMean > 1.0e-9f && fbin >= 0.0f && fbin < (float) numBins)
            {
                int   k  = juce::jlimit (0, numBins - 1, (int) fbin);
                float mg = smoothMagL[(size_t) k];
                if (mg > shMean * 1.0e-3f)
                    ratio = juce::jlimit (0.01f, 100.0f, mg * invShMean);
            }

            const float bx = std::log2 (juce::jmax (20.0f, freq));
            const float L  = juce::jlimit (-1.0f, 1.0f,
                    ShapeCurves::shapeL (shShape, bx, x0, shWidth, shCount, shLevel, ratio)) * shAmt;
            const float tm = L * (1.0f - shMode);
            const float gOut = (tm >= 0.0f) ? std::exp (tm * 1.386294361f)
                                             : (1.0f + tm) * (1.0f + tm);
            float y = gToY (gOut);
            if (x == 0) curve.startNewSubPath ((float) x, y);
            else        curve.lineTo ((float) x, y);
        }
        g.setColour (juce::Colour::fromHSV (0.13f, 0.55f, 1.0f, 0.85f * fadeAlpha));
        g.strokePath (curve, juce::PathStrokeType (1.5f));
    }

    // --- harmonize influence overlay ---------------------------------------
    const float harmAmt = pHarm != nullptr ? pHarm->load() : 0.0f;
    if (harmAmt > 0.001f && peakCount > 0)
    {
        const float sigma = pHarmWidth != nullptr ? pHarmWidth->load() : 0.5f;
        const float invS2 = 1.0f / (2.0f * juce::jmax (0.01f, sigma) * sigma);
        const float band  = (float) H * 0.30f;       // influence humps live in the top band
        float maxW = 1.0e-9f;
        for (int i = 0; i < peakCount; ++i) maxW = juce::jmax (maxW, peakWeight[(size_t) i]);

        const juce::Colour acc = juce::Colour::fromHSV (0.52f, 0.55f, 1.0f, 1.0f); // aurora blue

        for (int i = 0; i < peakCount; ++i)
        {
            const float f0 = peakFreq[(size_t) i];
            if (f0 < kMinHz || f0 > kMaxHz) continue;
            const float oct0 = std::log2 (f0);
            const float wn   = peakWeight[(size_t) i] / maxW; // 0..1 weight = pull strength

            // gaussian influence hump (its width = the Width knob)
            juce::Path hump;
            hump.startNewSubPath (0.0f, band);
            for (int x = 0; x < W; ++x)
            {
                float oct = std::log2 (juce::jlimit (kMinHz, kMaxHz, xToFreq ((float) x)));
                float d   = oct - oct0;
                float v   = wn * std::exp (-d * d * invS2);
                hump.lineTo ((float) x, band - v * band);
            }
            hump.lineTo ((float) (W - 1), band);
            g.setColour (acc.withAlpha (0.06f + 0.10f * wn));
            g.fillPath (hump);

            // peak marker + pull-direction arrow (right = pitch rising, left = falling)
            const float px = freqToX (f0);
            g.setColour (acc.withAlpha (0.25f + 0.4f * wn));
            g.drawVerticalLine ((int) px, 0.0f, (float) H);

            const float drift = peakDrift[(size_t) i];               // Hz/frame, signed
            const float len   = juce::jlimit (0.0f, 26.0f, std::abs (drift) * 3.0f);
            if (len > 1.5f)
            {
                const float dir = drift >= 0.0f ? 1.0f : -1.0f;
                const float ay  = band * 0.45f;
                const float x0  = px, x1 = px + dir * len;
                g.setColour (acc.withAlpha (0.95f));
                g.drawArrow ({ x0, ay, x1, ay }, 2.0f, 7.0f, 7.0f);
            }
        }
    }

    // --- brush cursor ------------------------------------------------------
    if (mouseInside)
    {
        const float sig    = brushSigmaOct;                       // octaves (matches DSP)
        const float invS2  = 1.0f / (2.0f * sig * sig);
        const float centreOct = std::log2 (juce::jmax (20.0f, xToFreq (mousePos.x)));
        const float mag01  = std::abs (brushStrength);
        // colour: boost -> aurora blue, cut -> red
        const bool  boost  = brushStrength >= 0.0f;
        const float hue    = boost ? 0.52f : 0.02f;   // aurora cyan-blue / red
        const float sat    = boost ? 0.55f : 0.7f;
        const auto  col    = juce::Colour::fromHSV (hue, sat, 1.0f, 1.0f);

        for (int x = 0; x < W; ++x)
        {
            float oct = std::log2 (juce::jmax (20.0f, xToFreq ((float) x)));
            float d   = oct - centreOct;
            float w   = std::exp (-(d * d) * invS2);            // blurred-brush falloff
            if (w < 0.02f)
                continue;
            float a = w * (0.12f + 0.6f * mag01);
            juce::ColourGradient grad (juce::Colours::transparentBlack, (float) x, 0.0f,
                                       juce::Colours::transparentBlack, (float) x, (float) H, false);
            grad.addColour (0.5,  col.withAlpha (a));
            grad.addColour (0.2,  col.withAlpha (a * 0.4f));
            grad.addColour (0.8,  col.withAlpha (a * 0.4f));
            g.setGradientFill (grad);
            g.fillRect ((float) x, 0.0f, 1.0f, (float) H);
        }

        // direction marker at the cursor height
        g.setColour (col.withAlpha (0.9f));
        g.fillEllipse (mousePos.x - 3.0f, mousePos.y - 3.0f, 6.0f, 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.15f));
        g.drawHorizontalLine ((int) ((float) H * 0.5f), 0.0f, (float) W); // centre = no change
    }

    // --- location strip (agent-wiki/plan-roadmap.md B7): the E<->W field, otherwise
    // invisible. Shares the main view's log-frequency x-mapping (xToFreq/freqToX depend
    // only on width, not on the H/fullH split above). y: East (loc=0) at the strip's
    // bottom, West (loc=1) at its top -- matches ewLocation's 0=East/1=West convention.
    if (locBins > 0 && ! layerMag.empty())
    {
        const int layers = (int) (layerMag.size() / (size_t) locBins);
        const float stripBottom = (float) fullH;

        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRect (0.0f, (float) H, (float) W, kLocStripH);
        g.setColour (juce::Colours::white.withAlpha (0.15f));
        g.drawHorizontalLine (H, 0.0f, (float) W);

        // hue by layer (not phase -- copyLayers doesn't carry it), same 0.48-0.72 family
        // the main view's phase-hue uses, so the strip reads as part of the same palette.
        for (int l = 0; l < layers; ++l)
        {
            const float hue = 0.72f - 0.24f * (layers > 1 ? (float) l / (float) (layers - 1) : 0.0f);
            const auto  base = juce::Colour::fromHSV (hue, 0.55f, 1.0f, 1.0f);
            for (int k = 1; k < locBins; ++k)
            {
                const float m = layerMag[(size_t) (l * locBins + k)];
                if (m <= 1.0e-4f)
                    continue;
                const float freq = (float) k * binToHz;
                if (freq < kMinHz || freq > kMaxHz)
                    continue;
                const float loc   = layerLoc[(size_t) (l * locBins + k)];
                const float x     = freqToX (freq);
                const float y     = stripBottom - loc * kLocStripH;
                const float alpha = juce::jlimit (0.0f, 1.0f, std::sqrt (m));
                g.setColour (base.withAlpha (alpha));
                g.fillEllipse (x - 1.5f, y - 1.5f, 3.0f, 3.0f);
            }
        }

        // marker line at the ewLocation knob (the listener position)
        if (pEwLocation != nullptr)
        {
            const float ewL = juce::jlimit (0.0f, 1.0f, pEwLocation->load());
            const float my  = stripBottom - ewL * kLocStripH;
            g.setColour (juce::Colours::white.withAlpha (0.4f));
            g.drawHorizontalLine ((int) my, 0.0f, (float) W);
        }
    }
}
