# Plan: roadmap batch — revFeed-done-right (main) + workflow/perform features (`exp/roadmap`)

Status: **PLAN — not implemented.** Written for a cheaper model to execute. Read
[architecture.md](architecture.md), [dsp-design.md](dsp-design.md), [parameters.md](parameters.md)
and [gotchas.md](gotchas.md) first. Line numbers below are as of commit `36ad37c`; re-locate by
the quoted code if they drifted.

## Ground rules (apply to every phase)

- **One phase = one commit.** Run `./build.sh` (build + `SpectralHoldTest`) before each commit.
- **Update the matching wiki page in the same commit** (each phase lists which).
- **Parameter IDs never change**; only `createLayout()` *order* may change (state restores by ID).
- `SpectralEngine` and `PlateReverb` must stay `juce_dsp`-only (the test target depends on it).
- Every new DSP behavior gets a `test_main.cpp` case, **including an `in == out` aliasing path**
  where the engine's `process` is involved (see gotchas.md "In-place processing aliasing").
- Defaults must preserve today's sound **bit-exact**: every new param's default is "off".

---

# Part A — on `main`: revFeed done right

Goal: reverb→hold feedback that actually works. The old `revFeed` failed because the wet was
re-injected through the normal input path, so the **Feed** knob scaled it to nothing (see
gotchas.md "revFeed was removed"). The fix is a **second, Feed-independent input path** into
`SpectralEngine`.

## A1. Engine: auxiliary input path

`Source/SpectralEngine.h/.cpp`.

1. **Params**: add `float revFeed = 0.0f; // 0..1 aux (reverb-wet) injection, Feed-independent`
   to `SpectralEngine::Params`.
2. **State**: add members
   - `std::vector<float> auxRing;` (assign `maxFftSize` zeros in `prepare`, refill in `reset`),
   - `std::vector<float> auxFftData;` (assign `2 * maxFftSize` zeros in `prepare`).
   The aux ring shares `inWrite` — aux and input advance in lockstep, so the aux frame is
   sample-aligned with the analysis frame by construction.
3. **process() signature**: change to
   `void process (const float* in, const float* aux, float* out, int numSamples, const Params& p);`
   where `aux` may be `nullptr` (treated as silence). Keep a 4-arg overload that forwards with
   `aux = nullptr` so `test_main.cpp`'s existing calls keep compiling unchanged.
   In the per-sample loop (currently `SpectralEngine.cpp:152-175`), next to
   `inRing[inWrite] = x;` write `auxRing[inWrite] = (aux != nullptr ? aux[n] : 0.0f);`
   **before** incrementing `inWrite`. (Latch `aux[n]` into a local like `x` — same aliasing rule.)
4. **processFrame(): aux analysis.** After the existing input FFT
   (`fft->performRealOnlyForwardTransform (fftData.data());`, line ~185), add — **only when
   `p.revFeed > 1.0e-4f`** so the default costs zero:
   ```
   for (int i = 0; i < fftSize; ++i)
       auxFftData[i] = auxRing[(inWrite + i) % fftSize] * window[i];
   fft->performRealOnlyForwardTransform (auxFftData.data());
   ```
   Keep a `const bool auxActive = p.revFeed > 1.0e-4f;` flag.
5. **Injection.** In the per-bin loop, next to the existing
   `const std::complex<float> inj = feed * x * comp;` (line ~277) compute
   ```
   std::complex<float> injAux {};
   if (auxActive)
   {
       const std::complex<float> xa { auxFftData[2*k], auxFftData[2*k + 1] };
       injAux = p.revFeed * injScale * xa * comp;
   }
   const float aInj = std::abs (inj + injAux);
   ```
   (`aInj` replaces the current `std::abs (inj)` — it drives the location pull and the
   `kInjLocFloor` gate; the routed/claimed-layer logic is unchanged.)
   Where the receiving layer currently does `sk += inj;` (line ~374), instead do:
   ```
   sk += inj;
   if (auxActive)
   {
       // feedback-loop safety: unlike live input, aux closes a real loop
       // (engine -> reverb -> engine); soft-ceiling it the same way the permanent
       // shaper's boost self-limits (kPermCeilNorm), so gain>1 loops saturate
       // instead of blowing up. Live input stays uncapped as before.
       const float permCeil = kPermCeilNorm * 0.5f * (float) fftSize;
       const float g = juce::jmax (0.0f, 1.0f - std::abs (sk) / permCeil);
       sk += injAux * g;
   }
   ```
6. **Frequency tracking is input-only.** Do **not** feed aux into `prevPhase`/`omega` tracking —
   aux energy inherits the receiving layer's omega. (The reverb wet is a smeared copy of the held
   tones at ~the same bins, so this is correct-enough and keeps the tracking clean; document it.)

## A2. Processor: wet tap + feedback buffer

`Source/PluginProcessor.h/.cpp`.

1. **Param**: `revFeed`, float 0..1, default 0, name `"Reverb Feed"`. **Append at the very end of
   `createLayout()`** (after `revMetal`) — it becomes host param index 24, first slot of a
   partial page 4. Part B regroups pages; on main this interim placement is accepted. Cache
   pointer `pRevFeed`.
2. **Members**: `std::vector<float> revFeedBuf;` — holds the **previous block's** wet mono.
   Size/zero it in `prepareToPlay` (like `revMono`). One-block feedback delay is inherent and fine.
3. **processBlock** changes (currently `PluginProcessor.cpp:287-327`):
   - Engines: pass the feedback tap:
     `eng.process (d, (pRevFeed->load() > 0.0f ? revFeedBuf.data() : nullptr), d, numSamples, p);`
     — guard `revFeedBuf.size() >= numSamples` (resize+zero if the host grew the block).
     Set `p.revFeed = pRevFeed->load();`.
   - Reverb gate: run the reverb when **`revMix > 0 || revFeed > 0`** (the feedback needs the wet
     even when the audible mix is 0). Bit-exact-dry hard bypass now requires *both* at 0; reset
     the tail when both fall to 0 (extend the `prevRevMix` logic with a `prevRevFeed`).
   - After `reverb.process(...)`, fill the tap: `revFeedBuf[n] = 0.5f * (revWetL[n] + revWetR[n]);`
     When the reverb is bypassed this block, zero `revFeedBuf`.
   - The wet **mix into the buffer** stays gated on `revMix > 0` exactly as today (at
     `revMix = 0, revFeed > 0` the reverb runs silently, only feeding the engines).
4. **GUI** (`PluginEditor.cpp`): Reverb tab gets a 7th knob `revFeed`, label **"Feed"** — same
   trailing-integration-knob pattern as the shaper's Feed. Add to `setupKnob`, both
   `reverbKnobs[]` arrays (`setActiveTab` and `resized`, counts 6→7), and an info line:
   `"Feeds the reverb tail back into the held spectrum. Independent of the main Feed."`

## A3. Tests (`Source/test_main.cpp`)

- **aux injects at feed=0**: engine, `feed=0, loss=0.2, revFeed=0.6`; push a 440 Hz sine via the
  **aux** pointer only (`in` = silence) for ~50 frames; assert output RMS grows > threshold
  (aux path works without Feed) and is finite.
- **feedback stability**: closed loop simulated: `loss=0, revFeed=1`, drive input 20 frames, then
  input silent, keep feeding the engine's own output (scaled ×1.0) back as aux for 2000 frames;
  assert every output sample finite and |output| bounded (the soft ceiling holds).
- **default is bit-exact**: `revFeed=0` run == 4-arg overload run, sample-identical.
- **in==out aliasing** with aux non-null.

## A4. Wiki (same commit)

- `parameters.md`: `revFeed` row + note page 4 is partial on main.
- `dsp-design.md`: short "Aux input path (reverb feed)" section: alignment via shared `inWrite`,
  injScale reuse, soft ceiling, tracking-is-input-only.
- `gotchas.md`: **rewrite** the "revFeed was removed" bullet → "revFeed exists again, done right:
  second Feed-independent path (`aux` arg); the old input-path revival is still forbidden."
  Note the new hard-bypass condition (both `revMix` and `revFeed` at 0).

---

# Part B — branch `exp/roadmap` (branched off main **after** Part A)

Phases ordered by dependency. B1 is the foundation (B6 and B9 reuse it). Within a phase, order
is prescriptive.

## B0. Final page grouping

Final `createLayout()` order (IDs unchanged, order only):

- **P1 "Hold"**: feed, loss, ewLocation, dryWet, output, limThreshold, limRelease
- **P2 "Shaper"**: shapeAmt, shape, shapeFreq, shapeWidth, shapeCount, shapeLevel, shapeMode, harmonize
- **P3 "Space"**: harmWidth, harmonic, revMix, revDecay, revDamp, revSize, revPredelay, revMetal
- **P4 "Perform"**: transpose, spread, phaseNoiseAmt, revFeed *(+ morph if B11 lands)* — partial page, accepted

`phaseNoise` (bool) leaves P1 (replaced by `phaseNoiseAmt` on P4, see B4). P1 stays a
7-slot page — no 8th control (see B2, cut).

## B1. Hold serialization ("the held sound survives save/reopen")

The whole product is the held state; losing it on session reload is the worst gap.

1. **Engine write** (`SpectralEngine`): public
   `void writeHold (juce::MemoryOutputStream& out) const;`
   Format (little-endian): `int32 version=1, int32 order, int32 kNumLayers, int32 numBins`, then
   per layer `l` in 0..kNumLayers-1: `S[li(l,0..numBins-1)]` as 2×float, then `omega[...]`
   (numBins floats), then `binLoc[...]` (numBins floats). Write only `numBins`, not `maxBins`.
   **Caller must hold the processor's callback lock** (document in the header comment) — the
   engine itself takes no lock here.
2. **Engine restore**: public `void queueHoldRestore (const void* data, size_t size);`
   Copies into a pending `juce::MemoryBlock pendingHold` under a new `holdLock`
   (`juce::CriticalSection`, same pattern as `brushLock`) + `std::atomic<bool> holdPending`.
   On the audio thread, at the frame boundary in `process()` right after `applyPendingOrder()`:
   try-lock `holdLock`; if pending: parse header; **if blob order == current `order`**, memcpy
   the three arrays layer-by-layer (stride `maxBins`!) and clear pending; if order differs and
   `pendingOrder` is still queued, keep it pending (the order switch lands first next frame);
   if order differs and no order change is queued, discard. Malformed/short blob: discard.
3. **Processor save** (`getStateInformation`): under
   `const juce::ScopedLock sl (getCallbackLock());`, for each engine: `writeHold` into a
   `MemoryOutputStream`, GZIP it (`juce::GZIPCompressorOutputStream`), base64
   (`juce::Base64::toBase64`) → state property `"hold0"` / `"hold1"`. Also property
   `"keepSound"` (bool). Only write the hold blobs when `keepSound` is true.
4. **Processor restore** (`setStateInformation`): **after** the existing `setFftOrder(ord)` call
   (order restore must be queued first): decode base64 → GZIP-decompress → `queueHoldRestore`
   per engine. Missing properties = old session = no-op.
5. **GUI toggle**: `keepSound` GUI-only bool (like `liveMode`; getter/setter + state property),
   default **true**. Utility-row toggle "Keep", info:
   `"Save the held sound inside the session, so it's still ringing when the project reopens."`
   Note in the wiki: ~100–500 KB per session when on.
6. **Tests**: roundtrip offline — run engine on a sine 50 frames, `writeHold`; `reset()` (output
   decays to silence — assert); `queueHoldRestore` the blob; process one hop of silence with
   `feed=0, loss=0`; assert output RMS recovers > threshold and finite. Also: blob with wrong
   order → discarded, engine keeps running clean.
7. **Wiki**: parameters.md (Keep toggle in the GUI-only rows), architecture.md (state format),
   gotchas.md (delete the "*Save sound* was removed / held state is no longer serialised" claims
   — there are two, in the E-W section and parameters.md Notes; this plan supersedes them).

## B2. Freeze — CUT

Implemented, then removed by the user: a dedicated "stop time" button doesn't earn its
slot next to `feed=0` (which already fully halts injection and tracking) — the only extra
it bought was pinning `decayL=1` too, not worth a whole P1 param/button for. Reverted in
full (param, engine branch, GUI button, tests, wiki rows); P1 stays 7 slots (see B0). Do
not resurrect this without a concrete request — if "stop decay independent of feed" comes
up again, prefer a cheap `loss=0` toggle over a new param.

## B3. Transpose + MIDI

Pitch-shift the *held* sound, non-destructively. The phasors free-run, so this is cheap.

1. **Param**: `transpose`, float −12..+12 semitones, default 0, P4 slot 1. Textbox suffix `" st"`.
2. **Engine** (`Params`: `float transpose = 0.0f;`):
   - New layered state `std::vector<float> transAcc;` (`kNumLayers*maxBins`, zeroed in
     prepare/reset) — the accumulated *extra* phase of the transposed output, and a new complex
     scratch `std::vector<std::complex<float>> synthScratch;` (`maxBins`).
   - In `processFrame`, `const float ratio = std::exp2 (p.transpose / 12.0f);`
     `const bool transposing = std::abs (p.transpose) > 1.0e-3f;`
   - **Non-transposing path unchanged** (bit-exact): keep writing `outBin` straight into
     `fftData[2k], fftData[2k+1]` as today.
   - Transposing path: zero `synthScratch[0..numBins)` at frame start. Per layer per bin, the
     output contribution becomes `sk * attL * polar(1, transAcc[idx])` where each frame
     `transAcc[idx] += omega[idx] * (ratio - 1.0f);` wrapped to `[-pi, pi]`
     (`a -= twoPi * round(a / twoPi)`). Accumulate the per-bin layer mix as today, apply `gOut`,
     then add into `synthScratch[k']` with `k' = (int) lround(k * ratio)`; drop `k' <= 0` or
     `k' >= numBins`. After the bin loop, write `synthScratch` into `fftData` (and into
     `dispScratch` so the display shows the transposed output).
   - Why this pitches correctly: perceived frequency = phase advance *rate*; the stored `S`
     still advances at `omega` (state untouched → non-destructive), and `transAcc` adds
     `omega·(r−1)` per hop, so the output bin advances at `omega·r`. Bin rounding error ≤ 0.5
     bin < the ±binW/2 coherence budget at 75 % overlap.
   - Update `transAcc` **only when transposing** (it holds a phase offset otherwise; harmless),
     and reset it in `reset()`.
3. **MIDI**: CMake `NEEDS_MIDI_INPUT TRUE`; `acceptsMidi() → true`. In `processBlock`, scan the
   `MidiBuffer` (rename the ignored arg): last note-on sets `int midiNote`; a note-off matching
   the current note clears it (−1). Effective semitones =
   `knob + (midiNote >= 0 ? midiNote - 60 : 0)` — monophonic, last-note priority, note 60 = as-is.
   Store `midiNote` as a plain member (audio thread only).
4. **Test**: hold a 440 Hz sine (50 frames), then `feed=0, transpose=+12`, render ~1 s, measure
   the dominant frequency of the last 4096 output samples (zero-crossing count or a small DFT
   probe at 880 vs 440) → expect ≈ 880 Hz within 3 %. Also `transpose=0` bit-exact vs before.
5. **Wiki**: parameters.md row (incl. MIDI note behaviour), dsp-design.md § "Transpose"
   (the transAcc math above), build-and-test.md if the probe helper is added.

## B4. Phase Noise becomes continuous

1. **Param**: new `phaseNoiseAmt`, float 0..1, default 0, P4 slot 3 (name "Phase Noise").
   **Remove** the old bool `phaseNoise` from the layout. Engine `Params.phaseNoise` becomes
   `float` (0..1); jitter = `w += (rng.nextFloat()*2-1) * kPhaseNoiseMax * p.phaseNoise;` with
   `constexpr float kPhaseNoiseMax = 0.5f;` — the legacy "on" (0.15 rad) maps to **amt = 0.3**.
2. **Migration** in `setStateInformation`, after `replaceState`: if the incoming tree has no
   `phaseNoiseAmt` PARAM child but has an old `phaseNoise` child with value > 0.5, call
   `apvts.getParameter("phaseNoiseAmt")->setValueNotifyingHost(0.3f normalised)` (0.3 of a 0..1
   range = 0.3). Old automation of the bool id is lost — accepted, note it in the wiki.
3. **GUI**: replace `noiseButton` (ToggleButton) with a small Knob in the utility row; update the
   info text (`"Shimmer: random per-frame phase jitter. Never detunes permanently."`).
4. **Test**: amt=0 bit-exact vs today; amt=1 output differs and stays finite.
5. **Wiki**: parameters.md (row + note the id swap/migration), gotchas.md short bullet.

## B5. Stereo spread — DONE

Per-bin complementary channel gains on the *output* (momentary, never enters `S`). Implemented
as specified below, with one addition: no GUI widget (the plan didn't list one, and the
utility row was already full with Transpose + Phase Noise) — `spread` is host-automatable /
generic-editor only for now.

1. **Param**: `spread`, float 0..1, default 0, P4 slot 2.
2. **Engine**: `Params`: `float spread = 0.0f; int spreadSign = +1;` (processor sets `+1` for
   ch 0, `−1` for ch 1, and forces `spread = 0` when the bus is mono).
   Per bin, deterministic hash `h(k)` in [−1, +1]:
   `uint32_t u = (uint32_t) k * 2654435761u; h = ((u >> 16) & 0xFFFF) / 32767.5f - 1.0f;`
   Gain `g = std::sqrt (1.0f + (float) p.spreadSign * p.spread * h);` (argument stays in [0,2] →
   gL²+gR²=2, equal-power). Apply to the final `outBin` (after `gOut`), skip entirely when
   `p.spread <= 1e-4` (bit-exact default).
3. **Test**: two engines, same input, signs ±1, spread=1 → outputs differ; per-sample
   `L²+R²` ≈ 2× the spread=0 single-channel power within 1 dB tolerance over an RMS window;
   spread=0 bit-exact.
4. **Wiki**: parameters.md row, dsp-design.md two lines.

## B6. Undo for destructive edits — DONE

GUI-only; reuses B1's blob machinery. Implemented as specified; the width budget for the
utility row (see B5's note) is now genuinely tight — see the comment in
`PluginEditor::resized()` before adding another control there.

1. **Processor**: `void snapshotHold();` — under `getCallbackLock()`, `writeHold` both engines
   into a ring `std::array<std::pair<juce::MemoryBlock, juce::MemoryBlock>, 4>` (depth 4) +
   write index. `bool undoHold();` — pop the newest snapshot, `queueHoldRestore` to both
   engines; returns false when empty.
2. **Triggers** (message thread):
   - `SpectrumDisplay::mouseDown` before the first `queueBrush` of a stroke → `proc.snapshotHold()`.
   - Permanent-shaper engage: in the editor, listen to `shapeMode` and `shapeLevel`
     (`APVTS::Listener`); snapshot on the transition into "armed permanent"
     (`shapeMode > 0.01 && |shapeLevel| > 0.01` becoming true from false). One snapshot per
     engagement, not per knob tick.
3. **GUI**: "Undo" TextButton in the utility row. Info:
   `"Revert the held sound to before the last brush stroke or permanent-shaper engagement."`
4. **Not undoable** (document): harmonize drift, loss decay, normal feeding. This is edit-undo,
   not time-travel.
5. **Test**: engine-level roundtrip is already covered by B1; add: snapshot → brush cut −1 →
   restore → spectra match pre-brush (compare `copyDisplay` mags within tolerance).
6. **Wiki**: gui-display.md (brush section), parameters.md notes.

## B7. Display: stereo + location strip — DONE, later revised by plan-uifix.md U1/U2

Implemented as specified, plus one addition not in the original bullets: since new DSP
behavior needs a `test_main.cpp` case per the ground rules at the top of this file,
`copyLayers` got one (two tones at different E-W locations show up as two populated
layers). The location strip is channel 0 only (not merged across channels like the main
view, which the plan didn't ask for) — see gotchas.md.

**Superseded**: the stereo `max(mag0, mag1)` merge below (step 1) made the held tone look
"ribbed" whenever the channels differed — replaced by a split L-up/R-down view in
agent-wiki/plan-uifix.md U1. The location strip's `ewLocation` marker line (part of step 2)
was found redundant and replaced with live recording/steal ticks in U2. The bullets below
describe what was originally built, not the current behavior — see
[gui-display.md](gui-display.md) for that.

1. **Stereo**: `getDisplaySnapshot` (`PluginProcessor.cpp:352`) — copy ch 0 as today, then
   `engines[1].copyDisplay` into scratch members; if both succeed, `mag[k] = max(mag0, mag1)`
   (phase stays ch 0). Mono bus / busy ch 1: fall back to ch 0 alone.
2. **Location strip**: the E–W field is invisible today; show it.
   - Engine: snapshot per-layer data in the existing try-lock block of `processFrame`
     (line ~396): new guarded `dispLayerMag`, `dispLayerLoc` (`kNumLayers*maxBins`, filled with
     `abs(S[li]) * norm` and `binLoc[li]`). New
     `int copyLayers (std::vector<float>& mag, std::vector<float>& loc)` (try-lock, returns
     `numBins` or 0; layout: layer-major, stride numBins).
   - Processor: forwarder `getLocationSnapshot(...)` (channel 0).
   - GUI (`SpectrumDisplay`): a ~40 px strip at the bottom of the display area (carve it inside
     the component so the log-frequency x-mapping is shared). For each layer/bin with
     mag > 1e-4: point at (x = freq mapping, y = loc: East at bottom), alpha ∝ sqrt(mag), same
     hue scheme as the main view. Draw a thin horizontal marker line at the `ewLocation` knob
     value (the listener). Repaint with the existing display timer.
3. **Wiki**: gui-display.md (strip section), architecture.md snapshot list.

## B8. Resizable editor — DONE

Implemented as specified. One clarification worth flagging for future phases (B9's ComboBox
etc.): this is a uniform visual scale (`AffineTransform`) of the same fixed 860-unit layout,
**not** a reflow — the utility row's width budget (see its comment in `layoutContent()`) is
exactly as tight at any window size. If a future phase wants more room in that row, resizing
doesn't buy any; a P4 tab (mentioned as the alternative since B5) is the actual fix.

1. Wrap all current children in a `content` component sized fixed **860×580** (move every
   `addAndMakeVisible` target into it; the editor's `resized()` becomes:
   `content.setTransform (AffineTransform::scale ((float) getWidth() / 860.0f));`
   `content.setBounds (0, 0, 860, 580);` — the transform scales knobs, fonts, display, all).
   The existing layout code moves into `content`'s `resized()` **unchanged**.
2. Editor ctor: `setResizable (true, true); setResizeLimits (645, 435, 1720, 1160);`
   `getConstrainer()->setFixedAspectRatio (860.0 / 580.0);`
3. Persist: GUI-only `editorScale` (float) via processor getter/setter + state property (same
   pattern as `liveMode`); editor ctor restores `setSize (860*s, 580*s)`, `resized()` stores it.
4. Check the mouse-driven display brush still maps correctly (it will if the brush math uses
   `getLocalBounds()` of the display inside the transformed content — verify in the standalone).
5. **Wiki**: gui-display.md note.

## B9. Presets — DONE (infrastructure only, curation still pending)

Implemented as specified, with the preset ComboBox placed at the right end of the **tab
strip** rather than the utility row — that row's width budget was already spent (see B5/B6's
notes). One structural addition not spelled out in the bullets below: "any manual knob
change deselects" requires listening to every parameter, so the editor's existing
`parameterChanged` (previously just `shapeMode`/`shapeLevel` for B6's undo trigger) is now
registered for **all** APVTS params via a loop over `proc.apvts.state`, and reused for both
features — see gotchas.md. Curation later **by ear — the shipped values are placeholders the
user must tune; say so in the code comment**.

1. `Source/Presets.h`: `struct Preset { const char* name; std::initializer_list<std::pair<const char*, float>> values; }`
   (values in **plain/denormalised** units, applied via
   `param->convertTo0to1(v)` + `setValueNotifyingHost`). Unlisted params reset to default first.
2. ~8 placeholders: Init (all defaults), Infinite Pad (loss 0, feed 0.3), Shimmer Freeze
   (phaseNoiseAmt 0.4, revMix 0.35), Cathedral (revMix 0.5, revDecay 0.85, revSize 1.6),
   Harmonic Drift (harmonize 0.05, harmonic 1.0), Dark Comb (shape 2, shapeLevel −0.6, revDamp 0.7),
   Metallic Room (revMix 0.4, revMetal 0.8), Feedback Bloom (revFeed 0.5, revMix 0.3, loss 0.1).
3. GUI: ComboBox "Preset" in the utility row; selecting applies; any manual knob change
   deselects (set text to "—"). Presets do **not** touch the held state, FT size, or GUI-only
   toggles.
4. No host program API (`getNumPrograms` stays 1) — this is a GUI convenience.
5. **Wiki**: parameters.md notes + a "presets are uncurated" flag in gotchas.md until tuned.

## B10. CI + CLAP — DONE

CI (part 1) turned out to already exist — `.github/workflows/build.yml` predates this plan
entry entirely (commits `5729849`/`cc385b3`, both older than this file); the plan was
written without checking. It already matched the spec below almost exactly (Linux/macOS/
Windows matrix, JUCE 8.0.12 pinned, deps installed, test run on every platform, not just
Linux). CLAP (part 2) is new: implemented per the spec, with one real deviation — the plan's
implied "just FetchContent the latest release" doesn't work against JUCE 8 (see
build-and-test.md's CLAP section for the exact incompatibility and why `main` is pinned
instead of a tag). Verified building locally (`cmake --build build --target
SpectralHold_CLAP`, `SPECTRALHOLD_CLAP=ON`) and via the exact CI-equivalent commands
(separate build dir + explicit `-DJUCE_PATH`). No Linux CLAP host was available to load-test
it (build-only, per the plan's own fallback clause).

1. `.github/workflows/build.yml`: job `linux`: checkout repo, checkout `juce-framework/JUCE`
   into a sibling dir (**pin the same JUCE major as `../JUCE` — check
   `../JUCE/modules/juce_core/system/juce_StandardHeader.h` for `JUCE_MAJOR_VERSION` before
   writing the workflow**), apt-get the JUCE Linux deps (`libasound2-dev libx11-dev
   libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libfreetype6-dev
   libfontconfig1-dev libcurl4-openssl-dev`), `cmake -DJUCE_PATH=... && cmake --build`, run
   `SpectralHoldTest`. Optional `macos`/`windows` build-only jobs (no test run needed there).
2. CLAP: `clap-juce-extensions` via `FetchContent`, then
   `clap_juce_extensions_plugin(TARGET SpectralHold CLAP_ID "com.autotel.spectralhold" CLAP_FEATURES audio-effect)`.
   Guard behind `option(SPECTRALHOLD_CLAP "Build CLAP" ON)` so a network-less local build can
   turn it off. Verify it loads in a Linux CLAP host if available; otherwise build-only.
3. **Wiki**: build-and-test.md.

## B11. Optional / cut-line (do last, skip if effort overruns)

- **Morph A/B**: two capture slots (B1 blobs held in the engine as `SA/SB` arrays) + a `morph`
  param (P4 slot 5) crossfading per-bin magnitude (linear) and omega (lerp). Naive per-bin morph
  — experimental by nature; gate behind everything else working.
- **Cross-layer harmonize**: single `applyHarmonize` pass over the union of peaks from all
  layers (peaks tagged with their layer), pair influence additionally weighted by
  `ewAtt(|loc_i − loc_j|)` so far-apart tones don't entrain; drift applied back into each peak's
  own layer. Only do it if the current per-layer isolation is audibly missed (gotchas.md flags
  it as a known limitation, not a bug).

## Final wiki sweep (last commit on the branch)

- parameters.md: full new page table (P1–P4 as in B0), all new rows.
- README.md: add this plan to the index; one-line description of the branch.
- gotchas.md: new decisions (freeze semantics, aux path safety ceiling, keepSound size,
  placeholder presets).
