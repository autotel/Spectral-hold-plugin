# PLAN — fix-list pass (branch `exp/integration`, for Sonnet)

**Status: implemented** (all of §1–§11, done on Sonnet 5 per the handoff). `./build.sh`
green, 33/33 tests. `SpectralEngine.{h,cpp}`/`ShapeCurves.h` were untouched in this pass
(as required) — the bipolar shaper rework listed under "already done" below predates the
handoff. One deviation worth flagging: the Metal knob's test (§5) couldn't confirm the
"sounds more metallic" *direction* — two different synthetic proxies gave contradicting
signal when actually measured, so the shipped test only asserts stability + that the knob
isn't a no-op (see gotchas.md). Current behavior is documented across dsp-design.md /
parameters.md / architecture.md / gotchas.md; this file stays as the historical work log.

## Already done this session (context, don't redo)
- Bipolar net-zero shaper curves (Sigmoid = tilt; Spikes/Harmonics = ± combs), permanent
  boost self-limit `kPermCeilNorm`. Level shape: Width = extent, Count = warp (#14).
- Loss localized along E–W; E–W v2 continuous locations.
- `exp/eastwest` merged into `exp/integration`.

## 1. Overlays visible regardless of tab
User: "harmonize and filter lines should show even when tab not shown, otherwise one might
have a modifier applied without knowing."
- `SpectrumDisplay.cpp`: delete the two `overlayMode == …` conditions so BOTH overlays
  always draw when their module is audibly active; remove `overlayMode`/`setOverlayMode`
  from `SpectrumDisplay.h` and the call in `PluginEditor::setActiveTab`.
- Wiki: gui-display.md "Overlay gating by editor tab" section → rewrite ("both overlays
  always drawn when active; tab no longer gates them").

## 2. Shape line must not disappear at level = 0
`SpectrumDisplay.cpp` shaper overlay: the visibility test is
`fadeAlpha > 0.001f && std::abs (shLevel) > 0.001f` — **drop the shLevel condition**
(keep the amount fade). At level=0 the curve is a flat line at unity; that's the point:
the user sees the module is armed. gui-display.md: update the sentence describing the
level hard-gate.

## 3. Remove `revFeed` (reverb→hold feedback)
User can't perceive it. Root cause (verified in code, keep this note in gotchas): the
re-injected wet enters the engine through the normal input path, whose injection is
multiplied by the **Feed knob** inside the engine — with Feed low or 0 (the typical frozen
case) revFeed does nothing audible. Making it Feed-independent needs a second (aux) input
path into `SpectralEngine` — deferred; flagged as an Opus/Fable task if ever wanted.
- Remove: `revFeed` param, `pRevFeed`, the injection block in `processBlock` (the
  `revFeedG` computation + clamped add), `revFeedCount`, the editor knob + info text, the
  two revFeed tests in `test_main.cpp`.
- Wiki: parameters.md row out; gotchas.md — replace the revFeed clamp note with the
  root-cause note above; architecture.md — drop the revFeed loop from the signal flow.

## 4. Output limiter module (replaces the hidden fixed limiter)
Two params on the existing linked limiter in `PluginProcessor.cpp` (keep `kLimAttMs = 5`
fixed, keep it cross-channel):
- `limThreshold`: −24..0 dB, default 0. Convert per block:
  `thr = juce::Decibels::decibelsToGain (pLimThreshold->load())`.
  In the loop: `target = (limEnv > thr) ? thr / limEnv : 1.0f` (was `1.0f`→`thr`).
- `limRelease`: 50..5000 ms, skew ~0.4, default 1200. Recompute `limRelCoef` per block
  from the param (one `exp` per block, fine).
- Editor: this is a module → but only 2 knobs; put them at the END of the persistent row
  (Feed, Loss, E↔W, Dry/Wet, Output, Thresh, Release) — no new tab. Info texts:
  "Output ceiling. The slow limiter pulls the level down to this." / "How fast the
  limiter recovers after pulling down."
- Test (`test_main.cpp` is engine-only; limiter lives in the processor — replicate the
  5-line envelope math in the test against a synthetic peak train, or skip the unit test
  and note it; do NOT link the processor into the test target).
- Wiki: dsp-design.md limiter section + parameters.md rows.

## 5. Reverb "Metal" knob
`PlateReverb`: new param `metal` 0..1, default 0, via `setParams (…, float metal)`.
Metallic = less diffusion + no modulation smear (discrete comb-like reflections):
- input diffusion gains ×`(1 − 0.9·metal)` (0.750/0.625 → toward ~0.07),
- `decayDiffusion1` magnitude: lerp 0.70 → 0.20 with metal (keep the sign flip),
- `excursionSamples` ×`(1 − metal)` (kills the LFO smear — the thing that prevents
  metallic ringing; turning it off IS the effect here).
Make those four values members computed in `setParams` instead of the namespace constants
being used directly in `process()`. Processor: `revMetal` param 0..1 default 0, plumb
through. Editor: knob on the Reverb tab. Test: impulse at metal=0 vs metal=1, late-window
(0.5–1 s) spectrum peak-to-median ratio strictly higher at metal=1 (comb resonances).
Wiki: parameters.md, gotchas.md (one line: metal=1 deliberately disables the LFO smear).

## 6. Shape knob shows names, not numbers
`createLayout()`: give the `shape` parameter
`AudioParameterFloatAttributes().withStringFromValueFunction(...)`: names
`{Level, Sigmoid, Spikes, Harmonics, Sine}`; if within 0.05 of an integer show the name,
else `"Level>Sigmoid"` style for the blend. The slider textbox picks it up via the APVTS
attachment automatically.

## 7. Renames (GUI labels only — parameter IDs must NOT change)
- `harmonic` knob label: "Harmonic" → "Harmonics" (editor `setupKnob` text + info text).
- `shapeMode` knob label: "Mode" → "Feed" (like the reverb's), and move it to be the LAST
  knob on the shaper tab row (Amount, Shape, Freq, Width, Count, Level, Feed). Update its
  info text: "How much the shaping is etched into the held sound vs only heard."

## 8. Reverb tab knob order: Damp before Size
Editor only: Mix, Decay, Damp, Size, Predelay, Metal (revFeed gone per §3).

## 9. Macro-page regrouping (host pages of 8 — do LAST, after §3–§5 settle the set)
Reorder `createLayout()` (creation order = Push/Maschine banks; IDs unchanged, saved
sessions restore by ID). 24 params → exactly three pages of 8:
- **P1 Hold**: feed, loss, ewLocation, dryWet, output, limThreshold, limRelease, phaseNoise
- **P2 Shaper(+)**: shapeAmt, shape, shapeFreq, shapeWidth, shapeCount, shapeLevel,
  shapeMode, harmonize   *(harmonize master closing the sculpt page is the accepted
  compromise to hit 8/8/8)*
- **P3 Space**: harmWidth, harmonic, revMix, revDecay, revDamp, revSize, revPredelay,
  revMetal
parameters.md: rewrite the page-grouping paragraph + table order.

## 10. Knob shine: more notorious, fades in smoothly
`SpectralLookAndFeel`: increase the hover glow strength (more alpha/radius), and animate
it: keep a `std::map<juce::Component*, float>` of per-slider glow levels inside the LNF,
ease toward `isMouseOverOrDragging()` each paint, and run a small shared `juce::Timer`
(~30 Hz) that repaints sliders whose glow is mid-transition (start it lazily; stop when
all settled). If the timer plumbing fights you, an acceptable fallback is a stronger glow
with the existing binary behaviour — say so in the commit message.

## 11. Wiki debt from the bipolar-shaper change (docs only)
- dsp-design.md "the spectral shaper" + parameters.md per-shape table still describe the
  old subtractive Sigmoid/Spikes/Harmonics and the old Level width/count meanings.
  Rewrite: Sigmoid = bipolar tilt (level>0 boosts above Freq / cuts below); Spikes &
  Harmonics = bipolar combs (boost teeth, cut halfway between; level<0 inverts); Level:
  Width = extent, Count = warp. Note the permanent-boost self-limit (`kPermCeilNorm`).
- plan-spectral-shaper.md: add one line to its status banner: "shapes made bipolar/net-zero
  later — see plan-fixes.md §11".

## Suggested order & model
§1+§2 (display) → §3 (revFeed out) → §4 (limiter) → §5 (metal) → §6–§8 (labels/order) →
§9 (pages) → §10 (shine) → §11 (wiki). Every step is deliberately scoped away from the
engine/DSP-math hot spots; **Sonnet 5 is the right model for all of it.** If anything
forces a change inside `SpectralEngine.{h,cpp}`/`ShapeCurves.h`, stop and flag it instead.
