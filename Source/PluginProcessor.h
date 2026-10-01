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
    bool acceptsMidi() const override  { return true; } // Transpose MIDI (agent-wiki/plan-roadmap.md B3)
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
    // Active editor tab (0 shaper, 1 harmonize, 2 alter, 3 reverb). GUI-only, persisted in state.
    void setUiTab (int t) { uiTab = t; }
    int  getUiTab() const { return uiTab; }

    // Whether the held spectral state is saved inside the session (agent-wiki/plan-roadmap.md
    // B1). GUI-only, persisted in state (like liveMode/uiTab, not an APVTS param). Default on.
    void setKeepSound (bool b) { keepSound = b; }
    bool getKeepSound() const  { return keepSound; }

    // Editor window scale (agent-wiki/plan-roadmap.md B8), GUI-only, persisted in state
    // (same pattern as liveMode/uiTab/keepSound). 1.0 = the base 860x580 size; range matches
    // the editor's setResizeLimits (645..1720 width / 860 = 0.75..2.0).
    void setEditorScale (float s) { editorScale = juce::jlimit (0.75f, 2.0f, s); }
    float getEditorScale() const  { return editorScale; }

    // Snapshot for the spectrum display: each channel's own magnitude/phase, split (not
    // merged -- see agent-wiki/plan-uifix.md U1). Falls back to channel 0 in both L and R
    // on a mono bus. Returns numBins or 0.
    int getDisplaySnapshot (std::vector<float>& magL, std::vector<float>& phaseL,
                            std::vector<float>& magR, std::vector<float>& phaseR,
                            double& sr, int& size);

    // Harmonize influence snapshot (channel 0): peak freqs/weights/drifts. Returns count
    // (0 = harmonize off), or -1 if busy (keep last frame).
    int getHarmonizePeaks (std::vector<float>& freq, std::vector<float>& weight, std::vector<float>& drift)
    {
        return engines[0].copyPeaks (freq, weight, drift);
    }

    // Location strip snapshot (channel 0 only; agent-wiki/plan-roadmap.md B7): per-layer
    // magnitude/location for the E<->W field. Returns numBins or 0.
    int getLocationSnapshot (std::vector<float>& mag, std::vector<float>& loc)
    {
        return engines[0].copyLayers (mag, loc);
    }

    // Injection snapshot (channel 0 only; agent-wiki/plan-uifix.md U2): where input is
    // landing this frame and whether it's dragging/stealing an existing tone. Returns
    // numBins or 0.
    int getInjectionSnapshot (std::vector<float>& loc, std::vector<float>& strength, std::vector<float>& drag)
    {
        return engines[0].copyInjection (loc, strength, drag);
    }

    // GUI brush edit: scale the held spectrum around centreFreqHz (all channels).
    // strength in [-1..+1]: +boost, 0 none, -cut. sigmaOct = brush size (GUI-only).
    void applySpectralBrush (float centreFreqHz, float strength, float sigmaOct)
    {
        for (auto& e : engines)
            e.queueBrush (centreFreqHz, strength, sigmaOct);
    }

    // Undo for destructive edits (agent-wiki/plan-roadmap.md B6): reuses B1's
    // writeHold/queueHoldRestore as an in-memory snapshot ring (depth 4, message thread
    // only). Edit-undo, not time-travel -- harmonize drift, loss decay and normal feeding
    // are never snapshotted, only explicit triggers (brush stroke start, permanent-shaper
    // engage) call snapshotHold().
    void snapshotHold();
    bool undoHold(); // false when there's nothing to undo

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
    std::atomic<float>* pTranspose = nullptr; // agent-wiki/plan-roadmap.md B3
    std::atomic<float>* pTransposeSnap  = nullptr; // agent-wiki/plan-uifix.md U3
    std::atomic<float>* pMidiIgnore     = nullptr; // Alter tab: gate MIDI note transposition
    std::atomic<float>* pTransposeGlide = nullptr; // agent-wiki/plan-uifix.md U3
    std::atomic<float>* pSpread = nullptr; // agent-wiki/plan-roadmap.md B5
    int midiNote = -1; // last held note-on (monophonic, last-note priority); audio thread only
    std::atomic<float>* pLimThreshold = nullptr;
    std::atomic<float>* pLimRelease   = nullptr;
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
    std::atomic<float>* pRevMetal    = nullptr;
    std::atomic<float>* pRevFeed     = nullptr; // reverb -> hold feedback (plan-roadmap.md Part A)
    PlateReverb reverb;
    std::vector<float> revMono, revWetL, revWetR;
    std::vector<float> revFeedBuf; // previous block's wet mono, fed back into the engines
    float prevRevMix  = 0.0f; // to detect the 1->0 transition and reset the tail
    float prevRevFeed = 0.0f; // to detect the 1->0 transition on the feedback path

    // spectral shaper (replaces the old filter + compress)
    std::atomic<float>* pShapeAmt   = nullptr;
    std::atomic<float>* pShapeMode  = nullptr;
    std::atomic<float>* pShape      = nullptr;
    std::atomic<float>* pShapeFreq  = nullptr;
    std::atomic<float>* pShapeWidth = nullptr;
    std::atomic<float>* pShapeCount = nullptr;
    std::atomic<float>* pShapeLevel = nullptr;

    // Undo ring (agent-wiki/plan-roadmap.md B6): in-memory writeHold() blobs, one pair
    // (engine 0, engine 1) per snapshot, message thread only -- never touched by the audio
    // thread directly (snapshotHold/undoHold both take getCallbackLock() around the parts
    // that read/apply live engine state, same as B1's session save/restore).
    std::array<std::pair<juce::MemoryBlock, juce::MemoryBlock>, 4> undoRing;
    int undoWriteIdx = 0;
    int undoCount = 0;

    // GUI-only switches (persisted manually, see get/setStateInformation)
    bool liveMode = false;
    int  uiTab = 0;
    bool keepSound = true; // agent-wiki/plan-roadmap.md B1: save the held state in the session
    float editorScale = 1.0f; // agent-wiki/plan-roadmap.md B8: last editor window scale

    // slow linked limiter state
    float limEnv  = 0.0f;
    float limGain = 1.0f;
    float limAttCoef = 0.0f, limRelCoef = 0.0f;

    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralHoldProcessor)
};
