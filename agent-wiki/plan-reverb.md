# PLAN — Output Reverb (branch `exp/reverb`)

**Status: NOT implemented — this is the plan.** Implement on a new branch `exp/reverb`
created from `exp/spectral-shaper` (the current work line; the reverb sits after the
shaper's output stage). When done, flip this banner, update the wiki pages listed in §8,
and make sure `./build.sh` is green.

## 1. What the feature is

A stereo **plate reverb applied to the plugin output**, with a wet/dry Mix knob and a
small set of character controls. Purpose: add space/variation to the held sound. It is a
*post* effect — it never feeds back into the spectral engines.

Algorithm: the **Dattorro plate** (Jon Dattorro, *"Effect Design, Part 1: Reverberator
and Other Filters"*, JAES 1997). Chosen over alternatives:
- `juce::Reverb` (Freeverb): cheap to wire but metallic on long decays — poor on drones.
- Convolution: fixed character, no decay/size control, needs IR assets.
- Generic FDN: flexible but requires ear-tuning of lengths/matrix; the Dattorro topology
  ships with published, known-good constants and a modulated tank that stays smooth on
  exactly the kind of static, sustained material this plugin produces.

## 2. Signal flow placement

```
input → engines (per channel) → output gain → [ PLATE REVERB ] → linked limiter → output
```

- **After** the `output` gain: the reverb tail follows fader rides like a post-fader send.
- **Before** the limiter: the limiter keeps protecting ±1 even with wet boost.
- Reverb input is the **mono sum** of the (post-gain) channels — canonical Dattorro; the
  tank's output taps produce a decorrelated stereo pair. With a mono bus, output the L tap
  set only.
- When `revMix == 0` (the default) **bypass entirely** (skip processing, and `reset()` the
  reverb once on the 1→0 transition so no stale tail plays when mix comes back up).

## 3. Parameters (APVTS, prefix `rev`)

| id | GUI | Range | Default | Meaning |
|----|-----|-------|---------|---------|
| `revMix`      | Mix      | 0..1 | 0.0 | Equal-power wet/dry: `dry·cos(m·π/2) + wet·sin(m·π/2)`. 0 = exact dry bypass. |
| `revDecay`    | Decay    | 0..1 | 0.5 | Tank feedback: `decayGain = jmap(k, 0.25, 0.98)`. Also sets `decayDiffusion2 = clamp(decayGain + 0.15, 0.25, 0.50)` (per the paper). |
| `revSize`     | Size     | 0.5..2.0, skew so 1.0 is centred | 1.0 | Scales **all tank delay lengths** (not the input diffusers). Fractional-delay reads + a smoothed scale factor (~50 ms one-pole) → moving it gives a gentle tape-style pitch bend, which is fine/musical. |
| `revDamp`     | Damp     | 0..1 | 0.3 | One-pole LP coefficient inside the tank (`lp += (x - lp) * (1 - damp)` form): 0 = bright, 1 = very dark. |
| `revPredelay` | Predelay | 0..250 ms, log-ish skew | 20 ms | Delay line on the wet path input, before the input diffusers. |

Five knobs total. Defaults chosen so **old sessions are unchanged** (`revMix = 0` → bit-exact
dry). Internal constants, *not* exposed: input bandwidth LP = 0.9995, input diffusion
gains 0.750/0.750/0.625/0.625, decay diffusion 1 = 0.70, LFO rates 0.50 Hz / 0.61 Hz,
excursion 16 samples (at reference rate).

## 4. The Dattorro topology — exact structure

All delay lengths below are **samples at the reference rate 29761 Hz**; at runtime scale
every length (and the excursion) by `sr / 29761.0` and round. Tank lengths are
additionally scaled by the smoothed `revSize` (fractional read). Preallocate every buffer
at `prepare()` for the worst case (`size = 2.0` at the current sample rate, + excursion
margin) — no audio-thread allocation, same rule as the engines.

**Allpass building block** (standard Schroeder form, used everywhere):
```
v              = x - g * buf[read]
y              = buf[read] + g * v
buf[write]     = v
```

**Input chain (mono):** predelay → bandwidth LP (one-pole, coef 0.9995) → 4 series
allpasses: lengths **142, 107, 379, 277**, gains **0.750, 0.750, 0.625, 0.625**.

**Tank — figure-8, two cross-coupled halves.** Each sample, the diffused input `u` is
injected into *both* halves; each half's end feeds the other's start.

Half A (input: `u + decayGain · outB`):
1. Modulated allpass, length **672** ± excursion 16 (LFO 1), gain **−0.70** (decay diffusion 1; note the sign — use `g = -0.70` in the block above)
2. Delay **4453**
3. Damping LP (coef from `revDamp`), then × `decayGain`
4. Allpass, length **1800**, gain **+0.50** (decay diffusion 2, value from §3)
5. Delay **3720** → × `decayGain` → this is `outA`, fed to half B

Half B (input: `u + decayGain · outA`): same chain with lengths **908** (± excursion 16,
LFO 2), **4217**, **2656**, **3163**.

**Output taps** (read at fixed offsets *into* the named buffers; offsets scale with sr
and size like the lengths; gain 0.6 each):
```
L = 0.6·delay4217[266] + 0.6·delay4217[2974] − 0.6·ap2656[1913]
  + 0.6·delay3163[1996] − 0.6·delay4453[1990] − 0.6·ap1800[187] − 0.6·delay3720[1066]

R = 0.6·delay4453[353] + 0.6·delay4453[3627] − 0.6·ap1800[1228]
  + 0.6·delay3720[2673] − 0.6·delay4217[2111] − 0.6·ap2656[335] − 0.6·delay3163[121]
```
(Tap constants are from the Dattorro paper's Table 2; if any tap offset exceeds a
size-scaled buffer length, clamp to length − 1.)

**LFOs:** two sines (0.50 Hz, 0.61 Hz — deliberately detuned) modulating the two tank
allpass read positions by ±excursion, with linear interpolation on the fractional read.
This is what keeps the tail from ringing metallically — do not omit it.

## 5. Code changes by file

1. **`Source/PlateReverb.{h,cpp}` (new)** — self-contained class, no JUCE plugin deps
   (fine to use `juce_dsp`/`juce_core` utilities only, same rule as `SpectralEngine`):
   - `void prepare (double sampleRate)` — allocate worst-case buffers, reset state.
   - `void reset()` — clear buffers/LP states/LFO phases.
   - `void setParams (float decay, float size, float damp, float predelaySec)` — cheap,
     called per block; `size` smoothing lives inside (per-sample one-pole).
   - `void process (const float* inMono, float* outL, float* outR, int n)` — wet only;
     the processor does the dry/wet mix so `mix=0` stays bit-exact dry.
2. **`PluginProcessor.{h,cpp}`** —
   - `createLayout()`: add the 5 `rev*` parameters (§3); cache raw-value pointers.
   - Member `PlateReverb reverb;` + a small mono scratch buffer + wet L/R scratch
     buffers (allocated in `prepareToPlay`).
   - `processBlock`: after the output-gain block and before the limiter — if
     `revMix > 0`: mono-sum → `reverb.process` → equal-power mix into the channels.
     Track previous mix for the bypass `reset()` (§2).
   - `getTailLengthSeconds()`: return `10.0` when `revMix > 0`, else keep `0.0` (hosts
     use this to keep processing after input stops — the frozen-hold behaviour already
     ignores it, so a coarse constant is fine).
3. **`PluginEditor.{h,cpp}`** — third knob row: **Mix, Decay, Size, Damp, Predelay**
   (same rotary style + APVTS attachments as the existing rows). Grow the editor height
   by one row; keep the spectrum display size unchanged.
4. **`CMakeLists.txt`** — add `PlateReverb.cpp` to the plugin target **and** to
   `SpectralHoldTest`.
5. **`test_main.cpp`** — §6.
6. **Wiki** — §8.

## 6. Tests (offline, `SpectralHoldTest` — test `PlateReverb` directly, no engine needed)

1. **Dry bypass exact:** `mix = 0` path in the processor is a skip, so at the unit level:
   feed an impulse, assert the *wet* output is nonzero (sanity) — and in a
   processor-level check if convenient, `revMix = 0` output == input bit-exact.
2. **Tail exists & decays:** impulse → wet output; RMS over [1.0 s, 1.5 s] is nonzero and
   RMS over [4 s, 4.5 s] is smaller (decay = 0.5, size = 1).
3. **Decay knob is monotonic:** RMS at t = 2 s with decay = 0.8 > with decay = 0.3.
4. **Stability at extremes:** decay = 1.0 (→ 0.98), size = 2.0 — feed 2 s of noise, then
   run 30 s of silence: every sample finite, |sample| bounded (< 10, say), no NaN/inf.
5. **Stereo decorrelation:** impulse tail, normalized cross-correlation of L vs R over
   the first 2 s < 0.9.
6. **Damping darkens:** compare tail HF energy (e.g. energy of the first-difference
   signal, a crude HF proxy) at damp = 0.1 vs damp = 0.9 — high damp must have less.
7. **Size change doesn't explode:** while feeding noise, sweep size 0.5 → 2.0 over 1 s —
   output stays finite/bounded (validates the smoothed fractional reads).

## 7. Suggested implementation order (one step = one buildable state)

1. `PlateReverb.{h,cpp}` + tests 2–7 wired into `test_main.cpp`, iterate until green.
2. Processor: params, wiring, bypass, tail length. Build + all tests.
3. Editor row 3.
4. Wiki pass + flip this file's status banner.
5. Listening check in the Standalone build (long-held drone, mix ≈ 0.4, decay ≈ 0.7):
   the tail must be smooth and non-metallic; if it rings, first suspect a dropped LFO
   or a wrong allpass sign.

## 8. Wiki updates (same task, per house rule)

- `parameters.md` — add the 5 `rev*` rows + knob-row layout note.
- `architecture.md` — signal-flow diagram gains the reverb stage; file list gains
  `PlateReverb.{h,cpp}`; note it is cross-channel (lives in the processor, like the
  limiter, *not* per-engine).
- `gotchas.md` — note the `mix=0` hard-bypass + reset behaviour and the size-knob
  pitch-bend-by-design.
- `README.md` — add this plan to the wiki index.
