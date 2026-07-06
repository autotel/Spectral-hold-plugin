#include "PluginEditor.h"

SpectralHoldEditor::SpectralHoldEditor (SpectralHoldProcessor& p)
    : AudioProcessorEditor (p), proc (p), display (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (display);

    // persistent row
    setupKnob (feed,       "feed",       "Feed");
    setupKnob (loss,       "loss",       "Loss");
    setupKnob (dryWet,     "dryWet",     "Dry/Wet");
    setupKnob (output,     "output",     "Output");

    // tabbed rows
    setupKnob (shapeAmt,   "shapeAmt",   "Amount");
    setupKnob (shapeMode,  "shapeMode",  "Mode");
    setupKnob (shape,      "shape",      "Shape");
    setupKnob (shapeFreq,  "shapeFreq",  "Freq");
    setupKnob (shapeWidth, "shapeWidth", "Width");
    setupKnob (shapeCount, "shapeCount", "Count");
    setupKnob (shapeLevel, "shapeLevel", "Level");

    setupKnob (harmonize,  "harmonize",  "Harmonize");
    setupKnob (harmWidth,  "harmWidth",  "Width");
    setupKnob (harmonic,   "harmonic",   "Harmonic");

    setupKnob (revMix,      "revMix",      "Mix");
    setupKnob (revDecay,    "revDecay",    "Decay");
    setupKnob (revSize,     "revSize",     "Size");
    setupKnob (revDamp,     "revDamp",     "Damp");
    setupKnob (revPredelay, "revPredelay", "Predelay");
    setupKnob (revFeed,     "revFeed",     "Feed");

    // tab strip (radio-style toggles switching the visible knob row)
    for (auto* t : { &shaperTab, &harmonizeTab, &reverbTab })
    {
        t->setClickingTogglesState (true);
        t->setRadioGroupId (1001);
        addAndMakeVisible (*t);
    }
    shaperTab.onClick    = [this] { if (shaperTab.getToggleState())    setActiveTab (0); };
    harmonizeTab.onClick = [this] { if (harmonizeTab.getToggleState()) setActiveTab (1); };
    reverbTab.onClick    = [this] { if (reverbTab.getToggleState())    setActiveTab (2); };

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

    setActiveTab (0);
    setSize (860, 560);
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

void SpectralHoldEditor::setActiveTab (int tabIndex)
{
    activeTab = tabIndex;

    Knob* shaperKnobs[]    = { &shapeAmt, &shapeMode, &shape, &shapeFreq, &shapeWidth, &shapeCount, &shapeLevel };
    Knob* harmonizeKnobs[] = { &harmonize, &harmWidth, &harmonic };
    Knob* reverbKnobs[]    = { &revMix, &revDecay, &revSize, &revDamp, &revPredelay, &revFeed };

    auto show = [] (Knob* const* ks, int n, bool visible)
    {
        for (int i = 0; i < n; ++i)
        {
            ks[i]->slider.setVisible (visible);
            ks[i]->label.setVisible (visible);
        }
    };
    show (shaperKnobs,    7, tabIndex == 0);
    show (harmonizeKnobs, 3, tabIndex == 1);
    show (reverbKnobs,    6, tabIndex == 2);

    shaperTab.setToggleState    (tabIndex == 0, juce::dontSendNotification);
    harmonizeTab.setToggleState (tabIndex == 1, juce::dontSendNotification);
    reverbTab.setToggleState    (tabIndex == 2, juce::dontSendNotification);
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

    // persistent row: the performance knobs, always visible
    auto row1 = controls.removeFromTop ((controls.getHeight() - 28) / 2);
    Knob* row1Knobs[] = { &feed, &loss, &dryWet, &output };
    layoutRow (row1, row1Knobs, 4);

    // tab strip
    auto tabs = controls.removeFromTop (28).reduced (0, 2);
    const int tw = tabs.getWidth() / 3;
    shaperTab.setBounds    (tabs.removeFromLeft (tw).reduced (2, 0));
    harmonizeTab.setBounds (tabs.removeFromLeft (tw).reduced (2, 0));
    reverbTab.setBounds    (tabs.reduced (2, 0));

    // tabbed row: all three share the same area; visibility picks the active one
    Knob* shaperKnobs[]    = { &shapeAmt, &shapeMode, &shape, &shapeFreq, &shapeWidth, &shapeCount, &shapeLevel };
    Knob* harmonizeKnobs[] = { &harmonize, &harmWidth, &harmonic };
    Knob* reverbKnobs[]    = { &revMix, &revDecay, &revSize, &revDamp, &revPredelay, &revFeed };
    layoutRow (controls, shaperKnobs,    7);
    layoutRow (controls, harmonizeKnobs, 3);
    layoutRow (controls, reverbKnobs,    6);

    sizeBox.setBounds (bottom.removeFromRight (80));
    sizeLabel.setBounds (bottom.removeFromRight (50));
    noiseButton.setBounds (bottom.removeFromLeft (100));
    liveButton.setBounds (bottom.removeFromLeft (96));
    saveSoundButton.setBounds (bottom.removeFromLeft (96));
    brushSizeSlider.setBounds (bottom.removeFromRight (150));
    brushLabel.setBounds (bottom.removeFromRight (44));
}
