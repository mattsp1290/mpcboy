# 06 — LSDJ `.sav` import of tables and waves

Covers WP11. This package runs after WP10. The plugin meets the user's request without it: the request names `gb-lsdj-instrument-extractor` as inspiration, and import is the most direct way to bring LSDJ material in. The user may defer it after WP10 (definition of done item 8).

## WP11 — Import tables and waves from an LSDJ 9.4.0 save

### Goal and prerequisite state

Prerequisite: WP10 has passed.

Goal: the user puts an LSDJ `.sav` into the plugin's `Content/Import/` folder, picks it on screen, and its first 16 allocated tables and waves 0–15 replace the plugin's bank, with one level of undo.

### Repository evidence

- `src/sav/` (vendored from rackboy, WP2): `compression::decompress()` returns `std::optional<std::vector<uint8_t>>` (allocates: worker thread only); `read_header` / `validate_header` require version byte `0x1B` (LSDJ 9.4.0); `parse_tables` reads the allocation map at `0x2020` and the transpose/command columns at `0x3480`/`0x3680`/`0x3880`/`0x3A80`/`0x3C80`; `parse_waves` reads 255 × 16 bytes at `0x6000`.
- `load_rackboy_sav` also parses instruments and the sidecar and can fail on instrument-pool problems (for example `rackboy_sav_instruments.cpp` handles kit instruments as a skip and rejects unknown type bytes). Import must not depend on instruments, so it calls the header check, `parse_tables` and `parse_waves` directly.
- The extractor (`$GB_LSDJ_EXTRACTOR_DIR`, default `~/git/gb-lsdj-instrument-extractor`): `crates/lsdj-sav/src/project.rs` line 20 `TABLE_ENVELOPE_OFFSET = 0x1690` (32 tables × 16 rows). Its `convert.rs` and `table_io.rs` (`empty_table`) use `0x00` as the empty envelope value.
- SDK `framework/mpc_resources.h`: `mpc::pluginFolder()`, `mpc::contentPath(rel)`, `mpc::readFile(path, bytes)`. No file I/O in `process()`.
- File-picker guidance (mpc-vst-plugins `docs/PORTING.md` §1): scan on a separate thread, sort by name, save the choice by name, never block audio. One tap can arrive as two presses 30–140 ms apart on a Live III; triggers need a 250 ms debounce.

### Change surface

| Item | Status | Description |
|---|---|---|
| `src/sav/rackboy_sav_tables.cpp` | `MPCBOY LOCAL MOD` | Also read the envelope column at `0x1690` into `RackboyTableRow::vol`; `has_vol = (vol != 0x00)`. A literal `0x00` volume cannot be told from "empty" in LSDJ data; it imports as no volume. Record this loss in `src/VENDORED.md`. |
| `src/engine/sav_import.{h,cpp}` | `new` | `ImportResult import_sav(const std::string& path, Content* out, ImportReport* report)`. Steps: read file, `decompress`, header checks, `parse_tables`, `parse_waves`. Maps the first 16 allocated tables, in allocation order, to slots 1–16 (missing slots cleared) and waves 0–15 to wave slots 1–16. `ImportReport` counts unsupported commands, `W` commands pointing at waves ≥ 16 (they become no-ops), and dropped allocated tables beyond 16. |
| `src/plugin/import_worker.{h,cpp}` | `new` | One worker thread per instance, started in the constructor, joined in the destructor. Scans `Content/Import/` (depth 1, `.sav` only, no symlinks followed, sorted by name, at most 64 entries) at start and on request; imports on request. Results go through `ContentStore::staged` (WP5), tagged as an import. |
| `src/engine/content_store.{h,cpp}` | edit | When the audio thread consumes staged content tagged as an import, it first copies `live` into a preallocated `undo` `Content` (≈ 2 KB `memcpy`) and sets `undoValid`. `imp_undo` makes the audio thread swap `undo` and `live` at the next block start. All undo work stays on the audio thread. |
| Parameters 59–62 | `new`, appended | `imp_file` (int 1–64; display text is the file name at that index from the last scan, `-` when empty), `imp_go`, `imp_undo`, `imp_rescan` (bool triggers: a 0→1 edge acts, a second edge within 250 ms is ignored, and the plugin sets the value back to 0 and refreshes the host through the WP7 host-refresh path). All `automatable = false`. |
| `plugin/ui.json` | edit | `TABLE` page row 4: `imp_file` (knob), `imp_go`, `imp_undo`, `imp_rescan`. No seventh page. |
| Extra state | edit | Version `2`: version-1 layout plus, before the CRC, a 1-byte length and up to 255 bytes of UTF-8 file name. The CRC covers everything before it. Version 1 is no longer accepted (no users, no compatibility requirement; the freeze rule in WP7 starts at the first tagged release, which comes after this package). The import is not re-run on project load; the imported tables are already in the saved state. |
| `tests/sav/import_test.cpp` | `new` | Uses `tests/fixtures/rackboy_v1.sav` and `tests/fixtures/expected.json` (copied in WP2; rackboy `docs/COMPATIBILITY.md` names them the normative reference fixture). |

### Behaviour

- An import replaces all 16 tables and all 16 waves at once. `imp_undo` restores the content from just before the last import (one level; a second undo re-applies the import, since it is a swap). The undo buffer is not saved in the project.
- On failure nothing changes and `imp_file`'s display text shows `ERR <code>` until the next scan.
- A second `imp_go` during an import is ignored.
- A missing `Content/Import/` folder gives an empty list. WP9 ships the folder with a README.
- Native tests set `MPC_PLUGIN_FOLDER` to a temporary folder holding `plugin.json` and `Content/Import/`, because `mpc::pluginFolder()` walks up from the library to the folder holding `plugin.json` (SDK `docs/plugin-folder-format.md`).
- `imp_file` is a `knob`, not a menu: its file names are dynamic display text, which the build-time skin cannot list.

### Tests and acceptance

1. Importing the fixture yields tables whose transpose and command columns equal `parse_tables`' output and waves equal to `parse_waves`' output for the same file.
2. Volume columns match the bytes at `0x1690`; `0x00` imports as `has_vol = false`.
3. A fixture variant with a kit instrument still imports tables and waves.
4. A truncated file, a wrong-version file (header byte ≠ `0x1B`) and a random 128 KB file are rejected without changing content, under ASan/UBSan.
5. Undo after import restores the previous content exactly.
6. Two `imp_go` edges 100 ms apart run one import.
7. The engine renders while an import runs on the worker: no ThreadSanitizer race.
8. State v2 round-trips, including the file name; a v1 blob is rejected.
9. Device: put the fixture into `Content/Import/` on the Live III, import, set `P1 Table = 1`, hear the table; tap `imp_go` once and confirm one import; undo works. Then re-run WP10 rows 1, 7, 8 and 10. Record in `docs/DEVICE_TESTS.md`.
