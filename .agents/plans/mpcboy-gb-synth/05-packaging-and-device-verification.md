# 05 — Packaging and device verification

Covers WP9 and WP10.

## WP9 — MPC plugin folder, host simulator, validator, zip

### Goal and prerequisite state

Prerequisite: WP7 is complete (the plugin builds for all three presets). WP8's `ui.json` may still be partial; the packaging script must work with or without it. The G-EDIT spike in WP8 needs this package's folder build, so WP9's folder assembly lands before WP8's spike (see [07-execution-handoff.md](07-execution-handoff.md)).

Goal: one command produces the exact folder MPC OS 3.10.1+ loads, checks it with the SDK tools, and zips it.

### Repository evidence (SDK at `febd2ca`)

- `docs/plugin-folder-format.md`: folder `"<manufacturer> - VST - <name>"`; `plugin.json` keys `name`, `manufacturer`, `uid`, `format: "vst2"`, `kind`, `numInputs`, `numOutputs` (> 0), `file32`, `file64`, `version`, `category`; `version.xml` with `<version>` and `<uiversion>0.1.0.0</uiversion>` (never `1`); `Plugin Skins/` with `TUI.json`, `Q-Links.json`, images. The folder name, `plugin.json` strings and the binary's reported vendor/name/uid must match, or the plugin gets the default UI.
- `tools/host-sim/host_sim.cpp` is one C++17 file. Commands `dump` (prints params.json) and `test --seconds N --block N --wav F --notes a,b --playing`. Exit 0 pass, 1 FAIL, 2 crash.
- `tools/gen_ui.py --params --ui --out [--kit-cache]`.
- `tools/validate.py <folder> [--params params.json] [--strict]`: checks ELF types, the exported symbol, library dependencies, glibc ≤ 2.39 and `GLIBCXX` ≤ 3.4.32.
- `tools/package.py` expects a project built by `tools/build.py`, which this repository does not use (D2), so it is not reused.

### Change surface (all `new`)

| Item | Description |
|---|---|
| `plugin/plugin.json` | `{"name": "mpcboy", "manufacturer": "mpcboy", "version": "0.1.0", "format": "vst2", "kind": "instrument", "uid": "MbGb", "category": "Synth", "numInputs": 0, "numOutputs": 2, "file32": "VST/mpcboy-arm32.so", "file64": "VST/mpcboy-arm64.so", "description": "Game Boy mono synth: 4 layered DMG channels with BPM-synced LSDJ-style tables.", "license": "MIT"}`. Keep in sync with `PluginInfo` (WP7). |
| CMake target `host_sim` | Native executable from `vendor/mpc-plugin-builder-sdk/tools/host-sim/host_sim.cpp`, built in the `native` preset. |
| `scripts/package_mpc.py` | Python 3.9+, standard library only. Steps below. |
| `scripts/INSTALL.txt` | User instructions: MPC OS 3.10.1+, Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support, copy the folder into `<drive>/VST2 Plugins/`, find it under External Plugins. |
| `README.md` (existing, rewrite) | What the plugin is, the install steps above, the build command, the test command, and the statement that arm32 builds are validated but not tested on a Gen1 device. |

`scripts/package_mpc.py` steps, each checked, stopping on the first failure:

1. Require `build/native/mpcboy.dylib` (or `.so`), `build/arm32/mpcboy-arm32.so`, `build/arm64/mpcboy-arm64.so`, `build/native/host_sim`.
2. Read `name` and `manufacturer` from `plugin/plugin.json`. Build the folder name with the SDK's character-stripping rule (`" # @ , ; : < > * ^ | ? \ /` removed). Output: `dist/<folder>/`. Delete only that folder before writing it.
3. Run `host_sim <native lib> dump > build/params.json`. Fail if the dumped `name`, `vendor` or `uid` differ from `plugin.json`.
4. Run `host_sim <native lib> test --seconds 4 --notes 48,60 --playing --wav build/test.wav`. Fail on exit code ≠ 0. `host_sim` only WARNs on silence (`host_sim.cpp` around line 1152), so then parse `build/test.wav` with Python's `wave` module and fail unless the peak is above −30 dBFS and below −0.1 dBFS. (Layering of all four channels is proven by WP7 test 9, because `host_sim` plays program 0, `Init`, which uses Pulse 1 only.)
5. Copy `plugin.json`, the two `.so` files into `VST/`, create `Content/Import/README.txt` (one line: put LSDJ `.sav` files here), write `version.xml` (`<version>` from `plugin.json` padded to four parts, `<uiversion>0.1.0.0</uiversion>`).
6. Run `gen_ui.py --params build/params.json --ui plugin/ui.json --out "dist/<folder>/Plugin Skins" --kit-cache build/ui-kit`. Without `plugin/ui.json`, omit `--ui` (automatic layout).
7. Run `validate.py "dist/<folder>" --params build/params.json --strict`.
8. Zip `dist/<folder>/` plus `INSTALL.txt` into `dist/mpcboy-<version>.zip`. Print the SHA-256.

`scripts/check.sh` (WP1) calls `package_mpc.py` after the tests.

### Tests and acceptance (S9)

1. `scripts/check.sh` exits 0 from a clean clone (`git submodule update --init` first).
2. The zip contains exactly: `<folder>/plugin.json`, `<folder>/version.xml`, `<folder>/VST/mpcboy-arm32.so`, `<folder>/VST/mpcboy-arm64.so`, `<folder>/Content/Import/README.txt`, `<folder>/Plugin Skins/TUI.json`, `<folder>/Plugin Skins/Q-Links.json`, the kit images, `INSTALL.txt`.
3. Changing `uid` in `plugin.json` without changing the code makes step 3 fail (negative check, run once by hand).
4. Step 4's peak check fails when the plugin is made silent on purpose (negative check, run once by hand).

## WP10 — Live III verification and CPU bench

### Goal and prerequisite state

Prerequisite: WP8 and WP9 are complete. The user has an MPC Live III on MPC OS 3.10.1 or later.

Goal: prove S1, S5, S7 and S8 on the target device and record the evidence.

### Install route

Copy `dist/<folder>/` to `VST2 Plugins/` on a USB drive or SD card, or use the SDK's `tools/mpc_remote.py <ip> add <folder>` (needs the device's developer mode, SDK `docs/install-and-test.md`). Ask the user before enabling developer mode or touching the device; that is their decision.

### Checklist (record results in `docs/DEVICE_TESTS.md`, `new`, and `tested.json`, `new`)

| # | Check | Pass condition |
|---|---|---|
| 1 | Plugin listed | Appears under External Plugins with the generated skin (not the 16-slider default). |
| 2 | Layered mono | `Stack4` program: one pad plays all four channels; a second pad while holding the first does not add a voice. |
| 3 | Channel solo | Each channel alone audible via the `MAIN` page On switches. |
| 4 | Tempo sync (S5) | `MajArp` on a 1-bar sequence at 60, 120 and 240 BPM: the arpeggio speed doubles with each step; record a 4-bar audio export at 120 BPM and check the arp period against `918.75` frames per tick (24 ticks/beat) within ±2 frames by loading the export into an editor. |
| 5 | Grid phase | `Tbl Phase = Grid` with a looping sequence: the arp restarts in the same place each loop; no audible burst at the loop point. |
| 6 | Transport values | Note what `tempoValid`/`ppqValid` do while stopped: add a temporary debug readout only in a debug build, or infer from behaviour (tables still sync to the project tempo while stopped). Update risk R4 in `00-overview.md` with the result. |
| 7 | State (S7) | Edit two table cells, change a wave selection, save the project, power-cycle, reload: all values return. |
| 8 | Q-Links | On each page, each group of four Q-Links moves the four controls in its frame, including the `MAIN` clock group with `visibleWhen` slots hidden in both `Sync` and `Free`. |
| 9 | CPU (S8) | Build the release code with `MPCBOY_CPU_PROBE` (WP3, same optimisation). Hold a `Stack4` note with all 4 tables running at 240 BPM, 96/beat, 128-frame buffer for 60 s. Pass: `dbg_us` mean ≤ 145 µs (5 % of 2,902 µs) and p99 ≤ 290 µs. Also record the MPC CPU meter before and after inserting the plugin. Repeat at a 32-frame buffer and record (no pass bar). |
| 10 | Stability | Four instances on four tracks play a sequence for 10 minutes without dropouts or a crash. |
| 11 | Note latency | Export a bounce of a one-pad sequence at a known step. Measure note-start to audio-onset in the export. Expected ≈ 118 frames (resampler delay) plus the APU attack. If the MPC compensates reported latency for instruments, decide with the user whether to report `latencySamples = 118`; otherwise keep 0 and state the delay in README. |

### Acceptance

- Every row passes, or a failing row has a recorded cause and a fix merged and re-tested.
- `tested.json` gets `[{"version": "<v>", "device": "MPC Live III (Gen2)", "firmware": "<MPC OS version>", "date": "<YYYY-MM-DD>"}]` (the community catalog's format, for later catalog listing).
- If row 9 fails, profile with the `dbg_us` build before changing anything; WP3's G-CPU fallbacks are the first candidates.
- After WP11 lands, re-run rows 1, 7, 8 and 10 on the WP11 build. The zip shared with others is the build that passed the latest run.
