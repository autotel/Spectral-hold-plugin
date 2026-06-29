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

## Notes
- LTO is on (JUCE recommended flags); link is a little slow. Normal.
- Linux + GCC is the developed target. No platform-specific code; macOS/Windows should work
  via the same CMake but is unverified here.
