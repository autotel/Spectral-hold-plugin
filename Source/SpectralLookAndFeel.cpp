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

    // TextButton on/off contrast: the default V4 look barely distinguishes toggled-on from
    // off against this dark theme (e.g. the Snap button was unclear whether it was engaged).
    // On = a solid accent fill with dark text; off = the same dark chrome as the ComboBoxes,
    // light text.
    setColour (juce::TextButton::buttonColourId,   Colour (0xff20202a));
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId,  Colour (0xffd0d0dc));
    setColour (juce::TextButton::textColourOnId,   Colour (0xff101014));
}

SpectralLookAndFeel::~SpectralLookAndFeel()
{
    stopTimer();
}

void SpectralLookAndFeel::stopGlowAnimation()
{
    stopTimer();
    glowLevel.clear();
}

void SpectralLookAndFeel::timerCallback()
{
    constexpr float kEase = 0.30f;    // per-tick approach rate (~30 Hz -> settles in ~300 ms)
    constexpr float kEpsilon = 0.002f;
    bool anyMoving = false;

    for (auto& [comp, level] : glowLevel)
    {
        const float target = comp->isMouseOverOrDragging (true) ? 1.0f : 0.0f;
        const float next = level + (target - level) * kEase;
        if (std::abs (next - level) > kEpsilon || std::abs (target - next) > kEpsilon)
            anyMoving = true;
        if (std::abs (next - level) > 0.0001f)
            comp->repaint();
        level = next;
    }

    if (! anyMoving)
        stopTimer(); // idle: no repaints needed until the next hover change
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
        ColourGradient body (Colour (0xff15151b), cx, cy - radius * 0.4f,
                             Colour (0xff0a0a0e), cx, cy + radius, false);
        g.setGradientFill (body);
        g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        g.setColour (Colour (0xff34343f));
        g.drawEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.0f);
    }

    // on hover, the runway shines outward (concentric arcs growing beyond the edge).
    // The glow level is eased (0..1) rather than an on/off toggle, so it fades in/out
    // smoothly; SpectralLookAndFeel::timerCallback animates it and drives repaints.
    {
        auto it = glowLevel.find (&s);
        if (it == glowLevel.end())
            it = glowLevel.emplace (&s, 0.0f).first;
        const float glow = it->second;

        const bool hovering = s.isMouseOverOrDragging (true);
        if ((hovering && glow < 0.999f) || (! hovering && glow > 0.001f))
            if (! isTimerRunning())
                startTimerHz (30);

        if (glow > 0.001f)
        {
            for (int i = 1; i <= 4; ++i)
            {
                const float rr = runR + (float) i * 2.1f;
                Path glowArc;
                glowArc.addCentredArc (cx, cy, rr, rr, 0.0f, fromAngle, angle, true);
                g.setColour (accent.withAlpha (glow * 0.34f / (float) i));
                g.strokePath (glowArc, PathStrokeType (runW * 1.15f, PathStrokeType::curved, PathStrokeType::rounded));
            }
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

void SpectralLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                                const juce::Colour& backgroundColour,
                                                bool isMouseOverButton, bool isButtonDown)
{
    using namespace juce;

    const auto id = b.getComponentID();
    if (! id.startsWith ("tab-"))
    {
        LookAndFeel_V4::drawButtonBackground (g, b, backgroundColour, isMouseOverButton, isButtonDown);
        return;
    }

    // Segmented tab strip: only the strip's outer edges are rounded (first button's left
    // corners, last button's right corners); every button in between -- and the inner edges
    // of the first/last -- stays flat, so the whole row reads as one continuous control.
    auto colour = backgroundColour;
    if (isButtonDown)           colour = colour.darker (0.15f);
    else if (isMouseOverButton) colour = colour.brighter (0.08f);

    const bool roundLeft  = id == "tab-first";
    const bool roundRight = id == "tab-last";
    constexpr float corner = 6.0f;

    auto bounds = b.getLocalBounds().toFloat();
    Path p;
    p.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(),
                           corner, corner, roundLeft, roundRight, roundLeft, roundRight);
    g.setColour (colour);
    g.fillPath (p);
}
