#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "SpectralEngine.h"
#include "PlateReverb.h"
#include "DryDelay.h"

class SpectralHoldProcessor : public juce::AudioProcessor
{
public:
    SpectralHoldProcessor();
    ~SpectralHoldProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Spectral Hold"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override
    {
        return (pRevMix != nullptr && pRevMix->load() > 0.0f) ? 10.0 : 0.0;
    }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

    // FFT size is GUI-only (not exposed to the DAW). order = log2(size).
    static constexpr int kMaxFftOrder = 13; // 8192
    static constexpr int kMinFftOrder = 10; // 1024
    void setFftOrder (int order);
    int  getFftOrder() const { return fftOrder.load(); }

    // GUI-only switches (persisted in state, not DAW parameters):
    // - liveMode: report 0 latency (PDC) so the host doesn't delay-compensate, for live use.
    void setLiveMode (bool b);
    bool getLiveMode() const      { return liveMode; }
    // Active editor tab (0 shaper, 1 harmonize, 2 reverb). GUI-only, persisted in state.
    void setUiTab (int t) { uiTab = t; }
    int  getUiTab() const { return uiTab; }

    // Snapshot for the spectrum display (channel 0). Returns numBins or 0.
    int getDisplaySnapshot (std::vector<float>& mag, std::vector<float>& phase,
                            double& sr, int& size);

    // Harmonize influence snapshot (channel 0): peak freqs/weights/drifts. Returns count
    // (0 = harmonize off), or -1 if busy (keep last frame).
    int getHarmonizePeaks (std::vector<float>& freq, std::vector<float>& weight, std::vector<float>& drift)
    {
        return engines[0].copyPeaks (freq, weight, drift);
    }

    // GUI brush edit: scale the held spectrum around centreFreqHz (all channels).
    // strength in [-1..+1]: +boost, 0 none, -cut. sigmaOct = brush size (GUI-only).
    void applySpectralBrush (float centreFreqHz, float strength, float sigmaOct)
    {
        for (auto& e : engines)
            e.queueBrush (centreFreqHz, strength, sigmaOct);
    }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::array<SpectralEngine, 2> engines;
    std::atomic<int> fftOrder { 12 }; // default 4096

    // cached param pointers
    std::atomic<float>* pFeed   = nullptr;
    std::atomic<float>* pLoss   = nullptr;
    std::atomic<float>* pEwLocation = nullptr;
    std::atomic<float>* pDryWet = nullptr;
    std::atomic<float>* pOutput = nullptr;
    std::atomic<float>* pPhaseNoise = nullptr;
    std::atomic<float>* pHarmonize = nullptr;
    std::atomic<float>* pHarmWidth = nullptr;
    std::atomic<float>* pHarmonic  = nullptr;

    // global dry/wet: latency-aligned dry path (see DryDelay.h)
    std::array<DryDelay, 2> dryDelay;
    std::vector<float> dryScratch;

    // output reverb (post-fader, pre-limiter; see agent-wiki/plan-reverb.md)
    std::atomic<float>* pRevMix      = nullptr;
    std::atomic<float>* pRevDecay    = nullptr;
    std::atomic<float>* pRevSize     = nullptr;
    std::atomic<float>* pRevDamp     = nullptr;
    std::atomic<float>* pRevPredelay = nullptr;
    PlateReverb reverb;
    std::vector<float> revMono, revWetL, revWetR;
    float prevRevMix = 0.0f; // to detect the 1->0 transition and reset the tail

    // spectral shaper (replaces the old filter + compress)
    std::atomic<float>* pShapeAmt   = nullptr;
    std::atomic<float>* pShapeMode  = nullptr;
    std::atomic<float>* pShape      = nullptr;
    std::atomic<float>* pShapeFreq  = nullptr;
    std::atomic<float>* pShapeWidth = nullptr;
    std::atomic<float>* pShapeCount = nullptr;
    std::atomic<float>* pShapeLevel = nullptr;

    // GUI-only switches (persisted manually, see get/setStateInformation)
    bool liveMode = false;
    int  uiTab = 0;

    // slow linked limiter state
    float limEnv  = 0.0f;
    float limGain = 1.0f;
    float limAttCoef = 0.0f, limRelCoef = 0.0f;

    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralHoldProcessor)
};
