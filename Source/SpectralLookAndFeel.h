#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>

// Dark, glowing rotary style matching the spectrum display (purple-blue accent).
class SpectralLookAndFeel : public juce::LookAndFeel_V4, private juce::Timer
{
public:
    SpectralLookAndFeel();
    ~SpectralLookAndFeel() override;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override;

    juce::Label* createSliderTextBox (juce::Slider&) override;

    // Must be called BEFORE the sliders that use this look-and-feel are destroyed
    // (i.e. as the first line of the owning editor's destructor). The glow animation
    // keeps a map of raw Component* keyed to hovered sliders; a stray timer tick after
    // a slider is gone would touch a dangling pointer. This stops the timer and drops
    // the map synchronously on the message thread, closing that window.
    void stopGlowAnimation();

private:
    void timerCallback() override;

    const juce::Colour accent  { 0xff8f86ff }; // purple-blue
    const juce::Colour accent2 { 0xff5fd0c8 }; // green-blue

    // Per-slider glow level (0..1), eased toward hovered/dragging each timer tick so the
    // shine fades in/out smoothly instead of popping. Timer runs only while something is
    // mid-transition (started lazily in drawRotarySlider, stopped once all settle).
    std::map<juce::Component*, float> glowLevel;
};
