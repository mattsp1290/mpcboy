// rackboy_core/rackboy_voice.cpp
//
// Implementation of the per-voice reset helpers declared in
// rackboy_voice.h. Intentionally trivial — the struct is pure data and all
// behavior that matters lives in the later beads (bank copy-in, table
// interpreter, pitch mapping, control hysteresis). Kept in a .cpp file
// rather than inline in the header so future additions (e.g. validated
// setters) have a natural home that does not force a full rebuild of every
// translation unit that includes the header.

#include "rackboy_voice.h"

namespace rackboy {
namespace core {

void voice_reset(VoiceState& v) noexcept
{
    // Voice selection + cursor + base pitch.
    v.active_instrument_index      = kNoInstrument;
    v.table_cursor                 = 0;
    v.table_delay_ticks_remaining  = 0;
    v.target_pitch                 = 0;
    v.triggered                    = false;
    // Recurring retrigger state (bead rackboy-bgu).
    v.retrigger_period_ticks       = 0;
    v.retrigger_counter_ticks      = 0;
    // Vibrato LFO state (bead rackboy-00h).
    v.vibrato_speed                = 0;
    v.vibrato_depth                = 0;
    v.vibrato_phase                = 0;
    // Pitch-slide state (bead rackboy-h2z).
    v.slide_target_pitch           = 0;
    v.slide_ticks_remaining        = 0;
}

void voices_reset_all(VoiceState (&voices)[kNumVoices]) noexcept
{
    // Range-for is safe here: the parameter is a reference-to-array of
    // exact size kNumVoices, so `auto& v : voices` can't pointer-decay or
    // over-/under-read the array.
    for (auto& v : voices) {
        voice_reset(v);
    }
}

}  // namespace core
}  // namespace rackboy
