# 07 — Execution handoff

Read [00-overview.md](00-overview.md) first. Nothing in this plan has been implemented.

## Work packages in dependency order

| WP | Name | File | Prerequisites | Changes (main surfaces) | Verification |
|---|---|---|---|---|---|
| WP1 | Scaffold and CMake/zig build | [01](01-foundation-and-build.md#wp1--repository-scaffold-and-cmakezig-build) | none | `.gitmodules`, `vendor/mpc-plugin-builder-sdk`, `CMakeLists.txt`, `CMakePresets.json`, `cmake/zig-arm{32,64}.cmake`, `scripts/check.sh`, `.gitignore`, placeholder `src/plugin/MpcBoyPlugin.cpp` | G-BUILD: three presets build; ELF types; only `VSTPluginMain` exported; `validate.py --strict` on a minimal folder; optional early device smoke |
| WP2 | Vendor rackboy core | [01](01-foundation-and-build.md#wp2--vendor-the-rackboy-core-and-doctest) | WP1 | `third_party/sameboy/`, `src/{apu,core,table,sav,dsp}/`, `tests/rackboy/` (incl. `support/`, `audio/`), `tests/third_party/doctest/`, `tests/fixtures/`, `src/VENDORED.md`, `THIRD_PARTY.md` | `ctest --preset native` passes all copied suites, golden audio ≤ −60 dBFS |
| WP3 | Single-pass APU, polyphase resampler, CPU gate | [02](02-apu-and-voice.md#wp3--single-pass-stereo-apu-polyphase-resampler-early-cpu-gate) | WP2 | `rackboy_apu_run_stereo`, `src/dsp/polyphase_resampler.*`, `src/dsp/dc_blocker.h`, `bench/`, `docs/BENCH.md`, debug CPU spike plugin | Tests 1–6; **G-CPU** on the Live III |
| WP4 | Mono voice, 4 layered channels | [02](02-apu-and-voice.md#wp4--mono-voice-and-the-four-layered-channels) | WP3 | `src/engine/{channel_settings,instrument_encoder,note_stack,mono_voice,engine}.*`, cents pitch, `TableRegState` fields, length, noise rules | Tests 1–12 in WP4 |
| WP5 | Content store, banks, table extensions | [03](03-tables-and-tempo-sync.md#wp5--table-bank-wave-bank-and-table-engine-extensions) | WP4 | `RackboyTableRow` vol column, `VoiceState` fields, `src/engine/{table_bank,content_store,bank_adapter,factory_content}.*`, `P`, delayed `K` | Tests 1–8 in WP5, incl. `native-tsan` |
| WP6 | BPM-synced tick clock | [03](03-tables-and-tempo-sync.md#wp6--tick-clock-synced-to-host-bpm) | WP5 (clock unit tests may start with WP5) | `src/engine/tick_clock.*`, event ordering in `Engine::render` | Tests 1–9 in WP6 (S4–S6) |
| WP7 | SDK plugin, params, programs, state | [04](04-plugin-params-ui-state.md#wp7--sdk-plugin-class-parameters-programs-state) | WP6 | `src/plugin/{MpcBoyPlugin,param_bridge,editor_sync,state_codec}.*`, `plugin/plugin.json` | Tests 1–10 in WP7 (incl. plugin-level tempo, Grid and layering) |
| WP9a | Packaging script and host_sim target | [05](05-packaging-and-device-verification.md#wp9--mpc-plugin-folder-host-simulator-validator-zip) | WP7 | `host_sim` CMake target, `scripts/package_mpc.py`, `scripts/INSTALL.txt` | Steps 1–8 of `package_mpc.py` pass with the automatic layout (no `ui.json`) |
| WP8 | Skin pages and table editor | [04](04-plugin-params-ui-state.md#wp8--skin-pages-q-links-and-the-table-editor) | WP9a | `plugin/ui.json`, `docs/EDITOR.md` | **G-EDIT** device spike first, then gen_ui/validate clean |
| WP9b | Packaging with the final UI, README | [05](05-packaging-and-device-verification.md#wp9--mpc-plugin-folder-host-simulator-validator-zip) | WP8 | `README.md` rewrite, `check.sh` calls packaging | WP9 tests 1–4 |
| WP10 | Live III verification and bench | [05](05-packaging-and-device-verification.md#wp10--live-iii-verification-and-cpu-bench) | WP9b | `docs/DEVICE_TESTS.md`, `tested.json`, program values tuned by ear | **G-DEVICE**: checklist rows 1–11 |
| WP11 | LSDJ `.sav` import | [06](06-lsdj-sav-import.md) | WP10 | `src/engine/sav_import.*`, `src/plugin/import_worker.*`, `content_store` undo, params 59–62, state v2, `TABLE` page row 4 | Tests 1–9 in WP11, then WP10 rows 1, 7, 8, 10 again |

WP9 is split: WP9a lands before WP8 because the G-EDIT spike needs an installable folder.

## Parallelization

- WP1 → WP2 → WP3 → WP4 → WP5 → WP6 → WP7 is a strict chain: each package changes the previous one's types or calls.
- Inside WP6, `tick_clock.*` and its unit tests (tests 1–8) can be written in parallel with WP5, because `TickClock` has no dependency on tables. Its integration into `Engine::render` (test 9) waits for WP5.
- WP7's `state_codec` can be written in parallel with WP6 once WP5's `TableBank`/`WaveBank` layouts are merged.
- Device steps (WP1 smoke, G-CPU, G-EDIT, WP10, WP11 device test) need the user and their Live III. Ask before each. When the user is unavailable, record the step as "pending" in `docs/DEVICE_TESTS.md` and continue with the code work; every pending device step is a release blocker (definition of done item 6). An implementing agent never claims a device result it did not observe.
- Nothing after WP7 runs in parallel; each step needs the device or the previous package.

## Commit and PR shape

One PR per work package (WP9 as two PRs). Each PR runs `scripts/check.sh` before review. Follow the user's review workflow: dual review, fixes, re-verify, then commit. Generated test binaries and `build/`, `dist/` stay untracked.

## Integration and regression gates

| Gate | When | Command or procedure | Pass |
|---|---|---|---|
| G-BUILD | end of WP1, then every PR | `cmake --preset native && cmake --build --preset native`, same for `arm32`, `arm64` | all build; ELF checks |
| Unit | every PR from WP2 | `ctest --preset native --output-on-failure` | 0 failures |
| Sanitizers | WP5, WP7, WP11 PRs | `native-asan` and `native-tsan` presets, `ctest` | 0 failures, 0 sanitizer reports |
| Host sim | every PR from WP7 | `build/native/host_sim build/native/mpcboy.dylib test --seconds 4 --notes 48,60 --playing` | exit 0, no clipping warning |
| Validator | every PR from WP9a | `python3 vendor/mpc-plugin-builder-sdk/tools/validate.py "dist/<folder>" --params build/params.json --strict` | exit 0 |
| G-CPU | end of WP3 | debug CPU spike on the Live III, [02](02-apu-and-voice.md#wp3--single-pass-stereo-apu-polyphase-resampler-early-cpu-gate) test 7 | mean ≤ 145 µs, p99 ≤ 290 µs per 128-frame block, or recorded "pending" |
| G-EDIT | start of WP8 | device spike, [04](04-plugin-params-ui-state.md#g-edit-device-spike-first-step-of-wp8) | outcome recorded; "1 fails" stops for a user decision |
| G-DEVICE | WP10 | checklist in [05](05-packaging-and-device-verification.md#checklist-record-results-in-docsdevice_testsmd-new-and-testedjson-new) | every row passes |

All gates run as separate, individually checked commands (`scripts/check.sh` uses `set -euo pipefail`).

## Definition of done

1. S1–S9 in [00-overview.md](00-overview.md#measurable-success-criteria) are each proven by the test or device row named there.
2. `scripts/check.sh` passes from a clean clone after `git submodule update --init`.
3. `dist/mpcboy-<version>.zip` exists, passes the strict validator, and is the exact build used for the latest device run (WP10, or WP11's re-run).
4. `src/VENDORED.md` lists every vendored file and every `MPCBOY LOCAL MOD`.
5. `README.md` states the install steps, MPC OS 3.10.1+ and the two Enhancements switches, and that arm32 is not device-tested.
6. `docs/DEVICE_TESTS.md` and `tested.json` record the Live III run, and no device gate (G-CPU, G-EDIT, WP10 rows, WP11 device test) is still "pending". Code-complete with pending device gates is a valid intermediate state ("implemented, device verification pending"), not done.
7. The unresolved decision on manufacturer/uid is closed before the first zip is shared.
8. WP11 is done, or explicitly deferred by the user after WP10 (it is the only package the core outcome does not depend on).
9. Only after items 1–8: the first tagged release, which starts the WP7 freeze rule.

## Deferred work and follow-ups

- Separate output pairs per channel (would reintroduce the 4-pass APU; measure CPU first).
- An on-device wave editor (32 samples × 16 waves). Waves come from the factory set and `.sav` import.
- LSDJ-faithful parameter layouts for `P`, `K`, `D`, `V` (currently rackboy/mpcboy definitions; LSDJ_REFERENCE marks them unverified).
- Table copy/paste/clear actions and table names.
- LSDJ grooves, the table `G` command, chords (`C`), `T`, `M`, `O`, `Z`, `A`.
- Importing LSDJ instruments (not only tables/waves) into channel settings.
- Kit instruments and the LSDJ soft-synth.
- GitHub Actions CI for the three presets.
- Listing in the Open MPC catalog (`sd88me/mpc-vst-plugins` `docs/CATALOG.md`), after WP10 passes.
- Gen1 device testing, if a Gen1 MPC becomes available.
- Adopting a shared GB core library if the rackboy request is accepted and released (overview, cross-repository request).
