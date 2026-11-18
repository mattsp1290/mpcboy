# Editing tables on the MPC

mpcboy has 16 tables of 16 rows. A table is bound to a channel through that
channel's `Table` control (`PU1 Table`, `PU2 Table`, `WAV Table`, `NOI Table`;
`Off`, or a table name: `MajArp` ... `Hat`, then `Table 09` to `Table 16`). The tables, and the 16 waves, are saved with the project.

## How to edit

All editing happens on the `TABLE` page. There is no separate edit mode: the
page always shows the **cell under a cursor**.

1. **Edit Table** (1-16) and **Edit Row** (0-15) move the cursor. The six cell
   controls below them update to the values stored at that row.
2. **Row Vol** is the volume column of the row: `Keep` (shown as `--` in the grid) means "no volume column on
   this row", `00`-`FF` (hex) sets one (high nibble is the volume, as in the
   factory `Fade` and `Hat` tables). Turn it to the lowest position for `Keep`.
3. **Row Semi** (-48 to +48 semitones, shown like `+12 st`) is the row's pitch offset.
4. **Row Cmd1 / Row Val1** and **Row Cmd2 / Row Val2** are the two command slots:
   a command letter plus word (`-` = none, `D Delay`, `E Env`, `H Hop`, `K Kill`,
   `L Slide`, `N Noise`, `P Pitch`, `R Retrig`, `S Sweep`, `V Vib`, `W Wave`) and a parameter shown
   as two hex digits (`00`-`FF`).
5. Turning a cell control writes the cell at once, even while a note is
   playing, so you can hold a pad and hear the table change. Moving the cursor
   never writes anything.

Row 0 plays at note-on; row 1, 2, ... follow at the table clock (`Table Sync`,
`Table Rate`, `Table Start` on the `MAIN` page). `H` (hop) jumps to a row, `K` kills
the note, `P`/`S`/`V`/`W` set pitch glide, sweep, vibrato and wave; see
`src/table/` and `docs/` in the repository for the exact semantics.

The factory tables are 1 MajArp, 2 MinArp, 3 Octave, 4 Vibrato, 5 KickDrop,
6 Fade, 7 Rise, 8 Hat; tables 9-16 start empty. The factory waves are 1 Saw,
2 Square, 3 Triangle, 4 Sine, 5 Pulse25, 6 Pulse12, 7 HalfSine, 8 Organ; waves
9-16 start as silence. There is no on-screen wave editor: choose a wave with
`WAV Shape` (the wave name; user slots read `Wave 09` to `Wave 16`).

## Things to know

- The cell controls, the cursor and `Edit Table`/`Edit Row` are **not
  automatable**: they are editing tools, not performance parameters. Programs
  never touch them and never change table content.
- A command byte that is not in the list (possible once `.sav` import exists)
  is shown as `-`. Turning the knob replaces it.
- After the cursor moves, the host is told the six cell values changed, at
  most ten times per second. If a cell knob jumps on its first turn after a
  row change, turn it once more; the cell itself is only written when the
  value you turn it to differs from what was shown.
- Loading a project or program restores the tables first, then moves the
  controls to the cursor's cell; loading never writes a cell through the
  controls.

## Host notifications from `process()` (device risk)

After the cursor moves or an import changes the list, `process()` tells the host
that parameters 53-62 changed (`audioMasterAutomate` x10, then
`audioMasterUpdateDisplay`), at most ten times per second. This is a decision
from the plan (the community wrapper does the same from `processReplacing` and
it is proven on the device); G-EDIT below is where it is verified. The SDK header
advises against reporting parameters from the audio thread, and the host may
answer `UpdateDisplay` by re-reading the display strings on the same thread. What
the plugin does about it:

- `formatValue` for `Import File` reads a fixed-size name table published by the
  import worker behind a seqlock (`ImportWorker::displayTextInto`): no mutex,
  no allocation (the text is at most 15 characters, inside `std::string`'s
  small buffer). The other formatted values use `snprintf` into a small buffer
  and short assignments.
- The remaining risk is outside the plugin: `Plugin::displayText` is SDK code and
  builds a `std::string` per call, and the generic parameter display for
  choice parameters copies the choice text (all choice labels here are short).
  If the host's re-read runs inside `process()` its cost is the host's and is
  not bounded by us. G-EDIT must therefore also record `process()` p99 while the
  cursor moves and an import runs (use the probe build of `docs/BENCH.md`), and
  must confirm `effSetChunk` / program loads never run on the thread that runs
  `process()` (the audio-thread mark is sticky; see `src/plugin/MpcBoyPlugin.h`).
  If the MPC accepts these reports from a non-audio thread, move them to the
  import worker's loop.

## G-EDIT device spike

Status: **pending (needs user's MPC Live III).** No device result is recorded
here and none is claimed. The code paths are covered by native tests (WP7
tests 4-6), but the MPC's behaviour toward the cursor-bound cell parameters
(risk R3 in the plan) has not been observed.

Install `dist/mpcboy-0.1.0.zip`, select program `MajArp` or `Stack4`, hold a
pad playing a table, and record each check:

| # | Check | Result |
|---|---|---|
| 1 | Turning `Edit Row` updates the text of the six cell controls within 200 ms. | pending |
| 2 | The six cell Q-Links' positions follow the new row (no jump on the next turn). | pending |
| 3 | Turning a cell knob quickly does not fight (the value does not snap back) and the 10 Hz display refresh does not drop the touch focus or the selected Q-Link group. | pending |
| 4 | `automatable = false` parameters (Edit Table, Edit Row and the six cell controls) still respond to Q-Links and on-screen touch. | pending |
| 5 | Editing a cell, saving the project and reloading it shows the edit. | pending |
| 6 | No audio dropout while scrolling rows quickly. | pending |
| 7 | All six tabs are reachable from the function keys. | pending |
| 8 | A `slider` with `"overlay": "menu"` on a 17-entry choice (`PU1 Table`) opens a populated list. If it opens empty, switch those controls to `knob` widgets in `ui/mpcboy_ui.json` (`"mpc"` section) and re-run `scripts/build_ui.py`. | pending |
| 10 | `process()` p99 (probe build, `docs/BENCH.md`) stays within the CPU budget while the cursor moves quickly and an import runs: the 10 Hz notifications and display re-reads cost nothing audible. | pending |
| 11 | `effSetChunk` (project load) and program changes arrive on a thread that is not the one that calls `process()`. If they do not, a trigger restored as 1 could act: it is cancelled by `loadExtraState`, but a restored cell value would be written into the live tables. | pending |
| 9 | A cell edit made on the screen changes the table (proves that screen edits arrive on the audio thread). If it does not, stop and return to the user. | pending |

Outcomes (from the plan): all pass - done. Only 2 fails - keep the design and
note here that a cell knob may jump on its first turn. 3 or 4 fails - try a 2 Hz
refresh and only on cursor change; if it still fails, stop and return to the
user. 1 or 9 fails - stop and return to the user: the fallback (fixed per-channel
tables, no free cursor) changes the design (D8) and is the user's decision.
Every pending row is a release blocker.
