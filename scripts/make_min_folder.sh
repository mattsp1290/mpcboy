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
# Probe builds (MPCBOY_CPU_PROBE=ON) are named mpcboy-probe-<arch>.so / mpcboy-probe.dylib.
probe=""
[ -n "$suffix" ] && probe="-probe"
sed -e "s#VST/mpcboy-arm32.so#VST/mpcboy${probe}-arm32.so#" -e "s#VST/mpcboy-arm64.so#VST/mpcboy${probe}-arm64.so#" \
  "$root/plugin/plugin.json" > "$folder/plugin.json"
cp "$root/build/arm32$suffix/mpcboy${probe}-arm32.so" "$root/build/arm64$suffix/mpcboy${probe}-arm64.so" "$folder/VST/"
# Version from plugin.json (0.1.0 -> 0.1.0.0), never hard-coded here.
ver="$(python3 - "$root/plugin/plugin.json" <<'PY'
import json, re, sys
v = json.load(open(sys.argv[1]))["version"]
p = [x for x in re.split(r"[.\-]", v) if x.isdigit()][:4]
print(".".join(p + ["0"] * (4 - len(p))))
PY
)"
cat > "$folder/version.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<plugincontent version="1.0">
    <version>${ver}</version>
</plugincontent>
XML
params=()
if [ -x "$root/build/native/host_sim" ] && [ -f "$root/build/native$suffix/mpcboy${probe}.dylib" ]; then
  "$root/build/native/host_sim" "$root/build/native$suffix/mpcboy${probe}.dylib" dump > "$root/build/params$suffix.json"
  params=(--params "$root/build/params$suffix.json")
fi
python3 "$root/vendor/mpc-plugin-builder-sdk/tools/validate.py" "$folder" "${params[@]+"${params[@]}"}" --strict
