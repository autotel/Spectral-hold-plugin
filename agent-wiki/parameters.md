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
| FT Size (GUI only)  | 1024 .. 8192   | 4096    | FFT size. `ComboBox`, powers of two. Not a DAW parameter. |

## Notes
- **Filter bell width** is a fixed constant `kSigmaOct = 1.25` octaves in
  `SpectralEngine.cpp` (the spec lists only amount + tone). Promote to a parameter here if
  ever requested.
- **Filter compensation** keeps the fed-in level constant regardless of `filterAmt` /
  `filterTone` — see [dsp-design.md](dsp-design.md). Don't "fix" the apparent gain coupling;
  it's intentional and only affects the held tail.
- **FT size** range is `kMinFftOrder=10 .. kMaxFftOrder=13` (orders, i.e. log2). Engines
  preallocate at the max order; changing size never allocates on the audio thread.
- Changing FT size **resets** the held state and changes plugin latency. Expected.
