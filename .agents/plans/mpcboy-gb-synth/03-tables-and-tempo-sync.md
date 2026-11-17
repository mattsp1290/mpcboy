# 03 — Tables and the BPM-synced tick clock

Covers WP5 and WP6. Decision D7 is in [00-overview.md](00-overview.md#key-decisions).

## WP5 — Table bank, wave bank and table-engine extensions

### Goal and prerequisite state

Prerequisite: WP4 is complete.

Goal: 16 editable tables and a 16-slot wave bank owned by the audio thread, updated from other threads without locks, with the LSDJ volume column, `P` and delayed `K` added to the vendored interpreter.

### Repository evidence

- `src/sav/rackboy_sav_types.h` (from rackboy): `RackboyTableRow { int8_t transpose; uint8_t cmd1, cmd1_param, cmd2, cmd2_param; }`, `RackboyTable { RackboyTableRow rows[16]; bool occupied; }`, `RackboyWaveFrame` with `occupied`, `RackboyBank` with `tables[32]` and `waves[255]`. `static_assert`s check these types. Commands are ASCII letters. The LSDJ table volume column is not represented.
- `table_tick` `W` only loads a wave when `bank->waves[param].occupied` is true (`rackboy_table.cpp` around lines 322–335).
- `table_tick`'s `E` path on CH3 writes the param to NR32 (around lines 213–220). The volume column for Wave reuses that path.
- `rackboy_table.cpp` implements D, E, H, K, L, N, R, S, V, W. `P`, `Z`, `A` are TODO. `K` kills at once. rackboy `docs/LSDJ_REFERENCE.md` (lines 116–121) marks the P, K, D and V parameter layouts "TBD — verify".
- SDK `framework/mpc_plugin.h` header comment: "A state load writes the parameters and calls loadExtraState() on the UI thread, then reset() is called on the audio thread before the next block: keep extra state in a staging copy inside loadExtraState() and swap it in from reset()." `setProgram` also sets `stateLoadPending_`, so `reset()` runs after a program change too.
- SDK host contract: screen edits arrive on the audio thread between `process` calls; project load, preset load and automation can arrive on the UI thread while `process` runs. State save (`effGetChunk` → `saveExtraState`) and value-text queries (`effGetParamDisplay` → `formatValue`) run on the UI thread, also during `process`.
- SDK `internalResume()` (from `effMainsChanged`, dispatcher thread) calls `prepare()` and then `reset()` directly. So `reset()` is **not** audio-thread-only.
- `src/core/rackboy_voice.h` line 193: `static_assert(sizeof(VoiceState) <= 16)` (exactly 16 today). rackboy `tests/sav/types_test.cpp` line 252: `kExpectedBankSize = 11028`.
- `rackboy_apu_write_wave_ram` writes NR30 = 0, then restores it. In SameBoy, NR30 = 0 deactivates the wave channel and restoring `0x80` does not reactivate it; the header says the caller retriggers. The table `W` handler does not retrigger, so today `W` silences the Wave channel until the next note.

### Change surface

| Item | Status | Description |
|---|---|---|
| `RackboyTableRow` in `src/sav/rackboy_sav_types.h` | `MPCBOY LOCAL MOD` | Add `uint8_t vol` and `bool has_vol`. Update the `static_assert`s. |
| `VoiceState` in `src/core/rackboy_voice.h` | `MPCBOY LOCAL MOD` | Add `int16_t pbend_accum` (1/16 semitone units), `int8_t pbend_step`, `uint8_t kill_ticks_remaining`. Reset in `table_trigger`. Raise the size `static_assert` bound from 16 to 24 bytes. |
| `tests/rackboy/sav/types_test.cpp` | `MPCBOY LOCAL MOD` (test) | Update `kExpectedBankSize` to the new `sizeof(RackboyBank)`. This and the `VoiceState` bound are the **only** allowed expected-value changes in vendored tests; both are recorded in `src/VENDORED.md`. |
| `src/engine/table_bank.{h,cpp}` | `new` | `TableBank { RackboyTable tables[16]; }`, `WaveBank { uint8_t waves[16][16]; }` (16 bytes = 32 packed 4-bit samples, the `rackboy_apu_write_wave_ram` format). `Content { TableBank tables; WaveBank waves; }`. |
| `src/engine/content_store.{h,cpp}` | `new` | The thread handoff (see "Concurrency"). `ContentStore` (`proposed`) owns: the audio thread's `live` `Content` and its `RackboyBank` view; one `staged` `Content` slot with `std::atomic<int> stagedState`; and a `published` `Content` copy guarded by a seqlock (`std::atomic<uint32_t> seq`, odd while writing) for readers on other threads. |
| `src/engine/bank_adapter.{h,cpp}` | `new` | Rebuilds the `RackboyBank` view from `live` after any change: tables 0–15 with `occupied = true`; waves 0–15 with `occupied = true`; slots 16+ unoccupied. Runs on the audio thread, no allocation (the `RackboyBank` is a member). |
| `src/engine/factory_content.{h,cpp}` | `new` | Default tables 1–8 and waves 1–8 (lists below). Slots 9–16 start empty. |
| `src/table/rackboy_table.{h,cpp}` | `MPCBOY LOCAL MOD` | Volume column, `P`, delayed `K` (below). |
| `tests/table/table_ext_test.cpp`, `tests/engine/content_store_test.cpp` | `new` | Tests below. |

Factory tables (original content). Transposes accumulate in the vendored interpreter, so values are deltas. One row is consumed per tick.

| Slot | Name | Rows |
|---|---|---|
| 1 | MajArp | row 0 `+0`, row 1 `+4`, row 2 `+3`, row 3 `−7` with `H01` (loop to row 1) |
| 2 | MinArp | `+0`, `+3`, `+4`, `−7 H01` |
| 3 | Octave | `+0`, `+12`, `−12 H01` |
| 4 | Vibrato | row 0 `V42`, row 1 `H01` |
| 5 | KickDrop | `+0`, then `−5` on rows 1–5, `K00` on row 6 (Pulse/Wave) |
| 6 | Fade | volume column `F0, E0, … 10, 00` on rows 0–15 (Pulse/Noise envelope byte, volume in the high nibble), `H0F` on row 15 (hold) |
| 7 | Rise | `P02` on row 0, `K10` on row 0's second command column (kill after 16 ticks) |
| 8 | Hat | Noise: volume column `F0` on row 0, `00` on row 2, `H02` on row 2 |

The implementer checks each by ear in WP10 and may adjust values. Names (≤ 8 characters) are shown on screen.

Wave bank defaults (original, 32 4-bit samples each): 1 Saw, 2 Square, 3 Triangle, 4 Sine, 5 Pulse25, 6 Pulse12, 7 HalfSine, 8 Organ (fundamental + octave + fifth, normalised to 0–15).

### Table-engine extensions (mpcboy-defined semantics)

LSDJ's own parameter layouts for these commands are unverified in the evidence, so the following are **mpcboy definitions**. WP11 does not claim LSDJ playback fidelity for them.

- **Volume column.** When a row has `has_vol`: Pulse/Noise write NRx2 = `vol` and retrigger with the current frequency (the `E` path). Wave writes `vol` through the existing CH3 `E` path to NR32.
- **`P` (pitch per tick).** Param is a signed byte in 1/16 semitone. It sets `pbend_step`; every tick from then on adds `pbend_step` to `pbend_accum` and rewrites the frequency without trigger. `P00` stops the motion and keeps the offset. A trigger resets both.
- **`K` with delay.** `Kxx` with `xx = 0` kills at once (rackboy behaviour). `xx > 0` sets `kill_ticks_remaining = xx`; the channel is killed on the tick where it reaches 0.
- **`W` retriggers.** After writing wave RAM, `W` retriggers CH3 with the current frequency (wave position restarts at sample 0). Without this the channel goes silent.
- **Slot numbering.** The screen shows tables and waves as 1–16. Everything inside the engine, the table data (`H`, `W` params) and the state format uses 0–15. `W02` therefore loads the wave shown as `3`.

### Concurrency and lifecycle

Rules (all paths through `ContentStore`). "Audio thread" means the thread that runs `process()`; the plugin marks it with a `thread_local bool` set at the top of every `process()` call.

1. **Audio thread owns `live`.** Only the audio thread reads or writes `live` and the `RackboyBank` view.
2. **Readers on other threads use `published`.** After every change to `live`, the audio thread copies it into `published` under the seqlock (increment `seq` to odd, `memcpy` ≈ 2 KB, increment to even, release order). `saveExtraState` and `formatValue` copy or read `published` and retry while `seq` is odd or changed. A save therefore contains every edit applied before it.
3. **Bulk content (state load, `.sav` import) uses `staged`.** `stagedState` has four values: `Empty`, `Writing`, `Ready`, `Reading`. A writer `compare_exchange`s from `Empty` **or** `Ready` to `Writing` (a newer load replaces one the audio thread has not consumed yet, which matters because the host may load state while `process` is not running), fills `staged`, then stores `Ready` (release). If the state is `Writing` or `Reading`, the writer `sched_yield()`s and retries; it runs on the UI or worker thread, and `Reading` lasts one ≈ 2 KB `memcpy`. The audio thread consumes at the start of each block: `compare_exchange(Ready → Reading)`, copy `staged` → `live`, rebuild the bank view, republish, store `Empty`.
4. **Editor cell edits are applied only on the audio thread.** The host contract delivers screen edits on the audio thread. `parameterChanged` for a cell parameter (WP7) writes `live` directly when the audio-thread marker is set and ignores the call otherwise (and counts it in a debug counter). Project and preset loads run on the UI thread, so their editor-parameter writes never touch tables, whether or not the extra state is valid. Editor parameters are not automatable, so no other legitimate path exists. G-EDIT item 1 verifies on the device that screen edits do arrive on the audio thread.
5. **`reset()` only requests.** `MpcBoyPlugin::reset()` sets `std::atomic<bool> resetPending_` and returns. At the start of the next `process()` call the audio thread exchanges it to false and runs `Engine::reset()` and the editor resync. This covers both the framework's audio-thread call after a state load and the dispatcher-thread call from `internalResume()`.
6. A table that changes while a note plays keeps its cursor and reads the new rows from the next tick.

### Tests and acceptance

1. Volume column: a Pulse 1 table with `vol = F0` on row 0 and `80` on row 1 lowers the measured RMS by 6 dB ± 1 dB after tick 1.
2. `P04` over 16 ticks raises pitch by 4 semitones ± 10 cents; `P00` then holds.
3. `K03` silences the channel on tick 3 exactly. `K00` silences at once.
4. `W02` from a Wave-channel table loads bank wave index 2 (bank adapter sets `occupied`): the spy sees the wave RAM write followed by a CH3 retrigger, and the Wave channel's RMS over the next 50 ms stays above −40 dBFS.
5. Content store under ThreadSanitizer (`native-tsan` preset, `new`): one thread stages bulk content repeatedly, a second thread reads `published` in a loop (as `saveExtraState` and `formatValue` do), the audio thread applies cell edits and renders 10,000 blocks. No race reports, and every snapshot read is internally consistent (a checksum written with each publish matches).
6. A cell write from a non-audio thread is ignored; the same write on the audio thread changes `live` and, after one block, `published`.
7. Two bulk loads in a row with no audio block between them: the second wins; the writer never blocks waiting for the audio thread.
8. Factory content: tables 1–8 and waves 1–8 are non-empty; waves contain only 0–15.

## WP6 — Tick clock synced to host BPM

### Goal and prerequisite state

Prerequisite: WP5 is complete. `tick_clock.*` and its unit tests (1–8) may start in parallel with WP5.

Goal: table ticks at sample-accurate positions inside each host block, derived from host tempo and position, as specified by D7.

### Repository evidence

- SDK `framework/mpc_plugin.h`: `struct Transport { bool valid, playing, recording, cycling, tempoValid, ppqValid; double bpm = 120.0, ppqPos, barStartPpq, cycleStartPpq, cycleEndPpq, samplePos; int timeSigNumerator, timeSigDenominator; }`, filled every block (`ctx.transport = fetchTransport()`).
- SDK `framework/dsp/tempo.h`: `safeBpm(bpm)` returns `bpm` when 1 < bpm < 999, else 120.
- SDK host contract: frames per call vary between 32 and 512; MIDI events carry `sampleOffset`, sorted (the framework sorts them and clamps offsets ≥ `numFrames` to the last frame).
- SDK `host_sim` fixes tempo at 120 BPM (`host_sim.cpp` line 198), so tempo behaviour needs mpcboy's own tests.
- Tick 0 is applied at note-on by WP4 step 2 (`table_tick` immediately after `table_trigger`).

### Change surface (all `new`)

| Item | Description |
|---|---|
| `src/engine/tick_clock.{h,cpp}` | `TickClock`. Per-block input `TickBlockInput { int frames; double bpm; bool tempoValid; double ppqPos; bool ppqValid; bool playing; }` (`proposed`). Output: up to `kMaxTicksPerBlock = 64` tick offsets in host frames, ascending. Settings: `mode {Sync, Free}`, `ticksPerBeat`, `phase {Note, Grid}`, `freeHz`. `noteOn(int hostFrameOffset)` restarts note phase (called only on trigger, not on glide). |
| `src/engine/engine.cpp` | `Engine::render` merges MIDI and tick offsets and renders APU segments between them. |
| `tests/engine/tick_clock_test.cpp`, `tests/engine/engine_timing_test.cpp` | Tests below. Plugin-level timing tests are in WP7. |

### Behaviour

Tick period in host frames (44.1 kHz): `Sync` → `44100 · 60 / (bpmUsed · ticksPerBeat)`; `Free` → `44100 / freeHz`.

`bpmUsed`: if `tempoValid` and 1 < bpm < 999, use `bpm` and remember it as `lastBpm`; otherwise `lastBpm`, which starts at 120.

`ticksPerBeat` choices and on-screen labels: `96/beat`, `48/beat`, `32/beat`, `24/beat LSDJ` (default), `16/beat`, `12/beat`, `8/beat`, `6/beat`, `4/beat`, `3/beat`. 24/beat equals LSDJ's tick at its default groove (6 ticks per 1/16 step).

Phase:

- **Note** (default). Tick 0 is the note-on itself (WP4). Tick k ≥ 1 fires at `noteOnFrame + k · period`. The clock stores the distance to the next tick as a `double` relative to the block start and subtracts `frames` each block; integer offset = `floor`. A tempo change rescales the remaining distance by `newPeriod / oldPeriod`, so the phase fraction is kept.
- **Grid.** Used only when `playing && ppqValid`; otherwise behaves as Note. Tick k fires where `ppq = k / ticksPerBeat`: frame `(k / ticksPerBeat − ppq0) · 60 / bpm · 44100` for a block starting at `ppq0`. Emit every k whose frame is in `[0, frames)`. If `ppq0` differs from the previous block's predicted end by more than one tick period (loop, locate, tempo jump), resynchronise to the next grid tick after `ppq0` without emitting the skipped ticks. A note-on still applies row 0 at once; row 1 waits for the next grid tick, which may be less than one period later (documented in README).
- **Free.** Like Note with the fixed period. Ignores transport.

Ticks reach the voice only while a note sounds, or after note-off in `Note Off = Ring` mode.

Rendering order inside one host block:

1. Collect MIDI events and tick offsets (host frames). Sort; at equal offsets MIDI comes first.
2. For each event at host offset `t` (relative to the block start): the event is applied at APU frame `apuBlockStart + round(t · 160 / 147)`, where `apuBlockStart` is the APU frame that corresponds to host frame 0 of the block. Render APU frames up to that position, apply the event, continue. There is no lookahead: the resampler's group delay (≈ 118 host frames) delays when the change is heard, not when it is written. After the last event, render the APU frames the resampler needs to emit the block's remaining host frames (`inputsNeeded`).
3. Apply: MIDI → `MonoVoice`; tick → `MonoVoice::tick(bank)`.
4. Render to the end of the block.

### Invariants

- At most 64 ticks per block. Worst case (bpm 999, 96/beat, 512 frames) is ≈ 18.6. If reached, the rest are dropped and counted in a debug counter.
- Tick positions come from the stored `double` phase, never from accumulated rounded periods.
- No allocation.

### Tests and acceptance

Domains: tests 1–8 check `TickClock` output in **host frames**. Test 9 checks `Engine` register writes, stamped with the APU frame and converted back to host frames by the test (not by the engine), so it measures the conversion error too.

1. **Constant tempo (S4):** bpm ∈ {40, 60, 120, 240, 300}, ticksPerBeat ∈ {24, 96}, block sizes drawn from {32, 64, 96, 128, 192, 256, 512} with a fixed seed, 10 minutes: tick k is within ±1 frame of `k · 44100 · 60 / (bpm · ticksPerBeat)` after the note-on.
2. **Tempo scaling (S5):** mean tick interval at 240 BPM is half that at 120 and a quarter of that at 60 (± 1 frame).
3. **Tempo change mid-note:** 120 → 60 BPM at frame F: intervals before and after match each period; no double or missing tick.
4. **Grid (S6):** with `playing`, `ppqValid` and exact ppq, every tick is within ±1 frame of `ppq = k / ticksPerBeat`.
5. **Loop jump (S6):** ppq jumps 7.99 → 0.0 in one block: at most one tick, at the grid position after 0.0.
6. **Invalid tempo:** `tempoValid = false` uses the last valid bpm; from cold start 120.
7. **Same-frame ordering:** a note-on and a tick at the same offset apply row 0 then row 1, in that order, once each.
8. **Free:** 59.73 Hz for 60 s gives 3584 ± 1 ticks.
9. **Engine end to end:** Octave table on Pulse 1, 120 BPM, 24/beat (one tick = 918.75 host frames). Row k's frequency write lands at host frame `round(k · 918.75)` ± 2 (±1 from the clock, ±1 from APU-frame conversion).
