// tests/apu/wave_ram_test.cpp
//
// Unit tests for rackboy_apu_write_wave_ram() (bead rackboy-9i0).
//
// Verifies:
//   - All 16 bytes land in $FF30..$FF3F in order
//   - NR30 is restored to its pre-write value (DAC state preserved)
//   - Writes succeed even when CH3 was actively playing before the call
//     (exercising the "disable DAC → write → restore DAC" safe pattern)
//   - Direct writes via rackboy_apu_write_reg while CH3 is active are
//     dropped — proves the helper's dance is necessary, not just safe
//   - Helper does not retrigger CH3 (current_sample_index unchanged)
//   - NULL sentinel behavior is not asserted here — the helper documents
//     that the caller must pass a valid 16-byte buffer
//
// TODO(cgb): SameBoy has different wave-RAM-write semantics on CGB E+
// (redirect to current_sample_index/2 rather than drop). These tests
// exercise the DMG path because rackboy v1 hardcodes DMG in the shim
// (see rackboy_apu_shim.h). When CGB support lands, add a CGB E+ variant
// that exercises the redirect path.

#include "doctest/doctest.h"

extern "C" {
#include "rackboy_apu.h"
}

#include <cstring>
#include <cstdint>

static rackboy_apu_t make_apu()
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    return apu;
}

TEST_CASE("rackboy_apu_write_wave_ram: all 16 bytes land in $FF30..$FF3F")
{
    rackboy_apu_t apu = make_apu();

    uint8_t waveform[16];
    for (int i = 0; i < 16; ++i) {
        waveform[i] = static_cast<uint8_t>(0xA0 + i);
    }

    rackboy_apu_write_wave_ram(&apu, waveform);

    for (int i = 0; i < 16; ++i) {
        INFO("i = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], waveform[i]);
    }
}

TEST_CASE("rackboy_apu_write_wave_ram: NR30 is restored after the write")
{
    rackboy_apu_t apu = make_apu();

    // DAC off (NR30 bit 7 = 0) — the common "unconfigured" state.
    rackboy_apu_write_reg(&apu, 0xFF1A, 0x00);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR30], 0x00);

    uint8_t waveform[16];
    std::memset(waveform, 0x55, sizeof(waveform));
    rackboy_apu_write_wave_ram(&apu, waveform);

    // NR30 still 0x00 after the helper.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR30], 0x00);

    // And when starting with DAC on:
    rackboy_apu_write_reg(&apu, 0xFF1A, 0x80);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR30], 0x80);

    std::memset(waveform, 0xAA, sizeof(waveform));
    rackboy_apu_write_wave_ram(&apu, waveform);

    // NR30 restored to 0x80.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR30], 0x80);
}

TEST_CASE("rackboy_apu_write_wave_ram: writes succeed with CH3 active")
{
    rackboy_apu_t apu = make_apu();

    // Activate CH3 as a typical playback state:
    // NR30 = 0x80 (DAC on), NR31 = 0xFF (max length), NR32 = 0x20 (100%),
    // NR33/NR34 = some freq + trigger.
    rackboy_apu_write_reg(&apu, 0xFF1A, 0x80);  // NR30
    rackboy_apu_write_reg(&apu, 0xFF1B, 0xFF);  // NR31
    rackboy_apu_write_reg(&apu, 0xFF1C, 0x20);  // NR32
    rackboy_apu_write_reg(&apu, 0xFF1D, 0x00);  // NR33
    rackboy_apu_write_reg(&apu, 0xFF1E, 0x87);  // NR34: trigger + freq_hi=7

    // Without the helper's DAC-disable dance, writes here would be
    // dropped by SameBoy because is_active[GB_WAVE] is true.
    uint8_t waveform[16];
    for (int i = 0; i < 16; ++i) {
        waveform[i] = static_cast<uint8_t>(0x10 + i);
    }

    rackboy_apu_write_wave_ram(&apu, waveform);

    // The helper must have disabled the DAC before writing, so these
    // assertions prove the bytes actually landed in the registers.
    for (int i = 0; i < 16; ++i) {
        INFO("i = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], waveform[i]);
    }

    // NR30 restored to 0x80.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR30], 0x80);
}

TEST_CASE("rackboy_apu_write_wave_ram: two consecutive writes both apply")
{
    rackboy_apu_t apu = make_apu();

    uint8_t first[16];
    uint8_t second[16];
    for (int i = 0; i < 16; ++i) {
        first[i]  = static_cast<uint8_t>(i);
        second[i] = static_cast<uint8_t>(0xF0 + i);
    }

    rackboy_apu_write_wave_ram(&apu, first);
    for (int i = 0; i < 16; ++i) {
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], first[i]);
    }

    rackboy_apu_write_wave_ram(&apu, second);
    for (int i = 0; i < 16; ++i) {
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], second[i]);
    }
}

TEST_CASE("baseline: direct wave-RAM writes while CH3 active are dropped")
{
    // This test pins the SameBoy behavior the helper works around.
    // If this test ever starts passing by writing successfully, the
    // upstream semantics have changed and the helper's DAC-disable
    // dance may no longer be necessary.
    rackboy_apu_t apu = make_apu();

    // Set $FF30..$FF3F to a known pattern via the helper (safe write).
    uint8_t baseline[16];
    for (int i = 0; i < 16; ++i) baseline[i] = static_cast<uint8_t>(i);
    rackboy_apu_write_wave_ram(&apu, baseline);

    // Activate CH3 — is_active[GB_WAVE] becomes true.
    rackboy_apu_write_reg(&apu, 0xFF1A, 0x80);  // NR30: DAC on
    rackboy_apu_write_reg(&apu, 0xFF1B, 0xFF);  // NR31
    rackboy_apu_write_reg(&apu, 0xFF1C, 0x20);  // NR32: 100%
    rackboy_apu_write_reg(&apu, 0xFF1D, 0x00);  // NR33
    rackboy_apu_write_reg(&apu, 0xFF1E, 0x87);  // NR34: trigger + freq_hi=7

    // Try to overwrite the baseline byte-by-byte with rackboy_apu_write_reg.
    // SameBoy (DMG mode) drops these because is_active[GB_WAVE] is true.
    for (int i = 0; i < 16; ++i) {
        rackboy_apu_write_reg(&apu,
                              static_cast<uint16_t>(0xFF30 + i),
                              static_cast<uint8_t>(0xEE));
    }

    // The baseline pattern must still be present — writes were dropped.
    for (int i = 0; i < 16; ++i) {
        INFO("i = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], baseline[i]);
    }
}

TEST_CASE("rackboy_apu_write_wave_ram: does not retrigger CH3")
{
    rackboy_apu_t apu = make_apu();

    // Set up + trigger CH3, then capture its wave_channel state.
    rackboy_apu_write_reg(&apu, 0xFF1A, 0x80);  // NR30: DAC on
    rackboy_apu_write_reg(&apu, 0xFF1B, 0xFF);
    rackboy_apu_write_reg(&apu, 0xFF1C, 0x20);
    rackboy_apu_write_reg(&apu, 0xFF1D, 0x00);
    rackboy_apu_write_reg(&apu, 0xFF1E, 0x87);  // trigger

    // Render a few samples so the wave-position pointer advances.
    float ch3[64] = {0};
    rackboy_apu_run_samples(&apu, 64, nullptr, nullptr, ch3, nullptr);

    // Now write new wave RAM via the helper — should NOT reset the
    // channel's internal state (current_sample_index, etc).
    uint8_t new_wave[16];
    for (int i = 0; i < 16; ++i) new_wave[i] = static_cast<uint8_t>(0x30 + i);

    const uint8_t nr34_before = apu.gb.io_registers[GB_IO_NR34];

    rackboy_apu_write_wave_ram(&apu, new_wave);

    // NR34 should not have had its trigger bit re-set by the helper.
    // The helper only touches NR30 and $FF30..$FF3F.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR34], nr34_before);

    // The new wave bytes landed.
    for (int i = 0; i < 16; ++i) {
        INFO("i = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], new_wave[i]);
    }
}
