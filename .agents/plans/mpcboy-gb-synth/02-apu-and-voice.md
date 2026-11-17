# 02 — APU rendering and the mono layered voice

Covers WP3 and WP4. Decisions D4–D6 are in [00-overview.md](00-overview.md#key-decisions).

## WP3 — Single-pass stereo APU, polyphase resampler, early CPU gate

### Goal and prerequisite state

Prerequisite: WP2 is complete and the vendored rackboy suites pass.

Goal: render all four channels in **one** APU pass to a stereo pair at 48 kHz, honouring NR50/NR51, and convert it to 44.1 kHz cheaply enough for the S8 budget, proven on the device before WP4.

### Repository evidence

- `src/apu/rackboy_apu.c` (from rackboy) `rackboy_sample_callback` stores only `sample->left / 32768.0f` into `_cb_buf`, because rackboy's 4-pass mode forces each channel to both sides.
- `rackboy_apu_run_samples` saves the whole `GB_gameboy_t` into `snapshot`, runs 4 muted passes, and restores band-limited state from `bl_save`. `rackboy_apu_init` sets the highpass filter off and interference to 0.
- `feed_samples(apu, n)` already drives `GB_apu_run` for exactly `n` output samples.
- `src/dsp/rackboy_resampler.{hpp,cpp}`: `kApuRateHz = 48000`; `HostResampler` is a 257-tap Kaiser-windowed sinc (β = 10). Its header says `emit` "DOES call `std::sin` once per tap (257 x per output sample) on purpose" (rackboy D-012), using double-precision `sinc_pi`. At 44.1 kHz that is about 11.3 million `sin` calls per second per instance, too slow for a 5 % budget on Cortex-A55/A17.
- 48000 / 44100 = 160 / 147, so a polyphase bank needs 147 phases.
- SDK host contract: 44.1 kHz, budget "a few percent of one core per instance" (`docs/overview.md`); a 128-frame block lasts 2,902 µs, so 5 % is 145 µs.

### Change surface

| Item | Status | Description |
|---|---|---|
| `rackboy_apu_run_stereo(rackboy_apu_t*, uint32_t n, float* left, float* right)` in `src/apu/rackboy_apu.{h,c}` | `proposed`, `MPCBOY LOCAL MOD` | Renders `n` frames in one pass: no channel muting, no snapshot, no `bl_save` swap. A second callback (`proposed`: `mpcboy_stereo_callback`) writes `left/32768` and `right/32768`. It swaps the callback in and restores rackboy's callback afterwards, so `rackboy_apu_run_samples` still works for tests. State carries over between calls (no restore). |
| `src/dsp/polyphase_resampler.{h,cpp}` | `new` | `PolyphaseResampler48To441`: 147 phases × 257 taps of `float`, coefficients computed once with the same Kaiser-windowed sinc formula as `rackboy_resampler.cpp`, stored in a process-wide `static` table built under `std::call_once` from the plugin constructor (≈ 151 KB once per process, never on the audio thread). Stereo only. API (`proposed`): `reset()`, `inputsNeeded(int outFrames)`, `process(const float* inL, const float* inR, int inFrames, float* outL, float* outR, int outFrames)` with an internal 256-frame history ring. |
| `src/dsp/dc_blocker.h` | `new` | Stereo one-pole DC blocker, α = 0.995, matching `taps_process` in `src/core/rackboy_taps.cpp`, with denormal flushing. |
| `tests/apu/stereo_pass_test.cpp`, `tests/dsp/polyphase_test.cpp` | `new` | Tests below. |
| `bench/bench_main.cpp`, `docs/BENCH.md` | `new` | `mpcboy_bench` target from WP1 and recorded numbers. |
| `src/plugin/MpcBoyPlugin.cpp` (debug define `MPCBOY_CPU_PROBE` only) | edit of the WP1 placeholder | **CPU spike plugin:** holds a note, runs APU single pass + resampler for all 4 channels, measures µs per `process` call with `clock_gettime(CLOCK_MONOTONIC)`, and shows mean/p99 as the display text of a read-only parameter `dbg_us` (test 7 says how). Compiled out of normal builds. |

`HostResampler` (rackboy) stays vendored as the reference implementation for tests only.

### Behaviour and invariants

- `rackboy_apu_run_stereo` allocates nothing and returns exactly `n` frames per side.
- Output scale: SameBoy stereo samples divided by 32768. The engine writes NR52 = `0x80`, NR50 = `0x77` at init; channel balance comes from NR51 only.
- The APU always runs at 48 kHz. The host rate is 44.1 kHz on the MPC. Any other host rate makes the plugin output silence (WP7).
- Resampler latency is half the kernel: 128 input frames ≈ 117.6 output frames. The plugin reports `latencySamples = 0` and documents the ≈ 2.7 ms delay; WP10 measures the real onset offset on the device and decides whether to report it (see WP10 row 11).

### Tests and acceptance

1. **Equivalence with ground truth (R1).** `pattern.csv` is a Gate 1/Gate 2 pattern, not a register script, and the golden `.bin` files are per-channel output after `taps_process` (DC blocker), rendered one sample per `rackboy_apu_run_samples` call (`regression.cpp`). Harness (`new`, in `tests/apu/stereo_pass_test.cpp`): (a) replay `pattern.csv` exactly as `regression.cpp` does, with a register-write spy that records `(sample index, addr, value)`; (b) in a fresh APU, replay that write log into `rackboy_apu_run_stereo` (writes applied before the sample they were recorded at, NR51 = `0xFF`, NR50 = `0x77`); (c) apply a stereo DC blocker with the same α as `taps_process`; (d) compare `left` and `right` with the sum of the four golden buffers, scale fixed at 1.0, within −60 dBFS RMS. Do not fit a scale factor. If it fails, find out why before WP4; do not loosen the threshold. The test fails, not skips, when a golden file is missing.
2. **Continuity:** rendering 1 s as one call equals rendering it as 344 calls of varying size (32–512), sample for sample.
3. **Panning:** NR51 = `0x10` (Pulse 1 left only) gives `right` below −90 dBFS and `left` above −30 dBFS.
4. **Resampler equivalence:** for white noise and a 1 kHz square, `PolyphaseResampler48To441` output equals the vendored `HostResampler` output within −100 dBFS RMS after both are warmed up.
5. **Resampler spur:** a 1 kHz Pulse 1 square at 48 kHz resampled to 44.1 kHz has no spur above −70 dBFS outside the harmonics of 1 kHz (use the copied `measurement.hpp` and `fft.h`).
6. **Bench (native gate):** `mpcboy_bench` reports p99 µs per 128-frame host block for APU single pass + resampler with all 4 channels sounding. Pass bar on the developer Mac: p99 ≤ 25 µs (about 6× headroom against the 145 µs device budget). Record numbers in `docs/BENCH.md`.
7. **G-CPU (device gate, needs the user's Live III):** install the debug CPU spike plugin (packaged as in WP1). Hold a note at a 128-frame buffer for 60 s. Pass: mean ≤ 145 µs and p99 ≤ 290 µs per block (one rule, used by S8 and WP10 row 9 too; no max bar, because scheduler jitter alone can exceed one block). Reading the number: the `process()` path stores per-block times in a fixed ring; a debug-only worker thread computes mean/p99 once per second, stores them for `formatValue` of `dbg_us`, and calls `requestDisplayUpdate()` from that worker (never from `process()`). Record in `docs/BENCH.md`. If it fails, profile and fix before WP4 (candidates in order: SameBoy per-sample feeding in `feed_samples`, then resampler taps reduced from 257 to 129 with test 5 re-run). If the user cannot run the device now, record "pending"; G-CPU then becomes a release blocker checked in WP10 row 9.

## WP4 — Mono voice and the four layered channels

### Goal and prerequisite state

Prerequisite: WP3 is complete.

Goal: a host-agnostic engine that turns MIDI notes into register writes for up to four channels at once, as one monophonic voice.

### Repository evidence

- `src/table/rackboy_table.h`: `table_trigger(VoiceState&, TableRegState&, VoiceChannel, const RackboyInstrument&, int16_t semitones_from_base, rackboy_apu_t*)` resets `table_cursor` to 0, sets `regs.pitch_offset = semitones_from_base` and writes the instrument's initial registers with the trigger bit. It does **not** apply table row 0. `table_tick(...)` reads `table.rows[voice.table_cursor]`, then advances the cursor (`rackboy_table.cpp` lines 718–723), so the first `table_tick` applies row 0.
- `table_trigger` hard-codes length: NR11/NR21 low bits `0x3F`, NR31 = `0xFF`, NR41 = `0x3F`, and never sets NRx4 bit 6 (`rackboy_table.cpp` around lines 563–680). The wave trigger writes NR30 = `0x80`.
- Noise in `table_trigger`: if `semitones_from_base == 0`, NR43 = `inst.raw[4]`; otherwise `pitch_noise_nr43(semitones_from_base)` (around line 676).
- `RackboyInstrument { uint8_t raw[16]; }`. Pulse: `[1]` envelope (NRx2), `[3]` length (bit 6 = unlimited), `[4]` sweep (NR10), `[5]` bit 4 table enable, `[6]` table slot, `[7]` duty bits 6–7 / finetune bits 2–5 / pan bits 0–1. Wave: `[1]` volume, `[6]` table, `[8]` wave index. Noise: `[1]` envelope, `[4]` NR43 shape.
- NR32 output level, bits 6–5: `00` mute, `01` 100 %, `10` 50 %, `11` 25 %.
- `src/core/rackboy_pitch.h`: `kBaseMidiNote = 72`; `pitch_square_11bit`, `pitch_wave_11bit` (both clamp to the 11-bit range), `pitch_noise_nr43` take whole semitones from the base. `rackboy_table.cpp` has 8 real call sites of these functions: 4 in `table_trigger` (one per channel, around lines 555, 591, 623, 678) and 4 in the tick path (around lines 853, 859, 865, 880). `L` slide moves `regs.pitch_offset` itself (using `VoiceState.slide_target_pitch` and `slide_ticks_remaining`), and vibrato is folded into one `effective_pitch` expression as an integer semitone `vibrato_offset`. NRx4 writes go through `write_freq_no_trigger` (freq high bits only) and `trigger_nrx4_byte` (freq high bits + trigger); neither carries the length-enable bit 6.

### Change surface (all `new` unless marked)

| Item | Description |
|---|---|
| `src/engine/channel_settings.h` | `ChannelSettings` POD per channel: `enabled`, `volume 0–15`, `envUp`, `envPeriod 0–7`, `duty 0–3`, `transpose −24..+24`, `fineCents −50..+50`, `pan {Off, L, C, R}`, `length 0 (off) or 1–64`, `sweepTime 0–7`, `sweepDown`, `sweepShift 0–7` (Pulse 1), `waveLevel {0, 25, 50, 100}`, `waveSlot 0–15` (Wave), `noiseWidth7`, `noisePitchMode {Track, Fixed}`, `noiseFixedShape 0–111` (Noise), `tableSlot −1 (off) or 0–15`. |
| `src/engine/instrument_encoder.{h,cpp}` | `encode_instrument(VoiceChannel, const ChannelSettings&) -> RackboyInstrument` in the LSDJ byte layout above. Wave level maps 0 → `00`, 100 → `01`, 50 → `10`, 25 → `11` (NR32 codes, not monotonic). Noise `raw[4]`: Track → `pitch_noise_nr43(0)` with bit 3 = width; Fixed → `noiseFixedShape`'s NR43 byte with bit 3 = width. |
| `src/engine/note_stack.h` | Fixed array of 16 held notes. Last-note priority. No allocation. |
| `src/engine/mono_voice.{h,cpp}` | `MonoVoice`: note stack, 4 `VoiceState` + `TableRegState` pairs, bend, the `rackboy_apu_t*`. Methods (`proposed`): `noteOn(note, vel)`, `noteOff(note)`, `allNotesOff()`, `pitchBend(int14)`, `tick(const RackboyBank&)`, `applySettings(const EngineSettings&)`, `reset()`. |
| `src/engine/engine.{h,cpp}` | `Engine`: owns the APU, `MonoVoice`, `TickClock` (WP6), `PolyphaseResampler48To441`, `DcBlocker`, and fixed scratch buffers (APU 48 kHz: 640 frames per side, enough for 512 host frames plus history). `render(const EngineBlock&, float* L, float* R, int frames)`. `EngineBlock` (`proposed`) carries MIDI events with host-frame offsets and transport fields. Constructed once; `reset()` defined below. |
| `src/core/rackboy_pitch.{h,cpp}` (`MPCBOY LOCAL MOD`) | Add `pitch_square_11bit_cents(int32_t cents_from_base)` and `pitch_wave_11bit_cents(int32_t)`, rounding to the nearest 11-bit register value. Existing semitone functions stay. |
| `src/table/rackboy_table.{h,cpp}` (`MPCBOY LOCAL MOD`) | See "Local changes to the table interpreter" below. |

### Local changes to the table interpreter (WP4 part)

1. **One pitch helper.** Add `static void write_channel_pitch(...)` (`proposed`) and route the 8 pitch call sites through it (list them with line numbers in `src/VENDORED.md`). Composition, in cents: `total_cents = (pitch_offset + vibrato_offset) · 100 + pbend_accum · 100 / 16 (WP5) + fine_cents + bend_cents`. `pitch_offset` already includes accumulated transposes and the `L` slide. At trigger time `pitch_offset = semitones_from_base` and the other terms are 0 except `fine_cents` and `bend_cents`.
2. **New `TableRegState` fields:** `int16_t bend_cents`, `int8_t fine_cents`, `uint8_t noise_flags` (bit 0 fixed, bit 1 width7), `uint8_t length_enable`. `table_reg_state_reset` zeroes them; `table_trigger` sets `fine_cents` and `noise_flags` from the instrument encoder's companion data (pass them in a new `TriggerExtras` struct, `proposed`, to keep `RackboyInstrument` 16 bytes). Update the `static_assert`s if they check size; they must stay trivially copyable and standard-layout.
3. **Length.** Length comes from `TriggerExtras.length` (0 or 1–64), never from `raw[3]` (which keeps rackboy's LSDJ meaning, so the vendored golden tests are unaffected). A new `TableRegState` field `uint8_t length_enable` is set when length > 0. `table_trigger` then writes NR11/NR21/NR41 low 6 bits = `64 − length` and NR31 = `256 − 4·length` (clamped to 0–255). `write_freq_no_trigger` and `trigger_nrx4_byte` OR bit 6 into **every** NRx4 write while `length_enable` is set, so table ticks, bends, glides and `E`/`R` retriggers keep the length running. Units: Pulse and Noise 1/256 s per step (max 0.25 s); Wave 4/256 s per step (max 1 s). README states both. When length is 0, today's behaviour (max length, bit 6 clear).
4. **Noise.** Every NR43 write ORs width into bit 3. When `noise_flags.fixed` is set, table transposes, slides, vibrato and bend never change NR43; only `N` commands do. In Track mode NR43 always comes from `pitch_noise_nr43(total_semitones)` (no special case for 0, because `raw[4]` already equals `pitch_noise_nr43(0)`).

### Behaviour

Engine init (constructor) and `reset()`:

1. `rackboy_apu_init(apu, 48000)`; NR52 = `0x80`; NR50 = `0x77`; NR51 = `0x00` (silent until a note); wave RAM = wave slot 0.
2. Clear the note stack, voices, bend; reset the tick clock; reset the resampler history and DC blocker.
3. `Engine::reset()` runs only on the audio thread, at the start of `process()` when the plugin's `resetPending_` flag is set (WP5 rule 5, WP7). The SDK may call `MpcBoyPlugin::reset()` from the dispatcher thread, so that call only sets the flag.

Note-on (velocity > 0):

1. Push the note. If no note was sounding, or `Retrigger = Always`, **trigger** (step 2). If a note was sounding and `Retrigger = Legato`, **glide** (step 5).
2. Trigger, for each channel with `enabled = true`:
   - Wave only: write wave slot `waveSlot` with `rackboy_apu_write_wave_ram` before the trigger.
   - Call `table_trigger` with the encoded instrument, `semitones_from_base = note − kBaseMidiNote + transpose`, and the extras.
   - If `Vel>Vol` is on, scale the initial envelope volume by `vel / 127`, rounded, minimum 1.
   - If the channel has a table, call `table_tick` once immediately (this is tick 0 and applies row 0).
3. Write NR51 from every channel's `pan` (`Off` or disabled clears both bits).
4. Restart the tick clock's note phase at this frame (WP6).
5. Glide (legato): for each sounding channel, add `newNote − oldNote` to `pitch_offset` and to `slide_target_pitch`, keep `table_cursor`, accumulated transposes and `pbend_accum`, and rewrite the frequency without the trigger bit. The tick clock phase is not reset.

Note-off:

- Remove the note. If other notes are held, glide to the new top note.
- If none is held and `Note Off = Cut`: NRx2 = `0x00` for Pulse 1, Pulse 2, Noise and NR30 = `0x00` for Wave (DACs off). Tables stop ticking.
- If `Note Off = Ring`: change nothing. Tables keep ticking until the next note or until the hardware envelope or length ends the sound.

Other MIDI:

- Pitch bend: `bend_cents = bend / 8192 · bendRange · 100`, applied to every sounding channel at once through `write_channel_pitch` (no trigger bit).
- CC 120 / CC 123: `allNotesOff()`, which acts like a final note-off in Cut mode.
- Everything else is ignored.

Settings changes while a note sounds apply at the next trigger, except `pan` (NR51 at once) and `enabled = false` (the channel's DAC is turned off at once).

Usable note range: Pulse and Wave clamp at the 11-bit register limits (`pitch_square_11bit` lowest ≈ 64 Hz, around MIDI 36; `pitch_wave_11bit` one octave lower). Notes outside the range clamp to the nearest playable pitch; README states the range.

### Invariants

- At most one note sounds. A 17th held note replaces the oldest entry.
- Trigger bits are set only by note-on step 2 and by the table interpreter's retriggering paths (`E`, `R`, and WP5's volume column).
- `Engine::render` allocates nothing and takes no lock.

### Tests and acceptance

1. Note-on with all 4 channels enabled writes trigger bits to NR14, NR24, NR34 and NR44 at the same host frame (register-write spy: a test-only hook recording `rackboy_apu_write_reg` calls with the APU frame index).
2. Legato: Major arp (factory table 1) on Pulse 1, note 60, glide to note 62 after row 2. The frequency after the glide equals note 62 + the accumulated table transposes (spy), no trigger bit is written, and the next row continues from the same cursor. `Always`: trigger bits are written again and the table restarts.
3. Each channel alone (other three off): 0.5 s render RMS above −40 dBFS. All four off: below −90 dBFS.
4. Four channels, four different tables, one note: the spy shows each channel's frequency sequence following its own table.
5. `Note Off = Cut`: output below −90 dBFS within 10 ms (plus the resampler delay) after the last note-off.
6. `pitch_square_11bit_cents(n · 100) == pitch_square_11bit(n)` for n in −36..+36; same for wave.
7. Bend +8191 with range 2 raises the Pulse 1 fundamental by 200 ± 10 cents (zero-crossing count over 0.5 s).
8. Bend and fine combined with `V` vibrato, `L` slide (and `P` after WP5): the pitch equals the documented composition within ±5 cents at sampled frames.
9. Length: `P1 Length = 16` silences Pulse 1 after `16/256 s` = 62.5 ms ± 5 ms in three cases: no table; Octave table running (ticks rewrite NRx4); a held pitch bend. `WV Length = 16` silences Wave after 250 ms ± 10 ms.
10. Noise Track: NR43 written for notes 60–84 moves monotonically in noise frequency and always carries the width bit. Noise Fixed: a table with transposes never changes NR43.
11. Panning: Pulse 1 alone with pan `L` gives the right channel below −90 dBFS.
12. All copied rackboy suites (including `vibrato_test`, `pitch_slide_test` and the golden audio regression) still pass after the local changes with no behavioural expected-value change. The only allowed vendored-test edits are the two size constants listed in WP5.
