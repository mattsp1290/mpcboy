#!/usr/bin/env python3
"""Assemble, check and zip the MPC plugin folder (WP9).

Standard library only (Python 3.9+). Stops on the first failing step.

  1. require the three built libraries and the native host_sim
  2. read plugin/plugin.json, build the folder name, clear dist/<folder>
  3. host_sim dump -> build/params.json, identity check against plugin.json
  4. host_sim test + WAV peak check (-30 .. -0.1 dBFS)
  5. copy plugin.json, VST/*.so, Content/Import/README.txt, write version.xml
  6. gen_ui.py (plugin/ui.json, kit cache build/ui-kit); no WARNING/NOTE allowed
  7. validate.py --strict
  8. zip dist/<folder> + INSTALL.txt into dist/<name>-<version>.zip, print SHA-256
"""
import array
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import wave
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SDK_TOOLS = os.path.join(ROOT, "vendor", "mpc-plugin-builder-sdk", "tools")
STRIP = '"#@,;:<>*^|?\\/'
PEAK_MIN_DB, PEAK_MAX_DB = -30.0, -0.1

_step = 0


class Fail(Exception):
    pass


def step(title):
    global _step
    _step += 1
    print("\n== step %d: %s" % (_step, title), flush=True)


def run(cmd, capture_stdout=False, cwd=None):
    """Runs a command; returns stdout (text) when capture_stdout. Raises Fail on a non-zero exit."""
    print("   $ " + " ".join('"%s"' % c if " " in c else c for c in cmd), flush=True)
    p = subprocess.run(cmd, cwd=cwd or ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT if not capture_stdout else subprocess.PIPE,
                       universal_newlines=True)
    if capture_stdout:
        if p.stderr.strip():
            for line in p.stderr.strip().splitlines()[-5:]:
                print("   | " + line)
        if p.returncode != 0:
            raise Fail("command exited %d" % p.returncode)
        return p.stdout
    out = p.stdout or ""
    for line in out.splitlines():
        print("   | " + line)
    if p.returncode != 0:
        raise Fail("command exited %d" % p.returncode)
    return out


def folder_name(manufacturer, name):
    clean = lambda s: "".join(c for c in s if c not in STRIP)
    m, n = clean(manufacturer), clean(name)
    return ("%s - VST - %s" % (m, n)) if m else ("VST - %s" % n)


def four_part_version(v):
    parts = [p for p in re.split(r"[.\-]", v) if p.isdigit()][:4]
    while len(parts) < 4:
        parts.append("0")
    return ".".join(parts)


def wav_peak_db(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise Fail("expected a 16-bit WAV, got %d-bit" % (8 * w.getsampwidth()))
        data = array.array("h")
        data.frombytes(w.readframes(w.getnframes()))
        if sys.byteorder == "big":
            data.byteswap()
    if not data:
        raise Fail("test.wav is empty")
    peak = max(max(data), -min(data)) / 32768.0
    return -999.0 if peak <= 0 else 20.0 * math.log10(peak)


def main():
    build = os.path.join(ROOT, "build")

    step("require the built artefacts (and refuse probe or stale builds)")
    native = None
    for cand in ("mpcboy.dylib", "mpcboy.so"):
        p = os.path.join(build, "native", cand)
        if os.path.isfile(p):
            native = p
            break
    arm32 = os.path.join(build, "arm32", "mpcboy-arm32.so")
    arm64 = os.path.join(build, "arm64", "mpcboy-arm64.so")
    host_sim = os.path.join(build, "native", "host_sim")
    missing = [p for p in (native or os.path.join(build, "native", "mpcboy.dylib"), arm32, arm64, host_sim) if not os.path.isfile(p)]
    if missing:
        raise Fail("missing build output (run the three presets first): " + ", ".join(os.path.relpath(m, ROOT) for m in missing))
    for p in (native, arm32, arm64, host_sim):
        print("   found " + os.path.relpath(p, ROOT))
    # A CPU-probe build (MPCBOY_CPU_PROBE=ON) holds a note by itself and adds a
    # parameter: it must never be shipped. Probe builds are named mpcboy-probe-*,
    # but a cache that was switched to ON in place is caught here too, and an arm
    # artefact older than the native build means a preset was not rebuilt.
    for lib in (native, arm32, arm64):
        cache = os.path.join(os.path.dirname(lib), "CMakeCache.txt")
        if os.path.isfile(cache):
            with open(cache, encoding="utf-8") as f:
                if re.search(r"^MPCBOY_CPU_PROBE:BOOL=ON$", f.read(), re.M):
                    raise Fail("%s was configured with MPCBOY_CPU_PROBE=ON" % os.path.relpath(os.path.dirname(lib), ROOT))
        if "probe" in os.path.basename(lib):
            raise Fail("%s is a probe build" % os.path.relpath(lib, ROOT))
    for lib in (arm32, arm64):
        if os.path.getmtime(lib) < os.path.getmtime(native):
            raise Fail("%s is older than the native build: rebuild all presets" % os.path.relpath(lib, ROOT))

    step("read plugin/plugin.json, prepare dist/<folder>")
    with open(os.path.join(ROOT, "plugin", "plugin.json"), "r", encoding="utf-8") as f:
        pj = json.load(f)
    name, manufacturer, version = pj["name"], pj["manufacturer"], pj["version"]
    folder = folder_name(manufacturer, name)
    dist = os.path.join(ROOT, "dist")
    out_dir = os.path.join(dist, folder)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    print("   folder: %s" % folder)
    print("   version: %s" % version)

    step("host_sim dump and identity check")
    os.makedirs(build, exist_ok=True)
    params_path = os.path.join(build, "params.json")
    dump = run([host_sim, native, "dump"], capture_stdout=True)
    with open(params_path, "w", encoding="utf-8") as f:
        f.write(dump)
    d = json.loads(dump)
    expect = {"name": pj["name"], "vendor": pj["manufacturer"], "uid": pj["uid"]}
    for k, v in expect.items():
        if d.get(k) != v:
            raise Fail("plugin reports %s=%r but plugin.json says %r" % (k, d.get(k), v))
    if any(p.get("name") == "CPU us" for p in d["params"]):
        raise Fail("the native build is a CPU-probe build (it has the 'CPU us' parameter)")
    # SDK encoding: version 1000 = 1.0.0.0, i.e. a*1000 + b*100 + c*10 + d.
    v4 = [int(x) for x in four_part_version(version).split(".")]
    expect_ver = v4[0] * 1000 + v4[1] * 100 + v4[2] * 10 + v4[3]
    if d.get("version") != expect_ver:
        raise Fail("plugin reports version %r but plugin.json says %s (= %d)" % (d.get("version"), version, expect_ver))
    print("   dump ok: name=%s vendor=%s uid=%s, %d parameters, %d programs"
          % (d["name"], d["vendor"], d["uid"], len(d["params"]), d.get("numPrograms", 0)))

    step("host_sim test and WAV peak check")
    wav_path = os.path.join(build, "test.wav")
    if os.path.exists(wav_path):
        os.remove(wav_path)
    out = run([host_sim, native, "test", "--seconds", "4", "--notes", "48,60", "--playing", "--wav", wav_path])
    if not os.path.isfile(wav_path):
        raise Fail("host_sim wrote no WAV")
    peak = wav_peak_db(wav_path)
    print("   WAV peak: %.2f dBFS (required %.1f .. %.1f)" % (peak, PEAK_MIN_DB, PEAK_MAX_DB))
    if not (PEAK_MIN_DB < peak < PEAK_MAX_DB):
        raise Fail("WAV peak %.2f dBFS is outside %.1f .. %.1f (silent or clipping)" % (peak, PEAK_MIN_DB, PEAK_MAX_DB))

    step("assemble the folder")
    shutil.copy2(os.path.join(ROOT, "plugin", "plugin.json"), os.path.join(out_dir, "plugin.json"))
    os.makedirs(os.path.join(out_dir, "VST"))
    for lib in (arm32, arm64):
        shutil.copy2(lib, os.path.join(out_dir, "VST", os.path.basename(lib)))
    for key in ("file32", "file64"):
        if not os.path.isfile(os.path.join(out_dir, pj[key])):
            raise Fail("plugin.json %s -> %s does not exist in the folder" % (key, pj[key]))
    imp = os.path.join(out_dir, "Content", "Import")
    os.makedirs(imp)
    with open(os.path.join(imp, "README.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("Put LSDJ 9.4.0 .sav files here (up to 64, directly in this folder, no sub-folders).\n"
                "On the TABLE page pick one with Imp File, then tap Imp Go. It replaces all 16 tables\n"
                "and 16 waves (the first 16 tables in the file, waves 0-15). Imp Undo restores the\n"
                "previous content; Imp Rescan re-reads this folder after you add files.\n")
    with open(os.path.join(out_dir, "version.xml"), "w", encoding="utf-8", newline="\n") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<plugincontent version="1.0">\n'
                "    <version>%s</version>\n"
                "    <uiversion>%s</uiversion>\n"
                "</plugincontent>\n" % (four_part_version(version), four_part_version(version)))
    print("   version.xml: version and uiversion %s" % four_part_version(version))

    step("generate the skins (gen_ui.py)")
    skins = os.path.join(out_dir, "Plugin Skins")
    cmd = [sys.executable, os.path.join(SDK_TOOLS, "gen_ui.py"), "--params", params_path,
           "--out", skins, "--kit-cache", os.path.join(build, "ui-kit")]
    ui = os.path.join(ROOT, "plugin", "ui.json")
    if os.path.isfile(ui):
        cmd[2:2] = ["--ui", ui]
    else:
        print("   no plugin/ui.json: automatic layout")
    out = run(cmd)
    bad = [l for l in out.splitlines() if "WARNING" in l or "NOTE" in l]
    if bad:
        raise Fail("gen_ui.py printed WARNING/NOTE lines:\n" + "\n".join(bad))

    step("validate.py --strict")
    run([sys.executable, os.path.join(SDK_TOOLS, "validate.py"), out_dir, "--params", params_path, "--strict"])

    step("zip")
    install = os.path.join(ROOT, "scripts", "INSTALL.txt")
    if not os.path.isfile(install):
        raise Fail("scripts/INSTALL.txt is missing")
    zip_path = os.path.join(dist, "%s-%s.zip" % (name, version))
    if os.path.exists(zip_path):
        os.remove(zip_path)
    entries = []   # (archive name, file path)
    for dirpath, dirnames, filenames in os.walk(out_dir):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            entries.append((os.path.join(folder, os.path.relpath(full, out_dir)).replace(os.sep, "/"), full))
    entries.sort()
    entries.append(("INSTALL.txt", install))
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, full in entries:
            zi = zipfile.ZipInfo(arc, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            with open(full, "rb") as f:
                z.writestr(zi, f.read())
    names = [a for a, _ in entries]
    required = ["%s/plugin.json" % folder, "%s/version.xml" % folder, "%s/%s" % (folder, pj["file32"]),
                "%s/%s" % (folder, pj["file64"]), "%s/Content/Import/README.txt" % folder,
                "%s/Plugin Skins/TUI.json" % folder, "%s/Plugin Skins/Q-Links.json" % folder, "INSTALL.txt"]
    for r in required:
        if r not in names:
            raise Fail("zip is missing " + r)
    h = hashlib.sha256()
    with open(zip_path, "rb") as f:
        h.update(f.read())
    print("   %d files -> %s (%d bytes)" % (len(names), os.path.relpath(zip_path, ROOT), os.path.getsize(zip_path)))
    print("   SHA-256 %s  %s" % (h.hexdigest(), os.path.basename(zip_path)))
    print("\npackage_mpc.py: OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print("\npackage_mpc.py: FAILED at step %d: %s" % (_step, e), file=sys.stderr)
        sys.exit(1)
