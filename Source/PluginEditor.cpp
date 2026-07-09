#include "PluginEditor.h"

SpectralHoldEditor::SpectralHoldEditor (SpectralHoldProcessor& p)
    : AudioProcessorEditor (p), proc (p), display (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (content);
    content.addAndMakeVisible (display);

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
    setupKnob (shapeMode,  "shapeMode",  "Feed");
    setupKnob (shape,      "shape",      "Shape");
    setupKnob (shapeFreq,  "shapeFreq",  "Freq");
    setupKnob (shapeWidth, "shapeWidth", "Width");
    setupKnob (shapeCount, "shapeCount", "Count");
    setupKnob (shapeLevel, "shapeLevel", "Level");

    setupKnob (harmonize,  "harmonize",  "Harmonize");
    setupKnob (harmWidth,  "harmWidth",  "Width");
    setupKnob (harmonic,   "harmonic",   "Harmonics");

    setupKnob (revMix,      "revMix",      "Mix");
    setupKnob (revDecay,    "revDecay",    "Decay");
    setupKnob (revDamp,     "revDamp",     "Damp");
    setupKnob (revSize,     "revSize",     "Size");
    setupKnob (revPredelay, "revPredelay", "Predelay");
    setupKnob (revMetal,    "revMetal",    "Metal");
    setupKnob (revFeed,     "revFeed",     "Feed");

    // Perform tab (agent-wiki/plan-uifix.md U3): Transpose was a tiny utility-row slider
    // despite its big sonic impact -- now a knob, with a Snap toggle (discrete semitones)
    // and a Glide knob (portamento, knob moves and MIDI note-ons alike). Spread finally
    // gets a widget (it had none since B5 -- the utility row was already full). Phase Noise
    // moves here too, off the utility row.
    setupKnob (transpose,      "transpose",      "Transpose");
    transpose.slider.setTextValueSuffix (" st");
    setupKnob (transposeGlide, "transposeGlide", "Glide");
    transposeGlide.slider.setTextValueSuffix (" ms");
    setupKnob (spread,         "spread",         "Spread");
    setupKnob (phaseNoise,     "phaseNoiseAmt",  "Phase Noise");

    snapButton.setClickingTogglesState (true);
    content.addAndMakeVisible (snapButton);
    snapAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.apvts, "transposeSnap", snapButton);

    // tab strip (radio-style toggles switching the visible knob row)
    for (auto* t : { &shaperTab, &harmonizeTab, &reverbTab, &performTab })
    {
        t->setClickingTogglesState (true);
        t->setRadioGroupId (1001);
        content.addAndMakeVisible (*t);
    }
    shaperTab.onClick    = [this] { if (shaperTab.getToggleState())    setActiveTab (0); };
    harmonizeTab.onClick = [this] { if (harmonizeTab.getToggleState()) setActiveTab (1); };
    reverbTab.onClick    = [this] { if (reverbTab.getToggleState())    setActiveTab (2); };
    performTab.onClick   = [this] { if (performTab.getToggleState())   setActiveTab (3); };

    // FFT size: GUI-only (not a DAW parameter).
    content.addAndMakeVisible (sizeBox);
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
    content.addAndMakeVisible (sizeLabel);

    // Brush size: GUI-only (pen/mouse editing is GUI-only), so no APVTS parameter.
    brushSizeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    brushSizeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 16);
    brushSizeSlider.setRange (0.1, 2.0, 0.01);
    brushSizeSlider.setValue (0.6, juce::dontSendNotification);
    brushSizeSlider.setTextValueSuffix (" oct");
    brushSizeSlider.onValueChange = [this] { display.setBrushSigmaOct ((float) brushSizeSlider.getValue()); };
    content.addAndMakeVisible (brushSizeSlider);
    display.setBrushSigmaOct ((float) brushSizeSlider.getValue());

    brushLabel.setText ("Brush", juce::dontSendNotification);
    brushLabel.setJustificationType (juce::Justification::centredRight);
    content.addAndMakeVisible (brushLabel);

    // GUI-only switches
    liveButton.setToggleState (proc.getLiveMode(), juce::dontSendNotification);
    liveButton.onClick = [this] { proc.setLiveMode (liveButton.getToggleState()); };
    content.addAndMakeVisible (liveButton);

    keepButton.setToggleState (proc.getKeepSound(), juce::dontSendNotification);
    keepButton.onClick = [this] { proc.setKeepSound (keepButton.getToggleState()); };
    content.addAndMakeVisible (keepButton);

    // Undo (agent-wiki/plan-roadmap.md B6): reverts to the last snapshotHold() -- taken on
    // brush-stroke start (SpectrumDisplay::mouseDown) or permanent-shaper engage (below).
    undoButton.onClick = [this] { proc.undoHold(); };
    content.addAndMakeVisible (undoButton);

    pShapeModeRaw  = proc.apvts.getRawParameterValue ("shapeMode");
    pShapeLevelRaw = proc.apvts.getRawParameterValue ("shapeLevel");

    // Register for every APVTS parameter (drives the B6 undo trigger for shapeMode/
    // shapeLevel specifically, and the B9 preset-deselect-on-edit for all of them) --
    // walking proc.apvts.state's PARAM children rather than hardcoding the id list, so this
    // stays correct as params are added (see gotchas.md).
    for (const auto& child : proc.apvts.state)
        if (child.hasType ("PARAM"))
        {
            const auto id = child.getProperty ("id").toString();
            if (id.isNotEmpty())
                proc.apvts.addParameterListener (id, this);
        }

    // Presets (agent-wiki/plan-roadmap.md B9): infrastructure now, curated later -- the
    // shipped values in Presets.h are placeholders, not a finished sound-design pass. No
    // host program API (getNumPrograms stays 1); this is a GUI convenience only, and never
    // touches FT size, held state, or GUI-only toggles (Live/Keep/tab/editor scale) --
    // applyPreset() only iterates proc.getParameters(), which excludes all of those.
    presetBox.setTextWhenNothingSelected ("Preset");
    for (int i = 0; i < kNumPresets; ++i)
        presetBox.addItem (kPresets[(size_t) i].name, i + 1);
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if (id > 0)
            applyPreset (id - 1);
    };
    content.addAndMakeVisible (presetBox);

    // info bar (Ableton-style): hover any control for a one-line explanation
    infoListener.bar = &infoBar;
    infoBar.setJustificationType (juce::Justification::centredLeft);
    infoBar.setColour (juce::Label::textColourId, juce::Colour (0xffa0a0aa));
    infoBar.setFont (juce::Font (juce::FontOptions (13.0f)));
    content.addAndMakeVisible (infoBar);

    setInfo (feed.slider,       "How much live input is injected into the held spectrum each hop.");
    setInfo (loss.slider,       "How fast held tones decay. 0 = hold forever.");
    setInfo (ewLocation.slider, "Walk the East-West line: tones are recorded at this position and heard louder the closer you are. Set Feed to 0 to walk without recording.");
    setInfo (dryWet.slider,     "Balance of untouched input vs the spectral hold output.");
    setInfo (output.slider,     "Output level, before the safety limiter.");
    setInfo (limThreshold.slider, "Output ceiling. The slow limiter pulls the level down to this.");
    setInfo (limRelease.slider,   "How fast the limiter recovers after pulling down.");
    setInfo (shapeAmt.slider,   "Shaper depth: scales the whole curve.");
    setInfo (shapeMode.slider,  "How much the shaping is etched into the held sound vs only heard.");
    setInfo (shape.slider,      "Morphs the curve: Level, Sigmoid, Spikes, Harmonics, Sine.");
    setInfo (shapeFreq.slider,  "Shaper curve centre frequency.");
    setInfo (shapeWidth.slider, "Shaper curve width / steepness / spacing (per shape).");
    setInfo (shapeCount.slider, "Shaper extent / repetitions across the spectrum (per shape).");
    setInfo (shapeLevel.slider, "Shaper strength, signed. 0 = off; negative inverts (cut/pass per shape).");
    setInfo (harmonize.slider,  "Held tones pull each other's pitch until they drift together. Permanent while Feed is low; live input re-tunes it back.");
    setInfo (harmWidth.slider,  "How far apart (in octaves) tones still influence each other.");
    setInfo (harmonic.slider,   "Harmonics blend: 0 = tones average together, 1 = tones snap to simple harmonic ratios.");
    setInfo (revMix.slider,     "Reverb wet/dry on the output. 0 = reverb fully off.");
    setInfo (revDecay.slider,   "Reverb tail length.");
    setInfo (revDamp.slider,    "Darkens the reverb tail.");
    setInfo (revSize.slider,    "Reverb room size. Moving it live bends the tail's pitch.");
    setInfo (revPredelay.slider,"Gap before the reverb starts.");
    setInfo (revMetal.slider,   "Trades diffusion for a harder, more metallic reflection character.");
    setInfo (revFeed.slider,    "Feeds the reverb tail back into the held spectrum. Independent of the main Feed.");
    setInfo (sizeBox,           "Spectral resolution vs time response. Changing it resets the held sound.");
    setInfo (presetBox,         "Load a starting point. Touching any knob afterward deselects it. Values are placeholders, not curated yet.");
    setInfo (liveButton,        "Report zero latency to the host (for live playing; disables delay compensation).");
    setInfo (keepButton,        "Save the held sound inside the session, so it's still ringing when the project reopens.");
    setInfo (undoButton,        "Revert the held sound to before the last brush stroke or permanent-shaper engagement.");
    setInfo (brushSizeSlider,   "Drag on the display to boost/cut held tones. This sets the brush width.");
    setInfo (transpose.slider,  "Pitch-shift the held sound (semitones), non-destructively. MIDI notes add to this, relative to middle C. Snap quantises to whole semitones; Glide smooths any pitch change, including MIDI note-ons, into a portamento.");
    setInfo (snapButton,        "Discrete transposition: snap the knob to whole semitones. MIDI notes are always discrete.");
    setInfo (transposeGlide.slider, "Portamento time for pitch changes -- knob moves and MIDI note-ons alike. 0 = instant.");
    setInfo (spread.slider,     "Momentary per-bin left/right spread of the output. Never enters the held sound.");
    setInfo (phaseNoise.slider, "Shimmer: random per-frame phase jitter. Never detunes permanently.");
    setInfo (shaperTab,         "Per-bin amplitude shaping: tilts, combs and more.");
    setInfo (harmonizeTab,      "Coupled-oscillator pitch interaction between held tones.");
    setInfo (reverbTab,         "Plate reverb on the output.");
    setInfo (performTab,        "Transpose, Spread and Phase Noise -- performance controls.");

    setActiveTab (proc.getUiTab()); // restore the last shown tab (persisted in state)

    // Resizable editor (agent-wiki/plan-roadmap.md B8): fixed 860x580 aspect ratio, scaled
    // as a whole (content's transform in resized()); editorScale is GUI-only persisted
    // state, same pattern as liveMode/uiTab/keepSound.
    setResizable (true, true);
    setResizeLimits (645, 435, 1720, 1160);
    getConstrainer()->setFixedAspectRatio (860.0 / 580.0);
    setSize ((int) (860.0f * proc.getEditorScale()), (int) (580.0f * proc.getEditorScale()));
}

SpectralHoldEditor::~SpectralHoldEditor()
{
    for (const auto& child : proc.apvts.state)
        if (child.hasType ("PARAM"))
        {
            const auto id = child.getProperty ("id").toString();
            if (id.isNotEmpty())
                proc.apvts.removeParameterListener (id, this);
        }

    // Must run before the knobs (declared/destroyed after lnf) are torn down -- see
    // SpectralLookAndFeel::stopGlowAnimation's comment.
    lnf.stopGlowAnimation();
    setLookAndFeel (nullptr);
}

void SpectralHoldEditor::parameterChanged (const juce::String& parameterID, float newValue)
{
    if (parameterID == "shapeMode" || parameterID == "shapeLevel")
    {
        const float sm = parameterID == "shapeMode"  ? newValue : pShapeModeRaw->load();
        const float sl = parameterID == "shapeLevel" ? newValue : pShapeLevelRaw->load();
        const bool armed = sm > 0.01f && std::abs (sl) > 0.01f;
        if (armed && ! shaperArmedPermanent)
            proc.snapshotHold();
        shaperArmedPermanent = armed;
    }

    // Presets (agent-wiki/plan-roadmap.md B9): any manual knob edit deselects the combo --
    // except the edits applyPreset() itself is making right now.
    if (! applyingPreset)
        presetBox.setSelectedId (0, juce::dontSendNotification);
}

void SpectralHoldEditor::applyPreset (int index)
{
    if (index < 0 || index >= kNumPresets)
        return;

    applyingPreset = true;
    // Unlisted params reset to default first (agent-wiki/plan-roadmap.md B9) -- only
    // touches APVTS (DAW) parameters; FT size, held state and GUI-only toggles (Live, Keep,
    // active tab, editor scale) are untouched, they aren't AudioProcessorParameters.
    for (auto* p : proc.getParameters())
        p->setValueNotifyingHost (p->getDefaultValue());

    for (const auto& kv : kPresets[(size_t) index].values)
        if (auto* param = proc.apvts.getParameter (kv.first))
            param->setValueNotifyingHost (param->convertTo0to1 (kv.second));
    applyingPreset = false;
}

void SpectralHoldEditor::setupKnob (Knob& k, const juce::String& paramId, const juce::String& text)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
    k.slider.setRepaintsOnMouseActivity (true); // so the runway shine reacts to hover
    // kill the value-readout box outline/background (component-level override wins)
    k.slider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    k.slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    content.addAndMakeVisible (k.slider);

    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    content.addAndMakeVisible (k.label);

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

    Knob* shaperKnobs[]    = { &shapeAmt, &shape, &shapeFreq, &shapeWidth, &shapeCount, &shapeLevel, &shapeMode };
    Knob* harmonizeKnobs[] = { &harmonize, &harmWidth, &harmonic };
    Knob* reverbKnobs[]    = { &revMix, &revDecay, &revDamp, &revSize, &revPredelay, &revMetal, &revFeed };
    Knob* performKnobs[]   = { &transpose, &transposeGlide, &spread, &phaseNoise };

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
    show (reverbKnobs,    7, tabIndex == 2);
    show (performKnobs,   4, tabIndex == 3);
    snapButton.setVisible (tabIndex == 3);

    shaperTab.setToggleState    (tabIndex == 0, juce::dontSendNotification);
    harmonizeTab.setToggleState (tabIndex == 1, juce::dontSendNotification);
    reverbTab.setToggleState    (tabIndex == 2, juce::dontSendNotification);
    performTab.setToggleState   (tabIndex == 3, juce::dontSendNotification);
}

void SpectralHoldEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff101014));
}

void SpectralHoldEditor::resized()
{
    // Resizable editor (agent-wiki/plan-roadmap.md B8): content is a fixed 860x580 canvas,
    // scaled uniformly to fill whatever size the window actually is. This does NOT give
    // layoutContent() more logical space to work with at any window size -- it's a visual
    // zoom, not a reflow.
    const float scale = (float) getWidth() / 860.0f;
    content.setTransform (juce::AffineTransform::scale (scale));
    content.setBounds (0, 0, 860, 580); // triggers layoutContent() once (bounds don't change
                                        // again -- only the transform does on later resizes)
    proc.setEditorScale (scale);
}

void SpectralHoldEditor::layoutContent()
{
    auto r = content.getLocalBounds();

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

    // persistent row: the performance knobs, always visible (7 slots, matching P1's host
    // page -- see agent-wiki/plan-roadmap.md B0)
    auto row1 = controls.removeFromTop ((controls.getHeight() - 28) / 2);
    Knob* row1Knobs[] = { &feed, &loss, &ewLocation, &dryWet, &output, &limThreshold, &limRelease };
    layoutRow (row1, row1Knobs, 7);

    // tab strip -- Preset (B9) hugs the right; the tab strip's own width is generous (was
    // ~281px/tab for three short labels, ~211px/tab now with four), so 150px for the combo
    // leaves plenty for all four.
    auto tabs = controls.removeFromTop (28).reduced (0, 2);
    presetBox.setBounds (tabs.removeFromRight (150).reduced (4, 0));
    const int tw = tabs.getWidth() / 4;
    shaperTab.setBounds    (tabs.removeFromLeft (tw).reduced (2, 0));
    harmonizeTab.setBounds (tabs.removeFromLeft (tw).reduced (2, 0));
    reverbTab.setBounds    (tabs.removeFromLeft (tw).reduced (2, 0));
    performTab.setBounds   (tabs.reduced (2, 0));

    // tabbed row: all four share the same area; visibility picks the active one
    // (layoutRow takes its area by value, so each call below works on its own copy of
    // `controls` -- they don't consume it from each other).
    Knob* shaperKnobs[]    = { &shapeAmt, &shape, &shapeFreq, &shapeWidth, &shapeCount, &shapeLevel, &shapeMode };
    Knob* harmonizeKnobs[] = { &harmonize, &harmWidth, &harmonic };
    Knob* reverbKnobs[]    = { &revMix, &revDecay, &revDamp, &revSize, &revPredelay, &revMetal, &revFeed };
    Knob* performKnobs[]   = { &transpose, &transposeGlide, &spread, &phaseNoise };
    layoutRow (controls, shaperKnobs,    7);
    layoutRow (controls, harmonizeKnobs, 3);
    layoutRow (controls, reverbKnobs,    7);
    // Perform (agent-wiki/plan-uifix.md U3): Snap toggle carved from the right, same
    // pattern the old Freeze button used (commit 5805a2a); 4 knobs fill the rest -- well
    // within the row budget the shaper tab's 7 knobs already prove out.
    {
        auto performArea = controls;
        auto snapCell = performArea.removeFromRight (performArea.getWidth() / 8);
        snapButton.setBounds (snapCell.withSizeKeepingCentre (
            juce::jmax (40, snapCell.getWidth() - 8), 28));
        layoutRow (performArea, performKnobs, 4);
    }

    // Utility row width budget (bottom is ~844px, see agent-wiki/build-and-test.md's note
    // on why this can't be visually verified here -- checked by adding up these constants
    // against bottom's actual width, not by eyeballing): right 80+50+120+44=294, left
    // 90+46+50=186, total 480 of 844 (364px margin) -- Transpose/Phase Noise moved to the
    // Perform tab (U3), so this row is no longer tight.
    sizeBox.setBounds (bottom.removeFromRight (80));
    sizeLabel.setBounds (bottom.removeFromRight (50));
    liveButton.setBounds (bottom.removeFromLeft (90));
    keepButton.setBounds (bottom.removeFromLeft (46));
    undoButton.setBounds (bottom.removeFromLeft (50));
    brushSizeSlider.setBounds (bottom.removeFromRight (120));
    brushLabel.setBounds (bottom.removeFromRight (44));
}
