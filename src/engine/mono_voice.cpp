#include "mono_voice.h"

#include <algorithm>

#include "channel_io.h"
#include "engine_consts.h"
#include "instrument_encoder.h"
#include "rackboy_apu.h"
#include "rackboy_pitch.h"

namespace mpcboy {

using rackboy::core::VoiceChannel;
namespace tbl = rackboy::table;

namespace {

constexpr VoiceChannel kChannels[4] = {VoiceChannel::Pulse1, VoiceChannel::Pulse2,
                                       VoiceChannel::Wave, VoiceChannel::Noise};

}  // namespace

void MonoVoice::init(rackboy_apu_t* apu)
{
    apu_ = apu;
    s_ = EngineSettings();
    reset();
}

void MonoVoice::reset()
{
    stack_.clear();
    rackboy::core::voices_reset_all(voices_);
    for (int i = 0; i < 4; ++i)
    {
        tbl::table_reg_state_reset(regs_[i]);
        tableSlot_[i] = -1;
    }
    ticking_ = false;
    silenced_ = false;
    triggeredSinceReset_ = false;
    bendRaw_ = 0;
    bendCents_ = 0;
    lastNr51_ = 0;
}

void MonoVoice::dacOff(int ch)
{
    tbl::kill_channel(voices_[ch], regs_[ch], kChannels[ch], apu_);
}

void MonoVoice::silence()
{
    for (int i = 0; i < 4; ++i) dacOff(i);
    ticking_ = false;
    silenced_ = true;
}

void MonoVoice::writeNr51()
{
    uint8_t v = 0;
    for (int i = 0; i < 4; ++i)
        if (s_.ch[i].enabled) v = static_cast<uint8_t>(v | pan_nr51_bits(i, s_.ch[i].pan));
    if (v != lastNr51_)
    {
        rackboy_apu_write_reg(apu_, kNR51, v);
        lastNr51_ = v;
    }
}

void MonoVoice::updateBendCents()
{
    const int num = bendRaw_ * static_cast<int>(s_.bendRange) * 100;
    bendCents_ = (num >= 0 ? num + 4096 : num - 4096) / 8192;   // round to nearest
}

void MonoVoice::rewriteAllPitch()
{
    for (int i = 0; i < 4; ++i)
    {
        if (!voices_[i].triggered) continue;
        regs_[i].bend_cents = static_cast<int16_t>(bendCents_);
        tbl::table_write_pitch(voices_[i], regs_[i], kChannels[i], apu_);
    }
}

void MonoVoice::applySettings(const EngineSettings& s)
{
    const EngineSettings old = s_;
    s_ = s;
    for (int i = 0; i < 4; ++i)
        if (old.ch[i].enabled && !s_.ch[i].enabled && voices_[i].triggered) dacOff(i);
    if (triggeredSinceReset_) writeNr51();
    if (old.bendRange != s_.bendRange)
    {
        updateBendCents();
        rewriteAllPitch();
    }
}

void MonoVoice::trigger(int note, int velocity, const rackboy::sav::RackboyBank& bank)
{
    triggeredSinceReset_ = true;
    for (int i = 0; i < 4; ++i)
    {
        const VoiceChannel ch = kChannels[i];
        ChannelSettings cs = s_.ch[i];
        if (!cs.enabled)
        {
            voices_[i].triggered = false;
            tableSlot_[i] = -1;
            continue;
        }
        if (s_.velToVol)
        {
            const int vel = std::clamp<int>(velocity, 1, 127);
            if (ch == VoiceChannel::Wave)
            {
                // Four discrete levels: quantise level * vel / 127.
                const int pct = waveLevelPercent(cs.waveLevel);
                if (pct > 0)
                    cs.waveLevel = waveLevelFromPercent(pct * vel / 127);
            }
            else if (cs.volume > 0)
            {
                const int v = (static_cast<int>(cs.volume) * vel + 63) / 127;   // rounded
                cs.volume = static_cast<uint8_t>(std::clamp<int>(v, 1, 15));
            }
        }
        if (ch == VoiceChannel::Wave)
            rackboy_apu_write_wave_ram(apu_, bank.waves[std::clamp<int>(cs.waveSlot, 0, kNumWaves - 1)].samples);

        const rackboy::sav::RackboyInstrument inst = encode_instrument(ch, cs);
        const tbl::TriggerExtras extras = encode_trigger_extras(ch, cs, bendCents_);
        const int semis = note - rackboy::core::kBaseMidiNote + cs.transpose;
        tbl::table_trigger(voices_[i], regs_[i], ch, inst, static_cast<int16_t>(semis), apu_, &extras);

        const int slot = cs.tableSlot;
        const bool hasTable = slot >= 0 && slot < static_cast<int>(rackboy::sav::kMaxTables) &&
                              bank.tables[slot].occupied;
        tableSlot_[i] = hasTable ? static_cast<int8_t>(slot) : static_cast<int8_t>(-1);
        if (hasTable)   // tick 0: applies row 0 at the note-on itself
            tbl::table_tick(voices_[i], regs_[i], ch, bank.tables[slot], apu_, &bank);
    }
    writeNr51();
    ticking_ = true;
}

void MonoVoice::glide(int oldNote, int newNote)
{
    const int delta = newNote - oldNote;
    if (delta == 0) return;
    for (int i = 0; i < 4; ++i)
    {
        if (!voices_[i].triggered) continue;
        regs_[i].pitch_offset = static_cast<int16_t>(regs_[i].pitch_offset + delta);
        if (voices_[i].slide_ticks_remaining > 0)
            voices_[i].slide_target_pitch = static_cast<int16_t>(voices_[i].slide_target_pitch + delta);
        tbl::table_write_pitch(voices_[i], regs_[i], kChannels[i], apu_);
    }
}

bool MonoVoice::noteOn(int note, int velocity, const rackboy::sav::RackboyBank& bank)
{
    const bool wasHeld = !stack_.empty();
    const int oldNote = wasHeld ? stack_.top() : note;
    stack_.push(note);
    if (!wasHeld || s_.retrigger == RetriggerMode::Always)
    {
        trigger(note, velocity, bank);
        return true;
    }
    glide(oldNote, note);
    return false;
}

void MonoVoice::noteOff(int note)
{
    const bool wasTop = !stack_.empty() && stack_.top() == note;
    const int oldTop = wasTop ? note : 0;
    if (!stack_.remove(note)) return;
    if (!stack_.empty())
    {
        if (wasTop) glide(oldTop, stack_.top());
        return;
    }
    if (s_.noteOff == NoteOffMode::Cut) silence();
    // Ring: change nothing; tables keep ticking.
}

void MonoVoice::allNotesOff()
{
    stack_.clear();
    silence();
}

void MonoVoice::pitchBend(int bend14)
{
    bendRaw_ = std::clamp<int>(bend14, -8192, 8191);
    updateBendCents();
    rewriteAllPitch();
}

void MonoVoice::tick(const rackboy::sav::RackboyBank& bank)
{
    if (!ticking_) return;
    for (int i = 0; i < 4; ++i)
    {
        if (!voices_[i].triggered || tableSlot_[i] < 0) continue;
        const rackboy::sav::RackboyTable& t = bank.tables[tableSlot_[i]];
        if (!t.occupied) continue;
        tbl::table_tick(voices_[i], regs_[i], kChannels[i], t, apu_, &bank);
    }
}

}  // namespace mpcboy
