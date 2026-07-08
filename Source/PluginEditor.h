#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include "PluginProcessor.h"
#include "SpectrumDisplay.h"
#include "SpectralLookAndFeel.h"

class SpectralHoldEditor : public juce::AudioProcessorEditor
{
public:
    explicit SpectralHoldEditor (SpectralHoldProcessor&);
    ~SpectralHoldEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<SliderAttach> attach;
    };

    void setupKnob (Knob&, const juce::String& paramId, const juce::String& text);
    void setActiveTab (int tabIndex); // 0 = Shaper, 1 = Harmonize, 2 = Reverb
    void setInfo (juce::Component&, const juce::String& description);

    SpectralHoldProcessor& proc;

    SpectralLookAndFeel lnf; // declared first so it outlives the components using it

    SpectrumDisplay display;

    // persistent row (always visible)
    Knob feed, loss, ewLocation, dryWet, output, limThreshold, limRelease;
    juce::TextButton freezeButton { "Freeze" }; // agent-wiki/plan-roadmap.md B2: stop time
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> freezeAttach;

    // tabbed rows (one visible at a time)
    Knob shapeAmt, shapeMode, shape, shapeFreq, shapeWidth, shapeCount, shapeLevel;
    Knob harmonize, harmWidth, harmonic;
    Knob revMix, revDecay, revDamp, revSize, revPredelay, revMetal, revFeed;

    juce::TextButton shaperTab { "Shaper" }, harmonizeTab { "Harmonize" }, reverbTab { "Reverb" };
    int activeTab = 0;

    juce::Label    sizeLabel;
    juce::ComboBox sizeBox;

    juce::ToggleButton noiseButton { "Phase Noise" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> noiseAttach;

    // GUI-only switches (persisted by the processor, not DAW parameters)
    juce::ToggleButton liveButton { "Live (0 PDC)" };
    juce::ToggleButton keepButton { "Keep" }; // agent-wiki/plan-roadmap.md B1: save held state

    juce::Label  brushLabel;
    juce::Slider brushSizeSlider; // GUI-only brush size, not a DAW parameter

    // Transpose (agent-wiki/plan-roadmap.md B3): utility-row linear slider (not a rotary
    // Knob) since it doesn't yet have a tab of its own -- P4 "Perform" is DAW-page-only
    // until B4/B5 fill out a fourth GUI tab.
    juce::Label  transposeLabel;
    juce::Slider transposeSlider;
    std::unique_ptr<SliderAttach> transposeAttach;

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
