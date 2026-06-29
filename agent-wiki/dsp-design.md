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
- **Attack → input smoothing.** `aCoef = exp(-attack · 9)`, applied as a one-pole on the
  fed spectrum: `Xs[k] += (X[k] - Xs[k]) · aCoef`. `attack=0 → aCoef=1` (instant); large
  attack → slow onset. Smoothing is on the **complex** input, so magnitude *and* phase ease in.
- **Filter (gaussian bell in log-frequency).**
  - `bell[k] = exp(-(oct - centreOct)² / (2·σ²))`, with `oct = log2(k·refFreq)`,
    `refFreq = sr/fftSize` (the frequency of bin 1), `centreOct = log2(filterTone)`,
    `σ = kSigmaOct = 1.25` octaves (constant; not exposed — change here if needed).
    Bin 0 (DC) is treated as fully out of band.
  - `gFilt[k] = 1 - filterAmt·(1 - bell[k])`  →  ranges `[1-filterAmt .. 1]`.
  - **The filter is NON-DESTRUCTIVE output shaping** (changed from an earlier destructive
    design). It does **not** enter `decay[k]` — the held state decays by `lossDecay` only.
    Output is `S[k] · gFilt[k]`. So turning the filter down restores the held waves intact,
    and sweeping Filter Tone sweeps the audible drone without permanently erasing partials.

## Filter compensation (the in/out invariant)
Spec: fed tones must come out "as if there was no filter". Since output is multiplied by
`gFilt`, fresh input would otherwise be attenuated at the output. So injection is
pre-divided by the filter gain:

- `comp[k] = 1 / max(kCompFloor, gFilt[k])`  (`kCompFloor = 0.05` prevents blow-up where
  the bell → 0).

Fresh input's output contribution is `feed·Xs·comp · lossDecay · gFilt = feed·Xs·lossDecay`,
independent of the filter — the requested constant in/out relationship. The held tail is
shaped at the output by the *current* `gFilt`, so moving the filter is audible, while `S`
itself is never lowered by the filter.

## Compress (per-tone level reshaping)
Applied to the held state `S` once per frame, before the main update (`processFrame`). It
reshapes each tone's level relative to a **pivot = the mean magnitude of the *active* bins**
(bins above `maxMag·1e-3`; the empty noise floor is excluded so the pivot doesn't collapse to
~0, and those bins are left untouched). Per active bin:
```
ratio = clamp(|S[k]| / mean, 0.01, 100)
S[k] *= ratio ^ (compress · kCompressRate)     // kCompressRate = 0.05
```
- `compress > 0` → exponent > 0 → bins above the mean get louder, below get quieter:
  **expansion / purify** (the dominant tones win).
- `compress < 0` → bins above the mean drop, below rise: **homogenise** (levels even out).
It's gentle per frame so it acts over time (like loss). The limiter and loss bound any runaway
of the loud bins. Verified by the offline `compress:` test (two tones; ratio widens vs narrows).

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
Linked across channels so the stereo image is preserved.
- `peak = max over channels of |sample|`.
- `limEnv` follows `peak` (one-pole): fast attack `kLimAttMs = 5 ms`, slow release
  `kLimRelMs = 1200 ms`.
- `target = (limEnv > 1) ? 1/limEnv : 1`. So gain is **1.0 until the signal would clip**.
- `limGain` eases toward `target` (fast when reducing, slow when recovering) and multiplies
  every channel. Result: transparent under -1..1, gentle slow pull-down above it.

## Reconfiguring FFT size at runtime
`setOrder()` is called from the message thread; it only stores `pendingOrder`. The audio
thread applies it in `applyPendingOrder()` **at a frame boundary**, under `displayLock`.
All buffers are preallocated at the **max** size in `prepare()`, so `configure()` never
allocates — it just recomputes window/rot tables and the FFT object. State is reset on
change (a clean re-freeze), and latency is re-reported.

## Constants worth knowing (top of SpectralEngine.cpp)
`kOverlap=4`, `kCompFloor=0.05`, `kDecayFloor=1e-4`, `kSigmaOct=1.25`. Limiter constants
are at the top of `PluginProcessor.cpp`.
