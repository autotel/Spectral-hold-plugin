#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include "PluginProcessor.h"
#include "SpectrumDisplay.h"
#include "SpectralLookAndFeel.h"
#include "Presets.h"

class SpectralHoldEditor : public juce::AudioProcessorEditor,
                            private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit SpectralHoldEditor (SpectralHoldProcessor&);
    ~SpectralHoldEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;

    // Listens to EVERY APVTS parameter (registered in the ctor by walking proc.apvts.state's
    // PARAM children -- see gotchas.md): drives both
    //  - Undo trigger #2 (agent-wiki/plan-roadmap.md B6): snapshot on the false->true edge
    //    of "armed permanent" (shapeMode > 0.01 && |shapeLevel| > 0.01), not per knob tick;
    //  - Presets (B9): any manual knob edit deselects the combo (see applyingPreset).
    // Only fires for GUI-driven changes (setValueNotifyingHost) -- host automation calls
    // setValue() directly and never reaches this, an accepted gap (see gotchas.md).
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    bool shaperArmedPermanent = false;
    std::atomic<float>* pShapeModeRaw  = nullptr;
    std::atomic<float>* pShapeLevelRaw = nullptr;

    // Presets (agent-wiki/plan-roadmap.md B9): infrastructure only, values are placeholders
    // (see Presets.h). applyingPreset suppresses parameterChanged's deselect-on-edit logic
    // while applyPreset() is itself the one touching every parameter.
    void applyPreset (int index);
    bool applyingPreset = false;

    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<SliderAttach> attach;
    };

    void setupKnob (Knob&, const juce::String& paramId, const juce::String& text);
    void setActiveTab (int tabIndex); // 0 = Shaper, 1 = Harmonize, 2 = Reverb, 3 = Perform
    void setInfo (juce::Component&, const juce::String& description);

    // Resizable editor (agent-wiki/plan-roadmap.md B8): all children live inside `content`,
    // a fixed 860x580 component that the outer editor scales via AffineTransform on resize.
    // layoutContent() holds the old resized() body verbatim (it only fires once per content
    // bounds change -- i.e. once ever, since content's bounds never change, only the
    // transform does), operating in content's own 860x580 coordinate space.
    void layoutContent();
    struct ContentComponent : public juce::Component
    {
        SpectralHoldEditor& owner;
        explicit ContentComponent (SpectralHoldEditor& o) : owner (o) {}
        void resized() override { owner.layoutContent(); }
    };

    SpectralHoldProcessor& proc;

    SpectralLookAndFeel lnf; // declared first so it outlives the components using it

    ContentComponent content { *this }; // declared before its children (see B8 note above)

    SpectrumDisplay display;

    // persistent row (always visible)
    Knob feed, loss, ewLocation, dryWet, output, limThreshold, limRelease;

    // tabbed rows (one visible at a time)
    Knob shapeAmt, shapeMode, shape, shapeFreq, shapeWidth, shapeCount, shapeLevel;
    Knob harmonize, harmWidth, harmonic;
    Knob revMix, revDecay, revDamp, revSize, revPredelay, revMetal, revFeed;

    // Perform tab (agent-wiki/plan-uifix.md U3): Transpose (+Snap toggle, +Glide), Spread,
    // Phase Noise -- replaces the old utility-row Transpose/Phase Noise sliders.
    Knob transpose, transposeGlide, spread, phaseNoise;
    juce::TextButton snapButton { "Snap" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> snapAttach;

    juce::TextButton shaperTab { "Shaper" }, harmonizeTab { "Harmonize" }, reverbTab { "Reverb" },
                      performTab { "Perform" };
    int activeTab = 0;

    // Preset picker (agent-wiki/plan-roadmap.md B9): lives at the right end of the tab
    // strip, not the utility row -- that row has no width budget left (see layoutContent()).
    juce::ComboBox presetBox;

    juce::Label    sizeLabel;
    juce::ComboBox sizeBox;

    // GUI-only switches (persisted by the processor, not DAW parameters)
    juce::ToggleButton liveButton { "Live (0 PDC)" };
    juce::ToggleButton keepButton { "Keep" }; // agent-wiki/plan-roadmap.md B1: save held state
    juce::TextButton   undoButton { "Undo" }; // agent-wiki/plan-roadmap.md B6

    juce::Label  brushLabel;
    juce::Slider brushSizeSlider; // GUI-only brush size, not a DAW parameter

    // Ableton-style info bar: hovering any control shows its one-line description here.
    struct InfoListener : juce::MouseListener
    {
        std::map<juce::Component*, juce::String> texts;
        juce::Label* bar = nullptr;
        void mouseEnter (const juce::MouseEvent& e) override
        {
            auto it = texts.find (e.eventComponent);
            if (bar != nullptr && it != texts.end())
                bar->setText (it->second, juce::dontSendNotification);
        }
        void mouseExit (const juce::MouseEvent&) override
        {
            if (bar != nullptr)
                bar->setText ({}, juce::dontSendNotification);
        }
    };
    InfoListener infoListener;
    juce::Label  infoBar;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralHoldEditor)
};
