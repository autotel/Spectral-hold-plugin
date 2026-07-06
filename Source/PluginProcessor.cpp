#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr float kLimAttMs = 5.0f;     // limiter attack (catch peaks)
    constexpr float kLimRelMs = 1200.0f;  // limiter release (slow envelope)
    constexpr float kRevFeedScale = 0.5f; // headroom on the reverb->hold feedback loop
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
    pPhaseNoise = apvts.getRawParameterValue ("phaseNoise");

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
    pRevFeed     = apvts.getRawParameterValue ("revFeed");
}

juce::AudioProcessorValueTreeState::ParameterLayout SpectralHoldProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    // Creation order = host page order (Push/Maschine bank 8 consecutive params per
    // page). Page 1 "Hold" (8): feed, loss, dryWet, output, phaseNoise, harmonize,
    // harmWidth, harmonic. Page 2 "Shaper" (7, revMix spills into its 8th slot -
    // accepted, see plan-integration.md section 3). Page 3 "Reverb" (rest).
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "feed", 1 }, "Feed",
        NormalisableRange<float> (0.0f, 1.0f), 0.5f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "loss", 1 }, "Loss",
        NormalisableRange<float> (0.0f, 1.0f), 0.2f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "ewLocation", 1 }, "E<->W",
        NormalisableRange<float> (0.0f, 1.0f), 0.5f)); // East(0)..West(1) location field

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "dryWet", 1 }, "Dry/Wet",
        NormalisableRange<float> (0.0f, 1.0f), 1.0f)); // 1 = wet-only (today's behavior)

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (0.0f, 2.0f), 1.0f)); // output level (linear gain)

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "phaseNoise", 1 }, "Phase Noise", false));

    // Harmonize (coupled-oscillator tone interaction; see agent-wiki/harmonize.md)
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "harmonize", 1 }, "Harmonize",
        NormalisableRange<float> (0.0f, 0.1f), 0.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "harmWidth", 1 }, "Harm Width",
        NormalisableRange<float> (0.01f, 3.0f, 0.0f, 0.4f), 0.5f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "harmonic", 1 }, "Harmonic",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f));

    // Spectral shaper (replaces the old Filter + Compress; see agent-wiki/dsp-design.md)
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeAmt", 1 }, "Shape Amount",
        NormalisableRange<float> (0.0f, 1.0f), 1.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeMode", 1 }, "Shape Mode",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f)); // 0 = momentary, 1 = permanent

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shape", 1 }, "Shape",
        NormalisableRange<float> (0.0f, 4.0f), 0.0f)); // Level/Sigmoid/Spikes/Harmonics/Sine

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

    // Output reverb (post-fader, pre-limiter; see agent-wiki/plan-reverb.md)
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revMix", 1 }, "Reverb Mix",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f)); // 0 = bit-exact dry

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revDecay", 1 }, "Reverb Decay",
        NormalisableRange<float> (0.0f, 1.0f), 0.5f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revSize", 1 }, "Reverb Size",
        NormalisableRange<float> (0.5f, 2.0f), 1.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revDamp", 1 }, "Reverb Damp",
        NormalisableRange<float> (0.0f, 1.0f), 0.3f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revPredelay", 1 }, "Reverb Predelay",
        NormalisableRange<float> (0.0f, 250.0f, 0.0f, 0.35f), 20.0f)); // ms, log-ish skew

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "revFeed", 1 }, "Reverb Feed",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f)); // wet fed back into the hold input

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
    limRelCoef = 1.0f - std::exp (-1.0f / (kLimRelMs * 0.001f * (float) sampleRate));
    limEnv = 0.0f;
    limGain = 1.0f;
    prepared = true;

    reverb.prepare (sampleRate);
    revMono.assign ((size_t) samplesPerBlock, 0.0f);
    revWetL.assign ((size_t) samplesPerBlock, 0.0f);
    revWetR.assign ((size_t) samplesPerBlock, 0.0f);
    prevRevMix = pRevMix->load();

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

void SpectralHoldProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    SpectralEngine::Params p;
    p.feed       = pFeed->load();
    p.loss       = pLoss->load();
    p.ewLocation = pEwLocation->load();
    p.phaseNoise = pPhaseNoise->load() > 0.5f;
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

    // --- global dry/wet: capture and mix the latency-aligned dry input. The rings
    // are always fed (cheap) so turning the knob down never reads stale audio, but at
    // dryWet=1 (the default) the mix itself is skipped -- output stays bit-exact.
    const float dryWet = pDryWet->load();
    if ((int) dryScratch.size() < numSamples)
        dryScratch.resize ((size_t) numSamples);

    // revFeed: re-inject last block's reverb wet into the engines' input, AFTER the
    // dry capture (the dry path stays untouched input) and only while the reverb is
    // active. The tail gets re-captured by the hold and becomes part of the held sound.
    const float revFeedG = (pRevMix->load() > 0.0f && revFeedCount > 0)
                             ? pRevFeed->load() * kRevFeedScale : 0.0f;

    {
        for (int ch = 0; ch < juce::jmin (numCh, (int) engines.size()); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            auto& eng = engines[(size_t) ch];
            dryDelay[(size_t) ch].process (d, dryScratch.data(), numSamples, eng.getLatency());
            if (revFeedG > 0.0f)
            {
                // The feedback tap is pre-limiter, so the loop must bound itself:
                // clamp the injected wet to +/-1. Without this the loop can grow
                // exponentially at high decay/low loss (verified by the worst-case
                // stability test); with it, injection is no hotter than normal input.
                const float* wet = (ch % 2 == 0) ? revWetL.data() : revWetR.data();
                const int nInj = juce::jmin (numSamples, revFeedCount);
                for (int n = 0; n < nInj; ++n)
                    d[n] += revFeedG * juce::jlimit (-1.0f, 1.0f, wet[n]);
            }
            eng.process (d, d, numSamples, p); // in place: capture dry BEFORE this
            if (dryWet < 1.0f)
            {
                const float wetG = dryWet, dryG = 1.0f - dryWet;
                for (int n = 0; n < numSamples; ++n)
                    d[n] = d[n] * wetG + dryScratch[(size_t) n] * dryG;
            }
        }
    }

    // output level (applied before the limiter so it still protects +/-1)
    const float outGain = pOutput->load();
    if (outGain != 1.0f)
        buffer.applyGain (outGain);

    // --- output reverb (post-fader, pre-limiter). Hard-bypassed at mix=0 so old
    // sessions (and mix left at its default) stay bit-exact dry; the tail is reset
    // on the 1->0 transition so no stale tail plays when mix comes back up.
    const float revMix = pRevMix->load();
    if (revMix <= 0.0f)
    {
        if (prevRevMix > 0.0f)
            reverb.reset();
        revFeedCount = 0; // no stale wet injected when mix comes back up
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
                           pRevPredelay->load() * 0.001f);
        reverb.process (revMono.data(), revWetL.data(), revWetR.data(), numSamples);

        const float dryGain = std::cos (revMix * juce::MathConstants<float>::halfPi);
        const float wetGain = std::sin (revMix * juce::MathConstants<float>::halfPi);
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            const float* wet = (ch % 2 == 0) ? revWetL.data() : revWetR.data();
            for (int n = 0; n < numSamples; ++n)
                d[n] = d[n] * dryGain + wet[n] * wetGain;
        }
        revFeedCount = numSamples; // raw wet stays in revWetL/R for next block's revFeed
    }
    prevRevMix = revMix;

    // --- slow linked limiter: lower gain only when peak would exceed +/-1
    for (int n = 0; n < numSamples; ++n)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            peak = juce::jmax (peak, std::abs (buffer.getReadPointer (ch)[n]));

        const float coef = (peak > limEnv) ? limAttCoef : limRelCoef;
        limEnv += (peak - limEnv) * coef;

        const float target = (limEnv > 1.0f) ? (1.0f / limEnv) : 1.0f;
        const float gcoef  = (target < limGain) ? limAttCoef : limRelCoef;
        limGain += (target - limGain) * gcoef;

        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[n] *= limGain;
    }
}

int SpectralHoldProcessor::getDisplaySnapshot (std::vector<float>& mag, std::vector<float>& phase,
                                               double& sr, int& size)
{
    sr   = engines[0].getSampleRate();
    size = engines[0].getFftSize();
    return engines[0].copyDisplay (mag, phase);
}

void SpectralHoldProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("fftOrder", fftOrder.load(), nullptr);
    state.setProperty ("liveMode", liveMode, nullptr);
    state.setProperty ("uiTab", uiTab, nullptr);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, dest);
}

void SpectralHoldProcessor::setStateInformation (const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, size));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (tree);

    setLiveMode    ((bool) tree.getProperty ("liveMode", false));
    uiTab         = juce::jlimit (0, 2, (int) tree.getProperty ("uiTab", 0));

    const int ord = (int) tree.getProperty ("fftOrder", fftOrder.load());
    setFftOrder (ord);
}

juce::AudioProcessorEditor* SpectralHoldProcessor::createEditor()
{
    return new SpectralHoldEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpectralHoldProcessor();
}
