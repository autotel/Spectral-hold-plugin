#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

class SpectralHoldProcessor;

// The spectral "lighting" view. Not a classic bin-height spectrogram:
//   x      = tone, logarithmic (low -> high left to right)
//   level  = opacity of a white vertical line at that x
//   line   = vertical gradient, brightest at centre, fading to black top & bottom
//   tint   = slight hue from phase (purple-blue -> green-blue)
class SpectrumDisplay : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumDisplay (SpectralHoldProcessor& p);
    ~SpectrumDisplay() override;

    void paint (juce::Graphics&) override;

    // Editing: drag (mouse or pen) over the view to reshape the held spectrum.
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit  (const juce::MouseEvent&) override;
    void mouseMove  (const juce::MouseEvent&) override;
    void mouseDown  (const juce::MouseEvent&) override;
    void mouseDrag  (const juce::MouseEvent&) override;
    void mouseUp    (const juce::MouseEvent&) override;

    // Brush size (GUI-only), gaussian half-width in octaves. Set from the editor.
    void setBrushSigmaOct (float s) { brushSigmaOct = juce::jmax (0.05f, s); repaint(); }

    // Which module overlay to draw, following the editor's active tab (view-only):
    // 0 = shaper gain curve, 1 = harmonize influence, 2 = none (reverb has no overlay).
    void setOverlayMode (int m) { overlayMode = m; repaint(); }

private:
    void timerCallback() override;

    float xToFreq (float x) const;       // pixel -> frequency (log)
    float freqToX (float freq) const;    // frequency -> pixel (log)
    float yToStrength (float y) const;   // pixel -> [-1..+1] (top=+1, centre=0, bottom=-1)
    void  updateBrushTarget (const juce::MouseEvent&);

    SpectralHoldProcessor& proc;

    std::vector<float> mag, phase;     // raw snapshot from the processor
    std::vector<float> smoothMag;      // visual smoothing for less flicker
    double sampleRate = 44100.0;
    int    fftSize = 0;

    // shaper curve overlay (drawn only when amount * |level| > 0)
    std::atomic<float>* pShapeAmt   = nullptr;
    std::atomic<float>* pShapeMode  = nullptr;
    std::atomic<float>* pShape      = nullptr;
    std::atomic<float>* pShapeFreq  = nullptr;
    std::atomic<float>* pShapeWidth = nullptr;
    std::atomic<float>* pShapeCount = nullptr;
    std::atomic<float>* pShapeLevel = nullptr;

    // harmonize influence overlay
    std::atomic<float>* pHarm      = nullptr;
    std::atomic<float>* pHarmWidth = nullptr;
    std::vector<float> peakFreq, peakWeight, peakDrift;
    int peakCount = 0;

    int overlayMode = 0; // see setOverlayMode

    // brush cursor state (message thread / paint only)
    juce::Point<float> mousePos;
    bool  mouseInside = false;
    bool  brushHeld = false;    // button down -> apply continuously (time-based) via the timer
    float brushStrength = 0.0f; // current vertical strength under the cursor
    float brushPressure = 1.0f; // pen pressure (1 for mouse)
    float brushSigmaOct = 0.6f; // brush size (octaves), GUI-only

    static constexpr float kMinHz = 20.0f;
    static constexpr float kMaxHz = 20000.0f;

    JUCE_LEAK_DETECTOR (SpectrumDisplay)
};
