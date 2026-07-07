# PLAN — Spectral Shaper (branch `exp/spectral-shaper`)

**Status: phase 1 AND phase 2 implemented** (engine, processor, editor, display overlay,
tests, wiki all updated on this branch; `./build.sh` green, 11/11 tests pass).
Kept as design-rationale reference — the
live behavior lives in `Source/ShapeCurves.h` / `SpectralEngine.cpp`; see
[dsp-design.md](dsp-design.md#the-spectral-shaper) and [parameters.md](parameters.md)
for the current, maintained description.

**Later update (exp/integration, plan-fixes.md §2/§11):** Sigmoid/Spikes/Harmonics were
made **bipolar / zero-mean** (were cut-only here) so a permanent edit is reversible in
principle, with a boost self-limit so compounding a boost stays bounded; the Level shape's
Width/Count meanings were swapped (Width = extent, Count = warp — more intuitive). The
math below describes the **original, cut-only** version; see dsp-design.md for what
actually runs now.

## 1. What the feature is

A per-bin amplitude shaper driven by math curves over the (log-frequency) spectrum.
Every shape produces a **normalized signed change** `L[k] ∈ [-1..+1]` per bin
(+1 = max boost, 0 = no change, -1 = full cut). One shared parameter set drives all
shapes; a Shape knob cross-fades between them.

It subsumes the two features it replaces:
- **Filter** (gaussian bell + tone) → the *Sine* shape at `level=-1`, low `count`
  is a single bell cut — same musical function.
- **Compress** → the *Level* shape at default `width`/`count` in *permanent* mode is
  **bit-for-bit the same math** as the old compress (see §3.1 and the equivalence test).

## 2. Parameters (APVTS)

Remove: `filterAmt`, `filterTone`, `compress` (and their knobs, and the `Params` fields).
Also remove the leftover `attack` field from `SpectralEngine::Params` if still present
(the parameter was already removed).

Add (prefix `shape`):

| id | GUI | Range | Default | Meaning |
|----|-----|-------|---------|---------|
| `shapeAmt`   | Amount | 0..1 | 1.0 | Global depth: scales `L[k]` before application. |
| `shapeMode`  | Mode   | 0..1 | 0.0 | Momentary ↔ permanent cross-fade. 0 = output-only shaping (non-destructive, like the old filter). 1 = fed into the held state `S` each frame (destructive, compounding, like compress/brush). Continuous: momentary weight `1-m`, permanent weight `m`. |
| `shape`      | Shape  | 0..3 | 0.0 | Cross-fades Level(0) → Sigmoid(1) → Spikes(2) → Sine(3). Linear blend of the two adjacent shapes' `L[k]`. |
| `shapeFreq`  | Freq   | 20..20000 Hz, log skew 0.25 | 1000 | Position on the spectrum (meaning per shape, §3). |
| `shapeWidth` | Width  | 0..1 | 0.5 | Width/steepness/spacing (per shape). 0.5 is the "neutral" value where the Level shape is 1:1 with the old compress. |
| `shapeCount` | Count  | 0..1 | 1.0 | Extent/repetition (per shape). 1.0 = covers the whole spectrum (needed so the Level shape defaults to old-compress behavior). |
| `shapeLevel` | Level  | -1..+1 | 0.0 | Signed strength. 0 = no effect for **every** shape (global bypass by default). |

Notation below: `x = log2(freqHz)` per bin (bin 0 excluded, as today), `x0 = log2(shapeFreq)`.

## 3. The four shapes — exact math

All shapes output `L[k] ∈ [-1..+1]`. Clamp after computing.

### 3.1 Level shape (index 0) — replaces Compress
Signal-dependent: reshapes each bin's level relative to the mean of the active bins.
Reuse the existing compress pivot code verbatim (maxMag → active threshold `maxMag·1e-3`
→ mean of active bins; inactive bins get `L=0`).

```
ratio = clamp(|S[k]| / mean, 0.01, 100)        // as today
lr    = ln(ratio)                              // ∈ [-4.6 .. +4.6]
u     = lr / 4.6                               // normalize to [-1..1]
γ     = 2^((width - 0.5) * 2)                  // width warp: 0.5 → γ=1 (1:1)
L[k]  = level * sign(u) * |u|^γ * win[k]
```
- **Width**: γ<1 (width low) pushes effect toward bins *near* the mean; γ>1 concentrates
  it on the extremes (peaks/troughs). At `width=0.5` it is exactly linear in `lr` — the
  1:1 value required for compress equivalence.
- **Count** = spectral window `win[k]`: gaussian in log-freq centred at `shapeFreq`,
  `win[k] = exp(-(x-x0)²/(2σ(c)²))` with `σ(c) = 0.3 · 2^(6c)` octaves, then
  `win = lerp(win, 1, smoothstep(0.8, 1.0, c))` so `count=1` is **exactly** flat (1:1
  everywhere → old compress). Low count = compress only around Freq.
- **Freq**: centre of that window. (This resolves the "freq has no effect" question:
  it has no effect at `count=1`, and localizes the effect below that.)

**Compress equivalence (must hold, tested):** with `width=0.5, count=1, mode=1 (permanent),
amount=1`, the per-frame factor applied in §4 is `exp(level·(lr/4.6)·kPermScale)` with
`kPermScale = 0.23`, which equals `exp(level·lr·0.05) = ratio^(level·0.05)` — identical to
the old `ratio^(compress·kCompressRate)`, `kCompressRate = 0.05`. Do not change 0.23 or
4.6 independently; they are coupled (`0.23 = 4.6 · 0.05`).

### 3.2 Sigmoid shape (index 1) — LP/HP shelf
```
wOct = 0.1 · 2^(width · 5.64)                  // ramp width 0.1 .. 5 octaves
s    = tanh((x - x0) / wOct)
L[k] = level > 0 ? level * (s - 1) / 2         // level>0: cut below x0 → HP
                 : level * (s + 1) / 2         // level<0: cut above x0 → LP
```
Cut-only (L ≤ 0), continuous through `level=0` (flat, zero change — as specified).
- **Freq**: slope position. **Width**: ramp width. **Count**: does nothing in v1.
  *Idea (noted, not implemented): count could repeat the sigmoid into a staircase of
  shelves across the spectrum.*

### 3.3 Spikes shape (index 2)
Gaussian spikes at `x_n = x0 + n·d`, `n = 0, ±1, ±2, …`:
```
d       = 0.2 · 2^(width · 4.6)                // spacing 0.2 .. 5 octaves
σ       = 0.25 · d                             // spikes stay distinct
nMax    = count * 40                           // 40 side-pairs ≈ covers any audio band
a_n     = clamp(nMax - |n| + 1, 0, 1)          // outer pairs fade in smoothly
L[k]    = level * clamp( Σ_n a_n · exp(-(x - x_n)²/(2σ²)), 0, 1 )
```
The fractional-amplitude fade of the outermost pair is the "smooth transition between
n spikes". `count` at min = 1 spike at Freq; at max = spikes everywhere.
Implementation note: don't loop all n per bin — for each bin only `n = round((x-x0)/d)`
and its two neighbours contribute meaningfully.

### 3.4 Sine shape (index 3) — replaces Filter
Cosine so the extremum sits **at** `shapeFreq` (a single negative lobe = bell cut = the
old filter):
```
ρ      = 0.25 · 2^(width · 4.6)                // spatial freq, 0.25 .. 5 cycles/octave
env[k] = gaussian window like §3.1: σe(c) = (0.5/ρ) · 2^(7c), lerp→1 near c=1
L[k]   = level * cos(2π · ρ · (x - x0)) * env[k]
```
- **Freq**: pattern centre — moving it shifts the phase of the pattern across the
  spectrum (the spec's "freq = phase", expressed as position so it matches the other shapes).
- **Width**: sine frequency. **Count**: from ~1 cycle at the centre (env ≈ one lobe) to
  full-spectrum repetition. **Level** -1..+1 flips cut/boost.
- Old-filter recipe: `shape=3, level=-1, count≈0, width≈0.35` (σ ≈ 1.25 oct lobe).

## 4. Applying L[k] — momentary vs permanent

Per frame, after the shape cross-fade, let `t[k] = shapeAmt · L[k]`, `m = shapeMode`.

**Permanent** (where compress ran, before the phasor update):
```
S[k] *= exp(t[k] · m · kPermScale)             // kPermScale = 0.23 (see §3.1)
```
Gentle per frame, compounds over time — same character as compress/brush.

**Momentary** (where the filter's `gFilt` multiplied the output):
```
tm      = t[k] · (1 - m)
gOut[k] = tm >= 0 ? exp(tm · ln 4)             // up to +12 dB boost
                  : (1 + tm)²                  // smooth to 0 (full cut) at tm = -1
out     = S[k] · gOut[k]
```
Non-destructive: turning Amount/Level down restores the held sound intact.

**Input compensation (keep the old in/out invariant for cuts):** injection is multiplied by
`comp[k] = 1 / max(kCompFloor, min(1, gOut[k]))` — fed tones pass unaffected through
momentary *cuts* (exactly the old filter behavior); boosts are **not** compensated
(don't attenuate live input). `kCompFloor = 0.05` stays.

Order inside `processFrame`: compute mean/pivot (needs current `|S|`) → compute `L[k]`
per bin (cross-faded) → apply permanent factor → existing phasor update → apply `gOut`
at the output write + display snapshot (snapshot the shaped output, as today).

Cost: a few transcendentals per bin per hop — same order as the old compress loop. Fine.

## 5. Code changes by file

1. **`Source/ShapeCurves.h` (new, header-only, `juce_dsp`-only)** — pure static functions:
   `shapeL(shapeIndex blend, x, params…, ratioForLevelShape)` and the per-shape helpers.
   Shared by engine, GUI overlay, and tests (same pattern as `filterGain` today — the
   display must never reimplement the math).
2. **`SpectralEngine.{h,cpp}`** — `Params`: drop `filterAmt/filterTone/compress/attack`,
   add `shapeAmt, shapeMode, shape, shapeFreq, shapeWidth, shapeCount, shapeLevel`.
   Remove `filterGain` + compress block; wire §4 into `processFrame`. Keep `kCompFloor`.
3. **`PluginProcessor.{h,cpp}`** — `createLayout()`: remove old ids, add the 7 new ones
   (ranges/defaults per §2, `shapeFreq` log-skewed like `filterTone` was). Pass through
   to `Params`.
4. **`PluginEditor.{h,cpp}`** — remove Compress/Filter/Tone knobs; add the 7 new knobs.
   10 rotaries total → two rows: **Feed, Loss, Output** / **Amount, Mode, Shape, Freq,
   Width, Count, Level** (keep existing knob style).
5. **`SpectrumDisplay.{h,cpp}`** — replace the filter-curve overlay: draw the momentary
   gain curve `gOut(freq)` (same amber stroke, same log-x) whenever `shapeAmt·|shapeLevel| > 0`.
   For the Level shape use the display-snapshot magnitudes to compute `ratio` (approximate
   is fine — it's a preview). Read params live from the APVTS as today.
6. **`test_main.cpp`** — see §6.
7. **Wiki** — update `parameters.md`, `dsp-design.md`, `gui-display.md`, `gotchas.md`
   (the "no separate output filter" note is already stale and now wrong twice — fix it),
   and remove this plan's "not yet implemented" banner when done.

State compat: old sessions' `filterAmt/filterTone/compress` values are silently dropped
(unknown APVTS ids). Acceptable on an experimental branch.

## 6. Tests (offline, `SpectralHoldTest`)

1. **Compress equivalence**: two tones, `shape=0, width=0.5, count=1, mode=1, amount=1`,
   sweep `level ±` → magnitude ratio widens/narrows exactly like the old `compress:` test
   (port that test's assertions).
2. **Sine-as-filter**: tone at `shapeFreq`, `shape=3, level=-1, count≈0, mode=0` → output
   strongly attenuated; then set `amount=0` → held tone restored (non-destructive).
3. **Sigmoid HP/LP**: low+high tones, `shape=1, mode=0`; `level=+1` cuts the low tone
   only, `level=-1` cuts the high tone only; `level=0` changes nothing.
4. **Permanent is destructive**: same as (2) but `mode=1` for a while → turning amount
   to 0 does *not* restore (levels stay reshaped).
5. **in==out aliasing path** with the shaper active (house rule — see gotchas).

## 7. Suggested implementation order (one step = one buildable state)

1. `ShapeCurves.h` + unit-test the pure math (no engine changes yet).
2. Engine: params + §4 wiring, old code out. `./build.sh` green with tests 1–5.
3. Processor layout + editor knobs.
4. Display overlay.
5. Wiki pass + this file's status flip.

## Phase 2 — Harmonics shape (IMPLEMENTED)

**Status: implemented** as planned below — `ShapeCurves::harmonicsShape`, index 3
(Sine moved to 4), `shape` APVTS range 0..4, new test "shaper harmonics subtractive"
in `test_main.cpp`. A fifth shape: a comb of **overtones and
undertones around the centre frequency** — spikes at `n·f0` (overtones) and `f0/n`
(undertones), `n = 1, 2, 3, …`, with the fundamental at `shapeFreq` shared by both series.

### Placement
Insert at **index 3, between Spikes and Sine** (equal-spaced comb → harmonic comb is the
musical morph). Sine moves to index 4. So: Level(0), Sigmoid(1), Spikes(2),
**Harmonics(3)**, Sine(4).

### Math (`ShapeCurves::harmonicsShape (x, x0, width, count, level)`)
In log-frequency, harmonic positions are `x0 ± log2(n)` (overtones +, undertones −).
Per bin:
```
D  = |x - x0|                       // octaves from the fundamental, side-symmetric
q  = 2^D                            // = freq/f0 (overtone side) or f0/freq (undertone side)
candidates: n ∈ { max(1, floor(q)), floor(q)+1 }   // the two nearest harmonics on this side
σ0  = 0.03 · 2^(width · 3.5)        // base spike half-width, ~0.03 .. 0.34 octaves
nMax = count * 12                   // harmonics per side at full knob (matches Spikes' slow ramp)

for each candidate n:
    a_n  = clamp(nMax - (n - 1) + 1, 0, 1)   // fundamental (n=1) always full once level≠0;
                                              // higher harmonics fade in smoothly with count
    gap  = log2((n + 1) / n)                  // local spacing on this side, shrinks with n
    σ_n  = min(σ0, 0.3 · gap)                 // keep neighbouring spikes distinct at high n
    dd   = D - log2(n)
    sum += a_n · exp(-dd² / (2·σ_n²))

env = clamp(sum, 0, 1)
L   = level ≥ 0 ? -level · env               // reject the harmonic series (notch comb)
                :  level · (1 - env)          // pass ONLY the harmonic series
```
Same subtractive sign-split as Spikes (`L ≤ 0` always, flat no-op at `level = 0`):
`level < 0` isolates everything harmonically related to `shapeFreq` — very musical;
`level > 0` notches the harmonic series out of the drone.

Knob meanings: **Freq** = fundamental; **Width** = spike width; **Count** = how many
overtone/undertone pairs (0 → fundamental only, 1 → 12 per side); **Level** = reject ↔ pass.

### Code changes
1. **`ShapeCurves.h`** — add `harmonicsShape` (above); `kNumShapes` 4 → 5; in
   `shapeLRaw`: `case 3` → harmonics, `default` (4) → sine. Nothing else — `shapeL`'s
   clamp derives from `kNumShapes`.
2. **`PluginProcessor.cpp`** — `shape` parameter range `0..3` → `0..4` in `createLayout()`.
3. **`SpectralEngine.h`** — update the `shape` field comment (0..4, five shapes).
4. **Editor / display: no changes** — the knob reads its range from the APVTS and the
   overlay calls `shapeL`, so both pick the new shape up automatically.
5. **`test_main.cpp`** — new case: deposit held tones at **1 kHz and 2.5 kHz** (2.5 k is
   not in 1 k's overtone/undertone series), `shape=3, shapeFreq=1000, mode=0, amount=1,
   width=0.5, count=0.3` (covers n≥3). Assert: `level=-1` (pass-only) keeps the 1 kHz bin,
   cuts the 2.5 kHz bin; `level=+1` (reject) cuts 1 kHz, keeps 2.5 kHz. Copy the measure
   pattern from the existing "shaper spikes subtractive" test (run ≥24 blocks before
   `copyDisplay` so the snapshot reflects shaped output — the earlier test failed exactly
   there). Also assert a mid-crossfade value (`shape=2.5`) stays finite.
6. **Wiki** — `parameters.md`: Shape range 0..4, add Harmonics row to the per-shape table;
   `dsp-design.md`: add Harmonics to the shape list; flip this section's status when done.

### Known caveat (accepted)
Changing the `shape` range remaps saved normalized values (a session saved with Sine=3.0
reloads as 0.75·4 = Harmonics side). Fine on this experimental branch; don't add
migration code.

## 8. Resolved design questions (defaults chosen, revisit only if asked)

- Level-shape **Freq** = window centre (inert at `count=1` by design).
- Level-shape **Width** = extremes-vs-middle warp γ (neutral at 0.5).
- Sigmoid **Count** = unused in v1 (staircase idea parked).
- Momentary compensation = cuts only, floored at `kCompFloor`.
- Mode is a continuous cross-fade, not a switch.
- Boost ceiling +12 dB (`ln 4`) — tune by ear later; the limiter bounds the rest.
