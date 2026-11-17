# 04 — SDK plugin, parameters, state and the on-screen UI

Covers WP7 and WP8. Decisions D1, D8, D9 are in [00-overview.md](00-overview.md#key-decisions).

## WP7 — SDK plugin class, parameters, programs, state

### Goal and prerequisite state

Prerequisite: WP6 is complete. `Engine` renders correct audio and ticks in native tests.

Goal: replace the WP1 placeholder with the real `mpc::Plugin` subclass that maps parameters to `Engine` settings, forwards MIDI and transport, and saves tables and waves in the project.

### Repository evidence (SDK at `febd2ca`)

- `framework/mpc_plugin.h`: `class Plugin` with `prepare(double, int)`, `reset()`, `process(ProcessContext&)`, `parameterChanged(int, float)`, `saveExtraState`, `loadExtraState`, `formatValue`, `addParam`, `addProgram`, `get`, `getInt`, `getBool`, `set`, `setNormalised` (calls `parameterChanged` synchronously), `notifyHostParameterChanged` (comment: "Not from the audio thread if avoidable"), `requestDisplayUpdate`, `MPC_PLUGIN_ENTRY`.
- `internalProcess` calls `prepare()` lazily **on the audio thread** if the host did not prepare, and calls `reset()` on the audio thread when `stateLoadPending_` is set (after a state load or `setProgram`).
- `internalLoadState` restores every saved parameter by id in saved order (each through `parameterChanged`), then calls `loadExtraState` only when the extra length is > 0, then sets `stateLoadPending_`.
- Host contract: lifecycle calls may run on the UI thread concurrently with `process`; parameter 0 is nudged on every prepare; parameter count is fixed; names ≈ 12 characters, value text ≈ 8; `requestDisplayUpdate` "also rebuilds the plugin's UI page. Use it sparingly."
- `examples/SimpleSynth/src/SimpleSynth.cpp`: enum + constructor parameter pattern, block-split MIDI rendering.
- `docs/dsp-cookbook.md` and `tools/gen_ui.py` (line 771): the MPC screen has six function keys; tabs beyond the sixth are untested.

### Change surface (all `new`)

| Item | Description |
|---|---|
| `src/plugin/MpcBoyPlugin.cpp` | `class MpcBoyPlugin : public mpc::Plugin`, `MPC_PLUGIN_ENTRY(MpcBoyPlugin)`. Enum `Params` in the order of the table below. |
| `src/plugin/param_bridge.{h,cpp}` | Pure functions from a parameter snapshot to `EngineSettings` (`proposed`). |
| `src/plugin/editor_sync.{h,cpp}` | Editor cursor logic (below), testable without the host. |
| `src/plugin/state_codec.{h,cpp}` | Encode/decode the extra-state blob. No SDK dependency. |
| `plugin/plugin.json` | SDK manifest (WP9). |
| `tests/plugin/*.cpp` | Unit and plugin-level tests below. They include `mpc_plugin.h` and drive `MpcBoyPlugin` directly. |

`PluginInfo`: `name = "mpcboy"`, `vendor = "mpcboy"`, `uniqueId = MPC_FOURCC('M','b','G','b')`, `version = 100` (the SDK/community mapping `major·1000 + minor·100 + patch·10 + build`, so 100 = 0.1.0.0, matching `plugin.json` `"version": "0.1.0"`), `isInstrument = true`, `numInputs = 0`, `numOutputs = 2`, `latencySamples = 0` (WP3). Name, vendor and uid are the overview's proposed values.

### Parameter table

Index order is fixed by the enum. Names are what the MPC shows and what `ui.json` references; they are unique and ≤ 12 characters. `id` is the saved key.

| # | id | Name | Kind and range | Default |
|---|---|---|---|---|
| 0 | `bendrng` | Bend Range | int 0–24 st | 2 |
| 1 | `vol` | Volume | float −40..+6 dB | −6 |
| 2 | `retrig` | Retrigger | choice Always, Legato | Always |
| 3 | `noteoff` | Note Off | choice Cut, Ring | Cut |
| 4 | `velvol` | Vel>Vol | bool | Off |
| 5 | `tblmode` | Tbl Clock | choice Sync, Free | Sync |
| 6 | `tblrate` | Tbl Rate | choice of the 10 labels in WP6 | `24/beat LSDJ` |
| 7 | `tblphase` | Tbl Phase | choice Note, Grid | Note |
| 8 | `freehz` | Free Hz | float 1–240 Hz, 2 decimals | 59.73 |
| 9–21 | `p1_on`, `p1_vol`, `p1_envdir`, `p1_envspd`, `p1_duty`, `p1_tsp`, `p1_fine`, `p1_pan`, `p1_len`, `p1_swtime`, `p1_swdir`, `p1_swshift`, `p1_tbl` | `P1 On`, `P1 Volume`, `P1 Env Dir`, `P1 Env Spd`, `P1 Duty`, `P1 Transp`, `P1 Fine`, `P1 Pan`, `P1 Length`, `P1 SwpTime`, `P1 SwpDir`, `P1 SwpShift`, `P1 Table` | bool; int 0–15; choice Down/Up; int 0–7; choice 12.5/25/50/75 %; int −24..+24; int −50..+50 ct; choice Off/L/C/R; int 0–64 (0 = Off); int 0–7; choice Up/Down; int 0–7; choice Off, 1–16 | On, 15, Down, 3, 50 %, 0, 0, C, 0, 0, Up, 0, Off |
| 22–31 | `p2_on`, `p2_vol`, `p2_envdir`, `p2_envspd`, `p2_duty`, `p2_tsp`, `p2_fine`, `p2_pan`, `p2_len`, `p2_tbl` | `P2 On` … `P2 Table` (same pattern) | as Pulse 1 without sweep | Off, 15, Down, 3, 25 %, 0, 0, C, 0, Off |
| 32–39 | `wv_on`, `wv_level`, `wv_wave`, `wv_tsp`, `wv_fine`, `wv_pan`, `wv_len`, `wv_tbl` | `WV On`, `WV Level`, `WV Wave`, `WV Transp`, `WV Fine`, `WV Pan`, `WV Length`, `WV Table` | bool; choice 0/25/50/100 %; choice 1–16; int −24..+24; int −50..+50; choice Off/L/C/R; int 0–64; choice Off, 1–16 | Off, 100 %, 1, −12, 0, C, 0, Off |
| 40–50 | `nz_on`, `nz_vol`, `nz_envdir`, `nz_envspd`, `nz_width`, `nz_pmode`, `nz_shape`, `nz_tsp`, `nz_pan`, `nz_len`, `nz_tbl` | `NZ On`, `NZ Volume`, `NZ Env Dir`, `NZ Env Spd`, `NZ Width`, `NZ PitchMd`, `NZ Shape`, `NZ Transp`, `NZ Pan`, `NZ Length`, `NZ Table` | bool; int 0–15; choice Down/Up; int 0–7; choice 15-bit/7-bit; choice Track/Fixed; int 0–111; int −24..+24; choice Off/L/C/R; int 0–64; choice Off, 1–16 | Off, 15, Down, 2, 15-bit, Track, 40, 0, C, 0, Off |
| 51 | `ed_tbl` | Edit Table | int 1–16 | 1 |
| 52 | `ed_row` | Edit Row | int 0–15 | 0 |
| 53 | `ed_vol` | Row Vol | int −1..255 (−1 shows `--`, else 2-digit hex) | from cell |
| 54 | `ed_tsp` | Row Transp | int −48..+48 | from cell |
| 55 | `ed_cmd1` | Row Cmd1 | choice `-`, D, E, H, K, L, N, P, R, S, V, W | from cell |
| 56 | `ed_val1` | Row Val1 | int 0–255, shown as 2-digit hex | from cell |
| 57 | `ed_cmd2` | Row Cmd2 | as Cmd1 | from cell |
| 58 | `ed_val2` | Row Val2 | as Val1 | from cell |

Rules:

- Programs list values for indices 0–50 only. Every index from 51 on (the editor here, the import controls in WP11) is UI plumbing that programs never touch.
- Indices 51 and up are `automatable = false`.
- **Choice labels are static.** `host_sim dump` infers each parameter's kind and choice labels by reading its display text at fixed positions, and the skin is generated from that dump at build time, so labels that depend on table content would be frozen at build time. `*_tbl` labels are `Off, 1 … 16`; `wv_wave` labels are `1 … 16` (factory names are listed in README); `ed_cmd*` labels are the fixed command list.
- `formatValue` is a pure function of the value: 2-digit uppercase hex for 53 (`--` for −1), 56 and 58; `Off` for length 0.
- A stored command byte outside the list (possible after WP11) maps to choice `-` (index 0) for the knob; the text shows `-` too, and turning the knob replaces the byte. `docs/EDITOR.md` states this.
- Parameter 0 (`bendrng`) is harmless to nudge (D9).
- **Freeze rule.** Until the first tagged release, ids, indices and the state format may change freely (no active users). From the first tagged release on, ids are never renamed, new parameters are appended, and the state format version is bumped on any layout change. The first tagged release happens only after the definition of done in [07-execution-handoff.md](07-execution-handoff.md) is met.

### Plugin behaviour

- **Constructor:** build `Engine` once at 44.1 kHz (all allocation happens here, including the shared polyphase table). Register parameters and programs. Load factory content into `ContentStore` (`live` directly, before any thread exists).
- **`prepare(sr, maxBlock)`:** never reallocates. Sets an atomic `silent_` flag when `sr != 44100` (unsupported), clears it otherwise. Safe if called on the UI thread during `process`, and safe if the framework calls it lazily on the audio thread.
- **`reset()`** (any thread; the SDK calls it on the audio thread after a load and on the dispatcher thread from `internalResume()`): only sets `resetPending_` (WP5 rule 5).
- **`process(ctx)`:** set the audio-thread marker. If `resetPending_` was set, run `Engine::reset()` (WP4) and mark the editor cursor for a resync. Consume staged content (WP5). If `silent_`, write zeros and return. Otherwise snapshot parameters into `EngineSettings` (`param_bridge`), build `EngineBlock` from `ctx.midi` and `ctx.transport`, call `Engine::render` into `ctx.outputs[0]` and `[1]`, apply master volume with a 10 ms smoother, and run the editor refresh (below). Guard `ctx.numOutputs < 2`.
- **`saveExtraState`** (UI thread): encodes the WP5 `published` snapshot.

### Editor cursor (`editor_sync`)

State: `std::atomic<int> shown_[6]` (the value last shown for each cell column) and `std::atomic<bool> cursorDirty_`.

- `parameterChanged(51 or 52)` (any thread): set `cursorDirty_`.
- `parameterChanged(53–58, v)`: if `v` equals `shown_[col]`, do nothing (this filters the plugin's own resync writes and host echoes). If not on the audio thread, do nothing (WP5 rule 4). Otherwise write the cell `(ed_tbl, ed_row, col)` in `live`, republish, and store `shown_[col] = v`.
- In `process()` (audio thread), if `cursorDirty_` was set: read the six cell values from `live` at the cursor, store each in `shown_[col]` **first**, then `set()` the parameter (its `parameterChanged` sees an equal value and posts nothing), then clear `cursorDirty_` and set `needsHostRefresh_`.
- Host refresh: when `needsHostRefresh_` is set and at least 4,410 frames (100 ms) have passed since the last refresh, call `notifyHostParameterChanged` for 53–58 and then `requestDisplayUpdate()` once. This is done from `process()` because the community wrapper sends every host report from `processReplacing` on the device (mpc-vst-plugins `docs/PORTING.md` §1). G-EDIT (WP8) verifies that this does not fight the user's knob or break touch focus.
- A state or program load runs on the UI thread, so its editor-parameter writes are ignored; the following reset resyncs the cursor from the loaded tables. A load never writes cells through the editor parameters.

### Programs

Eight built-in programs with values for indices 0–50: `Init` (Pulse 1 only, defaults), `ChipLead`, `MajArp`, `WaveBass`, `Kick`, `Hat`, `Stack4` (all four channels on, each with a different factory table), `Zap`. They select table and wave slots and never change table contents. Values beyond `Init` are tuned by ear in WP10; each program uses the channels its name implies.

### Extra state format (`state_codec`)

Little-endian, version 1:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `MBST` |
| 4 | 1 | format version, `1` |
| 5 | 1792 | 16 tables × 16 rows × 7 bytes: `has_vol, vol, transpose (int8), cmd1, val1, cmd2, val2` |
| 1797 | 256 | 16 waves × 16 bytes |
| 2053 | 4 | CRC-32 (IEEE) of bytes 0–2052 |

- Total 2,057 bytes.
- `loadExtraState` rejects wrong magic, unknown version, wrong size or bad CRC, and then stages nothing (current content stays). It never throws and never reads past `size`. Valid content goes to `ContentStore::staged` (WP5).
- Unsupported command bytes are stored unchanged.
- No older-version loader exists or is needed (no users, no compatibility requirement).

### Tests and acceptance

1. `state_codec`: random content round-trips byte-exact; each corruption (magic, version, truncated by 1 byte, flipped CRC) is rejected; 10,000 random blobs never crash under ASan/UBSan (`native-asan` preset, `new`).
2. `param_bridge`: each parameter's default maps to the documented `ChannelSettings` default.
3. Programs: `setProgram` for each program, then one `process` call, leaves table content unchanged.
4. Editor (edits issued from the thread that runs `process`): set `ed_row = 3`, process, set `ed_tsp = 5`, process: exactly one cell changes. Set `ed_row = 4` then back to 3, process: `ed_tsp` reads 5 and no cell changed during the resync.
5. **Load safety:** from a second thread, load a saved state whose extra blob is (a) valid, (b) missing, (c) corrupt, with editor parameters whose values differ from the current cells, then run one block. (a) content equals the blob; (b) and (c) content is unchanged. In no case did an editor parameter write a cell.
6. **Lifecycle under TSan:** a second thread calls `prepare(44100, 512)`, `reset()`, `loadExtraState`, `saveExtraState` and `formatValue` for every parameter, repeatedly, while the test thread calls `process` 10,000 times with notes and cell edits. No race report, no crash, and every saved blob passes `state_codec` validation.
7. **Plugin-level tempo (S4/S5 through the real glue):** drive `MpcBoyPlugin::process` with synthetic `ProcessContext`s (`Transport.tempoValid = true`, bpm 60, 120, 240; block sizes varied), program `MajArp`, one held note. Detect row changes in the **rendered audio** (pitch per 256-frame window by zero crossings). The row interval at 120 BPM is 918.75 ± 3 frames and halves at each BPM doubling.
8. **Plugin-level Grid:** `playing`, `ppqValid`, ppq advancing per block, `Tbl Phase = Grid`: row changes in the audio land within ±3 frames of `ppq = k/24` (after subtracting the measured resampler delay).
9. **Plugin-level layering:** program `Stack4`, one note: each channel's pitch sequence is visible in the spy and the output peak is above −20 dBFS.
10. `host_sim test` (SDK, native build): no FAIL.

## WP8 — Skin pages, Q-Links and the table editor

### Goal and prerequisite state

Prerequisite: WP7 is complete and `host_sim dump` prints the parameter table above; WP9a can build an installable folder.

Goal: a touch UI with at most six named pages generated by the SDK, Q-Links in groups of four, and a table editor confirmed on the device.

### Repository evidence

- SDK `docs/ui-guide.md`: `ui.json` with `pages[].name` (≤ 8 characters), `controls[]` in reading order on a 4 × 4 grid, widgets `knob`, `fader`, `switch`, `slider` (with `"overlay": "menu"`), `text`, `empty`, and `visibleWhen { param, choice }`. Q-Links act in groups of four, each a row or column of four.
- SDK `tools/gen_ui.py --params build/params.json --ui ui.json --out "<folder>/Plugin Skins"`. It prints a NOTE when there are more than six pages.

### Change surface (all `new`)

| Item | Description |
|---|---|
| `plugin/ui.json` | Six pages, below. |
| `docs/EDITOR.md` | How to edit a table on the MPC, with the G-EDIT result. |

Pages (rows of four; `·` = `empty`):

| Page | Row 1 | Row 2 | Row 3 | Row 4 |
|---|---|---|---|---|
| `MAIN` | Volume, Bend Range, Retrigger, Note Off | P1 On, P2 On, WV On, NZ On | Tbl Clock, Tbl Rate (`visibleWhen` Sync), Tbl Phase (`visibleWhen` Sync), Free Hz (`visibleWhen` Free) | Vel>Vol, ·, ·, · |
| `PULSE1` | P1 On, P1 Volume, P1 Env Dir, P1 Env Spd | P1 Duty, P1 Transp, P1 Fine, P1 Pan | P1 SwpTime, P1 SwpDir, P1 SwpShift, P1 Length | P1 Table, ·, ·, · |
| `PULSE2` | P2 On, P2 Volume, P2 Env Dir, P2 Env Spd | P2 Duty, P2 Transp, P2 Fine, P2 Pan | P2 Length, P2 Table, ·, · | |
| `WAVE` | WV On, WV Level, WV Wave, WV Pan | WV Transp, WV Fine, WV Length, WV Table | | |
| `NOISE` | NZ On, NZ Volume, NZ Env Dir, NZ Env Spd | NZ Width, NZ PitchMd, NZ Shape (`visibleWhen` PitchMd = Fixed), NZ Transp | NZ Pan, NZ Length, NZ Table, · | |
| `TABLE` | Edit Table, Edit Row, ·, · | Row Vol, Row Transp, ·, · | Row Cmd1, Row Val1, Row Cmd2, Row Val2 | (WP11 import controls) |

Choices with many entries (`*_tbl`, `wv_wave`, `tblrate`, `ed_cmd*`) use `"widget": "slider", "overlay": "menu"`. Two-way choices use `switch`.

### G-EDIT device spike (first step of WP8)

Install the WP7 build with the full six-page `ui.json` draft (WP9a packaging). Hold a note playing table 1 while testing. Record in `docs/EDITOR.md`:

1. Turning `Edit Row` updates the text of the six cell controls within 200 ms.
2. The six cell Q-Links' positions follow the new row (no jump on the next turn).
3. Turning a cell knob quickly does not fight (the value does not snap back) and the 10 Hz display refresh does not drop the touch focus or the selected Q-Link group.
4. `automatable = false` parameters (51–58) still respond to Q-Links and on-screen touch.
5. Editing a cell, saving the project and reloading it shows the edit.
6. No audio dropout while scrolling rows quickly.
7. All six tabs are reachable from the function keys.
8. A `slider` with `"overlay": "menu"` on a 17-entry choice (`P1 Table`) opens a populated list (the community wrapper reports MPC's native picker opening empty for VST2 lists; the SDK recommends the overlay). If it opens empty, switch those controls to `knob` widgets.
9. A cell edit made on the screen changes the table (proves WP5 rule 4: screen edits arrive on the audio thread). If it does not, stop and return to the user.

Outcomes:

- All pass: finish WP8.
- 2 fails, others pass: keep the design; document in `docs/EDITOR.md` that a cell knob may jump on its first turn after a row change (text is always right because `formatValue` reads the cell).
- 3 or 4 fails: try a refresh rate of 2 Hz and only on cursor change; if still failing, stop and return to the user.
- 1 or 9 fails: stop WP8 and return to the user with the observed behaviour. The fallback (fixed per-channel tables with one page each, no free cursor) changes D8 and is the user's decision.
- **Device not available yet:** finish WP8 with the design as written, record G-EDIT as "pending" in `docs/EDITOR.md`, and treat it as a release blocker in the definition of done. The code paths above are covered natively by WP7 tests 4–6.

### Tests and acceptance

1. `gen_ui.py` prints no WARNING and no NOTE lines.
2. `validate.py --strict` passes on the built folder (WP9).
3. Device (WP10 row 8): every page shows its controls; each Q-Link group moves the four controls it frames, including groups with `visibleWhen` slots hidden.
