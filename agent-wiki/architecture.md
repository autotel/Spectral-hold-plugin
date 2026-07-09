# Architecture

## Files (`Source/`)
- `PluginProcessor.{h,cpp}` — `AudioProcessor`. Owns the APVTS, the two `SpectralEngine`s
  (one per channel), the GUI-only FFT-size value, the output `PlateReverb`, and the linked
  limiter. `processBlock` reads params → runs each channel engine in place → output gain →
  reverb (mono-summed, wet/dry mixed back in) → limiter.
- `SpectralEngine.{h,cpp}` — the DSP core. Self-contained STFT spectral-hold engine.
  Depends only on `juce_dsp`, so it builds and is tested without a plugin host. Every held
  bin/tone carries a continuous East–West location (`binLoc[k]`); the `ewLocation` param
  is a listener/recorder position whose distance to each tone sets an absolute (never
  normalised) playback and edit-strength attenuation (see dsp-design.md — this replaced an
  earlier 16-slot design; don't resurrect slots).
- `PlateReverb.{h,cpp}` — the output reverb (Dattorro plate topology). Same host-free
  contract as `SpectralEngine` (`juce_dsp` only, unit-testable). Cross-channel like the
  limiter — lives in the processor, not per-engine. See
  [plan-reverb.md](plan-reverb.md) for the full design.
- `DryDelay.h` — header-only latency-aligned dry path for the global Dry/Wet mix.
- `PluginEditor.{h,cpp}` — `AudioProcessorEditor`. Persistent performance row + a
  **Shaper | Harmonize | Reverb** tab strip switching one shared knob row, the FFT-size
  `ComboBox`, the spectrum display, and the hover **info bar**.
- `SpectrumDisplay.{h,cpp}` — the custom "lighting" spectrum view (its own 30 Hz timer).
- `SpectralLookAndFeel.{h,cpp}` — the dark rotary knob style. Animates a per-slider hover
  glow (own lazily-started/stopped 30 Hz timer); see gotchas.md for the teardown-order
  hazard this animation has to guard against.
- `test_main.cpp` — offline smoke-test (`SpectralHoldTest` target).

## Signal flow
```
        ┌--(dry, delayed fftSize)--------──┐
input ──> [engine L] ──┐                   v
input ──> [engine R] ──┤─> dry/wet mix ─> output gain ─> plate reverb ─> linked limiter ─> out
                       (per-channel)                     (mono-sum in, stereo out)
```
Each engine: sliding ring buffers → per-hop STFT frame → held-phasor update → IFFT →
overlap-add. See [dsp-design.md](dsp-design.md). The reverb is a hard bypass at
`revMix = 0` (see [gotchas.md](gotchas.md)). The dry path (`DryDelay.h`) is a plain ring
delayed by the engine's `fftSize` so dry and wet stay time-aligned.

## Threading model
- **Audio thread:** `processBlock` → engines → limiter. No allocations (buffers
  preallocated at `prepare`). FFT-size change is applied here at a frame boundary.
- **Message thread:** the editor. Knob moves go through APVTS (thread-safe atomics).
  The FFT-size combo calls `setFftOrder()` → engines store a `pendingOrder` (atomic).
- **Display handoff:** the engine writes several snapshots under the same `CriticalSection`
  try-lock, at the end of each frame, all copied out by the editor's 30 Hz timer under that
  same lock (non-blocking on the audio side — it skips the write if the editor holds the
  lock, never the reverse): `dispMag`/`dispPhase` (main spectrum, `copyDisplay`),
  `dispPeakF/A/D` (harmonize influence, `copyPeaks`), `dispLayerMag`/`dispLayerLoc` (B7,
  per-layer magnitude + E<->W location, `copyLayers`, feeds the location strip's dots), and
  `dispInjLoc/Strength/Drag` (agent-wiki/plan-uifix.md U2, per-bin live-recording state,
  `copyInjection`, feeds the strip's ticks). `getDisplaySnapshot` is per-engine (one call per
  channel) and returns each channel's magnitude/phase **split**, not merged — see
  [gui-display.md](gui-display.md)'s "Split stereo" section (U1 replaced an earlier
  `max(mag0,mag1)` merge that caused a visual artifact).

## Parameters & state
- DAW-facing params live in `apvts` (see [parameters.md](parameters.md) for the full list).
- FFT order is **not** in the APVTS (GUI-only by spec). It is saved/restored manually in
  `get/setStateInformation` as a `fftOrder` property on the state tree, alongside `liveMode`,
  `uiTab`, and `keepSound` (all GUI-only, same mechanism).
- **Held state (agent-wiki/plan-roadmap.md B1):** when `keepSound` is on, `getStateInformation`
  writes each engine's `S`/`omega`/`binLoc` (`SpectralEngine::writeHold`) into a
  gzip-compressed, base64-encoded `holdN` property (N = channel index) on the state tree.
  `setStateInformation` decodes and queues it back in via `queueHoldRestore` — applied on the
  audio thread at the next frame boundary, **after** `setFftOrder()` has landed (the restore
  is gated on the blob's FFT order matching the engine's current order).

## Identity
Plugin code `Sphd`, manufacturer `Jqna`, company `autotel`. Formats: VST3 + Standalone.
