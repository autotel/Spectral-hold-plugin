#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

class SpectralHoldProcessor;

// The spectral "lighting" view. Not a classic bin-height spectrogram:
//   x      = tone, logarithmic (low -> high left to right)
//   level  = opacity of a white vertical line at that x
//   line   = vertical gradient, brightest at centre, fading to black top & bottom
//   tint   = slight hue from phase (purple-blue -> green-blue)
// Stereo (agent-wiki/plan-uifix.md U1): left channel's gradient projects UP from centre,
// right channel's projects DOWN. Identical channels render the old symmetric spike; the
// split is only visible when L and R actually differ (real stereo input, Phase Noise,
// Spread).
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

private:
    void timerCallback() override;

    float xToFreq (float x) const;       // pixel -> frequency (log)
    float freqToX (float freq) const;    // frequency -> pixel (log)
    float yToStrength (float y) const;   // pixel -> [-1..+1] (top=+1, centre=0, bottom=-1)
    void  updateBrushTarget (const juce::MouseEvent&);

    SpectralHoldProcessor& proc;

    // Split stereo (agent-wiki/plan-uifix.md U1): each channel's own raw snapshot + visual
    // smoothing, no merge. See getDisplaySnapshot().
    std::vector<float> magL, phaseL, magR, phaseR;
    std::vector<float> smoothMagL, smoothMagR;
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

    // Location strip (agent-wiki/plan-roadmap.md B7): shows the E<->W field, invisible
    // otherwise. layerMag/layerLoc are flat, layer-major, stride locBins (see
    // SpectralEngine::copyLayers); the layer count is inferred from the ratio, since the
    // engine's kNumLayers isn't exposed to the GUI.
    std::vector<float> layerMag, layerLoc;
    int locBins = 0;
    static constexpr float kLocStripH = 40.0f; // px, reserved at the bottom of the display

    // Injection ticks in the strip (agent-wiki/plan-uifix.md U2): raw per-frame snapshot
    // (injLoc/injStrength/injDrag) plus a slow-release visual smoothing (smoothInj) so a
    // brief recording event stays readable instead of flickering for one 30Hz frame.
    std::vector<float> injLoc, injStrength, injDrag;
    std::vector<float> smoothInj;      // decayed strength, drives tick alpha
    std::vector<float> smoothInjLoc;   // latched loc while smoothInj is decaying
    std::vector<float> smoothInjDrag;  // latched drag while smoothInj is decaying

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
