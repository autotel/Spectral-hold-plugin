# Gotchas & decisions already made

Read this before "fixing" something that looks wrong — it probably isn't.

## GUI / LookAndFeel
- **`SpectralLookAndFeel`'s hover-glow animation has a teardown-order hazard, already
  guarded — don't remove the guard.** `lnf` is declared *first* in `SpectralHoldEditor`
  (so it outlives the knobs during construction), which means by C++'s reverse-order
  member destruction it's destroyed *last* — i.e. the knob `Slider`s are destroyed
  *before* `lnf`. The glow animation keeps a `std::map<Component*, float>` of hovered
  sliders and a timer that dereferences those pointers. If that timer fired between a
  knob's destruction and `lnf`'s own destructor, it would touch a dangling pointer. Fixed
  by `SpectralHoldEditor::~SpectralHoldEditor()` calling `lnf.stopGlowAnimation()` as its
  **first** statement — synchronously stops the timer and clears the map on the message
  thread before any member (knob) starts being destroyed. If you add another animated,
  per-component LookAndFeel effect, it needs the same "stop first" call in the editor
  destructor; don't assume the LNF's own destructor is early enough.

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
- Gain is exactly 1.0 below the threshold by construction (`target=1` while
  `limEnv<=thr`). Threshold (`limThreshold`, dB) and release (`limRelease`, ms) are now
  user params — 0 dB / 1200 ms are just the defaults, not fixed constants; the release
  coefficient is recomputed once per block from the param. Attack stays fixed
  (`kLimAttMs = 5`, not exposed) — it must stay fast to actually catch peaks.

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
  "boingy pipe" — if the reverb starts sounding metallic unintentionally (Metal=0), check
  these weren't dropped or set to the same rate before touching anything else.
- **`revMetal` scales the same three things down toward zero** (input diffusion gains,
  the decay-diffusion allpass magnitude, and the LFO excursion) via `PlateReverb::setParams`
  — computed once per block into member fields (`inGain1..4`, `decayDiffusion1`,
  `excursionSamples`), not baked as fixed namespace constants any more. **The perceptual
  "sounds more metallic at Metal=1" direction was NOT confirmed by a synthetic proxy** —
  two different measurements (peak-to-median spectral ratio; dominant-bin drift across
  time windows) gave opposite directional signal when actually run. This is a genuinely
  ears-only judgement; the test only asserts stability + that the knob measurably changes
  the response. If you touch the Metal mapping, re-verify by listening, not by trusting a
  new proxy metric.
- Reverb lives in `PluginProcessor` (cross-channel, mono-summed), not per-engine — same
  category as the limiter.
- **`revFeed` exists again, done right** (agent-wiki/plan-roadmap.md Part A). The *original*
  version was removed because it was inaudible: the re-injected wet entered the engine
  through the same input path as live audio, scaled by the **Feed** knob — with Feed
  low/zero (the normal frozen-hold case the knob was meant for), the injected wet vanished
  before it could do anything. The current `revFeed` fixes this with a genuinely second,
  **Feed-independent** input path (`SpectralEngine::process`'s `aux` argument; see
  dsp-design.md "Aux input path"). **Don't revive the old input-path shortcut** (feeding
  wet through the normal `in` pointer, scaled by Feed) — that's the exact bug this fixes.
  The new path also needed a feedback-loop safety ceiling that the old one never needed
  (it never fed back anything audible to begin with) — see the soft-ceiling note in
  dsp-design.md. Reverb now runs whenever `revMix > 0 || revFeed > 0` (not just
  `revMix > 0`); hard bypass requires both at 0.

## East–West location field (exp/eastwest, plan v2 — continuous locations)
- **v1 (16 slots + normalised blend) was ripped out on purpose** — it produced audible
  volume jumps while sweeping (normalised weights flip near an occupied slot). Don't
  reintroduce slots or any *normalised* location weighting; the current attenuation is
  **absolute** (`ewAtt`, gaussian, `kLocSigma`) precisely so gains can't jump.
- **Every bin carries `binLoc[k]`** and it must **travel with energy**: harmonize's unison
  merge and packet migration already carry it; any new code that moves energy between bins
  must move `binLoc` too, or tones teleport across the room.
- **Room semantics replaced the v1 tent rules**: a lone tone at East *does* fade as you
  walk West now (distance attenuation) — that's the design, not a regression. Louder tones
  carry further because attenuation multiplies amplitude.
- **Edits are distance-weighted** (permanent shaper, brush, harmonize drift ×
  `att(|knob − binLoc|)`); the momentary shaper is not (it shapes the already
  location-gained output). Knob at 0 with everything at 0 → all weights 1 → legacy.
- **One knob records and plays.** With Feed > 0 you paint at the knob position; to audition
  without overwriting, set Feed = 0. By design.
- **`Save sound` was removed** on this branch — the held state is no longer serialised. If
  you see references to `writeAudioState`/`audioState`, they're gone.

## Location layers (exp/loclayers — fixes the per-bin steal above)
- **The "per-bin locations can't hold two same-frequency tones" limit above is fixed.**
  `S`/`omega`/`binLoc` are now `kNumLayers=4` parallel copies per bin (flat arrays, stride
  `maxBins`, index via the private `li(layer, bin)`). Re-recording a pitch far from where
  it's already held **claims a fresh layer** instead of dragging/smearing the old one; only
  re-recording *near* the existing location (`att ≥ kClaimAtt`) still drags it, same as
  before. See dsp-design.md's "Location layers" section for the full routing rule.
- **`prevPhase` is shared across layers, NOT layered.** It tracks the *input's* phase for
  unwrapping — a property of the analysis, not of any held layer — and gets unconditionally
  overwritten from the input every frame regardless of which layer holds what. Migration
  (harmonize step 4) used to copy `prevPhase` along with a migrating packet; that line was
  **removed**, not preserved-per-layer — it was already a no-op (immediately overwritten by
  the same frame's main loop before it could ever be read), confirmed before removing it.
  Don't try to "fix" this by re-adding a per-layer `prevPhase` — there's nothing to fix.
- **Harmonize doesn't couple across layers.** A tone held in layer 0 and one in layer 2
  won't entrain against each other even if both are near the listener — each layer runs
  `applyHarmonize` independently. Known limitation, not a bug; revisit if it's audibly
  missed (cross-layer coupling is unimplemented future work).
- **Layer exhaustion steals the quietest layer-bin**, not the nearest or oldest. If you're
  chasing a "why did my 5th distant recording erase something" report, this is why —
  finite resource, same spirit as `ParticleEngine`'s pool eviction.
- **The old `SpectralHoldTest` case "ew re-record drags location" was replaced**, not kept
  — it asserted the drag/steal that this fix removes (East used to shrink when West was
  recorded; now it doesn't). If you're diffing test output against an old run and a test
  name is missing, that's why; the new case is "ew location layers: coexist without
  stealing".

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
- **Tabs are view-only.** All modules process regardless of which tab is visible. Display
  overlays (shaper curve, harmonize influence) are **always shown** when their module is
  active, independent of the tab — don't gate them by tab again, it hid armed modifiers
  from view on a tab the user wasn't looking at.
