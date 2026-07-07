# PLAN — Location layers for the bin engine (`exp/loclayers`)

**Status: NOT implemented. Working plan (written for Sonnet). Experiment — "see where it
goes".** Branch is off `main` (bin engine only; the particle engine lives on
`exp/particles` and is not involved here).

## 0. The problem (why this branch exists)

The E–W v2 model stores **one location per bin** (`binLoc[k]`). Record a sound at East,
then record a sound sharing frequency content at West: both fight over the same bin slot.
The injection merge `loc ← (aHeld·loc + aInj·ewL)/(aHeld+aInj)` parks the merged tone
**between** the two positions, and the complex add mixes the old East phasor with the new
West content (partial phase cancellation). Net effect the user hears: the West recording
"steals" from East and *both* end up poorer — the shared component is audible properly at
neither end. This is the documented structural compromise in dsp-design.md ("two
same-frequency tones cannot coexist at two locations"); this branch removes it.

(The particle engine fixed the same problem with its `kMatchAttFloor` match gate —
`exp/particles` §17. This is the bin-paradigm equivalent.)

## 1. The model — K parallel location layers

Give the engine `kNumLayers = 4` parallel held states. Per layer `l`, per bin `k`:
`S_l[k]`, `omega_l[k]`, `binLoc_l[k]`. A frequency can now be held at up to 4 distinct
locations simultaneously.

**Shared (NOT per layer):** `inRing`/`outRing`, window, FFT object, `expectedAdv`,
`prevPhase` (it tracks the *input's* phase for unwrapping — a property of the analysis,
not of any held state), `Xs`, display buffers, brush queue.

**Playback stays one IFFT.** Mix per bin *before* synthesis:

```
outBin[k] = ( Σ_l  S_l[k] · att_l[k] ) · gOut[k],   att_l[k] = ewAtt(|ewL − binLoc_l[k]|)
```

Attenuation stays absolute/unnormalised per the v2 rule (nothing can jump). CPU cost
grows only in the per-bin update loop (K rotations, K decays, K `exp` for att); the
IFFT/windowing/OLA cost is unchanged. Memory: 4 layers × ~20 B/bin × 4097 bins ≈ 330 KB
per engine — preallocate everything in `prepare()` at max size, RT-safe as usual.

Storage layout: flat vectors of length `kNumLayers * maxBins` with stride `maxBins`
(better cache behaviour than vector-of-vectors, and `applyHarmonize`/`drainBrush` can
take a layer base offset).

## 2. Injection routing (the core of the fix)

Per frame, per bin with meaningful injection (`aInj > kInjLocFloor`, same gate as today):

1. Compute `att_l` for every layer at this bin. Find the best (max att; tie-break: prefer
   a layer already holding energy at this bin, then lowest index — keeps the legacy case
   deterministic, see §5).
2. **Near enough** (`att_best ≥ kClaimAtt`): inject into that layer exactly as today —
   complex add, loc pull `loc ← (aHeld·loc + aInj·ewL)/(aHeld+aInj)`, omega tracking
   update (`trackW = feed`) applied to *that layer's* omega. Dragging a *nearby* tone is
   correct behaviour (it's the same place, being re-recorded).
3. **All layers far** (`att_best < kClaimAtt`): **claim** a layer instead of dragging:
   pick the layer with the smallest `|S_l[k]|` (usually an empty one), and if its residual
   energy is non-trivial (`> kClaimClearFloor`) hard-reset that layer-bin's packet first
   (`S = 0`, `omega = expectedAdv`) so the new tone doesn't phase-mush with stale content.
   Then snap `binLoc_l[k] = ewL` and inject. The old East tone in its own layer is
   **untouched** — that is the whole point.

Self-consistency without extra state: the frame after a claim, the claimed layer's loc
*is* the knob position → `att = 1` → it wins the nearest-layer choice → continuous
recording keeps feeding the same layer. No per-bin "active layer" memory needed.

**Layer exhaustion** (all K layers hold far content at this bin): the claim steals the
quietest layer-bin. Finite-resource compromise, same spirit as the particle pool's
eviction. The stolen tone was far from the listener (att small), so the reset is nearly
inaudible *at the knob position*; at the stolen tone's home position it vanishes —
acceptable, document in gotchas.

Suggested `kClaimAtt = 0.1` (≈ d > 0.53 → claim) — deliberately the same value as the
particle engine's `kMatchAttFloor`, so the two paradigms agree on when "elsewhere" starts.
E↔W full-distance re-record (d = 1, att ≈ 3e-4) claims cleanly; a knob mid-way (d = 0.5,
att ≈ 0.13) still drags — tune by ear later if the boundary feels wrong.

**Frequency tracking gating:** the measured input advance (`dev`/`measured`) is computed
once per bin from the shared `prevPhase` (unchanged), but the `omega += (measured−omega)·
trackW` update goes **only to the layer receiving the injection**. Far layers' omegas
stay frozen — otherwise the West recording would silently retune the East tone's pitch,
which is the same steal in the frequency domain.

## 3. Per-frame update (what changes in `processFrame`)

Order unchanged (analysis → drainBrush → applyHarmonize → shaper precompute → per-bin
loop → snapshot → synthesis). Inside the per-bin loop, per layer:

- **Decay** stays localised per layer: `S_l[k] *= max(kDecayFloor, exp(−lossRate·att_l))`.
- **Phase advance**: each layer rotates by its own `omega_l[k]` (K complex mults/bin).
  Phase Noise jitters each layer's rotation independently (same non-accumulating rule).
- **Permanent shaper**: applied per layer, exponent scaled by that layer's `att_l`
  (identical to today's distance-weighted rule, just per layer). The Level shape's
  `ratio` pivot needs a decision: use the **listener mix** `mixAbs[k] = Σ_l |S_l[k]|·
  att_l[k]` for both the mean and the per-bin ratio — "shape what you hear", consistent
  with edits being listener-relative. (Computing it per layer instead would triple the
  mean-scan cost and edit far layers by their own local balance — wrong semantics.)
- **Momentary shaper / gOut / comp**: unchanged — one gOut per bin applied to the *mixed*
  output bin, comp applied to the injection as today.
- **Brush** (`drainBrush`): loop layers, exponent scaled by each layer's `att_l` — the
  same one-line generalisation as the permanent shaper.

## 4. Harmonize across layers

Run `applyHarmonize` **per layer, independently** (peak detect + drift + unison merge +
migration all confined to one layer's arrays; the function takes a layer offset). Within
a layer everything behaves exactly as today, including `binLoc` merging/migrating with
energy.

Known limitation, accepted for the experiment: tones held in *different* layers do not
entrain against each other even when both are near the listener. Cross-layer coupling
(and cross-layer unison merge for same-loc same-freq tones in different layers) is a
follow-up if layering survives listening. Document in gotchas + this plan.

Cost: K× peak-detect scan (O(numBins) each) + K× small O(P²). Display overlay
(`copyPeaks`): snapshot the union of all layers' peaks, or just the layer nearest the
knob — pick the union, capped at `kMaxPeaks`, weight-sorted; simplest honest view.

## 5. Backward compatibility (must hold exactly)

`reset()` fills all layers' `binLoc = 0` and `S = 0`. With the knob parked at 0
(default): every layer has att = 1, ties broken toward the energy-holding/lowest layer →
**all injection lands in layer 0 forever**, layers 1–3 stay empty (empty layers
contribute 0 to the mix), and layer 0 behaves exactly like today's single state. Every
existing non-E-W test (silence, sine feed, in-place, brush, shaper suite, feed=0,
phase noise, harmonize, migration, unison, peak detection) runs at `ewLocation = 0` and
must pass **unchanged** — they are the regression net for the refactor.

## 6. Tests (offline, `SpectralHoldTest`)

- **The headline replacement**: today's `ew re-record drags location` test asserts the
  drag (W grows, E *shrinks*). Replace it with the coexistence assertion, mirroring the
  particle engine's `pe-ew1`: deposit 1 kHz at E, deposit 1 kHz at W → measure the 1 kHz
  bin from both ends → **both** near full level (E must NOT have shrunk), and sweeping
  the knob gives the two-ended profile. Keep the old test's name out of the way (rename,
  don't silently repurpose).
- **Ported/kept as-is** (should pass with at most threshold nudges): room-walk smooth,
  two-tone smooth pan, localized loss, distance-weighted edit, everything-on sweep
  stability.
- **New — claim boundary**: deposit at E; feed same frequency at knob = 0.35 (att > kClaimAtt
  → drag expected, E tone moves) vs at knob = 1.0 (claim expected, E untouched). Asserts
  the threshold actually separates the two behaviours.
- **New — layer exhaustion**: deposit the same frequency at 5 distinct locations (> K=4)
  → engine survives, no NaN, quietest layer stolen, output bounded.
- **New — no cross-layer frequency steal**: deposit tone at E, then feed a *detuned*
  (+30 Hz) tone at W → E tone's pitch unchanged (omega tracking must not leak across
  layers). Measure via copyPeaks or bin energy distribution.
- **Perf sanity**: everything-on sweep already exists; confirm still comfortably real-time
  (K=4 → roughly 4× the per-bin loop, still far below the IFFT+reverb budget; the earlier
  perf figures leave an order of magnitude of headroom).

## 7. Implementation order (one buildable state each)

1. **Mechanical layering**: flat `K×maxBins` arrays, layer-0-only wiring (inject/decay/
   rotate layer 0, mix loop over layers). All existing tests must pass unchanged. No
   behaviour change yet.
2. **Injection routing** (§2: nearest/claim/steal) + per-layer decay/rotation/tracking.
   New tests: coexistence, claim boundary, no-cross-layer-steal.
3. **Edits per layer**: permanent shaper + brush per-layer att; Level-ratio from the mix.
   Existing shaper/brush tests still green (they run at knob 0 → mix == layer 0).
4. **Harmonize per layer** + copyPeaks union. Harmonize/migration/unison tests still
   green; exhaustion test.
5. **Wiki pass** (§8) and threshold listening notes.

## 8. Wiki updates (same-task rule)

- `dsp-design.md`: E–W section — replace the "one structural compromise" paragraph with
  the layer model (routing rule, kClaimAtt, exhaustion steal, cross-layer harmonize
  limitation).
- `gotchas.md`: layer exhaustion steals quietest; harmonize doesn't couple across layers;
  knob-at-0 = layer-0-only = legacy.
- README index: add this plan. Flip this banner when phases land.

## 9. Constants (top of `SpectralEngine.cpp`)

| Constant | Proposed | Meaning |
|---|---|---|
| `kNumLayers` | 4 | location layers per engine |
| `kClaimAtt` | 0.1 | att below which injection claims a new layer instead of dragging (matches particle `kMatchAttFloor`) |
| `kClaimClearFloor` | ~1e-4 (|S| units: check against `activeThresh` scale) | residual energy above which a claimed layer-bin is hard-reset before injection |
| reused | — | `kLocSigma`, `kInjLocFloor`, everything else unchanged |

## 10. Risks / open questions

- **CPU**: K× per-bin `exp` for att (~770k exp/s/engine at 4096/48k). Expected fine;
  measure in phase 2, and if hot, cache att per layer-bin and refresh only when `ewL`
  or the bin's loc moved more than epsilon.
- **kClaimAtt boundary feel**: drag-vs-claim flips at d ≈ 0.53. If audible A/B feels
  wrong mid-room, consider a soft zone (blend inject between nearest layer and claimed
  layer) — only if listening demands it, it complicates the self-consistency property.
- **Display**: the spectrum view shows the mix (automatic, no change needed), so two
  same-freq tones at different places show as one line whose height depends on the knob.
  Fine for now; per-layer tinting is a future GUI idea, out of scope.
- **State size**: plugin state doesn't serialise held audio (never has), so no
  persistence impact.
- This is additive machinery on the bin engine while the particle branch solves the same
  problem structurally. If layers get gnarly in practice, that's *useful signal* for the
  paradigm decision, not a failure of this experiment. Keep `exp/particles` untouched.
