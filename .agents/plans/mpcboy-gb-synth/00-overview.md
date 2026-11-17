# mpcboy — Game Boy mono synth for MPC OS: overview

Status: **Ready for implementation.** Nothing has been implemented. This directory is a plan only.

## Application context

```json
{
  "application_context": {
    "has_active_users": false,
    "backward_compatibility_required": false,
    "feature_flags": "not-applicable",
    "confirmation_digest": "e2b6789d46227ccff329e2e5b12385d1c8962f595a86d0fc199db1cb5182d16d",
    "confirmed_at": "2026-11-17T04:12:02Z"
  }
}
```

The user confirmed: no active users, no compatibility promise, so feature flags do not apply. Parameter ids, the state format and the plugin uid may change freely until the first tagged release, which happens only after the definition of done. From then on, the freeze rule in [04-plugin-params-ui-state.md](04-plugin-params-ui-state.md#parameter-table) applies (ids never renamed, parameters appended, state version bumped). That rule is a product decision for future users, not a compatibility requirement of this plan. No old-format loaders are built.

## Change type and affected areas

- **Change type:** greenfield product. A new repository with a C/C++ DSP core, an MPC plugin target, a native test harness and packaging.
- **Affected areas:** the whole repository. Today it holds only `README.md` and `LICENSE` (MIT, 2026 Matt Spurlin) at commit `fa1f446`.

## Requested outcome

An instrument plugin for the Akai MPC Live 3 (MPC Live III, Gen2, aarch64) for making Game Boy music:

1. All 4 Game Boy sound channels are available: Pulse 1 (with sweep), Pulse 2, Wave, Noise.
2. It is a **mono synth**: one note at a time. The user chose **"All 4 layered"**: one mono voice triggers every enabled channel together, each with its own settings and its own table, mixed like a stacked instrument.
3. LSDJ-style **tables** are available and their tick clock **syncs to the host BPM**.
4. It follows the pattern of the referenced community MPC plugins (Plaits, Model72, Profit-5): a native `.so` loaded by the MPC's built-in plugin host, with a touch-screen skin and Q-Link mapping.

## Measurable success criteria

| # | Criterion | Proven by |
|---|---|---|
| S1 | The plugin appears under External Plugins on an MPC Live III running MPC OS 3.10.1 or later, inserts on a track and makes sound from pads and from a sequence. | WP10 device checklist |
| S2 | One MIDI note triggers all enabled channels at once. A second overlapping note does not add a voice. | WP4 unit tests; WP10 |
| S3 | Each channel can be switched on or off independently and sounds alone when the other three are off. | WP4 render tests |
| S4 | At constant tempo, table rows advance exactly once per tick, row 0 at note-on. The tick clock places tick k within ±1 host frame of `noteOnFrame + k · 44100 · 60 / (BPM · ticksPerBeat)` for block sizes 32–512 and BPM 40–300. Register writes land within ±2 frames; row changes in rendered plugin audio within ±3 frames. | WP6 tests 1, 9; WP7 test 7 |
| S5 | Doubling the host BPM halves the time between table rows, measured in the plugin's rendered audio at 60, 120, 240 BPM. | WP6 test 2; WP7 test 7; WP10 row 4 |
| S6 | With the transport playing and `Table Phase = Grid`, ticks land within ±1 frame of `ppq = k / ticksPerBeat`, and a loop jump does not emit a burst of ticks. | WP6 tests 4, 5; WP7 test 8; WP10 row 5 |
| S7 | Project save and reload on the device restores every parameter, all 16 tables and the wave bank. | WP7 state tests; WP10 |
| S8 | One instance uses at most 5 % of a core on the Live III at 128-frame blocks (mean ≤ 145 µs and p99 ≤ 290 µs per block, measured in-plugin) while holding a note with 4 channels and 4 tables running. | G-CPU (WP3); WP10 row 9 |
| S9 | The SDK host simulator `test` command reports no FAIL for the native build, and the SDK validator passes for the built folder. | WP9 |

## Scope

- One instrument plugin, stereo output, monophonic, 4 layered GB channels on one emulated DMG APU.
- 16 user-editable tables of 16 rows, bound per channel, ticked by a host-synced or free clock.
- A 16-slot wave bank for the Wave channel with built-in waveforms.
- Built-in programs (factory presets).
- Builds for MPC Gen2 (arm64, the target device) and Gen1 (arm32, build-and-validate only), plus a native macOS build for tests and the host simulator.
- Importing tables and waves from an LSDJ 9.4.0 `.sav` placed in the plugin's `Content/` folder (WP11, last).

## Non-goals

- Polyphony or MPE. The plugin stays mono.
- Separate output pairs per channel. Stereo out only, panned per channel through NR51. See D6.
- An LSDJ song, chain or phrase player. Grooves. Kit instruments. The LSDJ soft-synth wave generator.
- MIDI output. The MPC discards plugin MIDI output (SDK `docs/host-contract.md`).
- Desktop VST3/AU. The SDK's `--target desktop` VST2 is allowed for listening but is not a deliverable.
- Rooted-device installs (SSH, `MPC.settings` edits). Only the official MPC OS 3.10.1+ `VST2 Plugins` folder route.
- Publishing to the Open MPC catalog. Deferred (see [07-execution-handoff.md](07-execution-handoff.md)).

## Constraints

- **Host contract** (Akai SDK `docs/host-contract.md`): fixed 44.1 kHz; float non-interleaved stereo out; frames per call 32–512 (the announced block size is only a maximum); no allocation, locks, logging or I/O in `process()`; parameter 0 is nudged on every prepare; parameter count is read once; state is an opaque chunk; transport (`bpm`, `ppqPos`, `playing`) is available every block.
- **Binary rules** (SDK `docs/building.md`, `docs/plugin-folder-format.md`): ELF arm32 `armv7 hard-float` and AArch64; glibc symbol versions ≤ 2.39 (the SDK targets 2.31); static C++ runtime; only `VSTPluginMain` exported.
- **Install route:** MPC OS 3.10.1+, Preferences → Enhancements (Beta) → Enable Enhancements + External Plugin Support, folder `<drive>/VST2 Plugins/<Manufacturer> - VST - <Name>/`.
- **Licences:** everything reused is MIT or Expat. No GPL code enters the repository (Model72 is GPL-3.0 and is reference only, not copied).

## Repository-grounded findings that drive the design

1. **Akai ships an official MIT SDK.** `github.com/inmusic-software/mpc-plugin-builder-sdk`, inspected at commit `febd2ca`. It provides a header-only VST2 framework (`framework/mpc_plugin.h`: `mpc::Plugin`, `ProcessContext`, `Transport{bpm, ppqPos, playing, tempoValid, ppqValid}`, `MidiEvent{sampleOffset}`, `addParam`, `addProgram`, `saveExtraState`/`loadExtraState`, `requestDisplayUpdate`, `notifyHostParameterChanged`, `MPC_PLUGIN_ENTRY`), DSP helpers (`framework/dsp/tempo.h`), a host simulator (`tools/host-sim/host_sim.cpp`), a skin generator (`tools/gen_ui.py`), a validator (`tools/validate.py`) and a zig cross-build (`tools/build.py`). Profit-5 2.0 already moved to it. The community wrapper repo (`sd88me/mpc-vst-plugins`, commit `66b7d9b`) now tells new plugins to start with this SDK.
2. **`tools/build.py` compiles only top-level `src/*.cpp` of the plugin project** (`find_sources`, line 189). It does not compile `.c` files or subdirectories. The reused SameBoy APU is C (`apu.c`). This is why D2 adds an own CMake build.
3. **`~/git/rackboy` (MIT, commit `245230d`) already has a host-agnostic GB core:** `rackboy_apu/` (C wrapper around SameBoy `Core/apu.c` at upstream `208ba4a`, Expat), `rackboy_table/` (LSDJ table interpreter: `table_trigger`, `table_tick`, commands D E H K L N R S V W), `rackboy_core/` (`VoiceState`, `pitch_square_11bit`, `pitch_wave_11bit`, `pitch_noise_nr43`, `taps_process`), `rackboy_sav/` (LSDJ 9.4.0 parser) and `src/rackboy_resampler.{hpp,cpp}` (48 kHz → host-rate 257-tap Kaiser sinc). None of it includes Rack or libDaisy headers. It has no tick clock and no note input: a gate edge is one tick and the base note is fixed at `kBaseMidiNote = 72`.
4. **rackboy separates channels with a "4-pass mute" (rackboy D-009):** the APU runs 4 times per block. A layered stereo mix needs one pass only. D5 adds a single-pass stereo function.
5. **rackboy measured SameBoy at 48 kHz as spur-clean and 44.1 kHz as aliasing (rackboy D-011).** The MPC is fixed at 44.1 kHz, so the APU runs at 48 kHz and is resampled. rackboy's resampler evaluates `std::sin` for each of 257 taps per output sample (rackboy D-012), about 11 million calls per second, so mpcboy precomputes a 147-phase polyphase table instead (WP3).
6. **No inspected repository has a BPM → tick formula.** `gb-lsdj-instrument-extractor` fixes 1 frame = 1 row. rackboy ticks on gate edges. The plan defines its own clock (D7).
7. **The SDK host simulator fixes tempo at 120 BPM** (`host_sim.cpp` line 198). Tempo-sync behaviour therefore needs its own native test harness (WP6).
8. **SDK threading:** lifecycle calls may run on the UI thread during `process`; `prepare()` can be called lazily on the audio thread; a state or program load restores parameters (through `parameterChanged`), then extra state, then `reset()` runs on the audio thread before the next block (`mpc_plugin.h` header comment and `internalLoadState`). WP5 and WP7 design the content handoff around this.
9. **The MPC has six function keys;** the SDK's skin generator warns that tabs beyond six are untested. The UI uses six pages.

## Key decisions

| # | Decision | Reason | Rejected alternative |
|---|---|---|---|
| D1 | Build on the Akai MPC Plugin SDK framework, pinned as a git submodule at `vendor/mpc-plugin-builder-sdk` (commit `febd2ca`). | Official, MIT, matches the device's host contract, provides host_sim, skin generator and validator. | The community `mpc-vst-plugins` wrapper (`mpc_engine_t`, int16 interleaved, Docker builds): older path, the repo itself now points new plugins at the SDK. JUCE/iPlug2: need Steinberg VST2 headers that can no longer be licensed (SDK `docs/building.md`). |
| D2 | Own CMake build that compiles C and C++ with zig using the exact SDK flags, for `native`, `arm32`, `arm64`. SDK Python tools are used for skins, validation and host_sim. | `build.py` cannot compile `apu.c` (finding 2). Native unit tests need a normal build anyway. | Compiling `apu.c` as C++ through a unity `.cpp` (fragile, unverified). Rewriting the APU in C++ (loses rackboy's tested, accuracy-checked SameBoy path). |
| D3 | Copy rackboy's core modules into `src/` with a `VENDORED.md` recording rackboy commit `245230d` and every local change. | Proven, MIT, host-agnostic, has tests. A shared library does not exist yet. | A git dependency on rackboy (pulls VCV code and its Makefile). Waiting on a shared-library extraction (see the request below; it is non-blocking). |
| D4 | One mono voice drives all 4 channels ("All 4 layered", user decision). Last-note priority with a held-note stack. | User choice. Matches LSDJ instruments stacked across channels. | One channel per instance. MIDI channel → GB channel. |
| D5 | One SameBoy APU per plugin instance, rendered in **one pass** to stereo at 48 kHz, panned by NR51, then resampled to 44.1 kHz by a precomputed polyphase filter. | 1× APU cost instead of rackboy's 4×; the layered mix needs no per-channel split; table lookup instead of per-tap `sin`. | rackboy's 4-pass mute (4× CPU). rackboy's per-sample `sin` resampler (too slow on the target CPU). |
| D6 | Stereo output only. Per-channel pan is LSDJ-style L / C / R / Off through NR51. | Keeps D5's single pass. Authentic to the hardware. | 4 stereo output pairs (forces 4-pass). Deferred. |
| D7 | Table tick clock = `ticksPerBeat` ticks per quarter note. Default 24 ticks/beat, which is LSDJ's tick at its default groove of 6 ticks per 1/16 step. `Table Phase = Note` counts from note-on; `Grid` aligns to host `ppqPos`. `Free` uses a fixed Hz rate (default 59.73 Hz, the DMG frame rate). | Gives LSDJ timing at any MPC tempo and keeps ticks sample-accurate. | Ticking at the DMG frame rate only (ignores BPM, which the user asked for). Using MIDI clock (the plugin receives transport, not clock). |
| D8 | Tables and the wave bank live in the plugin's extra state, edited on screen through cursor parameters (`Edit Table`, `Edit Row`, six cell parameters). The audio thread owns the live tables: screen edits (which arrive on the audio thread) write them directly, loads and imports go through a staging slot, and other threads read a published snapshot. | 16 × 16 × 6 = 1,536 cells cannot be plain parameters on a 16-knob device. The SDK's load order and threading would otherwise let editor parameters overwrite cells or let saves read torn data. | One parameter per cell (1,536 parameters). Fewer, fixed tables only. |
| D9 | Parameter 0 is `Bend Range`. | The host nudges parameter 0 on every prepare; bend range has no audible effect without a bend. | Master volume at index 0 (audible blip risk). |

## Change model

Before: an empty repository.

After (target architecture, all paths `new`):

```text
MIDI + Transport (SDK ProcessContext, 44.1 kHz, 32–512 frames)
        │
        ▼
MpcBoyPlugin::process()  (src/plugin/MpcBoyPlugin.cpp)
  ├─ pending reset? → Engine::reset(); ContentStore: apply staged content ──► live tables/waves   src/engine/content_store.*
  ├─ map host frame offsets → APU frames (48 kHz)
  ├─ for each segment between events:
  │     MonoVoice (note stack, bend, retrigger)       src/engine/mono_voice.*
  │     TickClock (BPM / grid / free)  ──► tick events src/engine/tick_clock.*
  │     4 channels → table_trigger/table_tick         src/table/ (from rackboy + local mods)
  │     rackboy_apu_run_stereo(48 kHz, one pass)      src/apu/ (from rackboy + local mod)
  ├─ PolyphaseResampler48To441                        src/dsp/polyphase_resampler.*
  └─ DC blocker, master gain → outputs[0], outputs[1]

audio thread: screen cell edits → live → published snapshot (seqlock)
UI / worker threads: loadExtraState / .sav import → staging slot; saveExtraState / formatValue ← published snapshot; reset() → resetPending_ flag
```

## Risks, assumptions and gates

| Item | Kind | Handling |
|---|---|---|
| R1 SameBoy single-pass stereo output may differ from the sum of rackboy's 4 passes. | Risk | WP3 equivalence test (≤ −60 dBFS RMS difference, rackboy's own golden threshold). |
| R2 CPU on Gen2: one SameBoy pass plus a 257-tap stereo polyphase filter. | Risk | WP3 native bench with a pass bar and the **G-CPU** device gate before WP4; WP10 row 9. Fallbacks in WP3. |
| R3 The MPC may not refresh cursor-bound cell parameters after the plugin changes them, or the 10 Hz refresh may disturb touch focus. | Risk | WP8 device spike **G-EDIT**. Fallbacks documented in WP8. |
| R7 The resampler adds ≈ 118 frames (2.7 ms) of delay. | Risk | Reported latency 0 for now; WP10 row 11 measures it on the device and decides. |
| R4 Transport fields while stopped (`tempoValid`, `ppqValid`) are not documented per state. | Assumption | WP6 treats invalid tempo as "use last valid BPM, else 120" and `Grid` without `ppqValid` as `Note`. WP10 records observed device values. |
| R5 Gen1 (arm32) cannot be device-tested by the user. | Assumption | arm32 is built and validated only. README says so. |
| R6 SDK API at `febd2ca` may change upstream. | Risk | Submodule pin. Upgrades are separate work. |
| G-BUILD | Gate | WP1: native, arm32 and arm64 artefacts build from CMake; right ELF types; strict SDK validator passes on a minimal folder. |
| G-CPU | Gate | End of WP3: APU + resampler within budget on the Live III. If the device is not available it is recorded "pending" and becomes a release blocker (WP10 row 9). |
| G-EDIT | Gate | WP8 device spike, before WP8's full editor. |
| G-DEVICE | Gate | WP10: S1, S5, S7, S8 checked on the Live III. |

## Unresolved decisions

| Decision | Blocking? | Owner | Resolution point |
|---|---|---|---|
| Manufacturer string and 4-char uid. Proposed and used from WP1: manufacturer `mpcboy`, name `mpcboy`, uid `"MbGb"`. | Non-blocking | User | Any time before the first zip is shared; changing them before the first tagged release costs only a rebuild (no users). |
| Extracting rackboy's core into a shared library. | Non-blocking | User (rackboy owner) | See the cross-repository request below. Local work vendors a copy. |

## Cross-repository request

| Request | Owner | Consumers | Affected WPs | Blocking | Unblock evidence |
|---|---|---|---|---|---|
| `$HOME/.agents/projects/rackboy/requests/2026-11-17-shared-gb-core-library.md` | User, as rackboy maintainer | Demonstrated: rackboy's VCV plugin (in tree; the Daisy build is currently broken per `daisy/Makefile`). Prospective: mpcboy (this plan, WP2). | WP2 (would replace the vendored copy) | **Non-blocking.** mpcboy vendors a copy (D3). | A tagged release of a shared core that builds without VCV/Daisy code. Adoption would be a separate mpcboy change. |

## Document map

| File | Purpose |
|---|---|
| [00-overview.md](00-overview.md) | This file: context, decisions, success criteria. |
| [01-foundation-and-build.md](01-foundation-and-build.md) | WP1 repository scaffold and CMake/zig build. WP2 vendoring the rackboy core and doctest. |
| [02-apu-and-voice.md](02-apu-and-voice.md) | WP3 single-pass stereo APU and resampler. WP4 mono voice and the 4 layered channels. |
| [03-tables-and-tempo-sync.md](03-tables-and-tempo-sync.md) | WP5 table engine extensions. WP6 the BPM-synced tick clock. |
| [04-plugin-params-ui-state.md](04-plugin-params-ui-state.md) | WP7 SDK plugin class, parameters, programs, state. WP8 skin pages and the table editor. |
| [05-packaging-and-device-verification.md](05-packaging-and-device-verification.md) | WP9 MPC folder packaging and validation. WP10 Live III verification and CPU bench. |
| [06-lsdj-sav-import.md](06-lsdj-sav-import.md) | WP11 importing tables and waves from an LSDJ `.sav`. |
| [07-execution-handoff.md](07-execution-handoff.md) | Ordered work packages, gates, definition of done, deferred work. |
