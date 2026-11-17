# Vendored code

Everything below was copied from <https://github.com/mattsp1290/rackboy> (local
checkout `~/git/rackboy`) at commit **`245230d`**, working tree clean, on
2026-11-16. SameBoy `apu.c`/`apu.h` come from upstream SameBoy commit
`208ba4afabffab9edde416f2dbb8ae459e34adb8` (Expat), via rackboy.
doctest is v2.4.11 (`ae7a13539fb71f270b87eb2e874fbac80bc8dda2`, MIT).

Do not restyle vendored files: it turns a re-vendor into a manual merge.
Behavioural changes are marked `// MPCBOY LOCAL MOD` in the source.

## Layout mapping

| mpcboy path | rackboy source |
|---|---|
| `third_party/vendor/sameboy/` (incl. `.clang-format`, byte-identical) | `rackboy_apu/vendor/sameboy/` |
| `src/apu/` | `rackboy_apu/{rackboy_apu.h,rackboy_apu.c,rackboy_apu_shim.h,rackboy_apu_shim.c,gb.h,defs.h}` |
| `src/core/` | `rackboy_core/rackboy_{constants,voice,pitch,taps}.*` |
| `src/table/rackboy_table.{h,cpp}` | `rackboy_table/rackboy_table.{h,cpp}` |
| `src/table/channel_io.{h,cpp}` | (mpcboy-owned, no upstream: the table interpreter's register helpers, see local change 8) |
| `src/sav/` | `rackboy_sav/*.{h,cpp}` (no `spike/`) |
| `src/dsp/rackboy_resampler.{hpp,cpp}` | `src/rackboy_resampler.{hpp,cpp}` (compiled into `gbcore`: the engine uses `HostResampler` for 96/192 kHz hosts; the MPC's fixed 44.1 kHz path never calls it, the polyphase resampler is used) |
| `tests/third_party/doctest/` | `tests/third_party/doctest/` |
| `tests/rackboy/{apu,core,table,sav,resampler,audio,common,fixtures}/`, `tests/rackboy/doctest_main.cpp` | rackboy `tests/` (same layout) |
| `tests/rackboy/support/rackboy_{bank,clock,trigger}.{h,cpp}`, `rackboy_gate.h` | `rackboy_core/rackboy_{bank,clock,gate,trigger}.*` (test-only; `rackboy_gate.h` is header-only) |
| `tests/rackboy/support/spike/vcv_resampler/fft.h` | `src/spike/vcv_resampler/fft.h` |

Not copied: `rackboy_core/rackboy_control.*` (VCV knob glue),
`rackboy_apu/patches/` (rackboy has zero committed patches), `rackboy_sav/spike/`,
`tests/sav/file_load_test.cpp` (needs VCV `RackboyFileLoad.hpp`),
`tests/core/control_test.cpp` (tests `rackboy_control`), `tests/daisy/`, all
`.gitkeep` placeholders. No header outside the planned set was needed.

## Local changes

1. `tests/rackboy/audio/regression.cpp` (`// MPCBOY LOCAL MOD`, two places): a
   missing `reference/metadata.json` or missing `reference/*.bin` now FAILS
   the golden regression instead of skipping with a note. Upstream's
   "half-present reference dir" probe was replaced by the unconditional
   failure. CMake additionally sets `FAIL_REGULAR_EXPRESSION "skipping RMS check"`
   on the ctest entry.
2. Build: `src/apu/rackboy_apu_shim.h` includes `"vendor/sameboy/apu.h"`.
   SameBoy lives at `third_party/vendor/sameboy/` and `third_party` is on
   `gbcore`'s include path, so the include resolves directly: no source line was
   changed and no generated symlink is needed (an earlier layout used a
   build-tree symlink; it is gone).
3. Build flags: C is compiled `-std=gnu11` (as rackboy's Makefile, needed by
   SameBoy), C++ `-std=c++17`. Include dirs are `src/{apu,core,table,sav,dsp,engine}`
   and `third_party` (rackboy's flat includes are unchanged). Tests add `tests/third_party`,
   `tests/rackboy`, `tests/rackboy/support`. Warning flags `-Wall -Wextra
   -Wno-unused-parameter`; `-O2`, no `NDEBUG`.
4. Test organisation: all copied suites are linked into ONE binary,
   `mpcboy_tests` (rackboy builds one per module). Test-case names were
   checked for duplicates (`mpcboy_tests -ltc`): none, so nothing was renamed.
   ctest runs it once (`rackboy_suites`) with `WORKING_DIRECTORY tests/rackboy`
   so cwd-relative `fixtures/` and `audio/` lookups resolve.

5. **WP3 / WP4 local changes** (all marked `// MPCBOY LOCAL MOD` in the source):
   - `src/apu/rackboy_apu.{h,c}`:
     - `rackboy_apu_run_stereo(apu, n, left, right)`: one APU pass, no muting,
       no snapshot, no `band_limited` swap, own sample callback (swapped in and
       restored), new `_cb_buf_r` field in `rackboy_apu_t`.
     - Test-only register-write spy, **per instance**: new type
       `rackboy_apu_write_spy_fn`, fields `rackboy_apu_t::_spy` / `_spy_user`
       (NULL unless a test sets them), set by
       `rackboy_apu_set_write_spy(apu, fn, user)`. It is called from
       `rackboy_apu_write_reg` and from every write inside
       `rackboy_apu_write_wave_ram`, on that `apu` only (no process-global
       state). `rackboy_apu_init` clears it with the rest of the struct, so
       `Engine::reset()` saves and restores it around the init call. (A
       compile-time define was not possible because the plugin and the tests link
       the same `gbcore` library. Having init preserve the spy was considered and
       rejected: vendored tests pass uninitialised `rackboy_apu_t` structs to
       `rackboy_apu_init`.)
     - **Frame sequencer** (found missing, not in the plan): upstream SameBoy
       fires `GB_apu_div_event` / `GB_apu_div_secondary_event` from the CPU's
       DIV counter (`GB_set_internal_div_counter`). rackboy has no CPU and never
       calls them, so in rackboy no length counter, volume envelope or sweep ever
       advances (verified: NR11 length 16 + NRx4 bit 6 played for 500 ms). New
       field `mpcboy_frame_seq` and `rackboy_apu_set_frame_sequencer()`;
       `advance_cycles()` (factored out of `feed_samples`) then advances
       `gb.div_counter` and fires the two events at bit 12 edges (every 4096
       T-cycles), splitting the T-cycle chunk at each edge. Default off, in which
       case the sequence of operations is identical to before, so
       `rackboy_apu_run_samples` and the golden regression are unchanged.
       `Engine::reset()` turns it on.
   - `src/core/rackboy_pitch.{h,cpp}`: `pitch_square_11bit_cents`,
     `pitch_wave_11bit_cents` (existing functions untouched; for whole
     semitones the cents path evaluates the identical expression).
   - `src/table/rackboy_table.{h,cpp}`:
     - New `TableRegState` fields `bend_cents`, `fine_cents`, `noise_flags`,
       `length_enable` (zeroed by `table_reg_state_reset`), constants
       `kLengthEnableBit`, `kNoiseFixed`, `kNoiseWidth7`, struct `TriggerExtras`,
       `table_trigger(..., const TriggerExtras* extras = nullptr)` (nullptr =
       old behaviour, including the "semitones == 0 uses raw[4]" noise rule),
       and `table_write_pitch()` (frequency rewrite for legato glide / bend).
     - One helper `write_channel_pitch()` replaces the 8 pitch call sites (it
       now lives in `src/table/channel_io.cpp`, see local change 8). Line
       numbers in `rackboy_table.cpp` at rackboy `245230d`:
       `table_trigger`: 555 (Pulse 1, `pitch_square_11bit`), 591 (Pulse 2,
       `pitch_square_11bit`), 623 (Wave, `pitch_wave_11bit`), 678 (Noise,
       `pitch_noise_nr43`); `table_tick`: 853 (Pulse 1), 859 (Pulse 2),
       865 (Wave), 880 (Noise). The helper composes
       `(pitch_offset + vibrato) * 100 + fine_cents + bend_cents` (the WP5
       `pbend_accum` term is a comment until WP5 adds the field) and writes
       NRx3/NRx4 (with the trigger bit when called from `table_trigger`).
       Noise rules: Fixed (`noise_flags` bit 0) never rewrites NR43 from pitch;
       width (`noise_flags` bit 1) is OR'd into every NR43 write, including `N`.
     - Length: `table_trigger` writes NR11/NR21/NR41 low 6 bits `64 - length`
       and NR31 `256 - 4 * length` from `TriggerExtras.length`, never from
       `raw[3]`; `write_freq_no_trigger`, `trigger_nrx4_byte` and every
       NR44 write OR bit 6 while `length_enable` is set (also the `E` and `R`
       paths).
     - Length reload on table retriggers (review stage 1): new
       `TableRegState::len_lo6` (the NRx1 / NR41 low 6 bits, `64 - length`,
       default `0x3F`) and `len_nr31` (NR31 `256 - 4 * length`, default `0xFF`),
       defaulted by `table_reg_state_reset` and set by `table_trigger`. Before a
       table retrigger (`E`, `R`, `W` on the engine path, the volume column) the
       length is reloaded from them, because on hardware a trigger reloads the
       length counter only when it has run out; pulse duty bits are kept. The
       helper is `reload_length()`, now inside `retrigger()` /
       `write_envelope_retrigger()` in `channel_io.cpp`.

6. **WP5 local changes** (all marked `// MPCBOY LOCAL MOD`):
   - `src/sav/rackboy_sav_types.h`: `RackboyTableRow` gained `uint8_t vol` and
     `bool has_vol` (LSDJ table volume column; 5 -> 7 bytes), zeroed by
     `table_reset`; new `static_assert(sizeof(RackboyTableRow) == 7)`. The `.sav`
     parser does not read the column and leaves both zero.
     `sizeof(RackboyBank)` is now 12052 (was 11028).
   - `src/core/rackboy_voice.{h,cpp}`: `VoiceState` gained `int16_t pbend_accum`,
     `int8_t pbend_step`, `uint8_t kill_ticks_remaining` (reset in `voice_reset`
     and `table_trigger`); the size `static_assert` bound is raised from 16 to
     24 (the struct is 20 bytes).
   - `src/table/rackboy_table.{h,cpp}`:
     - `write_channel_pitch` takes `pbend_accum` and adds `pbend_accum * 100 / 16`
       cents (the term that was a comment in WP4).
     - Volume column: when `row.has_vol`, after the pitch write and R check and
       before the row's commands, `NRx2 = vol` plus a retrigger with the running
       frequency (the `E` path, shared helper `write_envelope_retrigger`; Wave:
       `NR32 = vol & 0x60`).
     - `P` command: `pbend_step = (int8_t)param`; every tick adds it to
       `pbend_accum` (clamped to +-32000) before the pitch write. `P00` stops the
       motion and keeps the offset.
     - `K` with delay: `K00` kills at once; `Kxx` (xx > 0) sets
       `kill_ticks_remaining`, counted at the top of every `table_tick` (also
       during a `D` delay); the kill fires on the tick where it reaches 0 and
       that tick does nothing else. Shared helper `kill_channel`.
     - `W` retrigger: after `rackboy_apu_write_wave_ram`, NR34 is written with
       the trigger bit and the running frequency. **Only on the engine path**:
       new `TableRegState::engine_mode` (set by `table_trigger` when it received
       `TriggerExtras`; `write_channel_pitch` reads `regs.engine_mode` rather than
       taking a parameter, and the noise trigger rule keys off it too: the legacy
       rule "semitones == 0 uses raw[4]" applies only while `engine_mode` is 0).
       Reason: the vendored `wave_set_test` asserts three times
       that `W` does not set the NR34 trigger bit (legacy behaviour), and the
       plan allows no vendored expected-value change besides the two constants
       below, so the legacy path (no extras) keeps no retrigger.
   - `tests/rackboy/sav/types_test.cpp`: `kExpectedBankSize` 11028 -> 12052
     (allowed constant change 1 of 2).
   - The `VoiceState` size bound in `rackboy_voice.h` is the other allowed
     expected-value change (16 -> 24; no vendored test file carries it).

7. **WP11 local changes** (all marked `// MPCBOY LOCAL MOD`):
   - `src/sav/rackboy_sav_tables.{h,cpp}`: `parse_tables` also reads the table
     envelope (volume) column at `0x1690` (new constant `kTableEnvelopeOffset`,
     32 tables x 16 rows, same offset as `TABLE_ENVELOPE_OFFSET` in the
     extractor's `crates/lsdj-sav/src/project.rs`) into `RackboyTableRow::vol`
     and sets `has_vol = (vol != 0x00)`. The extractor writes `0x00` for an empty
     envelope cell (`table_io.rs` `empty_table`, `convert.rs` `table_to_sav`).
     **Recorded loss:** a literal `0x00` volume cannot be told from "empty" in
     LSDJ data, so it imports as no volume. The comment on `RackboyTableRow` in
     `rackboy_sav_types.h` was updated (the parser no longer leaves the two
     fields zero). `load_rackboy_sav` and the vendored tests are unchanged.

8. **Review stage 2: table register helpers moved out of the vendored file**
   (`src/table/channel_io.{h,cpp}`, mpcboy-owned, namespace `rackboy::table`).
   The helpers that local changes 5 and 6 had added to `rackboy_table.cpp` now
   live there: `write_channel_pitch`, `kill_channel`, `retrigger`,
   `write_envelope_retrigger`, `trigger_byte`, `noise_width_bits`, plus the
   internal `reload_length` and `cents_to_nearest_semitone`, all built on one
   descriptor `kChannelRegs[4]` ({NRx1, NRx2, NRx3, NRx4} per `VoiceChannel`).
   What remains in the vendored `src/table/rackboy_table.cpp`, each marked
   `// MPCBOY LOCAL MOD`:
   - `#include "channel_io.h"`;
   - the upstream statics `write_freq_no_trigger` and `trigger_nrx4_byte` are
     removed (their only callers moved; `channel_io.cpp` composes the same
     bytes), with a comment where they were;
   - the `E` command and the volume column call `write_envelope_retrigger`; the
     `N` command ORs `noise_width_bits(regs)`; the `W` command (engine path
     only) and the recurring `R` check call `retrigger(regs, ch, apu)`; the `K`
     paths call `kill_channel`;
   - the eight former pitch call sites call `write_channel_pitch(apu, ch, regs,
     vib, pbend_accum, trigger)` (the `engine_mode` argument is gone);
   - the `table_trigger` / `table_tick` / `table_write_pitch` bodies as in local
     changes 5 and 6.
   `rackboy_table.cpp` is about 1080 lines again (1356 before this step). The
   engine's `MonoVoice::dacOff` also calls `kill_channel` (it writes NRx2 or
   NR30 = 0 and clears `triggered`; the extra zeroing of `regs.envelope`,
   `wave_volume` and `kill_ticks_remaining` is overwritten by the next
   `table_trigger`). A re-vendor now means re-applying the marked call sites;
   `channel_io.*` is not touched by it.

9. **Host sample rates (desktop build):** `src/dsp/rackboy_resampler.cpp` is
   linked into `gbcore` again (it was test-only after the stage-2 review; the
   file itself is unchanged). `Engine::setHostRate` uses `HostResampler` for
   96000 and 192000 Hz hosts. Nothing in the vendored files changed.

## Test count

`mpcboy_tests` (run from `tests/rackboy`): **512 test cases**, 2215453
assertions, 0 skipped, including the golden `audio regression` case (473 test
cases at WP6; WP7-WP9 and WP11 brought it to 506; the stage-1 review fixes added
6 more; the stage-2 restructuring changed no count). The 426 copied rackboy cases
are unchanged and pass; the rest live in `tests/unit/` (`stereo_pass_test.cpp`,
`polyphase_test.cpp`, `engine_test.cpp`, `table_ext_test.cpp`,
`content_store_test.cpp`, `tick_clock_test.cpp`, `engine_timing_test.cpp`,
`plugin_test.cpp`, `plugin_codec_test.cpp`, and the .sav import tests in
`sav_import_test.cpp`, `import_undo_test.cpp` and `import_plugin_test.cpp`).
The host-rate work added 5 (`tests/unit/host_rate_test.cpp`). Later drift in this number should be explained in the commit that causes it.

## Copied files

```text
src/apu/defs.h
src/apu/gb.h
src/apu/rackboy_apu.c
src/apu/rackboy_apu.h
src/apu/rackboy_apu_shim.c
src/apu/rackboy_apu_shim.h
src/core/rackboy_constants.h
src/core/rackboy_pitch.cpp
src/core/rackboy_pitch.h
src/core/rackboy_taps.cpp
src/core/rackboy_taps.h
src/core/rackboy_voice.cpp
src/core/rackboy_voice.h
src/dsp/rackboy_resampler.cpp
src/dsp/rackboy_resampler.hpp
src/sav/rackboy_readout_format.h
src/sav/rackboy_sav_compression.cpp
src/sav/rackboy_sav_compression.h
src/sav/rackboy_sav_header.cpp
src/sav/rackboy_sav_header.h
src/sav/rackboy_sav_instruments.cpp
src/sav/rackboy_sav_instruments.h
src/sav/rackboy_sav_sidecar.cpp
src/sav/rackboy_sav_sidecar.h
src/sav/rackboy_sav_tables.cpp
src/sav/rackboy_sav_tables.h
src/sav/rackboy_sav_types.h
src/sav/rackboy_sav_validate.cpp
src/sav/rackboy_sav_validate.h
src/sav/rackboy_sav_waves.cpp
src/sav/rackboy_sav_waves.h
src/table/rackboy_table.cpp
src/table/rackboy_table.h
tests/rackboy/apu/driver_test.cpp
tests/rackboy/apu/wave_ram_test.cpp
tests/rackboy/audio/README.md
tests/rackboy/audio/pattern.csv
tests/rackboy/audio/reference/metadata.json
tests/rackboy/audio/reference/noise.bin
tests/rackboy/audio/reference/pulse1.bin
tests/rackboy/audio/reference/pulse2.bin
tests/rackboy/audio/reference/wave.bin
tests/rackboy/audio/regression.cpp
tests/rackboy/common/fixture_loader.cpp
tests/rackboy/common/fixture_loader.h
tests/rackboy/core/bank_test.cpp
tests/rackboy/core/clock_test.cpp
tests/rackboy/core/gate_test.cpp
tests/rackboy/core/pitch_test.cpp
tests/rackboy/core/smoke_test.cpp
tests/rackboy/core/taps_test.cpp
tests/rackboy/core/trigger_test.cpp
tests/rackboy/core/voice_test.cpp
tests/rackboy/doctest_main.cpp
tests/rackboy/fixtures/README.md
tests/rackboy/fixtures/expected.json
tests/rackboy/fixtures/rackboy_v1.sav
tests/rackboy/resampler/host_resampler_test.cpp
tests/rackboy/resampler/init_test.cpp
tests/rackboy/resampler/measurement.hpp
tests/rackboy/resampler/measurement_test.cpp
tests/rackboy/resampler/process_test.cpp
tests/rackboy/resampler/spectral_test.cpp
tests/rackboy/sav/fixture_test.cpp
tests/rackboy/sav/header_test.cpp
tests/rackboy/sav/instruments_test.cpp
tests/rackboy/sav/readout_format_test.cpp
tests/rackboy/sav/sidecar_test.cpp
tests/rackboy/sav/tables_test.cpp
tests/rackboy/sav/types_test.cpp
tests/rackboy/sav/validate_test.cpp
tests/rackboy/sav/waves_test.cpp
tests/rackboy/support/rackboy_bank.cpp
tests/rackboy/support/rackboy_bank.h
tests/rackboy/support/rackboy_clock.cpp
tests/rackboy/support/rackboy_clock.h
tests/rackboy/support/rackboy_gate.h
tests/rackboy/support/rackboy_trigger.cpp
tests/rackboy/support/rackboy_trigger.h
tests/rackboy/support/spike/vcv_resampler/fft.h
tests/rackboy/table/chord_noop_test.cpp
tests/rackboy/table/chord_scan_test.cpp
tests/rackboy/table/envelope_retrigger_test.cpp
tests/rackboy/table/fixture_integration_test.cpp
tests/rackboy/table/global_noop_test.cpp
tests/rackboy/table/noise_shape_test.cpp
tests/rackboy/table/pitch_slide_test.cpp
tests/rackboy/table/pitch_transpose_test.cpp
tests/rackboy/table/retrigger_test.cpp
tests/rackboy/table/sweep_test.cpp
tests/rackboy/table/vibrato_test.cpp
tests/rackboy/table/wave_set_test.cpp
third_party/vendor/sameboy/.clang-format
third_party/vendor/sameboy/LICENSE
third_party/vendor/sameboy/SOURCE.md
third_party/vendor/sameboy/apu.c
third_party/vendor/sameboy/apu.h
tests/third_party/doctest/.clang-format
tests/third_party/doctest/LICENSE.txt
tests/third_party/doctest/SOURCE.md
tests/third_party/doctest/doctest.h
```
