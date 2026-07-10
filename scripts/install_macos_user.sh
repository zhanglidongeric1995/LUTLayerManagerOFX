#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE_DIR="$ROOT_DIR/build/LUTLayerManager.ofx.bundle"
DEST_DIR="/Library/OFX/Plugins"
DEST="$DEST_DIR/LUTLayerManager.ofx.bundle"
CACHE="$HOME/Library/Application Support/Blackmagic Design/DaVinci Resolve/OFXPluginCacheV2.xml"

if [[ ! -d "$BUNDLE_DIR" ]]; then
  "$ROOT_DIR/scripts/build_macos.sh"
fi

if pgrep -fl "DaVinci Resolve|Resolve" >/dev/null 2>&1; then
  echo "Warning: DaVinci Resolve is running. Quit it completely before checking for the plugin."
fi

mkdir -p "$DEST_DIR"
if [[ -d "$DEST" ]]; then
  BACKUP_DIR="$HOME/Desktop/LIDONG 开发/DaVinci Plugin Backups"
  mkdir -p "$BACKUP_DIR"
  BACKUP="$BACKUP_DIR/LUTLayerManager.ofx.bundle.backup.$(date +%Y%m%d%H%M%S)"
  mv "$DEST" "$BACKUP"
  echo "Backup:   $BACKUP"
fi

cp -R "$BUNDLE_DIR" "$DEST"
codesign --force --deep --sign - "$DEST"
codesign --verify --deep --strict --verbose=2 "$DEST"

if [[ -f "$CACHE" ]]; then
  cp "$CACHE" "$CACHE.bak.$(date +%Y%m%d%H%M%S)"
  rm "$CACHE"
fi

echo "Installed: $DEST"
echo "Resolve OFX cache cleared. Restart DaVinci Resolve."
