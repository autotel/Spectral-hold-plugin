#pragma once
#include <vector>
#include <algorithm>

// Latency-aligned dry path for the global Dry/Wet mix: a plain circular delay that
// holds the untouched input so it can be mixed against the engine output (which lags
// by fftSize samples). Host-free and header-only so it's unit-testable like the other
// DSP pieces. The delay amount is passed per read (it follows the engine's current
// fftSize; changing it jumps the read point, which is fine - the held state resets on
// FFT-size changes anyway).
class DryDelay
{
public:
    // capacity must exceed maxDelay + the largest block the host will send.
    void prepare (int maxDelay, int maxBlock)
    {
        buf.assign ((size_t) std::max (4, maxDelay + std::max (maxBlock, 4096)), 0.0f);
        writePos = 0;
    }

    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); }

    // Write n input samples and fetch them delayed by delaySamples into out.
    // in/out may NOT alias (the processor uses a scratch buffer for out).
    void process (const float* in, float* out, int n, int delaySamples)
    {
        const int cap = (int) buf.size();
        const int d   = std::clamp (delaySamples, 0, cap - 1);
        for (int i = 0; i < n; ++i)
        {
            buf[(size_t) writePos] = in[i];
            out[i] = buf[(size_t) ((writePos - d + cap) % cap)];
            writePos = (writePos + 1) % cap;
        }
    }

private:
    std::vector<float> buf;
    int writePos = 0;
};
