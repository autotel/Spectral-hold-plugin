#!/usr/bin/env bash
# Configure + build Spectral Hold (VST3 + Standalone) and run the DSP smoke-test.
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR="build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc 2>/dev/null || echo 4)"

echo "==> Configuring ($BUILD_TYPE)"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

echo "==> Building"
cmake --build "$BUILD_DIR" \
  --target SpectralHold_VST3 SpectralHold_Standalone SpectralHoldTest \
  -j "$JOBS"

echo "==> Running DSP smoke-test"
"$BUILD_DIR/SpectralHoldTest_artefacts/$BUILD_TYPE/SpectralHoldTest"

echo
echo "Artifacts:"
echo "  $BUILD_DIR/SpectralHold_artefacts/$BUILD_TYPE/Standalone/Spectral Hold"
echo "  $BUILD_DIR/SpectralHold_artefacts/$BUILD_TYPE/VST3/Spectral Hold.vst3"
