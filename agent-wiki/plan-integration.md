# PLAN — Integration branch (`exp/integration`)

**Status: NOT implemented — this is the plan.** Create `exp/integration` from `exp/reverb`
and merge `experimental/harmonize` into it, then do the GUI/param reorganization below.
When done, flip this banner, update the wiki pages in §9, and make sure `./build.sh` is
green with the **union** of both branches' tests passing.

## 0. What integrates with what

- **`exp/reverb`** (base) — already contains everything on `main` **plus** the spectral
  shaper (replacing Filter + Compress) **plus** the output plate reverb.
- **`experimental/harmonize`** — coupled-oscillator pitch entrainment (`applyHarmonize`,
  peak detection, energy migration, unison merge, influence overlay). See
  [harmonize.md](harmonize.md) (arrives with the merge).
- **`experimental/creative-filters` — nothing to integrate.** It points at the same commit
  as `main`; its filter work was superseded by the shaper. Ignore it (delete later if the
  user agrees).

A dry-run `git merge` of harmonize into exp/reverb produces conflicts in 8 files
(11 hunks): `SpectralEngine.{h,cpp}`, `PluginProcessor.cpp`, `PluginEditor.{h,cpp}`,
`SpectrumDisplay.cpp`, `agent-wiki/README.md`, `agent-wiki/parameters.md`.
`PluginProcessor.h`, `SpectrumDisplay.h`, `test_main.cpp`, `.gitignore` auto-merge.

## 1. The one semantic trap in the merge (read this twice)

Harmonize was written against the **pre-shaper** engine: `processFrame` had a standalone
*compress block* after `drainBrush()`, and `applyHarmonize (p)` was inserted right after
it (before the per-hop scalars / spectral-update loop, so the shifted `omega` is used the
same frame).

On the shaper branch that structure is gone: the compress block was removed and the shaper
is applied **inside the per-bin spectral-update loop** (plus a small precompute block
`shaperActive`/mean-pivot right after `drainBrush()`).

**Correct resolution:** `applyHarmonize (p)` is a *state edit* (rewrites `S`, `omega`,
`prevPhase` — same category as `drainBrush`). Insert it **immediately after
`drainBrush()` and before the shaper precompute block**:

```
analysis → drainBrush() → applyHarmonize(p) → shaper precompute (mean/pivot)
        → per-hop scalars → per-bin loop (shaper L[k], omega tracking, phasor, gOut)
        → GUI snapshot → synthesis
```

Rationale: harmonize's energy migration moves whole `S` packets across bins; the shaper's
Level-shape mean must be computed on the post-migration spectrum or the two fight. Putting
harmonize *after* the phasor update instead would apply its `omega` shift one frame late —
it compiles, tests stay green-ish, and it's wrong. Don't.

Other merge resolutions (mechanical):
- `SpectralEngine::Params` = union of both branches **minus `compress`** (gone for good).
  Harmonize side: `harmonize, harmWidth, harmonic`. Shaper side: the 7 `shape*` fields.
- `kCompressRate` (harmonize side, top of SpectralEngine.cpp) is dead — drop it. Keep all
  harmonize constants (`kEntRate`, `kHarmRate`, `kHarmStep`, `kPeakFloor`, `kMaxDen`,
  `kMaxPeaks`, `kPad`) and all shaper constants (`kPermScale`, `kCompFloor`, `kLn4`).
- `PluginProcessor.cpp` `createLayout()`: both branches append params — take both sets,
  in the §3 order. Cache pointers for both.
- `SpectrumDisplay`: keep **both** overlays (shaper gain curve + harmonize influence
  humps/arrows); §5 gates their visibility by active tab.
- `copyPeaks` / peak snapshot machinery: take harmonize's side verbatim.
- `test_main.cpp` auto-merges (both appended); verify the harmonize tests still reference
  valid `Params` fields (they used `compress`-era struct — fix field initializers if so).
- Wiki conflicts: union, then rewrite per §9.

## 2. Mix-mode ("integration") knobs — per-module decisions

The requirement: each sound-alteration topic gets a knob for momentary ↔ integrated into
the held sound — *unless* that's a DSP nightmare or hurts quality. Decisions:

| Module | Decision | Why |
|--------|----------|-----|
| Shaper | **Already has it**: `shapeMode` | This is exactly the momentary↔permanent cross-fade. Nothing to do. |
| Harmonize | **No knob — inherently integrated.** | It *must* rewrite `omega` for tones to converge over time; a "momentary" variant is just a static detune (pointless, sounds worse). Feed > 0 already gives the momentary feel (input tracking self-heals the edit) — see harmonize.md. Document in the info bar + wiki instead of adding a knob. |
| Reverb | **Add `revFeed`** (0..1, default 0): wet reverb fed back into the engines' input | The reverb's "integrated" mode: the tail gets re-captured by the spectral hold and becomes part of the held sound. See §4. This is the module's mix-mode knob; `revMix` stays the output wet/dry. |
| Phase noise | **No knob.** | Already momentary by design (non-accumulating). A permanent variant = random-walk detune of `omega` — irreversibly degrades held tones. Rejected on quality grounds. |
| Brush | **No knob** (GUI-only, already permanent by design). | |

No *global* integration knob: shaper and reverb get per-module control, harmonize/noise
have principled fixed behavior. A global knob would just be a second hand on the same
faders.

## 3. Parameter inventory & Push/Maschine 8-groups

Hosts like Push/Maschine page parameters **8 at a time in creation order**, so
`createLayout()` order is the grouping. New order (21 params):

**Page 1 — "Hold" (exactly 8):**
| # | id | notes |
|---|----|----|
| 1 | `feed` | existing |
| 2 | `loss` | existing |
| 3 | `dryWet` | **NEW** — global dry/wet of the whole effect, see §4. Default 1.0 (wet-only = today's behavior). |
| 4 | `output` | existing |
| 5 | `phaseNoise` | existing bool |
| 6 | `harmonize` | from harmonize branch |
| 7 | `harmWidth` | " |
| 8 | `harmonic` | " |

**Page 2 — "Shaper" (7):** `shapeAmt, shapeMode, shape, shapeFreq, shapeWidth,
shapeCount, shapeLevel` — unchanged ids/order.

**Page 3 — "Reverb" (6):** `revMix, revDecay, revSize, revDamp, revPredelay, revFeed`
(one NEW at the end).

Known imperfection, accepted: page 2 has 7 shaper params, so on Push the 8th slot of that
page shows `revMix`. Per the product decision, we don't invent a filler param or drop a
feature to fix this. If a genuinely useful 8th shaper param ever appears, it slots there.

**State compat:** every existing id is unchanged, so sessions from either branch reload;
`dryWet`/`revFeed` didn't exist before and default to today's behavior (1.0 wet /
0 feedback). APVTS lookups are by id, not index — reordering `createLayout()` is safe for
our own state. (Hosts that saved by VST3 param index would mismatch, but JUCE derives
stable param IDs from the id strings — fine.)

## 4. New DSP (small)

### 4.1 `dryWet` — global dry/wet
The engines are wet-only today. A dry path must be **delayed by `fftSize` samples** to
time-align with the engine latency or dry+wet combs.
- Per-channel dry delay ring (preallocate at `1 << kMaxFftOrder`), write input before the
  engines run, read at `fftSize` delay.
- Mix: `out = wet * dryWet + dryDelayed * (1 - dryWet)` — linear, applied **before**
  output gain (chain: engines → dryWet mix → output gain → reverb → limiter).
- On FFT-size change the delay length follows `fftSize` (held state resets anyway — a
  transient glitch there is already expected/documented).
- `dryWet = 1` must be bit-exact today's output (skip the mix entirely, like revMix=0).

### 4.2 `revFeed` — reverb → hold feedback
- In `processBlock`, keep last block's wet L/R (the reverb already produces them).
- Before the engines run: `input[ch][n] += revFeed * kRevFeedScale * lastWet[ch][n]`
  (per-channel, `kRevFeedScale ≈ 0.5` headroom constant, tune by test).
- One-block feedback delay is inherent and fine (it's a reverb tail, nobody times it).
- Feedback only exists when `revMix > 0` (reverb bypass also kills the loop — keeps the
  mix=0 bit-exact-dry guarantee).
- **Stability:** the loop is input → engine (feed·hold) → reverb (decay) → back. Each
  stage is < 1 gain except eternal-hold (`loss=0`) cases, which the limiter already
  bounds at the *output* — but the engine's internal `S` could still grow from re-fed
  energy. Required test (§7): worst case `feed=1, loss=0, revFeed=1, revDecay=1`, loud
  noise burst, 30 s — output stays bounded (limiter) and finite. If `S` blows up
  numerically, clamp `revFeed` range down (e.g. 0..0.5) rather than adding a hidden
  compressor. If it can't be stabilized cheaply, ship without `revFeed` and note it —
  it's the one optional item in this plan.

## 5. GUI — tabs, not a knob wall

Keep: display on top (unchanged size), bottom utility row (FT size, Phase Noise toggle,
Live, Save sound, Brush) as-is.

- **Persistent row** under the display: Feed, Loss, Dry/Wet, Output — always visible
  (the performance knobs).
- **Tab strip**: three buttons — **Shaper | Harmonize | Reverb** — switching one shared
  knob-row area underneath. Implementation: three `juce::TextButton`s (radio group,
  toggle-styled by `SpectralLookAndFeel`) + `setVisible` on three child components each
  holding its knobs. Avoid `juce::TabbedComponent` unless it styles cleanly with the LNF
  — hand-rolled is less fighting.
  - Shaper tab: Amount, Mode, Shape, Freq, Width, Count, Level (7 knobs).
  - Harmonize tab: Harmonize, Width, Harmonic (3 knobs).
  - Reverb tab: Mix, Decay, Size, Damp, Predelay, Feed (6 knobs).
- **Overlay gating:** the display's shaper-curve overlay draws only when the Shaper tab is
  active; the harmonize influence overlay only on the Harmonize tab. (Both overlays exist
  after the merge and would clutter together.) Editor tells the display the active tab;
  audio behavior is *not* affected by tabs — they're view-only.
- Active tab is GUI-only state; persist it in the state tree like `fftOrder` (nice to
  have, one line each in get/set).

### Info bar (Ableton-style)
A one-line `juce::Label` at the very bottom (~20 px). Every control registers a
description; a shared `juce::MouseListener` (mouseEnter/mouseExit on each slider/button/
combo) sets/clears the label text. Extend the editor's `Knob` struct with the description
and wire it in `setupKnob`. Texts:

| control | info text |
|---------|-----------|
| Feed | How much live input is injected into the held spectrum each hop. |
| Loss | How fast held tones decay. 0 = hold forever. |
| Dry/Wet | Balance of untouched input vs the spectral hold output. |
| Output | Output level, before the safety limiter. |
| Amount | Shaper depth: scales the whole curve. |
| Mode | Shaper: momentary (out-only, reversible) vs permanent (etched into the held sound). |
| Shape | Morphs the curve: Level, Sigmoid, Spikes, Harmonics, Sine. |
| Freq | Shaper curve centre frequency. |
| Width | Shaper curve width / steepness / spacing (per shape). |
| Count | Shaper extent / repetitions across the spectrum (per shape). |
| Level | Shaper strength, signed. 0 = off; negative inverts (cut/pass per shape). |
| Harmonize | Held tones pull each other's pitch until they drift together. Permanent while Feed is low; live input re-tunes it back. |
| Width (harm) | How far apart (in octaves) tones still influence each other. |
| Harmonic | Blend: 0 = tones average together, 1 = tones snap to simple harmonic ratios. |
| Mix (rev) | Reverb wet/dry on the output. 0 = reverb fully off. |
| Decay | Reverb tail length. |
| Size | Reverb room size. Moving it live bends the tail's pitch. |
| Damp | Darkens the reverb tail. |
| Predelay | Gap before the reverb starts. |
| Feed (rev) | Feeds the reverb tail back into the hold — the space becomes part of the held sound. |
| Phase Noise | Adds shimmer by jittering each tone's phase. Never detunes permanently. |
| FT Size | Spectral resolution vs time response. Changing it resets the held sound. |
| Live | Report zero latency to the host (for live playing; disables delay compensation). |
| Save sound | Include the currently held sound in the saved preset. |
| Brush | Drag on the display to boost/cut held tones. This sets the brush width. |

## 6. Code changes by file (beyond the merge)

1. `PluginProcessor.{h,cpp}` — `dryWet` + `revFeed` params (§3 order!), dry delay rings,
   feedback wiring (§4), cached pointers.
2. `SpectralEngine.{h,cpp}` — nothing beyond the merge resolution (§1).
3. `PluginEditor.{h,cpp}` — the §5 restructure (persistent row, tab strip, three tab
   panels, info bar, descriptions).
4. `SpectrumDisplay.{h,cpp}` — `setActiveOverlay(enum)` from the editor; gate the two
   overlays.
5. `test_main.cpp` — union survives + §7 additions.
6. Wiki — §9.

## 7. Tests

All existing tests from both branches must pass unmodified (except mechanical `Params`
field fixes). New:
1. **dryWet alignment**: impulse train in, `dryWet=0` → output is the input delayed by
   exactly `fftSize` samples (cross-correlate to find the lag). `dryWet=1` → bit-exact
   equal to a run without the dry path.
2. **Everything-on stability** (in==out aliasing path, house rule): feed=1, loss=0.1,
   shaper active (permanent), harmonize=0.05/harmonic=1, phase noise on — 30 s noise →
   finite, bounded.
3. **revFeed integrates the tail** (processor-level logic is thin; test at whatever level
   is practical): with revFeed>0, after input stops, held energy is higher than with
   revFeed=0 (the tail got re-captured).
4. **revFeed worst-case stability** (§4.2): feed=1, loss=0, revFeed=max, revDecay=1 —
   bounded, finite, 30 s.

## 8. Implementation order (one step = one buildable state)

1. `git checkout -b exp/integration exp/reverb && git merge experimental/harmonize` —
   resolve per §1. `./build.sh` green with the union of tests. **Commit the merge before
   touching anything else.**
2. Param reorder + `dryWet` (+ test 1). Build green.
3. `revFeed` (+ tests 3, 4). Build green. (Droppable if unstable — see §4.2.)
4. Editor restructure: tabs + persistent row (no info bar yet). Build, launch Standalone,
   click all three tabs.
5. Info bar + descriptions + overlay gating + tab persistence.
6. Wiki pass (§9), flip this banner.

## 9. Wiki updates
- `parameters.md` — full rewrite of the table: 21 params, page grouping, tab layout.
- `architecture.md` — signal flow gains dryWet mix + revFeed loop; file list unchanged.
- `gui-display.md` — tabs, info bar, overlay gating.
- `gotchas.md` — revFeed stability envelope; harmonize-has-no-mode-knob rationale;
  dryWet delay alignment (don't "fix" the dry delay away).
- `harmonize.md` — update the "called after compress" sentence to the §1 call site.
- `README.md` — index this plan.

## 10. Difficulty assessment & model recommendation

Measured surface: 8 conflicted files / 11 hunks, of which `SpectralEngine.cpp` is a
*semantic* conflict (two branches restructured the same function differently — §1);
~800–1200 lines touched overall; 2 small new DSP features, one with a feedback-stability
question; a moderate editor rewrite; ~19 existing tests to keep green plus 4 new.

The risky 25% is step 1: resolving `processFrame` requires understanding *both* rewrites
and choosing an ordering whose failure mode (harmonize applied one frame late, or shaper
mean computed pre-migration) **compiles and passes most tests while being subtly wrong**.
Steps 2–6 are mechanical-to-moderate.

**Recommendation: run steps 1–3 on Opus 4.8 (or Fable 5), then steps 4–6 on Sonnet 5**
if splitting; if one model does everything, use **Opus 4.8**. Sonnet 5 alone is likely to
land the GUI flawlessly but is a real risk on the §1 merge and the §4.2 stability
reasoning — this plan pins both decisions precisely to compensate, but verification of
"did the merge preserve both features' behavior" still needs the stronger model's
judgement.
