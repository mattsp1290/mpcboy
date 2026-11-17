// rackboy_core/rackboy_clock.h
//
// Gate 2 tick-all dispatch: on a Gate 2 rising edge, advance every
// active voice's table cursor by one row and apply that row's commands.
// This is the glue between the gate edge detector (rackboy_gate.h) and
// the per-voice table tick function (rackboy_table.h table_tick()).
//
// Bead rackboy-hvz. Architecture Decision #10 binding: "when Gate 2
// has not edged recently, each voice's table cursor freezes at its
// current row. The APU itself continues running on its own clock."
// The caller is responsible for only calling tick_all() on a Gate 2
// rising edge; this module does not observe Gate 2 directly.
//
// For each of the four voices, tick_all():
//   1. Skips voices that have not been triggered (voice.triggered ==
//      false) — an untriggered voice has never had its initial NRxx
//      set, so advancing its table cursor would produce garbage writes.
//   2. Skips voices with no instrument (kNoInstrument) or out-of-bounds
//      indices (same defensive check as trigger_all).
//   3. Looks up the instrument's table slot (raw[6]) from the bank.
//      If the slot is kNoTable, out of range, or the table is not
//      occupied, the voice is table-less — it holds its current
//      register state and the tick is a no-op.
//   4. Otherwise, calls table_tick() with the voice state, register
//      cache, channel, table, APU, and the active bank (the bank is
//      threaded through so commands that read shared pools — e.g. the
//      `W` wave-set command looking up `bank->waves[param]` — have
//      access to them).
//
// No allocations, no STL containers, no virtual methods. All functions
// are `noexcept`. Safe to call from the audio thread.

#ifndef RACKBOY_CORE_RACKBOY_CLOCK_H_
#define RACKBOY_CORE_RACKBOY_CLOCK_H_

#include "rackboy_voice.h"          // VoiceState, VoiceChannel, kNumVoices
#include "rackboy_sav_types.h"      // RackboyBank, kNoTable, kMaxTables
#include "rackboy_table.h"          // TableRegState, table_tick

// Forward declaration — full definition in rackboy_apu/rackboy_apu.h.
extern "C" {
struct rackboy_apu_s;
typedef struct rackboy_apu_s rackboy_apu_t;
}

namespace rackboy {
namespace core {

// tick_all — Gate 2 rising-edge dispatch for all four voices
//
// Called once per Gate 2 rising edge. For each active voice, looks up
// the instrument's table slot and calls table_tick() if a valid,
// occupied table exists. Voices that are untriggered, have no
// instrument, or have no table are silently skipped.
//
// Parameters:
//   voices — mutable array of kNumVoices VoiceState (cursor advanced here)
//   regs   — mutable array of kNumVoices TableRegState (pitch updated here)
//   bank   — the currently active instrument bank (read-only)
//   apu    — APU driver to write registers into
void tick_all(
    VoiceState (&voices)[kNumVoices],
    rackboy::table::TableRegState (&regs)[kNumVoices],
    const sav::RackboyBank& bank,
    rackboy_apu_t* apu) noexcept;

}  // namespace core
}  // namespace rackboy

#endif  // RACKBOY_CORE_RACKBOY_CLOCK_H_
