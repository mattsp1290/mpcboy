#!/usr/bin/env bash
# Local quality gate: configure + build all presets, run native tests.
# Checks the committed generated UI (scripts/build_ui.py) is up to date (before the native tests).
# Ends with package_mpc.py: host_sim, skins, strict SDK validator, zip.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ ! -x .tools/zigenv/bin/python ] && ! command -v zig >/dev/null 2>&1; then
  scripts/bootstrap-zig.sh
fi

for preset in native arm32 arm64; do
  echo "== configure $preset"
  cmake --preset "$preset"
  echo "== build $preset"
  cmake --build --preset "$preset"
done

echo "== shared UI: params dump, generated-file stale check, python tests"
native_lib=build/native/mpcboy.dylib
[ -f "$native_lib" ] || native_lib=build/native/mpcboy.so
build/native/host_sim "$native_lib" dump > build/params.json
# Regenerates into a temp dir (the tree is untouched) and compares decoded PNG pixels / exact
# .inc text with the committed files; also fails on unstaged or untracked generated files
# (staged-but-uncommitted is fine). Runs before ctest so a stale .inc fails fast.
python3 scripts/check_ui_generated.py --params build/params.json
python3 -m unittest discover -s tests/ui -v

echo "== ctest native"
ctest --preset native

if [ "$(uname -s)" = "Darwin" ]; then
  echo "== mac VST bundle + host_sim at 44.1/48/96/192 kHz"
  cmake --preset mac-vst
  cmake --build --preset mac-vst
  lipo -archs build/mac-vst/mpcboy.vst/Contents/MacOS/mpcboy
  codesign -v build/mac-vst/mpcboy.vst
  echo "== editor offscreen render (build/editor-preview)"
  build/mac-vst/render_editor ui/art build/editor-preview 2 >/dev/null
  for sr in 44100 48000 96000 192000; do
    build/native/host_sim build/mac-vst/mpcboy.vst/Contents/MacOS/mpcboy test \
      --sr "$sr" --seconds 3 --notes 48,60 --playing | tee build/mac-vst/host_sim_$sr.log | tail -1
    if grep -q "^FAIL" "build/mac-vst/host_sim_$sr.log"; then echo "host_sim FAIL at $sr Hz"; exit 1; fi
  done
fi

echo "== package (host_sim, skins, strict validator, zip)"
python3 scripts/package_mpc.py

echo "check.sh: OK"
