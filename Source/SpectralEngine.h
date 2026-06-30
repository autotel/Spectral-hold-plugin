#pragma once
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <complex>

// One independent spectral-hold STFT engine. One instance per audio channel.
//
// Model (see agent-wiki/dsp-design.md): a phase-vocoder where every bin keeps a
// complex phasor S[k] that free-runs at the bin centre frequency. Input is
// re-injected each hop; the held content decays. The "filter" is a per-bin
// gaussian bell that makes out-of-bell bins decay faster (frequency-dependent
// loss); freshly fed input is compensated so it always enters at unity gain
// regardless of the filter.
class SpectralEngine
{
public:
    struct Params
    {
        float feed       = 0.5f;    // 0..1  how much input is mixed into the running FT
        float loss       = 0.2f;    // 0..1  how fast held magnitudes decay
        float filterAmt  = 0.0f;    // 0..1  depth of the gaussian shaping of the FT
        float filterTone = 1000.0f; // Hz, centre of the bell (log-mapped)
        float attack     = 0.0f;    // 0..1  0 = instant onset, 1 = slow onset
        float compress   = 0.0f;    // -1..+1  <0 homogenise levels, >0 expand (purify)
        bool  phaseNoise = false;   // feed random jitter into the frequency tracking
    };

    void prepare (double sampleRate, int maxFftOrder);
    // Reconfigure to a new FFT size (order = log2(size)). Must be <= maxFftOrder.
    // Cheap: recomputes tables only (all buffers preallocated at prepare()).
    void setOrder (int fftOrder);
    void reset();

    // Process one channel in place. Latency == fftSize (report it to the host).
    void process (const float* in, float* out, int numSamples, const Params& p);

    int    getFftSize() const    { return fftSize; }
    int    getLatency() const    { return fftSize; }
    double getSampleRate() const { return sampleRate; }

    // Display snapshot: copies current per-bin magnitude (normalised) and phase.
    // Returns numBins, or 0 if the engine is mid-reconfigure (non-blocking).
    int copyDisplay (std::vector<float>& mag, std::vector<float>& phase);

    // Gaussian bell shaping gain in [1-amt .. 1] at a given frequency. Shared with
    // the GUI so the displayed filter curve matches the DSP exactly. fft-size independent.
    static constexpr float kSigmaOct = 1.25f; // bell half-width, octaves
    static float filterGain (float freqHz, float toneHz, float amt);

    // Queue a brush edit (message thread): permanently scales the held spectrum around
    // centreFreqHz with a gaussian falloff in log-frequency of half-width sigmaOct octaves.
    // strength in [-1..+1]: +1 strongly boosts, 0 = no change, -1 strongly cuts. Applied on
    // the audio thread. sigmaOct is GUI-only (the brush size selector), passed per event.
    void queueBrush (float centreFreqHz, float strength, float sigmaOct);
    static constexpr float kBrushSigmaOct = 0.6f;  // default brush half-width in octaves
    static constexpr float kBrushMaxFactor = 8.0f; // gain factor at full strength/centre
    static constexpr float kBrushRate = 0.05f;     // per-event exponent scale (brush feel)

private:
    void applyPendingOrder();
    void configure (int fftOrder);
    void processFrame (const Params& p);
    void drainBrush();

    double sampleRate = 44100.0;
    int maxFftSize = 0, maxOrder = 0;

    int fftSize = 0, order = 0, hopSize = 0, numBins = 0;
    std::atomic<int> pendingOrder { -1 }; // requested from message thread

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window;
    float winNorm = 1.0f;

    // sliding I/O rings, length fftSize
    std::vector<float> inRing, outRing;
    int inWrite = 0, outRead = 0, hopCount = 0;

    // spectral state
    std::vector<std::complex<float>> S;   // held phasors
    std::vector<std::complex<float>> Xs;  // attack-smoothed input spectrum
    // Instantaneous-frequency phase tracking (smooth freeze, not bin-centre):
    std::vector<float> expectedAdv;       // 2*pi*k*hop/N, the bin-centre advance per hop
    std::vector<float> omega;             // measured per-hop phase advance per bin (rad)
    std::vector<float> prevPhase;         // last input phase per bin, for unwrapping
    juce::Random rng;                     // phase-noise source (audio thread only)

    // scratch (length 2*maxFftSize for juce real-only transform)
    std::vector<float> fftData;
    std::vector<std::complex<float>> dispScratch; // per-frame output spectrum for the display

    // display snapshot (guarded)
    juce::CriticalSection displayLock;
    std::vector<float> dispMag, dispPhase;

    // brush edit queue (message thread -> audio thread)
    struct BrushOp { float centreFreq; float strength; float sigmaOct; };
    juce::CriticalSection brushLock;
    std::vector<BrushOp> brushPending, brushScratch;

    JUCE_LEAK_DETECTOR (SpectralEngine)
};
