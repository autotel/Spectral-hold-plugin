# GUI — the spectrum "lighting" display

`Source/SpectrumDisplay.{h,cpp}`. Deliberately **not** a classic spectrogram.

## What it shows
- **x axis = tone, logarithmic** (`kMinHz=20` .. `min(20k, nyquist)`), low → high.
- For each pixel column: map x → frequency → fractional bin, interpolate magnitude/phase
  **per channel** (agent-wiki/plan-uifix.md U1 — see "Split stereo" below).
- **Level → opacity** of a white-ish vertical line at that column.
- The vertical line is a **gradient**, one channel each half: transparent at top/bottom,
  brightest at centre (`addColour(0.49/0.51, …)` full for L/R respectively, `0.28`/`0.72`
  shoulders at 35%). Reads as a soft glow / lighting effect rather than a bar.
- **Phase → hue**, very slightly, per channel: `pt = (phase+π)/2π`, `hue = 0.72 - 0.24·pt`
  (purple-blue → green-blue), saturation only **0.25** so lines stay near-white.

## Split stereo (agent-wiki/plan-uifix.md U1)
Left channel's half of the gradient projects **up** from centre, right channel's projects
**down**. Was previously `max(magL, magR)` per bin with a single symmetric gradient (see B7
below) — that made the held tone look "ribbed" whenever the two channels differed even
slightly (real stereo input; Phase Noise, whose jitter RNG is seeded independently per
engine/channel; Spread's per-bin ± gain hash), because adjacent bins could jump between two
different leakage curves from frame to frame. With genuinely identical L/R (mono material,
or Phase Noise/Spread both off) the two halves mirror into the same symmetric spike the
single-gradient version drew — the split costs nothing visually until the channels actually
differ, which is exactly when the extra information becomes honest and useful (you can see
stereo width, not just a merged blur of it). A column is skipped only when **both** channels
are quiet, so a tone alive in just one channel still renders on its own half.

## Overlays are always visible, regardless of tab
Both module overlays (shaper curve, harmonize influence) exist in `SpectrumDisplay` and
**both draw whenever their module is audibly active**, independent of which editor tab is
showing. This is deliberate: if the tab gated visibility, a modifier could be armed on a
tab you're not looking at with no visual sign of it. The active tab (`uiTab`, persisted
like `fftOrder`) only controls which knob row is shown — it no longer touches the display.

## Alter tab (agent-wiki/plan-uifix.md U3; called "Perform" until the rename)
`PluginEditor.{h,cpp}`, not `SpectrumDisplay` — noted here since it's the fourth tab, same
radio-toggle pattern (`setActiveTab`, 0..3 now instead of 0..2). Strip order is **Shaper |
Harmonize | Alter | Reverb**, i.e. Alter is index **2** and Reverb index **3** (the rename
moved Alter ahead of Reverb; `uiTab` persists a bare int, so a pre-rename session reopens on
the other of those two — cosmetic, no migration). Holds **Transpose, Glide, Spread, Phase
Noise** knobs plus a latching **Snap** toggle button and the **MIDI Ignore/Follow** switch
(both carved from the row's right edge, same pattern the old Freeze button used; the switch
is a `VerticalToggle` ported from `../lanes-audio-plugin`, a plain `Component` driven by a
raw `ParameterAttachment` on `midiIgnore` rather than a `ButtonAttachment`) — all four knobs
share the tabbed-row area exactly like the other three tabs'
knob rows do. Transpose was a utility-row linear slider before U3 despite its large sonic
impact; Spread had no widget at all since B5. Moving both here (plus Phase Noise, previously
also a utility-row slider) is what actually freed up the utility row — **not** B8's
resizable editor, which is a visual scale of the same fixed layout and creates no extra
logical space at any window size (see "Resizable editor" below).

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
  from the already-smoothed display magnitudes, **channel 0 only** (`smoothMagL` — see U1's
  split-stereo display), not the engine's exact `S` pivot — good enough for a preview, not
  meant to be sample-exact.
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
  stall). Since agent-wiki/plan-roadmap.md B7, `getDisplaySnapshot` is **stereo**; since
  agent-wiki/plan-uifix.md U1 it returns each channel **split** (`magL/phaseL`,
  `magR/phaseR`), not merged — the old `max(mag[ch0], mag[ch1])` per bin was the source of
  the "ribbed" look (see U1 above). Falls back to channel 0 in both L and R on a mono bus or
  if channel 1's copy is busy/mismatched-size.
- Magnitude is visually smoothed **per channel** (`smoothMagL`/`smoothMagR`, fast-attack /
  slow-release) to cut flicker, then mapped to brightness in dB: `-80..0 dB → 0..1`.

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
  shaper-curve preview, not a precise loudness mapping).
- **Hue by layer, not phase**: `copyLayers` doesn't carry phase, so layers are colour-coded
  by index instead, spread across the same `0.48..0.72` hue band the main view's phase-hue
  uses (visually "the same family", not the same mapping).
- **Known overlap**: the brush's y→strength mapping (`yToStrength`) still uses the full
  `getHeight()`, unchanged — a click inside the strip's 40px band both shows the strip *and*
  registers as a near-maximum-cut brush edit at that x. Not fixed; the plan didn't call for a
  separate hit-test policy for the strip and this doesn't break anything, just double-duty.

## Injection ticks — recording/steal state (agent-wiki/plan-uifix.md U2)
The strip used to also draw a thin horizontal line at the `ewLocation` knob's current value.
Removed: it was redundant, the knob already shows its own value. In its place, the strip now
shows something the knob *can't*: where input is landing this instant, and whether it's
dragging (stealing) an existing held tone or cleanly claiming a quiet one.
- **Data**: `SpectralEngine::processFrame`'s per-bin injection block already computes
  everything needed — `aInj` (injection strength), `claimed` (landed on a freshly-claimed
  quiet layer vs. the nearest-layer drag path), and `aHeld` (the receiving layer's magnitude
  *before* this hop's injection is added). Per-frame scratch (`injLocScratch`/
  `injStrengthScratch`/`injDragScratch`, plain per-bin, **not** layered — injection always
  resolves to one layer per bin) is written inside that block and copied into guarded
  `dispInj*` members in the same try-locked block as `dispMag`/`dispLayerMag`. New
  `SpectralEngine::copyInjection()` / `Processor::getInjectionSnapshot()` (channel 0 only,
  same convention as the strip's dots) follow the exact `copyLayers` pattern.
- **"Dragged" definition**: `! claimed && aHeld > kClaimClearFloor` — pulled a layer that
  already held audible content. A **fresh claim is drag=0 for exactly one hop**: the instant
  a layer is claimed, its `binLoc` snaps to the knob position, so on the *next* hop that same
  layer is now the nearest one and gets dragged instead of re-claimed — indistinguishable at
  the engine level from "continuing to record," which is accurate (see the U2 test's long
  comment in `test_main.cpp` for the full reasoning, and the `setOrder()`-defers-to-next-hop
  gotcha it had to work around to test hop 1 in isolation).
- **GUI**: `SpectrumDisplay::timerCallback` fetches into `injLoc/injStrength/injDrag`, then
  applies a slow-release visual smoothing (`smoothInj[k] = max(strength[k], smoothInj[k] *
  0.85)` at 30 Hz, ~0.5s to fade) so a brief event stays readable instead of flickering for
  one frame; `loc`/`drag` are only re-latched when `strength > 0` that frame, so a decaying
  tick still shows where/what it was. `paint()` draws a short vertical tick per bin with
  `smoothInj > 0`: **aurora blue** (the house accent, `fromHSV(0.52, 0.55, 1, 1)`) for a
  clean record, **red-orange** (`fromHSV(0.05, 0.80, 1, 1)`, matching the brush's cut colour)
  for a drag/steal, alpha `sqrt(smoothInj)`.

## Tuning knobs (all in SpectrumDisplay.cpp)
- Frequency range: `kMinHz`, `kMaxHz`.
- dB window: the `(db + 80)/80` mapping in `levelToBright`.
- Glow shape: the gradient stop alphas/positions.
- Tint: the `hue`/`sat` lines. Keep saturation low to preserve the "very slightly" intent.
- Smoothing: the `0.6` / `0.15` coefficients in `timerCallback`.
- Location strip: `kLocStripH` (px), the per-dot `alpha = sqrt(mag)` scale, the per-layer hue
  spread; injection ticks: the `0.85` smoothing decay, the record/steal hues.

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
  everything uniformly bigger/smaller — it does **not** create more logical layout room at
  any window size (the actual fix for a cramped row was giving it a tab of its own — see
  the Alter tab, formerly "Perform", below).
- **Mouse coordinates just work.** JUCE maps `MouseEvent` positions through a component's
  `AffineTransform` automatically, so `SpectrumDisplay`'s brush math (`xToFreq`/`yToStrength`,
  driven by its own `getWidth()`/`getHeight()`, fixed at content's logical size) needed zero
  changes — confirmed by the existing DSP tests still passing (no GUI test harness exists to
  drive real mouse events here; verified by reasoning through JUCE's transform semantics and
  by the fact that `display`'s own bounds don't change no matter the window's actual size).
- **Persistence**: `editorScale` (GUI-only float, like `liveMode`/`uiTab`/`keepSound`) saved
  in `get/setStateInformation`, restored via `setSize (860*s, 580*s)` in the editor ctor
  using a scale captured *before* `setResizeLimits` (which clamps the 0x0 editor and
  overwrites `editorScale` through `resized()` — see gotchas.md). Range 0.75..2.0, matching
  `setResizeLimits (645, 435, 1720, 1160)` (645/860 = 0.75, 1720/860 = 2.0).
- **Aspect ratio locked**: `getConstrainer()->setFixedAspectRatio (860.0/580.0)` — dragging a
  corner can't produce a non-860:580 window, so `getWidth()/860` and `getHeight()/580` always
  agree; only width is used for the scale factor.
