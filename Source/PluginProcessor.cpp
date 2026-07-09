#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr float kLimAttMs = 5.0f;     // limiter attack (catch peaks) -- fixed, not a param

    // Shape names for the display textbox (see ShapeCurves.h - kept as plain strings here
    // rather than including ShapeCurves.h just for the name list).
    const juce::StringArray kShapeNames { "Level", "Sigmoid", "Spikes", "Harmonics", "Sine" };

    juce::String shapeValueToString (float value, int)
    {
        const float v = juce::jlimit (0.0f, (float) (kShapeNames.size() - 1), value);
        const int   i0 = (int) std::floor (v);
        const int   i1 = juce::jmin (i0 + 1, kShapeNames.size() - 1);
        const float frac = v - (float) i0;
        if (frac < 0.05f || i0 == i1)
            return kShapeNames[i0];
        if (frac > 0.95f)
            return kShapeNames[i1];
        return kShapeNames[i0] + ">" + kShapeNames[i1];
    }
}

SpectralHoldProcessor::SpectralHoldProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pFeed   = apvts.getRawParameterValue ("feed");
    pLoss   = apvts.getRawParameterValue ("loss");
    pEwLocation = apvts.getRawParameterValue ("ewLocation");
    pDryWet = apvts.getRawParameterValue ("dryWet");
    pOutput = apvts.getRawParameterValue ("output");
    pPhaseNoise = apvts.getRawParameterValue ("phaseNoiseAmt");
    pTranspose  = apvts.getRawParameterValue ("transpose");
    pSpread     = apvts.getRawParameterValue ("spread");
    pLimThreshold = apvts.getRawParameterValue ("limThreshold");
    pLimRelease   = apvts.getRawParameterValue ("limRelease");

    pShapeAmt   = apvts.getRawParameterValue ("shapeAmt");
    pShapeMode  = apvts.getRawParameterValue ("shapeMode");
    pShape      = apvts.getRawParameterValue ("shape");
    pShapeFreq  = apvts.getRawParameterValue ("shapeFreq");
    pShapeWidth = apvts.getRawParameterValue ("shapeWidth");
    pShapeCount = apvts.getRawParameterValue ("shapeCount");
    pShapeLevel = apvts.getRawParameterValue ("shapeLevel");

    pHarmonize  = apvts.getRawParameterValue ("harmonize");
    pHarmWidth  = apvts.getRawParameterValue ("harmWidth");
    pHarmonic   = apvts.getRawParameterValue ("harmonic");

    pRevMix      = apvts.getRawParameterValue ("revMix");
    pRevDecay    = apvts.getRawParameterValue ("revDecay");
    pRevSize     = apvts.getRawParameterValue ("revSize");
    pRevDamp     = apvts.getRawParameterValue ("revDamp");
    pRevPredelay = apvts.getRawParameterValue ("revPredelay");
    pRevMetal    = apvts.getRawParameterValue ("revMetal");
    pRevFeed     = apvts.getRawParameterValue ("revFeed");
}

juce::AudioProcessorValueTreeState::ParameterLayout SpectralHoldProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    // Creation order = host page order (Push/Maschine bank 8 consecutive params per
    // page). Regrouped by agent-wiki/plan-roadmap.md B0 (was plan-fixes.md §9's three
    // pages of 8):
    //   P1 "Hold":   feed, loss, ewLocation, dryWet, output, limThreshold, limRelease
    //   P2 "Shaper": shapeAmt, shape, shapeFreq, shapeWidth, shapeCount, shapeLevel,
    //                shapeMode, harmonize   (harmonize closing the sculpt page is the
    //                accepted compromise to hit 8/8/8)
    //   P3 "Space":  harmWidth, harmonic, revMix, revDecay, revDamp, revSize,
    //                revPredelay, revMetal
    //   P4 "Perform" (partial, final target order): transpose, spread, phaseNoiseAmt,
    //                revFeed (+ morph if B11 lands) -- 4/8, accepted partial page.
    // Parameter IDs are unchanged by this grouping -- state restores by ID, not index.

    // --- P1: Hold ---
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "feed", 1 }, "Feed",
        NormalisableRange<float> (0.0f, 1.0f), 0.5f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "loss", 1 }, "Loss",
        NormalisableRange<float> (0.0f, 1.0f), 0.2f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "ewLocation", 1 }, "E<->W",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f)); // listener/recorder position, East(0,
                                                        // default = legacy) .. West(1)

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "dryWet", 1 }, "Dry/Wet",
        NormalisableRange<float> (0.0f, 1.0f), 1.0f)); // 1 = wet-only (today's behavior)

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (0.0f, 2.0f), 1.0f)); // output level (linear gain)

    // Output limiter (post-everything safety ceiling; see agent-wiki/dsp-design.md)
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "limThreshold", 1 }, "Limiter Threshold",
        NormalisableRange<float> (-24.0f, 0.0f), 0.0f)); // dB ceiling

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "limRelease", 1 }, "Limiter Release",
        NormalisableRange<float> (50.0f, 5000.0f, 0.0f, 0.4f), 1200.0f)); // ms, skewed

    // --- P2: Shaper (replaces the old Filter + Compress; see agent-wiki/dsp-design.md) ---
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeAmt", 1 }, "Shape Amount",
        NormalisableRange<float> (0.0f, 1.0f), 1.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shape", 1 }, "Shape",
        NormalisableRange<float> (0.0f, 4.0f), 0.0f, // Level/Sigmoid/Spikes/Harmonics/Sine
        AudioParameterFloatAttributes().withStringFromValueFunction (shapeValueToString)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeFreq", 1 }, "Shape Freq",
        NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.25f), 1000.0f)); // skew = log-ish

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeWidth", 1 }, "Shape Width",
        NormalisableRange<float> (0.0f, 1.0f), 0.5f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeCount", 1 }, "Shape Count",
        NormalisableRange<float> (0.0f, 1.0f), 1.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeLevel", 1 }, "Shape Level",
        NormalisableRange<float> (-1.0f, 1.0f), 0.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeMode", 1 }, "Shape Mode",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f)); // 0 = momentary, 1 = permanent

    // Harmonize (coupled-oscillator tone interaction; see agent-wiki/harmonize.md).
    // Master amount closes page 2 (accepted compromise, see plan-fixes.md §9).
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "harmonize", 1 }, "Harmonize",
        NormalisableRange<float> (0.0f, 0.1f), 0.0f));

    // --- P3: Space (harmonize character + the output reverb) ---
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "harmWidth", 1 }, "Harm Width",
        NormalisableRange<float> (0.01f, 3.0f, 0.0f, 0.4f), 0.5f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "harmonic", 1 }, "Harmonic",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f));

    // Output reverb (post-fader, pre-limiter; see agent-wiki/plan-reverb.md)
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revMix", 1 }, "Reverb Mix",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f)); // 0 = bit-exact dry

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revDecay", 1 }, "Reverb Decay",
        NormalisableRange<float> (0.0f, 1.0f), 0.5f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revDamp", 1 }, "Reverb Damp",
        NormalisableRange<float> (0.0f, 1.0f), 0.3f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revSize", 1 }, "Reverb Size",
        NormalisableRange<float> (0.5f, 2.0f), 1.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revPredelay", 1 }, "Reverb Predelay",
        NormalisableRange<float> (0.0f, 250.0f, 0.0f, 0.35f), 20.0f)); // ms, log-ish skew

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revMetal", 1 }, "Reverb Metal",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f)); // less diffusion, no LFO smear

    // --- P4 "Perform" (partial, interim): final target order per agent-wiki/plan-roadmap.md
    // B0 -- Transpose, Spread, Phase Noise, Reverb Feed (+ morph if B11 lands).
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "transpose", 1 }, "Transpose",
        NormalisableRange<float> (-12.0f, 12.0f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("st")));

    // Stereo spread (agent-wiki/plan-roadmap.md B5): momentary per-bin L/R gain, never
    // touches the held state. spread=0 is a no-op on both mono and stereo buses.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "spread", 1 }, "Spread",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f));

    // Continuous Phase Noise (agent-wiki/plan-roadmap.md B4): replaces the old bool
    // `phaseNoise`. Migrated from old sessions in setStateInformation() (legacy "on" -> 0.3).
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "phaseNoiseAmt", 1 }, "Phase Noise",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revFeed", 1 }, "Reverb Feed",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f));

    return layout;
}

void SpectralHoldProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    for (auto& e : engines)
    {
        e.prepare (sampleRate, kMaxFftOrder);
        e.setOrder (fftOrder.load());
    }
    // setOrder is deferred to the next frame; latency is still fftSize (or 0 in live mode).
    setLatencySamples (liveMode ? 0 : (1 << fftOrder.load()));

    limAttCoef = 1.0f - std::exp (-1.0f / (kLimAttMs * 0.001f * (float) sampleRate));
    limRelCoef = 1.0f - std::exp (-1.0f / (pLimRelease->load() * 0.001f * (float) sampleRate));
    limEnv = 0.0f;
    limGain = 1.0f;
    prepared = true;

    reverb.prepare (sampleRate);
    revMono.assign ((size_t) samplesPerBlock, 0.0f);
    revWetL.assign ((size_t) samplesPerBlock, 0.0f);
    revWetR.assign ((size_t) samplesPerBlock, 0.0f);
    revFeedBuf.assign ((size_t) samplesPerBlock, 0.0f);
    prevRevMix  = pRevMix->load();
    prevRevFeed = pRevFeed->load();

    for (auto& d : dryDelay)
        d.prepare (1 << kMaxFftOrder, samplesPerBlock);
    dryScratch.assign ((size_t) juce::jmax (samplesPerBlock, 16), 0.0f);
}

bool SpectralHoldProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void SpectralHoldProcessor::setFftOrder (int order)
{
    order = juce::jlimit (kMinFftOrder, kMaxFftOrder, order);
    fftOrder.store (order);
    if (prepared)
    {
        for (auto& e : engines)
            e.setOrder (order);
        setLatencySamples (liveMode ? 0 : (1 << order));
    }
}

void SpectralHoldProcessor::setLiveMode (bool b)
{
    liveMode = b;
    if (prepared)
        setLatencySamples (liveMode ? 0 : (1 << fftOrder.load()));
}

void SpectralHoldProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    // Transpose MIDI (agent-wiki/plan-roadmap.md B3): monophonic, last-note priority; a
    // note-off only clears midiNote if it matches the currently held note (so releasing an
    // older, already-superseded note doesn't cancel the newer one).
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn())
            midiNote = msg.getNoteNumber();
        else if (msg.isNoteOff() && msg.getNoteNumber() == midiNote)
            midiNote = -1;
    }

    SpectralEngine::Params p;
    p.feed       = pFeed->load();
    p.loss       = pLoss->load();
    p.ewLocation = pEwLocation->load();
    p.phaseNoise = pPhaseNoise->load();
    // note 60 (middle C) = no shift, so playing with no MIDI input matches the knob alone.
    // Not clamped to the knob's +/-12 st DAW range -- playing further from middle C should
    // keep transposing further, not flatten out at an octave.
    p.transpose  = pTranspose->load() + (float) (midiNote >= 0 ? midiNote - 60 : 0);
    p.harmonize  = pHarmonize->load();
    p.harmWidth  = pHarmWidth->load();
    p.harmonic   = pHarmonic->load();

    p.shapeAmt   = pShapeAmt->load();
    p.shapeMode  = pShapeMode->load();
    p.shape      = pShape->load();
    p.shapeFreq  = pShapeFreq->load();
    p.shapeWidth = pShapeWidth->load();
    p.shapeCount = pShapeCount->load();
    p.shapeLevel = pShapeLevel->load();
    p.revFeed    = pRevFeed->load();
    p.spread     = numCh > 1 ? pSpread->load() : 0.0f; // agent-wiki/plan-roadmap.md B5:
                                                        // no L/R to spread on a mono bus

    // --- global dry/wet: capture and mix the latency-aligned dry input. The rings
    // are always fed (cheap) so turning the knob down never reads stale audio, but at
    // dryWet=1 (the default) the mix itself is skipped -- output stays bit-exact.
    const float dryWet = pDryWet->load();
    if ((int) dryScratch.size() < numSamples)
        dryScratch.resize ((size_t) numSamples);

    // Aux (reverb-feedback) tap: revFeedBuf holds the PREVIOUS block's reverb wet mono
    // (filled at the end of this function). One-block feedback delay, inherent and fine.
    // Grow-and-zero on a host block-size increase so stale/garbage samples are never read.
    if ((int) revFeedBuf.size() < numSamples)
        revFeedBuf.resize ((size_t) numSamples, 0.0f);
    const float* auxTap = (p.revFeed > 0.0f) ? revFeedBuf.data() : nullptr;

    for (int ch = 0; ch < juce::jmin (numCh, (int) engines.size()); ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        auto& eng = engines[(size_t) ch];
        p.spreadSign = (ch == 0) ? 1 : -1; // agent-wiki/plan-roadmap.md B5
        dryDelay[(size_t) ch].process (d, dryScratch.data(), numSamples, eng.getLatency());
        eng.process (d, auxTap, d, numSamples, p); // in place: capture dry BEFORE this
        if (dryWet < 1.0f)
        {
            const float wetG = dryWet, dryG = 1.0f - dryWet;
            for (int n = 0; n < numSamples; ++n)
                d[n] = d[n] * wetG + dryScratch[(size_t) n] * dryG;
        }
    }

    // output level (applied before the limiter so it still protects +/-1)
    const float outGain = pOutput->load();
    if (outGain != 1.0f)
        buffer.applyGain (outGain);

    // --- output reverb (post-fader, pre-limiter). Runs whenever the audible mix OR the
    // feedback path is in use -- revFeed needs the wet tail even at revMix=0 ("reverb as
    // silent hold-exciter"). Hard-bypassed only when BOTH are 0, so old sessions (and the
    // defaults) stay bit-exact dry; the tail resets when both fall to 0 so no stale tail
    // plays -- or leaks into the feedback tap -- when either comes back up.
    const float revMix  = pRevMix->load();
    const float revFeed = p.revFeed;
    if (revMix <= 0.0f && revFeed <= 0.0f)
    {
        if (prevRevMix > 0.0f || prevRevFeed > 0.0f)
            reverb.reset();
        std::fill (revFeedBuf.begin(), revFeedBuf.begin() + numSamples, 0.0f);
    }
    else
    {
        if ((int) revMono.size() < numSamples)
        {
            revMono.resize ((size_t) numSamples);
            revWetL.resize ((size_t) numSamples);
            revWetR.resize ((size_t) numSamples);
        }

        for (int n = 0; n < numSamples; ++n)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                sum += buffer.getReadPointer (ch)[n];
            revMono[(size_t) n] = numCh > 0 ? sum / (float) numCh : 0.0f;
        }

        reverb.setParams (pRevDecay->load(), pRevSize->load(), pRevDamp->load(),
                           pRevPredelay->load() * 0.001f, pRevMetal->load());
        reverb.process (revMono.data(), revWetL.data(), revWetR.data(), numSamples);

        // feedback tap for NEXT block's aux input (see revFeedBuf declaration)
        for (int n = 0; n < numSamples; ++n)
            revFeedBuf[(size_t) n] = 0.5f * (revWetL[(size_t) n] + revWetR[(size_t) n]);

        if (revMix > 0.0f)
        {
            const float dryGain = std::cos (revMix * juce::MathConstants<float>::halfPi);
            const float wetGain = std::sin (revMix * juce::MathConstants<float>::halfPi);
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                const float* wet = (ch % 2 == 0) ? revWetL.data() : revWetR.data();
                for (int n = 0; n < numSamples; ++n)
                    d[n] = d[n] * dryGain + wet[n] * wetGain;
            }
        }
    }
    prevRevMix  = revMix;
    prevRevFeed = revFeed;

    // --- output limiter: lower gain only when peak would exceed the threshold.
    // Release recomputed per block from the param (attack stays fixed, kLimAttMs).
    limRelCoef = 1.0f - std::exp (-1.0f / (pLimRelease->load() * 0.001f * (float) getSampleRate()));
    const float thr = juce::Decibels::decibelsToGain (pLimThreshold->load());

    for (int n = 0; n < numSamples; ++n)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            peak = juce::jmax (peak, std::abs (buffer.getReadPointer (ch)[n]));

        const float coef = (peak > limEnv) ? limAttCoef : limRelCoef;
        limEnv += (peak - limEnv) * coef;

        const float target = (limEnv > thr) ? (thr / limEnv) : 1.0f;
        const float gcoef  = (target < limGain) ? limAttCoef : limRelCoef;
        limGain += (target - limGain) * gcoef;

        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[n] *= limGain;
    }
}

int SpectralHoldProcessor::getDisplaySnapshot (std::vector<float>& magL, std::vector<float>& phaseL,
                                               std::vector<float>& magR, std::vector<float>& phaseR,
                                               double& sr, int& size)
{
    sr   = engines[0].getSampleRate();
    size = engines[0].getFftSize();
    const int n0 = engines[0].copyDisplay (magL, phaseL);
    if (n0 == 0)
        return 0;

    // Split stereo (agent-wiki/plan-uifix.md U1): each channel keeps its own magnitude AND
    // phase -- the old per-bin max(ch0,ch1) merge made the tone look "ribbed" whenever the
    // two channels differed (real stereo input, Phase Noise's independent per-engine RNG,
    // Spread's per-bin +/- gains), since adjacent bins could jump between two different
    // leakage curves. Falls back to channel 0 in both slots on a mono bus, or if channel 1's
    // snapshot is busy / a mismatched size (mid-reconfigure) -- a symmetric view either way.
    if (getMainBusNumOutputChannels() > 1 && engines[1].copyDisplay (magR, phaseR) == n0)
        return n0;

    magR = magL;
    phaseR = phaseL;
    return n0;
}

void SpectralHoldProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("fftOrder", fftOrder.load(), nullptr);
    state.setProperty ("liveMode", liveMode, nullptr);
    state.setProperty ("uiTab", uiTab, nullptr);
    state.setProperty ("keepSound", keepSound, nullptr);
    state.setProperty ("editorScale", editorScale, nullptr);

    // Hold serialization (agent-wiki/plan-roadmap.md B1): the held spectral state is the
    // whole product, so it's saved inside the session unless the user turns Keep off.
    // gzip'd + base64'd into a ValueTree property, one per engine/channel.
    if (keepSound && prepared)
    {
        const juce::ScopedLock sl (getCallbackLock()); // writeHold() reads live audio-thread state
        for (size_t ch = 0; ch < engines.size(); ++ch)
        {
            juce::MemoryOutputStream raw;
            engines[ch].writeHold (raw);

            juce::MemoryOutputStream gz;
            {
                juce::GZIPCompressorOutputStream gzOut (gz, 9);
                gzOut.write (raw.getData(), raw.getDataSize());
            } // flushes/finalises the gzip stream on scope exit

            state.setProperty ("hold" + juce::String ((int) ch),
                                juce::Base64::toBase64 (gz.getData(), gz.getDataSize()), nullptr);
        }
    }

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, dest);
}

void SpectralHoldProcessor::setStateInformation (const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, size));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    // Phase Noise migration (agent-wiki/plan-roadmap.md B4): old sessions saved the bool
    // `phaseNoise`. Must be read **before** replaceState() below -- it redirects `state`
    // onto this same (reference-counted) tree and appends any missing PARAM children
    // (including a fresh, default `phaseNoiseAmt`) in place, so checking afterwards would
    // always see the new param and never detect an old session.
    bool hadOldPhaseNoiseOn = false;
    for (const auto& child : tree)
        if (child.hasType ("PARAM") && child.getProperty ("id").toString() == "phaseNoise"
            && (float) child.getProperty ("value") > 0.5f)
            hadOldPhaseNoiseOn = true;

    apvts.replaceState (tree);

    if (hadOldPhaseNoiseOn)
        if (auto* param = apvts.getParameter ("phaseNoiseAmt"))
            param->setValueNotifyingHost (0.3f); // 0..1 range, so 0.3 of range == 0.3

    setLiveMode    ((bool) tree.getProperty ("liveMode", false));
    uiTab         = juce::jlimit (0, 2, (int) tree.getProperty ("uiTab", 0));
    keepSound     = (bool) tree.getProperty ("keepSound", true);
    setEditorScale ((float) tree.getProperty ("editorScale", 1.0f));

    // order must land before the hold restore below -- queueHoldRestore()'s blob only
    // applies once the engine's order matches the blob's (see applyPendingHold()).
    const int ord = (int) tree.getProperty ("fftOrder", fftOrder.load());
    setFftOrder (ord);

    // Missing "holdN" properties = an old session (or Keep was off when it was saved) --
    // no-op, engines just start from silence as before B1.
    for (size_t ch = 0; ch < engines.size(); ++ch)
    {
        const juce::String key = "hold" + juce::String ((int) ch);
        if (! tree.hasProperty (key))
            continue;

        juce::MemoryOutputStream gzOut;
        if (! juce::Base64::convertFromBase64 (gzOut, tree.getProperty (key).toString()))
            continue;

        juce::MemoryInputStream gzIn (gzOut.getMemoryBlock(), false);
        juce::GZIPDecompressorInputStream gzDec (gzIn);
        juce::MemoryOutputStream raw;
        raw.writeFromInputStream (gzDec, -1);

        engines[ch].queueHoldRestore (raw.getData(), raw.getDataSize());
    }
}

void SpectralHoldProcessor::snapshotHold()
{
    if (! prepared)
        return;

    juce::MemoryOutputStream raw0, raw1;
    {
        // writeHold() reads live audio-thread state with no lock of its own -- see
        // PluginProcessor::getStateInformation, same pattern reused here.
        const juce::ScopedLock sl (getCallbackLock());
        engines[0].writeHold (raw0);
        engines[1].writeHold (raw1);
    }

    undoRing[(size_t) undoWriteIdx] = { juce::MemoryBlock (raw0.getData(), raw0.getDataSize()),
                                         juce::MemoryBlock (raw1.getData(), raw1.getDataSize()) };
    undoWriteIdx = (undoWriteIdx + 1) % (int) undoRing.size();
    undoCount = juce::jmin ((int) undoRing.size(), undoCount + 1);
}

bool SpectralHoldProcessor::undoHold()
{
    if (undoCount == 0)
        return false;

    undoWriteIdx = (undoWriteIdx - 1 + (int) undoRing.size()) % (int) undoRing.size();
    --undoCount;

    const auto& snap = undoRing[(size_t) undoWriteIdx];
    engines[0].queueHoldRestore (snap.first.getData(), snap.first.getSize());
    engines[1].queueHoldRestore (snap.second.getData(), snap.second.getSize());
    return true;
}

juce::AudioProcessorEditor* SpectralHoldProcessor::createEditor()
{
    return new SpectralHoldEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpectralHoldProcessor();
}
