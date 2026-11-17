// rackboy_core/rackboy_trigger.h
//
// Gate 1 trigger-all dispatch: on a Gate 1 rising edge, unconditionally
// trigger every voice that has an active instrument. This is the glue
// between the gate edge detector (rackboy_gate.h) and the per-voice
// table trigger function (rackboy_table.h table_trigger()).
//
// Bead rackboy-bfl. Architecture Decision #9 binding: "on Gate 1
// rising edge, every channel unconditionally resets its table cursor to
// row 0, reapplies its selected instrument's initial NRxx register set,
// and sets the trigger bit (NRx4 bit 7)."
//
// The dispatch iterates over the four voices (Pulse1, Pulse2, Wave,
// Noise) and, for each voice whose active_instrument_index is valid
// (not kNoInstrument), looks up the instrument from the bank's
// per-channel pool and calls table_trigger(). Voices with no instrument
// are silently skipped — they produce silence and should not write any
// APU registers.
//
// No allocations, no STL containers, no virtual methods. All functions
// are `noexcept`. Safe to call from the audio thread.

#ifndef RACKBOY_CORE_RACKBOY_TRIGGER_H_
#define RACKBOY_CORE_RACKBOY_TRIGGER_H_

#include "rackboy_voice.h"          // VoiceState, VoiceChannel, kNumVoices
#include "rackboy_sav_types.h"      // RackboyBank
#include "rackboy_table.h"          // TableRegState, table_trigger

// Forward declaration — full definition in rackboy_apu/rackboy_apu.h.
extern "C" {
struct rackboy_apu_s;
typedef struct rackboy_apu_s rackboy_apu_t;
}

namespace rackboy {
namespace core {

// trigger_all — Gate 1 rising-edge dispatch for all four voices
//
// Called once per Gate 1 rising edge. For each of the four channels:
//   1. If the voice has no instrument (active_instrument_index ==
//      kNoInstrument), skip it — an empty pool means silence.
//   2. Otherwise, look up the instrument from the bank's per-channel
//      pool using the voice's active_instrument_index.
//   3. Call table_trigger() with the voice state, register cache,
//      channel enum, instrument, the voice's target_pitch, and the APU.
//
// The caller is responsible for detecting the Gate 1 rising edge (via
// gate_step() from rackboy_gate.h) and only calling trigger_all() when
// the edge fires. This function does not observe Gate 1 directly.
//
// Parameters:
//   voices — mutable array of kNumVoices VoiceState (cursor reset here)
//   regs   — mutable array of kNumVoices TableRegState (populated here)
//   bank   — the currently active instrument bank (read-only)
//   apu    — APU driver to write registers into
void trigger_all(
    VoiceState (&voices)[kNumVoices],
    rackboy::table::TableRegState (&regs)[kNumVoices],
    const sav::RackboyBank& bank,
    rackboy_apu_t* apu) noexcept;

}  // namespace core
}  // namespace rackboy

#endif  // RACKBOY_CORE_RACKBOY_TRIGGER_H_
