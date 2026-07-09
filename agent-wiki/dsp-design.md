# DSP design — the spectral engine

All of this lives in `Source/SpectralEngine.{h,cpp}`. One engine per channel.

## STFT framing
- Real-only FFT via `juce::dsp::FFT`. `numBins = fftSize/2 + 1`.
- **Overlap = 4** (75%), so `hopSize = fftSize/4`. Fixed (`kOverlap`).
- Hann window applied on **both** analysis and synthesis. Overlap-add normalisation for a
  Hann² window at 75% overlap is the constant **1.5**, so output is scaled by `winNorm = 1/1.5`.
- I/O uses two sliding ring buffers of length `fftSize` (`inRing`, `outRing`). Per sample:
  pop one output sample, push one input sample; every `hopSize` samples run `processFrame()`.
- **Latency = fftSize.** Reported to the host via `setLatencySamples()`.
- `performRealOnlyInverseTransform` already applies the 1/N normalisation — do **not** add
  another 1/N. (Verified empirically: unity-ish passthrough, smoke-test `maxAbs ≈ 1.06`.)

## The held-phasor model (why it "extrapolates the FT")
Each bin keeps a complex state `S[k]`. Per frame, in order:
1. **Phase advance:** `S[k] *= exp(i·omega[k])`, where `omega[k]` is the bin's **measured
   instantaneous per-hop phase advance** (see below). This makes every bin a free-running
   oscillator that sustains as a continuous sinusoid across overlap-add frames.
2. **Inject input:** `S[k] += feed · Xs[k] · comp[k]` (see compensation below).
3. **Decay / shape:** `S[k] *= decay[k]`.

### Instantaneous-frequency tracking (why it's smooth, not granular)
Advancing each bin by its **bin-centre** frequency (`2π·k·hop/N`) is the naive freeze: a real
partial spreads across several leakage bins whose correct phases all advance at the partial's
*true* frequency, locked together. Bin-centre advance lets them drift apart within a few hops →
partials smear, frames stop being phase-continuous, and you hear each frame as an independent
windowed grain (a buzz/loop at ~sr/N). **This was a real defect and is fixed.**

Fix: estimate the true advance from the input via phase unwrapping and free-run at that rate.
Per frame, where the bin has energy (`|X[k]| > kTrackThresh`):
```
dev      = wrap_to_pi( (arg X[k] - prevPhase[k]) - expectedAdv[k] )
omega[k] = expectedAdv[k] + dev          // expectedAdv[k] = 2*pi*k*hop/N (bin centre)
```
`prevPhase[k]` is updated every frame; `omega[k]` is held (frozen) for empty bins and after the
input stops, so the tail sustains at the captured pitch. Leakage bins of one partial all measure
~the same `omega`, so they stay coherent → smooth continuous tone. JUCE forward transform uses
`exp(-i…)` and inverse `exp(+i…)`, so the measured advance is used directly as a `+omega` rotation.

**Phase Noise** (`phaseNoiseAmt`, continuous 0..1, agent-wiki/plan-roadmap.md B4): the rotation
each frame uses `omega[k] + jitter`, with `jitter = ±kPhaseNoiseMax·amt` rad (uniform,
`kPhaseNoiseMax = 0.5`) from a per-engine `juce::Random`. It's injected into the *frequency
tracking* (the rotation), **non-accumulating** (not stored back into `omega`), so it adds
shimmer/roughness without permanently detuning. RT-safe (no alloc). Was a bool pre-B4 (legacy
"on" = 0.15 rad, migrated to `amt = 0.3` on old-session load — see `setStateInformation`).

**Feed gates the tracking.** The update is `omega[k] += (measured - omega[k]) · trackW` with
`trackW = Feed`. This matters: the tracking is a *second* input coupling (the input retunes the
held pitch via phase), separate from magnitude injection. If it ran unconditionally, the input
would keep bending the held tones even at `feed=0` — audible "phase leak". Gating by Feed makes
`feed=0` a true freeze (input fully ignored, verified by the `feed=0 ignores input` test) and
`feed=1` snap to the input pitch each frame (the original smooth behavior). `prevPhase` is still
updated every frame regardless (bookkeeping only; it never reaches the audio).

`S` is then written straight into the inverse FFT — there is **no** separate output filter.
The filter is entirely inside `decay[k]`.

## Parameter math (all per-hop, recomputed each frame)
Let `hop = hopSize`, `sr = sampleRate`.

- **Loss → decay.** `lossDecay = exp(-loss · hop/sr · 6)`. `loss=0 → 1.0` (eternal hold),
  larger loss → faster magnitude decay. The `6` is a feel constant.
- **Injection scale (gain staging).** Because the loop is now phase-coherent, continuous
  feeding *integrates* and would build up by `1/(1-lossDecay)` (tens of dB) and slam the
  limiter. So injection is scaled: `injScale = max(kInjFloor, 1 - lossDecay)`, and the
  effective feed is `feed · injScale`. Steady state (filter flat) is then
  `S ≈ feed · |input|` — the held level tracks the input level, predictably. `kInjFloor=0.05`
  keeps capture working at `loss=0` (where `1-lossDecay → 0`); at that extreme the hold still
  integrates slowly toward clipping and the limiter takes over. This is gain staging only — it
  does **not** touch the filter compensation invariant below.
- **Attack was removed.** `Xs[k] = X[k]` unsmoothed each frame; lowering Feed gives the
  same slowed-onset effect the old attack knob did.

## The spectral shaper
Replaces the old Filter + Compress with one per-bin signed curve. Math lives in
`Source/ShapeCurves.h` (header-only, no JUCE dependency) so the engine, the GUI overlay
(`SpectrumDisplay.cpp`), and tests share it verbatim — **never reimplement this math
elsewhere** (the old filter bell drifted between DSP and GUI once; don't repeat that).

Each bin gets a signed change `L[k] ∈ [-1..+1]` from `ShapeCurves::shapeL(shape, x, x0,
width, count, level, ratio)`, where `x = log2(binFreq)`, `x0 = log2(shapeFreq)`. `shape`
(0..4) linearly cross-fades between two adjacent named shapes (and the `shape` param's
display textbox shows the name(s), not the raw float — see `shapeValueToString` in
`PluginProcessor.cpp`). **All five shapes are bipolar / zero-mean by construction**
(`L=0` on average across the spectrum) so a permanent edit is always reversible in
principle, not just a one-way cut — see "the boost self-limit" below for why that's safe
to compound:

- **Level** (0, replaces Compress): `L = level·sign(u)·|u|^γ · win(x)`, where
  `u = ln(ratio)/4.6`, `ratio = |S[k]|/mean(active bins)` (same active-bin pivot as the
  old Compress — bins ≤ `maxMag·1e-3` get `ratio=1` → `L=0`, leaving the noise floor
  alone). **Post-#14 remap: `Width` sets the gaussian window extent** (`win(x)`, flattens
  to 1 everywhere as `width → 1` — the more intuitive control, since it's what visibly
  changes the affected band), **`Count` sets the extremes-vs-mean warp** `γ =
  2^((count-0.5)·2)` (`count=0.5` → γ=1, the compress-equivalent 1:1 case). (Before #14
  these two were swapped: Width was the warp, Count the extent.)
- **Sigmoid** (1): `L = level·tanh((x-x0)/wOct)` — a **bipolar tilt**: `level>0` boosts
  above `x0` and cuts below by the same shape (antisymmetric), `level<0` the reverse,
  continuous through `level=0` (flat). Was cut-only pre-bipolar-rework (`(tanh∓1)/2`);
  now nets to zero and a permanent tilt is genuinely undoable by re-tilting the other way.
  `count` unused.
- **Spikes** (2): a **bipolar comb** — gaussian "teeth" at `x0 + n·d` and equal, opposite
  "anti-teeth" at the geometric midpoints `x0 + (n+0.5)·d`, `n=0,±1,±2,…`, amplitude
  fading in per pair as `count` grows. `L = level·(teeth_env − antiteeth_env)`, each env
  clamped to `[0,1]`. `level>0` boosts the teeth and cuts the gaps, `level<0` the reverse;
  zero-mean by construction (was subtractive-only pre-rework: `level>0` used to *notch*
  peaks, `level<0` used to *isolate* them, always `L≤0`).
- **Harmonics** (3): teeth at `x0 ± log2(n)`, `n=1,2,3,…` (overtones `n·f0` on `+`,
  undertones `f0/n` on `-`, fundamental shared at `n=1`), anti-teeth at the geometric
  midpoint between consecutive harmonics on each side. Same bipolar construction and sign
  convention as Spikes: `level>0` boosts the harmonic series and cuts between,
  `level<0` the reverse. Per bin only the 2–3 nearest harmonics are evaluated (cheap, same
  trick as Spikes); each harmonic's gaussian half-width is clamped to `0.3·log2((n+1)/n)`
  (local spacing) so high harmonics stay distinct. `count` controls how many
  overtone/undertone pairs are active (0 = fundamental only, 1 = 12 pairs/side).
- **Sine** (4, replaces Filter): `L = level·cos(2π·ρ·(x-x0))·env(x)`, `ρ` cycles/octave
  from `width`, `env` a gaussian window like Level's that flattens as `count → 1`. Already
  bipolar (a cosine is zero-mean over a full cycle) — unchanged by the rework. At
  `level=-1, count≈0` this is a single bell cut = the old filter.

**Applying L[k]** (`processFrame`, per bin, `t = shapeAmt·L[k]`, `m = shapeMode`):
- **Permanent** (compounds): `S[k] *= exp(t · m · kPermScale · att)` (`kPermScale = 0.23`;
  `att` is the East–West distance attenuation, 1 at the default `ewLocation=0`). At
  `shape=0, count=0.5, width=1, mode=1, amount=1` (post-#14 mapping) this is *exactly*
  `ratio ^ (level·0.05)` — bit-identical to the old Compress formula (the constants 4.6
  and 0.23 are coupled; don't change one without the other).
  - **The boost self-limit** (`kPermCeilNorm`): since the shapes are now bipolar, `L>0`
    means the permanent edit can *boost* a bin, and `exp(+)` compounding every frame would
    grow without bound (measured: ~1e34 in the everything-on stability test before this
    was added). So when `e = t·m·kPermScale·att > 0`, it's scaled by
    `max(0, 1 - |S[k]|/permCeil)` (`permCeil` = a full-scale bin, `kPermCeilNorm·fftSize/2`)
    — the boost self-limits toward that ceiling instead of compounding past it. Cuts
    (`e<0`) are unbounded-down, as before. Don't remove this for a hard per-bin clamp —
    the self-limit is what keeps the approach smooth near the ceiling.
- **Momentary** (non-destructive, replaces Filter): `tm = t·(1-m)`;
  `gOut = exp(tm·ln4)` for `tm≥0` (up to +12 dB boost), or `(1+tm)²` for `tm<0` (smooth
  cut to 0 at `tm=-1`). Output is `S[k]·gOut`, `S` itself untouched — turning Amount down
  restores the held sound intact. Not distance-weighted (it shapes the already
  East–West-gained output, not the held state).
- **Input compensation**: `comp[k] = 1/max(kCompFloor, min(1, gOut))`. Only momentary
  *cuts* are compensated (fed tones pass through unaffected, as the old filter did); boosts
  are **not** compensated, so live input isn't attenuated by them.

`kCompFloor = 0.05` (unchanged from the old filter). Bin 0 (DC) and frames where
`shapeAmt·|shapeLevel| ≈ 0` skip the whole shaper (`L=0`, `gOut=1`, `comp=1`) — cheap
early-out gate, mirrors the old `compress`/`filterAmt` gating.

## Spectral brush (GUI editing of the held state)
Dragging on the display (`SpectrumDisplay`) permanently reshapes the held spectrum `S` —
this is the destructive editor (distinct from the non-destructive filter). Each pointer
event is queued via `SpectralEngine::queueBrush(centreFreqHz, strength)` and applied on the
audio thread in `drainBrush()` (at a frame boundary, under a try-lock — never blocks audio):
- `strength ∈ [-1..+1]` from the vertical position: **top = +1 (boost), centre = 0 (no
  change), bottom = -1 (cut)**, smooth through zero.
- Gaussian falloff in log-frequency around the cursor (`kBrushSigmaOct = 0.6` oct), so it's
  a blurred brush, strongest at the centre tone.
- Per event: `S[k] *= exp(strength · w · kBrushRate · ln(kBrushMaxFactor))`. `kBrushRate`
  (0.012) is small so it compounds smoothly as you hold/drag (paint-like), rather than
  snapping. Pen pressure (if reported) scales `strength`.
- Note for testers: edits to `S` only reach the output after the **STFT latency** (fftSize
  samples). The offline brush test flushes >fftSize samples before measuring.

## Limiter (in `PluginProcessor`, not the engine)
Linked across channels so the stereo image is preserved. Two DAW-facing params:
`limThreshold` (dB, -24..0, default 0) and `limRelease` (ms, 50..5000, default 1200);
attack stays fixed at `kLimAttMs = 5 ms` (not exposed — always fast enough to catch peaks).
- `peak = max over channels of |sample|`.
- `limEnv` follows `peak` (one-pole): fast attack, release from `limRelease`
  (`limRelCoef` recomputed once per block from the param — cheap, one `exp` per block).
- `thr = decibelsToGain(limThreshold)`. `target = (limEnv > thr) ? thr/limEnv : 1`. Gain is
  **1.0 until the signal would exceed the threshold** (at the default 0 dB this is exactly
  the old fixed "don't clip ±1" behaviour).
- `limGain` eases toward `target` (fast when reducing, slow when recovering per
  `limRelease`) and multiplies every channel. Result: transparent under the threshold,
  gentle pull-down above it at the chosen release speed.
- Tested against a synthetic peak train in `test_main.cpp` (the limiter lives in the
  processor, not the engine, so the test replicates the 5-line envelope math rather than
  linking `PluginProcessor` into the test target).

## Aux input path (reverb feed, `revFeed`, agent-wiki/plan-roadmap.md Part A)
A second, **Feed-independent** injection path, so the output reverb's wet tail can feed
back into the held spectrum even when `feed` is low/zero (the normal frozen-hold case —
see gotchas.md for why the *old* `revFeed` never worked). Replaces the removed feature.
- `SpectralEngine::process` takes an optional `aux` pointer (nullptr = silent); a 4-arg
  overload forwards `aux=nullptr` for callers (tests, anything) that don't use it.
- `auxRing` shares `inWrite` with `inRing`, so the aux frame is sample-aligned with the
  analysis frame by construction — no extra bookkeeping needed.
- Aux gets its **own forward FFT** (`auxFftData`), computed only when `revFeed > 1e-4` so
  the default costs nothing.
- Injection: `injAux = revFeed · injScale · Xaux[k] · comp[k]` — reuses the same
  `injScale`/`comp` as the live path, so it responds to Loss and the momentary-cut
  compensation the same way.
- **Feedback-loop safety:** live input's `inj` is added uncapped (as always); `injAux` is
  added through the *same* soft ceiling the permanent shaper's boost uses
  (`g = max(0, 1 − |S|/permCeil)`, `permCeil = kPermCeilNorm·0.5·fftSize`) — because unlike
  live input, aux closes a **real loop** (engine → reverb → engine), and an uncapped
  additive loop at `loss=0` diverges. The cap holds each bin's normalised magnitude
  (`|S|·2/fftSize`) near `kPermCeilNorm ≈ 1.0`; it does **not** bound the *time-domain*
  sum across ~2000 bins, which can legitimately be large once many bins sit near their own
  cap (coherent sum) — don't mistake a big output sample for a bug, check per-bin magnitude
  instead (see the `aux feedback loop stability` test).
- **Frequency tracking is input-only.** Aux energy is not fed into `prevPhase`/the
  instantaneous-frequency measurement — it inherits the receiving layer's `omega`. The
  reverb wet is a smeared copy of the held tones at roughly the same bins, so this is
  correct-enough and keeps the tracking logic simple.
- In `PluginProcessor`: `revFeedBuf` holds the **previous block's** reverb wet mono
  (`0.5·(wetL+wetR)`), fed to every engine's `aux` this block — an inherent one-block
  feedback delay. The reverb now runs whenever `revMix > 0 || revFeed > 0` (revFeed needs
  the wet tail even at `revMix = 0`, i.e. reverb as a silent hold-exciter); hard bypass
  (skip `reverb.process` entirely, bit-exact dry) requires **both** at 0.

## Transpose (agent-wiki/plan-roadmap.md B3)
Pitch-shifts the **output** of the held sound; `S`/`omega` (the held state) are never
touched, so it's fully non-destructive — turning Transpose back to 0 instantly recovers the
original pitch, same frame.

- `transRatio = 2^(transpose/12)`; `transposing = |transpose| > 1e-3` gates the whole path
  (default costs one branch, no extra work).
- **Per-layer phase accumulator**, not a resample: each layer/bin keeps `transAcc[idx]`, an
  *extra* phase offset advanced only while transposing:
  `transAcc[idx] += omega[idx]·(transRatio − 1)`, wrapped to `[-π,π]`. The layer's
  contribution to the transposed output is `sk · attL · exp(i·transAcc[idx])` — same
  magnitude as the untransposed path (a pure phase rotation), so it doesn't touch loudness.
  This is what makes the pitch shift itself (not just the bin it lands in): the *output*
  bin's phase now advances at `omega·transRatio` per hop instead of `omega`.
- **Bin remap:** the transposed per-bin sum is written into `synthScratch[k']` where
  `k' = round(k · transRatio)` (dropped if `k' < 1` or `k' ≥ numBins`). This is what actually
  moves energy to the new frequency — the accumulator above only keeps what lands there
  phase-coherent. `synthScratch` is zeroed once per frame (only when transposing) and copied
  into `fftData`/`dispScratch` **after** the full per-bin loop (can't write in place —
  multiple source bins can map to bins the loop hasn't reached yet, or has already passed).
- **Non-transposing path is untouched** (writes straight into `fftData`/`dispScratch` inside
  the per-bin loop, exactly as before B3) — bit-exact at the default `transpose = 0`.
- **MIDI** (`PluginProcessor`): monophonic, last-note priority. `midiNote` (plain member,
  audio-thread only) is set on note-on, cleared on a note-off *matching the currently held
  note* (so releasing an older, already-superseded note can't cancel a newer one). Effective
  `p.transpose = knob + (midiNote>=0 ? midiNote-60 : 0)`, **not clamped** to the knob's
  ±12 st DAW range — playing further from middle C should keep transposing further.

## Stereo spread (agent-wiki/plan-roadmap.md B5)
Momentary per-bin complementary L/R gain on the **output only** — never touches `S`, so it
can't drift the held state and costs nothing when `spread=0` (skipped entirely). Deterministic
hash of the bin index `k` (`u = k·2654435761`, `h = ((u>>16)&0xFFFF)/32767.5 − 1 ∈ [−1,1]`)
gives a fixed, repeatable L/R pattern rather than noise. Per-channel gain
`g = sqrt(1 + spreadSign·spread·h)` (argument stays in `[0,2]`, so `gL²+gR² = 2`, equal-power);
`spreadSign` is `+1`/`−1` set by the processor per engine (channel 0 / 1), and it also forces
`spread=0` on a mono bus (no second channel to spread against). Applied after `gOut`, before
the bin is written to `fftData`/`synthScratch` — same insertion point for the transposing and
non-transposing paths. No dedicated GUI widget yet (host-automatable only) — see
[parameters.md](parameters.md).

## East–West location field (continuous tone locations, plan v2 + location layers)
Every bin/tone carries a **continuous location** `binLoc[k] ∈ [0,1]` alongside `S`/`omega`.
The `ewLocation` knob (0 = East = legacy default, 1 = West) is a **listener/recorder
walking the line**:

- **Playback**: each bin is output through `att(d) = exp(−(d/kLocSigma)²)`,
  `d = |ewLocation − binLoc[k]|`. The attenuation is **absolute — never normalised across
  tones** — so gains are smooth in both knob position and time; nothing can jump (v1's slot
  blend normalised weights and did jump; see plan-eastwest.md). Louder tones stay audible
  further away simply because attenuation multiplies amplitude.
- **Recording**: injection pulls the fed bin's location toward the knob, weighted by new
  vs held energy: `loc ← (aHeld·loc + aInj·L)/(aHeld+aInj)` (skipped for near-zero
  injections, `kInjLocFloor`, so silence never drags tones). Continuous — no quantisation.
- **Edits take the dimension into account**: permanent shaper and brush exponents and each
  harmonize peak's drift are scaled by the same `att(d)` — nearest tones are edited at full
  strength, far ones barely. The **momentary** shaper stays full-strength on the output
  (which is already location-gained).
- **`binLoc` travels with energy**: harmonize's unison merge (amplitude-weighted, like
  omega) and rigid packet migration both carry it. Any future code that moves energy
  between bins must move `binLoc` too.
- **Backward compatible at 0**: `reset()` fills `binLoc = 0` and the param defaults to 0,
  so parked at East every `att = 1` and the engine is exactly the pre-E–W single buffer —
  all params affect the sound as before.

### Location layers (`kNumLayers = 4`, agent-wiki/plan-loclayers.md)
The single structural compromise above — one location per bin, so two same-frequency
tones can't coexist, re-recording a pitch elsewhere used to *drag/smear* it — is fixed by
giving each bin **`kNumLayers` parallel held states** (`S`/`omega`/`binLoc`, flat arrays of
length `kNumLayers*maxBins`, stride `maxBins`, indexed via the private `li(layer, bin)`
helper). `expectedAdv` and `prevPhase` stay **shared** across layers: `prevPhase` tracks
the *input's* phase for unwrapping, a property of the analysis, not of any held layer.

- **Injection routing** (`processFrame`, per bin, before the per-layer update): find the
  layer nearest the knob (tie-break: whichever already holds energy here, then lowest
  index — keeps `ewLocation=0` landing in layer 0 forever, reproducing single-buffer
  behaviour exactly). If that nearest layer's `att ≥ kClaimAtt` (0.1, matching
  `ParticleEngine`'s `kMatchAttFloor` so both paradigms agree on where "elsewhere"
  starts), **drag** it toward the knob as before (same place, being re-recorded). If every
  layer is far (`att < kClaimAtt`), **claim** the quietest layer instead: hard-reset its
  stale residual if any (`kClaimClearFloor`), snap `binLoc = ewLocation`, inject fresh.
  Every other layer at that bin is untouched.
- **Frequency tracking is gated to the injection-receiving layer only** — otherwise
  recording a detuned tone far away would silently retune a held tone's pitch through the
  shared `omega` update. (`omega` itself is per layer, so this is enforced by only writing
  the chosen layer's slot.)
- **Playback mixes before synthesis, still one IFFT**: `outBin[k] = gOut · Σ_l S_l[k]·att_l[k]`.
  CPU cost grows only in the per-bin update loop (≈4× the rotate/decay/shaper work);
  windowing/OLA/IFFT cost is unchanged.
- **Shaper pivot is the listener mix**: `mixAbs[k] = Σ_l |S_l[k]|·att_l[k]` — "shape what
  you hear" — used for both the Level-shape mean scan and the per-bin ratio. Permanent
  shaper and brush apply **per layer**, each scaled by that layer's own `att_l`.
  Momentary shaper (`gOut`) stays bin-level, applied once to the mixed output.
  Loss/decay is per layer too (`exp(−lossRate·att_l)`), so a far layer is spared exactly
  like before, just independently per layer now.
- **Harmonize runs per layer, independently** (`applyHarmonize(p, layer, isFirstLayer)`):
  peak detect, unison merge, drift and migration all stay within one layer's slice. Tones
  held in *different* layers do not entrain against each other — a known limitation, not a
  bug; cross-layer coupling is unimplemented future work if it's missed by ear.
  `copyPeaks`'s influence overlay is the **union** across layers (`isFirstLayer` resets the
  running `dispPeakN`, every layer's call appends).
- **Layer exhaustion**: if all `kNumLayers` are occupied by far content at a bin, a claim
  steals the quietest one. The stolen tone was far from the listener already (small
  `att`), so it's nearly inaudible at the knob position where the steal happens; it simply
  vanishes at its own home position. Finite-resource compromise, same spirit as
  `ParticleEngine`'s pool eviction.
- **Backward compatible**: at `ewLocation=0` with a fresh `reset()`, every layer starts at
  `binLoc=0`, so the tie-break always lands injection in layer 0 and layers 1–3 stay
  empty (contribute 0 to the mix) — bit-for-bit the pre-layers single-buffer engine. Every
  pre-existing (non-layer-specific) test runs at this default and is the regression net.

Cost: `kNumLayers` × one `exp` per bin per hop for `att`, computed twice per bin (once in
the shaper's `mixAbs` pre-pass when active, once in the routing/update loop) — cheap
relative to the FFT/OLA budget, but a candidate to cache if a future profile shows it hot.

## Reconfiguring FFT size at runtime
`setOrder()` is called from the message thread; it only stores `pendingOrder`. The audio
thread applies it in `applyPendingOrder()` **at a frame boundary**, under `displayLock`.
All buffers are preallocated at the **max** size in `prepare()`, so `configure()` never
allocates — it just recomputes window/rot tables and the FFT object. State is reset on
change (a clean re-freeze), and latency is re-reported.

## Constants worth knowing (top of SpectralEngine.cpp)
`kOverlap=4`, `kCompFloor=0.05`, `kDecayFloor=1e-4`, `kPermScale=0.23` (coupled with the
`4.6` normaliser in `ShapeCurves.h` — see the shaper section above), `kLn4=ln(4)` (momentary
boost ceiling, +12 dB). East–West: `kLocSigma=0.35` (room size — gaussian attenuation width;
d=0.5 → ~−18 dB, d=1 → ~−71 dB), `kInjLocFloor` (ignore near-zero injections when pulling
`binLoc`). Location layers: `kNumLayers=4` (private, `SpectralEngine.h`), `kClaimAtt=0.1`
(drag-vs-claim threshold), `kClaimClearFloor=1e-4` (residual above which a claimed layer's
stale content is hard-reset before the fresh recording). Limiter constants are at the top
of `PluginProcessor.cpp`.
