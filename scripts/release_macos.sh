#!/usr/bin/env bash
set -euo pipefail

# Create a direct-distribution package for macOS:
#   test -> Developer ID sign -> Developer ID Installer package -> notarize -> staple
#
# Required environment variables:
#   DEVELOPER_ID_APPLICATION  e.g. Developer ID Application: Legal Name (TEAMID)
#   DEVELOPER_ID_INSTALLER    e.g. Developer ID Installer: Legal Name (TEAMID)
#   NOTARY_KEYCHAIN_PROFILE   a profile saved with `xcrun notarytool store-credentials`

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT_DIR/outputs"

require_environment_variable() {
  local name="$1"
  if [[ -z "${!name:-}" ]]; then
    echo "Missing required environment variable: $name" >&2
    exit 2
  fi
}

require_command() {
  local command="$1"
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "Required command is not available: $command" >&2
    exit 2
  fi
}

require_environment_variable DEVELOPER_ID_APPLICATION
require_environment_variable DEVELOPER_ID_INSTALLER
require_environment_variable NOTARY_KEYCHAIN_PROFILE
require_command xcrun
require_command pkgutil
require_command spctl

"$ROOT_DIR/scripts/test_macos.sh"

CODESIGN_IDENTITY="$DEVELOPER_ID_APPLICATION" "$ROOT_DIR/scripts/build_macos.sh"
PKG_SIGNING_IDENTITY="$DEVELOPER_ID_INSTALLER" "$ROOT_DIR/scripts/package_macos_pkg.sh"

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$ROOT_DIR/build/LUTLayerManager.ofx.bundle/Contents/Info.plist")"
PKG="$OUT_DIR/LUTLayerManager_${VERSION}.pkg"
ZIP="$OUT_DIR/LUTLayerManager.ofx.bundle.zip"

echo "Submitting for notarization: $PKG"
xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" --wait

echo "Submitting manual-install archive for notarization: $ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" --wait

echo "Stapling notarization ticket"
xcrun stapler staple "$PKG"
xcrun stapler validate "$PKG"
pkgutil --check-signature "$PKG"
spctl --assess --type install --verbose=4 "$PKG"

echo
echo "Release package ready: $PKG"
echo "Notarized manual-install archive: $ZIP"
