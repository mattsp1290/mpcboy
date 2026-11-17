// tests/core/trigger_test.cpp
//
// Unit tests for rackboy_core/rackboy_trigger.h — Gate 1 trigger-all
// dispatch (bead rackboy-bfl).
//
// Verifies that trigger_all():
//   * Calls table_trigger() for each voice with a valid instrument
//   * Skips voices with kNoInstrument
//   * Skips voices whose active_instrument_index is out of bounds
//   * Routes each voice to the correct per-channel instrument pool
//   * Passes the voice's target_pitch through to table_trigger()

#include "doctest/doctest.h"

#include "rackboy_trigger.h"
#include "rackboy_voice.h"
#include "rackboy_table.h"
#include "rackboy_pitch.h"
#include "rackboy_apu.h"
#include "rackboy_sav_types.h"
#include "rackboy_constants.h"

#include <cstring>

using rackboy::core::kNumVoices;
using rackboy::core::kNoInstrument;
using rackboy::core::VoiceChannel;
using rackboy::core::VoiceState;
using rackboy::core::voices_reset_all;
using rackboy::core::trigger_all;
using rackboy::table::TableRegState;
using rackboy::table::table_reg_state_reset;
using rackboy::sav::RackboyBank;
using rackboy::sav::bank_reset;

// Helper: initialize the APU once for the test suite.
static rackboy_apu_t make_apu()
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    return apu;
}

// Helper: populate a minimal instrument with a recognizable envelope
// byte so we can verify trigger_all routed to the correct pool entry.
static void set_instrument_pulse(rackboy::sav::RackboyInstrument& inst,
                                  uint8_t envelope, uint8_t duty_bits)
{
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypePulse;
    inst.raw[1] = envelope;     // NR12/NR22
    inst.raw[4] = 0;            // sweep
    inst.raw[7] = duty_bits;    // bits 6-7 = duty
}

static void set_instrument_wave(rackboy::sav::RackboyInstrument& inst,
                                 uint8_t volume_bits)
{
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeWave;
    inst.raw[1] = volume_bits;  // NR32 output level (bits 6-5)
}

static void set_instrument_noise(rackboy::sav::RackboyInstrument& inst,
                                  uint8_t envelope, uint8_t shape)
{
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeNoise;
    inst.raw[1] = envelope;     // NR42
    inst.raw[4] = shape;        // NR43 initial byte
}

TEST_CASE("trigger_all triggers all four voices when all have instruments")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Populate one instrument per channel.
    set_instrument_pulse(bank.pulse1_pool[0], 0xF0, 0x80);  // vol 15, 50% duty
    bank.pulse1_count = 1;

    set_instrument_pulse(bank.pulse2_pool[0], 0xA0, 0xC0);  // vol 10, 75% duty
    bank.pulse2_count = 1;

    set_instrument_wave(bank.wave_pool[0], 0x20);  // 50% output level
    bank.wave_count = 1;

    set_instrument_noise(bank.noise_pool[0], 0x80, 0x37);  // vol 8, some shape
    bank.noise_count = 1;
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    // Set all voices to instrument 0 with varying pitches.
    for (std::size_t i = 0; i < kNumVoices; ++i) {
        voices[i].active_instrument_index = 0;
        voices[i].target_pitch = static_cast<int16_t>(i * 3);  // 0, 3, 6, 9
    }

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // After trigger_all, every voice should be triggered with cursor at 0.
    for (std::size_t i = 0; i < kNumVoices; ++i) {
        INFO("voice index = ", i);
        CHECK(voices[i].triggered);
        CHECK_EQ(voices[i].table_cursor, 0);
        CHECK_EQ(voices[i].table_delay_ticks_remaining, 0);
    }

    // Check that register state was populated from the correct pool.
    SUBCASE("Pulse1 regs from pulse1_pool")
    {
        CHECK_EQ(regs[0].envelope, 0xF0);
        CHECK_EQ(regs[0].duty, 0x80);
        CHECK_EQ(regs[0].pitch_offset, 0);  // voice 0 had pitch 0
    }

    SUBCASE("Pulse2 regs from pulse2_pool")
    {
        CHECK_EQ(regs[1].envelope, 0xA0);
        CHECK_EQ(regs[1].duty, 0xC0);
        CHECK_EQ(regs[1].sweep, 0);  // CH2 has no sweep
        CHECK_EQ(regs[1].pitch_offset, 3);
    }

    SUBCASE("Wave regs from wave_pool")
    {
        CHECK_EQ(regs[2].wave_volume, 0x20);
        CHECK_EQ(regs[2].envelope, 0);   // CH3 doesn't use envelope
        CHECK_EQ(regs[2].pitch_offset, 6);
    }

    SUBCASE("Noise regs from noise_pool")
    {
        CHECK_EQ(regs[3].envelope, 0x80);
        CHECK_EQ(regs[3].nr43_shape, 0x37);
        CHECK_EQ(regs[3].pitch_offset, 9);
    }
}

TEST_CASE("trigger_all skips voices with kNoInstrument")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Only populate pulse1 — leave all other pools empty.
    set_instrument_pulse(bank.pulse1_pool[0], 0xF0, 0x80);
    bank.pulse1_count = 1;
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);

    // Only voice 0 has an instrument.
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 5;
    // voices[1..3] remain at kNoInstrument from reset.

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // Voice 0 should be triggered.
    CHECK(voices[0].triggered);
    CHECK_EQ(regs[0].envelope, 0xF0);
    CHECK_EQ(regs[0].pitch_offset, 5);

    // Voices 1-3 should NOT be triggered — they were skipped.
    for (std::size_t i = 1; i < kNumVoices; ++i) {
        INFO("voice index = ", i);
        CHECK_FALSE(voices[i].triggered);
        CHECK_EQ(regs[i].envelope, 0);  // still zero from reset
        CHECK_EQ(regs[i].pitch_offset, 0);
    }
}

TEST_CASE("trigger_all skips voices with out-of-bounds instrument index")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Pool has 2 instruments but voice claims index 5 (out of bounds).
    set_instrument_pulse(bank.pulse1_pool[0], 0xF0, 0x80);
    set_instrument_pulse(bank.pulse1_pool[1], 0xE0, 0x40);
    bank.pulse1_count = 2;
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 5;  // out of bounds
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // Voice 0 should NOT be triggered because index is out of bounds.
    CHECK_FALSE(voices[0].triggered);
    CHECK_EQ(regs[0].envelope, 0);
}

TEST_CASE("trigger_all selects correct instrument from multi-slot pool")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Pulse1 pool with 3 instruments, each with a distinct envelope.
    set_instrument_pulse(bank.pulse1_pool[0], 0x10, 0x00);
    set_instrument_pulse(bank.pulse1_pool[1], 0x20, 0x40);
    set_instrument_pulse(bank.pulse1_pool[2], 0x30, 0x80);
    bank.pulse1_count = 3;
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 2;  // third instrument
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    CHECK(voices[0].triggered);
    CHECK_EQ(regs[0].envelope, 0x30);  // from pool slot 2
    CHECK_EQ(regs[0].duty, 0x80);
}

TEST_CASE("trigger_all resets cursor and delay even if voice was mid-table")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_instrument_pulse(bank.pulse1_pool[0], 0xF0, 0x80);
    bank.pulse1_count = 1;
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;
    // Simulate a voice mid-table: cursor at row 7, delay active.
    voices[0].table_cursor = 7;
    voices[0].table_delay_ticks_remaining = 4;
    voices[0].triggered = true;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // AD #9: trigger unconditionally resets cursor to 0 and clears delay.
    CHECK_EQ(voices[0].table_cursor, 0);
    CHECK_EQ(voices[0].table_delay_ticks_remaining, 0);
    CHECK(voices[0].triggered);
}

TEST_CASE("trigger_all passes target_pitch through to table_trigger")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_instrument_pulse(bank.pulse1_pool[0], 0xF0, 0x80);
    bank.pulse1_count = 1;
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = -12;  // one octave down

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    CHECK_EQ(regs[0].pitch_offset, -12);

    // Verify freq_11bit is the square-wave frequency for -12 semitones.
    uint16_t expected_freq = rackboy::core::pitch_square_11bit(-12);
    CHECK_EQ(regs[0].freq_11bit, expected_freq);
}

TEST_CASE("trigger_all with completely empty bank triggers nothing")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);
    // bank.loaded is false, all counts are 0.

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    // All voices have kNoInstrument from reset.

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // Nothing should have changed.
    for (std::size_t i = 0; i < kNumVoices; ++i) {
        CHECK_FALSE(voices[i].triggered);
        CHECK_EQ(regs[i].pitch_offset, 0);
    }
}

TEST_CASE("trigger_all with negative instrument index skips voice")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_instrument_pulse(bank.pulse1_pool[0], 0xF0, 0x80);
    bank.pulse1_count = 1;
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = -5;  // negative, not kNoInstrument
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    CHECK_FALSE(voices[0].triggered);
}
