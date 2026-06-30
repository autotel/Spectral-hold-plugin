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
    std::atomic<float>* pFiltA  = nullptr;
    std::atomic<float>* pFiltT  = nullptr;
    std::atomic<float>* pAttack = nullptr;
    std::atomic<float>* pCompress = nullptr;
    std::atomic<float>* pPhaseNoise = nullptr;
    std::atomic<float>* pHarmonize = nullptr;
    std::atomic<float>* pHarmWidth = nullptr;
    std::atomic<float>* pHarmonic  = nullptr;

    // slow linked limiter state
    float limEnv  = 0.0f;
    float limGain = 1.0f;
    float limAttCoef = 0.0f, limRelCoef = 0.0f;

    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralHoldProcessor)
};
