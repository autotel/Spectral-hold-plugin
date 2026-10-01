#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

// A two-position vertical toggle switch with option labels to the right.
// Top half = state 0, bottom half = state 1.
// Ported from ../lanes-audio-plugin (Source/UI/VerticalToggle.{h,cpp}); the only changes
// are the colours, which come from the owning LookAndFeel instead of being hard-coded.
class VerticalToggle : public juce::Component
{
public:
    explicit VerticalToggle (juce::StringArray labels);

    void setState (int s, bool sendNotification = false);
    int getState() const { return state_; }

    std::function<void(int)> onStateChanged;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    juce::StringArray labels_;
    int state_ = 0;

    static constexpr int trackW = 24;
    static constexpr int trackGap = 5;
};
