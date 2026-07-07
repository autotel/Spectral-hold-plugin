#include "PluginEditor.h"

SpectralHoldEditor::SpectralHoldEditor (SpectralHoldProcessor& p)
    : AudioProcessorEditor (p), proc (p), display (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (display);

    // persistent row
    setupKnob (feed,       "feed",       "Feed");
    setupKnob (loss,       "loss",       "Loss");
    setupKnob (ewLocation, "ewLocation", "E<->W");
    setupKnob (dryWet,     "dryWet",     "Dry/Wet");
    setupKnob (output,     "output",     "Output");
    setupKnob (limThreshold, "limThreshold", "Thresh");
    setupKnob (limRelease,   "limRelease",   "Release");

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
    setupKnob (revDamp,     "revDamp",     "Damp");
    setupKnob (revSize,     "revSize",     "Size");
    setupKnob (revPredelay, "revPredelay", "Predelay");
    setupKnob (revMetal,    "revMetal",    "Metal");

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


    // info bar (Ableton-style): hover any control for a one-line explanation
    infoListener.bar = &infoBar;
    infoBar.setJustificationType (juce::Justification::centredLeft);
    infoBar.setColour (juce::Label::textColourId, juce::Colour (0xffa0a0aa));
    infoBar.setFont (juce::Font (juce::FontOptions (13.0f)));
    addAndMakeVisible (infoBar);

    setInfo (feed.slider,       "How much live input is injected into the held spectrum each hop.");
    setInfo (loss.slider,       "How fast held tones decay. 0 = hold forever.");
    setInfo (ewLocation.slider, "Walk the East-West line: tones are recorded at this position and heard louder the closer you are. Set Feed to 0 to walk without recording.");
    setInfo (dryWet.slider,     "Balance of untouched input vs the spectral hold output.");
    setInfo (output.slider,     "Output level, before the safety limiter.");
    setInfo (limThreshold.slider, "Output ceiling. The slow limiter pulls the level down to this.");
    setInfo (limRelease.slider,   "How fast the limiter recovers after pulling down.");
    setInfo (shapeAmt.slider,   "Shaper depth: scales the whole curve.");
    setInfo (shapeMode.slider,  "Shaper: momentary (out-only, reversible) vs permanent (etched into the held sound).");
    setInfo (shape.slider,      "Morphs the curve: Level, Sigmoid, Spikes, Harmonics, Sine.");
    setInfo (shapeFreq.slider,  "Shaper curve centre frequency.");
    setInfo (shapeWidth.slider, "Shaper curve width / steepness / spacing (per shape).");
    setInfo (shapeCount.slider, "Shaper extent / repetitions across the spectrum (per shape).");
    setInfo (shapeLevel.slider, "Shaper strength, signed. 0 = off; negative inverts (cut/pass per shape).");
    setInfo (harmonize.slider,  "Held tones pull each other's pitch until they drift together. Permanent while Feed is low; live input re-tunes it back.");
    setInfo (harmWidth.slider,  "How far apart (in octaves) tones still influence each other.");
    setInfo (harmonic.slider,   "Blend: 0 = tones average together, 1 = tones snap to simple harmonic ratios.");
    setInfo (revMix.slider,     "Reverb wet/dry on the output. 0 = reverb fully off.");
    setInfo (revDecay.slider,   "Reverb tail length.");
    setInfo (revDamp.slider,    "Darkens the reverb tail.");
    setInfo (revSize.slider,    "Reverb room size. Moving it live bends the tail's pitch.");
    setInfo (revPredelay.slider,"Gap before the reverb starts.");
    setInfo (revMetal.slider,   "Trades diffusion for a harder, more metallic reflection character.");
    setInfo (noiseButton,       "Adds shimmer by jittering each tone's phase. Never detunes permanently.");
    setInfo (sizeBox,           "Spectral resolution vs time response. Changing it resets the held sound.");
    setInfo (liveButton,        "Report zero latency to the host (for live playing; disables delay compensation).");
    setInfo (brushSizeSlider,   "Drag on the display to boost/cut held tones. This sets the brush width.");
    setInfo (shaperTab,         "Per-bin amplitude curves: filter, compress, combs and more.");
    setInfo (harmonizeTab,      "Coupled-oscillator pitch interaction between held tones.");
    setInfo (reverbTab,         "Plate reverb on the output, optionally fed back into the hold.");

    setActiveTab (proc.getUiTab()); // restore the last shown tab (persisted in state)
    setSize (860, 580);
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

void SpectralHoldEditor::setInfo (juce::Component& c, const juce::String& description)
{
    infoListener.texts[&c] = description;
    c.addMouseListener (&infoListener, false);
}

void SpectralHoldEditor::setActiveTab (int tabIndex)
{
    activeTab = tabIndex;
    proc.setUiTab (tabIndex); // persisted with the plugin state
    // Both display overlays (shaper curve, harmonize influence) are always drawn when
    // their module is active, regardless of the visible tab -- so an armed modifier is
    // never hidden just because you're looking at a different tab.

    Knob* shaperKnobs[]    = { &shapeAmt, &shapeMode, &shape, &shapeFreq, &shapeWidth, &shapeCount, &shapeLevel };
    Knob* harmonizeKnobs[] = { &harmonize, &harmWidth, &harmonic };
    Knob* reverbKnobs[]    = { &revMix, &revDecay, &revDamp, &revSize, &revPredelay, &revMetal };

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

    // reserve the bottom rows first so knobs never overlap them
    infoBar.setBounds (r.removeFromBottom (20).reduced (10, 0));
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
    Knob* row1Knobs[] = { &feed, &loss, &ewLocation, &dryWet, &output, &limThreshold, &limRelease };
    layoutRow (row1, row1Knobs, 7);

    // tab strip
    auto tabs = controls.removeFromTop (28).reduced (0, 2);
    const int tw = tabs.getWidth() / 3;
    shaperTab.setBounds    (tabs.removeFromLeft (tw).reduced (2, 0));
    harmonizeTab.setBounds (tabs.removeFromLeft (tw).reduced (2, 0));
    reverbTab.setBounds    (tabs.reduced (2, 0));

    // tabbed row: all three share the same area; visibility picks the active one
    Knob* shaperKnobs[]    = { &shapeAmt, &shapeMode, &shape, &shapeFreq, &shapeWidth, &shapeCount, &shapeLevel };
    Knob* harmonizeKnobs[] = { &harmonize, &harmWidth, &harmonic };
    Knob* reverbKnobs[]    = { &revMix, &revDecay, &revDamp, &revSize, &revPredelay, &revMetal };
    layoutRow (controls, shaperKnobs,    7);
    layoutRow (controls, harmonizeKnobs, 3);
    layoutRow (controls, reverbKnobs,    6);

    sizeBox.setBounds (bottom.removeFromRight (80));
    sizeLabel.setBounds (bottom.removeFromRight (50));
    noiseButton.setBounds (bottom.removeFromLeft (100));
    liveButton.setBounds (bottom.removeFromLeft (96));
    brushSizeSlider.setBounds (bottom.removeFromRight (150));
    brushLabel.setBounds (bottom.removeFromRight (44));
}
