#!/usr/bin/env bash
# Local quality gate: configure + build all presets, run native tests.
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

if [ "$(uname -s)" = "Darwin" ]; then
  echo "== mac VST bundle + host_sim at 44.1/48/96 kHz"
  cmake --preset mac-vst
  cmake --build --preset mac-vst
  lipo -archs build/mac-vst/mpcboy.vst/Contents/MacOS/mpcboy
  codesign -v build/mac-vst/mpcboy.vst
  for sr in 44100 48000 96000; do
    build/native/host_sim build/mac-vst/mpcboy.vst/Contents/MacOS/mpcboy test \
      --sr "$sr" --seconds 3 --notes 48,60 --playing | tee build/mac-vst/host_sim_$sr.log | tail -1
    if grep -q "^FAIL" "build/mac-vst/host_sim_$sr.log"; then echo "host_sim FAIL at $sr Hz"; exit 1; fi
  done
fi

echo "== package (host_sim, skins, strict validator, zip)"
python3 scripts/package_mpc.py

echo "check.sh: OK"
