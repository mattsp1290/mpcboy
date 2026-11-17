# mpcboy

A Game Boy synth for the Akai MPC (built for the MPC Live III). It is a
**mono** instrument: one note at a time drives all four Game Boy sound
channels together (Pulse 1 with sweep, Pulse 2, Wave, Noise), each with its own
settings and its own LSDJ-style **table**. Table rows tick in time with the
project tempo. The sound comes from a real Game Boy APU emulation (SameBoy's
`apu.c`, via the rackboy core).

Status: implemented and tested natively and in the SDK host simulator.
**Not yet run on a device** (see "Verification status").

## Requirements and install

- An MPC running **MPC OS 3.10.1 or later** (third-party plugins are ignored by
  earlier firmware).
- On the MPC, switch on **Preferences -> Enhancements (Beta) -> Enable
  Enhancements** and **External Plugin Support**.
- Copy the folder `mpcboy - VST - mpcboy` from the zip into `VST2 Plugins/` at
  the root of a USB drive, SD card or the MPC's internal drive, then find
  "mpcboy" under External Plugins. The zip's `INSTALL.txt` has the same steps.

The zip contains a 64-bit build (MPC Live III and other 64-bit MPCs) and a
32-bit build (Gen 1 MPCs). **The arm32 build is built and checked with the SDK
validator, but has not been tested on a Gen 1 device.**

## Using it

Six pages: `MAIN` (volume, retrigger, channel on/off switches, table clock),
`PULSE1`, `PULSE2`, `WAVE`, `NOISE` (per-channel settings and the table to
run), and `TABLE` (the table editor, see `docs/EDITOR.md`).

- **Note range:** the played note sets the base pitch, then the channel
  `Transp`/`Fine` controls and each table row offset it. The Pulse channels
  reach down to about MIDI note 36 (about 64 Hz) and the Wave channel one
  octave lower (about MIDI note 24, the Wave channel's `Transp` defaults to -12);
  lower notes clamp to that pitch instead of wrapping. There is no useful upper
  limit short of ultrasonic frequencies. The Noise channel follows the note in
  `Track` mode or stays at `NZ Shape` in `Fixed` mode.
- **Length units:** `Length` is the Game Boy length counter: 0 = off, 1-64
  counts of 1/256 s for the pulse and noise channels, and 4/256 s per count
  for the wave channel.
- **Table clock:** `Sync` ticks `Tbl Rate` times per quarter note of the host
  tempo (default `24/beat LSDJ`, LSDJ's tick at its default groove); `Tbl Phase
  = Note` counts from note-on, `Grid` aligns ticks to the host beat grid while
  the transport plays. `Free` ticks at a fixed `Free Hz` (default 59.73, the
  Game Boy frame rate).
- **Latency:** the output is delayed by about 3 ms. The 48 kHz APU is resampled
  to 44.1 kHz through a polyphase filter that adds about 2.7 ms (117.6 frames),
  and the APU adds roughly 30 more frames (measured: a table tick is audible
  about 146 frames later). The plugin reports a latency of 0. Desktop hosts:
  the resampler delay is 128 APU frames (2.67 ms) at every resampled rate, i.e.
  117.6 host frames at 44.1 kHz, 256 at 96 kHz and 512 at 192 kHz; at 48 kHz
  there is no resampler (0 frames). The APU's own ~30 APU frames come on top.
- **Host sample rate:** the MPC runs at 44.1 kHz. The engine also supports 48,
  96 and 192 kHz (desktop hosts): the tick clock, note timing, smoothers and
  refresh rate follow the host rate. 88.2 kHz and other rates are unsupported
  and the plugin outputs silence there. A rate change (`prepare()`) is applied
  on the audio thread at the start of the next block together with an engine
  reset.
- **Programs:** `Init`, `ChipLead`, `MajArp`, `WaveBass`, `Kick`, `Hat`,
  `Stack4` (all four channels, each with a different table), `Zap`. Programs
  only set parameters; they never change table contents.
- **Factory tables** (slots 1-8): MajArp, MinArp, Octave, Vibrato, KickDrop,
  Fade, Rise, Hat. **Factory waves** (slots 1-8): Saw, Square, Triangle, Sine,
  Pulse25, Pulse12, HalfSine, Organ. Slots 9-16 start empty.

## Importing tables and waves from an LSDJ save

Put LSDJ **9.4.0** `.sav` files (128 KiB, version byte `0x1B`, project slot 0)
into `Content/Import/` inside the plugin folder (`<drive>/VST2 Plugins/mpcboy -
VST - mpcboy/Content/Import/`): up to 64 files, directly in that folder (no
sub-folders; symbolic links and names starting with `.` are skipped). On the
`TABLE` page:

- `Imp File` picks a file (the text is its name without `.sav`, cut at 8
  characters, `-` when the folder is empty or the index is past the list, and
  `ERR <code>` after a failed import until the next scan). The list is read when
  the plugin loads and again with `Imp Rescan`; the file you imported last is
  remembered by name in the project and selected again after a scan.
- `Imp Go` imports it: the first 16 allocated tables become tables 1-16 (in the
  order LSDJ allocated them; missing ones are cleared) and waves 0-15 become
  waves 1-16, replacing all 16 tables and all 16 waves at once. Instruments,
  chains, phrases and songs are ignored. `Imp Go`, `Imp Undo` and `Imp Rescan` are
  buttons: they act on the 0 to 1 edge, ignore a second press within 250 ms and
  switch themselves back off.
- `Imp Undo` brings back the content from just before the last import (one
  level; pressing it again re-applies the import, it is a swap). The undo copy
  is not saved with the project.

Notes and limits: a volume of `00` in an LSDJ table cannot be told from an empty
cell, so it imports as no volume. Commands outside `D E H K L N P R S V W`
(for example `A`, `C`, `T`) and `W` commands that point at a wave above 16 do
nothing; a command the editor cannot show appears as `-` on the `TABLE` page.
Files from other LSDJ versions, files with a damaged block chain and anything
that is not a 128 KiB save are refused and change nothing. The file is read on a
background thread, never on the audio thread; the import is not re-run when a
project loads (the imported tables are already in the saved project).

## Build and test

Requires CMake 3.25+, Ninja, Python 3.9+, and the zig toolchain (installed into
`.tools/` by `scripts/bootstrap-zig.sh`; `scripts/check.sh` runs it when
needed). After cloning, `git submodule update --init`.

```sh
scripts/check.sh                       # everything below, then the package
cmake --preset native && cmake --build --preset native && ctest --preset native
cmake --preset arm64  && cmake --build --preset arm64    # build/arm64/mpcboy-arm64.so
cmake --preset arm32  && cmake --build --preset arm32    # build/arm32/mpcboy-arm32.so
python3 scripts/package_mpc.py         # dist/mpcboy-<version>.zip
```

`package_mpc.py` runs the SDK host simulator (`host_sim dump`/`test` plus a
WAV peak check), generates the skin from `ui/mpcboy_ui.json` with
`scripts/build_ui.py`, runs the SDK validator in `--strict` mode and zips the
folder with its SHA-256.

## UI (MPC skin and Mac editor)

Both builds share one Game Boy–inspired UI (design:
`.agents/plans/mpcboy-gb-ui/00-design.md`). Everything comes from
`ui/mpcboy_ui.json`:

- `"mpc"`: the pages, controls, widgets and `visibleWhen` rules, in the SDK's
  `ui.json` format (validated by the SDK's `gen_ui.py`; no `kit` key, the art is
  ours), plus the skin `style` colours.
- `"gb"`: the Game Boy palette, page titles, the wordmark, decorative panels
  (the `TABLE` page's LCD grid over rows 0–1, columns 2–3) and the Mac editor's
  tab bar.

`scripts/build_ui.py` (standard library only) turns it into:

- `ui/art/*.png`: the pixel-art kit (knobs, faders, toggles, segments; same
  names, sizes and frame counts as the SDK kit), one background per page, and
  the editor-only art (tab bar and buttons, LCD table panel, row cursor);
- `src/ui/generated/ui_layout.inc` and `pixel_font.inc`: the layout and the
  5 × 7 pixel font as C++ tables (structs in `src/ui/ui_layout.h`), walked from
  the same component tree as the MPC `TUI.json`;
- `build/ui-preview/<page>.png` (and `tabbar.png`): previews of each MPC page
  with the controls at their default values and approximate label text, so the
  look can be reviewed without a device. Slider controls and fonts are drawn
  by the firmware on the MPC, so they only approximate the real screen there.

To change the UI, edit `ui/mpcboy_ui.json` (or the art code in
`scripts/ui_art.py`), then regenerate and look at the previews:

```sh
cmake --preset native && cmake --build --preset native
build/native/host_sim build/native/mpcboy.dylib dump > build/params.json   # mpcboy.so on Linux
python3 scripts/build_ui.py            # rewrites ui/art and src/ui/generated, writes build/ui-preview
python3 -m unittest discover -s tests/ui -v
```

The generated files are committed, so native builds need no Python. Output is
byte-reproducible; `scripts/check.sh` regenerates them and fails if anything in
`ui/art` or `src/ui/generated` changed or is not in git, so commit (or at least
`git add`) the regenerated files with the change. `package_mpc.py` generates
the skin into the plugin folder without touching the tree, and fails on any
`WARNING`/`NOTE` line from the generator. On the MPC, labels use Nexa Bold in
upper case (names `8bac0f`, values `9bbc0f`); the knob and menu overlays are
drawn by the firmware on the dark `style.background` and cannot be themed.

For the CPU probe used on the device, see `docs/BENCH.md`.

## Mac / Ableton Live (dev build)

A macOS VST2 build for fast iteration in Ableton Live 12 (Apple Silicon or
Intel). It is a development convenience, not part of the MPC package, and does
not touch the MPC build.

```sh
scripts/install_mac_vst.sh      # builds build/mac-vst/mpcboy.vst, installs it, prints the steps
# or by hand:
cmake --preset mac-vst && cmake --build --preset mac-vst   # build/mac-vst/mpcboy.vst
```

The bundle is universal (arm64 + x86_64), exports only `VSTPluginMain` and
`main_macho`, is built with `-O2` (as the SDK's desktop build) and is ad-hoc
signed (`codesign --sign -`) whenever it is assembled; it is rebuilt if you
delete it or change `plugin/plugin.json`. The install script copies it to
`~/Library/Audio/Plug-Ins/VST/mpcboy.vst` next to the destination, clears extended
attributes (including any quarantine flag) on that copy, signs it, and then
swaps it into place, so a failed install never leaves a half-written plugin.
Any `.sav` files already in the installed bundle's import folder are carried over.
It warns (does not stop) if Ableton Live is running.

In Live: Preferences > Plug-ins > turn on **Use VST2 Plug-in System Folders**,
click **Rescan**, then find it under Plug-ins > mpcboy and drop it on a MIDI
track. After re-installing a new build, rescan or restart Live.

Caveats:

- VST2 only. Live 12 still loads VST2, but only from the system folder with the
  setting above; there is no AU/VST3 build.
- The plugin has its own Game Boy editor window (1280 x 704 points): the same
  pages, art and layout as the MPC skin, plus a tab bar to switch pages. Knobs
  drag vertically (Shift for fine, double-click resets), faders follow the mouse,
  segments and toggles are clicked, menu sliders step on click (Shift steps back)
  or drag. Edits of parameters 0-50 are recorded as automation; the table cursor,
  cells and `Imp *` buttons are not automatable. The TABLE page shows the edit
  table as a live LSDJ-style grid (it updates while Live processes audio). Art is
  read from `Contents/Resources/ui/`. `build/mac-vst/render_editor ui/art
  build/editor-preview 2` renders every page to PNG without a DAW.
- Host rates 44.1, 48, 96 and 192 kHz work; at 88.2 kHz the plugin is silent.
  96 and 192 kHz use a table-driven polyphase resampler (L = 2 / 4 phases,
  same kernel as the vendored reference resampler, matching it to better than
  -100 dBFS): `host_sim` reports about 0.4 % (96 kHz) and 0.8 % (192 kHz) average
  CPU per 128-frame block on an M-series Mac. Switching to 96/192 kHz computes
  the (at most 1028 coefficient) table once on the audio thread at the rate
  change, a few tens of microseconds. The DC blocker corner (about 35 Hz) is the
  same at every rate, so low notes have the same level at 44.1 to 192 kHz.
- Files that the MPC keeps in `Content/` (the `.sav` import folder) live inside
  the bundle: `mpcboy.vst/Contents/Resources/Content/Import`. The install script
  keeps the `*.sav` files found there across a re-install; keep your own copies
  as well. Adding files to an installed bundle by hand makes `codesign -v` report
  it as modified; re-running the install script re-signs it.
- The ad-hoc signature is a local, unnotarized signature: there is no Developer ID,
  so Gatekeeper would not accept a downloaded copy, but a bundle built and
  installed on this Mac is not quarantined. If Live refuses it, check the
  Plug-ins preferences for a blocked-plugin note and re-run the install script.

## Verification status

Done natively: unit and plugin-level tests (`ctest`), UBSan runs (preset
`native-ubsan`, asserts on, `-fno-sanitize-recover`), the SDK host simulator, the strict SDK validator on the packaged folder, arm32 and
arm64 builds. **Pending sanitizer gates:** the `native-asan` and `native-tsan` presets exist
but their binaries crash or hang at initialisation on the development Mac, so
the plan's ASan/TSan `ctest` gates are not met yet; the exact commands to run
on Linux are in `docs/DEVICE_TESTS.md` ("Host gates"). **Pending on the user's MPC Live III** (every one is a release
blocker, see `docs/DEVICE_TESTS.md`): G-CPU (CPU use of one instance), G-EDIT
(the table editor's cursor behaviour, `docs/EDITOR.md`), and the WP10
checklist of 11 device checks, plus WP11's device test 9 (the `.sav` import on
the device) and a re-run of WP10 rows 1, 7, 8 and 10 on the WP11 build. The
`.sav` import itself is implemented and tested natively (import, undo, debounce,
state v2) but not yet run on a device.

## Layout

- `src/apu`, `src/core`, `src/table`, `src/sav`, `src/dsp`: vendored rackboy
  and SameBoy code; every local change is listed in `src/VENDORED.md`.
- `src/engine`: mono voice, tick clock, content store, factory tables, `.sav`
  import (`sav_import`).
- `src/plugin`: the MPC plugin (parameters, state, editor cursor, import worker).
- `plugin/`: `plugin.json` and the import folder's README.
- `ui/`: `mpcboy_ui.json` (the shared UI source) and the generated `art/`;
  `src/ui/`: the editor layout header and its generated tables;
  `scripts/build_ui.py` + `scripts/ui_art.py`: the generator.
- `vendor/mpc-plugin-builder-sdk`: Akai's MIT plugin SDK (submodule).

License: MIT, see `LICENSE`; third-party notices in `THIRD_PARTY.md`.
