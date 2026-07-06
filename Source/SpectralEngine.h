#pragma once
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <complex>

// One independent spectral-hold STFT engine. One instance per audio channel.
//
// Model (see agent-wiki/dsp-design.md): a phase-vocoder where every bin keeps a
// complex phasor S[k] that free-runs at the bin centre frequency. Input is
// re-injected each hop; the held content decays. The "shaper" (ShapeCurves.h)
// applies a per-bin signed amplitude curve either as an output-only gain
// (momentary) or as a multiply on the held state itself (permanent).
class SpectralEngine
{
public:
    struct Params
    {
        float feed       = 0.5f;    // 0..1  how much input is mixed into the running FT
        float loss       = 0.2f;    // 0..1  how fast held magnitudes decay
        bool  phaseNoise = false;   // feed random jitter into the frequency tracking

        // Spectral shaper: per-bin amplitude change from a math curve (replaces the
        // old Filter + Compress). See ShapeCurves.h / agent-wiki/dsp-design.md.
        float shapeAmt   = 1.0f;    // 0..1  global depth
        float shapeMode  = 0.0f;    // 0..1  momentary (0, output-only) <-> permanent (1, fed into S)
        float shape      = 0.0f;    // 0..4  cross-fades Level/Sigmoid/Spikes/Harmonics/Sine
        float shapeFreq  = 1000.0f; // Hz, curve centre (log-mapped)
        float shapeWidth = 0.5f;    // 0..1  width/steepness/spacing, meaning per shape
        float shapeCount = 1.0f;    // 0..1  extent/repetition, meaning per shape
        float shapeLevel = 0.0f;    // -1..+1  signed strength; 0 = no effect for every shape

        // Harmonize (coupled-oscillator tone interaction, see agent-wiki/harmonize.md)
        float harmonize  = 0.0f;    // 0..1  entrainment: tones drift to amplitude-weighted mean
        float harmWidth  = 0.5f;    // octaves, sigma of the nearness-influence curve
        float harmonic   = 0.0f;    // 0..1  attraction toward low-denominator harmonic ratios
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

    // Harmonize influence snapshot: per detected peak, its frequency (Hz), weight (|S|) and
    // current pitch drift (Hz, signed). Returns the peak count (0 when harmonize is off).
    int copyPeaks (std::vector<float>& freq, std::vector<float>& weight, std::vector<float>& drift);

    // Queue a brush edit (message thread): permanently scales the held spectrum around
    // centreFreqHz with a gaussian falloff in log-frequency of half-width sigmaOct octaves.
    // strength in [-1..+1]: +1 strongly boosts, 0 = no change, -1 strongly cuts. Applied on
    // the audio thread. sigmaOct is GUI-only (the brush size selector), passed per event.
    void queueBrush (float centreFreqHz, float strength, float sigmaOct);
    static constexpr float kBrushSigmaOct = 0.6f;  // default brush half-width in octaves
    static constexpr float kBrushMaxFactor = 8.0f; // gain factor at full strength/centre
    static constexpr float kBrushRate = 0.10f;     // per-tick exponent scale (brush speed)

private:
    void applyPendingOrder();
    void configure (int fftOrder);
    void processFrame (const Params& p);
    void drainBrush();
    void applyHarmonize (const Params& p);

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
    std::vector<std::complex<float>> Xs;  // per-hop input spectrum (fed into S)
    // Instantaneous-frequency phase tracking (smooth freeze, not bin-centre):
    std::vector<float> expectedAdv;       // 2*pi*k*hop/N, the bin-centre advance per hop
    std::vector<float> omega;             // measured per-hop phase advance per bin (rad)
    std::vector<float> prevPhase;         // last input phase per bin, for unwrapping
    juce::Random rng;                     // phase-noise source (audio thread only)

    // harmonize peak scratch (preallocated; capped at kMaxPeaks)
    static constexpr int kMaxPeaks = 128;
    std::vector<int>   peakBin;
    std::vector<float> peakFreq, peakAmp, peakDelta;
    // peak snapshot for the GUI influence overlay (guarded by displayLock)
    std::vector<float> dispPeakF, dispPeakA, dispPeakD;
    int dispPeakN = 0;

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
