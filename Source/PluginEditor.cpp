#include "PluginEditor.h"

SpectralHoldEditor::SpectralHoldEditor (SpectralHoldProcessor& p)
    : AudioProcessorEditor (p), proc (p), display (p)
{
    addAndMakeVisible (display);

    setupKnob (feed,       "feed",       "Feed");
    setupKnob (loss,       "loss",       "Loss");
    setupKnob (filterAmt,  "filterAmt",  "Filter");
    setupKnob (filterTone, "filterTone", "Tone");
    setupKnob (attack,     "attack",     "Attack");
    setupKnob (compress,   "compress",   "Compress");

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

    setSize (720, 420);
}

void SpectralHoldEditor::setupKnob (Knob& k, const juce::String& paramId, const juce::String& text)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
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

    auto top = r.removeFromTop (260);
    display.setBounds (top.reduced (8));

    auto controls = r.reduced (8, 0);
    const int n = 6;
    const int kw = controls.getWidth() / n;

    Knob* knobs[n] = { &feed, &loss, &filterAmt, &filterTone, &attack, &compress };
    for (auto* k : knobs)
    {
        auto cell = controls.removeFromLeft (kw);
        k->label.setBounds (cell.removeFromTop (18));
        k->slider.setBounds (cell.reduced (4));
    }

    auto bottom = getLocalBounds().removeFromBottom (28).reduced (8, 4);
    sizeBox.setBounds (bottom.removeFromRight (90));
    sizeLabel.setBounds (bottom.removeFromRight (70));
}
