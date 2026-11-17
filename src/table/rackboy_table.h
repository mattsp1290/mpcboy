// rackboy_table/rackboy_table.h
//
// Per-voice LSDJ table state machine. Translates table rows from the
// parsed rackboy bank into APU register writes on each Gate 2 tick.
//
// Tracked by bead rackboy-tj3.
//
// Architecture decisions this module implements:
//
//   AD #9 (Gate 1 retrigger semantics): on Gate 1 rising edge, every
//   channel unconditionally resets its table cursor to row 0, reapplies
//   its selected instrument's initial NRxx register set, and sets the
//   trigger bit (NRx4 bit 7). Implemented by `table_trigger()`.
//
//   AD #10 (Gate 2 absence semantics): when Gate 2 has not edged
//   recently, each voice's table cursor freezes at its current row. The
//   APU itself continues running on its own clock. The caller is
//   responsible for only calling `table_tick()` on a Gate 2 rising
//   edge; this module does not observe Gate 2 directly.
//
// Tick semantics:
//   - Gate 2 rising edge → call table_tick() → advance cursor by 1 row,
//     apply that row's commands as NRxx writes.
//   - Delay commands (when table_delay_ticks_remaining > 0) hold the
//     cursor; each Gate 2 edge decrements the counter and returns early.
//   - Hop commands alter the cursor position.
//   - Between Gate 2 edges → caller does not call table_tick(); cursor
//     is frozen, APU continues under SameBoy's own emulation clock.
//   - When Gate 2 has never fired → tables never advance; voices play
//     their initial row 0 state from table_trigger().
//
// No allocations, no STL containers, no virtual methods. All functions
// are `noexcept`. The module is safe to call from the audio thread.

#ifndef RACKBOY_TABLE_RACKBOY_TABLE_H_
#define RACKBOY_TABLE_RACKBOY_TABLE_H_

#include <cstdint>
#include <type_traits>

#include "rackboy_voice.h"        // -Irackboy_core
#include "rackboy_sav_types.h"    // -Irackboy_sav

// ---------------------------------------------------------------------------
// Forward declaration of the C APU driver
// ---------------------------------------------------------------------------
//
// rackboy_apu_t is defined in rackboy_apu/rackboy_apu.h (a C header that
// pulls in SameBoy's GB_gameboy_t). Including that header here would
// introduce the entire SameBoy vendor tree as a compile-time dependency of
// every translation unit that includes rackboy_table.h. Instead, we forward-
// declare the struct tag and the functions we need; the full definition
// is visible in rackboy_table.cpp where rackboy_apu.h is included.
extern "C" {
struct rackboy_apu_s;
typedef struct rackboy_apu_s rackboy_apu_t;
void rackboy_apu_write_reg(rackboy_apu_t *apu, uint16_t addr, uint8_t value);
void rackboy_apu_write_wave_ram(rackboy_apu_t *apu, const uint8_t waveform[16]);
}

namespace rackboy {
namespace table {

// ---------------------------------------------------------------------------
// NRxx register CPU addresses
// ---------------------------------------------------------------------------
//
// These are the CPU-bus addresses passed to rackboy_apu_write_reg().
// Grouped here so they can be referenced from both table_trigger() and
// table_tick() without duplication.

// CH1 (Pulse1)
constexpr uint16_t kNR10 = 0xFF10;  // sweep
constexpr uint16_t kNR11 = 0xFF11;  // length / duty
constexpr uint16_t kNR12 = 0xFF12;  // volume / envelope
constexpr uint16_t kNR13 = 0xFF13;  // frequency low
constexpr uint16_t kNR14 = 0xFF14;  // frequency high + trigger

// CH2 (Pulse2) — no sweep register
constexpr uint16_t kNR21 = 0xFF16;  // length / duty
constexpr uint16_t kNR22 = 0xFF17;  // volume / envelope
constexpr uint16_t kNR23 = 0xFF18;  // frequency low
constexpr uint16_t kNR24 = 0xFF19;  // frequency high + trigger

// CH3 (Wave)
constexpr uint16_t kNR30 = 0xFF1A;  // DAC power
constexpr uint16_t kNR31 = 0xFF1B;  // length
constexpr uint16_t kNR32 = 0xFF1C;  // output level
constexpr uint16_t kNR33 = 0xFF1D;  // frequency low
constexpr uint16_t kNR34 = 0xFF1E;  // frequency high + trigger

// CH4 (Noise)
constexpr uint16_t kNR41 = 0xFF20;  // length
constexpr uint16_t kNR42 = 0xFF21;  // volume / envelope
constexpr uint16_t kNR43 = 0xFF22;  // LFSR clock / shape
constexpr uint16_t kNR44 = 0xFF23;  // trigger

// ---------------------------------------------------------------------------
// NRx4 / NR44 bit masks
// ---------------------------------------------------------------------------

// Setting bit 7 of NRx4 (or NR44 for CH4) restarts the channel — this is
// the hardware "trigger" bit. table_trigger() always sets it; table_tick()
// never does (pitch-only frequency updates must not retrigger the channel,
// which would reset the volume envelope to its initial value on every tick).
constexpr uint8_t kTriggerBit = 0x80;

// Bits 2-0 of NRx4 carry the high 3 bits of the 11-bit frequency value.
constexpr uint8_t kFreqHiMask = 0x07;

// Bits 6-5 of NR32 are the CH3 (Wave) output-level field:
//   0b00 = mute, 0b01 = 100%, 0b10 = 50%, 0b11 = 25%.
// Used by both table_trigger (initial NR32 write) and the `E` command
// handler to mask the LSDJ-source byte down to the hardware's 2-bit field.
constexpr uint8_t kNR32OutputLevelMask = 0x60;

// ---------------------------------------------------------------------------
// TableRegState — per-voice cached register target
// ---------------------------------------------------------------------------
//
// Carries the "current intent" for one voice's NRxx registers between
// Gate 1 trigger events and table ticks. On a Gate 1 rising edge,
// table_trigger() populates this struct from the instrument bytes and
// writes the initial register set to the APU. On each Gate 2 tick,
// table_tick() mutates pitch_offset (and recomputes the frequency) in
// response to table row transpose fields, then writes only the affected
// registers.
//
// POD contract: trivially copyable + standard layout so the audio-thread
// bank swap-in (rackboy-2i8) can memcpy a whole array of these without
// concern for non-trivial copy semantics or alignment surprises.
//
// Intentionally no user-declared constructors / destructor / copy-or-move
// operators — same reasoning as VoiceState in rackboy_voice.h. Initialize
// via table_reg_state_reset(), never via a constructor body.
struct TableRegState {
    // Current 11-bit frequency register value. Used for CH1 (Pulse1),
    // CH2 (Pulse2), and CH3 (Wave). For CH4 (Noise) this field is unused
    // (always 0) because the frequency register NR43 is packed differently.
    uint16_t freq_11bit;

    // Current volume envelope byte. Maps to NR12 (CH1), NR22 (CH2), or
    // NR42 (CH4). Unused (0) for CH3 where NR32 encodes a discrete output
    // level rather than a full DAC envelope. Table `E` commands will update
    // this field when that command handler is implemented.
    uint8_t envelope;

    // Duty cycle bits for Pulse channels. Sits in bits 6-7 of NR11/NR21.
    // The lower 6 bits of NRx1 are the length counter, which we always
    // write as 0x3F (max length / effectively no length limit in non-length
    // mode). Unused (0) for CH3 and CH4.
    uint8_t duty;

    // NR10 sweep byte, CH1 only. Written verbatim to 0xFF10 on trigger.
    // Always 0 for CH2, CH3, and CH4.
    uint8_t sweep;

    // NR43 LFSR clock / shape byte, CH4 only. Written verbatim to 0xFF22.
    // Always 0 for CH1, CH2, and CH3.
    uint8_t nr43_shape;

    // NR32 output-level bits, CH3 only. The hardware encodes output level
    // in bits 6-5 of NR32:
    //   0b00 = mute, 0b01 = 100%, 0b10 = 50%, 0b11 = 25%
    // We store the raw instrument byte here (bits 6-5 from inst.raw[1])
    // and shift it into position when writing NR32. Always 0 for other
    // channels.
    uint8_t wave_volume;

    // Current pitch offset in semitones from the instrument's base note.
    // Set to `semitones_from_base` on trigger. Each table row's `transpose`
    // field is added to this on every tick; the updated value is converted
    // to a frequency register by the pitch_* helpers and written to NRx3/NRx4
    // (without the trigger bit). int16_t gives ±32767 semitones of headroom;
    // the pitch helpers clamp to hardware-representable extremes.
    int16_t pitch_offset;

    // ---- MPCBOY LOCAL MOD (WP4) -------------------------------------------
    // Pitch bend in cents, applied on top of the table pitch by
    // write_channel_pitch. Set by the engine; table_trigger takes the initial
    // value from TriggerExtras.
    int16_t bend_cents;

    // Per-instrument fine tune in cents (-50..+50). Set at trigger from
    // TriggerExtras.
    int8_t fine_cents;

    // Noise options, set at trigger from TriggerExtras:
    //   bit 0 (kNoiseFixed)  - NR43 never follows pitch (transposes, slides,
    //                          vibrato, bend); only the `N` command changes it.
    //   bit 1 (kNoiseWidth7) - NR43 bit 3 (7-bit LFSR) is OR'd into every write.
    uint8_t noise_flags;

    // 1 when a length counter is in use (TriggerExtras.length > 0). Every NRx4
    // / NR44 write then carries bit 6 (length enable).
    uint8_t length_enable;
    // MPCBOY LOCAL MOD (WP5): set by table_trigger when it received
    // TriggerExtras (the engine path). Selects mpcboy-only behaviour that
    // would change vendored expected values: `W` retriggers CH3.
    uint8_t engine_mode;
};

// Enforce the POD contract at compile time. Same rationale as VoiceState's
// static_asserts in rackboy_voice.h: if someone accidentally adds a
// std::string member, a non-defaulted constructor, or a virtual method, this
// fires at build time rather than producing a silent audio-thread corruption.
static_assert(std::is_trivially_copyable<TableRegState>::value,
              "TableRegState must stay trivially copyable — the audio-thread "
              "bank swap may memcpy arrays of these and any non-trivial member "
              "would silently corrupt that path.");
static_assert(std::is_standard_layout<TableRegState>::value,
              "TableRegState must stay standard-layout so host unit tests and "
              "the Daisy target agree on field offsets / alignment.");

// Reset a TableRegState to its power-on / zero state. Called at module
// construction, on bank swap-in, and whenever a channel's instrument pool
// transitions to empty.
inline void table_reg_state_reset(TableRegState& r) noexcept
{
    r.freq_11bit  = 0;
    r.envelope    = 0;
    r.duty        = 0;
    r.sweep       = 0;
    r.nr43_shape  = 0;
    r.wave_volume = 0;
    r.pitch_offset = 0;
    r.bend_cents     = 0;  // MPCBOY LOCAL MOD
    r.fine_cents     = 0;
    r.noise_flags    = 0;
    r.length_enable  = 0;
    r.engine_mode    = 0;
}

// MPCBOY LOCAL MOD: NRx4 bit 6 (length enable) and TableRegState::noise_flags.
constexpr uint8_t kLengthEnableBit = 0x40;
constexpr uint8_t kNoiseFixed      = 0x01;
constexpr uint8_t kNoiseWidth7     = 0x02;

// MPCBOY LOCAL MOD: companion data for table_trigger that does not fit the
// 16-byte RackboyInstrument. Passing nullptr to table_trigger is exactly the
// pre-mpcboy behaviour (no fine tune, no bend, no length, legacy noise rule).
struct TriggerExtras {
    int8_t   fine_cents;   // -50..+50
    int16_t  bend_cents;   // current pitch bend, cents
    uint8_t  noise_flags;  // kNoiseFixed | kNoiseWidth7
    uint8_t  length;       // 0 = off, 1..64 steps (see 02-apu-and-voice.md)
};

// ---------------------------------------------------------------------------
// table_trigger — Gate 1 rising-edge handler (AD #9)
// ---------------------------------------------------------------------------
//
// Called on every Gate 1 rising edge. Unconditionally:
//   1. Resets voice.table_cursor to 0 and voice.table_delay_ticks_remaining
//      to 0 so the next Gate 2 tick begins at row 0.
//   2. Sets voice.triggered = true.
//   3. Populates `regs` from the instrument's raw bytes (channel-specific).
//   4. Computes the initial frequency from `semitones_from_base` using the
//      appropriate pitch helper for the channel.
//   5. Writes the initial NRxx register set to the APU, including the
//      trigger bit (NRx4 bit 7) so the hardware channel restarts cleanly.
//
// Parameters:
//   voice              — mutable VoiceState for this channel (cursor reset here)
//   regs               — mutable per-voice register cache (populated here)
//   ch                 — which of the four GB audio channels this voice is
//   inst               — the instrument selected for this trigger event
//   semitones_from_base — pitch offset from C5/MIDI 72 (VoiceState::target_pitch)
//   apu                — APU driver to write registers into
void table_trigger(
    rackboy::core::VoiceState& voice,
    TableRegState& regs,
    rackboy::core::VoiceChannel ch,
    const rackboy::sav::RackboyInstrument& inst,
    int16_t semitones_from_base,
    rackboy_apu_t* apu,
    const TriggerExtras* extras = nullptr) noexcept;  // MPCBOY LOCAL MOD: extras

// MPCBOY LOCAL MOD: rewrite the channel's frequency registers (no trigger bit)
// from regs + the voice's current vibrato phase, using the same pitch
// composition as table_tick. For legato glide and pitch bend between ticks.
void table_write_pitch(
    const rackboy::core::VoiceState& voice,
    TableRegState& regs,
    rackboy::core::VoiceChannel ch,
    rackboy_apu_t* apu) noexcept;

// ---------------------------------------------------------------------------
// table_tick — Gate 2 rising-edge handler (AD #10)
// ---------------------------------------------------------------------------
//
// Called on every Gate 2 rising edge. Advances the table cursor by one row
// (wrapping at kTableRowCount so LSDJ tables loop), applies the row's
// transpose offset to the running pitch, and dispatches the row's command
// slots to the (currently stubbed) command dispatcher.
//
// Delay semantics: if voice.table_delay_ticks_remaining > 0, the cursor does
// NOT advance — the counter is decremented and the function returns early.
// The delay counter is set by the `D` (delay) table command, which is tracked
// by a separate bead and will be wired into dispatch_command() later.
//
// Frequency writes from ticks intentionally omit the trigger bit (NRx4
// bit 7) so that pitch glides and table-driven modulation do not reset
// the volume envelope on every tick.
//
// Parameters:
//   voice  — mutable VoiceState for this channel (cursor advanced here)
//   regs   — mutable per-voice register cache (pitch_offset updated here)
//   ch     — which of the four GB audio channels this voice is
//   table  — the LSDJ table currently assigned to this voice
//   apu    — APU driver to write registers into
//   bank   — read-only pointer to the active RackboyBank, used by commands
//            that need access to shared pools (e.g. the `W` wave-set command
//            reads a frame out of `bank->waves[]`). Production callers should
//            pass the active bank; `nullptr` is reserved for tests that don't
//            exercise any pool-reading command and for genuinely bank-less
//            call sites. Pool-reading handlers degrade to no-ops in three
//            cases: (1) `bank == nullptr` (no pool context), (2) the param
//            selects an out-of-range slot (e.g. `W`'s param >= kMaxWaveFrames),
//            or (3) the selected slot is not `occupied`. In all three cases
//            the dispatcher never dereferences a null or out-of-range pointer.
void table_tick(
    rackboy::core::VoiceState& voice,
    TableRegState& regs,
    rackboy::core::VoiceChannel ch,
    const rackboy::sav::RackboyTable& table,
    rackboy_apu_t* apu,
    const rackboy::sav::RackboyBank* bank) noexcept;

// ---------------------------------------------------------------------------
// Chord command deferral — docs/DECISIONS.md D-001 (bead rackboy-u4j)
// ---------------------------------------------------------------------------
//
// The LSDJ `C` (chord) command is deferred to v1.1; at runtime the
// dispatcher treats it as a no-op (see the `case 'C'` arm in
// `dispatch_command`). D-001 requires callers to additionally emit a
// single load-time warning line per instrument that references it, so
// users are told about the intentional behavioral divergence rather
// than silently losing arpeggiations at play time. The scan + format
// helpers below are the canonical path for producing those warnings
// from a freshly parsed `RackboyBank`.
//
// Both helpers are `noexcept`, allocate nothing, and do not touch the
// audio thread's SPSC ring — they are intended to run off-thread (UI
// thread on VCV, the post-load section of the Daisy SD loader) once
// per load cycle.

// The LSDJ cmd-byte for `C` (chord). Promoted from an ASCII literal to
// a named constant so the scanner and the dispatcher agree on the
// exact byte without re-typing the character.
constexpr std::uint8_t kChordCommandByte = 0x43;

// One chord-command reference discovered by `scan_chord_references`.
// Per D-001 the warning is one line *per instrument*, so the scanner
// reports the instrument's coordinates (channel + dense-prefix slot)
// and the table slot it resolved to — the latter is informational,
// useful for power users debugging which table the warning pointed at
// but not printed in the canonical line.
struct ChordReference {
    rackboy::core::VoiceChannel channel;
    std::uint8_t                instrument_slot;  // [0, pool count)
    std::uint8_t                table_slot;       // [0, kMaxTables)
};

// Upper bound on the number of `ChordReference` entries `scan_chord_references`
// can emit for any single bank. Derived from the RackboyBank pool
// layout: one entry per instrument slot × four channels. Callers can
// size their output buffer to this constant and know the scan will
// never truncate.
constexpr std::size_t kMaxChordReferencesPerBank =
    4 * rackboy::sav::kMaxInstrumentsPerChannel;

// Walk every instrument in `bank` (dense-prefix honored per channel)
// and record one `ChordReference` for each instrument whose assigned
// table contains at least one `C` command byte in either command
// slot of any row. The check is per-instrument: an instrument with
// chord on both `cmd1` and `cmd2` of one row, or on several rows,
// still produces a single reference. Instruments with
// `raw[6] == kNoTable`, pulse instruments whose `raw[5]` bit 4 is
// clear (table-enable), or a slot outside `[0, kMaxTables)`, or a
// slot whose `RackboyTable::occupied` flag is false are skipped —
// these cannot feed rows to the dispatcher so there is nothing to
// warn about.
//
// The function always returns the *total* count of chord-using
// instruments found, even when `out_cap` is smaller than that
// count. At most `out_cap` references are written to `out`, in
// deterministic order: Pulse1 ascending by instrument slot, then
// Pulse2, then Wave, then Noise. A caller that has pre-allocated
// `kMaxChordReferencesPerBank` entries is guaranteed never to
// truncate.
//
// Null-pointer contract: passing `out == nullptr` is the
// "count-only" mode and always legal. When `out` is null the
// implementation internally clamps `out_cap` to 0 — i.e., a
// caller that asks for count-only writes but forgets to zero
// `out_cap` still gets defined behavior (no dereference of the
// null pointer, return value is still the correct total). This
// clamp is exercised by the test suite so rewrites of the guard
// stay audible.
std::size_t scan_chord_references(
    const rackboy::sav::RackboyBank& bank,
    ChordReference*                  out,
    std::size_t                      out_cap) noexcept;

// Format D-001's canonical warning line for one `ChordReference`
// into `buf`. Output is null-terminated and never exceeds
// `buf_len - 1` usable bytes. The channel abbreviation is:
//   Pulse1 → "P1"
//   Pulse2 → "P2"
//   Wave   → "W"
//   Noise  → "N"
// The full line format is:
//   [rackboy_table] instrument <CH>-<SLOT> uses chord command — deferred to v1.1, treated as no-op
// The em-dash is the Unicode U+2014 (three UTF-8 bytes 0xE2 0x80 0x94)
// so the line matches the exact D-001 spec byte-for-byte on every
// platform. `<SLOT>` is printed as a decimal integer with no zero
// padding.
//
// Returns the number of bytes that would have been written excluding
// the terminating NUL — identical to `snprintf`'s contract — so the
// caller can detect truncation via `return_value >= buf_len`. If
// `buf_len == 0` the function writes nothing and returns 0 without
// dereferencing `buf`; a non-null `buf` is required whenever
// `buf_len > 0`. No allocation, `noexcept`.
//
// Buffer sizing: the formatted line is at most ~100 bytes (one
// em-dash is 3 bytes in UTF-8, slot up to 2 decimal digits, channel
// up to 2 ASCII chars). Callers should pass at least
// `kChordReferenceLogBufferBytes` to leave headroom without risking
// truncation on any currently-supported ChordReference shape.
std::size_t format_chord_reference(
    const ChordReference& ref,
    char*                 buf,
    std::size_t           buf_len) noexcept;

// Recommended buffer size for `format_chord_reference` output.
// Callers should pass a `char[kChordReferenceLogBufferBytes]` to
// avoid truncation. The current canonical D-001 line is ~100 bytes
// (the em-dash is 3 bytes in UTF-8); 128 leaves ~25 bytes of
// headroom so a future format extension (e.g., an instrument-name
// suffix from `RackboyBankNames`) does not force every caller to
// resize in lockstep. Used by both the daisy USB-CDC logger
// (rackboy_errors.cpp) and the VCV Rack WARN path (Rackboy.hpp);
// hard-coding 128 at call sites is a drift hazard now that there
// are two production consumers.
constexpr std::size_t kChordReferenceLogBufferBytes = 128;

}  // namespace table
}  // namespace rackboy

#endif  // RACKBOY_TABLE_RACKBOY_TABLE_H_
