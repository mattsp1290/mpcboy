# 01 — Foundation: repository scaffold, build, vendored core

Covers WP1 and WP2. Decisions D1–D3 are in [00-overview.md](00-overview.md#key-decisions).

## WP1 — Repository scaffold and CMake/zig build

### Goal and prerequisite state

Prerequisite: the repository at commit `fa1f446` holds only `README.md` and `LICENSE`.

Goal: one command builds a native test library, an arm32 `.so` and an arm64 `.so` from the same sources, with the SDK's exact compiler flags, and compiles C as well as C++.

### Repository evidence

- SDK `tools/build.py` `find_sources()` (line 189) globs only `src/*.cpp` of a plugin project. It cannot compile the SameBoy `apu.c` that WP2 vendors.
- SDK `docs/building.md` lists the exact zig commands:
  - arm32: `zig c++ -target arm-linux-gnueabihf.2.31 -mcpu=cortex_a17`
  - arm64: `zig c++ -target aarch64-linux-gnu.2.31 -mcpu=cortex_a55`
  - shared flags (`COMMON_FLAGS`, `build.py` line 52): `-std=c++17 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections -Wall -Wextra`, then `-O2`, and for ARM `-Wl,--gc-sections -Wl,-s -Wl,--version-script=<sdk>/tools/exports.map`.
- SDK `tools/exports.map` is `{ global: VSTPluginMain; local: *; };`.
- rackboy compiles SameBoy's `apu.c` as `gnu11` C (rackboy `Makefile`).

### Change surface (all `new`)

| Path | Content |
|---|---|
| `.gitmodules`, `vendor/mpc-plugin-builder-sdk` | Submodule `https://github.com/inmusic-software/mpc-plugin-builder-sdk.git` pinned at `febd2ca5bd9eb16c4a412837196e145566bc981b`. |
| `CMakeLists.txt` | Top-level project, C11 (`gnu11` for SameBoy) and C++17. Targets listed below. |
| `cmake/zig-arm32.cmake`, `cmake/zig-arm64.cmake`, `scripts/zig-cc`, `scripts/zig-c++`, `scripts/zig-ar` | CMake does not accept a multi-word compiler, so small wrapper scripts run `$ZIG cc`, `$ZIG c++` and `$ZIG ar` with all arguments. `ZIG` defaults to `zig` on `PATH`; `scripts/bootstrap-zig.sh` (`new`) installs the `ziglang` Python package into `.tools/zigenv` (gitignored) and prints the `ZIG` value, as the SDK's `build.py` does. Toolchain files set `CMAKE_SYSTEM_NAME Linux`, `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER`/`CMAKE_AR`/`CMAKE_RANLIB` to the wrappers, the `-target` and `-mcpu` flags above, and `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY`. |
| `CMakePresets.json` | Presets `native`, `arm32`, `arm64` with build directories `build/native`, `build/arm32`, `build/arm64`. |
| `scripts/check.sh` | Local quality gate: configure and build all three presets, run `ctest` for `native`, run the host simulator and validator (added by WP9). Exits non-zero on the first failure (`set -euo pipefail`). |
| `.gitignore` | `build/`, `dist/`, `.sdk-tools/`, `.tools/`. |
| `nim.cfg` | Not applicable (not a Nim project). Do not add. |

CMake targets (`proposed` names):

| Target | Kind | Built in presets | Contents |
|---|---|---|---|
| `gbcore` | static library | all | Everything under `src/apu`, `src/table`, `src/core`, `src/dsp`, `src/engine`, `src/sav` and `third_party/sameboy/apu.c`. No SDK headers. |
| `mpcboy_plugin` | shared library (`MODULE`) | all | `src/plugin/*.cpp` linked with `gbcore`. Includes `vendor/mpc-plugin-builder-sdk/framework`. Set `PREFIX ""` and an explicit `OUTPUT_NAME`/`SUFFIX`: `mpcboy-arm32.so` / `mpcboy-arm64.so` on ARM, `mpcboy.dylib` natively. |
| `mpcboy_tests` | executable | `native` only | doctest suites under `tests/`. Registered with `ctest`. |
| `mpcboy_bench` | executable | `native` only | Renders a fixed workload and prints µs per 128-frame block (WP3, WP6). |

ARM link flags: `-Wl,--gc-sections -Wl,-s -Wl,--version-script=${SDK}/tools/exports.map`. The C++ runtime is linked statically, as zig does by default for these targets (SDK `docs/building.md`).

### Behaviour and invariants

- `-fvisibility=hidden` applies to `gbcore` too, so only `VSTPluginMain` is exported from the plugin.
- `gbcore` must not include any SDK header. This keeps the core testable without the VST2 glue.
- The native preset builds a `.dylib` on macOS (`.so` on Linux), loadable by `host_sim`.

### Tests and acceptance

- G-BUILD: `cmake --preset native && cmake --build --preset native` succeeds on macOS.
- G-BUILD: the `arm32` and `arm64` presets succeed. `file build/arm32/mpcboy-arm32.so` reports `ELF 32-bit LSB shared object, ARM, EABI5`. `file build/arm64/mpcboy-arm64.so` reports `ELF 64-bit LSB shared object, ARM aarch64`.
- `nm -D --defined-only` (or `llvm-nm`) on each ARM `.so` lists exactly one symbol: `VSTPluginMain`.
- At WP1 time `src/plugin/` holds a placeholder `mpc::Plugin` subclass with one parameter (`Bend Range`), the final `PluginInfo` identity (WP7) and a quiet 440 Hz sine on note-on, so the shared library links and makes sound. WP7 replaces it.
- WP1 also assembles a minimal plugin folder by hand or with a short script (`plugin.json`, `VST/`, no skins) and runs `python3 vendor/mpc-plugin-builder-sdk/tools/validate.py "<folder>" --strict`. This catches glibc symbol versions, `GLIBCXX`, dependencies and the export list in WP1 instead of WP9.
- **Early device smoke (recommended, needs the user's Live III):** install that folder into `VST2 Plugins/` and confirm the placeholder is listed under External Plugins and sounds. Record the result in `docs/DEVICE_TESTS.md` (`new`). If the user cannot do it now, record "skipped" and continue; WP10 still covers it.

### Risks and exclusions

- If `zig cc` rejects a SameBoy construct under `-std=gnu11`, use the same flags rackboy uses (`gnu11`) and record any extra flag in `src/VENDORED.md`. Do not edit SameBoy for a warning.
- No GitHub Actions CI in this package. The user's other repositories run gates locally; CI is listed as deferred.

## WP2 — Vendor the rackboy core and doctest

### Goal and prerequisite state

Prerequisite: WP1 builds an empty `gbcore`.

Goal: the rackboy modules compile inside `gbcore`, and rackboy's own unit tests for them pass in `mpcboy_tests`.

### Repository evidence

- rackboy commit `245230d` (`git -C ~/git/rackboy log -1`). Resolve the local checkout through the `RACKBOY_DIR` environment variable, default `~/git/rackboy`, or clone `git@github.com:mattsp1290/rackboy.git` at that commit.
- Include graph: `rackboy_table.h` includes `rackboy_voice.h` and `rackboy_sav_types.h`. `rackboy_table.cpp` includes `rackboy_apu.h` and `rackboy_pitch.h`. `rackboy_taps.h` includes `rackboy_voice.h`. `rackboy_voice.h` includes `rackboy_constants.h`.
- rackboy `THIRD_PARTY.md`: SameBoy `Core/apu.c`/`apu.h` at upstream `208ba4a`, Expat. doctest 2.4.11, MIT, at `tests/third_party/doctest/`.

### Change surface (all `new`, copied from rackboy unless marked)

| Destination | Source in rackboy |
|---|---|
| `third_party/sameboy/{apu.c,apu.h,LICENSE,SOURCE.md}` | `rackboy_apu/vendor/sameboy/` (byte-identical) |
| `src/apu/{rackboy_apu.h,rackboy_apu.c,rackboy_apu_shim.h,rackboy_apu_shim.c,gb.h,defs.h}` | `rackboy_apu/` |
| `src/core/` | `rackboy_core/rackboy_{constants,voice,pitch,taps}.{h,cpp}` only. `rackboy_gate.h`, `rackboy_control.*`, `rackboy_trigger.*`, `rackboy_clock.*`, `rackboy_bank.*` are **not** part of `gbcore`: they are VCV gate/knob glue that mpcboy replaces with WP4/WP6. |
| `tests/rackboy/support/` | **Test-only** copies of `rackboy_core/rackboy_{bank,clock,gate,trigger}.{h,cpp}` (flat) and of `src/spike/vcv_resampler/fft.h` at `tests/rackboy/support/spike/vcv_resampler/fft.h`, keeping the subdirectory so `#include "spike/vcv_resampler/fft.h"` resolves with `tests/rackboy/support` on the include path. They are needed by the copied suites (`tests/audio/regression.cpp` includes bank, clock, gate and trigger; `tests/table/fixture_integration_test.cpp` includes bank, clock and trigger; `tests/resampler/*` include `spike/vcv_resampler/fft.h`). They compile only into `mpcboy_tests`, never into `gbcore` or the plugin. |
| `src/table/rackboy_table.{h,cpp}` | `rackboy_table/` |
| `src/sav/` | `rackboy_sav/` all `.h`/`.cpp` except `spike/` (used by WP5 types and WP11) |
| `src/dsp/rackboy_resampler.{hpp,cpp}` | `src/rackboy_resampler.{hpp,cpp}` |
| `tests/third_party/doctest/` | `tests/third_party/doctest/` |
| `tests/rackboy/` | A mirror of rackboy's `tests/` layout: `apu/`, `core/`, `table/`, `sav/`, `resampler/` (incl. `measurement.hpp`), `audio/` (`regression.cpp`, `pattern.csv`, `reference/`), `common/` (`fixture_loader.{h,cpp}`), `fixtures/` (`rackboy_v1.sav`, `expected.json`, `README.md`, plus any other fixture the suites read), `doctest_main.cpp`. Keeping the layout keeps `#include "common/fixture_loader.h"` and the cwd-relative fixture and reference lookups working. Drop `sav/file_load_test.cpp` (includes VCV's `RackboyFileLoad.hpp`) and `core/control_test.cpp` (tests `rackboy_control`, not copied). Keep `core/{gate,trigger,bank,clock}_test.cpp` (sources in `support/`). |
| `src/VENDORED.md` (`new`) | Records rackboy commit `245230d`, SameBoy `208ba4a`, the copied file list, and every local change below. |
| `THIRD_PARTY.md` (`new`) | SameBoy (Expat), doctest (MIT), Akai SDK (MIT, submodule), rackboy (MIT). |

If `rackboy_table.cpp` or `rackboy_taps.cpp` turn out to reference a non-copied header, copy that header too and record it. Do not copy VCV or Daisy code.

### Local changes allowed in this package

- Include paths only: keep rackboy's flat includes (`#include "rackboy_voice.h"`) and add `src/apu`, `src/core`, `src/table`, `src/sav`, `src/dsp` as `gbcore` include directories, so no source line changes.
- Any test that depends on a dropped file is deleted, and the deletion is listed in `src/VENDORED.md`.

Behavioural changes to vendored code belong to WP3–WP5 and are marked `// MPCBOY LOCAL MOD` in the source, following rackboy's `// RACKBOY LOCAL MOD` convention.

### Tests and acceptance

- `fixture_loader.cpp` looks for `fixtures/`, `tests/fixtures/`, … and `regression.cpp` for `tests/audio/…` or `audio/…`, relative to the working directory. Register the copied suites with `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests/rackboy` so both resolve.
- `regression.cpp` skips (and passes) when reference files are missing. Add an `MPCBOY LOCAL MOD` that makes it fail instead, and check that the ctest output contains no skip line.
- rackboy's `tests/Makefile` builds one binary per module. mpcboy may build one `mpcboy_tests` binary with all suites; doctest test names must then stay unique (rename on collision and record it).
- `ctest --preset native` runs every copied suite. All pass. The count of test cases is recorded in `src/VENDORED.md` so later drift is visible.
- `tests/rackboy/audio` golden regression (`regression.cpp` with `pattern.csv` and `reference/{pulse1,pulse2,wave,noise}.bin`) passes at rackboy's threshold (per-channel RMS error below −60 dBFS at 48 kHz). This proves the vendored APU behaves like rackboy's. WP3 reuses these references.
- The `arm32` and `arm64` presets still build.

### Risks and exclusions

- Copying creates drift from rackboy. Mitigation: `src/VENDORED.md` plus the non-blocking shared-library request linked in the overview.
- Do not "clean up" vendored code in this package. Style changes make re-vendoring a manual merge.
