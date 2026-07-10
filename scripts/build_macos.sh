#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
BUNDLE_DIR="$BUILD_DIR/LUTLayerManager.ofx.bundle"
BIN_DIR="$BUNDLE_DIR/Contents/MacOS"
RES_DIR="$BUNDLE_DIR/Contents/Resources"
OUT_DIR="$ROOT_DIR/outputs"

mkdir -p "$BIN_DIR" "$RES_DIR" "$OUT_DIR"
cp "$ROOT_DIR/cmake/Info.plist.in" "$BUNDLE_DIR/Contents/Info.plist"

COMMON_FLAGS=(
  -std=c++17
  -O3
  -fvisibility=hidden
  -I"$ROOT_DIR/third_party/openfx/include"
  -Wall
  -Wextra
  -Wno-unused-parameter
  -x
  objective-c++
  -fobjc-arc
  -fblocks
  -bundle
  -Wl,-undefined,dynamic_lookup
  -framework
  Cocoa
)

TMP_ARM="$BUILD_DIR/LUTLayerManager_arm64.ofx"
TMP_X64="$BUILD_DIR/LUTLayerManager_x86_64.ofx"
FINAL_BIN="$BIN_DIR/LUTLayerManager.ofx"

clang++ "${COMMON_FLAGS[@]}" -arch arm64 "$ROOT_DIR/src/LUTLayerManager.cpp" -o "$TMP_ARM"
clang++ "${COMMON_FLAGS[@]}" -arch x86_64 "$ROOT_DIR/src/LUTLayerManager.cpp" -o "$TMP_X64"
lipo -create "$TMP_ARM" "$TMP_X64" -output "$FINAL_BIN"
chmod 755 "$FINAL_BIN"
codesign --force --deep --sign - "$BUNDLE_DIR"
codesign --verify --deep --strict --verbose=2 "$BUNDLE_DIR"

(
  cd "$BUILD_DIR"
  zip -qry "$OUT_DIR/LUTLayerManager.ofx.bundle.zip" "LUTLayerManager.ofx.bundle"
)

echo "Built: $BUNDLE_DIR"
echo "Zip:   $OUT_DIR/LUTLayerManager.ofx.bundle.zip"
