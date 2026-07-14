#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE_DIR="$ROOT_DIR/build/LUTLayerManager.ofx.bundle"
OUT_DIR="$ROOT_DIR/outputs"
PKG_SIGNING_IDENTITY="${PKG_SIGNING_IDENTITY:-}"

if [[ ! -d "$BUNDLE_DIR" ]]; then
  "$ROOT_DIR/scripts/build_macos.sh"
fi

mkdir -p "$OUT_DIR"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$BUNDLE_DIR/Contents/Info.plist")"
PKG="$OUT_DIR/LUTLayerManager_${VERSION}.pkg"
STAGING_DIR="$(mktemp -d "/tmp/lutlayermanager.pkg.XXXXXX")"
STAGED_BUNDLE="$STAGING_DIR/LUTLayerManager.ofx.bundle"

cleanup() {
  rm -rf "$STAGING_DIR"
}
trap cleanup EXIT

# Package from a clean temporary copy so workspace provenance attributes do
# not become AppleDouble ._* files in the installer payload.
ditto --norsrc --noextattr --noqtn --noacl "$BUNDLE_DIR" "$STAGED_BUNDLE"
find "$STAGING_DIR" -exec xattr -d com.apple.provenance {} \; 2>/dev/null || true

PKGBUILD_ARGS=(
  --component "$STAGED_BUNDLE" \
  --install-location "/Library/OFX/Plugins" \
  --identifier "com.lidong.ofx.lutlayermanager.pkg" \
  --version "$VERSION"
)

if [[ -n "$PKG_SIGNING_IDENTITY" ]]; then
  PKGBUILD_ARGS+=(--sign "$PKG_SIGNING_IDENTITY")
fi

rm -f "$PKG"
COPYFILE_DISABLE=1 pkgbuild "${PKGBUILD_ARGS[@]}" "$PKG"

if [[ -n "$PKG_SIGNING_IDENTITY" ]]; then
  pkgutil --check-signature "$PKG"
fi

echo "Package: $PKG"
