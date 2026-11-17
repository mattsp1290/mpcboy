// tests/table/sweep_test.cpp
//
// Integration tests for the S (sweep) table command (bead rackboy-5yc).
//
// Verifies:
//   - S command on CH1 writes param to NR10 and updates regs.sweep
//   - S command on non-CH1 channels is a no-op (hardware NR10 unchanged)
//   - S command on a row with transpose leaves freq writes untouched
//     (S only affects NR10; pitch writes to NR13/NR14 still happen)

#include "doctest/doctest.h"

#include "rackboy_table.h"
#include "rackboy_voice.h"
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

// Helper: create a zeroed APU at 48 kHz.
static rackboy_apu_t make_apu()
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    return apu;
}

// Helper: build a pulse instrument with envelope, duty, and sweep bytes.
static rackboy::sav::RackboyInstrument make_pulse_inst(uint8_t envelope,
                                                        uint8_t duty,
                                                        uint8_t sweep)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypePulse;
    inst.raw[1] = envelope;
    inst.raw[4] = sweep;
    inst.raw[7] = duty;
    return inst;
}

// Helper: build a wave instrument for wrong-channel tests.
static rackboy::sav::RackboyInstrument make_wave_inst(uint8_t volume_bits)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeWave;
    inst.raw[1] = volume_bits;
    return inst;
}

// Helper: build a noise instrument for wrong-channel tests.
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

// Helper: build a minimal table with an S command on row 0.
static rackboy::sav::RackboyTable make_table_with_s_cmd(uint8_t param)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 0;
    tbl.rows[0].cmd1 = 'S';
    tbl.rows[0].cmd1_param = param;
    return tbl;
}

TEST_CASE("S command on CH1 writes param to NR10 and updates regs.sweep")
{
    rackboy_apu_t apu = make_apu();

    auto inst = make_pulse_inst(0xF0, 0x80, 0x00);  // no initial sweep
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Sanity: trigger set sweep to 0 (the instrument's raw[4]).
    CHECK_EQ(regs.sweep, 0x00);

    // S command with a non-zero sweep byte.
    // Bits 0-2 = shift (3), bit 3 = direction (1 = decrease),
    // bits 4-6 = period (5). 0b0101_1011 = 0x5B.
    auto tbl = make_table_with_s_cmd(0x5B);

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    // After the S command, regs.sweep holds the new byte.
    CHECK_EQ(regs.sweep, 0x5B);

    // Verify the hardware NR10 register was actually written.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], 0x5B);
}

TEST_CASE("S command on non-CH1 channels is a no-op")
{
    // Test Pulse2 (no sweep hardware, no-op expected).
    {
        rackboy_apu_t apu = make_apu();
        auto inst = make_pulse_inst(0xF0, 0x80, 0x00);
        VoiceState voice;
        voice_reset(voice);
        TableRegState regs;
        table_reg_state_reset(regs);

        table_trigger(voice, regs, VoiceChannel::Pulse2, inst, 0, &apu);

        const uint8_t nr10_before = apu.gb.io_registers[GB_IO_NR10];
        const uint8_t regs_sweep_before = regs.sweep;

        auto tbl = make_table_with_s_cmd(0x5B);
        table_tick(voice, regs, VoiceChannel::Pulse2, tbl, &apu, /*bank=*/nullptr);

        CHECK_EQ(regs.sweep, regs_sweep_before);
        CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], nr10_before);
    }

    // Test Wave (CH3) — different instrument type, no sweep.
    {
        rackboy_apu_t apu = make_apu();
        auto inst = make_wave_inst(0x20);
        VoiceState voice;
        voice_reset(voice);
        TableRegState regs;
        table_reg_state_reset(regs);

        table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

        const uint8_t nr10_before = apu.gb.io_registers[GB_IO_NR10];

        auto tbl = make_table_with_s_cmd(0x5B);
        table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, /*bank=*/nullptr);

        CHECK_EQ(regs.sweep, 0);
        CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], nr10_before);
    }

    // Test Noise (CH4) — no sweep hardware either.
    {
        rackboy_apu_t apu = make_apu();
        auto inst = make_noise_inst(0xF0, 0x2C);
        VoiceState voice;
        voice_reset(voice);
        TableRegState regs;
        table_reg_state_reset(regs);

        table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);

        const uint8_t nr10_before = apu.gb.io_registers[GB_IO_NR10];
        const uint8_t regs_sweep_before = regs.sweep;

        auto tbl = make_table_with_s_cmd(0x5B);
        table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);

        CHECK_EQ(regs.sweep, regs_sweep_before);
        CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], nr10_before);
    }
}

TEST_CASE("S command on CH1 with same-row transpose: both pitch and sweep applied")
{
    rackboy_apu_t apu = make_apu();

    auto inst = make_pulse_inst(0xF0, 0x80, 0x00);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Row 0: transpose +2 AND S command. Pitch path (Step 4) writes
    // NR13/NR14; S command (Step 5) writes NR10. They target different
    // registers so both take effect independently.
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 2;
    tbl.rows[0].cmd1 = 'S';
    tbl.rows[0].cmd1_param = 0x32;  // period 3, dir 0, shift 2

    const uint16_t freq_before = regs.freq_11bit;

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    // Sweep applied.
    CHECK_EQ(regs.sweep, 0x32);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], 0x32);

    // Pitch applied (freq_11bit changed per transpose +2).
    CHECK_NE(regs.freq_11bit, freq_before);
    CHECK_EQ(regs.pitch_offset, 2);
}

TEST_CASE("S command NR10 value persists across subsequent pitch-only ticks")
{
    rackboy_apu_t apu = make_apu();

    auto inst = make_pulse_inst(0xF0, 0x80, 0x00);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Row 0: S command sets NR10. Row 1: plain transpose (no command).
    // Pitch path writes NR13/NR14 only, so NR10 should stay at S's value.
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 0;
    tbl.rows[0].cmd1 = 'S';
    tbl.rows[0].cmd1_param = 0x5B;
    tbl.rows[1].transpose = 1;
    tbl.rows[1].cmd1 = 0;

    // Row 0 — S fires.
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], 0x5B);
    CHECK_EQ(regs.sweep, 0x5B);

    // Row 1 — pitch-only, NR10 must persist.
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], 0x5B);
    CHECK_EQ(regs.sweep, 0x5B);
}

TEST_CASE("S 00 disables sweep: both regs.sweep and NR10 go to 0")
{
    rackboy_apu_t apu = make_apu();

    // Instrument has a non-zero baseline sweep — trigger writes it to NR10.
    auto inst = make_pulse_inst(0xF0, 0x80, 0x37);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    CHECK_EQ(regs.sweep, 0x37);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], 0x37);

    // S 00 — standard LSDJ idiom to disable the sweep unit mid-row
    // (NR10 period=0 disables sweep per GB hardware).
    auto tbl = make_table_with_s_cmd(0x00);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    CHECK_EQ(regs.sweep, 0x00);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], 0x00);
}
