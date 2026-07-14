#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
BUNDLE_DIR="$BUILD_DIR/LUTLayerManager.ofx.bundle"
BIN_DIR="$BUNDLE_DIR/Contents/MacOS"
RES_DIR="$BUNDLE_DIR/Contents/Resources"
OUT_DIR="$ROOT_DIR/outputs"
SIGNING_IDENTITY="${CODESIGN_IDENTITY:--}"

rm -rf "$BUNDLE_DIR"
mkdir -p "$BIN_DIR" "$RES_DIR" "$OUT_DIR"
cp "$ROOT_DIR/cmake/Info.plist.in" "$BUNDLE_DIR/Contents/Info.plist"
cp "$ROOT_DIR/third_party/openfx/LICENSE.md" "$RES_DIR/OpenFX-LICENSE.md"
cp "$ROOT_DIR/third_party/opencolorio/LICENSE.md" "$RES_DIR/OpenColorIO-LICENSE.md"

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
  -weak_framework
  UniformTypeIdentifiers
)

TMP_ARM="$BUILD_DIR/LUTLayerManager_arm64.ofx"
TMP_X64="$BUILD_DIR/LUTLayerManager_x86_64.ofx"
FINAL_BIN="$BIN_DIR/LUTLayerManager.ofx"

clang++ "${COMMON_FLAGS[@]}" -arch arm64 "$ROOT_DIR/src/LUTLayerManager.cpp" -o "$TMP_ARM"
clang++ "${COMMON_FLAGS[@]}" -arch x86_64 "$ROOT_DIR/src/LUTLayerManager.cpp" -o "$TMP_X64"
lipo -create "$TMP_ARM" "$TMP_X64" -output "$FINAL_BIN"
chmod 755 "$FINAL_BIN"

SIGN_ARGS=(
  --force
  --sign "$SIGNING_IDENTITY"
)

# Developer ID builds need a trusted timestamp and the hardened runtime for
# notarization. Keep the default ad-hoc signature for local development so a
# certificate is never required just to build or test the plugin.
if [[ "$SIGNING_IDENTITY" != "-" ]]; then
  SIGN_ARGS+=(
    --timestamp
    --options runtime
  )
fi

codesign "${SIGN_ARGS[@]}" "$BUNDLE_DIR"
codesign --verify --deep --strict --verbose=2 "$BUNDLE_DIR"

(
  rm -f "$OUT_DIR/LUTLayerManager.ofx.bundle.zip"
  ditto -c -k --keepParent --norsrc --noextattr --noqtn --noacl \
    "$BUNDLE_DIR" "$OUT_DIR/LUTLayerManager.ofx.bundle.zip"
)

echo "Built: $BUNDLE_DIR"
echo "Zip:   $OUT_DIR/LUTLayerManager.ofx.bundle.zip"
