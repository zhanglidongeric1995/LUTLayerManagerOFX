#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
TEST_BIN="$BUILD_DIR/LUTLayerManagerTests"

mkdir -p "$BUILD_DIR"
clang++ \
  -std=c++17 \
  -O2 \
  -I"$ROOT_DIR/third_party/openfx/include" \
  -Wall \
  -Wextra \
  -Wno-unused-parameter \
  -x objective-c++ \
  -fobjc-arc \
  -fblocks \
  "$ROOT_DIR/tests/LutLayerManagerTests.cpp" \
  "$ROOT_DIR/src/LUTLayerManagerMetal.mm" \
  -framework Cocoa \
  -framework Metal \
  -weak_framework UniformTypeIdentifiers \
  -o "$TEST_BIN"

"$TEST_BIN"
