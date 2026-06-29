# Gotchas & decisions already made

Read this before "fixing" something that looks wrong — it probably isn't.

## DSP
- **Filter gain coupling is intentional.** Fed input is compensated by `1/gFilt`, so live
  input is unaffected by the filter; only the *held tail* is shaped. This is the spec's
  "in/out relationship is always the same" invariant. Don't normalise it away.
- **No separate output filter.** The filter is a frequency-dependent *decay* inside the
  held-phasor update, not a post EQ. Looking for an output multiply by the bell? There
  isn't one by design.
- **Phasors free-run (`rot[k]`).** This is what makes it a spectral *hold* (continuous
  sinusoids) instead of a grain looper. Removing the rotation reintroduces grain looping —
  exactly what the spec forbids.
- **`comp` and `decay` floors** (`kCompFloor`, `kDecayFloor`) exist to stop blow-up where
  the bell → 0. Don't drop them.
- **IFFT normalisation:** `performRealOnlyInverseTransform` already divides by N. The only
  extra scaling is `winNorm = 1/1.5` (Hann² overlap-add COLA). Adding another 1/N makes
  everything ~N times too quiet.
- **Loss = 0 means eternal hold** (`decay = 1.0`). With `feed > 0` and a steady input,
  magnitudes can grow — that's real feedback, bounded by the limiter. Not a bug.

## Limiter
- Lives in `PluginProcessor`, **linked across channels** (one gain for both) to keep the
  image. Per-engine limiting would smear stereo.
- Gain is exactly 1.0 below clipping by construction (`target=1` while `limEnv<=1`). The
  release is deliberately slow (~1.2 s) per spec — don't speed it up to "tighten" it.

## FFT size
- It is **GUI-only**: a `ComboBox`, not an APVTS parameter (spec: "perhaps not presented to
  the DAW"). It is persisted manually in `get/setStateInformation` as `fftOrder`. If you
  ever expose it to the DAW, move it into the APVTS *and* drop the manual state code.
- Changing it **resets the held spectrum and changes latency**. Both expected.
- Engines preallocate at `kMaxFftOrder`; the size change recomputes tables only, applied on
  the audio thread at a frame boundary under `displayLock`. No audio-thread allocation.

## Latency
- Equals `fftSize` and is reported via `setLatencySamples`. The host compensates; the raw
  Standalone will show that much delay. Expected for an STFT effect.

## Display
- `copyDisplay` is non-blocking (try-lock). If the GUI ever looks frozen while audio is
  fine, it's not a deadlock — it just means snapshots are being skipped; check the timer.
- Shows channel 0 only.

## In-place processing aliasing (was a real silent-output bug)
- Hosts call `processBlock` with the **same buffer for input and output**. The engine loop
  must **latch `in[n]` into a local before writing `out[n]`** — otherwise the just-written
  output (zeros during the initial latency) overwrites the input and the engine never sees
  audio → permanent silence + empty display, even though the offline test (separate in/out
  buffers) passes. Regression-guarded by the "in-place (in==out)" case in `test_main.cpp`.
  When adding DSP tests, always include an `in == out` path.

## Standalone: no audio / empty display
- The JUCE **Standalone mutes audio input by default** — not a plugin bug. In
  `juce_StandaloneFilterWindow.h`: `processorHasPotentialFeedbackLoop = inputs>0 && outputs>0`
  (true for this in==out effect) and `shouldMuteInput` defaults to `true` (L344/L453).
- Symptom: silent output **and** an empty spectrum display, "as if input isn't connected".
- Fix: Standalone gear/Options → Audio/MIDI Settings → uncheck **"Mute audio input"** and
  select a real input device. The setting is persisted per user.
- In a DAW (VST3) there is no such mute; just route audio into the track.

## Build
- LTO on → slow links. Not a hang.
- `SpectralHoldTest` links only `juce_dsp` — keep `SpectralEngine` free of
  `juce_audio_processors`/GUI deps or the test stops building.
