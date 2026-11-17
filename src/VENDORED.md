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
| `third_party/sameboy/` (incl. `.clang-format`, byte-identical) | `rackboy_apu/vendor/sameboy/` |
| `src/apu/` | `rackboy_apu/{rackboy_apu.h,rackboy_apu.c,rackboy_apu_shim.h,rackboy_apu_shim.c,gb.h,defs.h}` |
| `src/core/` | `rackboy_core/rackboy_{constants,voice,pitch,taps}.*` |
| `src/table/` | `rackboy_table/rackboy_table.{h,cpp}` |
| `src/sav/` | `rackboy_sav/*.{h,cpp}` (no `spike/`) |
| `src/dsp/rackboy_resampler.{hpp,cpp}` | `src/rackboy_resampler.{hpp,cpp}` |
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
2. Build: `src/apu/rackboy_apu_shim.h` includes `"vendor/sameboy/apu.h"`, but
   SameBoy lives at `third_party/sameboy/`. Rather than edit the shim, CMake
   creates a symlink `<build>/include/vendor/sameboy -> third_party/sameboy`
   and adds `<build>/include` to `gbcore`'s include path. No source line
   changed. (Editors/LSP without that include path will not resolve the
   header; add `<build>/native/include` if needed.)
3. Build flags: C is compiled `-std=gnu11` (as rackboy's Makefile, needed by
   SameBoy), C++ `-std=c++17`. Include dirs are `src/{apu,core,table,sav,dsp}`
   (rackboy's flat includes are unchanged). Tests add `tests/third_party`,
   `tests/rackboy`, `tests/rackboy/support`. Warning flags `-Wall -Wextra
   -Wno-unused-parameter`; `-O2`, no `NDEBUG`.
4. Test organisation: all copied suites are linked into ONE binary,
   `mpcboy_tests` (rackboy builds one per module). Test-case names were
   checked for duplicates (`mpcboy_tests -ltc`): none, so nothing was renamed.
   ctest runs it once (`rackboy_suites`) with `WORKING_DIRECTORY tests/rackboy`
   so cwd-relative `fixtures/` and `audio/` lookups resolve.

## Test count

`mpcboy_tests` (run from `tests/rackboy`): **426 test cases**, 153179
assertions, 0 skipped, including the golden `audio regression` case. Later
drift in this number should be explained in the commit that causes it.

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
third_party/sameboy/.clang-format
third_party/sameboy/LICENSE
third_party/sameboy/SOURCE.md
third_party/sameboy/apu.c
third_party/sameboy/apu.h
tests/third_party/doctest/.clang-format
tests/third_party/doctest/LICENSE.txt
tests/third_party/doctest/SOURCE.md
tests/third_party/doctest/doctest.h
```
