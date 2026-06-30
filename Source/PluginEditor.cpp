#include "PluginEditor.h"

SpectralHoldEditor::SpectralHoldEditor (SpectralHoldProcessor& p)
    : AudioProcessorEditor (p), proc (p), display (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (display);

    setupKnob (feed,       "feed",       "Feed");
    setupKnob (loss,       "loss",       "Loss");
    setupKnob (filterAmt,  "filterAmt",  "Filter");
    setupKnob (filterTone, "filterTone", "Tone");
    setupKnob (attack,     "attack",     "Attack");
    setupKnob (compress,   "compress",   "Compress");
    setupKnob (harmonize,  "harmonize",  "Harmonize");
    setupKnob (harmWidth,  "harmWidth",  "Width");
    setupKnob (harmonic,   "harmonic",   "Harmonic");

    // FFT size: GUI-only (not a DAW parameter).
    addAndMakeVisible (sizeBox);
    int id = 1;
    for (int order = SpectralHoldProcessor::kMinFftOrder;
         order <= SpectralHoldProcessor::kMaxFftOrder; ++order, ++id)
        sizeBox.addItem (juce::String (1 << order), id);

    sizeBox.setSelectedId (proc.getFftOrder() - SpectralHoldProcessor::kMinFftOrder + 1,
                           juce::dontSendNotification);
    sizeBox.onChange = [this]
    {
        int order = SpectralHoldProcessor::kMinFftOrder + sizeBox.getSelectedId() - 1;
        proc.setFftOrder (order);
    };

    sizeLabel.setText ("FT Size", juce::dontSendNotification);
    sizeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (sizeLabel);

    addAndMakeVisible (noiseButton);
    noiseAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.apvts, "phaseNoise", noiseButton);

    // Brush size: GUI-only (pen/mouse editing is GUI-only), so no APVTS parameter.
    brushSizeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    brushSizeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 16);
    brushSizeSlider.setRange (0.1, 2.0, 0.01);
    brushSizeSlider.setValue (0.6, juce::dontSendNotification);
    brushSizeSlider.setTextValueSuffix (" oct");
    brushSizeSlider.onValueChange = [this] { display.setBrushSigmaOct ((float) brushSizeSlider.getValue()); };
    addAndMakeVisible (brushSizeSlider);
    display.setBrushSigmaOct ((float) brushSizeSlider.getValue());

    brushLabel.setText ("Brush", juce::dontSendNotification);
    brushLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (brushLabel);

    setSize (980, 420);
}

SpectralHoldEditor::~SpectralHoldEditor()
{
    setLookAndFeel (nullptr);
}

void SpectralHoldEditor::setupKnob (Knob& k, const juce::String& paramId, const juce::String& text)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
    k.slider.setRepaintsOnMouseActivity (true); // so the runway shine reacts to hover
    // kill the value-readout box outline/background (component-level override wins)
    k.slider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    k.slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (k.slider);

    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (k.label);

    k.attach = std::make_unique<SliderAttach> (proc.apvts, paramId, k.slider);
}

void SpectralHoldEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff101014));
}

void SpectralHoldEditor::resized()
{
    auto r = getLocalBounds();

    display.setBounds (r.removeFromTop (250).reduced (8));

    // reserve the bottom row first so knobs never overlap it
    auto bottom = r.removeFromBottom (30).reduced (8, 5);

    // knobs in the band between display and bottom row, kept small
    auto controls = r.reduced (8, 4);
    const int n = 9;
    const int kw = controls.getWidth() / n;
    Knob* knobs[n] = { &feed, &loss, &filterAmt, &filterTone, &attack, &compress,
                       &harmonize, &harmWidth, &harmonic };
    for (auto* k : knobs)
    {
        auto cell = controls.removeFromLeft (kw);
        k->label.setBounds (cell.removeFromTop (16));
        // square-ish, centred knob so it stays compact
        int s = juce::jmin (cell.getWidth() - 8, cell.getHeight() - 4);
        k->slider.setBounds (cell.withSizeKeepingCentre (s, s));
    }

    sizeBox.setBounds (bottom.removeFromRight (80));
    sizeLabel.setBounds (bottom.removeFromRight (56));
    noiseButton.setBounds (bottom.removeFromLeft (110));
    brushLabel.setBounds (bottom.removeFromLeft (44));
    brushSizeSlider.setBounds (bottom.removeFromLeft (170));
}
