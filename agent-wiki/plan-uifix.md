# Plan: UI/display fix batch (`exp/uifix`) — split-stereo view, strip content, Perform tab

Status: **DONE.** Implemented in full (U0–U4) on `exp/uifix`. Line numbers below are as of
the tree at plan-writing time (post B4–B10, pre-U1); re-locate by the quoted code if they
drifted — most did, since U1/U2/U3 each touched the same files in sequence. Current behavior
is documented in [gui-display.md](gui-display.md), [parameters.md](parameters.md),
[dsp-design.md](dsp-design.md) and [gotchas.md](gotchas.md); read this plan for the
design rationale (the "ribbed" diagnosis, why the strip's marker line was cut, the
setOrder()-defers-to-next-frame gotcha the U2 tests had to route around).

User-reported problems this plan fixes, verbatim intent:

1. The held tone looks **"ribbed"** since the stereo-display change; it should look similar
   to how it used to. Stereo should render as **left channel projecting up from centre,
   right channel projecting down from centre**.
2. The horizontal line in the location strip that tracks the E–W knob is **redundant**
   (the knob already shows its own value). Replace it with actual content: the state of
   **recording / tone-stealing** — or nothing.
3. Transpose is a tiny utility-row slider but has a big sonic impact → make it a **knob**,
   and add a **discrete/continuous toggle** for the transposition. Since transpose has MIDI
   input (B3: `midiNote` sets an integer semitone offset on note-on), also add a **glide
   time** knob — a note-on should be able to portamento into its new pitch instead of
   jumping, and the same smoothing should apply to knob moves so Snap doesn't zipper either.
4. Phase Noise (shimmer) also gets a proper **knob**.
5. When adding knobs, keep the host paging (8 params per page) coherent — modules stay
   together on their page.

## Ground rules (same as every plan)

- **One phase = one commit.** Run `./build.sh` (build + `SpectralHoldTest`) before each commit.
- **Update the matching wiki page in the same commit** (each phase lists which).
- **Parameter IDs never change**; only `createLayout()` *order* may change (state restores by ID).
- `SpectralEngine` stays `juce_dsp`-only (the test target depends on it).
- Defaults preserve today's sound **bit-exact**: `transposeSnap` defaults off (today's
  continuous behaviour) and `transposeGlide` defaults to 0 ms (instant, today's behaviour).
- **No GUI screenshots** — they don't work in this environment (see build-and-test.md).
  Verify layout by summing widths against the container rect; ask the user to eyeball it.

## U0. Branch setup — DONE

The `exp/roadmap` working tree currently holds ALL of the B4–B10 roadmap work uncommitted.
First commit that as-is on `exp/roadmap`:

    feat: roadmap B4-B10 - phaseNoiseAmt, spread, undo, stereo display, resizable editor, presets, CLAP+CI

(plus the B2 freeze revert — mention it in the body). Then:

    git checkout -b exp/uifix

---

## U1. Split-stereo display (fixes "ribbed") — DONE

**Diagnosis** (document it in gui-display.md): B7 merged channels per bin with
`mag[k] = max(mag0[k], mag1[k])` (`PluginProcessor.cpp:430-436`). Whenever the two channels
differ per bin — real stereo input, Phase Noise (each engine's `rng` is independently
seeded), or Spread (per-bin ± hash gains, which are inside `dispScratch` since B5) — the
per-bin max jumps between two different leakage curves and the smooth tone envelope turns
jagged/"ribbed". The fix is to never merge: show each channel in its own half.

1. **Processor** (`PluginProcessor.h/.cpp`): change
   `int getDisplaySnapshot (std::vector<float>& mag, std::vector<float>& phase, double& sr, int& size)`
   to
   `int getDisplaySnapshot (std::vector<float>& magL, std::vector<float>& phaseL, std::vector<float>& magR, std::vector<float>& phaseR, double& sr, int& size)`.
   - Fill L from `engines[0].copyDisplay` exactly as today (return 0 if busy, unchanged).
   - If `getMainBusNumOutputChannels() > 1` and `engines[1].copyDisplay (magR, phaseR) == n0`,
     keep it; **otherwise copy L into R** (mono bus, busy, or size mismatch → symmetric view).
   - Delete the `jmax` merge loop.
2. **SpectrumDisplay** (`SpectrumDisplay.h/.cpp`):
   - Members: `mag, phase` become `magL, phaseL, magR, phaseR`; `smoothMag` becomes
     `smoothMagL, smoothMagR` (same attack/release smoothing loop, run on both,
     `timerCallback` lines ~107-116).
   - **Paint** (the per-column loop, `SpectrumDisplay.cpp:156-184`): compute `mL`/`brightL`
     from `smoothMagL` and `mR`/`brightR` from `smoothMagR` (same interpolation). Skip the
     column only when **both** are ≤ 0.01. Per-channel hue from that channel's own phase.
     Replace the single symmetric gradient with one gradient, four stops — **left channel
     up from centre, right channel down from centre**:
     ```
     juce::ColourGradient grad (juce::Colours::transparentBlack, x, 0.0f,
                                juce::Colours::transparentBlack, x, (float) H, false);
     grad.addColour (0.28,  baseL.withAlpha (brightL * 0.35f));
     grad.addColour (0.49,  baseL.withAlpha (brightL));
     grad.addColour (0.51,  baseR.withAlpha (brightR));
     grad.addColour (0.72,  baseR.withAlpha (brightR * 0.35f));
     ```
     With identical channels this renders the same symmetric spike as the old view — "looks
     similar to how it used to" — and honest stereo separation when they differ.
   - `dispScratch` stays post-spread in the engine (don't "fix" it): with per-channel halves
     the spread ribs are honest information (bins alternate up/down between halves = width).
     The misleading part was only the max-merge. Document this decision.
3. **Test**: none possible at the engine level (display plumbing only; `copyDisplay` itself
   is unchanged). `./build.sh` must stay all-green.
4. **Wiki**: gui-display.md — rewrite the stereo paragraph of the B7 section (diagnosis +
   split-view mapping: top half = L, bottom half = R); gotchas.md — replace any mention of
   the max-merge.

## U2. Location strip: remove the redundant line, show recording/steal state — DONE

The white marker line at the ewLocation knob value (`SpectrumDisplay.cpp:374-381`) is
redundant — **delete it**. In its place the strip gets real content: where injection is
landing right now, and whether it drags (steals) an existing tone.

1. **Engine snapshot** (`SpectralEngine.h/.cpp`): the per-bin loop already knows everything
   at injection time (`SpectralEngine.cpp:408-437`): `aInj > kInjLocFloor` = injection
   happened, `claimed` = landed on a freshly-claimed quiet layer, and for the drag case
   `aHeld = std::abs (sk)` before `sk += inj`.
   - Add per-frame scratch (plain bins, NOT layered; `maxBins` sized, allocated in
     `prepare`): `injLocScratch`, `injStrengthScratch`, `injDragScratch` (all `std::vector<float>`).
     Zero `injStrengthScratch` at the top of `processFrame` (cheap memset of numBins).
   - Inside the `if (aInj > kInjLocFloor)` block, after `binLoc[idx]` is updated:
     ```
     injLocScratch[k]      = binLoc[idx];
     injStrengthScratch[k] = aInj * (2.0f / (float) fftSize);   // same norm as dispMag
     injDragScratch[k]     = (! claimed && aHeld > kClaimClearFloor) ? 1.0f : 0.0f;
     ```
     (`dragged` = pulled a layer that already held audible content = the "steal" the user
     wants to see; a claim of a quiet layer is the clean case.)
   - In the existing display try-lock block (`SpectralEngine.cpp:492-511`), copy the three
     scratches into guarded `dispInjLoc/dispInjStrength/dispInjDrag`; new
     `int copyInjection (std::vector<float>& loc, std::vector<float>& strength, std::vector<float>& drag)`
     — try-lock, returns `numBins` or 0, exactly the `copyLayers` pattern
     (`SpectralEngine.cpp:922-938`, but plain stride, no layers).
   - Audio path is untouched by construction (snapshot only) — defaults stay bit-exact.
2. **Processor**: forwarder `getInjectionSnapshot (...)` → `engines[0].copyInjection` (same
   channel-0 convention as `getLocationSnapshot`).
3. **GUI** (`SpectrumDisplay`): in `timerCallback`, fetch into members with a **slow visual
   release** so brief events stay readable:
   `smoothInj[k] = max (strength[k], smoothInj[k] * 0.85f);` (latch loc/drag when
   `strength > 0`). In the strip section of `paint()` (after the layer dots,
   `SpectrumDisplay.cpp:353-372`), for bins with `smoothInj[k] > 1e-4`:
   - a 5-px vertical tick centred at `(freqToX (k * binToHz), stripBottom - loc * kLocStripH)`,
   - colour: **aurora blue** (`fromHSV (0.52f, 0.55f, 1, 1)`, the house accent) when
     `drag < 0.5` (clean record), **red-orange** (`fromHSV (0.05f, 0.8f, 1, 1)`) when
     dragging/stealing an existing tone,
   - alpha `jlimit (0, 1, sqrt (smoothInj))`, matching the dots' scale.
   - Delete the ewLocation marker-line block and the now-unused `pEwLocation` member.
4. **Tests** (`test_main.cpp`, engine-level, reuse the `ewDeposit` helper and the existing
   "ew claim boundary" recipes):
   - feed a sine at `ewLocation = 0.3` → `copyInjection` returns bins with
     `strength > 0` near the tone's bin and `loc ≈ 0.3` (±0.05); `drag == 0` on a fresh
     deposit.
   - the near-boundary re-record case (copy the "ew claim boundary (near, drag expected)"
     setup) → the fed bin reports `drag == 1`.
   - `feed = 0`, input driven → `strength == 0` everywhere (aInj gate holds).
5. **Wiki**: gui-display.md — rewrite the strip section (dots = held tones; ticks =
   live recording, red = stealing/dragging an existing tone; marker line removed as
   redundant); parameters.md E–W row gets one sentence pointing at the strip.

## U3. Perform tab — Transpose knob + snap toggle + glide, Phase Noise knob, Spread knob — DONE

Two new **params**, one new **tab**; utility row sheds its two tiny sliders.

1. **Params**, `createLayout()` order (IDs unchanged, order-only) — insert **both right
   after `transpose`** so the transpose group and P4 stay one coherent Perform page:
   P4 = `transpose, transposeSnap, transposeGlide, spread, phaseNoiseAmt, revFeed` (6/8,
   accepted partial). Update the page-order comment block (`PluginProcessor.cpp:69-80`).
   - `transposeSnap` (`AudioParameterBool`, name `"Transpose Snap"`, default **false** =
     today's continuous behaviour, bit-exact). Cache `pTransposeSnap`.
   - `transposeGlide` (`AudioParameterFloat`, name `"Transpose Glide"`, range `0..2000` ms,
     skewed like `revPredelay`/`limRelease` (`NormalisableRange<float> (0.0f, 2000.0f, 0.0f,
     0.35f)`), default **0** = instant, bit-exact. Cache `pTransposeGlide`.
2. **Snap semantics** (`processBlock`, `PluginProcessor.cpp:~285`): snap the **knob** only;
   the MIDI offset is already an integer. The knob target (pre-glide) becomes:
   ```
   const float tKnob = pTranspose->load();
   const float tTarget = (pTransposeSnap->load() > 0.5f ? (float) juce::roundToInt (tKnob) : tKnob)
                        + (float) (midiNote >= 0 ? midiNote - 60 : 0);
   p.transpose = tTarget;
   p.transposeGlideMs = pTransposeGlide->load();
   ```
   The MIDI offset is included in the glide target — a note-on portamentos from the
   *previous* effective pitch (knob or last note) to the new one, snap or no snap.
3. **Glide (engine)** (`SpectralEngine.h/.cpp`): smoothing lives in the engine, not the
   processor, so it's sample-accurate at frame granularity and — like Phase Noise's RNG —
   naturally per-channel (harmless; both channels glide identically since they share `p`).
   - **Params**: add `float transposeGlideMs = 0.0f;` next to `transpose`.
   - **State**: `float transposeSmoothed = 0.0f; bool transposeSmoothInit = false;` (reset in
     `reset()`: `transposeSmoothInit = false;` — do **not** zero `transposeSmoothed` itself,
     just force the lazy-init below to re-snap next frame, so a `reset()` mid-glide doesn't
     make the *next* note glide from 0 st).
   - In `processFrame`, **before** the existing `transRatio`/`transposing` computation
     (`SpectralEngine.cpp:~267-271`):
     ```
     if (! transposeSmoothInit) { transposeSmoothed = p.transpose; transposeSmoothInit = true; }
     const float glideCoef = p.transposeGlideMs > 1.0e-3f
         ? 1.0f - std::exp (-(float) hopSize / ((float) sampleRate * p.transposeGlideMs * 0.001f))
         : 1.0f;
     transposeSmoothed += (p.transpose - transposeSmoothed) * glideCoef;
     const float transRatio = std::exp2 (transposeSmoothed / 12.0f);
     const bool  transposing = std::abs (transposeSmoothed) > 1.0e-3f;
     ```
     (`transposeSmoothed` replaces `p.transpose` here only — nowhere else reads
     `p.transpose` directly.) At the default `transposeGlideMs = 0`, `glideCoef = 1` every
     frame so `transposeSmoothed == p.transpose` always → **bit-exact** with pre-U3 behaviour.
4. **GUI — fourth tab "Perform"** (`PluginEditor.h/.cpp`):
   - Tab strip: add `performTab { "Perform" }` to the radio group (`PluginEditor.cpp:44-49`),
     `onClick → setActiveTab (3)`. In `layoutContent()` the strip divides by 4 instead of 3
     (`presetBox` keeps its 150 px on the right; ~690/4 ≈ 172 px per tab, fine).
   - `setActiveTab` (`PluginEditor.cpp:286-313`): add
     `Knob* performKnobs[] = { &transpose, &transposeGlide, &spread, &phaseNoise };` + the
     snap button; `show (performKnobs, 4, tabIndex == 3)`, `snapButton.setVisible (tabIndex == 3)`,
     `performTab.setToggleState (tabIndex == 3, ...)`.
   - `uiTab` clamp: `jlimit (0, 2, ...)` → `jlimit (0, 3, ...)` in `setStateInformation`.
   - **Knobs**: replace the utility-row `transposeSlider/transposeLabel` and
     `phaseNoiseSlider/phaseNoiseLabel` (declared `PluginEditor.h:76-95`, set up
     `PluginEditor.cpp:69-102`, laid out in the `bottom` row) with `Knob transpose,
     transposeGlide, spread, phaseNoise` via `setupKnob` — labels "Transpose", "Glide",
     "Spread", "Phase Noise". After `setupKnob (transpose, ...)`:
     `transpose.slider.setTextValueSuffix (" st");`; after `setupKnob (transposeGlide, ...)`:
     `transposeGlide.slider.setTextValueSuffix (" ms");`.
     **Spread finally gets its widget** (it had none — kill the corresponding gotchas.md
     bullet and the "no widget" comment in the ctor).
   - **Snap toggle**: latching `juce::TextButton snapButton { "Snap" }` + `ButtonAttachment`
     to `transposeSnap`, sharing the Perform knob row (carve a cell like the tabbed rows do:
     `auto cell = area.removeFromRight (area.getWidth() / 8);` then centre a ~40×28 button —
     the exact pattern the old Freeze button used, see commit `5805a2a`). 4 knobs + 1 button
     fits the same row budget the shaper tab's 7 knobs already prove out.
   - Info lines:
     - transpose: keep the existing text, plus "Snap quantises to whole semitones; Glide
       smooths any pitch change, including MIDI note-ons, into a portamento."
     - snapButton: `"Discrete transposition: snap the knob to whole semitones. MIDI notes are always discrete."`
     - transposeGlide: `"Portamento time for pitch changes -- knob moves and MIDI note-ons alike. 0 = instant."`
     - spread: `"Momentary per-bin left/right spread of the output. Never enters the held sound."`
     - phase noise: keep the existing shimmer text.
   - Utility row afterwards: Live, Keep, Undo, Brush, FT Size only — verify the width sum
     against the 844-px budget in `layoutContent()` and update its comment.
5. **Persisted tab index**: old sessions restore uiTab 0..2, new range harmless. Nothing to
   migrate.
6. **Tests** (`test_main.cpp`, engine-level, extend the existing "transpose +12st" recipe
   at `SpectralEngine.cpp`'s test — reuse its held-440Hz-tone setup):
   - **glide=0 bit-exact vs today**: same two-engine bit-exact comparison pattern as the B5
     spread test (`p1`/`p2`, one explicit `transposeGlideMs = 0`, one left at default,
     driven by identical noise) → `maxErr == 0.0f`.
   - **glide smooths a step**: hold the tone, then jump `transpose` from 0 to +12 with
     `transposeGlideMs = 200`; assert the output frequency passes through intermediate
     values over the first ~200 ms (probe at t≈50ms: neither ~440Hz nor ~880Hz, something
     between) and settles at ~880 Hz by t≈400ms (probe like the existing transpose test's
     small-DFT/zero-crossing approach). With `transposeGlideMs = 0`, the same step lands at
     ~880 Hz within the first hop (no intermediate frame required) — assert that too, as the
     control case.
   - **`reset()` mid-glide doesn't glide the next note from 0 st**: start a glide toward
     +12, call `reset()` partway through, then hold a fresh 440 Hz tone at `transpose = 0`
     with `glideMs = 200` → output should read ~440 Hz immediately (the lazy-init re-snaps),
     not audibly still mid-glide-from-old-target.
7. **Wiki**: parameters.md — `transposeSnap`/`transposeGlide` rows, 28-param page table
   (P4 6/8), editor-layout paragraph (4 tabs, spread has a knob now, utility row contents);
   dsp-design.md — extend the Transpose section with the glide smoothing (one-pole per hop,
   lazy-init on first frame / after `reset()`); gotchas.md — update the "param creation
   order" bullet (P4 list), delete the "`spread` has no GUI widget" bullet, update the two
   bullets that said "until a P4 tab exists" (it exists now).

## U4. Final sweep (last commit on the branch)

- README.md wiki index: add this plan's line; flip plan-roadmap.md's line if wording refers
  to display behaviour changed here.
- gui-display.md: one read-through pass — the B7 section must describe the *current*
  split-stereo view and strip, with the old max-merge behaviour only as a historical note.
- Re-run `./build.sh`; all tests green, including the three new U2 cases.
