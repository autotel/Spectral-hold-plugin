# Architecture

## Files (`Source/`)
- `PluginProcessor.{h,cpp}` — `AudioProcessor`. Owns the APVTS, the two `SpectralEngine`s
  (one per channel), the GUI-only FFT-size value, the output `PlateReverb`, and the linked
  limiter. `processBlock` reads params → runs each channel engine in place → output gain →
  reverb (mono-summed, wet/dry mixed back in) → limiter.
- `SpectralEngine.{h,cpp}` — the DSP core. Self-contained STFT spectral-hold engine.
  Depends only on `juce_dsp`, so it builds and is tested without a plugin host.
- `PlateReverb.{h,cpp}` — the output reverb (Dattorro plate topology). Same host-free
  contract as `SpectralEngine` (`juce_dsp` only, unit-testable). Cross-channel like the
  limiter — lives in the processor, not per-engine. See
  [plan-reverb.md](plan-reverb.md) for the full design.
- `DryDelay.h` — header-only latency-aligned dry path for the global Dry/Wet mix.
- `PluginEditor.{h,cpp}` — `AudioProcessorEditor`. Persistent performance row + a
  **Shaper | Harmonize | Reverb** tab strip switching one shared knob row, the FFT-size
  `ComboBox`, the spectrum display, and the hover **info bar**.
- `SpectrumDisplay.{h,cpp}` — the custom "lighting" spectrum view (its own 30 Hz timer).
- `test_main.cpp` — offline smoke-test (`SpectralHoldTest` target).

## Signal flow
```
        ┌--(dry, delayed fftSize)--------──┐
input ──> [engine L] ──┐                   v
input ──> [engine R] ──┤─> dry/wet mix ─> output gain ─> plate reverb ─> linked limiter ─> out
        ^              (per-channel)                     (mono-sum in,   (cross-channel)
        └──(revFeed: last block's wet, clamped ±1)───────┘ stereo out)
```
Each engine: sliding ring buffers → per-hop STFT frame → held-phasor update → IFFT →
overlap-add. See [dsp-design.md](dsp-design.md). The reverb is a hard bypass at
`revMix = 0` and `revFeed` only runs while the reverb does (see [gotchas.md](gotchas.md)).
The dry path (`DryDelay.h`) is a plain ring delayed by the engine's `fftSize` so
dry and wet stay time-aligned; captured *before* the revFeed injection, so it is
genuinely untouched input.

## Threading model
- **Audio thread:** `processBlock` → engines → limiter. No allocations (buffers
  preallocated at `prepare`). FFT-size change is applied here at a frame boundary.
- **Message thread:** the editor. Knob moves go through APVTS (thread-safe atomics).
  The FFT-size combo calls `setFftOrder()` → engines store a `pendingOrder` (atomic).
- **Display handoff:** the engine writes a magnitude/phase snapshot under a
  `CriticalSection` try-lock at the end of each frame; the editor's timer copies it out
  under the same lock. Non-blocking on the audio side — it skips the snapshot if the
  editor holds the lock, never the reverse.

## Parameters & state
- DAW-facing params live in `apvts` (`feed`, `loss`, `filterAmt`, `filterTone`, `attack`).
- FFT order is **not** in the APVTS (GUI-only by spec). It is saved/restored manually in
  `get/setStateInformation` as a `fftOrder` property on the state tree.

## Identity
Plugin code `Sphd`, manufacturer `Jqna`, company `autotel`. Formats: VST3 + Standalone.
