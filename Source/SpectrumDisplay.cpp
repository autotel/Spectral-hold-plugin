#include "SpectrumDisplay.h"
#include "PluginProcessor.h"

SpectrumDisplay::SpectrumDisplay (SpectralHoldProcessor& p) : proc (p)
{
    pFiltAmt  = proc.apvts.getRawParameterValue ("filterAmt");
    pFiltTone = proc.apvts.getRawParameterValue ("filterTone");
    pHarm      = proc.apvts.getRawParameterValue ("harmonize");
    pHarmWidth = proc.apvts.getRawParameterValue ("harmWidth");
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

void SpectrumDisplay::mouseDown (const juce::MouseEvent& e) { brushHeld = true;  updateBrushTarget (e); repaint(); }
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
    int n = proc.getDisplaySnapshot (mag, phase, sr, sz);
    if (n <= 0)
        return; // engine busy; keep last frame

    sampleRate = sr;
    fftSize    = sz;

    if (smoothMag.size() != mag.size())
        smoothMag.assign (mag.size(), 0.0f);

    // attack-fast / release-slow visual envelope
    for (size_t i = 0; i < mag.size(); ++i)
    {
        float m = mag[i];
        float& s = smoothMag[i];
        s += (m - s) * (m > s ? 0.6f : 0.15f);
    }
    // harmonize influence peaks (keep last frame if busy)
    int np = proc.getHarmonizePeaks (peakFreq, peakWeight, peakDrift);
    if (np >= 0)
        peakCount = np;

    repaint();
}

// --- paint -----------------------------------------------------------------
void SpectrumDisplay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    const int W = getWidth();
    const int H = getHeight();
    if (smoothMag.empty() || fftSize <= 0 || W <= 0 || H <= 0)
        return;

    const float binToHz  = (float) sampleRate / (float) fftSize;
    const int   numBins  = (int) smoothMag.size();
    const float logMin   = std::log (kMinHz);
    const float logMax   = std::log (juce::jmin (kMaxHz, (float) sampleRate * 0.5f));

    auto levelToBright = [] (float m)
    {
        float db = juce::Decibels::gainToDecibels (m + 1.0e-6f, -100.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f);
    };

    // --- spectrum "lighting" lines -----------------------------------------
    for (int x = 0; x < W; ++x)
    {
        float t    = (float) x / (float) (W - 1);
        float freq = std::exp (logMin + t * (logMax - logMin));
        float fbin = freq / binToHz;
        if (fbin < 1.0f || fbin >= (float) (numBins - 1))
            continue;

        int   k  = (int) fbin;
        float fr = fbin - (float) k;
        float m  = smoothMag[(size_t) k] * (1.0f - fr) + smoothMag[(size_t) (k + 1)] * fr;
        float ph = phase.empty() ? 0.0f : phase[(size_t) k];

        float bright = levelToBright (m);
        if (bright <= 0.01f)
            continue;

        float pt  = (ph + juce::MathConstants<float>::pi) * (0.5f / juce::MathConstants<float>::pi);
        float hue = 0.72f - 0.24f * pt;
        auto base = juce::Colour::fromHSV (hue, 0.25f, 1.0f, 1.0f);

        juce::ColourGradient grad (juce::Colours::transparentBlack, (float) x, 0.0f,
                                   juce::Colours::transparentBlack, (float) x, (float) H, false);
        grad.addColour (0.5,  base.withAlpha (bright));
        grad.addColour (0.28, base.withAlpha (bright * 0.35f));
        grad.addColour (0.72, base.withAlpha (bright * 0.35f));
        g.setGradientFill (grad);
        g.fillRect ((float) x, 0.0f, 1.0f, (float) H);
    }

    // --- filter curve overlay (only when filter amount > 0) ----------------
    const float fAmt  = pFiltAmt  != nullptr ? pFiltAmt->load()  : 0.0f;
    const float fTone = pFiltTone != nullptr ? pFiltTone->load() : 1000.0f;
    if (fAmt > 0.001f)
    {
        juce::Path curve;
        for (int x = 0; x < W; ++x)
        {
            float t    = (float) x / (float) (W - 1);
            float freq = std::exp (logMin + t * (logMax - logMin));
            float gain = SpectralEngine::filterGain (freq, fTone, fAmt);
            float y    = (1.0f - gain) * (float) H;
            if (x == 0) curve.startNewSubPath ((float) x, y);
            else        curve.lineTo ((float) x, y);
        }
        g.setColour (juce::Colour::fromHSV (0.13f, 0.55f, 1.0f, 0.85f));
        g.strokePath (curve, juce::PathStrokeType (1.5f));
    }

    // --- harmonize influence overlay ---------------------------------------
    const float harmAmt = pHarm != nullptr ? pHarm->load() : 0.0f;
    if (harmAmt > 0.001f && peakCount > 0)
    {
        const float sigma = pHarmWidth != nullptr ? pHarmWidth->load() : 0.5f;
        const float invS2 = 1.0f / (2.0f * juce::jmax (0.05f, sigma) * sigma);
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
}
