# Parameters

DAW-facing parameters are defined in `SpectralHoldProcessor::createLayout()`.
The engine consumes them via `SpectralEngine::Params`. FFT size is separate (GUI-only).

**Creation order = host page order.** Push/Maschine bank 8 consecutive params per page,
so `createLayout()` groups them: **page 1 "Hold"** (Feed, Loss, Dry/Wet, Output, Phase
Noise, Harmonize, Harm Width, Harmonic — exactly 8), **page 2 "Shaper"** (7; `revMix`
spills into its 8th slot — accepted, see [plan-integration.md](plan-integration.md) §3),
**page 3 "Reverb"** (the rest).

**Editor layout (tabs, not a knob wall):** the display on top; a persistent performance
row **Feed, Loss, Dry/Wet, Output**; a tab strip **Shaper | Harmonize | Reverb** switching
one shared knob row; the utility row (Phase Noise, Live, Save sound, Brush, FT Size); and
an **info bar** at the bottom that shows a one-line description of whatever control the
mouse is over (Ableton-style).

| GUI / id            | Range          | Default | Meaning / mapping |
|---------------------|----------------|---------|-------------------|
| Feed `feed`         | 0 .. 1         | 0.5     | Linear gain on input injected into the running FT each hop. |
| Loss `loss`         | 0 .. 1         | 0.2     | Decay of held magnitudes. `decay = exp(-loss·hop/sr·6)`. 0 = eternal hold. |
| Dry/Wet `dryWet`    | 0 .. 1         | 1.0     | Global mix: engine output (1) vs untouched input (0). The dry path is delayed by `fftSize` (`DryDelay.h`) so it stays time-aligned with the wet. 1 = bit-exact wet-only (skip). |
| Output `output`     | 0 .. 2         | 1.0     | Final output level (linear gain), applied **before** the limiter so it still protects ±1. |
| Phase Noise `phaseNoise` | bool      | off     | When on, injects ±`kPhaseNoise` rad of per-frame random jitter into each bin's phase advance (shimmer/roughness). Non-accumulating — does not permanently detune. |
| Harmonize `harmonize` | 0 .. 0.1   | 0.0     | Master amount of coupled-oscillator pitch interaction. See [harmonize.md](harmonize.md). Inherently *permanent* (no mode knob — see gotchas.md). |
| Width `harmWidth`     | 0.01 .. 3 oct | 0.5  | σ of the nearness-influence curve. |
| Harmonic `harmonic`   | 0 .. 1     | 0.0     | Character blend under Harmonize: 0 = entrainment, 1 = harmonic attraction. |
| Amount `shapeAmt`   | 0 .. 1         | 1.0     | Global depth of the shaper; scales `L[k]` before it's applied. |
| Mode `shapeMode`    | 0 .. 1         | 0.0     | Momentary (0, output-only, non-destructive) ↔ permanent (1, fed into the held state, compounding). Continuous cross-fade. This is the shaper's *integration* knob. |
| Shape `shape`       | 0 .. 4         | 0.0     | Cross-fades **Level(0) → Sigmoid(1) → Spikes(2) → Harmonics(3) → Sine(4)**. |
| Freq `shapeFreq`    | 20 .. 20000 Hz | 1000    | Curve centre, log-skewed range (`NormalisableRange` skew 0.25). Meaning depends on shape (§ below). |
| Width `shapeWidth`  | 0 .. 1         | 0.5     | Width/steepness/spacing; meaning per shape. |
| Count `shapeCount`  | 0 .. 1         | 1.0     | Extent/repetition; meaning per shape. |
| Level `shapeLevel`  | -1 .. +1       | 0.0     | Signed strength. **0 = no effect for every shape** (global bypass). |
| Mix `revMix`         | 0 .. 1         | 0.0     | Output reverb wet/dry, equal-power. 0 = bit-exact dry (hard bypass, see gotchas.md). |
| Decay `revDecay`     | 0 .. 1         | 0.5     | Tank feedback / tail length. |
| Size `revSize`       | 0.5 .. 2.0     | 1.0     | Scales the tank delay lengths (room size). Moving it live gently pitch-bends the tail (by design). |
| Damp `revDamp`       | 0 .. 1         | 0.3     | One-pole damping inside the tank: 0 = bright, 1 = dark. |
| Predelay `revPredelay` | 0 .. 250 ms  | 20      | Delay before the reverb's input diffusers. |
| Feed `revFeed`       | 0 .. 1         | 0.0     | The reverb's *integration* knob: last block's wet (clamped ±1, ×0.5) re-injected into the engines' input, so the tail becomes part of the held sound. Active only while `revMix > 0`. |
| FT Size (GUI only)  | 1024 .. 8192   | 4096    | FFT size. `ComboBox`, powers of two. Not a DAW parameter. |
| Live / 0 PDC (GUI only) | bool       | off     | Reports **0 latency** to the host (no plugin delay compensation) for live use. The real STFT latency is unchanged; the host just stops delay-compensating. |
| Save sound (GUI only)   | bool       | off     | When on, the saved preset **includes the held spectral state** (per-engine S/omega/phase/Xs), so reloading restores the ongoing frozen sound. |

**Attack** was removed — lowering Feed gives the same slowed-onset effect.
**Filter and Compress were removed** — replaced by the shaper (`shape=4, level<0` reproduces
the old Filter; `shape=0, width=0.5, count=1, mode=1` reproduces the old Compress exactly).

## The spectral shaper

One unified per-bin amplitude curve (`Source/ShapeCurves.h`), replacing the old Filter +
Compress. Full math in [dsp-design.md](dsp-design.md#the-spectral-shaper). Per shape,
Freq/Width/Count take on a different meaning:

| Shape | Freq | Width | Count | Level |
|-------|------|-------|-------|-------|
| **Level** (0) | window centre (inert at count=1) | extremes-vs-mean warp (0.5 = old Compress) | spectral extent of the effect (1 = everywhere) | strength, as old Compress |
| **Sigmoid** (1) | slope position | ramp width | unused (v1) | >0 = highpass, <0 = lowpass, 0 = flat |
| **Spikes** (2) | pattern centre | spike spacing | 1 spike → covers whole spectrum | subtractive band-select: >0 = **reject** peaks (notch), <0 = pass **only** peaks (cut the rest), flat at 0 |
| **Harmonics** (3) | fundamental | spike width | 0 = fundamental only → 12 overtone/undertone pairs/side | subtractive band-select, same sign convention as Spikes: >0 = **reject** the harmonic series, <0 = pass **only** the series (isolate tones related to Freq), flat at 0 |
| **Sine** (4) | pattern centre (phase) | cycles/octave | 1 lobe → repeats across spectrum | -1..+1, sign flips cut/boost |

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
- **Live / Save sound** are GUI-only booleans persisted in the state tree (like FT size), not
  APVTS params. `setStateInformation` restores them; with *Save sound* the held state is stored
  as a base64 binary `audioState` property and reloaded via `SpectralEngine::read/writeAudioState`
  under `audioStateLock` (which `processBlock` also takes, so restore can't race processing).
