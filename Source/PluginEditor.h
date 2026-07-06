#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
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

    SpectralHoldProcessor& proc;

    SpectralLookAndFeel lnf; // declared first so it outlives the components using it

    SpectrumDisplay display;

    Knob feed, loss, output;
    Knob shapeAmt, shapeMode, shape, shapeFreq, shapeWidth, shapeCount, shapeLevel;
    Knob harmonize, harmWidth, harmonic;
    Knob revMix, revDecay, revSize, revDamp, revPredelay;

    juce::Label    sizeLabel;
    juce::ComboBox sizeBox;

    juce::ToggleButton noiseButton { "Phase Noise" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> noiseAttach;

    // GUI-only switches (persisted by the processor, not DAW parameters)
    juce::ToggleButton liveButton { "Live (0 PDC)" };
    juce::ToggleButton saveSoundButton { "Save sound" };

    juce::Label  brushLabel;
    juce::Slider brushSizeSlider; // GUI-only brush size, not a DAW parameter

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralHoldEditor)
};
