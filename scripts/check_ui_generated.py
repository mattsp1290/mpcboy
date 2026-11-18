#!/usr/bin/env python3
"""check_ui_generated.py - is the committed generated UI what scripts/build_ui.py produces now?

Regenerates into a temporary root (the tree is not touched) and compares with ui/art and
src/ui/generated. PNGs are compared by content (IHDR plus the inflated IDAT scanlines), not by
file bytes, so a different zlib build cannot cause false staleness; .inc files are compared as
exact text. Also fails when a generated path differs between the working tree and the index or
has untracked files (staged-but-uncommitted files are fine).

    python3 scripts/check_ui_generated.py [--params build/params.json]

Exit 0 when current, 1 otherwise. Standard library only.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib

sys.dont_write_bytecode = True
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GENERATED = ("ui/art", "src/ui/generated")


def decode_png(data):
    """(IHDR bytes, inflated scanlines) of a PNG; raises ValueError when it is not one."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, ihdr, idat = 8, None, []
    while pos + 8 <= len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            ihdr = body
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break
        pos += 12 + n
    if ihdr is None or not idat:
        raise ValueError("PNG without IHDR/IDAT")
    return ihdr, zlib.decompress(b"".join(idat))


def same_content(name, a, b):
    """True when two generated files are equivalent (decoded pixels for .png, exact bytes otherwise)."""
    if name.endswith(".png"):
        try:
            return decode_png(a) == decode_png(b)
        except (ValueError, zlib.error):
            return False
    return a == b


def tree_files(root):
    out = {}
    for dirpath, _, files in os.walk(root):
        for fn in files:
            p = os.path.join(dirpath, fn)
            with open(p, "rb") as f:
                out[os.path.relpath(p, root)] = f.read()
    return out


def compare_trees(generated_root, repo_root):
    """Problems between a freshly generated tree and the repository's generated dirs."""
    problems = []
    for rel in GENERATED:
        gen = tree_files(os.path.join(generated_root, rel))
        have = tree_files(os.path.join(repo_root, rel))
        for name in sorted(set(gen) | set(have)):
            path = os.path.join(rel, name)
            if name not in have:
                problems.append("missing:  %s" % path)
            elif name not in gen:
                problems.append("obsolete: %s" % path)
            elif not same_content(name, gen[name], have[name]):
                problems.append("stale:    %s" % path)
    return problems


def git_lines(*args):
    p = subprocess.run(["git"] + list(args), cwd=ROOT, stdout=subprocess.PIPE, universal_newlines=True)
    return [l for l in p.stdout.splitlines() if l]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--params", default=os.path.join(ROOT, "build", "params.json"))
    args = ap.parse_args(argv)
    with tempfile.TemporaryDirectory(prefix="check_ui_generated_") as tmp:
        p = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "build_ui.py"), "--params", args.params,
                            "--root", tmp, "--no-preview", "--quiet"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        if p.returncode != 0:
            print("build_ui.py failed:\n" + p.stdout)
            return 1
        problems = compare_trees(tmp, ROOT)
    problems += ["not staged: %s" % n for n in git_lines("diff", "--name-only", "--", *GENERATED)]
    problems += ["untracked:  %s" % n for n in git_lines("ls-files", "--others", "--exclude-standard", "--", *GENERATED)]
    if problems:
        print("Generated UI files are stale or not in git (re-run scripts/build_ui.py and git add them):")
        for q in problems:
            print("  " + q)
        return 1
    print("generated UI is current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
