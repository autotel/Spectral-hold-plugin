#include "PlateReverb.h"

namespace
{
    constexpr double kRefRate = 29761.0; // Dattorro's reference sample rate

    // Input diffuser chain (fixed lengths/gains, scaled by sr only - not by size).
    constexpr float kInLen1 = 142.0f, kInLen2 = 107.0f, kInLen3 = 379.0f, kInLen4 = 277.0f;
    constexpr float kInGain1 = 0.750f, kInGain2 = 0.750f, kInGain3 = 0.625f, kInGain4 = 0.625f;
    constexpr float kBandwidth = 0.9995f; // one-pole coef on the input (near-transparent)

    // Tank (lengths scaled by sr AND size; excursion scaled by sr only).
    constexpr float kModLenA = 672.0f, kModLenB = 908.0f;
    constexpr float kExcursion = 16.0f;
    constexpr float kDelayLenA1 = 4453.0f, kApLenA2 = 1800.0f, kDelayLenA2 = 3720.0f;
    constexpr float kDelayLenB1 = 4217.0f, kApLenB2 = 2656.0f, kDelayLenB2 = 3163.0f;
    constexpr float kDecayDiffusion1 = 0.70f; // sign flipped to -0.70 on the modulated allpass

    constexpr float kLfoHzA = 0.50f, kLfoHzB = 0.61f;

    // Output tap offsets (Dattorro Table 2), scaled by sr and size at read time.
    constexpr float kTapL_B1a = 266.0f, kTapL_B1b = 2974.0f, kTapL_B2 = 1913.0f,
                     kTapL_D2 = 1996.0f, kTapL_A1 = 1990.0f, kTapL_A2 = 187.0f, kTapL_dA2 = 1066.0f;
    constexpr float kTapR_A1a = 353.0f, kTapR_A1b = 3627.0f, kTapR_A2 = 1228.0f,
                     kTapR_dA2 = 2673.0f, kTapR_B1 = 2111.0f, kTapR_B2 = 335.0f, kTapR_dB2 = 121.0f;
    constexpr float kTapGain = 0.6f;
}

float PlateReverb::allpassStep (DelayLine& dl, float delaySamples, float g, float x)
{
    const float bufOut = dl.readAt (delaySamples);
    const float v = x - g * bufOut;
    dl.write (v);
    return bufOut + g * v;
}

float PlateReverb::delayStep (DelayLine& dl, float delaySamples, float x)
{
    const float y = dl.readAt (delaySamples);
    dl.write (x);
    return y;
}

void PlateReverb::prepare (double sampleRate)
{
    sr = sampleRate;
    srScale = (float) (sr / kRefRate);

    // Tank buffers must hold the largest delay ever requested: base length at the
    // maximum size (2.0) plus modulation excursion, scaled to this sample rate.
    auto tankCap = [this] (float baseLen, float excursion = 0.0f)
    {
        return (int) std::ceil (baseLen * srScale * 2.0f + excursion * srScale) + 16;
    };
    auto fixedCap = [this] (float baseLen)
    {
        return (int) std::ceil (baseLen * srScale) + 8;
    };

    predelay.setup ((int) std::ceil (0.25 * sr) + 8); // max predelay 250 ms
    inAp1.setup (fixedCap (kInLen1));
    inAp2.setup (fixedCap (kInLen2));
    inAp3.setup (fixedCap (kInLen3));
    inAp4.setup (fixedCap (kInLen4));

    modApA.setup (tankCap (kModLenA, kExcursion));
    delayA1.setup (tankCap (kDelayLenA1));
    apA2.setup (tankCap (kApLenA2));
    delayA2.setup (tankCap (kDelayLenA2));

    modApB.setup (tankCap (kModLenB, kExcursion));
    delayB1.setup (tankCap (kDelayLenB1));
    apB2.setup (tankCap (kApLenB2));
    delayB2.setup (tankCap (kDelayLenB2));

    excursionBase = kExcursion * srScale;
    excursionSamples = excursionBase;
    lfoIncA = juce::MathConstants<float>::twoPi * kLfoHzA / (float) sr;
    lfoIncB = juce::MathConstants<float>::twoPi * kLfoHzB / (float) sr;

    sizeCoef = 1.0f - std::exp (-1.0f / (0.05f * (float) sr)); // ~50 ms

    reset();
}

void PlateReverb::reset()
{
    predelay.clear();
    inAp1.clear(); inAp2.clear(); inAp3.clear(); inAp4.clear();
    modApA.clear(); delayA1.clear(); apA2.clear(); delayA2.clear();
    modApB.clear(); delayB1.clear(); apB2.clear(); delayB2.clear();

    bandwidthLp = 0.0f;
    dampLpA = 0.0f; dampLpB = 0.0f;
    lastOutA = 0.0f; lastOutB = 0.0f;
    lfoPhaseA = 0.0f; lfoPhaseB = 0.0f;

    sizeSmoothed = sizeTarget;
    predelaySmoothed = predelaySamplesTarget;
}

void PlateReverb::setParams (float decay, float size, float damp, float predelaySec, float metal)
{
    decayGain = juce::jmap (juce::jlimit (0.0f, 1.0f, decay), 0.25f, 0.98f);
    decayDiffusion2 = juce::jlimit (0.25f, 0.50f, decayGain + 0.15f);
    damping = juce::jlimit (0.0f, 1.0f, damp);
    sizeTarget = juce::jlimit (0.5f, 2.0f, size);
    predelaySamplesTarget = juce::jlimit (0.0f, 0.25f, predelaySec) * (float) sr;

    const float m = juce::jlimit (0.0f, 1.0f, metal);
    inGain1 = kInGain1 * (1.0f - 0.9f * m);
    inGain2 = kInGain2 * (1.0f - 0.9f * m);
    inGain3 = kInGain3 * (1.0f - 0.9f * m);
    inGain4 = kInGain4 * (1.0f - 0.9f * m);
    decayDiffusion1 = juce::jmap (m, kDecayDiffusion1, 0.20f); // 0.70 -> 0.20
    excursionSamples = excursionBase * (1.0f - m);             // kills the LFO smear
}

void PlateReverb::process (const float* inMono, float* outL, float* outR, int numSamples)
{
    for (int n = 0; n < numSamples; ++n)
    {
        sizeSmoothed += (sizeTarget - sizeSmoothed) * sizeCoef;
        predelaySmoothed += (predelaySamplesTarget - predelaySmoothed) * sizeCoef;

        // input chain
        float u = delayStep (predelay, predelaySmoothed, inMono[n]);
        bandwidthLp += kBandwidth * (u - bandwidthLp);
        u = bandwidthLp;
        u = allpassStep (inAp1, kInLen1 * srScale, inGain1, u);
        u = allpassStep (inAp2, kInLen2 * srScale, inGain2, u);
        u = allpassStep (inAp3, kInLen3 * srScale, inGain3, u);
        u = allpassStep (inAp4, kInLen4 * srScale, inGain4, u);

        // LFOs (detuned - what keeps the tail from ringing metallically)
        lfoPhaseA += lfoIncA;
        if (lfoPhaseA >= juce::MathConstants<float>::twoPi)
            lfoPhaseA -= juce::MathConstants<float>::twoPi;
        lfoPhaseB += lfoIncB;
        if (lfoPhaseB >= juce::MathConstants<float>::twoPi)
            lfoPhaseB -= juce::MathConstants<float>::twoPi;

        const float modA = excursionSamples * std::sin (lfoPhaseA);
        const float modB = excursionSamples * std::sin (lfoPhaseB);

        const float sizeScale = srScale * sizeSmoothed;

        // tank: figure-8, two cross-coupled halves
        const float inA = u + decayGain * lastOutB;
        float a = allpassStep (modApA, kModLenA * sizeScale + modA, -decayDiffusion1, inA);
        a = delayStep (delayA1, kDelayLenA1 * sizeScale, a);
        dampLpA += (1.0f - damping) * (a - dampLpA);
        a = dampLpA * decayGain;
        a = allpassStep (apA2, kApLenA2 * sizeScale, decayDiffusion2, a);
        a = delayStep (delayA2, kDelayLenA2 * sizeScale, a);
        const float outA = a * decayGain;

        const float inB = u + decayGain * outA;
        float b = allpassStep (modApB, kModLenB * sizeScale + modB, -decayDiffusion1, inB);
        b = delayStep (delayB1, kDelayLenB1 * sizeScale, b);
        dampLpB += (1.0f - damping) * (b - dampLpB);
        b = dampLpB * decayGain;
        b = allpassStep (apB2, kApLenB2 * sizeScale, decayDiffusion2, b);
        b = delayStep (delayB2, kDelayLenB2 * sizeScale, b);
        const float outB = b * decayGain;

        lastOutA = outA;
        lastOutB = outB;

        // output taps (fixed offsets into the tank buffers, per the Dattorro paper)
        outL[n] = kTapGain * delayB1.readAt (kTapL_B1a * sizeScale)
                + kTapGain * delayB1.readAt (kTapL_B1b * sizeScale)
                - kTapGain * apB2.readAt   (kTapL_B2  * sizeScale)
                + kTapGain * delayB2.readAt (kTapL_D2  * sizeScale)
                - kTapGain * delayA1.readAt (kTapL_A1  * sizeScale)
                - kTapGain * apA2.readAt   (kTapL_A2  * sizeScale)
                - kTapGain * delayA2.readAt (kTapL_dA2 * sizeScale);

        outR[n] = kTapGain * delayA1.readAt (kTapR_A1a * sizeScale)
                + kTapGain * delayA1.readAt (kTapR_A1b * sizeScale)
                - kTapGain * apA2.readAt   (kTapR_A2  * sizeScale)
                + kTapGain * delayA2.readAt (kTapR_dA2 * sizeScale)
                - kTapGain * delayB1.readAt (kTapR_B1  * sizeScale)
                - kTapGain * apB2.readAt   (kTapR_B2  * sizeScale)
                - kTapGain * delayB2.readAt (kTapR_dB2 * sizeScale);
    }
}
