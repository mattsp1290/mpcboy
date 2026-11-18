# Round 1 result: agreed terminology (moderator's ruling)

Inputs: three debaters with three stances (mainstream producer, chiptune/LSDJ native, hardware/screen pragmatist). Each gave an opening and a rebuttal.

Command letters and length timing were checked against the code:
- **Command letters:** D Delay, E Env, H Hop, K Kill, L Slide, N Noise (NR43), P Pitch, R Retrig, S Sweep, V Vibrato, W Wave.
- **Length:** counts at 256 Hz. Pulse and Noise top out at 250 ms; Wave tops out at 1000 ms.

## Naming rules
1. Every channel parameter carries a 3-letter LSDJ prefix: `PU1`, `PU2`, `WAV`, `NOI`.
2. Global groups use full words: `Table …`, `Row …`, `Import …`.
3. Title Case with spaces. No CamelCase.
4. Abbreviations:
   - Allowed: Env, Swp, Amt, Vel, Dir, Semi, Cmd, Val.
   - Banned: Tbl, Imp, Transp, Spd, Md.
   - Never abbreviate when the full word fits.
5. Units live in the value text. The name never carries a unit.
6. Value text is 8 characters or fewer. Hex appears only in the Row editor.
7. "Time" means higher = longer. "Amt" and "Rate" mean higher = more or faster.
8. The same concept gets the same word on every channel.

## Renames and relabels
| Current | New name | Values / value text |
|---|---|---|
| P1/P2/WV/NZ prefixes | PU1/PU2/WAV/NOI | |
| `Env Spd` | `Env Time` | Off, 1..7 (higher = slower) |
| `Env Dir` | `Env Dir` | Down / Up |
| `SwpTime` | `Swp Time` | Off, 1..7 |
| `SwpDir` | `Swp Dir` | Down / Up (same order as Env Dir) |
| `SwpShift` | `Swp Amt` | Off, 1..7. Higher = bigger pitch change. Internally shift = 8 − amt. |
| channel `Volume` 0..15 | `Level` | 0..15 (start level) |
| Wave `Level` | `Level` | Mute / 25% / 50% / 100% |
| `Vel>Vol` | `Vel to Level` | Off / On |
| `Transp` | `Semi` | `+12 st` style |
| `Fine` | `Fine` | `+25 ct` style |
| `Pan` | `Pan` | Mute / Left / Both / Right |
| `Length` | `Length` | Off, then ms per channel: Pulse/Noise ≈ 4..250 ms, Wave ≈ 16..1000 ms |
| Wave `Wave` 1..16 | `WAV Shape` | the wave name (Saw, Square, Triangle, Sine, Pulse25, Pulse12, HalfSine, Organ), user slots `Wave 09` |
| Noise `Width` | `NOI Mode` | `Hiss 15` / `Buzz 7` |
| Noise `PitchMd` | `NOI Pitch` | Track / Fixed |
| Noise `Shape` 0..111 | `NOI FixPitch` | pitch-sorted values 0..N-1 (duplicate NR43 frequencies collapsed), strictly higher value = higher pitch |
| channel `Table` | `Table` (kept) | Off or the table name (MajArp, MinArp, Octave, Vibrato, KickDrop, Fade, Rise, Hat; user `Table 09`) |
| `Tbl Clock` Sync/Free | `Table Sync` | Tempo / Free |
| `Tbl Rate` | `Table Rate` | `96/beat` … `24/beat` … `3/beat`. The default 24/beat is noted as "LSDJ timing" in the manual, not in the value text. |
| `Tbl Phase` Note/Grid | `Table Start` | Note / Grid |
| `Free Hz` | `Free Rate` | `59.73Hz` |
| `Note Off` Cut/Ring | `Note Off` | Cut / Hold |
| `Row Vol` | `Row Vol` (kept, matches LSDJ's VOL column) | `--` shown as `Keep`, else hex |
| `Row Transp` | `Row Semi` | `+12 st` |
| `Row Cmd1/2` | `Row Cmd1/2` | letter plus word, 8 characters or fewer: `-`, `D Delay`, `E Env`, `H Hop`, `K Kill`, `L Slide`, `N Noise`, `P Pitch`, `R Retrig`, `S Sweep`, `V Vib`, `W Wave` |
| `Row Val1/2` | kept | hex |
| `Imp File/Go/Undo/Rescan` | `Import File` / `Import Now` / `Import Undo` / `Import Scan` | triggers stay Off/On toggles. Note: "Press" was dropped because it turned triggers into 2-way segments. |

## Kept
`Duty` (12.5%/25%/50%/75%), `Retrigger` (Always/Legato), `Bend Range`, master `Volume` (dB), `Edit Table`, `Edit Row`, `PU1 On` etc., and the page names MAIN / PULSE 1 / PULSE 2 / WAVE / NOISE / TABLE.

## Rejected, with the reason
- **"Mod Seq" / "Macro" for Table.** It breaks LSDJ identity, collides with the MPC sequencer, and the factory-name value text answers the wavetable worry.
- **"PW" for Duty.** There are only 4 steps, so PW would imply continuous modulation.
- **"Auto Cut" / "Gate" for Length.** Gate means held duration, which is wrong here. ms value text fixes the clarity.
- **"Release" as a name.** The Game Boy has no release stage.
- **"Ring" value.** It reads as ring modulation.
- **"LR" for Pan both.** Unclear outside LSDJ.
- **Ratio value text for sweep shift.** It describes the frequency register, not the pitch.
- **"Transpose" on channels.** "PU1 Transpose" is 13 characters.
- **A Legato toggle replacing Retrigger.** Same information, no gain.
- **`Row Env` / `Row Level`.** It wouldn't match the VOL column of the LSDJ grid.
