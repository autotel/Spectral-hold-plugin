# Product specification (as given)

This is the user's original specification, preserved so the development path stays
congruent. Treat it as the source of truth for *intent*. Where the implementation makes
a concrete choice the spec left open, that choice is noted in **[impl]** lines.

## Concept
> Create a spectral hold plugin. The plugin contains a spectrogram to which different
> frequencies are fed in. It keeps playing the spectral frequencies onward. It doesn't
> loop a windowed sample, it literally extrapolates the FT of the sound.

**[impl]** Realised as a phase-vocoder: each FFT bin is a free-running complex phasor
that advances by its centre-frequency phase increment every hop, so held content is a
continuous set of sinusoids — not a re-triggered grain. See [dsp-design.md](dsp-design.md).

## Parameters
- **Feed** — how much of the input sound is fed into the running FT.
- **Loss** — how much the frequencies on the FT are lowered in volume through time.
- **Filter amount** — how much a gaussian bell is applied to shape the FT.
- **Filter tone** — the tone, logarithmic, at which the centre of the bell is located.
- **Attack** — whether incoming tones are added right away at their volume, or their onset
  is slowed down through time.
- **FT size** — *perhaps not presented to the DAW but present on the GUI.* Choose a good
  default; generous but not too CPU-hogging.
  **[impl]** GUI-only `ComboBox`, not a DAW parameter. Default **4096**; options 1024–8192.

## Filter compensation (important invariant)
> When a tone is input into the FT with the filter applied, the volume of the fed-in
> frequencies have to be compensated against the filter, so that they come out as if there
> was no filter, so that the relationship between in and out is always the same.

**[impl]** Fed input is multiplied by `1/filterGain(bin)` on injection, so fresh input
always enters at unity regardless of the filter. The filter then shapes only the *held*
(decaying) content as a frequency-dependent loss. See [dsp-design.md](dsp-design.md).

## Limiter
> A sort of limiter: the volume is not changed when the output is below -1 to 1 range,
> but it has to be lowered to prevent out-of-range samples. The limiter's envelope has to
> be slow.

**[impl]** Linked (stereo) peak limiter in `PluginProcessor`. Gain stays at 1.0 until the
peak envelope exceeds 1.0; release is slow (~1.2 s). See [dsp-design.md](dsp-design.md).

## Spectrogram / display
> Let the FT graph not be the classic vertical level-to-horizontal-bin. Rather: horizontal
> tone (log), level to opacity of the white vertical line, and the vertical line has to
> have more opacity and fade to black on the two top and bottom sections — basically it
> should look like a sort of smooth lighting effect. Also tint the vertical white lines,
> very slightly, with hue according to phase, going from purple-ish blue to green-ish blue.

**[impl]** `SpectrumDisplay`. x = log frequency, per-column vertical gradient (black →
colour at centre → black), alpha driven by level, hue lerped from phase across
purple-blue → green-blue at low saturation. See [gui-display.md](gui-display.md).

## Environment
> JUCE at `../JUCE`. Reference plugins at `../tape-looper` and `../lanes-audio-plugin`.
> Write a gitignore and a wiki for coding agents. Do the first git staging to commit.
