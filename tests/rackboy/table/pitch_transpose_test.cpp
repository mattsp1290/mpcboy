// tests/table/pitch_transpose_test.cpp
//
// Unit tests for per-row pitch transpose in table_tick (bead rackboy-gxo).
//
// Row transpose is a per-row signed semitone delta that accumulates into
// regs.pitch_offset on every Gate 2 tick. The accumulated pitch_offset is
// then converted to a channel-appropriate NRx3/NRx4 (or NR43 for CH4)
// frequency register write — without the trigger bit, so pitch-only rows
// do not retrigger the envelope.
//
// Base note is C5 / MIDI 72 per rackboy_pitch.h: semitones_from_base == 0
// corresponds to C5. Positive values transpose up, negative values down.
//
// Implementation lives in table_tick() in rackboy_table.cpp:
//   regs.pitch_offset += static_cast<int16_t>(row.transpose);
//   // then channel-specific pitch_*() → NRx3/NRx4 write (no trigger bit)

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

static rackboy::sav::RackboyInstrument make_pulse_inst()
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypePulse;
    inst.raw[1] = 0xF0;  // vol 15, no envelope
    inst.raw[7] = 0x80;  // 50% duty
    return inst;
}

static rackboy::sav::RackboyInstrument make_wave_inst()
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeWave;
    inst.raw[1] = 0x20;  // NR32 bits 6-5 = 01 → 100% output level
    return inst;
}

static rackboy::sav::RackboyInstrument make_noise_inst()
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeNoise;
    inst.raw[1] = 0xF0;
    inst.raw[4] = 0x30;  // long-period shape
    return inst;
}

// Build a 2-row table with a transpose value on row 0 and a separate
// transpose value on row 1. Neither row carries a command.
static rackboy::sav::RackboyTable make_2row_transpose_table(int8_t t0,
                                                              int8_t t1)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = t0;
    tbl.rows[0].cmd1 = 0;
    tbl.rows[1].transpose = t1;
    tbl.rows[1].cmd1 = 0;
    return tbl;
}

// ---------------------------------------------------------------------------
// CH1/CH2 (Pulse) — pitch transpose uses pitch_square_11bit
// ---------------------------------------------------------------------------

TEST_CASE("Pulse1: row transpose accumulates into pitch_offset and updates freq")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    CHECK_EQ(regs.pitch_offset, 0);
    const uint16_t freq_at_c5 = regs.freq_11bit;

    // 2-row table: row 0 transposes +3, row 1 transposes +2.
    // After row 0: pitch_offset = 3. After row 1: pitch_offset = 5.
    auto tbl = make_2row_transpose_table(3, 2);

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 3);
    CHECK_EQ(regs.freq_11bit, rackboy::core::pitch_square_11bit(3));

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 5);
    CHECK_EQ(regs.freq_11bit, rackboy::core::pitch_square_11bit(5));

    // freq must have moved away from the initial C5 freq.
    CHECK_NE(regs.freq_11bit, freq_at_c5);

    // NR14 must NOT have the trigger bit set (pitch ticks are no-trigger).
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x80, 0x00);

    // Raw register bytes must match the freq_11bit low/high split — locks
    // the shim contract, not just the regs cache.
    const uint16_t expected_freq = rackboy::core::pitch_square_11bit(5);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR13], expected_freq & 0xFF);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x07, (expected_freq >> 8) & 0x07);
}

TEST_CASE("Pulse2: negative row transpose shifts pitch down")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse2, inst, 0, &apu);
    const uint16_t freq_at_c5 = regs.freq_11bit;

    // 2-row table with negative transposes.
    auto tbl = make_2row_transpose_table(-5, -7);

    table_tick(voice, regs, VoiceChannel::Pulse2, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, -5);
    CHECK_EQ(regs.freq_11bit, rackboy::core::pitch_square_11bit(-5));

    table_tick(voice, regs, VoiceChannel::Pulse2, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, -12);  // one octave down from C5
    CHECK_EQ(regs.freq_11bit, rackboy::core::pitch_square_11bit(-12));

    // Frequency should have decreased from C5 (lower notes → lower NRx3/NRx4).
    CHECK_LT(regs.freq_11bit, freq_at_c5);

    // No trigger bit on NR24.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR24] & 0x80, 0x00);
}

// ---------------------------------------------------------------------------
// CH3 (Wave) — pitch transpose uses pitch_wave_11bit
// ---------------------------------------------------------------------------

TEST_CASE("Wave: row transpose updates NR33/NR34 freq without trigger")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);
    CHECK_EQ(regs.pitch_offset, 0);

    auto tbl = make_2row_transpose_table(7, -2);

    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 7);
    CHECK_EQ(regs.freq_11bit, rackboy::core::pitch_wave_11bit(7));

    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 5);
    CHECK_EQ(regs.freq_11bit, rackboy::core::pitch_wave_11bit(5));

    // No trigger bit on NR34.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR34] & 0x80, 0x00);
}

// ---------------------------------------------------------------------------
// CH4 (Noise) — pitch transpose uses pitch_noise_nr43 + preserves width bit
// ---------------------------------------------------------------------------

TEST_CASE("Noise: row transpose repacks NR43 and preserves width bit (long period)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_noise_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);
    // Long-period instrument — bit 3 of nr43_shape is clear.
    CHECK_EQ(regs.nr43_shape & 0x08, 0x00);
    // Capture NR44 right after trigger — CH4 pitch ticks must not touch it.
    const uint8_t nr44_after_trigger = apu.gb.io_registers[GB_IO_NR44];
    CHECK_EQ(nr44_after_trigger & 0x80, 0x80);  // trigger bit was set

    auto tbl = make_2row_transpose_table(12, -6);  // +12 then -6 → +6

    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 12);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR43] & 0x08, 0x00);  // long period

    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 6);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR43] & 0x08, 0x00);

    // CH4 pitch path only touches NR43; NR44 is not rewritten by
    // transpose ticks. Asserted explicitly so a future regression that
    // spuriously rewrites NR44 during pitch-only ticks will fail here.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR44], nr44_after_trigger);
}

TEST_CASE("Noise: row transpose preserves width bit (short period)")
{
    rackboy_apu_t apu = make_apu();
    // Build a short-period instrument: bit 3 of NR43 shape set.
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeNoise;
    inst.raw[1] = 0xF0;
    inst.raw[4] = 0x38;  // bit 3 set = short period

    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);
    CHECK_EQ(regs.nr43_shape & 0x08, 0x08);  // short period

    auto tbl = make_2row_transpose_table(4, -2);

    // After each pitch-only tick, width bit must remain set. This
    // exercises the `| width_bit` half of the merge at
    // rackboy_table.cpp's CH4 branch:
    //   merged_nr43 = (pitched_nr43 & ~0x08u) | (regs.nr43_shape & 0x08u)
    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR43] & 0x08, 0x08);

    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR43] & 0x08, 0x08);
}

// ---------------------------------------------------------------------------
// Transpose does not retrigger the envelope
// ---------------------------------------------------------------------------

TEST_CASE("Pulse1: envelope register is not rewritten on transpose ticks")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    const uint8_t env_after_trigger = apu.gb.io_registers[GB_IO_NR12];
    CHECK_EQ(env_after_trigger, 0xF0);

    // Pure transpose — no command, no retrigger expected.
    auto tbl = make_2row_transpose_table(3, 3);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    // NR12 should NOT have been touched — table_tick's pitch path writes
    // only NRx3/NRx4, never NRx2.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR12], env_after_trigger);
    CHECK_EQ(regs.envelope, 0xF0);  // regs cache also untouched
}

// ---------------------------------------------------------------------------
// Zero transpose: pitch_offset unchanged
// ---------------------------------------------------------------------------

TEST_CASE("Zero row transpose leaves pitch_offset unchanged")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 5, &apu);
    CHECK_EQ(regs.pitch_offset, 5);

    // 2-row table with transpose 0 on both rows.
    auto tbl = make_2row_transpose_table(0, 0);

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 5);  // unchanged

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 5);  // still unchanged
}

TEST_CASE("Table cursor wraps at kTableRowCount=16")
{
    using rackboy::core::kTableRowCount;
    static_assert(kTableRowCount == 16,
                  "This test assumes the LSDJ table has 16 rows; update "
                  "the tick count if kTableRowCount changes.");

    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    CHECK_EQ(voice.table_cursor, 0);
    CHECK_EQ(regs.pitch_offset, 0);

    // Table: all rows transpose 0 EXCEPT row 15 transposes +1.
    // Row 0 also transposes +1 (so we can detect cursor wrap).
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 1;
    tbl.rows[15].transpose = 1;
    // rows[1..14] stay at transpose 0 from table_reset.

    // Tick 1: reads row 0 (transpose +1), cursor → 1. pitch_offset = 1.
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.table_cursor, 1);
    CHECK_EQ(regs.pitch_offset, 1);

    // Ticks 2..15: cursor advances through rows 1..14 (all transpose 0).
    for (int i = 0; i < 14; ++i) {
        table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    }
    CHECK_EQ(voice.table_cursor, 15);
    CHECK_EQ(regs.pitch_offset, 1);  // still 1 — rows 1..14 are transpose 0

    // Tick 16: reads row 15 (transpose +1), cursor wraps to 0. pitch_offset = 2.
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.table_cursor, 0);  // wrapped from 16 → 0
    CHECK_EQ(regs.pitch_offset, 2);

    // Tick 17: reads row 0 again (transpose +1), cursor → 1. pitch_offset = 3.
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.table_cursor, 1);
    CHECK_EQ(regs.pitch_offset, 3);
}
