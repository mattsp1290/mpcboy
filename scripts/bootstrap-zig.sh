#!/usr/bin/env bash
# Install zig (via the `ziglang` PyPI package) into .tools/zigenv and print the ZIG value.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
env_dir="$root/.tools/zigenv"
if [ ! -x "$env_dir/bin/python" ]; then
  python3 -m venv "$env_dir"
fi
"$env_dir/bin/python" -m pip install --quiet --upgrade ziglang
"$env_dir/bin/python" -m ziglang version >&2
echo "ZIG=\"$env_dir/bin/python -m ziglang\""
