#include "PluginEditor.h"

SpectralHoldEditor::SpectralHoldEditor (SpectralHoldProcessor& p)
    : AudioProcessorEditor (p), proc (p), display (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (display);

    setupKnob (feed,       "feed",       "Feed");
    setupKnob (loss,       "loss",       "Loss");
    setupKnob (output,     "output",     "Output");

    setupKnob (shapeAmt,   "shapeAmt",   "Amount");
    setupKnob (shapeMode,  "shapeMode",  "Mode");
    setupKnob (shape,      "shape",      "Shape");
    setupKnob (shapeFreq,  "shapeFreq",  "Freq");
    setupKnob (shapeWidth, "shapeWidth", "Width");
    setupKnob (shapeCount, "shapeCount", "Count");
    setupKnob (shapeLevel, "shapeLevel", "Level");

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

    // GUI-only switches
    liveButton.setToggleState (proc.getLiveMode(), juce::dontSendNotification);
    liveButton.onClick = [this] { proc.setLiveMode (liveButton.getToggleState()); };
    addAndMakeVisible (liveButton);

    saveSoundButton.setToggleState (proc.getSaveWithSound(), juce::dontSendNotification);
    saveSoundButton.onClick = [this] { proc.setSaveWithSound (saveSoundButton.getToggleState()); };
    addAndMakeVisible (saveSoundButton);

    setSize (860, 480);
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

    // knobs in the band between display and bottom row, kept small.
    // Row 1: Feed/Loss/Output. Row 2: the spectral shaper (Amount/Mode/Shape/Freq/Width/Count/Level).
    auto layoutRow = [] (juce::Rectangle<int> area, Knob* const* knobs, int count)
    {
        const int kw = area.getWidth() / count;
        for (int i = 0; i < count; ++i)
        {
            auto cell = area.removeFromLeft (kw);
            knobs[i]->label.setBounds (cell.removeFromTop (16));
            int s = juce::jmin (cell.getWidth() - 8, cell.getHeight() - 4);
            knobs[i]->slider.setBounds (cell.withSizeKeepingCentre (s, s));
        }
    };

    auto controls = r.reduced (8, 4);
    auto row1 = controls.removeFromTop (controls.getHeight() / 2);
    auto row2 = controls;

    Knob* row1Knobs[] = { &feed, &loss, &output };
    Knob* row2Knobs[] = { &shapeAmt, &shapeMode, &shape, &shapeFreq, &shapeWidth, &shapeCount, &shapeLevel };
    layoutRow (row1, row1Knobs, 3);
    layoutRow (row2, row2Knobs, 7);

    sizeBox.setBounds (bottom.removeFromRight (80));
    sizeLabel.setBounds (bottom.removeFromRight (50));
    noiseButton.setBounds (bottom.removeFromLeft (100));
    liveButton.setBounds (bottom.removeFromLeft (96));
    saveSoundButton.setBounds (bottom.removeFromLeft (96));
    brushSizeSlider.setBounds (bottom.removeFromRight (150));
    brushLabel.setBounds (bottom.removeFromRight (44));
}
