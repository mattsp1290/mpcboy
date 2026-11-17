#!/usr/bin/env bash
# Build the macOS VST2 bundle (mpcboy.vst, universal, ad-hoc signed) and install it for
# Ableton Live: ~/Library/Audio/Plug-Ins/VST/mpcboy.vst. Development build, not the MPC package.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ "$(uname -s)" != "Darwin" ]; then
  echo "install_mac_vst.sh: macOS only" >&2
  exit 1
fi

echo "== configure + build (preset mac-vst)"
cmake --preset mac-vst
cmake --build --preset mac-vst

SRC="build/mac-vst/mpcboy.vst"
DEST_DIR="$HOME/Library/Audio/Plug-Ins/VST"
DEST="$DEST_DIR/mpcboy.vst"
[ -d "$SRC" ] || { echo "install_mac_vst.sh: $SRC was not produced" >&2; exit 1; }

echo "== install to $DEST"
mkdir -p "$DEST_DIR"
rm -rf "$DEST"
cp -R "$SRC" "$DEST"
# This macOS xattr has no -r: strip the quarantine flag file by file (no-op when absent).
find "$DEST" -exec xattr -d com.apple.quarantine {} \; 2>/dev/null || true
codesign --force --sign - "$DEST"
codesign -v "$DEST"
echo "installed: $DEST"
lipo -archs "$DEST/Contents/MacOS/mpcboy" | sed 's/^/architectures: /'

cat <<MSG

Ableton Live 12:
  1. Preferences > Plug-ins > turn ON "Use VST2 Plug-in System Folders".
  2. Click "Rescan" (hold Option while clicking for a full rescan if mpcboy does not show up).
  3. Browser > Plug-ins > mpcboy (VST2 instrument). Drop it on a MIDI track.
  4. Live shows its generic parameter panel (Configure to pick parameters); there is no custom editor.
  Live runs at 44.1 or 48 kHz (also 96 / 192 kHz). 88.2 kHz is unsupported and the plugin is silent there.
  To re-install after a code change, run this script again (Live must rescan or restart).
  LSDJ .sav files go in: $DEST/Contents/Resources/Content/Import
MSG
