#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "SpectralEngine.h"

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
    double getTailLengthSeconds() const override { return 0.0; }

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
    // - saveWithSound: include the held spectral state in the saved preset.
    void setLiveMode (bool b);
    bool getLiveMode() const      { return liveMode; }
    void setSaveWithSound (bool b) { saveWithSound = b; }
    bool getSaveWithSound() const  { return saveWithSound; }

    // Snapshot for the spectrum display (channel 0). Returns numBins or 0.
    int getDisplaySnapshot (std::vector<float>& mag, std::vector<float>& phase,
                            double& sr, int& size);

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
    std::atomic<float>* pOutput = nullptr;
    std::atomic<float>* pPhaseNoise = nullptr;

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
    bool saveWithSound = false;
    juce::CriticalSection audioStateLock; // guards engine state vs. preset restore

    // Held audio state restored from a preset but waiting for prepareToPlay (the host may call
    // setStateInformation BEFORE prepareToPlay, when the engine buffers don't exist yet).
    juce::MemoryBlock pendingAudioState;
    void applyAudioState (const juce::MemoryBlock&);

    // slow linked limiter state
    float limEnv  = 0.0f;
    float limGain = 1.0f;
    float limAttCoef = 0.0f, limRelCoef = 0.0f;

    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralHoldProcessor)
};
