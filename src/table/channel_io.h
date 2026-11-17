// channel_io.h — mpcboy-owned register helpers for the table interpreter.
//
// These used to live inside the vendored rackboy_table.cpp as MPCBOY LOCAL MOD
// code. They sit here so the vendored file keeps only call sites (see
// src/VENDORED.md). One register descriptor replaces the per-channel switches;
// "retrigger with the running frequency" is one function.
#pragma once

#include <cstdint>

#include "rackboy_table.h"
#include "rackboy_voice.h"

namespace rackboy {
namespace table {

struct ChannelRegs {
    uint16_t nrx1;  // length / duty (NR30 is the wave DAC power, NR31 its length)
    uint16_t nrx2;  // envelope (wave: NR32 output level)
    uint16_t nrx3;  // frequency low (noise: NR43)
    uint16_t nrx4;  // frequency high + trigger + length enable
};

constexpr ChannelRegs kChannelRegs[4] = {
    {kNR11, kNR12, kNR13, kNR14},
    {kNR21, kNR22, kNR23, kNR24},
    {kNR31, kNR32, kNR33, kNR34},
    {kNR41, kNR42, kNR43, kNR44},
};

static_assert(static_cast<int>(core::VoiceChannel::Pulse1) == 0 &&
              static_cast<int>(core::VoiceChannel::Pulse2) == 1 &&
              static_cast<int>(core::VoiceChannel::Wave) == 2 &&
              static_cast<int>(core::VoiceChannel::Noise) == 3,
              "kChannelRegs is indexed by VoiceChannel");

inline const ChannelRegs& channel_regs(core::VoiceChannel ch) noexcept
{
    return kChannelRegs[static_cast<int>(ch)];
}

// NR43 bit 3 when the noise 7-bit width flag is set, else 0.
uint8_t noise_width_bits(const TableRegState& regs) noexcept;

// NRx4 byte with the trigger bit (+ length enable). Noise has no frequency bits.
uint8_t trigger_byte(const TableRegState& regs, core::VoiceChannel ch) noexcept;

// Reload the configured length (when length is enabled) and write NRx4 with the
// trigger bit and the running frequency.
void retrigger(const TableRegState& regs, core::VoiceChannel ch, rackboy_apu_t* apu) noexcept;

// `E` command / table volume column: store the envelope (wave: NR32 & 0x60),
// then retrigger.
void write_envelope_retrigger(uint8_t value, TableRegState& regs, core::VoiceChannel ch,
                              rackboy_apu_t* apu) noexcept;

// `K`: silence the channel (NRx2 = 0, wave NR30 = 0), clear triggered and any
// pending delayed kill.
void kill_channel(core::VoiceState& voice, TableRegState& regs, core::VoiceChannel ch,
                  rackboy_apu_t* apu) noexcept;

// The single pitch path: composes cents, writes NRx3/NRx4 (with the trigger bit
// when `trigger`). Reads regs.engine_mode (set by table_trigger).
void write_channel_pitch(rackboy_apu_t* apu, core::VoiceChannel ch, TableRegState& regs,
                         int16_t vibrato_semis, int16_t pbend_accum, bool trigger) noexcept;

}  // namespace table
}  // namespace rackboy
