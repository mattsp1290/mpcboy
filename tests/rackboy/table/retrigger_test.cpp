// tests/table/retrigger_test.cpp
//
// Integration tests for the R (retrigger) table command (bead rackboy-bgu).
//
// Per docs/LSDJ_REFERENCE.md, `R` "re-arms the channel trigger bit every N
// ticks" where N is the command parameter, and `R00` is a no-op. rackboy
// implements this as recurring state on VoiceState: the `R` handler stores
// `retrigger_period_ticks` and `retrigger_counter_ticks`, and table_tick()
// fires NRx4 bit 7 (or NR44 on CH4) whenever the counter reaches zero,
// resetting the counter back to the period each fire.
//
// Verifies:
//   - R N populates retrigger_period_ticks / counter on the dispatch tick
//   - First retrigger fires exactly N Gate 2 ticks later (not on the R tick)
//   - Subsequent retriggers fire every N ticks (recurring)
//   - NRx4 fire preserves freq_11bit high bits for CH1/CH2/CH3
//   - NR44 fire is bit 7 only (CH4 has no freq bits in NR44)
//   - R00 disables retrigger (counter stays zero, no fires)
//   - Gate 1 / table_trigger resets retrigger state (no bleed across notes)
//   - pitch_offset and envelope are not altered by the retrigger fire

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

// Build a 16-row table: row 0 carries `R param`; rows 1..15 are empty.
// Because table_tick advances past kTableRowCount, the empty rows carry
// no commands and only exercise the retrigger state machine.
static rackboy::sav::RackboyTable make_table_with_r_cmd(uint8_t param)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 0;
    tbl.rows[0].cmd1 = 'R';
    tbl.rows[0].cmd1_param = param;
    return tbl;
}

// Empty 16-row table with only transpose=0 everywhere. Used to advance
// the cursor without running any commands.
static rackboy::sav::RackboyTable make_empty_table()
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    return tbl;
}

TEST_CASE("R N populates retrigger state on dispatch tick (Pulse1)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    REQUIRE(voice.retrigger_period_ticks == 0);
    REQUIRE(voice.retrigger_counter_ticks == 0);

    auto tbl = make_table_with_r_cmd(3);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    // Dispatch tick stores period + counter but does NOT fire on this tick.
    CHECK_EQ(voice.retrigger_period_ticks, 3);
    CHECK_EQ(voice.retrigger_counter_ticks, 3);
}

TEST_CASE("R 3 on Pulse1: first retrigger fires on tick 3, recurring every 3")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    // Trigger at +24 semitones to ensure non-trivial freq high bits.
    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 24, &apu);
    const uint16_t freq_snapshot = regs.freq_11bit;
    REQUIRE(freq_snapshot != 0);

    // Tick 0: the R command is on row 0; it sets period=3 and counter=3,
    // but retrigger fires AFTER the check, so no fire on this tick — the
    // NR14 state from table_trigger (trigger bit was already set by the
    // Gate 1 trigger) is not re-poked by us. We snapshot the NR14 byte
    // right after trigger: it has the trigger bit set from table_trigger.
    const uint8_t nr14_after_trigger = apu.gb.io_registers[GB_IO_NR14];
    REQUIRE((nr14_after_trigger & 0x80) == 0x80);

    auto r_tbl = make_table_with_r_cmd(3);
    table_tick(voice, regs, VoiceChannel::Pulse1, r_tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.retrigger_counter_ticks, 3);

    // Tick 1: counter 3 → 2, no fire. Use an empty table so only the
    // retrigger state machine runs (the pitch path also writes NR13/NR14,
    // but without the trigger bit — so bit 7 will be cleared if no fire
    // happens). Verify NR14 bit 7 is clear after this tick.
    auto empty = make_empty_table();
    // Park the cursor at a row with cmd1==0 so dispatch_command isn't
    // invoked and the only NR14 write comes from the pitch path
    // (no-trigger) and the retrigger check (maybe-trigger).
    voice.table_cursor = 5;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.retrigger_counter_ticks, 2);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x80, 0x00);  // no fire

    // Tick 2: counter 2 → 1, no fire.
    voice.table_cursor = 6;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.retrigger_counter_ticks, 1);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x80, 0x00);

    // Tick 3: counter 1 → 0 → fire, reset counter to period (3).
    voice.table_cursor = 7;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.retrigger_counter_ticks, 3);  // reset to period
    const uint8_t nr14_fire = apu.gb.io_registers[GB_IO_NR14];
    CHECK_EQ(nr14_fire & 0x80, 0x80);              // trigger bit set
    CHECK_EQ(nr14_fire & 0x07,
             (regs.freq_11bit >> 8) & 0x07);       // freq hi preserved

    // Ticks 4, 5: no fire (cleared again by pitch-path write).
    voice.table_cursor = 8;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x80, 0x00);
    voice.table_cursor = 9;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x80, 0x00);

    // Tick 6: counter 1 → 0 → fire again (recurring).
    voice.table_cursor = 10;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x80, 0x80);
    CHECK_EQ(voice.retrigger_counter_ticks, 3);
}

TEST_CASE("R 2 on Pulse2: fires every 2 ticks on NR24")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xA0, 0xC0);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse2, inst, 12, &apu);
    const uint16_t freq = regs.freq_11bit;
    REQUIRE(freq != 0);

    auto r_tbl = make_table_with_r_cmd(2);
    table_tick(voice, regs, VoiceChannel::Pulse2, r_tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.retrigger_period_ticks, 2);

    auto empty = make_empty_table();
    voice.table_cursor = 5;
    table_tick(voice, regs, VoiceChannel::Pulse2, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR24] & 0x80, 0x00);  // tick +1: no fire

    voice.table_cursor = 6;
    table_tick(voice, regs, VoiceChannel::Pulse2, empty, &apu, /*bank=*/nullptr);
    // tick +2: fire
    const uint8_t nr24 = apu.gb.io_registers[GB_IO_NR24];
    CHECK_EQ(nr24 & 0x80, 0x80);
    CHECK_EQ(nr24 & 0x07, (regs.freq_11bit >> 8) & 0x07);
}

TEST_CASE("R 1 on Wave: fires every tick on NR34, freq hi preserved")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst(0x40);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 7, &apu);
    REQUIRE(regs.freq_11bit != 0);

    auto r_tbl = make_table_with_r_cmd(1);
    table_tick(voice, regs, VoiceChannel::Wave, r_tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.retrigger_period_ticks, 1);
    // Dispatch tick writes NR34 no-trigger (pitch path) and does NOT
    // fire the retrigger: the retrigger check at Step 5 saw period==0
    // because the R dispatch runs later at Step 6. Verify bit 7 is
    // clear so a regression that moved the retrigger check to run
    // AFTER dispatch (and would fire on the same tick as R even at
    // R=1) is caught on the Wave path too — the Pulse1 test already
    // covers this for NR14.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR34] & 0x80, 0x00);

    auto empty = make_empty_table();
    // With period=1, every subsequent tick fires.
    voice.table_cursor = 5;
    table_tick(voice, regs, VoiceChannel::Wave, empty, &apu, /*bank=*/nullptr);
    uint8_t nr34 = apu.gb.io_registers[GB_IO_NR34];
    CHECK_EQ(nr34 & 0x80, 0x80);
    CHECK_EQ(nr34 & 0x07, (regs.freq_11bit >> 8) & 0x07);
    CHECK_EQ(voice.retrigger_counter_ticks, 1);  // re-armed

    voice.table_cursor = 6;
    table_tick(voice, regs, VoiceChannel::Wave, empty, &apu, /*bank=*/nullptr);
    nr34 = apu.gb.io_registers[GB_IO_NR34];
    CHECK_EQ(nr34 & 0x80, 0x80);
}

TEST_CASE("R on Noise: fires NR44 with only the trigger bit")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_noise_inst(0xF0, 0x30);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);
    // Clear NR44 so we can detect the R fire distinctly from the Gate 1
    // trigger's write.
    apu.gb.io_registers[GB_IO_NR44] = 0x00;

    auto r_tbl = make_table_with_r_cmd(2);
    table_tick(voice, regs, VoiceChannel::Noise, r_tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR44], 0x00);  // dispatch tick: no fire

    auto empty = make_empty_table();
    voice.table_cursor = 5;
    table_tick(voice, regs, VoiceChannel::Noise, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR44], 0x00);  // tick +1: no fire

    voice.table_cursor = 6;
    table_tick(voice, regs, VoiceChannel::Noise, empty, &apu, /*bank=*/nullptr);
    // tick +2: fire — NR44 = trigger bit only
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR44], 0x80);
}

TEST_CASE("R00 disables retrigger (no fires, counter stays zero)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // First set up a recurring retrigger...
    auto r3 = make_table_with_r_cmd(3);
    table_tick(voice, regs, VoiceChannel::Pulse1, r3, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.retrigger_period_ticks, 3);

    // ...then cancel it with R00.
    auto r0 = make_table_with_r_cmd(0);
    voice.table_cursor = 0;  // rewind so the R command executes again
    table_tick(voice, regs, VoiceChannel::Pulse1, r0, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.retrigger_period_ticks, 0);
    CHECK_EQ(voice.retrigger_counter_ticks, 0);

    // Now tick many times with an empty table: NR14 bit 7 must never be
    // set by the retrigger check (the pitch path writes no-trigger NR14).
    auto empty = make_empty_table();
    for (int i = 0; i < 10; ++i) {
        voice.table_cursor = static_cast<uint8_t>(i % 16);
        table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
        CAPTURE(i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x80, 0x00);
    }
}

TEST_CASE("Gate 1 (table_trigger) resets retrigger state")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    // Prime retrigger state from a previous "note".
    voice.retrigger_period_ticks  = 7;
    voice.retrigger_counter_ticks = 3;

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    CHECK_EQ(voice.retrigger_period_ticks, 0);
    CHECK_EQ(voice.retrigger_counter_ticks, 0);
}

TEST_CASE("R fire does not touch pitch_offset, envelope, or freq_11bit")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 5, &apu);
    // Seed CH3- and CH4-specific cache fields to distinctive values
    // so a buggy R path that zeroed them on a Pulse1 voice would be
    // caught. (On Pulse1, wave_volume and nr43_shape are normally
    // left at their default 0 by table_trigger — any non-default
    // value here is load-bearing only for this isolation check.)
    regs.wave_volume            = 0x40;
    regs.nr43_shape             = 0x08;
    const int16_t  pitch_before = regs.pitch_offset;
    const uint8_t  env_before   = regs.envelope;
    const uint16_t freq_before  = regs.freq_11bit;
    const uint8_t  wv_before    = regs.wave_volume;
    const uint8_t  nr43_before  = regs.nr43_shape;

    auto r_tbl = make_table_with_r_cmd(1);
    table_tick(voice, regs, VoiceChannel::Pulse1, r_tbl, &apu, /*bank=*/nullptr);

    auto empty = make_empty_table();
    voice.table_cursor = 5;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);

    // pitch_offset / envelope / freq must be unchanged — R only re-arms
    // the hardware trigger bit; it does not alter any register cache.
    CHECK_EQ(regs.pitch_offset, pitch_before);
    CHECK_EQ(regs.envelope,     env_before);
    CHECK_EQ(regs.freq_11bit,   freq_before);
    // Same isolation guarantee for the CH3/CH4-specific cache fields.
    CHECK_EQ(regs.wave_volume,  wv_before);
    CHECK_EQ(regs.nr43_shape,   nr43_before);
}

TEST_CASE("R fire on Noise touches only NR44; pitch-path NR43 survives")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_noise_inst(0xF0, 0x55);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);

    // Poke NR43 to a distinct non-trigger-related value so we can detect
    // whether the retrigger fire accidentally touches it. The table_tick
    // pitch path DOES write NR43 for CH4 (pitched_nr43 merged with width)
    // every tick, so this check is per-tick AFTER the pitch-path write:
    // we just verify the retrigger fire itself doesn't add a redundant
    // NR43 write by checking the pre-fire and post-fire NR43 are the
    // pitch-path value (not the trigger bit pattern).
    auto r_tbl = make_table_with_r_cmd(1);
    table_tick(voice, regs, VoiceChannel::Noise, r_tbl, &apu, /*bank=*/nullptr);

    auto empty = make_empty_table();
    voice.table_cursor = 5;
    apu.gb.io_registers[GB_IO_NR44] = 0x00;
    const uint8_t nr43_expected = rackboy::core::pitch_noise_nr43(regs.pitch_offset);
    const uint8_t width_bit = regs.nr43_shape & 0x08u;
    const uint8_t nr43_merged = static_cast<uint8_t>(
        (nr43_expected & ~0x08u) | width_bit);
    table_tick(voice, regs, VoiceChannel::Noise, empty, &apu, /*bank=*/nullptr);

    CHECK_EQ(apu.gb.io_registers[GB_IO_NR44], 0x80);        // fired
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR43], nr43_merged); // pitch-path
}
