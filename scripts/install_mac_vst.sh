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
if pgrep -f "Ableton Live" >/dev/null 2>&1; then
  echo "warning: Ableton Live is running. It keeps the old plugin loaded until it rescans or restarts." >&2
fi
mkdir -p "$DEST_DIR"
NEW="$DEST.new.$$"
OLD="$DEST.old.$$"
trap 'rm -rf "$NEW"' EXIT
rm -rf "$NEW"
cp -R "$SRC" "$NEW"
# Keep the user's .sav files: they live inside the bundle (Contents/Resources/Content/Import).
IMPORT_REL="Contents/Resources/Content/Import"
if [ -d "$DEST/$IMPORT_REL" ]; then
  mkdir -p "$NEW/$IMPORT_REL"
  kept=0
  for f in "$DEST/$IMPORT_REL"/*.sav; do
    [ -e "$f" ] || continue
    cp -p "$f" "$NEW/$IMPORT_REL/"
    kept=$((kept + 1))
  done
  echo "kept $kept .sav file(s) from the existing install"
fi
# Strip quarantine / other xattrs from the new copy. Newer macOS xattr has -r; this one may not
# (it only has -c), so fall back to clearing file by file.
xattr -cr "$NEW" >/dev/null 2>&1 || find "$NEW" -exec xattr -c {} + >/dev/null 2>&1 || true
codesign --force --sign - "$NEW"
codesign -v --strict "$NEW"
# Swap in one step: the old bundle is moved aside, the new one renamed into place, then the old one removed.
if [ -e "$DEST" ]; then mv "$DEST" "$OLD"; fi
mv "$NEW" "$DEST"
rm -rf "$OLD"
trap - EXIT
echo "installed: $DEST"
lipo -archs "$DEST/Contents/MacOS/mpcboy" | sed 's/^/architectures: /'

cat <<MSG

Ableton Live 12:
  1. Preferences > Plug-ins > turn ON "Use VST2 Plug-in System Folders".
  2. Click "Rescan" (hold Option while clicking for a full rescan if mpcboy does not show up).
  3. Browser > Plug-ins > mpcboy (VST2 instrument). Drop it on a MIDI track.
  4. Open the device's plugin window for the Game Boy editor (tabs at the bottom switch pages).
  Live runs at 44.1 or 48 kHz (also 96 / 192 kHz). 88.2 kHz is unsupported and the plugin is silent there.
  To re-install after a code change, run this script again (Live must rescan or restart).
  LSDJ .sav files go in: $DEST/Contents/Resources/Content/Import
MSG
