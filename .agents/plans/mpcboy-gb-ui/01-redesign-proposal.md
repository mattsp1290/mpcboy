# mpcboy UI redesign proposal: "Handheld Mixer"

Status: proposal from two subagent debate rounds. Not implemented. Builds on [00-design.md](00-design.md) and keeps its shared-UI architecture: one layout source, one art set, one geometry, and the MPC skin plus Mac editor generated together.

## How this was decided

1. **Terminology debate.** Three stances argued it out: mainstream producer, chiptune/LSDJ native, and hardware/screen pragmatist. Each gave an opening and a rebuttal, and the moderator ruled. The result is in [redesign/agreed-terms.md](redesign/agreed-terms.md).
   - Command-letter meanings and length timing were checked against the code.
   - Length counts at 256 Hz: up to 250 ms on Pulse and Noise, up to 1000 ms on Wave.
2. **Redesign debate.** Three stances argued it out, using the round-1 terms. Each produced a palette with computed WCAG contrast and a mock, then a rebuttal and a hybrid.
   - Handheld-authentic: [mock-handheld-pulse1.png](redesign/mock-handheld-pulse1.png), [palette-handheld.png](redesign/palette-handheld.png)
   - Modern-synth clarity: [mock-clarity-main.png](redesign/mock-clarity-main.png), [palette-clarity.png](redesign/palette-clarity.png)
   - Tracker-native: [mock-tracker-pulse1.png](redesign/mock-tracker-pulse1.png), [palette-tracker.png](redesign/palette-tracker.png)

All three advocates converged on the same core hybrid. The moderator resolved what was left.

## What is wrong with the current UI (agreed by all three)

- **Everything is one green.** Name text `8bac0f` on the panel `1b4a1b` is **3.92:1**, which fails WCAG AA. Names, values and on/off segments are all near-identical greens, so hierarchy comes only from font size.
- **Pages list parameters in registration order.** Rows mix unrelated controls, so a Q-Link bank of four means nothing. Table sits stranded on its own.
- **MAIN is admin, not an overview.** It shows On switches but no level, timbre or table.
- **The MPC TABLE page shows a decorative fake grid.** Tables, the product's differentiator, sit only on F6.
- **The four channels look identical.**

## The proposal

### Concept

The page is the face of a handheld:
- a dark slate bezel;
- grey shell-plastic knobs;
- printed section legends that name each Q-Link bank.

Each channel has its own Game Boy Color shell colour, so the four layered voices read at a glance.

Green is reserved for the screen: a negative DMG LCD that shows real state. On the Mac it is live, with the table rows and a playhead. On the MPC it shows parameter-driven readouts.

MAIN becomes a four-channel mixer.

### Palette

Contrast ratios below were re-computed by the moderator. Every text pair passes AA.

| Role | Hex | Notes |
|---|---|---|
| Overlay / `style.background` | `#1C1D27` | Firmware knob and menu overlays: white on it is 16.74:1 |
| Bezel (page field) | `#2A2C3B` | |
| Plate (slot card) | `#343747` | 1 art-px highlight `#4A4E62`, shadow `#121219` |
| Primary text (values) | `#ECEAE4` | 11.47 on bezel, 9.79 on plate, 13.92 on overlay |
| Secondary text (names) | `#A3A7B0` | 5.72 on bezel, 4.88 on plate |
| Shell plastic (knob caps, Mac tab bar) | `#CBC6BB` | hi `#E4E0D6`, dark `#948F86` |
| Knob pointer | `#262A55` | 7.98 on shell |
| Off (rubber segments/toggles) | `#4C4F61` | caption `#ECEAE4` at 6.72 |
| Focus / selected tab (A-button magenta) | `#E8558A` | used only for focus and the selected tab |
| **PU1** "Berry" | `#FF5A7E` | `#1C1D27` text on it: 5.59 |
| **PU2** "Dandelion" | `#F5C542` | 10.32 |
| **WAV** "Teal" | `#35C9C1` | 8.19 |
| **NOI** "Grape" | `#9D8CFF` | 6.08 |
| LCD field | `#0F380F` | DMG darkest |
| LCD ink | `#9BBC0F` | 6.02 on the field |
| LCD cursor row | `#9BBC0F` fill / `#0F380F` ink | 6.02 |
| LCD playhead row | `#2A5C2A` / `#E0F8D0` ink | 6.95 |
| LCD beat row (every 4th row) | `#184818` | ink on it 4.85 |

Rules:
- **Channel colours are for fills, arcs, stripes and LEDs, never body text on plates.** On a plate, PU1 and NOI text would fall below AA. Text on a channel fill uses `#1C1D27`.
- **Colour-blind check.** The channel colours stay at least ΔE 23.6 apart under protan, deutan and tritan simulation, as computed by the clarity advocate. The tag text (PU1/PU2/WAV/NOI) and the fixed position are redundant cues, so colour is never the only signal.
- **On or selected segments, toggle fills, knob arcs and lit fader cells use the page's channel colour.** MAIN and TABLE use the DMG accent `#9BBC0F` for these. Magenta is never used for state, only for the selected tab and focus. This avoids magenta meaning everything.

Why this palette:
- **It reads as a Game Boy:** a slate bezel, the classic DMG LCD greens, A-button magenta and Game Boy Color shell hues.
- **It sits well in Ableton's dark theme and the MPC's own UI:** a dark neutral base, so no glaring green slab.
- **It keeps the MPC's non-themable knob and menu overlays coherent:** the page is already dark, so opening an overlay doesn't flip the screen.

### Information architecture (6 pages, agreed names)

Q-Link bank direction becomes **Row**, so one screen row is one bank. Section legends are printed in the row gaps. The Q-number badges come from gen_ui's `slot_qlink` (13 + col − 4·row), not hand-numbered.

#### MAIN (first page): the mixer

| Row | col 0 | col 1 | col 2 | col 3 | Legend |
|---|---|---|---|---|---|
| 0 | PU1 Level | PU2 Level | WAV Level | NOI Level | LEVEL |
| 1 | PU1 Table | PU2 Table | WAV Table | NOI Table | TABLES |
| 2 | Table Sync | Table Rate / Free Rate | Table Start | Vel to Level | TABLE CLOCK |
| 3 | Volume | Bend Range | Retrigger | Note Off | MASTER |

- **Header band.** Each column gets a channel-colour strip with an LED and its `PUx On` toggle, touch only.
  - **Device gate:** check whether the firmware allows these. If not, the On switches live only on the channel pages, where they already are.
- **Channel strips (columns 0–3)** carry the channel colour on their top stripe and lit cells.
- **Table Rate and Free Rate share a slot** with complementary `visibleWhen` (Tempo/Free), via a generator bounds post-process.
- **The table clock moves here.** The TONE row (Duty/Shape/Mode) was dropped from MAIN to make room. Timbre lives on the channel pages, and the TABLE page has no room for the clock.

#### PULSE 1 / PULSE 2 / WAVE / NOISE (same grammar on every channel page)

| Row | PULSE 1 | PULSE 2 | WAVE | NOISE | Legend |
|---|---|---|---|---|---|
| 0 | LCD (cols 0–2) · PU1 On | LCD · PU2 On | LCD · WAV On | LCD · NOI On | (screen) |
| 1 | Duty, Semi, Fine, Table | Duty, Semi, Fine, Table | Shape, Semi, Fine, Table | Mode, Semi, Pitch, Table | TONE |
| 2 | Level, Env Dir, Env Time, Length | same | Level (Mute/25/50/100), –, –, Length | Level, Env Dir, Env Time, Length | ENVELOPE |
| 3 | Swp Time, Swp Dir, Swp Amt, Pan | –, –, –, Pan | –, –, –, Pan | –, FixPitch, –, Pan | SWEEP + OUT |

- **Column 3 always holds Table / Length / Pan.** Unused slots stay blank in the same place on every page, with decals "SWEEP: PU1 ONLY" and "NO ENVELOPE".
- **NOI FixPitch** sits under NOI Pitch and is shown when Pitch = Fixed.
- **The row-0 LCD (negative DMG) is a channel monitor:**
  - Mac (live):
    - the channel's table window, ±3 rows around the playhead;
    - the duty or wave shape;
    - the envelope steps;
    - the current note;
    - the breadcrumb `PU1 > MAJARP ROW 02`.
  - MPC:
    - a live Value label with the table name;
    - parameter-bound film-strip Images (ui-reference 6.6): duty wave (4 frames), wave shape (16), envelope slope, direction arrow.
    - These are device-gated and ship last. The fallback is static channel art plus the live table-name label.

#### TABLE (library and row editing)

| Row | cols 0–1 | col 2 | col 3 |
|---|---|---|---|
| 0 | LCD | Edit Table | Edit Row |
| 1 | LCD | Row Vol | Row Semi |
| 2 | Row Cmd1, Row Val1 | Row Cmd2 | Row Val2 |
| 3 | Import File, Import Now | Import Undo | Import Scan |

- **The LCD moves to cols 0–1,** out from under the MPC Q-Link sidebar (x ≥ 1025).
  - Mac: the full 16-row live grid, with hex row numbers, beat rows, the playhead row, the inverted cursor row and a column caret. Grid cells are clickable.
  - MPC: a printed command reference card (D Delay, E Env, H Hop, K Kill, L Slide, N Noise, P Pitch, R Retrig, S Sweep, V Vib, W Wave), plus a live cursor-row readout from Value labels. No fake rows.
- **Import triggers show `Press`.**

### Visual language

- **Knob (90×90, 127 frames):** a shell-grey cap with a navy pointer, and a pixel-stepped arc in the page colour.
- **Level fader (248×36):** 15 discrete pixel cells that match the 0..15 hardware steps. Lit cells are the page colour.
- **Segments (2/3/4):**
  - Off: rubber `#4C4F61`.
  - On: page-colour fill with a corner marker.
  - Captions: one colour (gen_ui limitation). `#ECEAE4` reads on rubber (6.72), but on light fills such as Dandelion it needs a dark caption.
  - Resolution: per-page kits pick the caption colour per page, or the selected fill keeps a dark inner plate. To be decided in implementation.
- **Toggle:** a rubber pill, filled with the page colour when on.
- **Menu slider** (Table, Shape, Row Cmd, Table Rate): a navy "cartridge label" value window.
- **Page art:**
  - the slate bezel with raised plates per slot;
  - the top band: an LED in the page colour, the pixel-font page title, a breadcrumb (Mac) and the `MPCBOY` wordmark left of x = 1025;
  - section legends in the page colour, printed in the row gaps.
  - No dot grid outside the LCD.
- **Typography:**
  - MPC: Nexa Bold uppercase, with names in `#A3A7B0` and values in `#ECEAE4`. Values are larger than names, the reverse of today.
  - Mac: the 5×7 pixel font, with names at 2× and values at 3×.

## Rejected ideas, and why

- **Tracker cells replacing knobs and segments (ValueSlider, 60 px cells).** They lose tap-to-select on a touchscreen, use a widget we have never shipped, and make touch targets too small.
- **`Edit Col` / `Edit Val` parameters.** They add 2 parameters with steps that vary per column, and it is unclear on the MPC which table they edit. Revisit only if users ask for knob-only table editing on channel pages.
- **A neutral-only base with no bezel.** It loses the Game Boy identity.
- **A green-on-green negative page.** It brings back the fatigue the current UI has.
- **Import on the WAVE page.** It was a weak placement, conceded by its own advocate.

## Implementation outline (for a follow-up plan)

Ordered by risk:

1. **Terms.** Rename and relabel parameters and value text per [agreed-terms.md](redesign/agreed-terms.md), in `param_bridge.cpp`, `formatValue` and `ui/mpcboy_ui.json`.
   - This includes the `Swp Amt` inversion (shift = 8 − amt), ms value text for Length per channel, and `NOI FixPitch` orientation.
   - Update the param-name agreement test.
2. **Palette, art and layout on the existing grid.**
   - New `ui_art` palette and backgrounds, and per-page kit variants. The kit variants are a post-process that renames the assets per page.
   - Row bank direction, section legends, and the reordered pages.
   - The TABLE LCD moves to cols 0–1, with the command card.
3. **Mac editor.**
   - The row-0 LCD channel monitor and TABLE grid improvements: 16 rows, playhead, beat rows, clickable cells.
   - The breadcrumb, and channel-colour tabs.
4. **MPC dynamics (device-gated, last):**
   - live Value labels in the LCDs;
   - parameter-bound film-strip Images;
   - header-band On toggles;
   - the overlapped `visibleWhen` slot.

Each item has a static fallback, and the existing G-EDIT gate covers the device checks.
