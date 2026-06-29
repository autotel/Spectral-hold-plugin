#include "SpectrumDisplay.h"
#include "PluginProcessor.h"

SpectrumDisplay::SpectrumDisplay (SpectralHoldProcessor& p) : proc (p)
{
    pFiltAmt  = proc.apvts.getRawParameterValue ("filterAmt");
    pFiltTone = proc.apvts.getRawParameterValue ("filterTone");
    startTimerHz (30);
}

SpectrumDisplay::~SpectrumDisplay() { stopTimer(); }

void SpectrumDisplay::timerCallback()
{
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
    repaint();
}

void SpectrumDisplay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    const int W = getWidth();
    const int H = getHeight();
    if (smoothMag.empty() || fftSize <= 0 || W <= 0 || H <= 0)
        return;

    const float midY    = (float) H * 0.5f;
    const float binToHz  = (float) sampleRate / (float) fftSize;
    const int   numBins  = (int) smoothMag.size();
    const float logMin   = std::log (kMinHz);
    const float logMax   = std::log (juce::jmin (kMaxHz, (float) sampleRate * 0.5f));

    // dB-ish mapping of magnitude -> brightness
    auto levelToBright = [] (float m)
    {
        float db = juce::Decibels::gainToDecibels (m + 1.0e-6f, -100.0f); // [-100..]
        return juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f);           // -80..0 dB -> 0..1
    };

    for (int x = 0; x < W; ++x)
    {
        // pixel -> log frequency -> fractional bin (with interpolation)
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

        // hue from phase: purple-blue (~0.72) -> green-blue (~0.48), very low saturation
        float pt  = (ph + juce::MathConstants<float>::pi) * (0.5f / juce::MathConstants<float>::pi);
        float hue = 0.72f - 0.24f * pt;
        float sat = 0.25f;
        auto base = juce::Colour::fromHSV (hue, sat, 1.0f, 1.0f);

        // vertical gradient: black -> colour (centre) -> black, alpha scaled by level
        juce::ColourGradient grad (juce::Colours::transparentBlack, (float) x, 0.0f,
                                   juce::Colours::transparentBlack, (float) x, (float) H, false);
        grad.addColour (0.5, base.withAlpha (bright));
        // soft shoulders so the centre glows and the ends fade out
        grad.addColour (0.28, base.withAlpha (bright * 0.35f));
        grad.addColour (0.72, base.withAlpha (bright * 0.35f));

        g.setGradientFill (grad);
        g.fillRect ((float) x, 0.0f, 1.0f, (float) H);
    }

    // --- filter curve overlay (only when filter amount > 0) -----------------
    const float fAmt  = pFiltAmt  != nullptr ? pFiltAmt->load()  : 0.0f;
    const float fTone = pFiltTone != nullptr ? pFiltTone->load() : 1000.0f;
    if (fAmt > 0.001f)
    {
        juce::Path curve;
        for (int x = 0; x < W; ++x)
        {
            float t    = (float) x / (float) (W - 1);
            float freq = std::exp (logMin + t * (logMax - logMin));
            float gain = SpectralEngine::filterGain (freq, fTone, fAmt); // [1-amt .. 1]
            float y    = (1.0f - gain) * (float) H;                      // 1 -> top, 0 -> bottom
            if (x == 0) curve.startNewSubPath ((float) x, y);
            else        curve.lineTo ((float) x, y);
        }
        auto c = juce::Colour::fromHSV (0.13f, 0.55f, 1.0f, 0.85f); // soft amber
        g.setColour (c);
        g.strokePath (curve, juce::PathStrokeType (1.5f));

        // mark the bell centre
        float ct  = (std::log (juce::jlimit (kMinHz, kMaxHz, fTone)) - logMin) / (logMax - logMin);
        float cx  = ct * (float) (W - 1);
        g.setColour (c.withAlpha (0.25f));
        g.drawVerticalLine ((int) cx, 0.0f, (float) H);
    }

    juce::ignoreUnused (midY);
}
