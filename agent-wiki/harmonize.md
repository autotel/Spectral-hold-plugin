# Harmonize (experimental)

**Branch `experimental/harmonize`.** Coupled-oscillator tone interaction: held tones pull on
each other's *pitch* so they drift together over time (like the synchronising-metronomes
experiment). Lives in `SpectralEngine::applyHarmonize`, called each frame in `processFrame`
after compress and before the per-bin synthesis loop (so the shifted `omega` is used the same
frame). Works on the held state, **independent of Feed** (it's a property of what's held).

## Why it fits our engine
Each bin already free-runs at `omega[k]` (rad/hop) and synthesis plays it at that frequency
(phase-vocoder). So harmonizing = adding a small per-frame **drift to `omega`** and you *hear*
the tones glide into alignment. No new synthesis path. `f = omega · sr / (hop · 2π)`.

## Algorithm (peak-based — the key to it being cheap)
1. **Extract peaks**: local maxima of `|S|` above `kPeakFloor · maxMag`, capped at `kMaxPeaks`
   (128). Per peak: bin, amplitude `A`, frequency `f` (from `omega`). Operating on peaks, not
   all bins, makes it O(P²) ≈ tiny instead of O(B²).
2. **Reciprocal drift** (all peaks from the same snapshot, then applied):
   - **Entrainment** (`harmonize`): `Δf_i = Σ_j w·A_j·(f_j−f_i) / Σ_j w·A_j` — drift toward the
     amplitude-weighted mean of neighbours (louder tones pull harder).
   - **Harmonic** (`harmonic`): for each pair, snap toward `f_j · (nearest low-denominator
     ratio n/m)`, weight `A_j · 1/(n·m)` so simple ratios (2/1, 3/2…) dominate. Ratio table
     built once for `n,m ≤ kMaxDen` (coprime).
   - `w(i,j) = exp(−Δoct² / 2σ²)`, `σ = harmWidth` (octaves) = the nearness-influence width.
   - Combine: `df = harmonize·kEntRate·Δent + harmonic·kHarmRate·Δharm`, clamp the resulting
     `omega` step to `±kHarmStep`.
3. **Apply**: shift `omega` of each peak bin ±2 neighbours (leakage), clamped to
   `expectedAdv ± π` so the phase-vocoder stays valid.

## Parameters
| GUI / id          | Range     | Default | Meaning |
|-------------------|-----------|---------|---------|
| Harmonize `harmonize` | 0..1   | 0       | Entrainment strength (averaging / metronome sync). |
| Width `harmWidth` | 0.05..3 oct | 0.5  | σ of the nearness curve. Wide = global averaging; thin = only close tones interact. |
| Harmonic `harmonic` | 0..1    | 0       | Attraction toward low-denominator harmonic ratios. |

## Cost
Peak-based: P≈tens → O(P²)·(ratio table) ≈ ~1M ops/s. Negligible. The trap is per-bin O(B²);
don't do that.

## Known limitation / next steps
`omega`-drift only glides a tone a **modest** distance before the single-bin representation
breaks. Great for gliding into tune / sync (cents–semitone). **Large** harmonic locks (moving a
tone a fourth) would need **energy migration between bins** — a future stage. Tuning constants
(`kEntRate`, `kHarmRate`, `kHarmStep`, `kPeakFloor`, `kMaxDen`, `kMaxPeaks`) live at the top of
`SpectralEngine.cpp` / in the header. Smoke-tested by the `harmonize:` case (active + bounded);
convergence itself is judged by ear.
