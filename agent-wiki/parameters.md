# Parameters

DAW-facing parameters are defined in `SpectralHoldProcessor::createLayout()`.
The engine consumes them via `SpectralEngine::Params`. FFT size is separate (GUI-only).

**Creation order = host page order.** Push/Maschine bank 8 consecutive params per page, so
`createLayout()` order is the grouping — 27 params, two full pages of 8 plus a partial P1
and a partial page-4 (regrouped by agent-wiki/plan-roadmap.md B0):
- **P1 "Hold"**: Feed, Loss, E↔W, Dry/Wet, Output, Limiter Threshold, Limiter Release.
- **P2 "Shaper"**: Amount, Shape, Freq, Width, Count, Level, Feed (shapeMode), Harmonize.
  Harmonize's *master amount* closes this page — accepted so the shaper's own 7 params plus
  one harmonize knob hit exactly 8; the two harmonize *character* knobs live on page 3.
- **P3 "Space"**: Harm Width, Harmonics (harmonic), Mix, Decay, Damp, Size, Predelay, Metal.
- **P4 "Perform" (partial, final target order, 4/8 — see plan-roadmap.md B0)**: Transpose,
  Spread, Phase Noise (`phaseNoiseAmt`), Reverb Feed (`revFeed`).

Parameter **IDs are unchanged** by this grouping (only `createLayout()`'s call order moved)
— saved sessions restore by ID, so this reorder is state-compatible.

**Editor layout (tabs, not a knob wall):** the display on top; a persistent performance
row **Feed, Loss, E↔W, Dry/Wet, Output, Thresh, Release**; a tab strip
**Shaper | Harmonize | Reverb** switching one shared knob row; the utility row (Live, Keep,
Undo, Transpose, Phase Noise, Brush, FT Size — see `resized()`'s width-budget comment
before adding another control, it's already tight); and an **info bar** at the bottom that
shows a one-line description of whatever control the mouse is over (Ableton-style). `spread`
has **no GUI widget** — the utility row is full; it's host-automatable / generic-editor only
until B8 (resizable editor) or a dedicated P4 tab makes room.

**Undo** (agent-wiki/plan-roadmap.md B6, GUI-only, not a DAW param): "Undo" button in the
utility row, reverts the held sound to before the last brush stroke or permanent-shaper
engagement. Edit-undo, not time-travel — harmonize drift, loss decay and normal feeding are
never snapshotted. Reuses B1's `writeHold`/`queueHoldRestore` as an in-memory ring (depth 4).

| GUI / id            | Range          | Default | Meaning / mapping |
|---------------------|----------------|---------|-------------------|
| Feed `feed`         | 0 .. 1         | 0.5     | Linear gain on input injected into the running FT each hop. |
| Loss `loss`         | 0 .. 1         | 0.2     | Decay of held magnitudes. `decay = exp(-loss·hop/sr·6)`. 0 = eternal hold. |
| E↔W `ewLocation`    | 0 .. 1         | 0.0     | Listener/recorder position on the continuous East–West line. Held tones carry their own location; they are heard through an absolute gaussian distance attenuation, input is deposited at the knob (pulling the fed tone's location), and edits reach nearest tones hardest. Parked at 0 (default) = exact legacy behaviour. See [dsp-design.md](dsp-design.md). The display's location strip shows held tones as dots and live recording/steal activity as ticks — see [gui-display.md](gui-display.md). |
| Dry/Wet `dryWet`    | 0 .. 1         | 1.0     | Global mix: engine output (1) vs untouched input (0). The dry path is delayed by `fftSize` (`DryDelay.h`) so it stays time-aligned with the wet. 1 = bit-exact wet-only (skip). |
| Output `output`     | 0 .. 2         | 1.0     | Final output level (linear gain), applied **before** the limiter so it still protects ±1. |
| Thresh `limThreshold` | -24 .. 0 dB  | 0       | Output limiter ceiling. The linked limiter pulls the level down to this; at the default 0 dB it's exactly the old fixed "don't clip ±1" behaviour. |
| Release `limRelease` | 50 .. 5000 ms | 1200   | How fast the limiter recovers after pulling down. Attack is fixed (5 ms, not exposed) — always fast enough to catch peaks. |
| Phase Noise `phaseNoiseAmt` | 0 .. 1 | 0.0     | Continuous amount of per-frame random jitter injected into each bin's phase advance (shimmer/roughness): `±kPhaseNoiseMax·amt` rad, `kPhaseNoiseMax=0.5`. Non-accumulating — does not permanently detune. Replaces the old bool `phaseNoise` (id changed; old sessions with it on migrate to `amt=0.3`, see [gotchas.md](gotchas.md)). |
| Harmonize `harmonize` | 0 .. 0.1   | 0.0     | Master amount of coupled-oscillator pitch interaction. See [harmonize.md](harmonize.md). Inherently *permanent* (no mode knob — see gotchas.md). |
| Width `harmWidth`     | 0.01 .. 3 oct | 0.5  | σ of the nearness-influence curve. |
| Harmonics `harmonic`  | 0 .. 1     | 0.0     | Character blend under Harmonize: 0 = entrainment, 1 = harmonic attraction. (GUI label was "Harmonic", id unchanged.) |
| Amount `shapeAmt`   | 0 .. 1         | 1.0     | Global depth of the shaper; scales `L[k]` before it's applied. |
| Shape `shape`       | 0 .. 4         | 0.0     | Cross-fades **Level(0) → Sigmoid(1) → Spikes(2) → Harmonics(3) → Sine(4)**. The textbox shows the shape name (or `"Name>Name"` mid-crossfade), not the raw number. |
| Freq `shapeFreq`    | 20 .. 20000 Hz | 1000    | Curve centre, log-skewed range (`NormalisableRange` skew 0.25). Meaning depends on shape (§ below). |
| Width `shapeWidth`  | 0 .. 1         | 0.5     | Width/steepness/spacing; meaning per shape. |
| Count `shapeCount`  | 0 .. 1         | 1.0     | Extent/repetition; meaning per shape. |
| Level `shapeLevel`  | -1 .. +1       | 0.0     | Signed strength. **0 = no effect for every shape** (global bypass). |
| Feed `shapeMode`    | 0 .. 1         | 0.0     | Momentary (0, output-only, non-destructive) ↔ permanent (1, fed into the held state, compounding). Continuous cross-fade. This is the shaper's *integration* knob. GUI label was "Mode"; id unchanged. Last knob on the Shaper tab (like the reverb's Feed pattern before revFeed was removed — see gotchas.md). |
| Mix `revMix`         | 0 .. 1         | 0.0     | Output reverb wet/dry, equal-power. 0 = bit-exact dry (hard bypass, see gotchas.md). |
| Decay `revDecay`     | 0 .. 1         | 0.5     | Tank feedback / tail length. |
| Damp `revDamp`       | 0 .. 1         | 0.3     | One-pole damping inside the tank: 0 = bright, 1 = dark. |
| Size `revSize`       | 0.5 .. 2.0     | 1.0     | Scales the tank delay lengths (room size). Moving it live gently pitch-bends the tail (by design). |
| Predelay `revPredelay` | 0 .. 250 ms  | 20      | Delay before the reverb's input diffusers. |
| Metal `revMetal`    | 0 .. 1         | 0.0     | Trades diffusion for a harder, more discrete reflection character: input diffusion gains shrink, the decay-diffusion allpass weakens, and the LFO excursion that smears the tank's resonances is scaled down to zero. See gotchas.md — the perceptual direction wasn't confirmed by a synthetic proxy, only by ear. |
| Feed `revFeed`      | 0 .. 1         | 0.0     | Reverb → hold feedback. A second, **Feed-independent** injection path into the engine (not the main Feed knob) — the reverb's wet tail feeds the held spectrum even at Feed=0. Soft-ceilinged per bin so the closed loop (revFeed=1, Loss=0) converges instead of diverging. See [dsp-design.md](dsp-design.md#aux-input-path-reverb-feed-revfeed-agent-wikiplan-roadmapmd-part-a). |
| Transpose `transpose` | -12 .. +12 st | 0.0   | Pitch-shifts the **output** of the held sound; the held state (`S`/`omega`) is untouched — non-destructive, reversible by ear instantly. GUI: utility-row linear slider (no tab of its own yet). MIDI notes add to this (note 60/middle C = no shift, monophonic last-note priority), unclamped — playing further from middle C keeps transposing further than the knob's own ±12 st range. See [dsp-design.md](dsp-design.md#transpose-agent-wikiplan-roadmapmd-b3). |
| FT Size (GUI only)  | 1024 .. 8192   | 4096    | FFT size. `ComboBox`, powers of two. Not a DAW parameter. |
| Live / 0 PDC (GUI only) | bool       | off     | Reports **0 latency** to the host (no plugin delay compensation) for live use. The real STFT latency is unchanged; the host just stops delay-compensating. |

**Attack** was removed — lowering Feed gives the same slowed-onset effect.
**Filter and Compress were removed** — replaced by the shaper (`shape=4, level<0` reproduces
the old Filter; `shape=0, width=0.5, count=1, mode=1` reproduces the old Compress exactly).

## The spectral shaper

One unified per-bin amplitude curve (`Source/ShapeCurves.h`), replacing the old Filter +
Compress. **All five shapes are bipolar / zero-mean** (a permanent edit is reversible in
principle — see dsp-design.md for the boost self-limit that makes compounding a boost
safe). Full math in [dsp-design.md](dsp-design.md#the-spectral-shaper). Per shape,
Freq/Width/Count take on a different meaning:

| Shape | Freq | Width | Count | Level |
|-------|------|-------|-------|-------|
| **Level** (0) | window centre (inert at width=1) | spectral extent of the effect (1 = everywhere) — **post-#14**, was Count's job | extremes-vs-mean warp (0.5 = old Compress) — **post-#14**, was Width's job | strength, as old Compress |
| **Sigmoid** (1) | tilt centre | tilt width | unused | bipolar tilt: >0 boosts above Freq / cuts below, <0 the reverse, 0 = flat |
| **Spikes** (2) | comb centre | tooth spacing | 1 tooth → covers whole spectrum | bipolar comb: >0 boosts the teeth / cuts between, <0 the reverse, flat at 0 |
| **Harmonics** (3) | fundamental | tooth width | 0 = fundamental only → 12 overtone/undertone pairs/side | bipolar comb, same convention as Spikes: >0 boosts the harmonic series / cuts between, <0 the reverse, flat at 0 |
| **Sine** (4) | pattern centre (phase) | cycles/octave | 1 lobe → repeats across spectrum | -1..+1, sign flips cut/boost (already bipolar, unchanged) |

**Width/Count swap on Level (#14):** before this pass, Width was the extremes-vs-mean warp
and Count was the spectral extent — swapped because Width-as-extent is more intuitive (it's
the control that visibly changes *how much* of the spectrum is affected, matching what
"Width" means on every other shape).

## The output reverb

A stereo plate reverb (Dattorro topology, JAES 1997) applied **after** the Output gain and
**before** the limiter, so its wet tail still gets peak-protected. `Source/PlateReverb.h`
is host-free (`juce_dsp` only, no plugin deps) — see [plan-reverb.md](plan-reverb.md) for
the full topology/constants and [gotchas.md](gotchas.md) for the hard-bypass and
size-knob-pitch-bend behavior.

## Notes
- **Spectral brush** (not a DAW parameter): drag on the display to permanently boost/cut the
  held spectrum around a tone. See [gui-display.md](gui-display.md) / [dsp-design.md](dsp-design.md).
- **FT size** range is `kMinFftOrder=10 .. kMaxFftOrder=13` (orders, i.e. log2). Engines
  preallocate at the max order; changing size never allocates on the audio thread.
- Changing FT size **resets** the held state and changes plugin latency. Expected.
- **Live**, **Keep**, **editor scale** (agent-wiki/plan-roadmap.md B8), and the active tab
  are GUI-only values persisted in the state tree (like FT size), not APVTS params;
  `setStateInformation` restores them.
- **Editor is resizable** (agent-wiki/plan-roadmap.md B8): a uniform visual scale of the
  fixed 860x580 layout (aspect-ratio-locked, 0.75x..2.0x), not a reflow — see
  [gui-display.md](gui-display.md) "Resizable editor".
- **Presets** (agent-wiki/plan-roadmap.md B9, `Source/Presets.h`, GUI convenience — no host
  program API): a ComboBox at the right end of the tab strip. Selecting one resets every DAW
  parameter to its default, then applies the preset's specific overrides (plain units via
  `convertTo0to1`); touching any knob afterward deselects it (combo shows "Preset" again).
  Never touches FT size, the held state, or GUI-only toggles. **The ~8 shipped presets are
  placeholders, not curated by ear yet** — see gotchas.md before treating their values as
  meaningful sound design.
- **Keep** (agent-wiki/plan-roadmap.md B1, default **on**): saves the held spectral state
  inside the session (gzip'd + base64'd per channel, `SpectralEngine::writeHold`/
  `queueHoldRestore`), so a frozen sound survives save/reopen. Off = old behaviour (silent
  on reload). ~100–500 KB per session when on, depending on FT size.
