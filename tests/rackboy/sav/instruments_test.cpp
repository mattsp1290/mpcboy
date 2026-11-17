// tests/sav/instruments_test.cpp
//
// Unit tests for rackboy_sav/rackboy_sav_instruments.{h,cpp} —
// the instrument block parser for rackboy-1ub.
//
// Tests build a synthetic 32 KiB "decompressed song" blob in
// memory and stamp only the bytes the parser actually reads
// (alloc map + instrument records). Every error path, every
// type-byte classification, and the "bank left in reset state on
// failure" contract are locked by dedicated TEST_CASEs.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "doctest/doctest.h"

#include "rackboy_sav_instruments.h"
#include "rackboy_sav_types.h"

using rackboy::sav::bank_reset;
using rackboy::sav::instruments_error_string;
using rackboy::sav::InstrumentsError;
using rackboy::sav::kDecompressedSongSizeBytes;
using rackboy::sav::kInstrumentAllocated;
using rackboy::sav::kInstrumentAllocMapLen;
using rackboy::sav::kInstrumentAllocMapOffset;
using rackboy::sav::kInstrumentParamsOffset;
using rackboy::sav::kInstrumentRecordBytes;
using rackboy::sav::kInstrumentSlotStride;
using rackboy::sav::kInstrumentTypeKit;
using rackboy::sav::kInstrumentTypeNoise;
using rackboy::sav::kInstrumentTypePulse;
using rackboy::sav::kInstrumentTypeWave;
using rackboy::sav::kInstrumentUnallocated;
using rackboy::sav::kMaxInstrumentsPerChannel;
using rackboy::sav::parse_instruments;
using rackboy::sav::RackboyBank;

namespace {

// Build a zero-filled 32 KiB "decompressed song" blob. All 64
// slots start as unallocated. Callers stamp per-slot state via
// `allocate_slot`.
std::vector<std::uint8_t> make_empty_song()
{
    return std::vector<std::uint8_t>(kDecompressedSongSizeBytes, 0);
}

// Mark a slot as allocated and write the given 16-byte record.
// `record_seed` is a per-byte seed so different slot indices
// produce distinct record patterns without the test needing to
// spell out 16 bytes every time. The pattern is deliberately
// additive-with-a-+1 bias rather than XOR: XOR can produce a
// zero byte for unlucky (seed, k) pairs, and a zero byte in the
// diagnostic is indistinguishable from an unwritten
// (post-bank_reset) byte — which makes a mismatch failure
// confusing. `seed + k*13 + 1` is collision-free with any
// zero-init and distinct per (seed, k) within a single record.
void allocate_slot(std::vector<std::uint8_t>& song,
                   std::size_t                slot,
                   std::uint8_t               type_byte,
                   std::uint8_t               record_seed)
{
    song[kInstrumentAllocMapOffset + slot] = kInstrumentAllocated;
    const std::size_t record_offset =
        kInstrumentParamsOffset + slot * kInstrumentSlotStride;
    song[record_offset + 0] = type_byte;
    for (std::size_t k = 1; k < kInstrumentRecordBytes; ++k) {
        song[record_offset + k] =
            static_cast<std::uint8_t>(record_seed + (k * 13) + 1);
    }
}

// Compare a pool slot's `raw` bytes to the deterministic pattern
// `allocate_slot` would have written for the given seed. Used to
// prove the parser memcpy lands the correct record in the correct
// pool index, not just any record of the right type.
void check_record_matches_seed(const std::uint8_t (&raw)[kInstrumentRecordBytes],
                               std::uint8_t        type_byte,
                               std::uint8_t        record_seed)
{
    CHECK_EQ(raw[0], type_byte);
    for (std::size_t k = 1; k < kInstrumentRecordBytes; ++k) {
        CAPTURE(k);
        const std::uint8_t expected =
            static_cast<std::uint8_t>(record_seed + (k * 13) + 1);
        CHECK_EQ(raw[k], expected);
    }
}

// Convenience: confirm a bank matches the "freshly reset" state
// (no allocated instruments, not loaded, every slot byte zero).
// Lets the error-path tests CHECK one invariant instead of
// spelling out the count/loaded checks inline every time.
void expect_bank_is_reset(const RackboyBank& b)
{
    CHECK_FALSE(b.loaded);
    CHECK_EQ(b.pulse1_count, 0);
    CHECK_EQ(b.pulse2_count, 0);
    CHECK_EQ(b.wave_count,   0);
    CHECK_EQ(b.noise_count,  0);
}

}  // namespace

TEST_CASE("parse_instruments rejects null out_bank")
{
    auto song = make_empty_song();
    CHECK_EQ(parse_instruments(song.data(), song.size(), nullptr),
             InstrumentsError::NullOutBank);
}

TEST_CASE("parse_instruments rejects null song buffer and resets the bank")
{
    RackboyBank bank{};
    // Poison the bank so we can prove it gets reset on failure.
    bank.pulse1_count = 42;
    bank.loaded       = true;

    CHECK_EQ(parse_instruments(nullptr, kDecompressedSongSizeBytes, &bank),
             InstrumentsError::NullSong);
    expect_bank_is_reset(bank);
}

TEST_CASE("parse_instruments rejects a too-short song buffer")
{
    std::vector<std::uint8_t> short_song(kDecompressedSongSizeBytes - 1, 0);
    RackboyBank bank{};
    bank.pulse1_count = 7;
    bank.loaded       = true;

    CHECK_EQ(parse_instruments(short_song.data(), short_song.size(), &bank),
             InstrumentsError::SongTooShort);
    expect_bank_is_reset(bank);
}

TEST_CASE("parse_instruments accepts a buffer larger than 32 KiB "
          "(reads only the first 32 KiB)")
{
    // An oversized buffer should be fine — the header contract
    // says only the first kDecompressedSongSizeBytes are read.
    std::vector<std::uint8_t> over_song(kDecompressedSongSizeBytes * 2, 0);
    // Allocate one Pulse in the first 32 KiB.
    allocate_slot(over_song, /*slot=*/3, kInstrumentTypePulse, 0x42);

    // Stamp some "garbage" past the 32 KiB boundary that MUST not
    // affect the result. If the parser accidentally reads past
    // the boundary, the types will not classify cleanly.
    for (std::size_t i = kDecompressedSongSizeBytes;
         i < over_song.size(); ++i) {
        over_song[i] = 0xAB;
    }

    RackboyBank bank{};
    CHECK_EQ(parse_instruments(over_song.data(), over_song.size(), &bank),
             InstrumentsError::Ok);
    CHECK_EQ(bank.pulse1_count, 1);
    CHECK_EQ(bank.wave_count,   0);
    CHECK_EQ(bank.noise_count,  0);
    CHECK(bank.loaded);
    check_record_matches_seed(bank.pulse1_pool[0].raw,
                              kInstrumentTypePulse, 0x42);
}

TEST_CASE("parse_instruments rejects an alloc-map byte that is neither 0 nor 1")
{
    auto song = make_empty_song();
    // Stamp a bogus alloc-map byte at slot 5.
    song[kInstrumentAllocMapOffset + 5] = 0x42;

    RackboyBank bank{};
    bank.pulse1_count = 3;  // poisoned
    bank.loaded       = true;

    CHECK_EQ(parse_instruments(song.data(), song.size(), &bank),
             InstrumentsError::AllocMapInvalid);
    expect_bank_is_reset(bank);
}

TEST_CASE("parse_instruments rejects a slot whose type byte is unknown")
{
    auto song = make_empty_song();
    // Allocate slot 0 as a "valid" Pulse so the alloc-map pass
    // doesn't trip. Then allocate slot 1 with a garbage type
    // byte that doesn't match any of the four LSDJ types.
    allocate_slot(song, /*slot=*/0, kInstrumentTypePulse, 0x10);
    allocate_slot(song, /*slot=*/1, /*type=*/0x77,         0x20);

    RackboyBank bank{};
    CHECK_EQ(parse_instruments(song.data(), song.size(), &bank),
             InstrumentsError::UnknownType);
    // The parser resets the bank on failure — slot 0's contents
    // must NOT leak through. This locks the "no half-populated"
    // invariant.
    expect_bank_is_reset(bank);
}

TEST_CASE("parse_instruments ignores record bytes for unallocated slots")
{
    // Locks the "alloc-map gates the per-record read" invariant.
    // A future refactor that walks all 64 slots blindly without
    // consulting the alloc map would fail this test instantly.
    auto song = make_empty_song();
    // Allocate slot 0 only (Pulse).
    allocate_slot(song, /*slot=*/0, kInstrumentTypePulse, 0xA0);
    // Stamp a "Wave" type byte at slot 1's record area WITHOUT
    // marking slot 1 as allocated. The parser MUST NOT count it.
    const std::size_t slot1_offset =
        kInstrumentParamsOffset + 1 * kInstrumentSlotStride;
    song[slot1_offset] = kInstrumentTypeWave;

    RackboyBank bank{};
    REQUIRE_EQ(parse_instruments(song.data(), song.size(), &bank),
               InstrumentsError::Ok);
    CHECK(bank.loaded);
    CHECK_EQ(bank.pulse1_count, 1);
    CHECK_EQ(bank.wave_count,   0);
    CHECK_EQ(bank.noise_count,  0);
}

TEST_CASE("parse_instruments classifies a mixed slot layout into per-channel pools")
{
    auto song = make_empty_song();
    // Slots 0 & 2: Pulse (different seeds to prove we copy the
    // right record to the right pool index).
    allocate_slot(song, /*slot=*/0, kInstrumentTypePulse, 0xA0);
    allocate_slot(song, /*slot=*/2, kInstrumentTypePulse, 0xA2);
    // Slot 5: Wave.
    allocate_slot(song, /*slot=*/5, kInstrumentTypeWave,  0xB5);
    // Slot 7: Noise.
    allocate_slot(song, /*slot=*/7, kInstrumentTypeNoise, 0xC7);
    // Slot 9: Kit — deliberately skipped by the parser (not an error).
    allocate_slot(song, /*slot=*/9, kInstrumentTypeKit,   0xD9);

    RackboyBank bank{};
    CHECK_EQ(parse_instruments(song.data(), song.size(), &bank),
             InstrumentsError::Ok);
    CHECK(bank.loaded);

    // Pulse pool: 2 entries, in allocation-map order (slot 0 at
    // pulse1_pool[0], slot 2 at pulse1_pool[1]).
    CHECK_EQ(bank.pulse1_count, 2);
    check_record_matches_seed(bank.pulse1_pool[0].raw,
                              kInstrumentTypePulse, 0xA0);
    check_record_matches_seed(bank.pulse1_pool[1].raw,
                              kInstrumentTypePulse, 0xA2);

    // Pulse2 is empty in v1 (chain-based split is a future bead).
    CHECK_EQ(bank.pulse2_count, 0);

    // Wave pool: 1 entry.
    CHECK_EQ(bank.wave_count, 1);
    check_record_matches_seed(bank.wave_pool[0].raw,
                              kInstrumentTypeWave, 0xB5);

    // Noise pool: 1 entry.
    CHECK_EQ(bank.noise_count, 1);
    check_record_matches_seed(bank.noise_pool[0].raw,
                              kInstrumentTypeNoise, 0xC7);

    // Kit is skipped — not an error, not in any pool, no count.
    // The slot past the end of each count must remain zeroed
    // (from the initial bank_reset inside parse_instruments).
    CHECK_EQ(bank.pulse1_pool[bank.pulse1_count].raw[0], 0);
    CHECK_EQ(bank.wave_pool  [bank.wave_count  ].raw[0], 0);
    CHECK_EQ(bank.noise_pool [bank.noise_count ].raw[0], 0);
}

TEST_CASE("parse_instruments handles an all-unallocated song (empty bank, loaded=true)")
{
    // The alloc map is all zeros -> no slots allocated. This is
    // a valid song layout (an LSDJ project with zero instruments)
    // and must succeed with empty pools and `loaded == true`.
    auto song = make_empty_song();

    RackboyBank bank{};
    CHECK_EQ(parse_instruments(song.data(), song.size(), &bank),
             InstrumentsError::Ok);
    CHECK(bank.loaded);
    CHECK_EQ(bank.pulse1_count, 0);
    CHECK_EQ(bank.pulse2_count, 0);
    CHECK_EQ(bank.wave_count,   0);
    CHECK_EQ(bank.noise_count,  0);
}

TEST_CASE("parse_instruments handles an all-Kit song (all skipped, loaded=true)")
{
    // Every slot is a Kit — none land in the per-channel pools,
    // but the parse itself is successful and `loaded == true`.
    // This exercises the "Kit is a skip, not an error" branch on
    // every slot in one go.
    auto song = make_empty_song();
    for (std::size_t slot = 0; slot < kInstrumentAllocMapLen; ++slot) {
        allocate_slot(song, slot, kInstrumentTypeKit,
                      static_cast<std::uint8_t>(0xE0 + slot));
    }

    RackboyBank bank{};
    CHECK_EQ(parse_instruments(song.data(), song.size(), &bank),
             InstrumentsError::Ok);
    CHECK(bank.loaded);
    CHECK_EQ(bank.pulse1_count, 0);
    CHECK_EQ(bank.pulse2_count, 0);
    CHECK_EQ(bank.wave_count,   0);
    CHECK_EQ(bank.noise_count,  0);
}

TEST_CASE("parse_instruments fills all 64 slots when they are all Pulse")
{
    // Total instrument count is LSDJ-capped at 64 across ALL
    // channels; if every slot is Pulse they all land in
    // pulse1_pool. This exercises the high-count boundary without
    // tripping ChannelPoolFull, because pulse1_pool is sized to
    // kMaxInstrumentsPerChannel == 64.
    auto song = make_empty_song();
    for (std::size_t slot = 0; slot < kInstrumentAllocMapLen; ++slot) {
        allocate_slot(song, slot, kInstrumentTypePulse,
                      static_cast<std::uint8_t>(slot + 1));
    }

    RackboyBank bank{};
    CHECK_EQ(parse_instruments(song.data(), song.size(), &bank),
             InstrumentsError::Ok);
    CHECK(bank.loaded);
    CHECK_EQ(bank.pulse1_count, kMaxInstrumentsPerChannel);
    CHECK_EQ(bank.wave_count,   0);
    CHECK_EQ(bank.noise_count,  0);
    // Spot-check: slot 0's record lands at pulse1_pool[0], slot 63's at [63].
    check_record_matches_seed(bank.pulse1_pool[0].raw,
                              kInstrumentTypePulse, 0x01);
    check_record_matches_seed(bank.pulse1_pool[63].raw,
                              kInstrumentTypePulse, 0x40);
}

TEST_CASE("instruments_error_string returns a non-null, distinct, "
          "non-empty string for every error")
{
    const InstrumentsError all[] = {
        InstrumentsError::Ok,
        InstrumentsError::NullSong,
        InstrumentsError::NullOutBank,
        InstrumentsError::SongTooShort,
        InstrumentsError::AllocMapInvalid,
        InstrumentsError::UnknownType,
        InstrumentsError::ChannelPoolFull,
    };
    constexpr std::size_t kAllCount = sizeof(all) / sizeof(all[0]);

    const char* seen[kAllCount] = {};
    std::size_t seen_count = 0;
    for (InstrumentsError e : all) {
        const char* s = instruments_error_string(e);
        REQUIRE(s != nullptr);
        CHECK(s[0] != '\0');
        for (std::size_t i = 0; i < seen_count; ++i) {
            CAPTURE(i);
            CHECK(std::strcmp(seen[i], s) != 0);
        }
        seen[seen_count++] = s;
    }
    CHECK_EQ(seen_count, kAllCount);
}
