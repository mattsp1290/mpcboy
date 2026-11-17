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
  about 146 frames later). The plugin reports a latency of 0.
- **Programs:** `Init`, `ChipLead`, `MajArp`, `WaveBass`, `Kick`, `Hat`,
  `Stack4` (all four channels, each with a different table), `Zap`. Programs
  only set parameters; they never change table contents.
- **Factory tables** (slots 1-8): MajArp, MinArp, Octave, Vibrato, KickDrop,
  Fade, Rise, Hat. **Factory waves** (slots 1-8): Saw, Square, Triangle, Sine,
  Pulse25, Pulse12, HalfSine, Organ. Slots 9-16 start empty.

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
WAV peak check), generates the skin from `plugin/ui.json`, runs the SDK
validator in `--strict` mode and zips the folder with its SHA-256.

For the CPU probe used on the device, see `docs/BENCH.md`.

## Verification status

Done natively: unit and plugin-level tests (`ctest`), UBSan runs, the SDK
host simulator, the strict SDK validator on the packaged folder, arm32 and
arm64 builds. **Pending on the user's MPC Live III** (every one is a release
blocker, see `docs/DEVICE_TESTS.md`): G-CPU (CPU use of one instance), G-EDIT
(the table editor's cursor behaviour, `docs/EDITOR.md`), and the WP10
checklist of 11 device checks. WP11 (LSDJ `.sav` import) is not implemented.

## Layout

- `src/apu`, `src/core`, `src/table`, `src/sav`, `src/dsp`: vendored rackboy
  and SameBoy code; every local change is listed in `src/VENDORED.md`.
- `src/engine`: mono voice, tick clock, content store, factory tables.
- `src/plugin`: the MPC plugin (parameters, state, editor cursor).
- `plugin/`: `plugin.json` and `ui.json` for the skin.
- `vendor/mpc-plugin-builder-sdk`: Akai's MIT plugin SDK (submodule).

License: MIT, see `LICENSE`; third-party notices in `THIRD_PARTY.md`.
