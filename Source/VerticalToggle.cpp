#include "VerticalToggle.h"

VerticalToggle::VerticalToggle (juce::StringArray labels)
    : labels_ (std::move (labels))
{
    jassert (labels_.size() == 2);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void VerticalToggle::setState (int s, bool sendNotification)
{
    if (state_ == s) return;
    state_ = s;
    repaint();
    if (sendNotification && onStateChanged)
        onStateChanged (state_);
}

void VerticalToggle::paint (juce::Graphics& g)
{
    auto b = getLocalBounds();

    // Fixed-size pill track, centered vertically in the component
    float tW  = (float) (trackW - 2);         // inner width with 1px margin each side
    float tH  = tW * 1.75f;                   // ~40px for trackW=24 -- classic pill ratio
    float tX  = 1.0f;
    float tY  = ((float) b.getHeight() - tH) * 0.5f;
    float cornerR = tW * 0.5f;                // full capsule

    juce::Rectangle<float> track (tX, tY, tW, tH);

    g.setColour (juce::Colour (0xff15151b));
    g.fillRoundedRectangle (track, cornerR);
    g.setColour (juce::Colour (0xff3a3a46));
    g.drawRoundedRectangle (track, cornerR, 1.0f);

    // Circular thumb
    float thumbPad  = 3.0f;
    float thumbDiam = tW - 2.0f * thumbPad;
    float thumbX    = track.getX() + thumbPad;
    float thumbTopY = track.getY() + thumbPad;
    float thumbBotY = track.getBottom() - thumbDiam - thumbPad;
    float thumbY    = (state_ == 0) ? thumbTopY : thumbBotY;

    g.setColour (findColour (juce::Slider::thumbColourId));
    g.fillEllipse (thumbX, thumbY, thumbDiam, thumbDiam);

    // Labels stacked to the right, each aligned with the thumb's half of the track
    float labelX = (float) (trackW + trackGap);
    float labelW = (float) (b.getWidth() - trackW - trackGap);
    float halfH  = tH * 0.5f;

    g.setFont (juce::Font (juce::FontOptions (11.0f)));

    for (int i = 0; i < 2; ++i)
    {
        bool active = (state_ == i);
        g.setColour (active ? juce::Colour (0xffd0d0dc) : juce::Colour (0xff5a5a66));
        g.drawText (labels_[i],
                    (int) labelX, (int) (tY + i * halfH),
                    (int) labelW, (int) halfH,
                    juce::Justification::centredLeft, true);
    }
}

void VerticalToggle::mouseDown (const juce::MouseEvent& e)
{
    // Click top half of track -> option 0, bottom half -> option 1
    float tH  = (float) (trackW - 2) * 1.75f;
    float midY = ((float) getHeight() - tH) * 0.5f + tH * 0.5f;
    int newState = (e.y < (int) midY) ? 0 : 1;
    setState (newState, true);
}
