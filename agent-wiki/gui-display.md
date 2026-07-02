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

## Shaper curve overlay
- Alpha **fades in/out over `shapeAmt ∈ [0 .. kFadeRange=0.15]`** rather than popping
  on/off at a threshold (`fadeAlpha` in `SpectrumDisplay::paint`) — Level itself still
  hard-gates visibility (`|shapeLevel| > 0.001`) since `level=0` is a flat no-op curve
  for every shape. It plots the **momentary** gain curve
  `gOut(freq)` across the same log-x axis, as a soft amber stroke, mapped linearly:
  `gOut=0` (full cut) → bottom, `gOut=1` (no change) → mid-height, `gOut≥2` (+6 dB) → top
  (clamped; the real momentary ceiling is +12 dB, the curve just clips visually there).
- It calls `ShapeCurves::shapeL` directly (`#include "ShapeCurves.h"`) — the **same**
  header-only math the DSP uses — so the displayed curve always matches the actual shaping.
  Don't reimplement any shape's math here.
- The **Level** shape needs a pivot mean of the active bins; the overlay approximates it
  from the already-smoothed display magnitudes (`smoothMag`), not the engine's exact `S`
  pivot — good enough for a preview, not meant to be sample-exact.
- Params are read live from the APVTS (`shapeAmt`, `shapeMode`, `shape`, `shapeFreq`,
  `shapeWidth`, `shapeCount`, `shapeLevel`) in `paint`.

## Brush editing (interactive)
- The view is editable: drag (mouse or pen) to reshape the held spectrum via
  `proc.applySpectralBrush(freq, strength)` → `SpectralEngine::queueBrush` (see dsp-design).
- x = tone (log) → brush centre; y → strength (top boost, centre none, bottom cut).
- **Brush cursor**: while the pointer is inside, a soft full-height glow is drawn with a
  gaussian falloff across tones (same `kBrushSigmaOct` as the DSP), tinted by sign —
  **green = boost, red = cut**, alpha by `|strength|` — plus a dot at the cursor and a faint
  centre line marking "no change". The system cursor is hidden (`NoCursor`) so this is the cursor.
- **Pen**: handled through the normal `mouseDown/Drag/Move` path. If the source reports
  pressure (`e.source.isPressureValid()`), `e.pressure` scales the brush strength; mouse =
  full strength. No separate pen event path needed.
- **Time-based, not movement-based**: holding the button at a point keeps reshaping the wave.
  The edit is applied each timer tick (30 Hz) in `timerCallback` while `brushHeld`; mouse
  events only update the target position/strength. So a static press still sculpts.
- Hovering (`mouseMove`) previews the cursor but does **not** edit; only press/hold edits.
- **Brush size** is a GUI-only `Slider` in the editor (0.1–2.0 octaves, default 0.6) — *not*
  an APVTS parameter, because pen/mouse editing only exists in the GUI. Its value is pushed to
  `SpectrumDisplay::setBrushSigmaOct` and carried **per brush event** into
  `queueBrush(centreFreq, strength, sigmaOct)`, so the DSP falloff and the on-screen cursor
  width always match the slider.

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
