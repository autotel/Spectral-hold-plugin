# Gotchas & decisions already made

Read this before "fixing" something that looks wrong — it probably isn't.

## DSP
- **Filter and Compress were removed** (see [plan-spectral-shaper.md](plan-spectral-shaper.md)),
  replaced by the unified **shaper** (`ShapeCurves.h`, `shape`/`shapeMode`/etc. params). If
  you're hunting for `filterAmt`, `filterTone`, `compress`, or `SpectralEngine::filterGain`,
  they're gone on purpose — see [dsp-design.md](dsp-design.md#the-spectral-shaper).
- **Momentary-cut compensation is intentional.** Fed input is compensated by
  `1/max(kCompFloor, min(1,gOut))` for momentary *cuts* only (not boosts), so live input is
  unaffected by a momentary cut; only the *held tail* is shaped. This is the old filter's
  "in/out relationship is always the same" invariant, kept for cuts. Don't normalise it away.
- **Momentary shaping is a pure output multiply, not a decay.** `S` decays by `lossDecay`
  only; the momentary gain (`gOut`) is applied when writing to `fftData`/the display
  snapshot, never fed back into `S`. Turning momentary Amount down restores the held state
  exactly — that's the point.
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

## Brush edits seem to "do nothing"
- The brush scales the held `S`. Three reasons it can look inert, none a bug:
  1. **Feed washes it out.** With input flowing and Feed > 0, the loop re-injects input levels
     every hop and `S` returns to steady state in ~1/(1-decay) frames. Brush edits are most
     visible on *held* content (input stopped / low Feed). `kBrushRate` was raised to 0.05 so a
     short drag is clearly audible.
  2. **STFT latency** (`fftSize` samples, ~85 ms at 4096/48k): edits reach the output one
     latency later, not instantly.
  3. The brush **scales existing energy** — boosting a bin with ~0 energy stays ~0. It shapes
     present tones; it doesn't synthesise new ones.
- Vertical position is the strength: near the centre line strength ≈ 0, so dragging through the
  middle intentionally does nothing.

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
  `juce_audio_processors`/GUI deps or the test stops building. `PlateReverb` follows the
  same rule (`juce_dsp` only) so it's unit-testable without a host.

## Output reverb (`PlateReverb`, branch `exp/reverb`)
- **`revMix = 0` is a hard bypass**, not just "wet gain zero" — the processor skips calling
  `reverb.process` entirely so old sessions/presets stay bit-exact dry. It also calls
  `reverb.reset()` on the 1→0 transition so a stale tail can't reappear next time mix goes up.
- **Size knob pitch-bends by design.** `revSize` scales the tank delay lengths via a
  smoothed fractional read; moving it while a tail is ringing produces a gentle tape-style
  pitch shift. That's expected, not a fractional-delay bug — see plan-reverb.md §3.
- **Mono in, stereo out.** The processor sums L+R (post output-gain, pre-limiter) before
  calling `PlateReverb::process`; the tank's own topology produces the decorrelated stereo
  pair. It never reads the two channels independently.
- **Two detuned LFOs (0.50/0.61 Hz) modulate the tank's two "modulated allpass" read
  positions.** This is *the* thing standing between a smooth diffuse tail and a metallic
  "boingy pipe" — if the reverb starts sounding metallic, check these weren't dropped or
  set to the same rate before touching anything else.
- Reverb lives in `PluginProcessor` (cross-channel, mono-summed), not per-engine — same
  category as the limiter.
- **`revFeed`'s ±1 clamp on the injected wet is load-bearing.** The feedback tap is
  *pre-limiter*, so nothing else bounds the loop; without the clamp it grows
  exponentially at high decay / low loss (the worst-case test measured ~1e32). Clamped,
  the worst case degenerates to the documented eternal-hold linear growth, and the
  limiter caps the output. Don't remove it, and don't "fix" it with a hidden compressor.

## East–West location field (exp/eastwest)
- **The held state is 16 location slots, not one spectrum.** `S`/`omega`/`prevPhase` are
  pointers into per-slot flat stores; `useSlot(s)` selects the current one. If you add code
  that touches held state, decide which slot(s) it hits — permanent edits target the
  **active** slot (nearest to `ewLocation`); output is a blend of all occupied slots.
- **The blend is presence-weighted, not binary occupancy.** Don't "simplify" it to
  on/off — that reintroduces the silent gap and the pop when a slot empties. The presence
  smoothstep (`kPresLo`/`kPresHi`) is what lets neighbours bridge a fading slot.
- **Injection is nearest-slot** (round `ewLocation·15`), deliberately not split across two
  slots: splitting halved the level in the crossfade. So a deposit "between" grid points
  lands on the nearer one (inaudible quantisation at 16 slots).
- **One knob records and plays.** With Feed > 0 you paint into the slot at the knob; to
  audition locations without overwriting, set Feed = 0. This dual role is by design.
- **`Save sound` was removed** here — the held state is no longer serialised (it would have
  meant persisting 16 buffers). If you see references to `writeAudioState`/`audioState`,
  they're gone.

## Integration decisions (exp/integration)
- **Harmonize has no momentary/permanent mode knob on purpose.** It must rewrite `omega`
  for tones to converge over time; a momentary variant is just a static detune. Feed > 0
  already gives the momentary feel (input tracking self-heals the edit). Don't add one.
- **The dry path is delayed by `fftSize` on purpose** (`DryDelay.h`). Mixing undelayed
  dry against the engine's latent output combs. If Dry/Wet sounds "flangey", check the
  delay length matches `engine.getLatency()`, don't remove the delay.
- **Param creation order is the Push/Maschine page grouping** (8 per page). Don't
  alphabetize or "tidy" `createLayout()` — order is meaningful. Page 2 (shaper) has 7 +
  `revMix` spilling into slot 8; accepted, not a bug.
- **Tabs are view-only.** All modules process regardless of which tab is visible; the
  display overlays follow the active tab so they don't stack.
