# GUI — the spectrum "lighting" display

`Source/SpectrumDisplay.{h,cpp}`. Deliberately **not** a classic spectrogram.

## What it shows
- **x axis = tone, logarithmic** (`kMinHz=20` .. `min(20k, nyquist)`), low → high.
- For each pixel column: map x → frequency → fractional bin, interpolate magnitude/phase.
- **Level → opacity** of a white-ish vertical line at that column.
- The vertical line is a **gradient**: transparent at top/bottom, brightest in the centre
  band (`addColour(0.5, …)` full, `0.28`/`0.72` shoulders at 35%). Reads as a soft glow /
  lighting effect rather than a bar.
- **Phase → hue**, very slightly: `pt = (phase+π)/2π`, `hue = 0.72 - 0.24·pt`
  (purple-blue → green-blue), saturation only **0.25** so lines stay near-white.

## Filter curve overlay
- Drawn **only when Filter amount > 0**. It plots `SpectralEngine::filterGain(freq, tone, amt)`
  across the same log-x axis (`1` at top = no attenuation, `0` at bottom = full bell cut), as a
  soft amber stroke, plus a faint vertical line at the bell centre (Filter Tone).
- It calls the **same** `SpectralEngine::filterGain` static the DSP uses, so the displayed
  curve always matches the actual shaping (shared `kSigmaOct`). Don't reimplement the bell here.
- Params are read live from the APVTS (`filterAmt`, `filterTone`) in `paint`.

## Data path
- 30 Hz `Timer` calls `proc.getDisplaySnapshot()` → `SpectralEngine::copyDisplay()`,
  which copies per-bin `|S|` (normalised by `2/fftSize`) and `arg(S)` under a try-lock.
  If the engine is mid-frame the call returns 0 and the last frame is kept (no stall).
- Magnitude is visually smoothed (`smoothMag`, fast-attack / slow-release) to cut flicker,
  then mapped to brightness in dB: `-80..0 dB → 0..1`.

## Tuning knobs (all in SpectrumDisplay.cpp)
- Frequency range: `kMinHz`, `kMaxHz`.
- dB window: the `(db + 80)/80` mapping in `levelToBright`.
- Glow shape: the gradient stop alphas/positions.
- Tint: the `hue`/`sat` lines. Keep saturation low to preserve the "very slightly" intent.
- Smoothing: the `0.6` / `0.15` coefficients in `timerCallback`.

## Note
The display reflects channel 0 only (`engines[0]`). Fine for a stereo hold; revisit if a
stereo/mid-side view is ever wanted.
