"""Checks for scripts/build_ui.py (the shared Game Boy UI generator).

Run from the repository root (scripts/check.sh does, after the native build and the
host_sim params dump):  python3 -m unittest discover -s tests/ui -v

Needs build/params.json (host_sim dump); the tests are skipped without it.
Standard library only.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True  # no __pycache__ inside the vendored SDK submodule
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "scripts", "build_ui.py")
PARAMS = os.path.join(ROOT, "build", "params.json")
sys.path.insert(0, os.path.join(ROOT, "vendor", "mpc-plugin-builder-sdk", "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import gen_ui  # noqa: E402
import make_ui_kit  # noqa: E402
import check_ui_generated  # noqa: E402

KIT_NAMES = ("knob_uni", "knob_bi", "fader_h_uni", "fader_h_bi", "toggle_on", "toggle_off",
             "seg2_on", "seg2_off", "seg3_on", "seg3_off", "seg4_on", "seg4_off")


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    assert head[:8] == b"\x89PNG\r\n\x1a\n", path
    return struct.unpack(">II", head[16:24])


def pairs(items):
    """object_pairs_hook keeping duplicate keys (as a list of pairs)."""
    return list(items)


def walk(node):
    yield node
    if isinstance(node, list):
        for v in node:
            if isinstance(v, tuple) and len(v) == 2:
                yield from walk(v[1])
            else:
                yield from walk(v)


def get(node, key):
    for k, v in node:
        if k == key:
            return v
    raise KeyError(key)


def tree_files(root):
    out = {}
    for dirpath, _, files in os.walk(root):
        for fn in files:
            p = os.path.join(dirpath, fn)
            with open(p, "rb") as f:
                out[os.path.relpath(p, root)] = f.read()
    return out


@unittest.skipUnless(os.path.isfile(PARAMS), "build/params.json missing (run the native build and host_sim dump)")
class BuildUiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="build_ui_test_")
        cls.runs = []
        for i in (1, 2):
            base = os.path.join(cls.tmp, "run%d" % i)
            p = subprocess.run([sys.executable, SCRIPT, "--params", PARAMS, "--root", os.path.join(base, "root"),
                                "--skins", os.path.join(base, "skins"), "--preview-dir", os.path.join(base, "preview")],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, cwd=ROOT)
            cls.runs.append((base, p.returncode, p.stdout))
        cls.base = cls.runs[0][0]
        cls.skins = os.path.join(cls.base, "skins")
        with open(os.path.join(cls.skins, "TUI.json"), "r", encoding="utf-8") as f:
            cls.tui_text = f.read()
        cls.tui = json.loads(cls.tui_text, object_pairs_hook=pairs)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_runs_clean(self):
        for base, rc, out in self.runs:
            self.assertEqual(rc, 0, out)
            bad = [l for l in out.splitlines() if "WARNING" in l or "NOTE" in l]
            self.assertEqual(bad, [], "package_mpc.py would fail on these lines")

    def test_byte_identical(self):
        a, b = (tree_files(r[0]) for r in self.runs)
        self.assertEqual(sorted(a), sorted(b))
        for name in a:
            self.assertEqual(a[name], b[name], "%s differs between two runs" % name)

    def test_matches_committed_tree(self):
        # PNGs compare by decoded pixels (a different zlib may compress differently); .inc exactly.
        problems = check_ui_generated.compare_trees(os.path.join(self.base, "root"), ROOT)
        self.assertEqual(problems, [], "re-run scripts/build_ui.py")

    def test_png_compare_ignores_compression(self):
        import zlib
        a = check_ui_generated.tree_files(os.path.join(self.base, "root", "ui", "art"))["knob_uni.png"]
        ihdr, raw = check_ui_generated.decode_png(a)
        def chunk(tag, data):
            body = tag + data
            return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        b = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b"")
        self.assertNotEqual(a, b)
        self.assertTrue(check_ui_generated.same_content("x.png", a, b))
        self.assertFalse(check_ui_generated.same_content("x.png", a, a[:-20] + b"\0" * 20))
        self.assertFalse(check_ui_generated.same_content("x.inc", b"a", b"b"))

    def test_background_first_in_every_tab(self):
        tabs = get(get(self.tui, "pageData"), "tabs")
        self.assertEqual(len(tabs), 6)
        for tab in tabs:
            first = get(get(tab, "componentDefinition"), "componentsData")[0]
            cd = get(first, "componentData")
            self.assertEqual(get(cd, "type"), "Image")
            data = get(cd, "data")
            self.assertEqual([k for k, _ in data], ["version", "imageType", "colour", "image"])
            self.assertEqual((get(data, "version"), get(data, "imageType"), get(data, "colour")), (2, "Regular", "0"))
            bounds = get(first, "bounds")
            self.assertEqual(get(bounds, "bounds"), "0 0 1280 628")
            self.assertEqual(get(bounds, "acceptsHWFocus"), "No")
            self.assertEqual(png_size(os.path.join(self.skins, get(data, "image"))), (1280, 628))

    def test_label_fonts(self):
        labels = 0
        for node in walk(self.tui):
            if isinstance(node, list) and node and all(isinstance(p, tuple) for p in node):
                keys = [k for k, _ in node]
                if "textStyle" in keys:
                    ts = get(node, "textStyle")
                    font = get(ts, "font")
                    self.assertEqual(get(font, "name"), "Nexa")
                    self.assertEqual(get(font, "style"), "Bold")
                    self.assertEqual(get(ts, "case"), "Upper Case")
                    if get(node, "type") == "Text":  # segment captions
                        self.assertEqual(get(ts, "colour"), "ffe0f8d0")
                    labels += 1
        self.assertGreater(labels, 20)

    def test_overlay_duplicate_version_keys_survive(self):
        # ui-reference 1.8: the overlays need both "version" keys; a json round-trip would drop one.
        self.assertIn('"overlayType": "Knob With Header"', self.tui_text)
        for node in walk(self.tui):
            if isinstance(node, list) and node and all(isinstance(p, tuple) for p in node):
                keys = [k for k, _ in node]
                if "overlayType" in keys or "withNumbers" in keys:
                    self.assertEqual(keys[:2], ["version", "version"])

    def test_kit_assets_match_gen_ui(self):
        table = make_ui_kit.asset_table()
        for name in KIT_NAMES:
            _, (w, h), frames = table[name]
            got = png_size(os.path.join(ROOT, "ui", "art", name + ".png"))
            self.assertEqual(got, (w, h * frames), name)
        for n, (w, _) in gen_ui.SEGMENTS.items():
            self.assertEqual(png_size(os.path.join(ROOT, "ui", "art", "seg%d_on.png" % n)), (w, 40))

    def test_referenced_images_exist(self):
        images = set()
        for node in walk(self.tui):
            if isinstance(node, list) and node and all(isinstance(p, tuple) for p in node):
                for k, v in node:
                    if k in ("filmStrip", "onImage", "offImage", "image") and isinstance(v, str) and v:
                        images.add(v)
        self.assertTrue(images)
        for img in images:
            self.assertTrue(os.path.isfile(os.path.join(self.skins, img)), img)
            self.assertTrue(os.path.isfile(os.path.join(ROOT, "ui", "art", img)), img)

    def test_previews_written(self):
        names = sorted(os.listdir(os.path.join(self.base, "preview")))
        self.assertEqual(names, ["main.png", "noise.png", "pulse1.png", "pulse2.png", "tabbar.png", "table.png", "wave.png"])

    def test_layout_header_compiles(self):
        cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if not cxx:
            self.skipTest("no C++ compiler")
        src = os.path.join(self.tmp, "layout_check.cpp")
        with open(src, "w") as f:
            f.write('#include "ui_layout.h"\n'
                    "static_assert(mpcboy::ui::kNumPages == 6, \"six pages\");\n"
                    "static_assert(mpcboy::ui::kEditorHeight == 704, \"628 + tab bar\");\n"
                    "int main() { return mpcboy::ui::kPages[0].numControls > 0 ? 0 : 1; }\n")
        p = subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                            "-I", os.path.join(ROOT, "src", "ui"), src],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        self.assertEqual(p.returncode, 0, p.stdout)


if __name__ == "__main__":
    unittest.main()
