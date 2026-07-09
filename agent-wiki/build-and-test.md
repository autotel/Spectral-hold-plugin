# Build and test

## Quick
```bash
./build.sh                 # Release: VST3 + Standalone + smoke-test, then runs the test
BUILD_TYPE=Debug ./build.sh
```

## Manual
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target \
    SpectralHold_VST3 SpectralHold_Standalone SpectralHoldTest
```
Override JUCE location (CI): `-DJUCE_PATH=/path/to/JUCE` (defaults to `../JUCE`).

## Artifacts
- `build/SpectralHold_artefacts/<cfg>/VST3/Spectral Hold.vst3`
- `build/SpectralHold_artefacts/<cfg>/Standalone/Spectral Hold`

## CLAP (agent-wiki/plan-roadmap.md B10)
Off by default in `build.sh` (`-DSPECTRALHOLD_CLAP=OFF` — see the comment there): the CLAP
target fetches `clap-juce-extensions` over the network **at configure time**, regardless of
whether you actually build the target, which would silently turn the fast/offline local dev
loop into one that needs network. Build it explicitly when you need it:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSPECTRALHOLD_CLAP=ON
cmake --build build --target SpectralHold_CLAP
```
Artifact: `build/SpectralHold_artefacts/<cfg>/CLAP/Spectral Hold.clap`.

**Pinned to a `clap-juce-extensions` `main` commit, not the latest tag** (`0.26.0` as of
writing): the tag still includes JUCE's `LegacyAudioParameter.cpp` via the pre-split path
(`juce_audio_processors/format_types/...`), which doesn't exist in JUCE 8's module layout
(it moved to `juce_audio_processors_headless/format_types/...`) — fails at compile time with
"No such file or directory". The pinned `main` commit already has the updated include path.
Re-check upstream if bumping the pin; `git log -- src/wrapper/clap-juce-wrapper.cpp` on their
repo is the fastest way to confirm a candidate commit/tag has the new path.

No Linux CLAP host was available to load-test the built plugin (the only one installed,
Carla 2.5.8, ships a `carla-discovery-native` binary without CLAP support compiled in — its
usage string only lists `vst2`/`VST3`/`ladspa`/`dssi`/`internal`). Verified build-only, which
the plan explicitly allows as the fallback. If a CLAP-capable host becomes available, loading
the built `.clap` and confirming it shows up as an effect is the remaining verification step.

## Offline DSP test (`SpectralHoldTest`)
`Source/test_main.cpp`. Links **only** `juce_dsp` + `SpectralEngine` (no host/GUI), so it
builds fast and is the right place to validate DSP changes. Checks:
1. silence → silence (no self-oscillation / NaN);
2. a fed 1 kHz sine produces bounded, non-trivial output **and** a sustained tail after the
   input stops (proves the hold actually holds);
3. an FFT-size change mid-stream stays finite.

Run directly:
```bash
build/SpectralHoldTest_artefacts/Release/SpectralHoldTest
```
Exit code 0 = all pass. **Always run it after touching `SpectralEngine`.** If you change
overlap-add / windowing / normalisation, watch the `maxAbs` it prints — it should stay
near ~1.0 for the unity-ish passthrough case (currently ~1.06).

## CI (agent-wiki/plan-roadmap.md B10)
`.github/workflows/build.yml` — predates this plan's B10 entry (the plan was written without
checking; it already existed). Matrix: Linux/macOS/Windows, each checks out this repo and a
pinned `juce-framework/JUCE@8.0.12` (same major as `../JUCE` locally — re-pin together if you
bump JUCE) into sibling dirs, builds VST3 + Standalone + `SpectralHoldTest`, and **runs** the
test on Linux/macOS ("Run DSP smoke-test (Unix)") and Windows (separate `.exe` step) — not
build-only anywhere. Linux additionally builds the CLAP target (network-fetches
`clap-juce-extensions`, same pin as local; see the CLAP section above) and uploads it,
build-only (no CLAP host in the runner). Artifacts (VST3 all platforms, Standalone per
platform, CLAP Linux) are uploaded via `actions/upload-artifact`.

## Notes
- LTO is on (JUCE recommended flags); link is a little slow. Normal.
- Linux + GCC is the primary dev target here, but macOS and Windows both build **and run the
  DSP test** in CI (see above) — no platform-specific code, and it's actually verified there,
  not just assumed to work.
- **Automated GUI screenshotting does not work in this environment — don't attempt it.**
  There's a real X display (`$DISPLAY=:0`), but no `Xvfb`/`scrot`/`import`/`xdotool`; `xwd`
  is present but fails (`BadMatch` on root, `BadColor` on the app window — likely an
  ARGB/compositing visual mismatch) and no headless fallback is installed. Confirmed
  by the user ("the ability to take screenshots hasn't worked in the past"). For GUI/layout
  changes, reason about pixel math from `resized()` instead (verify by summing widths
  against the actual container rect, not assumption), or ask the user to eyeball it — don't
  burn time re-discovering this each session.
