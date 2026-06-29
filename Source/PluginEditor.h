#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "SpectrumDisplay.h"

class SpectralHoldEditor : public juce::AudioProcessorEditor
{
public:
    explicit SpectralHoldEditor (SpectralHoldProcessor&);
    ~SpectralHoldEditor() override = default;

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

    SpectrumDisplay display;

    Knob feed, loss, filterAmt, filterTone, attack, compress;

    juce::Label    sizeLabel;
    juce::ComboBox sizeBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralHoldEditor)
};
