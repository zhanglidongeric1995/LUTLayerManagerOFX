#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE_DIR="$ROOT_DIR/build/LUTLayerManager.ofx.bundle"
OUT_DIR="$ROOT_DIR/outputs"
PKG="$OUT_DIR/LUTLayerManager_1.0.1.pkg"

if [[ ! -d "$BUNDLE_DIR" ]]; then
  "$ROOT_DIR/scripts/build_macos.sh"
fi

mkdir -p "$OUT_DIR"
pkgbuild \
  --component "$BUNDLE_DIR" \
  --install-location "/Library/OFX/Plugins" \
  --identifier "com.lidong.ofx.lutlayermanager.pkg" \
  --version "1.0.1" \
  "$PKG"

echo "Package: $PKG"
