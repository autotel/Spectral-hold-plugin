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

**Phase Noise** (boolean): when on, the rotation each frame uses `omega[k] + jitter`, with
`jitter = ±kPhaseNoise` rad (uniform, `kPhaseNoise = 0.15`) from a per-engine `juce::Random`.
It's injected into the *frequency tracking* (the rotation), **non-accumulating** (not stored back
into `omega`), so it adds shimmer/roughness without permanently detuning. RT-safe (no alloc).

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
(0..4) linearly cross-fades between two adjacent named shapes:

- **Level** (0, replaces Compress): `L = level·sign(u)·|u|^γ · win(x)`, where
  `u = ln(ratio)/4.6`, `ratio = |S[k]|/mean(active bins)` (same active-bin pivot as the
  old Compress — bins ≤ `maxMag·1e-3` get `ratio=1` → `L=0`, leaving the noise floor
  alone), `γ = 2^((width-0.5)·2)` (width=0.5 → γ=1, the compress-equivalent 1:1 case),
  and `win(x)` a gaussian window in log-freq centred at `shapeFreq` that flattens to 1
  everywhere as `count → 1` (so Freq is inert at the default `count=1`).
- **Sigmoid** (1): `L = level·(tanh((x-x0)/wOct) ∓ 1)/2` — cut-only, continuous through
  `level=0` (flat). `level>0` cuts below `x0` (highpass), `level<0` cuts above (lowpass).
  `count` unused in v1.
- **Spikes** (2): a gaussian spike comb `env ∈ [0..1]` at `x0 + n·d`, `n=0,±1,±2,…`,
  amplitude fading in per spike-pair as `count` grows (smooth 1-spike → many-spikes). It's
  **purely subtractive** and sign-split like the Sigmoid: `level>0 → L = -level·env`
  (reject/notch the peaks), `level<0 → L = level·(1-env)` (pass **only** the peaks, cut
  everything between), flat no-op at `level=0`. Always attenuates (`L ≤ 0`).
- **Harmonics** (3): spikes at `x0 ± log2(n)`, `n=1,2,3,…` — overtones (`n·f0`) on the `+`
  side, undertones (`f0/n`) on the `-` side, fundamental shared at `n=1`. Same
  subtractive sign-split as Spikes (`level>0` rejects/notches the harmonic series,
  `level<0` passes only the series — isolates everything harmonically related to
  `shapeFreq`). Per bin only the 2 nearest harmonics on that side are evaluated (cheap,
  same trick as Spikes); each harmonic's gaussian half-width is clamped to
  `0.3·log2((n+1)/n)` (the local spacing) so high harmonics stay distinct instead of
  smearing together. `count` controls how many overtone/undertone pairs are active
  (0 = fundamental only, 1 = 12 pairs/side — same slow ramp as Spikes' `count`).
- **Sine** (4, replaces Filter): `L = level·cos(2π·ρ·(x-x0))·env(x)`, `ρ` cycles/octave
  from `width`, `env` a gaussian window like Level's that flattens as `count → 1`. At
  `level=-1, count≈0` this is a single bell cut = the old filter.

**Applying L[k]** (`processFrame`, per bin, `t = shapeAmt·L[k]`, `m = shapeMode`):
- **Permanent** (compounds, replaces Compress): `S[k] *= exp(t · m · kPermScale)`
  (`kPermScale = 0.23`). At `shape=0, width=0.5, count=1, mode=1, amount=1` this is
  *exactly* `ratio ^ (level·0.05)` — bit-identical to the old Compress formula (the two
  constants 4.6 and 0.23 are coupled; don't change one without the other).
- **Momentary** (non-destructive, replaces Filter): `tm = t·(1-m)`;
  `gOut = exp(tm·ln4)` for `tm≥0` (up to +12 dB boost), or `(1+tm)²` for `tm<0` (smooth
  cut to 0 at `tm=-1`). Output is `S[k]·gOut`, `S` itself untouched — turning Amount down
  restores the held sound intact.
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

## East–West location field (continuous tone locations, plan v2)
Every bin/tone carries a **continuous location** `binLoc[k] ∈ [0,1]` alongside `S`/`omega`/
`prevPhase`. The `ewLocation` knob (0 = East = legacy default, 1 = West) is a **listener/
recorder walking the line**:

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

One structural compromise: locations are per **bin**, so two same-frequency tones cannot
coexist at two locations — re-recording a pitch elsewhere *drags* it (weighted merge).
Documented in gotchas. Cost: one `exp` per bin per hop.

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
`binLoc`). Limiter constants are at the top of `PluginProcessor.cpp`.
