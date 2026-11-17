// rackboy_table/rackboy_table.cpp
//
// Implementation of the per-voice LSDJ table state machine.
// See rackboy_table.h for the public API and full design commentary.
//
// Tracked by bead rackboy-tj3. Architecture Decisions #9 and #10 from
// docs/prompts/rackboy-lsdj-quad-voice-module.md govern the trigger and
// tick semantics; see the header for the precise AD text.
//
// No allocations, no STL containers, no virtual methods. All paths are
// safe to call from the audio thread.

#include "rackboy_table.h"

#include <cstdio>                 // std::snprintf for format_chord_reference

// Include the full APU header here (not in the .h) to avoid pulling the
// entire SameBoy vendor tree into every translation unit that uses the
// table interpreter.
#include "rackboy_apu.h"          // -Irackboy_apu

// Pitch helpers: pitch_square_11bit (CH1/CH2), pitch_wave_11bit (CH3),
// pitch_noise_nr43 (CH4).
#include "rackboy_pitch.h"        // -Irackboy_core

namespace rackboy {
namespace table {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// write_freq_no_trigger — update NRx3/NRx4 without setting the trigger bit
//
// Used by table_tick() for pitch-glide and table-driven frequency updates.
// Deliberately omits bit 7 of NRx4 so the volume envelope is NOT reset on
// every table row advance — only table_trigger() sets the trigger bit.
//
// For CH1/CH2/CH3 the `freq` parameter is the 11-bit register value:
//   NRx3 ← freq & 0xFF           (low 8 bits)
//   NRx4 ← (freq >> 8) & 0x07   (high 3 bits, no trigger bit 7)
//
// For CH4 the `freq` parameter is the NR43 byte and `addr_lo` / `addr_hi`
// are NR43 / 0 (we only write one register, the second write is suppressed
// by passing addr_hi == 0).
//
// MPCBOY LOCAL MOD: `length_enable` ORs NRx4 bit 6 into the high write so a
// running length counter keeps running across table ticks (write_channel_pitch
// is the only caller).
static void write_freq_no_trigger(
    rackboy_apu_t* apu,
    uint16_t freq,
    uint16_t addr_lo,
    uint16_t addr_hi,
    uint8_t length_enable = 0) noexcept
{
    rackboy_apu_write_reg(apu, addr_lo, static_cast<uint8_t>(freq & 0xFF));
    if (addr_hi != 0) {
        rackboy_apu_write_reg(
            apu, addr_hi,
            static_cast<uint8_t>(((freq >> 8) & kFreqHiMask) |
                                 (length_enable ? kLengthEnableBit : 0)));
    }
}

// trigger_nrx4_byte — pack the NRx4 retrigger write with preserved freq hi
//
// Returns the byte to write into NRx4 (x = 1, 2, or 3) to retrigger the
// channel while preserving its current 11-bit frequency: bit 7 set
// (kTriggerBit) | bits 2-0 = freq_11bit >> 8 & kFreqHiMask. Callers
// for CH4 use kTriggerBit directly since NR44 has no freq bits.
//
// Shared by the `E` command and the eventual `R` retrigger command.
//
// MPCBOY LOCAL MOD: `length_enable` ORs NRx4 bit 6 (see write_freq_no_trigger).
static inline uint8_t trigger_nrx4_byte(uint16_t freq_11bit,
                                        uint8_t length_enable = 0) noexcept
{
    return static_cast<uint8_t>(
        kTriggerBit | ((freq_11bit >> 8) & kFreqHiMask) |
        (length_enable ? kLengthEnableBit : 0));
}

// vibrato_triangle — signed triangle-wave shaper for the `V` command
//
// Maps a uint8_t phase accumulator (period = 256 ticks) onto a
// symmetric triangle wave in the int16_t range ~[-128, +128]. Four
// quadrants:
//
//   phase in [0,   64):  ramp up from 0 to ~+128  (slope +2)
//   phase in [64,  128): ramp down from ~+128 to 0
//   phase in [128, 192): ramp down from 0 to ~-128
//   phase in [192, 256): ramp up from ~-128 to 0
//
// Used by dispatch_command's `V` handler and the table_tick pitch
// step to compute an ephemeral semitone offset on top of
// `regs.pitch_offset` without persisting anything. The triangle
// shape was chosen over sine to avoid a lookup table — the
// stepped-vibrato character is consistent with how LSDJ itself
// has historically sounded on real hardware.
//
// Cycle period is determined by `vibrato_speed` (the phase
// increment per tick) — the phase wraps every 256 ticks at speed=1
// and approximately every 17 ticks at speed=15:
//
//   speed=1   → 256 Gate 2 ticks per full cycle
//   speed=2   → 128 ticks per cycle
//   speed=4   →  64 ticks per cycle
//   speed=8   →  32 ticks per cycle
//   speed=15  → ~17 ticks per cycle
//
// The audible LFO frequency (in Hz) is therefore
// (speed / 256) × Gate2_rate. Gate 2's rate is host-driven (either
// the VCV modulated gate input or a Daisy GPIO edge detector), so
// this table is intentionally rate-agnostic.
static inline int16_t vibrato_triangle(uint8_t phase) noexcept
{
    if (phase < 64) {
        return static_cast<int16_t>(phase * 2);               // 0..126
    } else if (phase < 128) {
        return static_cast<int16_t>((128 - phase) * 2);       // 128..2
    } else if (phase < 192) {
        return static_cast<int16_t>(-(static_cast<int>(phase) - 128) * 2);  // 0..-126
    } else {
        return static_cast<int16_t>(-(256 - static_cast<int>(phase)) * 2);  // -128..-2
    }
}

// vibrato_offset — scale the triangle by `depth` to a semitone offset
//
// depth is the low nibble of the `V` parameter (0..15). The triangle
// is in roughly [-128, +128]; we multiply by depth and divide by 128
// so the final offset range is roughly [-depth, +depth] semitones.
// depth == 0 short-circuits to zero so a `Vx0` (non-zero speed, zero
// depth) produces no audible modulation even while the phase
// accumulator continues to advance — useful for muting vibrato
// without losing phase continuity.
static inline int16_t vibrato_offset(uint8_t phase, uint8_t depth) noexcept
{
    if (depth == 0) return 0;
    const int32_t tri = vibrato_triangle(phase);
    return static_cast<int16_t>((tri * static_cast<int32_t>(depth)) / 128);
}

// ---------------------------------------------------------------------------
// MPCBOY LOCAL MOD: write_channel_pitch — the single pitch path
// ---------------------------------------------------------------------------
//
// Every pitch computation of the interpreter goes through here (the 8 former
// call sites of pitch_square_11bit / pitch_wave_11bit / pitch_noise_nr43: 4 in
// table_trigger, 4 in table_tick; see src/VENDORED.md).
//
// Composition, in cents:
//   total = (pitch_offset + vibrato) * 100
//         + pbend_accum * 100 / 16      (WP5 `P` command, 1/16 semitone units)
//         + fine_cents + bend_cents
// pitch_offset already holds the accumulated transposes and the `L` slide. With
// fine = bend = 0 the cents are a multiple of 100 and the register values are
// bit-identical to the old semitone functions.
//
// `trigger` writes the trigger bit with the frequency (table_trigger); without
// it only pitch bits are written. `engine_mode` (table_trigger called with
// TriggerExtras) selects the new noise rule; nullptr extras keep the legacy
// "semitones == 0 uses raw[4]" rule so the vendored tests are unaffected.

static inline int16_t cents_to_nearest_semitone(int32_t cents) noexcept
{
    const int32_t c = cents + 50;
    int32_t semis = (c >= 0) ? (c / 100) : -((-c + 99) / 100);
    if (semis > 32767) semis = 32767;
    if (semis < -32768) semis = -32768;
    return static_cast<int16_t>(semis);
}

static inline uint8_t noise_width_bit(const TableRegState& regs) noexcept
{
    return (regs.noise_flags & kNoiseWidth7) ? 0x08u
                                              : static_cast<uint8_t>(regs.nr43_shape & 0x08u);
}

static void write_channel_pitch(
    rackboy_apu_t* apu,
    rackboy::core::VoiceChannel ch,
    TableRegState& regs,
    int16_t vibrato_semis,
    int16_t pbend_accum,
    bool trigger,
    bool engine_mode) noexcept
{
    const int32_t cents =
        (static_cast<int32_t>(regs.pitch_offset) + vibrato_semis) * 100 +
        static_cast<int32_t>(pbend_accum) * 100 / 16 +
        static_cast<int32_t>(regs.fine_cents) +
        static_cast<int32_t>(regs.bend_cents);
    const uint8_t le = regs.length_enable;

    switch (ch) {
    case rackboy::core::VoiceChannel::Pulse1:
        regs.freq_11bit = rackboy::core::pitch_square_11bit_cents(cents);
        if (trigger) {
            rackboy_apu_write_reg(apu, kNR13, static_cast<uint8_t>(regs.freq_11bit & 0xFF));
            rackboy_apu_write_reg(apu, kNR14, trigger_nrx4_byte(regs.freq_11bit, le));
        } else {
            write_freq_no_trigger(apu, regs.freq_11bit, kNR13, kNR14, le);
        }
        break;
    case rackboy::core::VoiceChannel::Pulse2:
        regs.freq_11bit = rackboy::core::pitch_square_11bit_cents(cents);
        if (trigger) {
            rackboy_apu_write_reg(apu, kNR23, static_cast<uint8_t>(regs.freq_11bit & 0xFF));
            rackboy_apu_write_reg(apu, kNR24, trigger_nrx4_byte(regs.freq_11bit, le));
        } else {
            write_freq_no_trigger(apu, regs.freq_11bit, kNR23, kNR24, le);
        }
        break;
    case rackboy::core::VoiceChannel::Wave:
        regs.freq_11bit = rackboy::core::pitch_wave_11bit_cents(cents);
        if (trigger) {
            rackboy_apu_write_reg(apu, kNR33, static_cast<uint8_t>(regs.freq_11bit & 0xFF));
            rackboy_apu_write_reg(apu, kNR34, trigger_nrx4_byte(regs.freq_11bit, le));
        } else {
            write_freq_no_trigger(apu, regs.freq_11bit, kNR33, kNR34, le);
        }
        break;
    case rackboy::core::VoiceChannel::Noise:
    {
        const bool fixed = (regs.noise_flags & kNoiseFixed) != 0;
        uint8_t nr43;
        if (fixed) {
            // Fixed noise: only table_trigger and `N` write NR43.
            if (!trigger) break;
            nr43 = static_cast<uint8_t>(regs.nr43_shape |
                                        ((regs.noise_flags & kNoiseWidth7) ? 0x08u : 0u));
        } else {
            const int16_t semis = cents_to_nearest_semitone(cents);
            if (trigger && !engine_mode) {
                // Legacy trigger rule (vendored behaviour): the instrument's
                // raw[4] is authoritative at zero offset.
                nr43 = (semis == 0) ? regs.nr43_shape
                                    : rackboy::core::pitch_noise_nr43(semis);
            } else {
                // Track mode: always derived from the pitch; width merged in.
                nr43 = static_cast<uint8_t>(
                    (rackboy::core::pitch_noise_nr43(semis) & ~0x08u) |
                    noise_width_bit(regs));
            }
        }
        rackboy_apu_write_reg(apu, kNR43, nr43);
        if (trigger) {
            rackboy_apu_write_reg(
                apu, kNR44,
                static_cast<uint8_t>(kTriggerBit | (le ? kLengthEnableBit : 0)));
        }
        break;
    }
    }
}

// MPCBOY LOCAL MOD: kill_channel — the `K` effect (also the delayed `K` firing).
static void kill_channel(
    rackboy::core::VoiceState& voice,
    TableRegState& regs,
    rackboy::core::VoiceChannel ch,
    rackboy_apu_t* apu) noexcept
{
    switch (ch) {
    case rackboy::core::VoiceChannel::Pulse1:
        regs.envelope = 0;
        rackboy_apu_write_reg(apu, kNR12, 0x00);
        break;
    case rackboy::core::VoiceChannel::Pulse2:
        regs.envelope = 0;
        rackboy_apu_write_reg(apu, kNR22, 0x00);
        break;
    case rackboy::core::VoiceChannel::Wave:
        regs.wave_volume = 0;
        // CH3: turn off the DAC by clearing NR30 bit 7.
        rackboy_apu_write_reg(apu, kNR30, 0x00);
        break;
    case rackboy::core::VoiceChannel::Noise:
        regs.envelope = 0;
        rackboy_apu_write_reg(apu, kNR42, 0x00);
        break;
    }
    voice.triggered = false;
    voice.kill_ticks_remaining = 0;
}

// MPCBOY LOCAL MOD: write an envelope byte and retrigger with the running
// frequency. This is the `E` command's effect and the table volume column's
// (Pulse/Noise: NRx2 = value; Wave: NR32 = value & 0x60, which restarts the
// wave position).
static void write_envelope_retrigger(
    uint8_t value,
    TableRegState& regs,
    rackboy::core::VoiceChannel ch,
    rackboy_apu_t* apu) noexcept
{
    switch (ch) {
    case rackboy::core::VoiceChannel::Pulse1:
        regs.envelope = value;
        rackboy_apu_write_reg(apu, kNR12, value);
        rackboy_apu_write_reg(apu, kNR14,
                              trigger_nrx4_byte(regs.freq_11bit, regs.length_enable));
        break;
    case rackboy::core::VoiceChannel::Pulse2:
        regs.envelope = value;
        rackboy_apu_write_reg(apu, kNR22, value);
        rackboy_apu_write_reg(apu, kNR24,
                              trigger_nrx4_byte(regs.freq_11bit, regs.length_enable));
        break;
    case rackboy::core::VoiceChannel::Wave:
        regs.wave_volume = value;
        rackboy_apu_write_reg(apu, kNR32,
                              static_cast<uint8_t>(value & kNR32OutputLevelMask));
        rackboy_apu_write_reg(apu, kNR34,
                              trigger_nrx4_byte(regs.freq_11bit, regs.length_enable));
        break;
    case rackboy::core::VoiceChannel::Noise:
        regs.envelope = value;
        rackboy_apu_write_reg(apu, kNR42, value);
        rackboy_apu_write_reg(apu, kNR44,
                              static_cast<uint8_t>(kTriggerBit | (regs.length_enable ? kLengthEnableBit : 0)));
        break;
    }
}

// dispatch_command — stub for LSDJ table command letters
//
// Each LSDJ table row carries two command slots (cmd, param). The individual
// command handlers are each tracked by their own bead and will be wired in
// incrementally. Until then, every command is a no-op routed through this
// stub so the tick loop's structure is already in the right shape for future
// extension.
//
// cmd_byte values (LSDJ 9.4.0, docs/LSDJ_REFERENCE.md):
//   0x00 = no command (always-no-op; filtered by caller before reaching here)
//   'A' (0x41) = rackboy-TODO: amplitude / volume envelope command
//   'C' (0x43) = chord command — deferred to v1.1 per D-001; runtime no-op
//                here, per-instrument load-time warning emitted by
//                `scan_chord_references` + `format_chord_reference`
//                (rackboy-u4j, closed wont-fix in iteration 88).
//   'D' (0x44) = delay command (sets table_delay_ticks_remaining) [DONE rackboy-jgb]
//   'E' (0x45) = envelope retrigger command [DONE rackboy-qz3]
//   'F' (0x46) = rackboy-TODO: frame command (wave frame select, CH3)
//   'G' (0x47) = global no-op: groove / speed command (rackboy does
//                not run LSDJ phrases; Gate 2 replaces grooves)
//                [DONE rackboy-ydc]
//   'H' (0x48) = hop command (alters cursor position) [DONE rackboy-oks]
//   'K' (0x4B) = kill / note-off command [DONE rackboy-pr1]
//   'L' (0x4C) = rackboy-TODO: slide command
//   'M' (0x4D) = global no-op: master volume command (host mixer
//                owns final level; per-channel outputs are pre-mixer)
//                [DONE rackboy-ydc]
//   'N' (0x4E) = noise shape command (CH4 only) [DONE rackboy-3rj]
//   'O' (0x4F) = global no-op: output routing command (host owns
//                channel-to-bus routing; NR51 panning unmodeled)
//                [DONE rackboy-ydc]
//   'P' (0x50) = rackboy-TODO: pitch bend command (per-voice)
//   'Q' (0x51) = rackboy-TODO: not in docs/LSDJ_REFERENCE.md's v1
//                per-voice / channel-specific / global / deferred
//                tables — the label needs cross-check against the
//                LSDJ 9.4.0 manual or the extractor's
//                `command_to_bytes()` before a future bead assumes a
//                specific semantic ("random note" was the prior
//                unsourced guess, kept here as a TODO anchor).
//   'R' (0x52) = retrigger command (fires NRx4 bit 7 every N ticks)
//                [DONE rackboy-bgu]
//   'S' (0x53) = sweep command (CH1 only) [DONE rackboy-5yc]
//   'T' (0x54) = global no-op: tempo command (rackboy's clock comes
//                from Gate 2, not an internal BPM counter)
//                [DONE rackboy-ydc]
//   'V' (0x56) = vibrato command (triangle-LFO pitch offset)
//                [DONE rackboy-00h]
//   'W' (0x57) = wave-set command (CH3, writes wave RAM) [DONE rackboy-8tz]
//   'Z' (0x5A) = rackboy-TODO: random command
//
// H (hop) writes back to voice.table_cursor; D (delay) writes
// voice.table_delay_ticks_remaining. The full voice reference is
// in the signature for these mutations. `bank` carries read-only
// access to the shared wave/table pools for commands that need them
// (currently `W` — others ignore it); see the table_tick docstring
// for the nullptr contract.
static void dispatch_command(
    uint8_t cmd_byte,
    uint8_t param,
    rackboy::core::VoiceState& voice,
    TableRegState& regs,
    rackboy::core::VoiceChannel ch,
    rackboy_apu_t* apu,
    const rackboy::sav::RackboyBank* bank) noexcept
{
    switch (cmd_byte) {
    // D (0x44) — Delay command (bead rackboy-jgb).
    // Holds the table cursor on its current row for `param` Gate 2
    // ticks. On the next table_tick() call, the delay counter check
    // at the top of the function will decrement this counter instead
    // of advancing the cursor, effectively freezing the table in
    // place for the requested duration. A param of 0 means "no
    // delay" and is equivalent to no command.
    case 'D':
        voice.table_delay_ticks_remaining = param;
        break;

    // E (0x45) — Envelope retrigger command (bead rackboy-qz3).
    // Rewrites the channel's volume/envelope register with the param
    // byte AND sets NRx4 bit 7 (trigger) so the channel re-arms with
    // the new envelope. Per docs/LSDJ_REFERENCE.md's `E` row:
    //   CH1/CH2/CH4 — param → NRx2 (volume envelope byte)
    //   CH3        — param → NR32 (output-level register; NRx2 does
    //                not exist on CH3). The high 2 bits of param are
    //                treated as the NR32 output-level field per the
    //                NR32 caveat in LSDJ_REFERENCE.md.
    // The NRx4 trigger bit preserves the current freq_11bit high bits
    // for CH1/CH2/CH3 (merged in via regs.freq_11bit) so the channel
    // keeps its running pitch. CH4's NR44 only has the trigger bit.
    // Also updates regs.envelope (or regs.wave_volume for CH3) to keep
    // the cache consistent with the new register value.
    //
    // Note on deviation from LSDJ_REFERENCE.md: the reference describes
    // `E` as "pre-trigger" — i.e., it writes NRx2 but does NOT
    // retrigger the channel; a later `R` or Gate 1 does that. Per the
    // `rackboy-qz3` bead description, rackboy's `E` explicitly sets
    // the trigger bit, which is the "retrigger" in the bead title.
    // Consequence on CH3: setting NR34 bit 7 resets the wave-position
    // pointer to 0 (a documented hardware side effect) — this is
    // expected under rackboy's E semantics and is not a bug.
    case 'E':
        write_envelope_retrigger(param, regs, ch, apu);
        break;

    // H (0x48) — Hop command (bead rackboy-oks).
    // In-table hop: sets the cursor to the target row specified by
    // `param`. Only params in [0, kTableRowCount) are valid in-table
    // hops. Params >= kTableRowCount are "phrase-level" hop forms
    // that are out of scope for v1 (rackboy does not run LSDJ phrases)
    // and are treated as no-ops per docs/LSDJ_REFERENCE.md.
    //
    // Infinite hop loops (e.g., row 5 hops to row 5) are safe: each
    // tick returns from table_tick() normally, so the audio thread
    // never deadlocks.
    case 'H':
        if (param < rackboy::core::kTableRowCount) {
            voice.table_cursor = param;
        }
        // else: phrase-level form, no-op in v1
        break;

    // S (0x53) — Sweep command (bead rackboy-5yc).
    // CH1 only: writes the parameter byte directly into NR10 ($FF10),
    // which encodes the sweep unit's shift count (bits 0-2), direction
    // (bit 3: 0=increase, 1=decrease), and period (bits 4-6). Also
    // updates regs.sweep so the value is consistent with other paths
    // that may read the cached sweep byte. On non-CH1 channels this is
    // a no-op per docs/LSDJ_REFERENCE.md ("channel-specific command on
    // wrong channel"). CH2/CH3/CH4 have no sweep hardware.
    case 'S':
        if (ch == rackboy::core::VoiceChannel::Pulse1) {
            regs.sweep = param;
            rackboy_apu_write_reg(apu, kNR10, param);
        }
        // else: wrong channel, no-op
        break;

    // N (0x4E) — Noise shape command (bead rackboy-3rj).
    // CH4 only: writes the parameter byte directly into NR43 ($FF22),
    // which encodes the LFSR clock shift, width mode (bit 3), and
    // dividing ratio. Also updates regs.nr43_shape so subsequent
    // pitch-only ticks preserve the new width bit. On non-CH4 channels
    // this is a no-op per docs/LSDJ_REFERENCE.md ("channel-specific
    // command on wrong channel").
    case 'N':
        if (ch == rackboy::core::VoiceChannel::Noise) {
            regs.nr43_shape = param;
            // MPCBOY LOCAL MOD: width flag is OR'd into every NR43 write.
            rackboy_apu_write_reg(
                apu, kNR43,
                static_cast<uint8_t>(param | ((regs.noise_flags & kNoiseWidth7) ? 0x08u : 0u)));
        }
        // else: wrong channel, no-op
        break;

    // W (0x57) — Wave-set command (bead rackboy-8tz).
    // CH3 only: loads the 16-byte wave frame at `bank->waves[param]`
    // into CH3 wave RAM ($FF30..$FF3F) via rackboy_apu_write_wave_ram,
    // which performs the canonical DAC-disable / write / restore dance.
    // Per docs/LSDJ_REFERENCE.md, `W` is CH3-specific — on the pulse and
    // noise channels it is a no-op. Additional no-op conditions:
    //   - bank == nullptr (caller has no pool access; see table_tick
    //     docstring)
    //   - param >= kMaxWaveFrames (out-of-range slot index)
    //   - bank->waves[param].occupied == false (slot never populated
    //     by the parser; silently skip rather than write 16 zero bytes
    //     and mute the channel).
    // The helper does NOT retrigger CH3 (per its docstring) and NR30 = 0
    // deactivates the channel, so mpcboy retriggers it below (WP5).
    case 'W':
        if (ch == rackboy::core::VoiceChannel::Wave
            && bank != nullptr
            && param < rackboy::sav::kMaxWaveFrames
            && bank->waves[param].occupied) {
            rackboy_apu_write_wave_ram(apu, bank->waves[param].samples);
            // MPCBOY LOCAL MOD (WP5): the helper leaves CH3 silent (NR30 = 0
            // deactivates it in SameBoy), so retrigger with the running
            // frequency; the wave position restarts at sample 0. Only on the
            // engine path (table_trigger got TriggerExtras): the vendored
            // wave_set_test asserts the legacy no-retrigger behaviour.
            if (regs.engine_mode) {
                rackboy_apu_write_reg(apu, kNR34,
                                      trigger_nrx4_byte(regs.freq_11bit, regs.length_enable));
            }
        }
        // else: wrong channel, no bank, out-of-range, or unoccupied slot
        break;

    // R (0x52) — Retrigger command (bead rackboy-bgu).
    // Arms a recurring NRx4 (or NR44 for CH4) trigger fire every
    // `param` Gate 2 ticks. Per docs/LSDJ_REFERENCE.md, `R00` is a
    // no-op and is modeled here as "disable retrigger" — callers use
    // it to cancel an earlier R. Non-zero param sets both the period
    // and the running counter to `param` so the first retrigger fires
    // exactly `param` ticks after this command executes; subsequent
    // fires happen every `param` ticks until another R overrides the
    // period or Gate 1 resets the voice.
    //
    // The actual NRx4 bit-7 fire happens in table_tick()'s retrigger
    // check (before command dispatch), not here — this case is only
    // responsible for updating the counter state.
    //
    // Deliberately simpler than LSDJ's full `R` semantics: the LSDJ
    // reference mentions an optional NRx2 envelope ramp encoded in
    // the high nibble of `param`. rackboy-bgu scope is trigger-only;
    // the ramp form is out of scope for this bead (tracked as a
    // future extension). Every param byte is therefore treated as
    // a retrigger-period value with no NRx2 side effect here.
    case 'R':
        voice.retrigger_period_ticks  = param;
        voice.retrigger_counter_ticks = param;
        break;

    // V (0x56) — Vibrato command (bead rackboy-00h).
    // Arms a triangle-LFO pitch offset that is applied on top of
    // `regs.pitch_offset` during every subsequent table_tick's
    // frequency write (the LFO state does NOT mutate pitch_offset
    // itself; it's purely an ephemeral modulation).
    //
    // Param encoding per docs/LSDJ_REFERENCE.md (rackboy intends
    // `x` = speed, `y` = depth):
    //   high nibble (bits 4-7) → vibrato_speed (phase increment per tick)
    //   low  nibble (bits 0-3) → vibrato_depth (peak semitone amplitude)
    //
    // `V00` disables vibrato entirely (speed=0, depth=0, phase=0).
    // A non-zero `V` resets vibrato_phase to 0 so every new vibrato
    // command starts from zero crossing — this keeps tests
    // deterministic and matches the musical expectation that
    // retyping a V command restarts the LFO cleanly.
    //
    // The actual LFO application happens inside table_tick's pitch
    // step (and, for CH4, the NR43 recomputation); see the
    // vibrato_triangle / vibrato_offset helpers above for the
    // waveform shape.
    case 'V':
        if (param == 0) {
            voice.vibrato_speed = 0;
            voice.vibrato_depth = 0;
            voice.vibrato_phase = 0;
        } else {
            voice.vibrato_speed = static_cast<uint8_t>((param >> 4) & 0x0Fu);
            voice.vibrato_depth = static_cast<uint8_t>( param       & 0x0Fu);
            voice.vibrato_phase = 0;
        }
        break;

    // T (0x54), G (0x47), M (0x4D), O (0x4F) — global no-op commands
    // (bead rackboy-ydc). Per docs/LSDJ_REFERENCE.md "Global no-op
    // commands (v1)" table: rackboy recognizes these so a `.sav` that
    // carries them loads cleanly, but they are deliberately executed
    // as no-ops because rackboy does not own the state they would
    // otherwise modify:
    //   T — Tempo: rackboy's clock comes from Gate 2, not internal BPM.
    //   G — Groove: rackboy does not run LSDJ phrases; Gate 2 replaces
    //       groove-driven phrase-row scheduling.
    //   M — Master volume: rackboy's per-channel outputs are pre-mixer;
    //       the host's mixer owns final level.
    //   O — Output routing: host owns channel-to-bus routing (NR51
    //       panning is not modeled).
    // The reference explicitly says "No load-time warning — these are
    // expected in every `.sav`", so this handler emits nothing and
    // writes no registers. Explicit cases (rather than falling through
    // to default) keep the command list grep-able and catch any
    // accidental future reuse of these letters for a different role.
    case 'T':
    case 'G':
    case 'M':
    case 'O':
        // Grouped fall-through to a shared break — all four letters
        // are pure no-ops. No APU write, no VoiceState or TableRegState
        // mutation, no load-time log (see block comment above).
        break;

    // L (0x4C) — Pitch slide (bead rackboy-h2z).
    // Intentionally handled INLINE in table_tick's Step 4.25 (not here)
    // because the slide set-up needs `row.transpose` to reconstruct the
    // pre-transpose start pitch, and it must run before the per-channel
    // frequency write so there is exactly one freq emission per tick.
    // Both cmd1 and cmd2 slots are detected in that step. Falling
    // through this case here keeps the command-letter switch honest
    // (every known letter has an explicit arm).
    case 'L':
        break;

    // K (0x4B) — Kill / note-off command (bead rackboy-pr1).
    // Silences the active channel by zeroing its volume/envelope
    // register (NRx2 for Pulse/Noise, NR30 DAC-off for Wave) and
    // clearing the voice's triggered flag so future Gate 2 ticks
    // skip this voice. Per docs/LSDJ_REFERENCE.md, K's param
    // encodes a tick delay before the kill fires; for v1 this is
    // simplified to immediate kill regardless of param value. A
    // future bead may add a kill-timer field to VoiceState for
    // proper deferred kill semantics.
    case 'K':
        // MPCBOY LOCAL MOD (WP5): Kxx with xx == 0 kills at once (the
        // original behaviour); xx > 0 arms kill_ticks_remaining and the
        // channel is killed on the tick where it reaches 0 (a later K or a
        // trigger overrides it).
        if (param == 0) {
            kill_channel(voice, regs, ch, apu);
        } else {
            voice.kill_ticks_remaining = param;
        }
        break;

    // P (0x50) — pitch per tick (MPCBOY LOCAL MOD, WP5). The param is a
    // signed byte in 1/16 semitone: it sets pbend_step, every following tick
    // adds it to pbend_accum and rewrites the frequency (no trigger). P00
    // stops the motion and keeps the offset; a trigger resets both.
    case 'P':
        voice.pbend_step = static_cast<int8_t>(param);
        break;

    // C (0x43) — Chord command, deferred to v1.1 per docs/DECISIONS.md
    // D-001 (bead rackboy-u4j — closed wont-fix in iteration 88).
    //
    // Chord is an arpeggiation-style effect whose semantics on an
    // externally clocked voicer are ambiguous: with no guaranteed
    // "next tick" between Gate 2 edges, the cycled note either freezes
    // (silent) or bleeds stale state into the next Gate 1 retrigger.
    // Neither behavior maps cleanly onto the hardware LSDJ timing
    // chord was designed against, so v1 treats the row as a hard no-op
    // and the load-time scanner (`scan_chord_references` +
    // `format_chord_reference` in rackboy_table.h) emits a single
    // warning line per chord-using instrument at load time so the user
    // is told about the intentional divergence from stock LSDJ rather
    // than silently losing the arpeggiation.
    //
    // Audio-thread contract: no APU write, no VoiceState mutation, no
    // TableRegState mutation. Grouped as a distinct case (not folded
    // under `default:`) so `-Wswitch-enum`-style greps stay accurate
    // and a future v1.1 handler replaces this arm in place.
    case kChordCommandByte:  // 'C'
        break;

    default:
        // Unimplemented commands are deliberate no-ops. Each command
        // is tracked by its own bead and will be wired in here as
        // it is implemented.
        break;
    }
}

// ---------------------------------------------------------------------------
// table_trigger — Gate 1 rising-edge handler (Architecture Decision #9)
// ---------------------------------------------------------------------------

void table_trigger(
    rackboy::core::VoiceState& voice,
    TableRegState& regs,
    rackboy::core::VoiceChannel ch,
    const rackboy::sav::RackboyInstrument& inst,
    int16_t semitones_from_base,
    rackboy_apu_t* apu,
    const TriggerExtras* extras) noexcept
{
    // Step 1: Reset the table cursor to row 0 and clear any pending delay.
    // AD #9 says "unconditionally resets its table cursor to row 0" — even
    // if a delay command was mid-count, a Gate 1 retrigger restarts from the
    // top. The recurring `R` retrigger state is cleared on the same terms:
    // a Gate 1 event is a full voice restart, so any in-flight retrigger
    // countdown from the previous note must not bleed into this one.
    voice.table_cursor                  = 0;
    voice.table_delay_ticks_remaining   = 0;
    voice.retrigger_period_ticks        = 0;
    voice.retrigger_counter_ticks       = 0;
    // Vibrato (bead rackboy-00h): a Gate 1 trigger is a full voice
    // restart, so any in-flight vibrato from the previous note must
    // not bleed into this one. The `V` command re-arms vibrato from
    // a fresh zero-phase if the new note wants it.
    voice.vibrato_speed                 = 0;
    voice.vibrato_depth                 = 0;
    voice.vibrato_phase                 = 0;
    // Pitch slide (bead rackboy-h2z): same rationale. A Gate 1 retrigger
    // cancels any in-flight slide so the new note starts at its full
    // target pitch rather than mid-interpolation.
    voice.slide_target_pitch            = 0;
    voice.slide_ticks_remaining         = 0;
    // MPCBOY LOCAL MOD (WP5): `P` motion and a pending delayed `K` also end
    // with the note.
    voice.pbend_accum                   = 0;
    voice.pbend_step                    = 0;
    voice.kill_ticks_remaining          = 0;

    // Step 2: Mark this voice as triggered so the rest of the audio path
    // knows it should be producing audio rather than silence.
    voice.triggered = true;

    // Step 3: Populate regs from the instrument bytes and compute the initial
    // frequency. The instrument type byte (raw[0]) selects the decode path.
    regs.pitch_offset = semitones_from_base;

    // MPCBOY LOCAL MOD: companion data (fine tune, bend, noise options, length).
    // nullptr extras reproduce the pre-mpcboy behaviour exactly.
    const bool engine_mode = (extras != nullptr);
    regs.fine_cents    = engine_mode ? extras->fine_cents : 0;
    regs.bend_cents    = engine_mode ? extras->bend_cents : 0;
    regs.noise_flags   = engine_mode ? extras->noise_flags : 0;
    unsigned length    = engine_mode ? extras->length : 0u;
    if (length > 64u) length = 64u;
    regs.length_enable = (length > 0u) ? 1 : 0;
    regs.engine_mode   = engine_mode ? 1 : 0;
    // Length register payloads: pulse/noise NRx1 low 6 bits = 64 - length,
    // wave NR31 = 256 - 4 * length (counter 4 * length steps).
    const uint8_t len_lo6 = length ? static_cast<uint8_t>((64u - length) & 0x3Fu) : 0x3F;
    const uint8_t len_nr31 = length ? static_cast<uint8_t>(256u - 4u * length > 255u ? 255u : 256u - 4u * length) : 0xFF;

    switch (ch) {
    // -----------------------------------------------------------------------
    case rackboy::core::VoiceChannel::Pulse1:
    // -----------------------------------------------------------------------
    {
        // Instrument byte layout (Pulse, from rackboy_sav_types.h):
        //   raw[1] = envelope  → NR12
        //   raw[4] = sweep     → NR10 (CH1 only)
        //   raw[7] bits 6-7   = duty cycle

        regs.envelope   = inst.raw[1];
        regs.sweep      = inst.raw[4];
        regs.duty       = inst.raw[7] & 0xC0;  // bits 6-7 only
        regs.nr43_shape = 0;
        regs.wave_volume = 0;

        // (pitch is computed and NR13/NR14 written by write_channel_pitch below)

        // NR10: sweep (write before NR11/NR12 so the sweep unit has its
        // initial parameters before the trigger fires).
        rackboy_apu_write_reg(apu, kNR10, regs.sweep);
        // NR11: duty in bits 6-7; lower 6 bits are length counter — write 0x3F
        // for maximum length so the channel runs until silenced by Gate.
        rackboy_apu_write_reg(apu, kNR11, static_cast<uint8_t>(regs.duty | len_lo6));
        // NR12: initial volume envelope.
        rackboy_apu_write_reg(apu, kNR12, regs.envelope);
        // NR13 / NR14: frequency + trigger bit (bit 7) (+ length enable, bit 6).
        write_channel_pitch(apu, ch, regs, 0, voice.pbend_accum, true, engine_mode);
        break;
    }

    // -----------------------------------------------------------------------
    case rackboy::core::VoiceChannel::Pulse2:
    // -----------------------------------------------------------------------
    {
        // Instrument byte layout (Pulse):
        //   raw[1] = envelope  → NR22
        //   raw[4] = sweep byte is ignored on CH2 (no sweep hardware)
        //   raw[7] bits 6-7   = duty cycle


        regs.envelope    = inst.raw[1];
        regs.sweep       = 0;  // CH2 has no sweep
        regs.duty        = inst.raw[7] & 0xC0;
        regs.nr43_shape  = 0;
        regs.wave_volume = 0;

        // (pitch is computed and NR23/NR24 written by write_channel_pitch below)

        // NR21: duty in bits 6-7; lower 6 bits = length (0x3F = max).
        rackboy_apu_write_reg(apu, kNR21, static_cast<uint8_t>(regs.duty | len_lo6));
        // NR22: initial volume envelope.
        rackboy_apu_write_reg(apu, kNR22, regs.envelope);
        // NR23 / NR24: frequency + trigger bit (+ length enable).
        write_channel_pitch(apu, ch, regs, 0, voice.pbend_accum, true, engine_mode);
        break;
    }

    // -----------------------------------------------------------------------
    case rackboy::core::VoiceChannel::Wave:
    // -----------------------------------------------------------------------
    {
        // Instrument byte layout (Wave, from rackboy_sav_types.h):
        //   raw[1] = volume → NR32 output level bits (bits 6-5 of NR32)
        //   raw[6] = table slot (consumed by caller, not needed here)
        //   raw[8] = wave_index (consumed by bank/wave loader bead, not here)


        regs.envelope    = 0;  // CH3 uses discrete levels, not a DAC envelope
        regs.sweep       = 0;
        regs.duty        = 0;
        regs.nr43_shape  = 0;
        regs.wave_volume = inst.raw[1];  // bits 6-5 carry the 2-bit level

        // (pitch is computed and NR33/NR34 written by write_channel_pitch below)

        // NR30: DAC power on (bit 7). Must be set before triggering CH3 or
        // the trigger is silently ignored by the hardware.
        rackboy_apu_write_reg(apu, kNR30, 0x80);
        // NR31: length counter — 0xFF = maximum (effectively unlimited in
        // non-length mode).
        rackboy_apu_write_reg(apu, kNR31, len_nr31);
        // NR32: output level. Bits 6-5 encode the 2-bit discrete level:
        //   00 = mute, 01 = 100%, 10 = 50%, 11 = 25%.
        // wave_volume already contains the instrument raw[1] byte which
        // sources these bits; mask to ensure only bits 6-5 reach the register.
        rackboy_apu_write_reg(apu, kNR32,
                              static_cast<uint8_t>(regs.wave_volume & kNR32OutputLevelMask));
        // NR33 / NR34: frequency + trigger bit (+ length enable).
        write_channel_pitch(apu, ch, regs, 0, voice.pbend_accum, true, engine_mode);
        break;
    }

    // -----------------------------------------------------------------------
    case rackboy::core::VoiceChannel::Noise:
    // -----------------------------------------------------------------------
    {
        // Instrument byte layout (Noise, from rackboy_sav_types.h):
        //   raw[1] = envelope → NR42
        //   raw[4] = shape    → NR43 initial byte (LFSR clock + shift + period)


        regs.envelope    = inst.raw[1];
        regs.sweep       = 0;
        regs.duty        = 0;
        regs.wave_volume = 0;
        // raw[4] is the initial NR43 byte as stored by the LSDJ parser.
        // We also compute a pitch-adjusted NR43 in case semitones_from_base
        // is non-zero (e.g., a pitch CV offset applied at trigger time).
        // If semitones_from_base is zero (no pitch offset), pitch_noise_nr43(0)
        // returns the "closest to C5" NR43 — callers should set inst.raw[4]
        // as the authoritative initial shape and only call pitch_noise_nr43()
        // when a transposition is actually requested. For now we always use
        // pitch_noise_nr43() so pitch CV works, and keep raw[4] in nr43_shape
        // for later commands that restore the instrument shape.
        regs.nr43_shape  = inst.raw[4];
        // freq_11bit is unused for CH4; zero it for cleanliness.
        regs.freq_11bit  = 0;

        // NR41: length counter — 0x3F = maximum (6-bit counter, all ones).
        rackboy_apu_write_reg(apu, kNR41, len_lo6);
        // NR42: initial volume envelope.
        rackboy_apu_write_reg(apu, kNR42, regs.envelope);
        // NR43 + NR44 (trigger bit, + length enable): write_channel_pitch.
        // Legacy rule (extras == nullptr): semitones_from_base == 0 uses the
        // instrument's raw[4] byte, otherwise pitch_noise_nr43().
        write_channel_pitch(apu, ch, regs, 0, voice.pbend_accum, true, engine_mode);
        break;
    }
    }  // switch (ch)
}

// ---------------------------------------------------------------------------
// table_tick — Gate 2 rising-edge handler (Architecture Decision #10)
// ---------------------------------------------------------------------------

void table_tick(
    rackboy::core::VoiceState& voice,
    TableRegState& regs,
    rackboy::core::VoiceChannel ch,
    const rackboy::sav::RackboyTable& table,
    rackboy_apu_t* apu,
    const rackboy::sav::RackboyBank* bank) noexcept
{
    // MPCBOY LOCAL MOD (WP5): delayed `K`. Counts every tick (also while a `D`
    // delay holds the cursor); the kill fires on the tick where the counter
    // reaches 0 and that tick does nothing else.
    if (voice.kill_ticks_remaining > 0) {
        if (--voice.kill_ticks_remaining == 0) {
            kill_channel(voice, regs, ch, apu);
            return;
        }
    }

    // Step 1: Delay handling. If a delay command is in effect, consume one
    // tick and return without advancing the cursor. The cursor stays frozen
    // and no registers are written this tick.
    if (voice.table_delay_ticks_remaining > 0) {
        --voice.table_delay_ticks_remaining;
        return;
    }

    // Step 2: Read the current row BEFORE advancing the cursor. On the very
    // first Gate 2 tick after a Gate 1 trigger the cursor is 0, so this
    // applies row 0's transpose and commands. After reading, the cursor
    // advances to the next row for the following tick. LSDJ tables loop at
    // kTableRowCount (16 rows).
    const rackboy::sav::RackboyTableRow& row = table.rows[voice.table_cursor];

    // Step 3: Advance the cursor for the next tick.
    ++voice.table_cursor;
    if (voice.table_cursor >= rackboy::core::kTableRowCount) {
        voice.table_cursor = 0;
    }

    // Step 4: Apply transpose. The row's signed transpose byte is a semitone
    // delta that accumulates into the running pitch_offset. After updating,
    // recompute the frequency register and write NRx3/NRx4 WITHOUT the trigger
    // bit so the envelope is not restarted.
    regs.pitch_offset += static_cast<int16_t>(row.transpose);

    // Step 4.25: Pitch slide / L command (bead rackboy-h2z). LSDJ `L xx`
    // linearly interpolates from the previous note's frequency to the
    // current row's post-transpose target over `xx` ticks. Handled
    // inline here (rather than in dispatch_command) because it needs
    // `row.transpose` to reconstruct the slide start, mirrors the
    // structure of Step 5's recurring `R` check, and keeps the per-
    // channel frequency write below this block to a single emission
    // per tick.
    //
    // Fresh L (L is in cmd1 or cmd2 with a non-zero param):
    //   target = regs.pitch_offset (post-transpose, what Step 4 just
    //            computed)
    //   start  = target - row.transpose (pre-transpose, the "previous
    //            note's frequency")
    //   slide_target_pitch := target
    //   slide_ticks_remaining := param
    //   regs.pitch_offset := start  (so this tick's freq write lands
    //                                 at the slide start)
    //
    // Mid-slide (slide_ticks_remaining > 0 and this row does NOT
    // initiate a fresh L): advance one linear step toward target via
    // integer division — (target - current) / ticks_remaining. On
    // the final tick that quotient is exactly the remaining delta,
    // so the slide lands precisely on the target with no rounding
    // drift. Then decrement ticks_remaining.
    //
    // L00 (param == 0): per the LSDJ reference, an `L` with a zero
    // param is a no-op slide; treat it as a cancel of any in-flight
    // slide so a row that explicitly writes `L 00` does not leave a
    // prior slide state lingering. The current row's transpose still
    // takes effect normally.
    //
    // Precedence: if both cmd1 and cmd2 are 'L', cmd1 wins and cmd2 is
    // silently ignored. A row cannot carry two independent slides, and
    // the first slot is the primary command slot per the table-row
    // layout in docs/LSDJ_REFERENCE.md.
    //
    // Mid-slide + non-L row transpose: the slide target is translated
    // by `row.transpose` BEFORE the per-tick step is computed, so a
    // transpose while a slide is in flight shifts both the current
    // pitch and the target by the same amount — preserving the slide's
    // relative direction and magnitude. Option B from the iteration 60
    // review: matches musical expectation (transposes stack additively
    // onto the running slide destination) and is the more natural read
    // of the LSDJ reference's "current row's target" wording.
    //
    // Retrigger + slide: the recurring `R` fire in Step 5 writes the
    // channel's NRx4 trigger bit but does not touch slide state, so an
    // in-flight slide continues across retrigger fires. LSDJ's "A table
    // row that both slides and retriggers queues the slide starting
    // from the new note" is handled by the existing L/R ordering:
    // Step 4.25 arms the new slide; Step 5 resets the retrigger
    // counter; subsequent ticks slide + retrigger independently.
    {
        uint8_t l_param = 0;
        if (row.cmd1 == 'L') l_param = row.cmd1_param;
        else if (row.cmd2 == 'L') l_param = row.cmd2_param;

        const bool fresh_l = (l_param != 0);
        const bool explicit_l_zero = (row.cmd1 == 'L' && row.cmd1_param == 0) ||
                                     (row.cmd2 == 'L' && row.cmd2_param == 0);

        if (fresh_l) {
            voice.slide_target_pitch    = regs.pitch_offset;
            voice.slide_ticks_remaining = l_param;
            regs.pitch_offset = static_cast<int16_t>(
                regs.pitch_offset - static_cast<int16_t>(row.transpose));
        } else if (explicit_l_zero) {
            voice.slide_target_pitch    = 0;
            voice.slide_ticks_remaining = 0;
        } else if (voice.slide_ticks_remaining > 0) {
            // Option B: translate the slide target by the current
            // row's transpose so a transpose mid-slide shifts target
            // and current pitch by the same amount, keeping the slide
            // relative. row.transpose has already been folded into
            // regs.pitch_offset at Step 4; mirroring it into the
            // target preserves the unchanged delta.
            voice.slide_target_pitch = static_cast<int16_t>(
                voice.slide_target_pitch + static_cast<int16_t>(row.transpose));
            const int16_t remaining_delta = static_cast<int16_t>(
                voice.slide_target_pitch - regs.pitch_offset);
            const int16_t step = static_cast<int16_t>(
                remaining_delta / voice.slide_ticks_remaining);
            regs.pitch_offset = static_cast<int16_t>(regs.pitch_offset + step);
            --voice.slide_ticks_remaining;
        }
    }

    // Step 4.5: Vibrato (bead rackboy-00h). If the `V` command has
    // armed vibrato on this voice, advance the LFO phase by
    // vibrato_speed and compute an ephemeral semitone offset for
    // THIS tick's frequency write. The offset is NOT persisted to
    // regs.pitch_offset — vibrato modulates the frequency register
    // only, leaving the "base" pitch untouched so a later `K` kill
    // or a Gate 1 reset behaves as if vibrato were not running.
    //
    // Phase advance happens BEFORE the per-channel pitch write so
    // the very first tick after `V` dispatches already reflects a
    // non-zero phase (V is dispatched at Step 6, which runs AFTER
    // this step — but for the first tick the new vibrato_speed is
    // not yet installed, so no phase advance happens that first
    // tick, and the row-0 frequency write is the unmodulated base
    // pitch. The LFO starts audibly advancing on the tick AFTER V,
    // matching the dispatch-ordering story of R and D).
    int16_t vibrato_offset_semi = 0;
    if (voice.vibrato_speed != 0) {
        voice.vibrato_phase = static_cast<uint8_t>(
            voice.vibrato_phase + voice.vibrato_speed);  // wraps at 256
        vibrato_offset_semi = vibrato_offset(voice.vibrato_phase,
                                              voice.vibrato_depth);
    }

    // MPCBOY LOCAL MOD: effective pitch for THIS tick's register write only
    // (regs.pitch_offset is left unmodified so the next tick's base pitch is
    // the transpose-accumulated value, not transpose + vibrato). Composition,
    // noise rules and length-enable handling live in write_channel_pitch; the
    // four former per-channel call sites of the pitch helpers (original lines
    // 853, 859, 865, 880) are routed through it.
    // MPCBOY LOCAL MOD (WP5): `P` motion advances once per tick.
    if (voice.pbend_step != 0) {
        int32_t acc = static_cast<int32_t>(voice.pbend_accum) + voice.pbend_step;
        if (acc > 32000) acc = 32000;
        if (acc < -32000) acc = -32000;
        voice.pbend_accum = static_cast<int16_t>(acc);
    }
    write_channel_pitch(apu, ch, regs, vibrato_offset_semi, voice.pbend_accum, false, true);

    // Step 5: Recurring `R` retrigger check (bead rackboy-bgu).
    // When retrigger_period_ticks > 0, an R command is active. Decrement
    // the running counter; when it reaches zero, fire NRx4 bit 7 and reset
    // the counter back to the period. The fire preserves the current
    // freq_11bit high bits (via trigger_nrx4_byte) so the channel keeps
    // its running pitch — R only re-arms the trigger, it does not change
    // pitch or envelope (see comment in dispatch_command's 'R' case).
    //
    // This runs BEFORE dispatch_command so that an R on the current row
    // sets the period and counter AFTER this check — meaning the first
    // retrigger fires exactly `param` ticks after R is placed rather than
    // on the same tick as R itself. Dispatch ordering invariant: other
    // commands in the same row (E, K) that also touch NRx4 run after
    // this check and therefore win if they conflict.
    //
    // On CH3 specifically, writing NR34 with bit 7 set resets the
    // wave-position pointer to 0 — same hardware side effect as the
    // `E` command's NR34 retrigger (documented at the `E` case above).
    // This is the expected behavior for R on CH3: a retrigger should
    // restart the wave from the top. A non-resetting variant that only
    // re-arms the envelope without disturbing the wave cursor would
    // have to bypass the trigger bit entirely and is out of scope for
    // rackboy-bgu.
    if (voice.retrigger_period_ticks != 0) {
        --voice.retrigger_counter_ticks;
        if (voice.retrigger_counter_ticks == 0) {
            voice.retrigger_counter_ticks = voice.retrigger_period_ticks;
            switch (ch) {
            case rackboy::core::VoiceChannel::Pulse1:
                rackboy_apu_write_reg(apu, kNR14,
                                      trigger_nrx4_byte(regs.freq_11bit, regs.length_enable));
                break;
            case rackboy::core::VoiceChannel::Pulse2:
                rackboy_apu_write_reg(apu, kNR24,
                                      trigger_nrx4_byte(regs.freq_11bit, regs.length_enable));
                break;
            case rackboy::core::VoiceChannel::Wave:
                rackboy_apu_write_reg(apu, kNR34,
                                      trigger_nrx4_byte(regs.freq_11bit, regs.length_enable));
                break;
            case rackboy::core::VoiceChannel::Noise:
                // NR44 has no frequency bits — only the trigger bit.
                rackboy_apu_write_reg(apu, kNR44,
                                      static_cast<uint8_t>(kTriggerBit | (regs.length_enable ? kLengthEnableBit : 0)));
                break;
            }
        }
    }

    // MPCBOY LOCAL MOD (WP5): table volume column. After the pitch write so the
    // retrigger carries the current frequency; before the commands so an `E`
    // or `K` in the same row still wins.
    if (row.has_vol) {
        write_envelope_retrigger(row.vol, regs, ch, apu);
    }

    // Step 6: Dispatch table commands. Command byte 0x00 means "no command in
    // this slot" and is skipped to avoid handing the stub a no-op it would
    // have to handle. When the individual command handlers are implemented
    // (each tracked by its own bead), dispatch_command() will route to them.
    if (row.cmd1 != 0) {
        dispatch_command(row.cmd1, row.cmd1_param, voice, regs, ch, apu, bank);
    }
    if (row.cmd2 != 0) {
        dispatch_command(row.cmd2, row.cmd2_param, voice, regs, ch, apu, bank);
    }
}

// ---------------------------------------------------------------------------
// MPCBOY LOCAL MOD: table_write_pitch
// ---------------------------------------------------------------------------

void table_write_pitch(
    const rackboy::core::VoiceState& voice,
    TableRegState& regs,
    rackboy::core::VoiceChannel ch,
    rackboy_apu_t* apu) noexcept
{
    // Keep the current vibrato phase (do not advance it): a bend or glide
    // between ticks must not jump the LFO.
    const int16_t vib = (voice.vibrato_speed != 0)
        ? vibrato_offset(voice.vibrato_phase, voice.vibrato_depth)
        : static_cast<int16_t>(0);
    write_channel_pitch(apu, ch, regs, vib, voice.pbend_accum, false, true);
}

// ---------------------------------------------------------------------------
// Chord-command load-time scanner / formatter — docs/DECISIONS.md D-001
// ---------------------------------------------------------------------------
//
// Parallels `rackboy_core/rackboy_clock.cpp`'s table-eligibility gate so
// the scan sees the same set of instrument→table edges the runtime
// tick path actually follows. Duplicated here (instead of promoted to a
// shared header) for the same reason the clock module duplicated its
// pool lookup: the helper is five lines, a shared header would drag
// rackboy_core into rackboy_table's public surface, and a third caller
// is the threshold for extracting it.

namespace {

// Instrument byte offset for the table slot index. Same constant as
// rackboy_core/rackboy_clock.cpp's `kTableSlotByteOffset`; kept in
// sync by docs/COMPATIBILITY.md.
constexpr std::size_t kTableSlotByteOffset = 6;

// Pulse instrument byte 5 bit 4: table-enable flag. Pulse instruments
// with this bit clear do not run a table even if byte 6 names a slot.
constexpr std::uint8_t kPulseTableEnableBit = 0x10;

// Per-channel table eligibility — mirrors rackboy_clock.cpp's
// `instrument_has_table` so the scan enumerates exactly the
// instrument → table edges the runtime tick path would follow.
bool instrument_has_table(
    rackboy::core::VoiceChannel ch,
    const rackboy::sav::RackboyInstrument& inst) noexcept
{
    const std::uint8_t slot = inst.raw[kTableSlotByteOffset];
    if (slot == rackboy::sav::kNoTable) return false;

    switch (ch) {
    case rackboy::core::VoiceChannel::Pulse1:
    case rackboy::core::VoiceChannel::Pulse2:
        return (inst.raw[5] & kPulseTableEnableBit) != 0;
    case rackboy::core::VoiceChannel::Wave:
    case rackboy::core::VoiceChannel::Noise:
        return true;
    }
    // VoiceChannel is a closed enum (uint8_t underlying type, 4
    // named enumerators — Pulse1/Pulse2/Wave/Noise). Landing here
    // means a future contributor added a new enumerator without
    // wiring it up to the two channel groups above — we want
    // `-Wswitch-enum` to surface that as a diagnostic before the
    // silent `return false` fallback lets the scanner skip every
    // instrument on the new channel. Marking the path unreachable
    // keeps the compiler from suppressing the warning (a post-switch
    // `return false` is a legal path and silences -Wswitch).
    __builtin_unreachable();
}

// Return true iff `table` has at least one row whose cmd1 or cmd2
// slot contains the chord command byte.
bool table_contains_chord(const rackboy::sav::RackboyTable& table) noexcept
{
    for (std::size_t row = 0; row < rackboy::sav::kTableRowCount; ++row) {
        const rackboy::sav::RackboyTableRow& r = table.rows[row];
        if (r.cmd1 == kChordCommandByte) return true;
        if (r.cmd2 == kChordCommandByte) return true;
    }
    return false;
}

// Channel abbreviation used by `format_chord_reference`. Mirrors the
// D-001 "<CH>" placeholder convention — no switch-enum fallthrough on
// purpose so an added VoiceChannel enumerator triggers a diagnostic
// here at compile time.
const char* channel_abbrev(rackboy::core::VoiceChannel ch) noexcept
{
    switch (ch) {
    case rackboy::core::VoiceChannel::Pulse1: return "P1";
    case rackboy::core::VoiceChannel::Pulse2: return "P2";
    case rackboy::core::VoiceChannel::Wave:   return "W";
    case rackboy::core::VoiceChannel::Noise:  return "N";
    }
    // See the comment in `instrument_has_table`: the unreachable
    // marker is load-bearing for `-Wswitch-enum` exhaustiveness
    // checking. A post-switch `return "?"` would silence the
    // diagnostic and let a new channel ship with a placeholder
    // abbreviation in every D-001 warning line.
    __builtin_unreachable();
}

// Walk one instrument pool, append a ChordReference for every
// chord-using slot, and return the updated `written` count. The
// `channel_in` + `pool` + `count` triple is the dense-prefix view
// used by both the parser and rackboy_clock. `total` tracks the
// *discovered* chord-using instrument count (may exceed `out_cap`);
// `written` tracks the *stored* count (bounded by `out_cap`).
void scan_pool(
    const rackboy::sav::RackboyBank&     bank,
    rackboy::core::VoiceChannel          channel_in,
    const rackboy::sav::RackboyInstrument* pool,
    std::uint8_t                         count,
    ChordReference*                      out,
    std::size_t                          out_cap,
    std::size_t&                         written,
    std::size_t&                         total) noexcept
{
    const std::size_t pool_size = static_cast<std::size_t>(count);
    for (std::size_t i = 0; i < pool_size; ++i) {
        const rackboy::sav::RackboyInstrument& inst = pool[i];
        if (!instrument_has_table(channel_in, inst)) continue;

        const std::uint8_t table_slot = inst.raw[kTableSlotByteOffset];
        if (table_slot >= rackboy::sav::kMaxTables) continue;

        const rackboy::sav::RackboyTable& table = bank.tables[table_slot];
        if (!table.occupied) continue;

        if (!table_contains_chord(table)) continue;

        ++total;
        if (written < out_cap) {
            out[written].channel         = channel_in;
            out[written].instrument_slot = static_cast<std::uint8_t>(i);
            out[written].table_slot      = table_slot;
            ++written;
        }
    }
}

}  // namespace

std::size_t scan_chord_references(
    const rackboy::sav::RackboyBank& bank,
    ChordReference*                  out,
    std::size_t                      out_cap) noexcept
{
    // "Count-only" mode: out == nullptr is legal when out_cap == 0.
    // Any other mismatch is a caller contract violation — the most
    // useful thing we can do is degrade to count-only rather than
    // write into a null pointer, so clamp out_cap to 0 when out is
    // null. A debug-build static_assert would be nice but is not
    // feasible here because `out_cap` is a runtime value.
    if (out == nullptr) out_cap = 0;

    std::size_t written = 0;
    std::size_t total   = 0;

    scan_pool(bank, rackboy::core::VoiceChannel::Pulse1,
              bank.pulse1_pool, bank.pulse1_count,
              out, out_cap, written, total);
    scan_pool(bank, rackboy::core::VoiceChannel::Pulse2,
              bank.pulse2_pool, bank.pulse2_count,
              out, out_cap, written, total);
    scan_pool(bank, rackboy::core::VoiceChannel::Wave,
              bank.wave_pool, bank.wave_count,
              out, out_cap, written, total);
    scan_pool(bank, rackboy::core::VoiceChannel::Noise,
              bank.noise_pool, bank.noise_count,
              out, out_cap, written, total);

    return total;
}

std::size_t format_chord_reference(
    const ChordReference& ref,
    char*                 buf,
    std::size_t           buf_len) noexcept
{
    if (buf_len == 0) return 0;

    // Hand-written formatter — no allocation, no printf dependency
    // beyond std::snprintf. The canonical D-001 template:
    //
    //   [rackboy_table] instrument <CH>-<SLOT> uses chord command — deferred to v1.1, treated as no-op
    //
    // The em-dash is U+2014 (UTF-8: 0xE2 0x80 0x94). std::snprintf
    // emits the literal bytes from the format string directly —
    // rackboy firmware and the host both assume UTF-8 source
    // encoding (enforced by -finput-charset=UTF-8 in both
    // Makefiles) so no transcoding is needed here.
    const char* ch_abbrev = channel_abbrev(ref.channel);
    const int   slot_val  = static_cast<int>(ref.instrument_slot);

    const int written = std::snprintf(
        buf, buf_len,
        "[rackboy_table] instrument %s-%d uses chord command \xE2\x80\x94"
        " deferred to v1.1, treated as no-op",
        ch_abbrev, slot_val);

    // std::snprintf returns a negative value only on an encoding
    // error against the ambient locale, which cannot happen with a
    // literal UTF-8 format string and %s/%d substitutions. Clamp
    // defensively anyway so the return stays a well-defined size_t
    // in the pathological case.
    if (written < 0) return 0;
    return static_cast<std::size_t>(written);
}

}  // namespace table
}  // namespace rackboy
