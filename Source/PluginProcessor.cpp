#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr float kLimAttMs = 5.0f;     // limiter attack (catch peaks)
    constexpr float kLimRelMs = 1200.0f;  // limiter release (slow envelope)
}

SpectralHoldProcessor::SpectralHoldProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pFeed   = apvts.getRawParameterValue ("feed");
    pLoss   = apvts.getRawParameterValue ("loss");
    pOutput = apvts.getRawParameterValue ("output");
    pPhaseNoise = apvts.getRawParameterValue ("phaseNoise");

    pShapeAmt   = apvts.getRawParameterValue ("shapeAmt");
    pShapeMode  = apvts.getRawParameterValue ("shapeMode");
    pShape      = apvts.getRawParameterValue ("shape");
    pShapeFreq  = apvts.getRawParameterValue ("shapeFreq");
    pShapeWidth = apvts.getRawParameterValue ("shapeWidth");
    pShapeCount = apvts.getRawParameterValue ("shapeCount");
    pShapeLevel = apvts.getRawParameterValue ("shapeLevel");
}

juce::AudioProcessorValueTreeState::ParameterLayout SpectralHoldProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "feed", 1 }, "Feed",
        NormalisableRange<float> (0.0f, 1.0f), 0.5f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "loss", 1 }, "Loss",
        NormalisableRange<float> (0.0f, 1.0f), 0.2f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (0.0f, 2.0f), 1.0f)); // output level (linear gain)

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "phaseNoise", 1 }, "Phase Noise", false));

    // Spectral shaper (replaces the old Filter + Compress; see agent-wiki/dsp-design.md)
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeAmt", 1 }, "Shape Amount",
        NormalisableRange<float> (0.0f, 1.0f), 1.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shapeMode", 1 }, "Shape Mode",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f)); // 0 = momentary, 1 = permanent

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "shape", 1 }, "Shape",
        NormalisableRange<float> (0.0f, 3.0f), 0.0f)); // Level/Sigmoid/Spikes/Sine

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

    return layout;
}

void SpectralHoldProcessor::prepareToPlay (double sampleRate, int)
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

    // apply any held audio state that was restored from a preset before we were prepared
    if (pendingAudioState.getSize() > 0)
    {
        applyAudioState (pendingAudioState);
        pendingAudioState.reset();
    }
}

void SpectralHoldProcessor::applyAudioState (const juce::MemoryBlock& mb)
{
    juce::MemoryInputStream is (mb, false);
    const int nEng = is.readInt();
    const juce::ScopedLock sl (audioStateLock);
    for (int i = 0; i < nEng && i < (int) engines.size(); ++i)
        engines[(size_t) i].readAudioState (is);
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
    p.phaseNoise = pPhaseNoise->load() > 0.5f;

    p.shapeAmt   = pShapeAmt->load();
    p.shapeMode  = pShapeMode->load();
    p.shape      = pShape->load();
    p.shapeFreq  = pShapeFreq->load();
    p.shapeWidth = pShapeWidth->load();
    p.shapeCount = pShapeCount->load();
    p.shapeLevel = pShapeLevel->load();

    {
        const juce::ScopedLock sl (audioStateLock); // block preset restore mid-process
        for (int ch = 0; ch < juce::jmin (numCh, (int) engines.size()); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            engines[(size_t) ch].process (d, d, numSamples, p);
        }
    }

    // output level (applied before the limiter so it still protects +/-1)
    const float outGain = pOutput->load();
    if (outGain != 1.0f)
        buffer.applyGain (outGain);

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
    state.setProperty ("saveWithSound", saveWithSound, nullptr);

    if (saveWithSound)
    {
        juce::MemoryBlock mb;
        {
            juce::MemoryOutputStream os (mb, false);
            os.writeInt ((int) engines.size());
            const juce::ScopedLock sl (audioStateLock);
            for (auto& e : engines)
                e.writeAudioState (os);
        }
        state.setProperty ("audioState", juce::var (mb), nullptr); // base64 in the XML
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
    apvts.replaceState (tree);

    setLiveMode    ((bool) tree.getProperty ("liveMode", false));
    saveWithSound = (bool) tree.getProperty ("saveWithSound", false);

    const int ord = (int) tree.getProperty ("fftOrder", fftOrder.load());
    setFftOrder (ord);

    pendingAudioState.reset();
    if (auto* mbv = tree.getPropertyPointer ("audioState"))
        if (auto* mb = mbv->getBinaryData())
        {
            // The host may call this BEFORE prepareToPlay (engine buffers not allocated yet),
            // so stash it and apply once prepared. If we're already prepared, apply now.
            pendingAudioState = *mb;
            if (prepared)
            {
                applyAudioState (pendingAudioState);
                pendingAudioState.reset();
            }
        }
}

juce::AudioProcessorEditor* SpectralHoldProcessor::createEditor()
{
    return new SpectralHoldEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpectralHoldProcessor();
}
