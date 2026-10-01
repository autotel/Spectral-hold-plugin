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

- **`TextButton` on/off contrast and the segmented tab strip** are both driven by
  `SpectralLookAndFeel`, not per-button code. `buttonOnColourId`/`buttonColourId` (set once
  in the LNF constructor: accent fill when on, dark chrome when off) make latching buttons
  like Snap unambiguous — the default V4 look barely distinguished the two states against
  this dark theme. The tab strip's flat/segmented corners (only the strip's outer edges
  round) are keyed off `Button::getComponentID()` — the editor tags the four tab buttons
  `"tab-first"`/`"tab-mid"`/`"tab-mid"`/`"tab-last"` before adding them, and
  `drawButtonBackground` checks that ID to pick the custom flat-corner path vs. falling
  through to the normal `LookAndFeel_V4` rounded-rect for every other button (Snap, Undo,
  etc.). Adding a 5th tab means re-tagging: the *new* last tab gets `"tab-last"`, the old
  last one becomes `"tab-mid"`.

- **Editor scale: read `proc.getEditorScale()` before `setResizeLimits`** (was a real
  "always opens at minimum size" bug). `setResizeLimits` clamps the still-0x0 editor to the
  645x435 minimum, which fires `resized()`, which writes `editorScale = 0.75` back to the
  processor — so the later `setSize(860 * getEditorScale())` always used 0.75 and the
  persisted B8 scale was never restored. The constructor now captures `savedScale` first.
  Found by the offscreen screenshot target (build-and-test.md), not by reasoning.

## Host parameter display text
- **`AudioParameterFloat` defaults to 7 decimal places whenever `NormalisableRange::interval`
  is 0** (true for every continuous knob — the interval is only nonzero if you explicitly
  step-quantise a param, which we don't). Without an explicit `withStringFromValueFunction`,
  a host's own knob display (Maschine, Push, any generic param strip) shows the raw value at
  that precision — `"3820.4271000"` for a frequency knob, no unit, all digits. Every float
  param in `createLayout()` now sets one of four shared formatter helpers (see
  [parameters.md](parameters.md) Notes for what each does); if you add a new float param,
  give it one too, or it silently reverts to the 7-decimal default. `AudioParameterBool`
  doesn't have this problem — its default `getText` already returns `"On"`/`"Off"`.
- **`shape` is the one param that already looked right** before this pass — it has its own
  `shapeValueToString` (curve names, e.g. `"Sigmoid"`, `"Level>Sigmoid"` mid-crossfade). Not
  a generic formatter; don't try to unify it with the others, the crossfade text is specific
  to how the shaper's 5 curves blend into each other.
- **Don't `setTextValueSuffix` on an APVTS-attached knob whose param already has a unit.**
  `SliderAttachment` shows the parameter's own text (which now includes the unit), so an
  editor-side suffix doubles it — Transpose/Glide read `"5 st st"`/`"0 ms ms"` until the
  suffixes were removed. Only non-parameter sliders (Brush's `" oct"`) need a suffix.

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

## DC / sub-bass runaway (was a real "channel chokes after ~10 min" bug)
- **Symptom:** with the reverb on (esp. `revFeed` > 0, high Decay, low/zero Loss), after
  minutes the output fills with a DC offset + 10–40 Hz rumble; the limiter ducks all real
  content under it, and downstream plugins choke on the DC.
- **Cause (3 parts):** (1) `PlateReverb`'s tank passes DC with its *highest* gain (~7× at
  max decay — allpasses and the damping LP are unity at DC, the loop recirculates it, the
  signed output taps don't cancel), so the revFeed loop (engine → reverb → aux → engine)
  had loop gain > 1 exactly at the DC/sub bins, which then grew *exponentially from float
  noise* — nothing for minutes, then they sit at the aux soft ceiling. (2) The engine held
  bin 0 like any tone: a 0 Hz phasor is a DC offset, and at `loss = 0` it integrated any
  input DC forever (0.2 DC in → output mean 8.1 after 20 s). (3) No DC/sub filtering
  anywhere in the loop.
- **Fix (don't remove any of these):** bin 0 is never held (zeroed every frame, the main
  per-bin loop starts at `k = 1`); aux injection is faded out below 20→40 Hz
  (`auxLowCut`, `kAuxCutLoHz/HiHz`); the engine output passes a one-pole 5 Hz DC blocker
  (`kDcBlockHz`, state `dcX1/dcY1`, reset in `reset()`); `PlateReverb` has its own 5 Hz DC
  blocker on the input, after the predelay. Tests: the three `dc*` cases in `test_main.cpp`.
- A sliver of input DC still leaks into bin 1 through the Hann window and is held there at
  ω≈0; the 5 Hz blocker removes the static part. At `loss = 0` with constant DC input the
  held level still grows linearly (see "Loss = 0 means eternal hold"), so a tiny residual
  offset remains (~0.07 % of the signal RMS) — expected, and why `dc2` checks it relative.

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
- Main spectrum is stereo since agent-wiki/plan-roadmap.md B7 (`max` per bin across
  channels, phase from ch 0; falls back to ch 0 alone on mono/busy). The **location strip**
  (also B7) is still **channel 0 only** — `copyLayers` isn't merged across channels, unlike
  the main view. See [gui-display.md](gui-display.md).

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
- **The reverb input has a 5 Hz DC blocker** (after the predelay, before the bandwidth LP).
  The Dattorro tank amplifies DC several × at high decay; see "DC / sub-bass runaway".
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
- **`Save sound` (the old `writeAudioState`/`audioState` feature) was removed and later
  superseded.** The held state IS serialised again, differently — see "Hold serialization"
  below (agent-wiki/plan-roadmap.md B1). If you see references to `writeAudioState`, they're
  gone for good; `SpectralEngine::writeHold`/`queueHoldRestore` is the current mechanism.

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

## Hold serialization (agent-wiki/plan-roadmap.md B1, `exp/roadmap`)
- **`writeHold()` takes no lock of its own** — it reads live audio-thread state (`S`,
  `omega`, `binLoc`), so the caller (`PluginProcessor::getStateInformation`) must hold
  `getCallbackLock()` for the duration of the call. Don't call it unlocked.
- **Restore is queued, not immediate.** `queueHoldRestore()` (message thread) stashes the
  blob; it's applied on the audio thread at the next frame boundary, same pattern as
  `queueBrush`/`setOrder`. Don't expect the engine's state to change synchronously.
- **Order gate.** A restored blob only applies if its FFT order matches the engine's
  *current* order. `setStateInformation` calls `setFftOrder()` **before** queuing the hold
  restores specifically so the order lands first; if you reorder that, restores whose
  order differs from the default will silently get dropped (the "no pendingOrder queued"
  discard path, not a bug in isolation — just wrong call order).
- **Format has no forward-compat plan.** `version` is checked for equality (`!= 1` discards
  the whole blob); there's no migration path yet. If you change the wire format, bump
  `version` and decide then whether old blobs should be discarded (current behaviour) or
  migrated.
- **`keepSound` gates the save, not the restore.** Restoring an old blob from a session
  works even if `keepSound` is later turned off; turning it off only stops writing *new*
  blobs. Turning it off does not clear the currently-held sound either — it's a save-time
  switch, not a mute.

## Integration decisions (exp/integration)
- **Harmonize has no momentary/permanent mode knob on purpose.** It must rewrite `omega`
  for tones to converge over time; a momentary variant is just a static detune. Feed > 0
  already gives the momentary feel (input tracking self-heals the edit). Don't add one.
- **The dry path is delayed by `fftSize` on purpose** (`DryDelay.h`). Mixing undelayed
  dry against the engine's latent output combs. If Dry/Wet sounds "flangey", check the
  delay length matches `engine.getLatency()`, don't remove the delay.
- **Param creation order is the Push/Maschine page grouping** (8 per page). Don't
  alphabetize or "tidy" `createLayout()` — order is meaningful. Page 2 (shaper) has its own
  7 + `harmonize`'s master amount spilling into slot 8 (not `revMix` — see
  [parameters.md](parameters.md) for the current 4-page table); accepted, not a bug. As of
  agent-wiki/plan-uifix.md U3 there's a 4th, partial page too (`transpose`, `transposeSnap`,
  `transposeGlide`, `midiIgnore`, `spread`, `phaseNoiseAmt`, `revFeed` — 7/8, accepted
  partial). `transposeSnap`/`transposeGlide`/`midiIgnore` are inserted right after
  `transpose`, not appended at the end, specifically to keep the transpose group together. P1 is 7 slots, not 8 — see below.
- **The 4th tab is "Alter", and it sits *before* Reverb** (was "Perform", last in the strip).
  Tab indices moved with it: 0 Shaper, 1 Harmonize, **2 Alter, 3 Reverb** — `uiTab` is
  persisted as a bare int, so a session saved before the rename reopens on the *other* of
  those two tabs. Cosmetic, one time, not worth a migration.
- **MIDI note transposition is off by default** (`midiIgnore` = true). B3 shipped it always
  on; a hold sitting on a MIDI-fed track was silently re-pitched by notes meant for
  something else. The gate is in `processBlock` *before* the note-tracking loop and also
  clears `midiNote`, so flipping to Follow never inherits a note held from before.
- **`spread` and `transpose` moved off the utility row onto a 4th tab**
  (agent-wiki/plan-uifix.md U3). `spread` had no widget at all from B5 until U3 — the plan
  never specified one. `transpose` was a utility-row linear slider despite its large sonic
  impact; now a knob, same tab, with a Snap toggle and a Glide knob alongside it. **B8's
  resizable editor did NOT create this room** — it's a uniform visual scale of the same
  fixed 860-unit layout, no extra logical space at any window size; the actual fix was
  giving these controls their own tab, exactly the escape hatch B5's/B6's gotchas already
  pointed at.
- **Undo (B6) is GUI/message-thread only, never touches the audio thread's own locking.**
  `snapshotHold()`/`undoHold()` live on `SpectralHoldProcessor`, reuse B1's
  `writeHold`/`queueHoldRestore`, and store 4 in-memory blob pairs (no gzip/base64 — that's
  only for the XML session state in get/setStateInformation). Two triggers:
  `SpectrumDisplay::mouseDown` (before `brushHeld = true`, so it's once per stroke) and the
  editor's `parameterChanged` on `shapeMode`/`shapeLevel` (see the blanket-listener bullet
  below), snapshotting on the false→true edge of "armed permanent". That path only fires for
  GUI-driven changes (`setValueNotifyingHost`) — host automation of those two params calls
  `setValue` directly and never reaches `AudioProcessorParameter::Listener`, so an automated
  engage won't snapshot. Accepted: undo is for the interactive editing workflow, not
  automation.
- **The editor listens to EVERY APVTS parameter, not a curated few.** `SpectralHoldEditor`'s
  ctor/dtor walk `proc.apvts.state`'s `PARAM` children (present for every registered param
  right after APVTS construction — same trick as the B4 migration code) and
  add/removeParameterListener for each id, rather than hardcoding the list. One
  `parameterChanged` override now serves two features: B6's shaper-armed-permanent edge
  detection (`shapeMode`/`shapeLevel` specifically) and B9's preset-deselect-on-edit (any
  param). If you add a feature that needs to react to "any parameter changed", it goes in
  this same callback — don't add a second blanket registration loop.
- **Presets (B9) are uncurated placeholders.** `Source/Presets.h`'s ~8 presets exist to prove
  the mechanism (ComboBox → reset-to-default → apply overrides → deselect-on-edit), not as
  finished sound design — the file says so, but don't assume the shipped values are
  meaningful until someone tunes them by ear. `applyPreset()` resets **every** DAW parameter
  to its default first (`AudioProcessorParameter::getDefaultValue()`, generic across
  `AudioParameterFloat`/`Bool`), then overrides only what the preset lists — so "unlisted
  params reset to default" is real, not just missing coverage.
- **Freeze was implemented, then cut.** agent-wiki/plan-roadmap.md B2 added a `freeze` bool
  (stop time: no feed, no tracking, no decay) with its own P1 slot and button. Removed
  wholesale — `feed=0` already halts injection/tracking, so the extra param only bought
  pinning decay too, not worth a dedicated control. Don't re-add without a concrete request.
- **`phaseNoise` (bool) became `phaseNoiseAmt` (float 0..1) in B4.** The id changed, so old
  automation on the bool is lost; sessions saved with the bool "on" migrate to `amt=0.3` in
  `setStateInformation` (checked **before** `apvts.replaceState()` — replaceState redirects
  `state` onto the same reference-counted tree and appends missing PARAM children in place,
  so checking after it would always see a freshly-defaulted `phaseNoiseAmt` and never detect
  an old session).
- **Tabs are view-only.** All modules process regardless of which tab is visible. Display
  overlays (shaper curve, harmonize influence) are **always shown** when their module is
  active, independent of the tab — don't gate them by tab again, it hid armed modifiers
  from view on a tab the user wasn't looking at.
