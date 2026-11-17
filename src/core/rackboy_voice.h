// rackboy_core/rackboy_voice.h
//
// Per-voice runtime state for the four GB audio channels (Pulse1, Pulse2,
// Wave, Noise). Owns no memory, does no I/O, and performs no allocation —
// this is a pure-data POD module so it can live inside the audio-thread
// section of rackboy_core and on the embedded Daisy target with identical
// semantics.
//
// Part of the per-voice state struct + bank wiring work tracked by the
// rackboy-a8f bead. Later beads layer behavior on top of this state:
//
//   * rackboy-2i8  — audio-thread-owned bank struct + copy-in
//   * rackboy-2yi  — control-to-index hysteresis mapping (knob/CV -> index)
//   * rackboy-7fk  — C5/MIDI 72 base note pitch mapping
//
// The field naming mirrors the bead description; when that bead says
// `target_pitch`, this header uses `target_pitch` so the contract stays
// greppable across beads, code, and close-reasons.

#ifndef RACKBOY_CORE_RACKBOY_VOICE_H_
#define RACKBOY_CORE_RACKBOY_VOICE_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "rackboy_constants.h"  // for kNoInstrument

namespace rackboy {
namespace core {

// Number of LSDJ / GB audio channels. Fixed by hardware — Pulse1, Pulse2,
// Wave, Noise. Not a tunable; any code that loops over voices should use
// this constant rather than an inline literal.
constexpr std::size_t kNumVoices = 4;

// LSDJ tables are 16 rows. Gate 2 rising edges advance the cursor; delay
// commands hold it in place for a specified number of ticks. Declared here
// so VoiceState field ranges are self-documenting.
constexpr std::uint8_t kTableRowCount = 16;

// kNoInstrument lives in rackboy_constants.h so rackboy_control.h can
// reuse it without having to pull in this whole voice header.

// Physical channel ordering matches the SameBoy APU channel index space
// (0..3). Keeping the enum in rackboy_core lets code that has no APU shim
// dependency still name voices without reaching into rackboy_apu/.
enum class VoiceChannel : std::uint8_t {
    Pulse1 = 0,
    Pulse2 = 1,
    Wave   = 2,
    Noise  = 3,
};

// VoiceState — per-channel runtime state for one of the four GB audio
// voices. Pure-data POD: no allocations, no STL containers, no virtuals.
// The audio thread owns a fixed 4-element array of these, one per channel.
//
// Invariants (maintained by voice_reset and the later work loops):
//
//   * active_instrument_index == kNoInstrument when the pool for this
//     channel is empty. The audio path must output silence for that voice.
//   * table_cursor is always in [0, kTableRowCount). Gate 1 rising edge
//     zeroes it per rackboy-lsdj-quad-voice-module.md Tick Semantics #105.
//   * table_delay_ticks_remaining is non-zero only when a delay command is
//     holding the cursor in place. Each Gate 2 rising edge either advances
//     the cursor (when zero) or decrements this counter.
//   * target_pitch is a signed offset in semitones from the C5 / MIDI 72
//     base note (see rackboy-7fk). int16_t gives plenty of headroom; the
//     expected musical range is roughly ±48 semitones.
//   * triggered flips to true on Gate 1 rising edge and latches there
//     until a retrigger or until the channel is silenced. It is not the
//     gate *input* level — it is the voice's own run-state mirror.
//
// Intentionally no user-declared constructors / destructor / copy-or-move
// operators: adding any of them would make VoiceState non-trivial, which
// would break the `is_trivially_copyable_v` static_assert below and
// invalidate the memcpy-ability the audio-thread bank copy-in (rackboy-2i8)
// is going to rely on. Initialize via `voice_reset()`, never via a ctor body.
struct VoiceState {
    // Currently selected instrument index into this channel's pool.
    // kNoInstrument when the pool is empty.
    std::int16_t active_instrument_index;

    // LSDJ table row, [0, kTableRowCount).
    std::uint8_t table_cursor;

    // Remaining ticks that the current delay command is holding the cursor
    // in place. Zero means the cursor is free to advance on the next Gate 2
    // rising edge.
    std::uint8_t table_delay_ticks_remaining;

    // Pitch offset from the C5/MIDI 72 base note, in semitones
    // (see rackboy-7fk).
    std::int16_t target_pitch;

    // Voice run-state mirror (see invariant above).
    bool triggered;

    // Table `R` (retrigger) command state (bead rackboy-bgu). When
    // retrigger_period_ticks is non-zero, the table interpreter fires
    // the channel's trigger bit (NRx4 bit 7) every `retrigger_period_ticks`
    // Gate 2 ticks without touching the pitch or envelope registers.
    // `retrigger_counter_ticks` is the running countdown — each table_tick
    // decrements it, and when it reaches zero the trigger fires and the
    // counter is reset to `retrigger_period_ticks`. A period of zero
    // means retrigger is disabled (including `R00`, which per the LSDJ
    // reference is a no-op); Gate 1 resets both fields to zero so a
    // retrigger from a previous note does not bleed into a new one.
    //
    // Invariant: retrigger_counter_ticks is zero iff
    // retrigger_period_ticks is zero. The R command handler and the
    // retrigger fire both write the two fields together, so the
    // counter can never reach the pre-decrement in table_tick while
    // the period is non-zero — there is no 0→255 underflow path.
    // If a future change ever separates the two fields (e.g., a
    // "retrigger once N ticks from now" one-shot form), it must
    // re-establish this invariant explicitly.
    std::uint8_t retrigger_period_ticks;
    std::uint8_t retrigger_counter_ticks;

    // Table `V` (vibrato) command state (bead rackboy-00h). When
    // vibrato_speed is non-zero, the table interpreter adds a
    // triangle-LFO pitch offset to the frequency write on every
    // Gate 2 tick without mutating `regs.pitch_offset`. Layout per
    // docs/LSDJ_REFERENCE.md (rackboy intends `x` = speed, `y` =
    // depth — the high and low nibbles of the param byte):
    //   vibrato_speed — phase increment per tick, 0 = disabled
    //                   (the V00 no-op form from the LSDJ reference)
    //   vibrato_depth — peak absolute semitone offset applied to
    //                   the frequency write; 0 produces silence of
    //                   vibrato even when speed is non-zero
    //   vibrato_phase — running 8-bit phase accumulator; the top two
    //                   bits select one of four triangle-wave
    //                   quadrants (+up, peak, -down, trough). See
    //                   `vibrato_triangle()` in rackboy_table.cpp
    //                   for the exact shape.
    //
    // Gate 1 resets all three so vibrato from a previous note does
    // not bleed into a new note.
    std::uint8_t vibrato_speed;
    std::uint8_t vibrato_depth;
    std::uint8_t vibrato_phase;

    // Table `L` (pitch slide / portamento) command state (bead
    // rackboy-h2z). When `slide_ticks_remaining` is non-zero, the
    // table interpreter linearly interpolates the running pitch
    // toward `slide_target_pitch` over the remaining ticks — one
    // step per Gate 2 tick. On the last remaining tick, the
    // integer-division step lands exactly on the target.
    //
    // Slide target is stored in the same C5/MIDI-72 semitone offset
    // space as `target_pitch` and `TableRegState::pitch_offset`, so
    // the slide step math is just `(target - current) / remaining`.
    //
    // Invariant: `slide_target_pitch` is meaningful only while
    // `slide_ticks_remaining > 0`. Both fields are zeroed by
    // `voice_reset` and by `table_trigger` so a slide from a prior
    // note cannot bleed into a new Gate 1 trigger.
    std::int16_t slide_target_pitch;
    std::uint8_t slide_ticks_remaining;
};

// Enforce the POD contract at compile time. These two traits together are
// the modern-C++ replacement for the deprecated "POD" label the comments
// above use for shorthand: trivially copyable lets the bank copy-in
// (rackboy-2i8) memcpy the whole struct; standard layout pins the field
// order so host tests and the Daisy target agree on the ABI. If someone
// adds a non-trivial member (std::string, a non-defaulted ctor, a
// non-trivial member of a member, ...), this fires at compile time — long
// before a mysterious audio-thread bug shows up at runtime. See
// ~/.claude/research/cpp-pod-trivially-copyable-audio.md for context.
static_assert(std::is_trivially_copyable<VoiceState>::value,
              "VoiceState must stay trivially copyable — the audio-thread bank "
              "copy-in memcpys it and any non-trivial member would silently "
              "corrupt that path.");
static_assert(std::is_standard_layout<VoiceState>::value,
              "VoiceState must stay standard-layout so host unit tests and the "
              "Daisy target agree on field offsets / alignment.");
// Size canary. VoiceState grows by small amounts as new table commands
// land (each P0 command tends to add 0-3 uint8_t counter/phase fields).
// The bound below is intentionally loose — the point is not to cap
// growth, but to make accidental growth LOUD at compile time so a
// reviewer sees it and can decide whether the new field is necessary.
// Bump this upward whenever a new bead legitimately adds state; do NOT
// relax it to `<= sizeof(VoiceState)` (that defeats the purpose).
//
// As of rackboy-h2z (pitch slide), sizeof(VoiceState) == 16, exactly at
// the bound — the next command to add state will need to either bump
// this bound or re-pack existing fields. `bool triggered` is the
// obvious bit-pack candidate if a future iteration wants to keep the
// 16-byte budget intact.
static_assert(sizeof(VoiceState) <= 16,
              "VoiceState growth — audit the new field before bumping this "
              "bound. Audio-thread bank copy-in memcpys one of these per "
              "voice per bank swap, so byte budget is load-bearing.");

// Reset a single voice to its power-on state: no instrument selected,
// cursor at row 0, not triggered. Called at module construction, on every
// bank swap-in, and whenever a channel's pool transitions to empty.
void voice_reset(VoiceState& v) noexcept;

// Reset every voice in a fixed kNumVoices-element array. The reference
// parameter deliberately binds to an array of the exact size so callers
// cannot accidentally pass a shorter buffer.
void voices_reset_all(VoiceState (&voices)[kNumVoices]) noexcept;

}  // namespace core
}  // namespace rackboy

#endif  // RACKBOY_CORE_RACKBOY_VOICE_H_
