#!/usr/bin/env bash
# Assemble a minimal MPC plugin folder (plugin.json + VST/*.so, no skins) in
# build/minfolder/ and run the SDK validator on it. WP9's package_mpc.py
# supersedes this with the full folder.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
folder="$root/build/minfolder/mpcboy - VST - mpcboy"
rm -rf "$root/build/minfolder"
mkdir -p "$folder/VST"
cp "$root/plugin/plugin.json" "$folder/plugin.json"
cp "$root/build/arm32/mpcboy-arm32.so" "$root/build/arm64/mpcboy-arm64.so" "$folder/VST/"
cat > "$folder/version.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<plugincontent version="1.0">
    <version>0.1.0.0</version>
</plugincontent>
XML
params=()
if [ -x "$root/build/native/host_sim" ] && [ -f "$root/build/native/mpcboy.dylib" ]; then
  "$root/build/native/host_sim" "$root/build/native/mpcboy.dylib" dump > "$root/build/params.json"
  params=(--params "$root/build/params.json")
fi
python3 "$root/vendor/mpc-plugin-builder-sdk/tools/validate.py" "$folder" "${params[@]+"${params[@]}"}" --strict
