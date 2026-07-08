# Agent Wiki — Spectral Hold

Knowledge base for coding agents (Claude Code et al.) working on this project.
**Read this before exploring the codebase.** It compiles the non-obvious facts so you
don't have to re-derive them (and burn credits) every session.

## What this project is
A JUCE audio plugin: a **spectral hold / spectral freeze** effect. Input audio is
analysed with an STFT and fed into a *running* spectral state that keeps playing
forward forever. It does **not** loop a windowed grain — it extrapolates the
Fourier transform: each bin is a free-running phasor (phase-vocoder synthesis), so
held tones are continuous sinusoids, not a repeating buffer. VST3 + Standalone, Linux.

## Wiki index
- [product-spec.md](product-spec.md) — the product specification **as given by the user**.
  Keep development congruent with this. Do not silently diverge.
- [architecture.md](architecture.md) — file layout, signal flow, threading.
- [dsp-design.md](dsp-design.md) — the spectral engine: STFT, the held-phasor model,
  feed/loss/filter/attack math, the filter compensation, the limiter. Design rationale.
- [parameters.md](parameters.md) — every parameter: id, range, default, mapping.
- [gui-display.md](gui-display.md) — the custom "lighting" spectrum view.
- [build-and-test.md](build-and-test.md) — configure, build, run the offline test.
- [gotchas.md](gotchas.md) — traps, decisions already made, things that look like bugs but aren't.
- [harmonize.md](harmonize.md) — coupled-oscillator tone interaction (entrainment +
  harmonic attraction), integrated on `exp/integration`.
- [plan-spectral-shaper.md](plan-spectral-shaper.md) — design rationale for the spectral
  **shaper** (branch `exp/spectral-shaper`, implemented) that replaced Filter + Compress.
  The current behavior is documented in dsp-design.md / parameters.md; read this plan for
  the "why" behind the math constants.
- [plan-reverb.md](plan-reverb.md) — design rationale for the output plate reverb
  (branch `exp/reverb`, implemented). Topology, constants, tests.
- [plan-integration.md](plan-integration.md) — plan for `exp/integration`: merge harmonize
  into shaper+reverb, param pages of 8, tabbed GUI, per-module integration knobs, info bar.
  Includes the merge-conflict resolution map.
- [plan-fixes.md](plan-fixes.md) — fix-list pass on `exp/integration` (for Sonnet): overlays,
  revFeed removal, limiter params, reverb Metal, renames, macro pages, knob shine.
- [plan-eastwest.md](plan-eastwest.md) — plan (NOT yet implemented) for the **East↔West
  location knob** (`exp/eastwest`): the held sound becomes a 2-D `[frequency, location]`
  field of 16 slots blended by a presence-weighted location gain. DSP model, engine
  restructure, tests.
- [plan-roadmap.md](plan-roadmap.md) — plan (NOT yet implemented) for the roadmap batch:
  **revFeed done right** (Feed-independent aux path, lands on `main`) and, on `exp/roadmap`,
  hold serialization, freeze, transpose+MIDI, continuous phase noise, stereo spread, undo,
  stereo display + E–W location strip, resizable editor, presets, CI+CLAP.
- [plan-loclayers.md](plan-loclayers.md) — plan (implemented) for **location layers**
  (`exp/loclayers`): K=4 parallel held states so the same frequency can coexist at
  multiple E–W locations — fixes the "re-recording a pitch elsewhere steals/drags it"
  compromise. Injection routes to the nearest layer or claims a fresh one; playback mixes
  per bin before a single IFFT. Current behavior is in dsp-design.md's "Location layers"
  section; read this plan for the design rationale and rejected alternatives.

## Fast facts
- JUCE lives at `../JUCE` (sibling of repo root), used via `add_subdirectory`. Modern
  `juce_add_plugin` CMake API. Reference plugins: `../tape-looper`, `../lanes-audio-plugin`.
- Source in `Source/`. DSP isolated in `SpectralEngine.{h,cpp}` — depends only on
  `juce_dsp`, so it is unit-testable without a host. Test target: `SpectralHoldTest`.
- Build dir is `build/`. Build + test with `./build.sh`.
- One `SpectralEngine` **per channel** (stereo = 2 independent engines). The limiter is
  the only cross-channel (linked) stage; it lives in `PluginProcessor`.
- FFT size is **GUI-only**, not a DAW parameter (deliberate — see product-spec). Default
  4096. It is stored in plugin state manually, not in the APVTS.

## Maintenance rule for agents
When you change behavior, **update the relevant wiki page in the same task**. Keep pages
dense and factual. Do not duplicate code into the wiki — describe intent, invariants, and
rationale that the code alone doesn't convey. If you change DSP, re-run `./build.sh` and
make sure the smoke-test still passes.
