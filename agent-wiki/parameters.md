# Parameters

DAW-facing parameters are defined in `SpectralHoldProcessor::createLayout()`.
The engine consumes them via `SpectralEngine::Params`. FFT size is separate (GUI-only).

| GUI / id            | Range          | Default | Meaning / mapping |
|---------------------|----------------|---------|-------------------|
| Feed `feed`         | 0 .. 1         | 0.5     | Linear gain on input injected into the running FT each hop. |
| Loss `loss`         | 0 .. 1         | 0.2     | Decay of held magnitudes. `decay = exp(-loss·hop/sr·6)`. 0 = eternal hold. |
| Filter `filterAmt`  | 0 .. 1         | 0.0     | Depth of the gaussian bell shaping. 0 = flat (no shaping). |
| Tone `filterTone`   | 20 .. 20000 Hz | 1000    | Bell centre, log-skewed range (`NormalisableRange` skew 0.25). |
| Attack `attack`     | 0 .. 1         | 0.0     | Onset smoothing of injected spectrum. 0 = instant; 1 = slow. |
| Compress `compress` | -1 .. +1       | 0.0     | Per-tone level reshaping vs the average active level. >0 expands (loud louder, quiet quieter → purify); <0 homogenises (quiet up, loud down). |
| Phase Noise `phaseNoise` | bool      | off     | When on, injects ±`kPhaseNoise` rad of per-frame random jitter into each bin's phase advance (shimmer/roughness). Non-accumulating — does not permanently detune. |
| FT Size (GUI only)  | 1024 .. 8192   | 4096    | FFT size. `ComboBox`, powers of two. Not a DAW parameter. |

## Notes
- **Filter bell width** is a fixed constant `kSigmaOct = 1.25` octaves in
  `SpectralEngine.cpp` (the spec lists only amount + tone). Promote to a parameter here if
  ever requested.
- **Filter is non-destructive**: it shapes the *output* (`out = S·gFilt`), never the held
  state's decay. Turning it off restores the held waves. Compensation keeps the fed-in level
  constant regardless of `filterAmt` / `filterTone` — see [dsp-design.md](dsp-design.md).
- **Spectral brush** (not a DAW parameter): drag on the display to permanently boost/cut the
  held spectrum around a tone. See [gui-display.md](gui-display.md) / [dsp-design.md](dsp-design.md).
- **FT size** range is `kMinFftOrder=10 .. kMaxFftOrder=13` (orders, i.e. log2). Engines
  preallocate at the max order; changing size never allocates on the audio thread.
- Changing FT size **resets** the held state and changes plugin latency. Expected.
