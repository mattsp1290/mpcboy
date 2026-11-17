# mpcboy Game Boy UI: shared design for the MPC skin and the Mac editor

Status: design, revised after two independent reviews. Not implemented. Branch: `feat/gb-ui`, from `feat/mac-vst` (`d39a82e`).

## Goal and what "shared" means

Both builds show one Game Boy–inspired UI:

- **MPC:** the firmware draws pages from `Plugin Skins/TUI.json`, `Q-Links.json` and PNGs.
- **Mac (Ableton):** a native VST2 editor window replaces Live's generic parameter list.

Shared, from a single source of truth:

1. **Layout:** pages, controls, widget kinds and `visibleWhen`, in `ui/mpcboy_ui.json`.
2. **Art:** kit film strips, buttons, page backgrounds and the pixel font, rendered once by `scripts/build_ui.py`.
3. **Geometry:** slot bounds, widget sub-bounds and segment positions. These are taken from the same in-memory component tree that produces the MPC `TUI.json`, so the numbers are never retyped.
4. **Value text:** `formatValue` in the plugin, shown by both.

Not shared, because the MPC firmware implements its own interaction: hit-testing, drag feel and overlays. `src/ui` is a Mac-only, platform-free and unit-tested interaction layer that copies the firmware's behaviour as documented (vertical drag for knobs, horizontal for faders, tap for segments and toggles). The device check (the existing G-EDIT gate) confirms the feel.

## Verified facts (SDK `febd2ca`)

- **`gen_ui` imports as a module.** Its functions:
  - `load_params(path)` and `ParamTable(params)`.
  - `load_ui(path, table)`, which returns `(pages, style, knob_image, frames, kit)`.
  - `build_tui(pages, params, style, knob_image, frames, kit_assets)`, where `kit_assets` is `{name: (file, frames, (w, h))}`.
  - `build_qlinks(pages, params)`.
  - `dump_json(Obj)`, which preserves duplicate keys.
- **`load_ui` rejects unknown keys.** Allowed keys:
  - Top level: `pages`, `style`, `kit`, `knobImage`, `knobFrames`.
  - Page: `name`, `controls`.
  - Control: `param`, `widget`, `visibleWhen`, `text`, `overlay`, `polarity`.
- **`build_tui` returns an `Obj` pair-tree.** A tab's `componentDefinition` ends with `componentsData` (a list), so a background `Image` entry can be inserted at index 0. Re-reading the emitted JSON with `json.load` would drop the duplicate `version` keys the overlays need (ui-reference 1.8).
- **Font is a module global.** `label_data` uses `FONT_NAME` and style `Regular`, case `Original`, looked up at call time.
- **Segment captions are one Label per segment, in `style.text` colour.** The colour doesn't change with on/off state.
- **`style.background` also fills the full-screen Knob and Menu overlays,** whose own colours are not themable.
- **The slot grid takes nearly the whole tab.** It is `SLOT_X = [23, 343, 663, 983]`, `SLOT_Y = [45, 196, 346, 497]`, 278 × 107. The free bands are the top 45 px and margins of roughly 20 px. The hardware Q-Link sidebar overlays x ≥ 1025.
- **`validate.py` checks only that referenced images exist,** plus handles and tabs. Extra images are harmless.
- **`package_mpc.py` fails on any `WARNING`/`NOTE` printed by the skin generator.**
- **`make_ui_kit.py` has a stdlib `Canvas` + `write_png`.**
- **SDK dispatcher:** `effEdit*` return 0 and the flags lack `effFlagsHasEditor` (`mpc_plugin.h:713`, `:799`). `effClose` deletes the plugin and frees the `AEffect`. `host_` and `effect_` are public. Opcodes 0, 43 and 44 exist. `ERect` is `{top, left, bottom, right}`, all `VstInt16`.
- **Parameter indices:** cell parameters 51–58 and import parameters 59–62 are `automatable = false`. On the audio thread, `onCellParam` and `ImportTriggers::onValue` act; off it, they reject the call.

## Decisions

### Layout source (`ui/mpcboy_ui.json`)

- It has two top-level sections:
  - `"mpc"` holds exactly the keys `gen_ui` accepts: `pages` (the six pages from today's `plugin/ui.json`) and `style`.
  - `"gb"` holds the Game Boy extras: the palette, page titles, and decorative panels per page (for example the TABLE page's LCD panel over the slot block at rows 0–1, cols 2–3).
- `build_ui.py` writes the `"mpc"` section to a temp file for `load_ui`, so `check_keys` still validates it.
- `plugin/ui.json` is removed. README, `package_mpc.py` and the docs are updated to point at `ui/mpcboy_ui.json`.

### Generator (`scripts/build_ui.py`, stdlib only)

1. Load `build/params.json` (the host_sim dump) and the `"mpc"` section with `gen_ui.load_params` / `load_ui`. This keeps gen_ui's validation, including its page-name `WARNING`s.
2. Patch `gen_ui` module globals before building: `FONT_NAME = "Nexa"`, and a wrapped `label_data` that uses style `Bold` and case `Upper Case`. Pinned to the vendored SDK commit. A Python check asserts font, style and case in the output.
3. Build `kit_assets` from our own Game Boy kit. The kit uses gen_ui's exact names, sizes and frame counts: `knob_uni` / `knob_bi` 90², 127 frames; `fader_h_uni` / `fader_h_bi` 248×36, 127; `toggle_on` / `toggle_off` 64×32; `seg{2,3,4}_{on,off}` 122/80/58 × 40.
4. Call `build_tui` and `build_qlinks`. Post-process **only the in-memory `Obj` tree**: insert a background `Image` (version 2, `Regular`, colour `0`, bounds `0 0 1280 628`, `acceptsHWFocus "No"`) as the first entry of every tab. Write the files with `gen_ui.dump_json`. Never round-trip through `json`.
5. Copy only the PNGs the tree references (gen_ui's `referenced_images()` logic) into `Plugin Skins/`.
6. Emit `src/ui/generated/ui_layout.inc`, walking the same `Obj` tree:
   - every tab's controls: param index **and param name**, definition kind (`knobUni`, `knobBi`, `faderUni`, `faderBi`, `toggle`, `radioN`, `sliderWithFocus`, plus a `Menu` flag), slot bounds, the `visibleWhen` handle / index / count, and segment captions;
   - every `localComponentDefinitions` entry's children (name, type, bounds, image, frames);
   - the theme colours, asset files and sizes, and page titles.
7. Emit `ui/art/` (all art, including editor-only art: tab bar, LCD panel, cursor) and preview composites in `build/ui-preview/<page>.png`, so the look can be reviewed without a device.
8. Rendering uses the stdlib only, through `make_ui_kit`'s `Canvas`/`write_png` or a small local equivalent. Output is byte-reproducible.
9. The generated files are committed: `ui/art/**`, `src/ui/generated/ui_layout.inc`, `src/ui/generated/pixel_font.inc`. Native builds and tests then need no generator. `check.sh`, after the native build and dump, runs `build_ui.py` into the tree and fails on `git diff --exit-code` if the committed files are stale. `package_mpc.py` calls `build_ui.py` for `Plugin Skins/` instead of `gen_ui.py`, and keeps the `WARNING`/`NOTE` gate.
10. A C++ test checks the plugin's parameter table against `ui_layout.inc`: each control's param index has the recorded name. This catches drift between the dump and the code.

### Game Boy look (original art; no Nintendo marks or wording)

- **Palette:**
  - LCD greens `0f380f` / `306230` / `8bac0f` / `9bbc0f`.
  - Shell `c4c0b8` / `8b8680`, label accent `5c5a6e`, button magenta `8e1c4f`.
  - A light caption green `e0f8d0`.
- **Page background (1280×628):**
  - A dark-green field with a lighter LCD "screen" panel behind the slot grid. The panel is tight to the slots, with about 20 px margins.
  - The page title is in the pixel font in the top 45 px band (`PULSE 1`). A small `MPCBOY` wordmark sits top right, left of x = 1025. A subtle dot grid.
- **`style.background` stays dark (`ff0f380f`),** so the firmware's knob and menu overlays stay readable. Their look is not themable; this is noted.
- **Kit (pixel art, 2 design px per art pixel):**
  - **Knob:** a round dial with dithered greens and a notch pointer. The bipolar variant fills from the top centre.
  - **Fader:** a segmented pixel bar.
  - **Toggle:** a pill button.
  - **Segments:** both fills dark: off is `306230`, on is `0f380f` with a pixel corner marker. Captions use the single colour `e0f8d0`, which reads on both fills.
- **MPC labels:** Nexa Bold, uppercase. Names in `8bac0f`, values in `9bbc0f`, on the dark field.

### Mac editor

- **Size:** the window is 1280 × 704 points (628 content plus a 76-point tab bar). That is one point per design unit, so on Retina every art pixel maps to an integer number of device pixels.
- **Drawing:**
  - Interpolation is none, and rects are snapped with `convertRectToBacking`.
  - `viewDidChangeBackingProperties` is handled.
  - Film strips are decoded once with ImageIO and cropped per frame.
  - Only controls whose value or visibility changed are invalidated.
- **Text:** all editor text uses the shared 5×7 pixel font from `pixel_font.inc` (the same glyphs the generator uses for page titles), at an integer cell scale.
- **TABLE page:** the Mac draws an LSDJ-style grid inside the 2×2 slot block (598 × 258 design units).
  - It shows a header plus the 8 rows around the cursor of Edit Table. The cursor row is inverted.
  - It reads `ContentStore::readPublished` at 30 Hz.
  - It updates only while `process()` runs, which is acceptance-noted.
  - The MPC shows the same block as decorative LCD art.
- **Thread and edit contract** (the editor runs on the main thread):
  - **Params 0–50:**
    - Mouse-down sends `audioMasterBeginEdit`.
    - Each change sends `setNormalised`, then `audioMasterAutomate`.
    - Mouse-up sends `audioMasterEndEdit`.
    - Discrete clicks send begin, automate and end together.
    - Any open edit is ended on `effEditClose`, on window resign, or when mouse tracking is cancelled.
  - **Params 51–58 (cells) and 60–62 (triggers):** no begin, automate or end, because they are not automatable.
    - The editor calls `setNormalised` for display, then posts to **per-slot atomics**.
    - Cells have one 64-bit slot per column packing `{table, row, value, valid}`, captured at click time. Triggers have one flag per trigger.
    - This is latest-wins, safe with several producers, and cannot overflow.
  - **51–52 and 59 (cursor and file pick)** keep today's behaviour.
  - Host or other off-audio-thread writes to 51–58 and 60–62 stay rejected, as today.
- **Drain point in `process()`:** after `store_->beginBlock()` and the bulk-generation check, before the cursor resync.
  - Cells call `onCellParam` with the captured table and row.
  - Triggers call a new `ImportTriggers::acceptFromEditor(k)`, which sets `pending` and honours the debounce. The edge state from the main-thread `setNormalised` would otherwise swallow it.
  - The drain also runs on the early-return paths (silent / unsupported rate, no outputs).
- **Dispatcher wrap** (mac-vst build only, `src/plugin/plugin_entry_mac.cpp`):
  - Use `MPC_PLUGIN_ENTRY_BODY`'s steps: the version check, then `createEffect`. Check for null.
  - Set `effFlagsHasEditor`, keep the SDK dispatcher, and install a wrapper:
    - **`effEditGetRect`:** writes `*(ERect**)ptr` pointing at a plugin-member `ERect {0, 0, 704, 1280}` and returns 1. The size is the same before and after open.
    - **`effEditOpen`:** ptr is the parent `NSView*`. Add the subview (idempotent) and return 1.
    - **`effEditClose`:** tears down safely when nothing is open.
    - **`effEditIdle`:** no-op.
    - **`effClose`:** tear down the editor (end open edits, invalidate the timer, remove the view) **before** forwarding. Touch nothing afterwards.
  - Both `VSTPluginMain` and `main_macho` are still exported.
  - The non-mac builds keep `MPC_PLUGIN_ENTRY` unchanged, with the editor flag clear.
- **Lifecycle:**
  - Editor state is a member of `MpcBoyPlugin`.
  - The NSView holds a weak/raw back-pointer that is cleared on teardown.
  - The `NSTimer` (30 Hz, `NSRunLoopCommonModes`) targets a small proxy, so it never retains the plugin. It is invalidated on close, on `viewWillMoveToWindow:nil` and in the destructor.
  - ObjC class names carry a unique prefix (`MPCBOY_` plus the bundle version).
- **Resources:**
  - Art is found at `mpc::pluginFolder() + "/ui/"`, which is `Contents/Resources/ui` in the bundle.
  - Art is loaded in `effEditOpen`, with a flat-colour fallback for any missing file.
  - CMake mac-vst enables `OBJCXX` with `-fobjc-arc` and links Cocoa, QuartzCore, CoreGraphics and ImageIO.
  - `ui/art/**` is added to the bundle `DEPENDS` / `OUTPUT` and copied before codesign.

## Work packages

1. **Generator, art and MPC skin:**
   - Write `ui/mpcboy_ui.json` and `scripts/build_ui.py`.
   - Commit `ui/art/**`, `ui_layout.inc` and `pixel_font.inc`.
   - Update `package_mpc.py` and `check.sh` (stale check), and docs.
   - Acceptance:
     - `validate.py --strict` passes, and every tab's first entry is a background Image.
     - The font and case asserts pass.
     - Re-running the generator is byte-identical.
     - Preview PNGs exist and look right by eye.
2. **`src/ui` model (platform-free, doctest):**
   - Layout access, hit-testing per kind, drag and click mapping to values (bipolar, segments, toggles, menu sliders step on click/drag), film-strip frame, `visibleWhen`, tab bar, table-grid text rows, pixel-font text layout.
   - The param name and index agreement test.
3. **Plugin edit plumbing:** the editor edit API, per-slot atomics, `acceptFromEditor`, and the drain placement including early returns. Tests:
   - A cell edit is applied to the captured row even if the cursor moved.
   - An `Imp Go` via the editor path starts an import.
   - Begin/end are paired.
   - Edits on 51–62 send no automate.
4. **Mac editor:** `mac_editor.mm`, `plugin_entry_mac.cpp`, CMake.
   - Acceptance: in Ableton the GB UI shows; dragging a knob changes the sound and records automation; tabs switch; a table cell edit changes the grid and the sound; closing and reopening the editor and removing the plugin are safe.
   - `check.sh` and host_sim pass at all rates, the CFBundle load test passes, and MPC builds are unchanged apart from the skin.
5. **Verify** with check.sh, a visual check in Ableton via computer use, and MPC skin preview PNGs. The MPC on-device check stays the pending G-EDIT gate.
