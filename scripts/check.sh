#!/usr/bin/env bash
# Local quality gate: configure + build all presets, run native tests.
# Checks the committed generated UI (scripts/build_ui.py) is up to date.
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

echo "== ctest native"
ctest --preset native

echo "== shared UI: params dump, build_ui.py, python tests, stale check"
native_lib=build/native/mpcboy.dylib
[ -f "$native_lib" ] || native_lib=build/native/mpcboy.so
build/native/host_sim "$native_lib" dump > build/params.json
python3 scripts/build_ui.py --params build/params.json
python3 -m unittest discover -s tests/ui -v
# The generated UI is committed: fail if regenerating changed a tracked file or left an
# untracked one (new art must be added to git; staged-but-uncommitted files are fine).
ui_paths=(ui/art src/ui/generated)
stale_tracked=$(git diff --name-only -- "${ui_paths[@]}")
stale_untracked=$(git ls-files --others --exclude-standard -- "${ui_paths[@]}")
if [ -n "$stale_tracked$stale_untracked" ]; then
  echo "Generated UI files are stale or not in git (re-run scripts/build_ui.py and add them):"
  [ -n "$stale_tracked" ] && echo "$stale_tracked" | sed 's/^/  modified:  /'
  [ -n "$stale_untracked" ] && echo "$stale_untracked" | sed 's/^/  untracked: /'
  exit 1
fi

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
