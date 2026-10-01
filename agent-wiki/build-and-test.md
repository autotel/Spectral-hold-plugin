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
checking; it already existed). **Triggers: version-tag pushes (`v*` by convention, bare `X.Y.Z` also accepted) and
manual `workflow_dispatch` only** — no builds on pushes to `main` or on PRs. Matrix: Linux/macOS/Windows, each checks out this repo and a
pinned `juce-framework/JUCE@8.0.12` (same major as `../JUCE` locally — re-pin together if you
bump JUCE) into sibling dirs, builds VST3 + Standalone + `SpectralHoldTest`, and **runs** the
test on Linux/macOS ("Run DSP smoke-test (Unix)") and Windows (separate `.exe` step) — not
build-only anywhere. Linux additionally builds the CLAP target (network-fetches
`clap-juce-extensions`, same pin as local; see the CLAP section above), build-only (no CLAP
host in the runner).

**Artifact packaging**: each platform's outputs are staged into a single
`autotel-spectral-hold/` folder (`dist/autotel-spectral-hold/` — VST3 always, Standalone
always, CLAP on Linux only) before upload. `upload-artifact`'s `path:` points at `dist`
(the folder's *parent*), not the bundle or the staging folder itself — pointing it at a
`.vst3` bundle directly zips that bundle's *contents* (a bare `Contents/` folder at the zip
root with no indication what plugin it even is); pointing it at the staging folder itself
loses the wrapper name the same way. One combined artifact per OS:
`autotel-spectral-hold-<Linux|macOS|Windows>`.

**Releases**: a version-tag push additionally runs the `release` job (`needs:
build`, gated on `startsWith(github.ref, 'refs/tags/')`) — downloads every platform's
artifact, zips each `autotel-spectral-hold/` folder into
`autotel-spectral-hold-<platform>.zip`, and publishes a GitHub Release via
`softprops/action-gh-release` with those zips attached and auto-generated release notes.
Uses the default `GITHUB_TOKEN` (no PAT needed) — the job has explicit
`permissions: contents: write` since the repo/org default may not grant that.
- **A workflow only fires for a tag whose *tagged commit* has the matching trigger** —
  GitHub reads `build.yml` from the tag's commit. Before 2026-10-01 only `v*` matched, so the
  UI-created `1.2.0` tag/release did nothing. A manual `workflow_dispatch` run builds but
  does **not** publish (its `github.ref` is a branch). Bump `project(... VERSION ...)` in
  `CMakeLists.txt` before tagging — the plugin reports that version, not the tag.

## Notes
- LTO is on (JUCE recommended flags); link is a little slow. Normal.
- Linux + GCC is the primary dev target here, but macOS and Windows both build **and run the
  DSP test** in CI (see above) — no platform-specific code, and it's actually verified there,
  not just assumed to work.
- **GUI screenshots: use the `SpectralHoldScreenshots` target, not X capture.**
  `Source/screenshot_main.cpp` builds the real processor + editor, feeds synthetic stereo
  chords through `processBlock` while pumping the message loop (`runDispatchLoopUntil`,
  needs `JUCE_MODAL_LOOPS_PERMITTED=1`, set on that target only) and renders scenes via
  `createComponentSnapshot` at 1.5x into `docs/images/*.png` — the images used by
  `README.md` and `docs/MANUAL.md`. Not built by `build.sh`:
  ```bash
  cmake --build build --target SpectralHoldScreenshots
  xvfb-run -a build/SpectralHoldScreenshots_artefacts/Release/SpectralHoldScreenshots docs/images
  ```
  ~80 s (real-time: the display's 30 Hz timer needs wall-clock time). Then `Read` the PNGs
  to eyeball layout changes.
  - **Mouse input is real JUCE dispatch**: the editor is `addToDesktop`'d on the Xvfb
    screen so `ComponentPeer::handleMouseEvent` can inject hover/press — that's what drives
    the info bar (hover) and brush strokes (press-and-hold) in the scenes.
  - **`JUCEApplicationBase::createInstance` is set to a dummy** at the top of `main()`:
    JUCE only installs its ignore-all X error handler for "standalone apps", and a
    WM-less Xvfb raises a harmless `BadAtom` on window creation that otherwise kills the
    process in Xlib's default handler.
  - Private editor members are found by walking children (`Rig::knob("Loss")` matches a
    visible `Label` to the rotary `Slider` under it; nth=0 = topmost, e.g. main-row Feed vs
    the Shaper's Feed). Annotations (amber rings, lettered badges) are drawn onto the
    snapshot in `Rig::save`.
  - Capture never touches the X server, so the old `xwd` `BadMatch`/`BadColor` failures on
    `$DISPLAY=:0` are irrelevant. When UI or behaviour shown in the manual changes, update
    the scene and `docs/MANUAL.md`, and regenerate.
