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

## Overlays are always visible, regardless of tab
Both module overlays (shaper curve, harmonize influence) exist in `SpectrumDisplay` and
**both draw whenever their module is audibly active**, independent of which editor tab is
showing. This is deliberate: if the tab gated visibility, a modifier could be armed on a
tab you're not looking at with no visual sign of it. The active tab (`uiTab`, persisted
like `fftOrder`) only controls which knob row is shown — it no longer touches the display.

## Info bar
A one-line label at the very bottom of the editor. Every control registers a description
via `SpectralHoldEditor::setInfo` (shared `MouseListener`, mouseEnter/mouseExit) —
Ableton-style. When adding a control, register its text there too.

## Shaper curve overlay
- Alpha **fades in/out over `shapeAmt ∈ [0 .. kFadeRange=0.15]`** rather than popping
  on/off at a threshold (`fadeAlpha` in `SpectrumDisplay::paint`). Unlike an earlier
  version, `level=0` does **not** hide the line — it draws flat at unity gain, which is
  the intended signal that the module is armed (Amount > 0) even though Level is neutral.
  Only Amount fades the overlay out. It plots the **momentary** gain curve
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
- **`mouseDown` snapshots for Undo** (agent-wiki/plan-roadmap.md B6) before setting
  `brushHeld = true`: `proc.snapshotHold()`, once per stroke (not per drag tick or timer
  tick), so Undo reverts the whole gesture, not the last tick of it.
- **Brush size** is a GUI-only `Slider` in the editor (0.1–2.0 octaves, default 0.6) — *not*
  an APVTS parameter, because pen/mouse editing only exists in the GUI. Its value is pushed to
  `SpectrumDisplay::setBrushSigmaOct` and carried **per brush event** into
  `queueBrush(centreFreq, strength, sigmaOct)`, so the DSP falloff and the on-screen cursor
  width always match the slider.

## Data path
- 30 Hz `Timer` calls `proc.getDisplaySnapshot()` → `SpectralEngine::copyDisplay()` per
  channel, which copies per-bin `|S|` (normalised by `2/fftSize`) and `arg(S)` under a
  try-lock. If the engine is mid-frame the call returns 0 and the last frame is kept (no
  stall). Since agent-wiki/plan-roadmap.md B7, `getDisplaySnapshot` is **stereo**: it takes
  `max(mag[ch0], mag[ch1])` per bin, phase stays channel 0's — falls back to channel 0 alone
  on a mono bus or if channel 1's copy is busy/mismatched-size.
- Magnitude is visually smoothed (`smoothMag`, fast-attack / slow-release) to cut flicker,
  then mapped to brightness in dB: `-80..0 dB → 0..1`.

## Location strip (agent-wiki/plan-roadmap.md B7)
The E<->W field (agent-wiki/plan-eastwest.md) is otherwise invisible — this makes it visible.
- **Data**: `proc.getLocationSnapshot()` → `SpectralEngine::copyLayers()` (channel 0 only),
  same try-lock/keep-last-frame-if-busy pattern as the main snapshot. Returns a flat,
  layer-major buffer (`layerMag`/`layerLoc`, stride `locBins`); the layer count isn't a
  public engine constant, so the GUI infers it as `layerMag.size() / locBins`.
- **Layout**: `paint()` reserves a `kLocStripH = 40px` band at the bottom by shrinking its
  local `H` (used for every main-view vertical extent) to `getHeight() - kLocStripH`, keeping
  a separate `fullH` for the strip's own coordinate space. Everything above that line in
  `paint()` is unchanged code operating on a smaller `H` — no other edits needed to confine
  the main "lighting" view + overlays above the strip.
- **Mapping**: x is the *same* `freqToX`/`xToFreq` as the main view (they depend only on
  width, unaffected by the `H`/`fullH` split). y: East (`loc=0`) at the strip's bottom, West
  (`loc=1`) at its top, matching `ewLocation`'s 0=East/1=West convention. Per bin/layer with
  `mag > 1e-4`: a small dot, `alpha = sqrt(mag)` (uncalibrated, a legibility choice like the
  shaper-curve preview, not a precise loudness mapping). A thin horizontal line marks the
  `ewLocation` knob's current value (the listener position).
- **Hue by layer, not phase**: `copyLayers` doesn't carry phase, so layers are colour-coded
  by index instead, spread across the same `0.48..0.72` hue band the main view's phase-hue
  uses (visually "the same family", not the same mapping).
- **Known overlap**: the brush's y→strength mapping (`yToStrength`) still uses the full
  `getHeight()`, unchanged — a click inside the strip's 40px band both shows the strip *and*
  registers as a near-maximum-cut brush edit at that x. Not fixed; the plan didn't call for a
  separate hit-test policy for the strip and this doesn't break anything, just double-duty.

## Tuning knobs (all in SpectrumDisplay.cpp)
- Frequency range: `kMinHz`, `kMaxHz`.
- dB window: the `(db + 80)/80` mapping in `levelToBright`.
- Glow shape: the gradient stop alphas/positions.
- Tint: the `hue`/`sat` lines. Keep saturation low to preserve the "very slightly" intent.
- Smoothing: the `0.6` / `0.15` coefficients in `timerCallback`.
- Location strip: `kLocStripH` (px), the per-dot `alpha = sqrt(mag)` scale, the per-layer hue
  spread.

## Resizable editor (agent-wiki/plan-roadmap.md B8)
`PluginEditor.{h,cpp}`, not `SpectrumDisplay` — noted here since it changes how everything
above is actually presented on screen.
- **Structure**: every child the editor used to own directly (`display`, all `Knob`s, tabs,
  utility-row controls, `infoBar`) is now parented to `content`, a `juce::Component` fixed at
  **860x580**. The editor itself only holds `content` and paints the background.
- **Scaling, not reflow**: the editor's `resized()` sets
  `content.setTransform (AffineTransform::scale (getWidth() / 860.0f))` and
  `content.setBounds (0, 0, 860, 580)` — the bounds never actually change (same values every
  call), so `content`'s own `resized()` (→ `SpectralHoldEditor::layoutContent()`, the old
  `resized()` body, unchanged) only fires **once**, on the first bounds assignment. Every
  later window resize only touches the transform. Consequence: resizing the window makes
  everything uniformly bigger/smaller — it does **not** create more logical layout room (the
  utility row is exactly as cramped at 1720px wide as at 860px; see its width-budget comment
  in `layoutContent()`).
- **Mouse coordinates just work.** JUCE maps `MouseEvent` positions through a component's
  `AffineTransform` automatically, so `SpectrumDisplay`'s brush math (`xToFreq`/`yToStrength`,
  driven by its own `getWidth()`/`getHeight()`, fixed at content's logical size) needed zero
  changes — confirmed by the existing DSP tests still passing (no GUI test harness exists to
  drive real mouse events here; verified by reasoning through JUCE's transform semantics and
  by the fact that `display`'s own bounds don't change no matter the window's actual size).
- **Persistence**: `editorScale` (GUI-only float, like `liveMode`/`uiTab`/`keepSound`) saved
  in `get/setStateInformation`, restored via `setSize (860*s, 580*s)` in the editor ctor
  *before* `setResizable`/`setResizeLimits` are wired up. Range 0.75..2.0, matching
  `setResizeLimits (645, 435, 1720, 1160)` (645/860 = 0.75, 1720/860 = 2.0).
- **Aspect ratio locked**: `getConstrainer()->setFixedAspectRatio (860.0/580.0)` — dragging a
  corner can't produce a non-860:580 window, so `getWidth()/860` and `getHeight()/580` always
  agree; only width is used for the scale factor.
