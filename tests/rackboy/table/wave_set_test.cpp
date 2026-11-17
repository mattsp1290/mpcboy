// tests/table/wave_set_test.cpp
//
// Integration tests for the W (wave-set) table command (bead rackboy-8tz).
//
// Per docs/LSDJ_REFERENCE.md, W is a CH3-only command that loads a named
// wave frame from the shared wave pool into CH3 wave RAM ($FF30..$FF3F).
// rackboy implements W by calling rackboy_apu_write_wave_ram() (bead
// rackboy-9i0) which performs the canonical DAC-disable / write / restore
// dance so the 16 bytes land even if CH3 was actively playing.
//
// Verifies:
//   - W on CH3 with a valid occupied wave slot writes all 16 bytes to
//     $FF30..$FF3F in order.
//   - W on non-CH3 channels (Pulse1, Pulse2, Noise) is a no-op: wave RAM
//     is unchanged.
//   - W with a null bank is a no-op (the caller-contract escape hatch for
//     test paths that never expect a W command on the current row).
//   - W with param >= kMaxWaveFrames is a no-op (out-of-range slot index).
//   - W pointing at an unoccupied wave slot is a no-op (parser never wrote
//     it — silently skipping is better than zeroing the wave and muting CH3).
//   - W does NOT retrigger CH3 — NR34 bit 7 is not set by the W handler
//     (the helper's docstring guarantees no trigger, and the dispatcher
//     does not add one).
//   - W works when placed in cmd2 as well as cmd1 (both slots dispatch).

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
    inst.raw[1] = 0xF0;
    inst.raw[7] = 0x80;  // 50% duty
    return inst;
}

static rackboy::sav::RackboyInstrument make_wave_inst()
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeWave;
    inst.raw[1] = 0x20;  // NR32 output level = 100%
    return inst;
}

static rackboy::sav::RackboyInstrument make_noise_inst()
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeNoise;
    inst.raw[1] = 0xF0;
    inst.raw[4] = 0x55;
    return inst;
}

// Populate bank->waves[slot] with a recognizable 16-byte pattern.
static void set_wave_slot(rackboy::sav::RackboyBank& bank,
                          std::size_t slot,
                          uint8_t seed)
{
    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        bank.waves[slot].samples[i] = static_cast<uint8_t>(seed + i);
    }
    bank.waves[slot].occupied = true;
}

static rackboy::sav::RackboyTable make_table_with_w_cmd(uint8_t param)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 0;
    tbl.rows[0].cmd1 = 'W';
    tbl.rows[0].cmd1_param = param;
    return tbl;
}

static rackboy::sav::RackboyTable make_table_with_w_in_cmd2(uint8_t param)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 0;
    // cmd1 is a no-op (0x00 triggers the "skip empty slot" path in
    // table_tick). We deliberately avoid putting another real command
    // in cmd1 so any bug where only cmd1 dispatches would be caught by
    // the cmd2-only test case rather than masked by a side-effecting
    // cmd1 that happens to look the same.
    tbl.rows[0].cmd1 = 0x00;
    tbl.rows[0].cmd1_param = 0x00;
    tbl.rows[0].cmd2 = 'W';
    tbl.rows[0].cmd2_param = param;
    return tbl;
}

TEST_CASE("W on CH3: writes all 16 bytes of the selected wave frame to $FF30..$FF3F")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

    // Populate wave slot 7 with the pattern 0x30, 0x31, ..., 0x3F so
    // each byte is unique and recognizable.
    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    set_wave_slot(bank, 7, 0x30);

    auto tbl = make_table_with_w_cmd(7);
    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, &bank);

    for (int i = 0; i < 16; ++i) {
        INFO("byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i],
                 static_cast<uint8_t>(0x30 + i));
    }
}

TEST_CASE("W does not set NR34 trigger bit (wave swap without retrigger)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

    // Clear NR34 AFTER trigger so we can detect any spurious write by the
    // W handler. table_tick()'s Step 4 will write NR33/NR34 (pitch only,
    // no trigger bit) for the zero-transpose row; the bit we must verify
    // is absent is the trigger bit (0x80), which only table_trigger() and
    // the E / R retrigger commands may set.
    apu.gb.io_registers[GB_IO_NR34] = 0x00;

    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    set_wave_slot(bank, 3, 0xC0);

    auto tbl = make_table_with_w_cmd(3);
    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, &bank);

    CHECK_EQ(apu.gb.io_registers[GB_IO_NR34] & 0x80, 0x00);
}

TEST_CASE("W on Pulse1 is a no-op (wave RAM untouched)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Snapshot wave RAM BEFORE the tick. rackboy_apu_init zero-fills the
    // io_registers, so the baseline is all-zero, but we snapshot anyway
    // to keep the test robust against any future APU init that might
    // seed a non-zero wave table.
    uint8_t before[rackboy::sav::kWaveFrameBytes];
    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        before[i] = apu.gb.io_registers[GB_IO_WAV_START + i];
    }

    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    set_wave_slot(bank, 0, 0xAA);

    auto tbl = make_table_with_w_cmd(0);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, &bank);

    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        INFO("byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], before[i]);
    }
}

TEST_CASE("W on Pulse2 is a no-op (wave RAM untouched)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse2, inst, 0, &apu);

    uint8_t before[rackboy::sav::kWaveFrameBytes];
    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        before[i] = apu.gb.io_registers[GB_IO_WAV_START + i];
    }

    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    set_wave_slot(bank, 2, 0xBB);

    auto tbl = make_table_with_w_cmd(2);
    table_tick(voice, regs, VoiceChannel::Pulse2, tbl, &apu, &bank);

    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        INFO("byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], before[i]);
    }
}

TEST_CASE("W on Noise is a no-op (wave RAM untouched)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_noise_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);

    uint8_t before[rackboy::sav::kWaveFrameBytes];
    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        before[i] = apu.gb.io_registers[GB_IO_WAV_START + i];
    }

    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    set_wave_slot(bank, 4, 0xCC);

    auto tbl = make_table_with_w_cmd(4);
    table_tick(voice, regs, VoiceChannel::Noise, tbl, &apu, &bank);

    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        INFO("byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], before[i]);
    }
}

TEST_CASE("W with null bank is a no-op")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

    // Wave RAM starts empty. If the W handler failed to check for a null
    // bank it would dereference a pool pointer and either crash or write
    // garbage — this test's survival proves the null-bank guard works.
    auto tbl = make_table_with_w_cmd(5);
    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, /*bank=*/nullptr);

    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        INFO("byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], 0x00);
    }
}

TEST_CASE("W with param equal to kMaxWaveFrames is a no-op (out-of-range index)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    // Populate every slot so a bug that indexed past kMaxWaveFrames would
    // read uninitialized / out-of-bounds memory rather than a zero bank.
    for (std::size_t s = 0; s < rackboy::sav::kMaxWaveFrames; ++s) {
        set_wave_slot(bank, s, static_cast<uint8_t>(s));
    }

    // param == kMaxWaveFrames is the smallest out-of-range index: it is
    // not less than kMaxWaveFrames, so the handler must not index
    // bank.waves[kMaxWaveFrames] (one past the end of the pool). The
    // static_assert pins the assumption that the value still fits in a
    // uint8_t — the `W` param is a single byte in the LSDJ table row,
    // so if kMaxWaveFrames ever grows past 255 the test itself becomes
    // unreachable and needs rethinking rather than a silent truncation.
    static_assert(rackboy::sav::kMaxWaveFrames <= 255,
                  "W's out-of-range test uses kMaxWaveFrames as a uint8_t "
                  "cmd param; if kMaxWaveFrames grows past 255 the test "
                  "must be rewritten (the param would truncate).");
    constexpr uint8_t kOutOfRangeWaveSlot =
        static_cast<uint8_t>(rackboy::sav::kMaxWaveFrames);

    auto tbl = make_table_with_w_cmd(kOutOfRangeWaveSlot);
    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, &bank);

    // Wave RAM must be unchanged (all zeros) — the out-of-range param
    // did not cause a write.
    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        INFO("byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], 0x00);
    }
}

TEST_CASE("W pointing at unoccupied wave slot is a no-op")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

    // bank_reset leaves every wave slot with occupied == false and all
    // samples == 0. If the W handler skipped the occupied check it would
    // still write 16 zero bytes — but the wave RAM already contains zeros
    // from init, so that's indistinguishable from a true no-op here. To
    // force the issue, seed the wave RAM with a non-zero pattern via the
    // helper so we can detect any subsequent zeroing by W.
    uint8_t prefill[rackboy::sav::kWaveFrameBytes];
    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        prefill[i] = static_cast<uint8_t>(0xE0 + i);
    }
    rackboy_apu_write_wave_ram(&apu, prefill);

    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    // Slot 10 stays unoccupied (the default after bank_reset).

    auto tbl = make_table_with_w_cmd(10);
    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, &bank);

    for (std::size_t i = 0; i < rackboy::sav::kWaveFrameBytes; ++i) {
        INFO("byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i], prefill[i]);
    }
}

TEST_CASE("W in cmd2 slot dispatches (both slots work)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    set_wave_slot(bank, 9, 0x50);

    auto tbl = make_table_with_w_in_cmd2(9);
    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, &bank);

    for (int i = 0; i < 16; ++i) {
        INFO("byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i],
                 static_cast<uint8_t>(0x50 + i));
    }
}

TEST_CASE("W swaps waveform mid-note without retriggering (two successive W ticks)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    set_wave_slot(bank, 1, 0x10);
    set_wave_slot(bank, 2, 0x80);

    // Two-row table: row 0 = W 1, row 1 = W 2. After two ticks, wave RAM
    // must hold the second pattern. In between, NR34 must not carry a
    // trigger bit — the canonical rackboy "wave swap without retrigger"
    // semantics documented by the W command handler.
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].cmd1 = 'W';
    tbl.rows[0].cmd1_param = 1;
    tbl.rows[1].cmd1 = 'W';
    tbl.rows[1].cmd1_param = 2;

    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, &bank);
    for (int i = 0; i < 16; ++i) {
        INFO("after row 0, byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i],
                 static_cast<uint8_t>(0x10 + i));
    }

    // Clear any NR34 state before row 1 so we can assert no retrigger.
    apu.gb.io_registers[GB_IO_NR34] = 0x00;
    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, &bank);
    for (int i = 0; i < 16; ++i) {
        INFO("after row 1, byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i],
                 static_cast<uint8_t>(0x80 + i));
    }
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR34] & 0x80, 0x00);
}

TEST_CASE("W on a row with a non-zero transpose: pitch updates AND wave swaps")
{
    // Pins down the Step 4 (pitch write) → Step 5 (command dispatch)
    // ordering inside table_tick(): on a row that carries both a
    // transpose and a `W` command, the pitch must already have been
    // written to NR33/NR34 by the time the `W` handler runs. A future
    // refactor that reordered command dispatch ahead of the pitch write
    // would leave NR33/NR34 at their trigger-time values for this row,
    // and this test would catch that regression.

    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

    // Capture the NR33/NR34 values immediately after trigger so we can
    // detect the pitch write for the transposed row by a change in the
    // register bytes.
    const uint8_t nr33_at_trigger = apu.gb.io_registers[GB_IO_NR33];
    const uint8_t nr34_at_trigger = apu.gb.io_registers[GB_IO_NR34];

    rackboy::sav::RackboyBank bank;
    rackboy::sav::bank_reset(bank);
    set_wave_slot(bank, 12, 0x40);

    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose = 7;         // +7 semitones
    tbl.rows[0].cmd1 = 'W';
    tbl.rows[0].cmd1_param = 12;

    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, &bank);

    // The pitch_offset in the register cache must reflect the transpose
    // — this is the direct consequence of Step 4 running.
    CHECK_EQ(regs.pitch_offset, 7);

    // And the NRx3/NRx4 freq registers must have been rewritten for the
    // new pitch. We assert "at least one of NR33/NR34 differs from the
    // trigger-time value" rather than a specific encoded value — that
    // keeps this test independent of the pitch_wave_11bit() semitone →
    // frequency LUT and focused on the ordering invariant. The `||` is
    // wrapped in a local bool because doctest's CHECK captures the
    // expression for pretty-printing and chokes on chained comparisons.
    const uint8_t nr33_after = apu.gb.io_registers[GB_IO_NR33];
    const uint8_t nr34_after = apu.gb.io_registers[GB_IO_NR34];
    const bool pitch_register_changed =
        (nr33_after != nr33_at_trigger) || (nr34_after != nr34_at_trigger);
    CHECK(pitch_register_changed);

    // NR34 must not carry a trigger bit — pitch-only freq writes in
    // table_tick() omit it, and `W` does not add one.
    CHECK_EQ(nr34_after & 0x80, 0x00);

    // Wave RAM must now hold slot 12's pattern — the `W` dispatched.
    for (int i = 0; i < 16; ++i) {
        INFO("byte index = ", i);
        CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_START + i],
                 static_cast<uint8_t>(0x40 + i));
    }
}
