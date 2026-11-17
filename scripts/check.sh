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

echo "== package (host_sim, skins, strict validator, zip)"
python3 scripts/package_mpc.py

echo "check.sh: OK"
