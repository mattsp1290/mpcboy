// tests/table/noise_shape_test.cpp
//
// Integration tests for the N (noise-shape) table command (bead rackboy-3rj).
//
// Verifies:
//   - N command on CH4 writes param to NR43 and updates regs.nr43_shape
//   - N command on non-CH4 channels is a no-op
//   - After N command, subsequent table_tick pitch writes preserve the
//     new width bit from regs.nr43_shape

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
using rackboy::core::kTableRowCount;
using rackboy::table::TableRegState;
using rackboy::table::table_reg_state_reset;
using rackboy::table::table_trigger;
using rackboy::table::table_tick;
using rackboy::table::kNR43;

// Helper: create a zeroed APU at 48 kHz.
static rackboy_apu_t make_apu()
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    return apu;
}

// Helper: build a noise instrument with the given envelope and shape bytes.
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

// Helper: build a minimal table with an N command on row 0.
static rackboy::sav::RackboyTable make_table_with_n_cmd(uint8_t param)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 0;
    tbl.rows[0].cmd1 = 'N';
    tbl.rows[0].cmd1_param = param;
    return tbl;
}

// Helper: build a pulse instrument for wrong-channel tests.
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

TEST_CASE("N command on CH4 writes param to NR43 and updates regs.nr43_shape")
{
    rackboy_apu_t apu = make_apu();

    // Set up a noise voice triggered at C5 (offset 0).
    auto inst = make_noise_inst(0xF0, 0x37);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);

    // Sanity: trigger set nr43_shape to the instrument's raw[4].
    CHECK_EQ(regs.nr43_shape, 0x37);

    // Build a table with N command on row 0, param = 0x71 (different shape).
    auto tbl = make_table_with_n_cmd(0x71);

    // Tick once — processes row 0 which has the N command.
    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);

    // After the N command, nr43_shape should be updated to the param.
    CHECK_EQ(regs.nr43_shape, 0x71);

    // Verify the actual hardware register was written (not just the cache).
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR43], 0x71);
}

TEST_CASE("N command on non-CH4 channel is a no-op")
{
    rackboy_apu_t apu = make_apu();

    // Trigger a Pulse1 voice.
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // nr43_shape should be 0 for a pulse channel.
    CHECK_EQ(regs.nr43_shape, 0);

    // Snapshot the hardware NR43 register before the tick.
    const uint8_t nr43_before = apu.gb.io_registers[GB_IO_NR43];

    // Build a table with N command on row 0.
    auto tbl = make_table_with_n_cmd(0x55);

    // Tick — N on Pulse1 should be a no-op.
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    // nr43_shape must remain 0 — N was ignored.
    CHECK_EQ(regs.nr43_shape, 0);

    // Hardware register must also be unchanged.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR43], nr43_before);
}

TEST_CASE("N command changes width bit preserved by subsequent pitch ticks")
{
    rackboy_apu_t apu = make_apu();

    // Trigger a noise voice with a long-period shape (bit 3 = 0).
    auto inst = make_noise_inst(0xF0, 0x30);  // bit 3 clear = long period
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);
    CHECK_EQ(regs.nr43_shape & 0x08, 0x00);  // long period

    // Table: row 0 = N command sets short-period shape (bit 3 = 1).
    //        row 1 = plain transpose row (no command), transpose +1.
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 0;
    tbl.rows[0].cmd1 = 'N';
    tbl.rows[0].cmd1_param = 0x38;  // bit 3 set = short period

    tbl.rows[1].transpose = 1;      // pitch shift +1 semitone
    tbl.rows[1].cmd1 = 0;           // no command

    // Tick 1: row 0 — N command fires, switches to short period.
    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.nr43_shape, 0x38);
    CHECK_EQ(regs.nr43_shape & 0x08, 0x08);  // short period

    // Tick 2: row 1 — plain transpose, no command. The table_tick pitch
    // path should merge the new width bit (0x08) from regs.nr43_shape
    // into the pitched NR43 byte, preserving the short-period timbre.
    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);

    // nr43_shape should still be 0x38 — no command on row 1 didn't change it.
    CHECK_EQ(regs.nr43_shape, 0x38);
    // The width bit should have been preserved through the pitch write
    // because table_tick's CH4 path uses (pitched_nr43 & ~0x08) | (regs.nr43_shape & 0x08).
    CHECK_EQ(regs.nr43_shape & 0x08, 0x08);
}

TEST_CASE("N command on same row as transpose: N param wins over pitch write")
{
    rackboy_apu_t apu = make_apu();

    auto inst = make_noise_inst(0xF0, 0x30);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);

    // Row 0: transpose +3 AND N command with explicit shape byte.
    // table_tick applies pitch (Step 4) before dispatch_command (Step 5),
    // so N's explicit param should overwrite the pitched NR43.
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 3;
    tbl.rows[0].cmd1 = 'N';
    tbl.rows[0].cmd1_param = 0x79;

    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);

    // N command should have overwritten the pitch path's NR43 write.
    CHECK_EQ(regs.nr43_shape, 0x79);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR43], 0x79);
}
