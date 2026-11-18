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
COLOURS = ("pu1", "pu2", "wav", "noi", "dmg")
PAGE_COLOUR = {"MAIN": "dmg", "PULSE1": "pu1", "PULSE2": "pu2", "WAVE": "wav", "NOISE": "noi", "TABLE": "dmg"}


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


def params_json():
    with open(PARAMS, "r", encoding="utf-8") as f:
        return json.load(f)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


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
        a = check_ui_generated.tree_files(os.path.join(self.base, "root", "ui", "art"))["knob_uni_pu1.png"]
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
                    if get(node, "type") == "Text":  # segment captions: one colour, reads on both rubber states
                        self.assertEqual(get(ts, "colour"), "ffeceae4")
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
        # Every kit variant keeps gen_ui's size and frame count (knob_uni_pu1.png is a knob_uni).
        table = make_ui_kit.asset_table()
        art = os.path.join(ROOT, "ui", "art")
        seen = 0
        for f in sorted(os.listdir(art)):
            base = f[:-4]
            for c in COLOURS:
                if base.endswith("_" + c) and base[:-len(c) - 1] in KIT_NAMES:
                    base = base[:-len(c) - 1]
            if base in KIT_NAMES:
                _, (w, h), frames = table[base]
                self.assertEqual(png_size(os.path.join(art, f)), (w, h * frames), f)
                seen += 1
        self.assertGreater(seen, 20)
        for n, (w, _) in gen_ui.SEGMENTS.items():
            if n != 3:
                self.assertEqual(png_size(os.path.join(art, "seg%d_on_dmg.png" % n if n == 2 else "seg%d_on_wav.png" % n)), (w, 40))

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

    # ---- redesign post-processes ------------------------------------------------------------
    def tabs(self):
        return get(get(self.tui, "pageData"), "tabs")

    def entries(self, tab):
        return get(get(tab, "componentDefinition"), "componentsData")

    def test_kit_definitions_cloned_per_colour(self):
        defs = get(get(get(self.tui, "pageData"), "componentDefinitions"), "localComponentDefinitions")
        names = [get(d, "key") for d in defs]
        self.assertEqual(len(names), len(set(names)))
        kit_files = {n + ".png" for n in KIT_NAMES}
        for d in defs:
            for node in walk(get(d, "value")):
                if isinstance(node, list) and node and all(isinstance(p, tuple) for p in node):
                    for k, v in node:
                        if k in ("filmStrip", "onImage") and isinstance(v, str):
                            # coloured kit images are always a colour variant; never the bare gen_ui name
                            self.assertNotIn(v, kit_files - {"toggle_off.png", "seg2_off.png", "seg3_off.png", "seg4_off.png"})
        used = set()
        for tab in self.tabs():
            colour = PAGE_COLOUR[get(tab, "tabName")]
            for e in self.entries(tab):
                t = get(get(e, "componentData"), "type")
                if t in names:
                    used.add(t)
                    if "_" in t and get(tab, "tabName") != "MAIN":
                        self.assertTrue(t.endswith("_" + colour), (get(tab, "tabName"), t))
        # Every definition is used (the shared gen_ui originals were replaced), apart from the
        # slider template gen_ui always keeps and the overlays.
        self.assertEqual(set(names) - used, {"sliderWithFocus", "knob overlay", "knob overlay bipolar", "menu overlay"})

    def test_main_strips_use_channel_colours(self):
        main = self.tabs()[0]
        types = {get(get(e, "componentData"), "name"): get(get(e, "componentData"), "type") for e in self.entries(main)}
        self.assertEqual(types["PU1 Level"], "faderUni_pu1")
        self.assertEqual(types["PU2 Level"], "faderUni_pu2")
        self.assertEqual(types["NOI Level"], "faderUni_noi")
        self.assertEqual(types["WAV Level"], "radio4_wav")
        self.assertEqual(types["Volume"], "faderUni_dmg")

    def test_shared_slot(self):
        main = self.entries(self.tabs()[0])
        by_name = {get(get(e, "componentData"), "name"): e for e in main}
        rate, free = by_name["Table Rate"], by_name["Free Rate"]
        self.assertEqual(get(get(rate, "bounds"), "bounds"), get(get(free, "bounds"), "bounds"))
        hr = get(get(rate, "bounds"), "additionalInvalidatingHandles")
        hf = get(get(free, "bounds"), "additionalInvalidatingHandles")
        sync = [p for p in params_json()["params"] if p["name"] == "Table Sync"][0]
        self.assertEqual(sorted(hr + hf), ["IndexedEnabling/0/2/Parameter %d" % sync["index"],
                                           "IndexedEnabling/1/2/Parameter %d" % sync["index"]])
        # The overlapped control follows its occupant directly.
        names = [get(get(e, "componentData"), "name") for e in main]
        self.assertEqual(names.index("Free Rate"), names.index("Table Rate") + 1)

    def test_qlinks_bank_by_row(self):
        with open(os.path.join(self.skins, "Q-Links.json"), "r", encoding="utf-8") as f:
            q = json.load(f, object_pairs_hook=pairs)
        entries = get(get(q, "Screen Mode Q-Links"), "map")
        self.assertEqual(len(entries), 6)
        for e in entries:
            self.assertEqual([k for k, _ in e], ["Tab", "SubTab", "Bank Direction", "Q-Links"])
            self.assertEqual(get(e, "Bank Direction"), "Row")
        # One rectangle per screen row, in bank order (Row = bottom row first), each wrapping that
        # row's four slots and nothing of the next row.
        for tab in self.tabs():
            rects = [tuple(int(v) for v in r.split()) for r in get(tab, "qlinkBoundsData")]
            self.assertEqual(len(rects), 4)
            for b, (x, y, w, h) in enumerate(rects):
                row = 3 - b
                sy = gen_ui.SLOT_Y[row]
                self.assertLess(y, sy)
                self.assertGreater(y + h, sy + gen_ui.SLOT_H)
                self.assertLessEqual(x, gen_ui.SLOT_X[0])
                self.assertGreaterEqual(x + w, gen_ui.SLOT_X[3] + gen_ui.SLOT_W)
                if row > 0:
                    self.assertGreaterEqual(y, gen_ui.SLOT_Y[row - 1] + gen_ui.SLOT_H)
            self.assertEqual([k for k, _ in tab],
                             ["version", "tabName", "fnKeyIndex", "fnKeySubIndex", "componentDefinition", "qlinkBoundsData"])
        # The Q-Link of each slot is gen_ui's slot_qlink.
        main_q = dict(get(entries[0], "Q-Links"))
        idx = {p["name"]: p["index"] for p in params_json()["params"]}
        self.assertEqual(main_q["Q-Link 13"], idx["PU1 Level"])
        self.assertEqual(main_q["Q-Link 4"], idx["Note Off"])
        self.assertEqual(main_q["Q-Link 6"], idx["Table Rate"])

    def dynamics(self):
        out = []
        for tab in self.tabs():
            for e in self.entries(tab):
                cd = get(e, "componentData")
                if get(cd, "name").startswith("dyn "):
                    out.append((get(tab, "tabName"), e))
        return out

    def test_dynamics_json_shape(self):
        idx = {p["name"]: p for p in params_json()["params"]}
        dyn = self.dynamics()
        self.assertEqual(len(dyn), 17 + 11)
        for tab, e in dyn:
            self.assertEqual([k for k, _ in e], ["version", "componentData", "handle remapping", "bounds"])
            cd = get(e, "componentData")
            self.assertEqual([k for k, _ in cd], ["version", "name", "type", "data"])
            param = idx[get(cd, "name")[4:]]
            remap = get(e, "handle remapping")
            self.assertEqual(get(remap, "version"), 1)
            self.assertEqual([(get(m, "key"), get(m, "value")) for m in get(remap, "map")],
                             [("Data", "Parameter %d" % param["index"])])
            b = get(e, "bounds")
            self.assertEqual([k for k, _ in b],
                             ["version", "acceptsHWFocus", "showWhenDataModelInvalid", "whenVisible", "boundsType", "bounds"])
            self.assertEqual(get(b, "acceptsHWFocus"), "No")
            data = get(cd, "data")
            if get(cd, "type") == "Image":
                # ui-reference 6.6: version, imageType, colour, image, numFrames, handleName
                self.assertEqual([k for k, _ in data], ["version", "imageType", "colour", "image", "numFrames", "handleName"])
                self.assertEqual((get(data, "version"), get(data, "imageType"), get(data, "handleName")), (2, "FilmStrip", "Data"))
                self.assertEqual(get(data, "numFrames"), len(param["choices"]))
                w, h = png_size(os.path.join(self.skins, get(data, "image")))
                bx, by, bw, bh = (int(v) for v in get(b, "bounds").split())
                self.assertEqual((w, h), (bw, bh * get(data, "numFrames")))
            else:
                # ui-reference 6.3: version, textStyle, type, handleName
                self.assertEqual(get(cd, "type"), "Label")
                self.assertEqual([k for k, _ in data], ["version", "textStyle", "type", "handleName"])
                self.assertEqual((get(data, "type"), get(data, "handleName")), ("Value", "Data"))
        # The dynamics sit right after the background, before the controls (under the focus rings).
        for tab in self.tabs():
            names = [get(get(e, "componentData"), "name") for e in self.entries(tab)]
            n = sum(1 for x in names if x.startswith("dyn "))
            self.assertTrue(all(x.startswith("dyn ") for x in names[1:1 + n]))

    def test_dynamics_flags_off(self):
        # The static fallback: no Image film strips or Value labels; the backgrounds carry the art.
        with open(os.path.join(ROOT, "ui", "mpcboy_ui.json"), "r", encoding="utf-8") as f:
            spec = json.load(f)
        spec["gb"]["mpcDynamics"] = {"valueLabels": False, "filmStrips": False}
        base = os.path.join(self.tmp, "flags_off")
        os.makedirs(base)
        ui = os.path.join(base, "ui.json")
        with open(ui, "w", encoding="utf-8") as f:
            json.dump(spec, f)
        p = subprocess.run([sys.executable, SCRIPT, "--params", PARAMS, "--ui", ui, "--root", os.path.join(base, "root"),
                            "--skins", os.path.join(base, "skins"), "--no-preview", "--quiet"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, cwd=ROOT)
        self.assertEqual(p.returncode, 0, p.stdout)
        with open(os.path.join(base, "skins", "TUI.json"), "r", encoding="utf-8") as f:
            text = f.read()
        self.assertNotIn('"dyn ', text)
        self.assertNotIn('"imageType": "FilmStrip"', text)
        with open(os.path.join(base, "root", "src", "ui", "generated", "ui_layout.inc"), "r", encoding="utf-8") as f:
            inc = f.read()
        self.assertNotIn("Dynamic kPage", inc)
        # Backgrounds differ from the live build: the default frames are baked in.
        a = check_ui_generated.decode_png(read_bytes(os.path.join(base, "root", "ui", "art", "bg_pulse1.png")))
        b = check_ui_generated.decode_png(read_bytes(os.path.join(ROOT, "ui", "art", "bg_pulse1.png")))
        self.assertNotEqual(a, b)

    def test_backgrounds_have_no_dot_grid_and_use_the_palette(self):
        # The bezel colour fills the field outside the slots (a dot grid would break the run).
        data = read_bytes(os.path.join(ROOT, "ui", "art", "bg_main.png"))
        _, raw = check_ui_generated.decode_png(data)
        stride = 1 + 1280 * 4
        row = raw[170 * stride + 1:171 * stride]   # a row in the gap between rows 0 and 1, right of the legend
        px = {bytes(row[i:i + 4]) for i in range(900 * 4, 1270 * 4, 4)}
        self.assertTrue(px <= {bytes((0x2a, 0x2c, 0x3b, 255)), bytes((0x4a, 0x4e, 0x62, 255))}, px)

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
