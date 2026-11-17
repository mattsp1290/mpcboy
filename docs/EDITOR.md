# Editing tables on the MPC

mpcboy has 16 tables of 16 rows. A table is bound to a channel through that
channel's `Table` control (`P1 Table`, `P2 Table`, `WV Table`, `NZ Table`;
`Off`, or 1-16). The tables, and the 16 waves, are saved with the project.

## How to edit

All editing happens on the `TABLE` page. There is no separate edit mode: the
page always shows the **cell under a cursor**.

1. **Edit Table** (1-16) and **Edit Row** (0-15) move the cursor. The six cell
   controls below them update to the values stored at that row.
2. **Row Vol** is the volume column of the row: `--` means "no volume column on
   this row", `00`-`FF` (hex) sets one (high nibble is the volume, as in the
   factory `Fade` and `Hat` tables). Turn it to the lowest position for `--`.
3. **Row Transp** (-48 to +48 semitones) is the row's pitch offset.
4. **Row Cmd1 / Row Val1** and **Row Cmd2 / Row Val2** are the two command slots:
   a command letter (`-` = none, `D E H K L N P R S V W`) and a parameter shown
   as two hex digits (`00`-`FF`).
5. Turning a cell control writes the cell at once, even while a note is
   playing, so you can hold a pad and hear the table change. Moving the cursor
   never writes anything.

Row 0 plays at note-on; row 1, 2, ... follow at the table clock (`Tbl Clock`,
`Tbl Rate`, `Tbl Phase` on the `MAIN` page). `H` (hop) jumps to a row, `K` kills
the note, `P`/`S`/`V`/`W` set pitch glide, sweep, vibrato and wave; see
`src/table/` and `docs/` in the repository for the exact semantics.

The factory tables are 1 MajArp, 2 MinArp, 3 Octave, 4 Vibrato, 5 KickDrop,
6 Fade, 7 Rise, 8 Hat; tables 9-16 start empty. The factory waves are 1 Saw,
2 Square, 3 Triangle, 4 Sine, 5 Pulse25, 6 Pulse12, 7 HalfSine, 8 Organ; waves
9-16 start as silence. There is no on-screen wave editor: choose a wave with
`WV Wave`.

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
| 8 | A `slider` with `"overlay": "menu"` on a 17-entry choice (`P1 Table`) opens a populated list. If it opens empty, switch those controls to `knob` widgets in `plugin/ui.json`. | pending |
| 9 | A cell edit made on the screen changes the table (proves that screen edits arrive on the audio thread). If it does not, stop and return to the user. | pending |

Outcomes (from the plan): all pass - done. Only 2 fails - keep the design and
note here that a cell knob may jump on its first turn. 3 or 4 fails - try a 2 Hz
refresh and only on cursor change; if it still fails, stop and return to the
user. 1 or 9 fails - stop and return to the user: the fallback (fixed per-channel
tables, no free cursor) changes the design (D8) and is the user's decision.
Every pending row is a release blocker.
