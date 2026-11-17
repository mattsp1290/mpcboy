#!/usr/bin/env bash
# Assemble a minimal MPC plugin folder (plugin.json + VST/*.so, no skins) in
# build/minfolder/ and run the SDK validator on it. WP9's package_mpc.py
# supersedes this with the full folder.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# MPCBOY_BUILD_SUFFIX=-probe packages the CPU spike plugin built into
# build/{arm32,arm64,native}-probe (see docs/BENCH.md) as build/minfolder-probe.
suffix="${MPCBOY_BUILD_SUFFIX:-}"
folder="$root/build/minfolder$suffix/mpcboy - VST - mpcboy"
rm -rf "$root/build/minfolder$suffix"
mkdir -p "$folder/VST"
cp "$root/plugin/plugin.json" "$folder/plugin.json"
cp "$root/build/arm32$suffix/mpcboy-arm32.so" "$root/build/arm64$suffix/mpcboy-arm64.so" "$folder/VST/"
cat > "$folder/version.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<plugincontent version="1.0">
    <version>0.1.0.0</version>
</plugincontent>
XML
params=()
if [ -x "$root/build/native/host_sim" ] && [ -f "$root/build/native$suffix/mpcboy.dylib" ]; then
  "$root/build/native/host_sim" "$root/build/native$suffix/mpcboy.dylib" dump > "$root/build/params$suffix.json"
  params=(--params "$root/build/params$suffix.json")
fi
python3 "$root/vendor/mpc-plugin-builder-sdk/tools/validate.py" "$folder" "${params[@]+"${params[@]}"}" --strict
