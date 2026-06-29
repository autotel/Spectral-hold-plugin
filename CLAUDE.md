# Spectral Hold — coding agent entry point

JUCE spectral-hold / spectral-freeze plugin (VST3 + Standalone, Linux). It extrapolates
the FT — held bins are free-running phasors, not looped grains.

**Before doing anything, read [`agent-wiki/README.md`](agent-wiki/README.md)** and the
pages it links. The wiki has the architecture, DSP math, parameter table, GUI notes,
build/test instructions, gotchas, and the verbatim product spec.

Fast start:
- Build + test: `./build.sh`
- DSP core: `Source/SpectralEngine.{h,cpp}` (host-free, covered by `SpectralHoldTest`).
- JUCE at `../JUCE`; reference plugins at `../tape-looper`, `../lanes-audio-plugin`.

Rule: when you change behavior, update the matching `agent-wiki/` page in the same task and
re-run `./build.sh`.
