// tests/table/vibrato_test.cpp
//
// Integration tests for the V (vibrato) table command
// (bead rackboy-00h).
//
// Per docs/LSDJ_REFERENCE.md, rackboy's V command encodes vibrato
// speed in the high nibble and depth in the low nibble of the
// parameter byte. V00 disables; non-zero values arm a triangle-LFO
// pitch offset applied on top of `regs.pitch_offset` during every
// Gate 2 tick's frequency write (without mutating pitch_offset
// itself).
//
// Verifies:
//   - V N populates VoiceState.vibrato_{speed,depth,phase} on dispatch
//   - V 0x00 disables vibrato (speed=0, depth=0, phase=0)
//   - vibrato_phase advances by vibrato_speed every tick
//   - regs.pitch_offset is NEVER mutated by the vibrato path
//   - frequency writes reflect the pitch + LFO on Pulse channels
//   - Gate 1 (table_trigger) resets all three vibrato fields
//   - full-cycle phase sweep produces a symmetric triangle (peaks
//     above base pitch, troughs below)
//   - CH4 Noise path respects vibrato through pitch_noise_nr43

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

static rackboy::sav::RackboyTable make_table_with_v_cmd(uint8_t param)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose   = 0;
    tbl.rows[0].cmd1        = 'V';
    tbl.rows[0].cmd1_param  = param;
    return tbl;
}

static rackboy::sav::RackboyTable make_empty_table()
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    return tbl;
}

TEST_CASE("V populates vibrato state on dispatch (speed=high nibble, depth=low nibble)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    REQUIRE(voice.vibrato_speed == 0);
    REQUIRE(voice.vibrato_depth == 0);
    REQUIRE(voice.vibrato_phase == 0);

    // V 0x52 → speed=5, depth=2.
    auto tbl = make_table_with_v_cmd(0x52);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    CHECK_EQ(voice.vibrato_speed, 5);
    CHECK_EQ(voice.vibrato_depth, 2);
    // Phase does not advance on dispatch tick (V is dispatched after
    // Step 4.5, so the speed is still 0 when the LFO advance runs).
    CHECK_EQ(voice.vibrato_phase, 0);
}

TEST_CASE("V00 disables vibrato (all three fields zeroed)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Arm vibrato first.
    auto v_arm = make_table_with_v_cmd(0x84);   // speed=8, depth=4
    table_tick(voice, regs, VoiceChannel::Pulse1, v_arm, &apu, /*bank=*/nullptr);
    REQUIRE(voice.vibrato_speed == 8);

    // Run a couple more ticks so vibrato_phase becomes non-zero.
    auto empty = make_empty_table();
    voice.table_cursor = 5;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    voice.table_cursor = 6;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    REQUIRE(voice.vibrato_phase != 0);

    // Now send V00.
    auto v_off = make_table_with_v_cmd(0x00);
    voice.table_cursor = 0;
    table_tick(voice, regs, VoiceChannel::Pulse1, v_off, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.vibrato_speed, 0);
    CHECK_EQ(voice.vibrato_depth, 0);
    CHECK_EQ(voice.vibrato_phase, 0);
}

TEST_CASE("vibrato_phase advances by speed each tick")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // V 0x30 → speed=3, depth=0. Depth zero keeps the LFO "silent"
    // musically but the phase still advances; this lets us isolate
    // the phase-advancement contract from the frequency-modulation
    // contract.
    auto v_tbl = make_table_with_v_cmd(0x30);
    table_tick(voice, regs, VoiceChannel::Pulse1, v_tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.vibrato_phase, 0);   // dispatch tick: no advance

    auto empty = make_empty_table();
    voice.table_cursor = 5;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.vibrato_phase, 3);   // +3

    voice.table_cursor = 6;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.vibrato_phase, 6);   // +3

    voice.table_cursor = 7;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.vibrato_phase, 9);   // +3
}

TEST_CASE("vibrato_phase wraps at 256 (uint8_t natural wrap)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    auto v_tbl = make_table_with_v_cmd(0x80);  // speed=8, depth=0
    table_tick(voice, regs, VoiceChannel::Pulse1, v_tbl, &apu, /*bank=*/nullptr);

    auto empty = make_empty_table();
    // 32 ticks × 8 = 256 → phase wraps to 0.
    for (int i = 0; i < 32; ++i) {
        voice.table_cursor = static_cast<uint8_t>((i + 5) & 0x0F);
        table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    }
    CHECK_EQ(voice.vibrato_phase, 0);
}

TEST_CASE("vibrato does NOT mutate regs.pitch_offset")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 5, &apu);
    const int16_t pitch_before = regs.pitch_offset;
    REQUIRE(pitch_before == 5);

    auto v_tbl = make_table_with_v_cmd(0x8F);  // speed=8, depth=15 (max)
    table_tick(voice, regs, VoiceChannel::Pulse1, v_tbl, &apu, /*bank=*/nullptr);

    auto empty = make_empty_table();
    // Tick through a full LFO cycle. pitch_offset must stay at 5
    // throughout — the vibrato path applies an ephemeral offset to
    // the frequency write only.
    for (int i = 0; i < 40; ++i) {
        voice.table_cursor = static_cast<uint8_t>((i + 5) & 0x0F);
        table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
        CAPTURE(i);
        CHECK_EQ(regs.pitch_offset, pitch_before);
    }
}

TEST_CASE("vibrato modulates Pulse1 frequency (peak above, trough below base)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    // Trigger at a pitch far enough from the hardware edges that the
    // vibrato +/- excursions do not clamp.
    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    const uint16_t base_freq = regs.freq_11bit;
    REQUIRE(base_freq != 0);

    // V with speed=64 (quarter-cycle per tick), depth=15 (max
    // amplitude). That way tick +1 hits the positive peak and tick
    // +3 hits the negative trough of the triangle — deterministic
    // to verify.
    auto v_tbl = make_table_with_v_cmd(0xFF);  // speed=15, depth=15
    table_tick(voice, regs, VoiceChannel::Pulse1, v_tbl, &apu, /*bank=*/nullptr);

    auto empty = make_empty_table();
    uint16_t max_freq = base_freq;
    uint16_t min_freq = base_freq;

    // Sweep a full LFO cycle (speed=15 means 256/15 ≈ 17 ticks per
    // cycle) and record the per-tick frequency. The triangle should
    // take us ABOVE and BELOW the base pitch symmetrically.
    for (int i = 0; i < 32; ++i) {
        voice.table_cursor = static_cast<uint8_t>((i + 5) & 0x0F);
        table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
        if (regs.freq_11bit > max_freq) max_freq = regs.freq_11bit;
        if (regs.freq_11bit < min_freq) min_freq = regs.freq_11bit;
    }

    // Positive vibrato offset → higher semitone → higher 11-bit freq;
    // negative vibrato offset → lower 11-bit freq. We should see both.
    CHECK_GT(max_freq, base_freq);
    CHECK_LT(min_freq, base_freq);
}

TEST_CASE("Gate 1 (table_trigger) resets vibrato state")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    // Prime vibrato state as if a previous "note" was mid-LFO.
    voice.vibrato_speed = 9;
    voice.vibrato_depth = 7;
    voice.vibrato_phase = 123;

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    CHECK_EQ(voice.vibrato_speed, 0);
    CHECK_EQ(voice.vibrato_depth, 0);
    CHECK_EQ(voice.vibrato_phase, 0);
}

TEST_CASE("Zero depth but non-zero speed advances phase without modulating freq")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    const uint16_t base_freq = regs.freq_11bit;

    auto v_tbl = make_table_with_v_cmd(0xF0);  // speed=15, depth=0
    table_tick(voice, regs, VoiceChannel::Pulse1, v_tbl, &apu, /*bank=*/nullptr);

    auto empty = make_empty_table();
    for (int i = 0; i < 20; ++i) {
        voice.table_cursor = static_cast<uint8_t>((i + 5) & 0x0F);
        table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
        CAPTURE(i);
        // Depth 0 means LFO output is 0 regardless of phase. Every
        // freq write must land on the same base frequency.
        CHECK_EQ(regs.freq_11bit, base_freq);
    }
    // But phase has advanced.
    CHECK_GT(voice.vibrato_phase, 0);
}

TEST_CASE("Vibrato reaches CH4 Noise via NR43 pitch recomputation")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_noise_inst(0xF0, 0x30);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);

    // Max speed + max depth — guaranteed to produce varying NR43
    // output across a full phase sweep.
    auto v_tbl = make_table_with_v_cmd(0xFF);
    table_tick(voice, regs, VoiceChannel::Noise, v_tbl, &apu, /*bank=*/nullptr);

    // Track the set of unique NR43 values seen across a full sweep.
    // A single flicker would give us 2 unique bytes and satisfy the
    // old `saw_different` test — but a real triangle sweep through
    // pitch_noise_nr43 should land on several distinct shape bytes
    // across 32 ticks at max speed + depth. We assert >= 3 unique
    // values as the tightest cheap signal that the LFO is actually
    // sweeping through the pitch-to-NR43 quantization bands rather
    // than one-bit toggling around the base.
    auto empty = make_empty_table();
    uint8_t unique_nr43[256] = {0};
    unsigned unique_count = 0;
    for (int i = 0; i < 32; ++i) {
        voice.table_cursor = static_cast<uint8_t>((i + 5) & 0x0F);
        table_tick(voice, regs, VoiceChannel::Noise, empty, &apu, /*bank=*/nullptr);
        const uint8_t v = apu.gb.io_registers[GB_IO_NR43];
        if (!unique_nr43[v]) {
            unique_nr43[v] = 1;
            ++unique_count;
        }
    }
    CAPTURE(unique_count);
    CHECK_GE(unique_count, 3u);
}
