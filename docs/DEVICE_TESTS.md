# Device tests

Results observed on a real MPC Live III. An implementing agent never records a
device result it did not observe; unobserved steps stay "pending" and block
the release (see the plan's definition of done).

## Early gates

| Step | Status | Notes |
|---|---|---|
| WP1 device smoke | pending (needs user's MPC Live III) | Install the placeholder folder (`scripts/make_min_folder.sh` output) into `VST2 Plugins/`; confirm "mpcboy" is listed under External Plugins. The placeholder is gone since WP7; this is now covered by WP10 row 1. |
| G-CPU (WP3 test 7) | pending (needs user's MPC Live III) | Build the CPU probe plugin (`-DMPCBOY_CPU_PROBE=ON`, see `docs/BENCH.md` for the exact commands), install the folder, insert it on a track. Since WP7 the probe is the real plugin with the `Stack4` program, a note it holds by itself, and the read-only parameter `CPU us` showing `m<mean> p99 <p99>` in microseconds per process() call (refreshed once per second). Hold at a 128-frame buffer for 60 s. Pass: mean <= 145 us and p99 <= 290 us. Not run: no device result is claimed. If it is still pending at WP10 it is a release blocker (WP10 row 9). |
| G-EDIT (WP8) | pending (needs user's MPC Live III) | The nine checks are listed in `docs/EDITOR.md`. |

## Host gates (sanitizers)

The plan (`07-execution-handoff.md`) requires ASan and TSan `ctest` runs with 0
reports for the WP5, WP7 and WP11 PRs. On the development Mac the `native-asan`
and `native-tsan` binaries crash or hang at initialisation, so those two gates
are **pending**; no result is claimed. What did run is `native-ubsan` (UBSan, asserts
on, `-fno-sanitize-recover`). The thread-safety claims for `ContentStore`, the
seqlocks, the import worker and the lifecycle calls therefore rest on the
stress tests (`content 5`, `plugin 6`, `import 7`) run un-instrumented.

| Gate | Status | Command |
|---|---|---|
| `native-ubsan` full tests (asserts on) | passes on the dev Mac | `cmake --preset native-ubsan && cmake --build --preset native-ubsan && (cd tests/rackboy && ../../build/native-ubsan/mpcboy_tests)` |
| `native-tsan` ctest (WP5 test 5, WP7 test 6, WP11 tests 6-7) | pending (TSan does not run on the dev Mac) | on Linux x86_64 or arm64: `cmake --preset native-tsan && cmake --build --preset native-tsan && ctest --preset native-tsan` |
| `native-asan` ctest (WP7 test 1, WP11 test 4) | pending | same with `native-asan` |

## WP11 (LSDJ `.sav` import) device test

Status of the code: implemented and verified natively (`tests/unit/import_test.cpp`).
Everything below needs the user's MPC Live III and is **pending**; no device
result is recorded or claimed. It is a release blocker like the WP10 rows.

| # | Check | Pass condition | Status | Result |
|---|---|---|---|---|
| 9 | Import on the device | Copy `tests/rackboy/fixtures/rackboy_v1.sav` into `Content/Import/` on the MPC, load the plugin, `TABLE` page: `Imp File` shows `rackboy_` (the file name cut at 8 characters). Tap `Imp Go` once: exactly one import runs (the three buttons go back to Off by themselves). Set `P1 Table = 1` and play: the table now holds the fixture's content (the fixture has no allocated tables, so its tables are empty and the table does nothing; to hear table content use a save that has tables). Undo with `Imp Undo` restores the factory tables (`MajArp` audible again with `P1 Table = 1`). Also check: `Imp Rescan` after adding a second file lists it; a text file renamed to `bad.sav` shows `ERR 2` and changes nothing; the file name is selected again after saving and reloading the project. | pending | |
| 9b | Re-run WP10 rows 1, 7, 8 and 10 on the WP11 build | Same pass conditions as the WP10 table below, on the build that contains the import (63 parameters, `TABLE` page with the fourth row of import controls). Row 8 now also covers the `Imp File` / `Imp Go` / `Imp Undo` / `Imp Rescan` Q-Link group. | pending | |

## WP10 checklist (MPC Live III, MPC OS 3.10.1 or later)

Build under test: `dist/mpcboy-0.1.0.zip` (record its SHA-256 here when a run
starts). Fill in "Result" only from what was observed on the device.

| # | Check | Pass condition | Status | Result |
|---|---|---|---|---|
| 1 | Plugin listed | Appears under External Plugins with the generated skin (not the 16-slider default). | pending | |
| 2 | Layered mono | `Stack4` program: one pad plays all four channels; a second pad while holding the first does not add a voice. | pending | |
| 3 | Channel solo | Each channel alone audible via the `MAIN` page On switches. | pending | |
| 4 | Tempo sync (S5) | `MajArp` on a 1-bar sequence at 60, 120 and 240 BPM: the arpeggio speed doubles with each step; record a 4-bar audio export at 120 BPM and check the arp period against `918.75` frames per tick (24 ticks/beat) within +-2 frames by loading the export into an editor. | pending | |
| 5 | Grid phase | `Tbl Phase = Grid` with a looping sequence: the arp restarts in the same place each loop; no audible burst at the loop point. | pending | |
| 6 | Transport values | Note what `tempoValid`/`ppqValid` do while stopped: add a temporary debug readout only in a debug build, or infer from behaviour (tables still sync to the project tempo while stopped). Update risk R4 in `00-overview.md` with the result. | pending | |
| 7 | State (S7) | Edit two table cells, change a wave selection, save the project, power-cycle, reload: all values return. | pending | |
| 8 | Q-Links | On each page, each group of four Q-Links moves the four controls in its frame, including the `MAIN` clock group with `visibleWhen` slots hidden in both `Sync` and `Free`. | pending | |
| 9 | CPU (S8) | Build the release code with `MPCBOY_CPU_PROBE` (same optimisation). Hold a `Stack4` note with all 4 tables running at 240 BPM, 96/beat, 128-frame buffer for 60 s. Pass: `dbg_us` mean <= 145 us (5 % of 2,902 us) and p99 <= 290 us. Also record the MPC CPU meter before and after inserting the plugin. Repeat at a 32-frame buffer and record (no pass bar). | pending | |
| 10 | Stability | Four instances on four tracks play a sequence for 10 minutes without dropouts or a crash. | pending | |
| 11 | Note latency | Export a bounce of a one-pad sequence at a known step. Measure note-start to audio-onset in the export. Native engine measurement (WP7 tests 7/8): a table tick becomes audible about 146 frames (3.3 ms) later; the polyphase resampler alone accounts for 117.6 of them. If the MPC compensates reported latency for instruments, decide with the user whether to report `latencySamples`; otherwise keep 0 and state the delay in the README. | pending | |

After the run, create `tested.json` (`[{"version": "<v>", "device": "MPC Live III (Gen2)", "firmware": "<MPC OS version>", "date": "<YYYY-MM-DD>"}]`).
If row 9 fails, profile with the `dbg_us` build before changing anything; the
WP3 G-CPU fallbacks are the first candidates. After WP11 lands, re-run rows 1,
7, 8 and 10 on the WP11 build.
