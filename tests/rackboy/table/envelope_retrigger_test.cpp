// tests/table/envelope_retrigger_test.cpp
//
// Integration tests for the E (envelope retrigger) table command
// (bead rackboy-qz3).
//
// Per docs/LSDJ_REFERENCE.md, the E command writes its parameter byte
// into the channel's volume/envelope register:
//   CH1/CH2/CH4 — NRx2 (direct envelope byte)
//   CH3         — NR32 (output level; NRx2 doesn't exist on wave)
//
// rackboy's E command ALSO sets the NRx4 trigger bit so the channel
// re-arms with the new envelope (the "retrigger" in the bead title).
// The current freq_11bit is preserved in NRx4 bits 0-2.
//
// Verifies:
//   - E on CH1/CH2/CH4 updates regs.envelope AND writes NRx2 + NRx4 trigger
//   - E on CH3 updates regs.wave_volume AND writes NR32 (masked to bits 6-5)
//     + NR34 trigger
//   - NRx4 trigger bit is set; freq high bits preserved from regs.freq_11bit
//   - CH4 NR44 holds only the trigger bit (no freq bits exist on NR44)

#include "doctest/doctest.h"

#include "rackboy_table.h"
#include "rackboy_voice.h"
#include "rackboy_pitch.h"
#include "rackboy_apu.h"
#include "rackboy_sav_types.h"

#include <cstring>

using rackboy::core::VoiceChannel;
using rackboy::core::VoiceState;
using rackboy::core::voice_reset;
using rackboy::table::TableRegState;
using rackboy::table::table_reg_state_reset;
using rackboy::table::table_trigger;
using rackboy::table::table_tick;

static rackboy_apu_t make_apu()
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    return apu;
}

static rackboy::sav::RackboyInstrument make_pulse_inst(uint8_t envelope,
                                                        uint8_t duty)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypePulse;
    inst.raw[1] = envelope;
    inst.raw[7] = duty;
    return inst;
}

static rackboy::sav::RackboyInstrument make_wave_inst(uint8_t volume_bits)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeWave;
    inst.raw[1] = volume_bits;
    return inst;
}

static rackboy::sav::RackboyInstrument make_noise_inst(uint8_t envelope,
                                                        uint8_t shape)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeNoise;
    inst.raw[1] = envelope;
    inst.raw[4] = shape;
    return inst;
}

static rackboy::sav::RackboyTable make_table_with_e_cmd(uint8_t param)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 0;
    tbl.rows[0].cmd1 = 'E';
    tbl.rows[0].cmd1_param = param;
    return tbl;
}

TEST_CASE("E on Pulse1: writes NR12 and retriggers via NR14")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    CHECK_EQ(regs.envelope, 0xF0);

    // E with a new envelope byte (vol 5, down, period 3).
    auto tbl = make_table_with_e_cmd(0x53);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    CHECK_EQ(regs.envelope, 0x53);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR12], 0x53);

    // NR14 must have trigger bit set and preserve freq high bits.
    const uint8_t nr14 = apu.gb.io_registers[GB_IO_NR14];
    CHECK_EQ(nr14 & 0x80, 0x80);  // trigger bit set
    CHECK_EQ(nr14 & 0x07, (regs.freq_11bit >> 8) & 0x07);
}

TEST_CASE("E on Pulse2: writes NR22 and retriggers via NR24")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xA0, 0xC0);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse2, inst, 0, &apu);

    auto tbl = make_table_with_e_cmd(0x77);
    table_tick(voice, regs, VoiceChannel::Pulse2, tbl, &apu, /*bank=*/nullptr);

    CHECK_EQ(regs.envelope, 0x77);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR22], 0x77);

    const uint8_t nr24 = apu.gb.io_registers[GB_IO_NR24];
    CHECK_EQ(nr24 & 0x80, 0x80);
    CHECK_EQ(nr24 & 0x07, (regs.freq_11bit >> 8) & 0x07);
}

TEST_CASE("E on Wave: writes NR32 (output level) and retriggers via NR34")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst(0x40);  // initial output level 100%
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);
    CHECK_EQ(regs.wave_volume, 0x40);

    // E with a full param byte (0x2F). Bits 6-5 = 0b01 = 100% output
    // level; the extra low bits (0x0F) are included to verify that
    // NR32 is masked to bits 6-5 while regs.wave_volume caches the
    // full byte.
    auto tbl = make_table_with_e_cmd(0x2F);
    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, /*bank=*/nullptr);

    // regs cache stores full param byte.
    CHECK_EQ(regs.wave_volume, 0x2F);
    // NR32 register receives only bits 6-5 (other bits masked out).
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR32], 0x20);

    const uint8_t nr34 = apu.gb.io_registers[GB_IO_NR34];
    CHECK_EQ(nr34 & 0x80, 0x80);
    CHECK_EQ(nr34 & 0x07, (regs.freq_11bit >> 8) & 0x07);
}

TEST_CASE("E on Noise: writes NR42 and retriggers via NR44 (trigger-only)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_noise_inst(0xF0, 0x30);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);

    auto tbl = make_table_with_e_cmd(0x65);
    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);

    CHECK_EQ(regs.envelope, 0x65);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR42], 0x65);

    // NR44 holds only the trigger bit (bit 7). No freq bits exist there.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR44], 0x80);
}

TEST_CASE("E preserves freq high bits from regs.freq_11bit (Pulse1)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    // Trigger at +24 semitones to produce a non-trivial freq hi.
    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 24, &apu);
    const uint16_t freq_before = regs.freq_11bit;
    // Precondition: freq_before must be non-zero so the "unchanged"
    // check below isn't satisfied vacuously.
    REQUIRE(freq_before != 0);

    auto tbl = make_table_with_e_cmd(0x41);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    // pitch_offset should still be 24 (E doesn't touch pitch).
    CHECK_EQ(regs.pitch_offset, 24);
    // freq_11bit unchanged (E doesn't change freq; table_tick only
    // re-runs pitch_square_11bit(pitch_offset) which for same input
    // returns the same output).
    CHECK_EQ(regs.freq_11bit, freq_before);

    const uint8_t nr14 = apu.gb.io_registers[GB_IO_NR14];
    CHECK_EQ(nr14 & 0x80, 0x80);  // trigger
    CHECK_EQ(nr14 & 0x07, (freq_before >> 8) & 0x07);
}

TEST_CASE("E's NR34 trigger overrides table_tick pitch-path NR34 write")
{
    // table_tick() order: (Step 4) write NRx3/NRx4 WITHOUT trigger bit,
    // then (Step 5) dispatch_command (which, for `E`, writes NRx4 WITH
    // trigger bit). This test pins the "E wins over pitch path on
    // NRx4" invariant so a future reorder of table_tick's steps gets
    // caught.
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst(0x40);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    // Trigger at +12 to get non-trivial freq_hi bits.
    table_trigger(voice, regs, VoiceChannel::Wave, inst, 12, &apu);

    // Build a row with BOTH a transpose AND the E command — the pitch
    // path will write NR34 (no trigger) and then E will overwrite it
    // (with trigger) later in the same tick.
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 3;
    tbl.rows[0].cmd1 = 'E';
    tbl.rows[0].cmd1_param = 0x40;

    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, /*bank=*/nullptr);

    // After the tick, NR34 must have the trigger bit set (E's write
    // wins over the pitch path's no-trigger write).
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR34] & 0x80, 0x80);
    // And freq high bits reflect the post-transpose pitch (pitch path
    // runs BEFORE E, so regs.freq_11bit reflects pitch_offset = 12 + 3 = 15).
    CHECK_EQ(regs.pitch_offset, 15);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR34] & 0x07,
             (regs.freq_11bit >> 8) & 0x07);
}
