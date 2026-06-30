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
   - Combine as a **blend**, with `harmonize` as the master and `harmonic` as the character:
     `df = harmonize · kHarmRate · ((1−harmonic)·Δent + harmonic·Δharm)`, then clamp the
     `omega` step to `±kHarmStep`. **`harmonic` does nothing when `harmonize = 0`** (master gate).
3. **Apply**: shift `omega` of each peak bin ±2 neighbours (leakage), clamped to
   `expectedAdv ± π` so the phase-vocoder stays valid.

## Parameters
| GUI / id          | Range     | Default | Meaning |
|-------------------|-----------|---------|---------|
| Harmonize `harmonize` | 0..1   | 0       | **Master amount.** Strength of the pitch coupling. 0 = off (and disables Harmonic). |
| Width `harmWidth` | 0.05..3 oct | 0.5  | σ of the nearness curve. Wide = global averaging; thin = only close tones interact. |
| Harmonic `harmonic` | 0..1    | 0       | **Character blend** under Harmonize: 0 = pure entrainment (averaging), 1 = pure harmonic attraction (low-denominator ratios). |

## Permanent vs momentary (important UX point)
Harmonize **permanently rewrites the held `omega`** — that's required for tones to *converge*
over time (integration). Like Brush and Compress, it's a **state edit**, so:
- with **Feed = 0** the shift sticks after you lower the knob (permanent);
- with **Feed > 0** the input's frequency tracking keeps pulling `omega` back, so the edit
  self-heals and it feels momentary.
Filter and Phase Noise, by contrast, are output/live and always momentary. This Feed
interaction is the source of the "sometimes permanent, sometimes momentary" feel.

## Influence overlay
When Harmonize > 0 the display overlays, per detected peak: a gaussian **influence hump**
(width = the Width knob, height/alpha = the peak's weight), a vertical marker, and a
**drift arrow** (right = pitch rising, left = falling; length ∝ current drift). Data comes
from `SpectralEngine::copyPeaks` (snapshotted under `displayLock` in `applyHarmonize`).

## Cost
Peak-based: P≈tens → O(P²)·(ratio table) ≈ ~1M ops/s. Negligible. The trap is per-bin O(B²);
don't do that.

## Energy migration (step 4 — lets tones cross bins for big moves)
`omega` alone only represents a frequency within ~±2 bins of its home bin (phase-advance
range). To move a tone further, the **energy itself** must walk across bins:
- A bin's `omega` is frequency-absolute, so the *same* `omega` value in an adjacent bin plays
  the *same* pitch — moving energy between bins (carrying `omega`) is sonically transparent.
- When a peak's centre drifts past **half a bin** (`rel > binW/2`), its whole packet
  (centre ± `kPad` bins) is shifted **rigidly** by one bin, carrying each bin's `S`, `omega`
  and `prevPhase`. Rigid (whole-packet) is essential: migrating bins *independently* tears the
  coherent peak apart (it splits into two). After the shift the centre's `rel` drops by one
  bin, so it can keep drifting and migrate again — unlimited travel, one bin at a time.
- This is why **peak detection matters** (`SpectralEngine.cpp`, step 1):
  - `kPeakFloor = 0.03` (~-30 dB, just above Hann's ~-31 dB first sidelobe): low enough to
    include fairly **quiet tones** in harmonizing, high enough that leakage isn't a "tone".
    Don't drop it below the sidelobe level or leakage becomes false peaks.
  - prominence = local max over **±1 bin** only. A wider window suppresses the quieter of two
    **close tones**, counting them as one peak — they then never entrain against each other and
    beat. ±1 resolves tones down to ~2 bins apart while a single tone's mainlobe stays one peak.
  - These two are coupled with the unison lock: `lockTolHz` (~2 bins) must be ≥ the separation
    at which ±1 detection collapses the pair to one, so they get locked just before that.
  Verified by the `peak detection:` test (close pair → 2 peaks; a -26 dB tone is detected).

Verified by the `energy migration:` test: a weak tone is pulled to a strong anchor's 4/3
harmonic and lands **below** its omega-only floor (1400 → ~1371 Hz, past ~1383), proving the
energy actually moved bins.

## What causes the lingering beating (and what doesn't)
Only **slow, audible** beating is a problem, and its sole cause here is tones entraining to
*almost* the same frequency and **stalling a hair mistuned** (below). Note what is **not** a
cause: locking to a simple harmonic ratio (3/2, 4/3…) gives a **short** common period, so its
interference is fast — perceived as timbre, not as audible beating. Short-period interference is
not a sign of inharmonicity, so harmonic ratios don't need "fixing".

## Unison merge (why slow beating eventually stops)
Two tones entrained to *almost* the same frequency would otherwise sit a hair mistuned and
**beat forever** (a slow, audible beat) — each tone is a separate per-bin phasor, and the
detector stops resolving them as two before they reach exact unison.

Fix (step 1b): when two peaks are within `lockTolHz` (~2 bins) in frequency, **sum their energy
into the single bin nearest the common (amplitude-weighted) frequency, and clear both packets**.
The result is **one centred phasor**.

Two earlier attempts and why they failed (don't repeat them):
- *Frequency lock only* (equalise `omega`, leave energy in place): a phasor stored away from its
  bin centre produces an **amplitude ripple** from the overlap-add — itself a beat. So leaving
  the two clusters apart at the same frequency still beats.
- *Bin-distance merge*: tones could be the same pitch yet several storage bins apart; bin
  distance missed them. Merge on **frequency**, and place the result at the bin **nearest** that
  frequency so it's centred (no ripple) and clear ±`kPad` around both sources so leakage doesn't
  re-form a ghost peak.

Migration (step 4) also has **collision avoidance**: it skips any peak with another peak within
`2·kPad` bins, because overlapping rigid packets would trample each other. Near pairs are handled
by the merge instead. Verified by the `unison lock:` test (beat depth **0.73 → ~0.07**).

## Tuning / next steps
Constants at the top of `SpectralEngine.cpp`: `kEntRate`, `kHarmRate`, `kHarmStep`,
`kPeakFloor`, `kMaxDen`, `kMaxPeaks`, and `kPad` (migration packet half-width). The GUI
`harmonize` range is intentionally small (0..0.1) — the drift is strong, so a little goes a
long way. Possible next steps: persistent peak *tracking* (identity across frames) for cleaner
migration under dense spectra, and handling packet *collisions* when two tones migrate into
each other.
