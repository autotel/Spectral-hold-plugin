#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Dark, glowing rotary style matching the spectrum display (purple-blue accent).
class SpectralLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SpectralLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override;

    juce::Label* createSliderTextBox (juce::Slider&) override;

private:
    const juce::Colour accent  { 0xff8f86ff }; // purple-blue
    const juce::Colour accent2 { 0xff5fd0c8 }; // green-blue
};
