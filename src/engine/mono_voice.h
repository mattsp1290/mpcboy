// src/engine/mono_voice.h - one monophonic voice driving all four GB channels.
// WP4 (D4): note stack with last-note priority, trigger / legato glide, pitch
// bend, table ticks. Host-agnostic; talks to the APU only through
// rackboy_apu_write_reg / rackboy_apu_write_wave_ram.
#pragma once

#include <cstdint>

#include "channel_settings.h"
#include "note_stack.h"
#include "rackboy_sav_types.h"
#include "rackboy_table.h"
#include "rackboy_voice.h"
#include "table_bank.h"

namespace mpcboy {

class MonoVoice
{
public:
    // `apu` must outlive the voice. `waves` points at 16 wave slots of 16
    // bytes (32 packed 4-bit samples), or is nullptr (then no wave RAM write
    // happens before a Wave trigger).
    void init(rackboy_apu_t* apu, const uint8_t (*waves)[kWaveBytes]);

    // Clear note stack, voice/table state and bend. Does not touch the APU.
    void reset();

    // Settings changes apply at the next trigger, except pan (NR51 at once)
    // and enabled = false (DAC off at once).
    void applySettings(const EngineSettings& s);

    // Returns true when the channels were (re)triggered, false for a legato
    // glide. `bank` supplies tables for tick 0.
    bool noteOn(int note, int velocity, const rackboy::sav::RackboyBank& bank);
    void noteOff(int note);
    void allNotesOff();
    void pitchBend(int bend14);   // -8192..8191

    // One table tick for every sounding channel that has a table. No-op when
    // nothing is ticking (idle, or after a Cut note-off).
    void tick(const rackboy::sav::RackboyBank& bank);

    // True once after a Cut note-off / allNotesOff silenced the channels (the
    // engine uses it to flush the DC blocker's tail; see Engine).
    bool takeSilenced() { const bool b = silenced_; silenced_ = false; return b; }

    bool ticking() const { return ticking_; }
    bool holding() const { return !stack_.empty(); }
    int bendCents() const { return bendCents_; }
    const rackboy::table::TableRegState& regs(int ch) const { return regs_[ch]; }
    const rackboy::core::VoiceState& voice(int ch) const { return voices_[ch]; }

private:
    void trigger(int note, int velocity, const rackboy::sav::RackboyBank& bank);
    void glide(int oldNote, int newNote);
    void silence();
    void dacOff(int ch);
    void writeNr51();
    void updateBendCents();
    void rewriteAllPitch();

    rackboy_apu_t* apu_ = nullptr;
    const uint8_t (*waves_)[kWaveBytes] = nullptr;
    EngineSettings s_;
    NoteStack stack_;
    rackboy::core::VoiceState voices_[4];
    rackboy::table::TableRegState regs_[4];
    int8_t tableSlot_[4] = {-1, -1, -1, -1};   // table in use per channel since the last trigger
    bool ticking_ = false;
    bool silenced_ = false;
    bool triggeredSinceReset_ = false;         // NR51 stays 0 until a note
    int bendRaw_ = 0;
    int bendCents_ = 0;
    uint8_t lastNr51_ = 0;
};

}  // namespace mpcboy
