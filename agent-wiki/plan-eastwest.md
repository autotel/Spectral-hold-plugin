# PLAN — East↔West location knob (`exp/eastwest`)

**Status: implemented** on `exp/eastwest` (off `exp/integration`). Save sound removed first
(§7). The engine holds 16 location slots; `processFrame` runs active-slot update → other
occupied slots free-run → presence-weighted blend. `./build.sh` green, 30/30 tests (5 new
E–W cases). One realisation note vs the plan: injection is **nearest-slot** (round to the
grid), not split across two — this keeps the crossfade constant-level (splitting halved it)
and makes "active slot" a single unambiguous index. Current behaviour is documented in
[dsp-design.md](dsp-design.md) / [parameters.md](parameters.md) / [gotchas.md](gotchas.md).

Scope is **only** the E–W knob. The other items on the user's fix list (output limiter
module, reverb "metal" knob, shape-name display, knob renames, *Save sound* removal,
macro-page reordering) are separate passes — *Save sound* removal is assumed to land
first or alongside (see §7 note), because it removes the only thing that would have had to
serialize 16 buffers.

## 1. What the feature is

A single knob **`ewLocation`** (0 = East, 1 = West) that turns the held sound from a 1-D
spectrum into a **2-D field `[frequency, location]`**. The held state becomes **16
location slots**, each a full spectral state. The knob does two things at once:

- **Records** (while Feed > 0): input is injected into the slot(s) at the knob's location.
- **Plays**: the audible output is a location-weighted blend of the slots, peaking at the
  knob's position and blending toward neighbours.

Turn the knob to place deposits along the E–W line, then sweep it to morph between them.
If the knob never moves it feeds a single slot → behaves exactly like today's single held
buffer (backward-compatible default, `ewLocation = 0.5`).

## 2. The location→gain model (the heart of it)

16 slots at fixed grid positions `Lₛ = s/15`, `s = 0..15`. Each slot `s` has a **presence**
`pₛ ∈ [0,1]` derived from its current overall level (so an emptying slot smoothly stops
acting as a spatial anchor — this is the "silent slot extends its neighbours' crossfade"
requirement):

```
levelₛ   = slot RMS (or max |Sₛ[k]|), tracked incrementally in the update loop
pₛ       = smoothstep(kPresLo · levelMax, kPresHi · levelMax, levelₛ)   // 0 when silent
```

At knob position `L`, each slot's spatial gain is **presence-weighted inverse-distance**
(Shepard), normalised:

```
φ(d)   = 1 / (|d|^kLocP + kLocEps)
Aₛ(L)  = pₛ · φ(L − Lₛ)  /  Σⱼ pⱼ · φ(L − Lⱼ)          (0 if Σ ≈ 0)
```

Output spectrum (one IFFT of the blend): `X[k] = Σₛ Aₛ(L) · Sₛ[k]`.

This satisfies every case the user described, **including the graded refinement**:

- **Only East deposited** → only slot 0 has `p>0` → `A₀ = 1` for all `L` → holds full,
  no fade moving West. ✓
- **East + West** → between them it's a crossfade (near-slots dominate). ✓
- **East + Middle + West** → East fades to 0 at Middle, Middle fades to 0 toward West
  (nearest *present* slot is the fade endpoint). ✓
- **A slot decaying toward silence** → `pₛ → 0` smoothly → its term vanishes and the
  normalisation hands its weight to the neighbours, which bleed in to fill the gap (no
  silent hole, no pop). ✓ "tent slopes proportional to level; an almost-silent slot lets
  the two next slots mix in." ✓
- **Beyond the outermost present slot** → that slot is the nearest present one → `A ≈ 1`
  → holds full out to the edge. ✓

`kLocP` (≈ 2) sets locality: higher = crisper nearest-neighbour crossfade, lower = broader
blend. `kLocEps` avoids div-by-zero at `L = Lₛ`. `kPresLo/Hi` are the silence→anchor
fade thresholds (fractions of the loudest slot). All tuning constants at the top of
`SpectralEngine.cpp`.

Cost: the 16 scalar weights `Aₛ` are computed **once per hop**, then applied per bin in the
mix. Negligible.

## 3. Recording / injection

`activeLo = floor(L·15)`, `frac = L·15 − activeLo`. Inject into slot `activeLo` with weight
`(1−frac)` and slot `activeLo+1` with weight `frac` (single slot at the ends). These are the
**active slots**. Only active slots receive:
- the fed input `feed · xs · comp · injWeight`,
- the instantaneous-frequency **tracking** (`omega` retune — input only informs where it's
  deposited),
- **permanent edits** (see §4).

Every **occupied** slot (level above a floor) still gets the per-hop **phasor advance +
loss decay** so held tones stay free-running and continuous even while inaudible. Empty
slots are skipped entirely (cost scales with slots actually used, not always 16).

## 4. Interaction with the existing sculpting (decided)

**Permanent, state-editing operations act on the active slot(s) at the knob location** —
"edit where you're pointing" (user's decision):
- Shaper **permanent** mode (`shapeMode>0`): the `S *= exp(...)` multiply runs only on
  active slots.
- **Harmonize**: `applyHarmonize` runs on each active slot's `S`/`omega` (couples tones
  within the slot you're pointing at). Signature gains a slot index.
- **Brush**: `drainBrush` edits the active slot(s) (matches the display, which shows `L`).

**Momentary, output-only operations act on the blended output**, after the mix:
- Shaper **momentary** gain (`gOut`) and its mean-pivot are computed on the combined
  `X[k]`, applied once post-mix. This is "what you hear," and keeps it a single pass.

This split also keeps the expensive sculpting (harmonize peak search, permanent shaper)
running on **1–2 slots**, not 16.

## 5. Engine restructure (`SpectralEngine.{h,cpp}`)

Promote the three per-bin state arrays to **per-slot**, flat and preallocated at max size
(`kNumSlots · maxBins`, indexed `s·numBins + k`) so `setOrder` still never allocates:
- `S`, `omega`, `prevPhase`  →  `Sslot`, `omegaSlot`, `prevPhaseSlot` (flat, 16×).
- **Shared, unchanged** (depend only on `k`, not slot): `expectedAdv`, `Xs` (per-hop input
  spectrum), `fftData`, `dispScratch/dispMag/dispPhase` (they hold the *output* blend),
  the window, the rings.

Memory at max FFT (order 13, numBins 4097): 16 slots × (S 8B + omega 4B + prevPhase 4B) ×
4097 ≈ **1.05 MB/engine**, ×2 engines ≈ 2.1 MB. Fine.

Rework `processFrame` into three phases (was one interleaved loop):
1. **Per-slot update** (loop occupied slots): for each slot, per bin — phasor advance,
   loss decay, and *for active slots only* input injection + freq tracking + permanent
   shaper + (once per slot) harmonize/brush. Accumulate `levelₛ` incrementally here.
2. **Blend**: compute the 16 `Aₛ(L)` weights (§2), then per bin
   `X[k] = Σₛ Aₛ·Sslot[s·numBins+k]` → `fftData`.
3. **Momentary shaper + display snapshot** on `X[k]`, then the existing IFFT + overlap-add.

`reset()` clears all slots. FFT-size change resets all slots (already the contract).

New public surface: `void setLocation(float ewLocation)` isn't needed — pass `ewLocation`
through `Params` like the other knob values (simplest, matches feed/loss). Add
`float ewLocation = 0.5f;` to `SpectralEngine::Params`.

## 6. Parameter & editor

- New APVTS param **`ewLocation`**, range 0..1, default **0.5**, inserted in
  `createLayout()` **right after `loss`** (user's placement). Cache the pointer; set
  `p.ewLocation` in `processBlock`. Both channel engines get the same value (stereo image
  preserved).
- Editor: one `Knob ew` in the **persistent row**, labelled "E↔W" (or "Location"), between
  Loss and Dry/Wet. Info-bar text: "Position along the East–West field: records held tones
  here, and blends between deposited locations." (Macro-page 8-grouping is a later pass —
  don't reorder the other pages here.)
- No new display overlay required; the spectrum view already shows the blended output at
  `L`. (A future E–W strip visualiser is possible but out of scope.)

Workflow note for the manual/tooltips: because one knob both records and plays, to audition
locations *without* overwriting them, set Feed = 0 then sweep. With Feed > 0 you paint.

## 7. State / persistence

Assumes **Save sound is removed** (separate fix-list item). With it gone, nothing
serialises the held state, so the 16 slots need no (de)serialisation — big simplification.
If Save-sound removal has *not* landed when this starts, do it first on this branch, or the
`writeAudioState/readAudioState` path must be extended to loop slots (avoid — just remove
the feature per the user's request).

## 8. Tests (offline, `SpectralHoldTest`)

Engine-level, no host needed. Add:
1. **Single-location == legacy feel**: knob fixed at 0.5, feed a tone, stop, hold — output
   sustains exactly like the pre-E–W single-buffer test (port the "sine feed" assertions).
2. **Two deposits crossfade**: deposit tone A at `L=0` (feed while ew=0), tone B at `L=1`.
   Then feed=0. Sweep ew 0→1: at 0 only A present, at 1 only B, at 0.5 both ~equal. Assert
   A's bin magnitude falls monotonically and B's rises across the sweep.
3. **Silent slot bridges (the graded refinement)**: deposit at `L=0` and `L=1` only. Set
   ew=0.5 → both audible (nothing at 0.5 to block). Then also deposit at 0.5; let it decay
   (high loss, feed=0) → as the middle empties, output at ew=0.5 returns to the A+B blend
   with no silent dip (sample RMS never drops below a floor across the decay). 
4. **Hold-full beyond extreme**: deposit only at `L=0`; sweep ew 0→1 → the tone's level is
   ~constant (no fade), finite.
5. **Permanent edit hits the active slot only**: deposit A at `L=0`, B at `L=1`; with ew=0
   apply a permanent shaper cut; assert A (at L=0) is reshaped and B (at L=1) is untouched.
6. **Everything-on stability** (in==out aliasing, house rule): all 16 slots deposited,
   shaper permanent + harmonize + phase noise on, sweep ew for 30 s of noise → finite,
   bounded.

## 9. Implementation order (one buildable state each)

1. Engine: per-slot arrays + three-phase `processFrame`, `ewLocation` in `Params`, the §2
   blend and §3 injection. Legacy behaviour when knob is parked (test 1). `./build.sh`
   green. **Commit.**
2. Route permanent edits / harmonize / brush to active slots (§4). Tests 2–5.
3. Param + editor knob + info text.
4. Test 6 + wiki pass (§ below) + flip this banner.

## 10. Wiki updates
- `dsp-design.md` — new "East–West location field" section: the 2-D state, the §2 gain
  model, the three-phase frame.
- `architecture.md` — note the engine now holds 16 slots; signal flow unchanged downstream.
- `parameters.md` — add `ewLocation` row; note it's the persistent-row knob after Loss.
- `gotchas.md` — the presence-weighted blend (don't "simplify" to binary occupancy — it
  reintroduces pops and silent gaps); permanent-edits-hit-active-slot; one-knob
  record+play duality (feed=0 to audition).
- `gui-display.md` — the view shows the blended output at `L` (no new overlay).
- `README.md` — index this plan.

## 11. Difficulty & model recommendation

**Medium–high**, concentrated entirely in the `SpectralEngine` refactor. Promoting three
hot-loop arrays to per-slot and splitting the interleaved `processFrame` into
update→blend→shape is a real restructure of the most performance- and correctness-sensitive
file, and it interacts with harmonize/brush/shaper routing (§4) and the display snapshot.
The gain model itself (§2) is simple once written but must be got right (continuity, the
normalisation edge cases). The knob/param/editor work (steps 3) is trivial.

**Recommendation:** do steps 1–2 (the engine) on **Opus 4.8** or **Fable 5** — the
restructure has the same "compiles and mostly passes while subtly wrong" risk profile as
the integration merge (e.g. tracking or permanent edits leaking to the wrong slot, or a
blend discontinuity that only shows as a click). Steps 3–4 are fine on **Sonnet 5**.
Single-model run: **Opus 4.8**.
