#include "SpectralLookAndFeel.h"

SpectralLookAndFeel::SpectralLookAndFeel()
{
    using juce::Colour;
    setColour (juce::Slider::textBoxTextColourId,       Colour (0xffb8b8c8));
    setColour (juce::Slider::textBoxOutlineColourId,    Colour (0x00000000));
    setColour (juce::Slider::textBoxBackgroundColourId, Colour (0x00000000));
    setColour (juce::Slider::trackColourId,             accent);
    setColour (juce::Slider::backgroundColourId,        Colour (0xff2a2a34));
    setColour (juce::Slider::thumbColourId,             accent);

    setColour (juce::Label::textColourId,               Colour (0xffd0d0dc));

    setColour (juce::ComboBox::backgroundColourId,      Colour (0xff20202a));
    setColour (juce::ComboBox::textColourId,            Colour (0xffd0d0dc));
    setColour (juce::ComboBox::outlineColourId,         Colour (0xff3a3a46));
    setColour (juce::ComboBox::arrowColourId,           accent);

    setColour (juce::PopupMenu::backgroundColourId,     Colour (0xff20202a));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.35f));

    setColour (juce::ToggleButton::textColourId,        Colour (0xffd0d0dc));
    setColour (juce::ToggleButton::tickColourId,        accent);
    setColour (juce::ToggleButton::tickDisabledColourId, Colour (0xff4a4a56));
}

void SpectralLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float startAngle, float endAngle,
                                            juce::Slider& s)
{
    using namespace juce;
    auto bounds = Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (5.0f);
    const float radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    const float angle = startAngle + sliderPos * (endAngle - startAngle);

    // runway sits on the outer edge of the circle and is thin
    const float runR = radius - 1.5f;
    const float runW = 2.0f;

    // bipolar knobs (e.g. Compress) fill from centre; others fill from the start
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float fromAngle = bipolar ? (startAngle + 0.5f * (endAngle - startAngle)) : startAngle;

    // body with a soft radial shade
    {
        ColourGradient body (Colour (0xff2b2b37), cx, cy - radius * 0.4f,
                             Colour (0xff15151c), cx, cy + radius, false);
        g.setGradientFill (body);
        g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        g.setColour (Colour (0xff34343f));
        g.drawEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.0f);
    }

    // on hover, the runway shines outward (concentric arcs growing beyond the edge)
    if (s.isMouseOverOrDragging (true))
    {
        for (int i = 1; i <= 3; ++i)
        {
            const float rr = runR + (float) i * 1.7f;
            Path glow;
            glow.addCentredArc (cx, cy, rr, rr, 0.0f, fromAngle, angle, true);
            g.setColour (accent.withAlpha (0.22f / (float) i));
            g.strokePath (glow, PathStrokeType (runW, PathStrokeType::curved, PathStrokeType::rounded));
        }
    }

    // background track arc (on the edge)
    Path track;
    track.addCentredArc (cx, cy, runR, runR, 0.0f, startAngle, endAngle, true);
    g.setColour (Colour (0xff3a3a46));
    g.strokePath (track, PathStrokeType (runW, PathStrokeType::curved, PathStrokeType::rounded));

    // value runway
    Path value;
    value.addCentredArc (cx, cy, runR, runR, 0.0f, fromAngle, angle, true);
    ColourGradient grad (accent, cx - runR, cy, accent2, cx + runR, cy, false);
    g.setGradientFill (grad);
    g.strokePath (value, PathStrokeType (runW, PathStrokeType::curved, PathStrokeType::rounded));

    // pointer reaches out to touch the runway
    const float p1 = runR - runW * 0.5f;     // outer end, on the runway
    const float p0 = p1 - 9.0f;              // inner end
    Point<float> a (cx + std::sin (angle) * p0, cy - std::cos (angle) * p0);
    Point<float> b (cx + std::sin (angle) * p1, cy - std::cos (angle) * p1);
    g.setColour (Colours::white.withAlpha (0.9f));
    g.drawLine ({ a, b }, 3.0f);
}

juce::Label* SpectralLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = juce::LookAndFeel_V4::createSliderTextBox (s);
    l->setJustificationType (juce::Justification::centred);
    l->setFont (juce::Font (juce::FontOptions (12.0f)));
    l->setColour (juce::Label::outlineColourId,            juce::Colours::transparentBlack);
    l->setColour (juce::Label::backgroundColourId,         juce::Colours::transparentBlack);
    l->setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::TextEditor::outlineColourId,        juce::Colours::transparentBlack);
    return l;
}
