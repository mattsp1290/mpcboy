#!/usr/bin/env bash
# Local quality gate: configure + build all presets, run native tests.
# WP9 extends this with host_sim and package_mpc.py.
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

echo "== minimal folder + SDK validator"
scripts/make_min_folder.sh

echo "check.sh: OK"
