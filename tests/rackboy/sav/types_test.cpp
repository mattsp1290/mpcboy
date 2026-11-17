// tests/sav/types_test.cpp
//
// Unit tests for rackboy_sav/rackboy_sav_types.h — the POD schema for
// the audio-thread-owned rackboy bank (bead rackboy-3hk).
//
// The tests lock:
//   * the POD trait pair for every struct in the header;
//   * the reset helpers produce the documented "empty slot" state;
//   * the pool / table / wave sizing constants match the spec;
//   * RackboyInstrument is exactly 16 bytes (verifies no padding
//     surprise in the raw[16] layout);
//   * bank_reset on a deliberately-poisoned bank produces a clean
//     silent state across every field that matters to the audio
//     thread;
//   * sizeof(RackboyBank) stays in a sensible SRAM/SDRAM budget.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "doctest/doctest.h"

#include "rackboy_sav_types.h"

using rackboy::sav::bank_reset;
using rackboy::sav::instrument_reset;
using rackboy::sav::kInstrumentRecordBytes;
using rackboy::sav::kInstrumentTypeKit;
using rackboy::sav::kInstrumentTypeNoise;
using rackboy::sav::kInstrumentTypePulse;
using rackboy::sav::kInstrumentTypeWave;
using rackboy::sav::kMaxInstrumentsPerChannel;
using rackboy::sav::kMaxTables;
using rackboy::sav::kMaxWaveFrames;
using rackboy::sav::kNoTable;
using rackboy::sav::kNoWaveFrame;
using rackboy::sav::kTableRowCount;
using rackboy::sav::kWaveFrameBytes;
using rackboy::sav::RackboyBank;
using rackboy::sav::RackboyInstrument;
using rackboy::sav::RackboyTable;
using rackboy::sav::RackboyTableRow;
using rackboy::sav::RackboyWaveFrame;
using rackboy::sav::table_reset;
using rackboy::sav::wave_frame_reset;

TEST_CASE("sizing constants match the binding wire contract")
{
    CHECK_EQ(kMaxInstrumentsPerChannel, std::size_t{64});
    CHECK_EQ(kInstrumentRecordBytes,    std::size_t{16});
    CHECK_EQ(kMaxTables,                std::size_t{32});
    CHECK_EQ(kMaxWaveFrames,            std::size_t{255});  // LSDJ WAVE_COUNT
    CHECK_EQ(kTableRowCount,            std::size_t{16});
    CHECK_EQ(kWaveFrameBytes,           std::size_t{16});
}

TEST_CASE("LSDJ instrument type byte constants match the spike parser")
{
    // Source: rackboy_sav/spike/spike_parser.cpp kExpectedInstruments,
    // docs/COMPATIBILITY.md per-type instrument table.
    CHECK_EQ(kInstrumentTypePulse, std::uint8_t{0x00});
    CHECK_EQ(kInstrumentTypeWave,  std::uint8_t{0x01});
    CHECK_EQ(kInstrumentTypeKit,   std::uint8_t{0x02});
    CHECK_EQ(kInstrumentTypeNoise, std::uint8_t{0x03});
}

TEST_CASE("sentinels match LSDJ's wire-level 0xFF convention")
{
    // docs/COMPATIBILITY.md pins WAVE_COUNT = 255 with slot 0xFF
    // reserved. We use the same 0xFF value for the "no table"
    // sentinel for consistency — the static_asserts in
    // rackboy_sav_types.h prevent collision with any real slot.
    CHECK_EQ(kNoTable,     std::uint8_t{0xFF});
    CHECK_EQ(kNoWaveFrame, std::uint8_t{0xFF});
}

TEST_CASE("POD trait pair holds for every audio-thread struct")
{
    // These are the same assertions the header carries as
    // static_asserts. Duplicating them at runtime gives a test-level
    // failure signal if a future toolchain disables static_assert
    // diagnostics and lets a non-POD change slip in.
    CHECK(std::is_trivially_copyable<RackboyInstrument>::value);
    CHECK(std::is_standard_layout<RackboyInstrument>::value);

    CHECK(std::is_trivially_copyable<RackboyTableRow>::value);
    CHECK(std::is_standard_layout<RackboyTableRow>::value);

    CHECK(std::is_trivially_copyable<RackboyTable>::value);
    CHECK(std::is_standard_layout<RackboyTable>::value);

    CHECK(std::is_trivially_copyable<RackboyWaveFrame>::value);
    CHECK(std::is_standard_layout<RackboyWaveFrame>::value);

    CHECK(std::is_trivially_copyable<RackboyBank>::value);
    CHECK(std::is_standard_layout<RackboyBank>::value);
}

TEST_CASE("RackboyInstrument is exactly kInstrumentRecordBytes wide")
{
    // The raw[16] layout must have zero padding so a parser memcpy
    // from the decompressed song's 16-byte record maps byte-for-byte.
    CHECK_EQ(sizeof(RackboyInstrument), kInstrumentRecordBytes);
}

TEST_CASE("instrument_reset zeroes every byte of the raw record")
{
    RackboyInstrument inst{};
    // Poison every byte with a distinct non-zero pattern.
    for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
        inst.raw[k] = static_cast<std::uint8_t>(0xA0 | k);
    }

    instrument_reset(inst);

    for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
        CAPTURE(k);
        CHECK_EQ(inst.raw[k], 0);
    }
}

TEST_CASE("table_reset zeroes every row and clears occupancy")
{
    RackboyTable table{};
    for (std::size_t r = 0; r < kTableRowCount; ++r) {
        table.rows[r].transpose  = static_cast<std::int8_t>(r);
        table.rows[r].cmd1       = 0xA0 + static_cast<std::uint8_t>(r);
        table.rows[r].cmd1_param = 0xB0 + static_cast<std::uint8_t>(r);
        table.rows[r].cmd2       = 0xC0 + static_cast<std::uint8_t>(r);
        table.rows[r].cmd2_param = 0xD0 + static_cast<std::uint8_t>(r);
    }
    table.occupied = true;

    table_reset(table);

    for (std::size_t r = 0; r < kTableRowCount; ++r) {
        CAPTURE(r);
        CHECK_EQ(table.rows[r].transpose,  0);
        CHECK_EQ(table.rows[r].cmd1,       0);
        CHECK_EQ(table.rows[r].cmd1_param, 0);
        CHECK_EQ(table.rows[r].cmd2,       0);
        CHECK_EQ(table.rows[r].cmd2_param, 0);
    }
    CHECK_FALSE(table.occupied);
}

TEST_CASE("wave_frame_reset zeroes the 16 sample bytes and clears occupancy")
{
    RackboyWaveFrame wave{};
    for (std::size_t i = 0; i < kWaveFrameBytes; ++i) {
        wave.samples[i] = static_cast<std::uint8_t>(0x80 | i);
    }
    wave.occupied = true;

    wave_frame_reset(wave);

    for (std::size_t i = 0; i < kWaveFrameBytes; ++i) {
        CAPTURE(i);
        CHECK_EQ(wave.samples[i], 0);
    }
    CHECK_FALSE(wave.occupied);
}

TEST_CASE("bank_reset clears every pool, count, table, wave, and loaded flag")
{
    // Pre-poison the bank with non-zero raw bytes, non-zero counts,
    // table content, and wave samples so a trivial zero-init cannot
    // silently pass the test.
    RackboyBank bank{};
    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
            bank.pulse1_pool[i].raw[k] = 0xF0;
            bank.pulse2_pool[i].raw[k] = 0xF1;
            bank.wave_pool  [i].raw[k] = 0xF2;
            bank.noise_pool [i].raw[k] = 0xF3;
        }
    }
    bank.pulse1_count = 13;
    bank.pulse2_count = 7;
    bank.wave_count   = 3;
    bank.noise_count  = 2;

    for (std::size_t t = 0; t < kMaxTables; ++t) {
        bank.tables[t].occupied          = true;
        bank.tables[t].rows[0].transpose = 12;
    }
    for (std::size_t w = 0; w < kMaxWaveFrames; ++w) {
        bank.waves[w].occupied   = true;
        bank.waves[w].samples[0] = 0xFF;
    }
    bank.loaded = true;

    bank_reset(bank);

    // Every instrument slot is zero bytes.
    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
            CAPTURE(i);
            CAPTURE(k);
            CHECK_EQ(bank.pulse1_pool[i].raw[k], 0);
            CHECK_EQ(bank.pulse2_pool[i].raw[k], 0);
            CHECK_EQ(bank.wave_pool  [i].raw[k], 0);
            CHECK_EQ(bank.noise_pool [i].raw[k], 0);
        }
    }

    // Every count is zero.
    CHECK_EQ(bank.pulse1_count, 0);
    CHECK_EQ(bank.pulse2_count, 0);
    CHECK_EQ(bank.wave_count,   0);
    CHECK_EQ(bank.noise_count,  0);

    // Every table slot is empty.
    for (std::size_t t = 0; t < kMaxTables; ++t) {
        CAPTURE(t);
        CHECK_FALSE(bank.tables[t].occupied);
        CHECK_EQ(bank.tables[t].rows[0].transpose, 0);
    }

    // Every wave frame slot is empty.
    for (std::size_t w = 0; w < kMaxWaveFrames; ++w) {
        CAPTURE(w);
        CHECK_FALSE(bank.waves[w].occupied);
        CHECK_EQ(bank.waves[w].samples[0], 0);
    }

    // Bank as a whole is unloaded.
    CHECK_FALSE(bank.loaded);
}

TEST_CASE("RackboyBank size fits in the Daisy SRAM / SDRAM budget")
{
    // Budget ceiling: 64 KiB. This is comfortably smaller than
    // Daisy's 64 MB SDRAM and also smaller than the STM32H750's
    // 512 KiB internal SRAM — so the same struct can live in either
    // memory region without re-auditing the size math. The 255-slot
    // wave frame pool pushes the measured size up from iteration 25's
    // initial ~4.8 KiB to the 8-10 KiB range, which is still well
    // under the ceiling.
    CHECK_LE(sizeof(RackboyBank), std::size_t{64 * 1024});

    // Lower bound on the same expression: if the struct ever shrinks
    // below a few KB, someone probably dropped one of the pools.
    CHECK_GE(sizeof(RackboyBank), std::size_t{4 * 1024});

    // Exact size captured for future-drift detection. Measured on
    // macOS clang++ 16 with -std=c++17 at the end of iteration 25's
    // review-fix pass. A future compiler / ABI change that alters
    // this value will fire the CHECK and force an explicit re-audit
    // rather than a silent drift. Update the constant below when a
    // schema change is deliberate.
    constexpr std::size_t kExpectedBankSize = 11028;
    CHECK_EQ(sizeof(RackboyBank), kExpectedBankSize);
}
