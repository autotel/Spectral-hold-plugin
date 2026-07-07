#pragma once
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cmath>

// Stereo plate reverb (Dattorro topology, JAES 1997) applied post-fader, pre-limiter.
// Self-contained, host-free (juce_dsp only) so it is unit-testable without a plugin
// host, same contract as SpectralEngine. See agent-wiki/plan-reverb.md for the design
// rationale and exact constants this implements.
//
// Mono in (the processor sums L+R before calling process), decorrelated stereo out.
// Wet-only: the caller does the dry/wet mix so mix=0 stays bit-exact dry.
class PlateReverb
{
public:
    void prepare (double sampleRate);
    void reset();

    // decay/size/damp/predelaySec in their raw APVTS ranges (see plan-reverb.md §3).
    // metal (0..1, default 0) trades diffusion for discrete, comb-like reflections:
    // less input diffusion, a weaker decay-diffusion allpass, and less LFO excursion
    // (the modulation smear is what keeps the tail from ringing metallically -- turning
    // it down IS the effect). Cheap - call once per block; size and predelay are
    // smoothed per-sample inside.
    void setParams (float decay, float size, float damp, float predelaySec, float metal);

    void process (const float* inMono, float* outL, float* outR, int numSamples);

private:
    // Circular buffer with fractional (linearly interpolated) reads, used for every
    // delay/allpass stage. `readAt` clamps the requested delay into [0, capacity-1],
    // which is also how out-of-range output taps get clamped (per the plan).
    struct DelayLine
    {
        std::vector<float> buf;
        int writePos = 0;

        void setup (int capacity)
        {
            buf.assign ((size_t) juce::jmax (4, capacity), 0.0f);
            writePos = 0;
        }

        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); writePos = 0; }
        int  size() const { return (int) buf.size(); }

        void write (float x)
        {
            buf[(size_t) writePos] = x;
            writePos = (writePos + 1) % (int) buf.size();
        }

        float readAt (float delaySamples) const
        {
            const int cap = (int) buf.size();
            const float d = juce::jlimit (0.0f, (float) (cap - 1), delaySamples);
            float pos = (float) writePos - d;
            while (pos < 0.0f)
                pos += (float) cap;
            const int i0 = (int) pos;
            const int i1 = (i0 + 1) % cap;
            const float frac = pos - (float) i0;
            return buf[(size_t) i0] + frac * (buf[(size_t) i1] - buf[(size_t) i0]);
        }
    };

    static float allpassStep (DelayLine& dl, float delaySamples, float g, float x);
    static float delayStep   (DelayLine& dl, float delaySamples, float x);

    double sr = 44100.0;
    float  srScale = 1.0f; // sr / 29761 (Dattorro's reference rate)

    // control-rate params (set by setParams, some smoothed per-sample in process)
    float decayGain = 0.615f;       // jmap(decay, 0.25, 0.98)
    float decayDiffusion2 = 0.40f;  // clamp(decayGain + 0.15, 0.25, 0.50)
    float damping = 0.3f;           // one-pole coef, 0 = bright .. 1 = dark
    float sizeTarget = 1.0f, sizeSmoothed = 1.0f;
    float predelaySamplesTarget = 0.0f, predelaySmoothed = 0.0f;
    float sizeCoef = 0.0f; // ~50 ms one-pole, computed in prepare()

    // Metal (0..1): scales diffusion/excursion toward a discrete, comb-like character.
    // These replace the fixed input-diffusion gains, decay-diffusion-1 magnitude and LFO
    // excursion namespace constants -- computed once per setParams() call from `metal`.
    float inGain1 = 0.750f, inGain2 = 0.750f, inGain3 = 0.625f, inGain4 = 0.625f;
    float decayDiffusion1 = 0.70f;  // magnitude; sign flipped to -decayDiffusion1 on use
    float excursionBase = 16.0f;    // kExcursion * srScale, set in prepare()
    float excursionSamples = 16.0f; // excursionBase * (1 - metal), set in setParams()

    // input chain (mono)
    DelayLine predelay;
    float bandwidthLp = 0.0f;
    DelayLine inAp1, inAp2, inAp3, inAp4;

    // tank: two cross-coupled halves (figure-8)
    DelayLine modApA, delayA1, apA2, delayA2;
    DelayLine modApB, delayB1, apB2, delayB2;
    float dampLpA = 0.0f, dampLpB = 0.0f;
    float lastOutA = 0.0f, lastOutB = 0.0f;

    // detuned LFOs modulating the two "modulated allpass" read positions
    float lfoPhaseA = 0.0f, lfoPhaseB = 0.0f;
    float lfoIncA = 0.0f, lfoIncB = 0.0f;

    JUCE_LEAK_DETECTOR (PlateReverb)
};
