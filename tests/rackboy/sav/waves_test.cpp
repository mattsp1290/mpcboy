// tests/sav/waves_test.cpp
//
// Unit tests for rackboy_sav/rackboy_sav_waves.{h,cpp} — the
// CH3 wave frame pool parser for rackboy-ntq.
//
// Tests build a synthetic 32 KiB "decompressed song" blob and
// stamp only the bytes the parser reads (the 4080-byte wave
// pool at 0x6000). Lock every error code, the "all-zero ->
// unoccupied / any-nonzero -> occupied" occupancy rule, and
// the "does not touch neighbor bank state" cross-parser
// invariant.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "doctest/doctest.h"

#include "rackboy_sav_instruments.h"  // kDecompressedSongSizeBytes
#include "rackboy_sav_types.h"
#include "rackboy_sav_waves.h"

using rackboy::sav::kDecompressedSongSizeBytes;
using rackboy::sav::kMaxWaveFrames;
using rackboy::sav::kTotalWaveFrameSlots;
using rackboy::sav::kWaveFrameBytes;
using rackboy::sav::kWaveFramePoolOffset;
using rackboy::sav::kWaveFrameSlotStride;
using rackboy::sav::parse_waves;
using rackboy::sav::RackboyBank;
using rackboy::sav::RackboyWaveFrame;
using rackboy::sav::waves_error_string;
using rackboy::sav::WavesError;

namespace {

std::vector<std::uint8_t> make_empty_song()
{
    return std::vector<std::uint8_t>(kDecompressedSongSizeBytes, 0);
}

// Stamp an allocated wave frame with a deterministic per-byte
// pattern. Every byte is non-zero so the parser's
// "any-non-zero -> occupied" heuristic fires.
//
// Pattern design:
//   samples[b] = (uint8_t)(0x10 | ((slot + b) & 0x0F))
//
// - Upper nibble 0x1 guarantees every byte is non-zero so the
//   occupancy heuristic triggers.
// - Lower nibble cycles by (slot + b) so each slot's pattern
//   is distinct AND every position within a slot is distinct,
//   which catches a "writes every sample from samples[0]"
//   bug.
void allocate_wave(std::vector<std::uint8_t>& song,
                   std::size_t                slot)
{
    for (std::size_t b = 0; b < kWaveFrameSlotStride; ++b) {
        song[kWaveFramePoolOffset
             + slot * kWaveFrameSlotStride + b] =
            static_cast<std::uint8_t>(0x10 | ((slot + b) & 0x0F));
    }
}

std::uint8_t expected_byte(std::size_t slot, std::size_t byte_idx)
{
    return static_cast<std::uint8_t>(
        0x10 | ((slot + byte_idx) & 0x0F));
}

void check_wave_matches_expected(const RackboyWaveFrame& w,
                                  std::size_t              slot)
{
    CHECK(w.occupied);
    for (std::size_t b = 0; b < kWaveFrameBytes; ++b) {
        CAPTURE(slot);
        CAPTURE(b);
        CHECK_EQ(w.samples[b], expected_byte(slot, b));
    }
}

// Confirm every sample of a wave frame is zeroed (the
// post-wave_frame_reset state). Used on unallocated slots and
// on allocated slots that a test expects to have been
// cleared.
void check_wave_zeroed(const RackboyWaveFrame& w)
{
    CHECK_FALSE(w.occupied);
    for (std::size_t b = 0; b < kWaveFrameBytes; ++b) {
        CAPTURE(b);
        CHECK_EQ(w.samples[b], 0);
    }
}

}  // namespace

TEST_CASE("parse_waves rejects null out_bank")
{
    auto song = make_empty_song();
    CHECK_EQ(parse_waves(song.data(), song.size(), nullptr),
             WavesError::NullOutBank);
}

TEST_CASE("parse_waves rejects null song")
{
    RackboyBank bank{};
    CHECK_EQ(parse_waves(nullptr, kDecompressedSongSizeBytes, &bank),
             WavesError::NullSong);
}

TEST_CASE("parse_waves NullSong leaves waves[] cleared even when "
          "caller poisoned the bank")
{
    RackboyBank bank{};
    // Poison: pretend a previous run left junk in waves[].
    bank.waves[12].occupied    = true;
    bank.waves[12].samples[0]  = 0xAA;
    bank.waves[12].samples[15] = 0xBB;

    // Poison the instrument/table/loaded side the waves
    // parser must NOT touch so we lock the cross-pipeline
    // invariant on the error path too.
    bank.pulse1_count       = 7;
    bank.tables[3].occupied = true;
    bank.loaded             = true;

    CHECK_EQ(parse_waves(nullptr, kDecompressedSongSizeBytes, &bank),
             WavesError::NullSong);

    // waves[12] is back to its post-wave_frame_reset state.
    CHECK_FALSE(bank.waves[12].occupied);
    CHECK_EQ(bank.waves[12].samples[0],  0);
    CHECK_EQ(bank.waves[12].samples[15], 0);

    // Instrument / table / loaded state untouched — the
    // waves parser owns waves[] only.
    CHECK_EQ(bank.pulse1_count, 7);
    CHECK(bank.tables[3].occupied);
    CHECK(bank.loaded);
}

TEST_CASE("parse_waves rejects a too-short song")
{
    std::vector<std::uint8_t> short_song(kDecompressedSongSizeBytes - 1, 0);
    RackboyBank bank{};
    CHECK_EQ(parse_waves(short_song.data(), short_song.size(), &bank),
             WavesError::SongTooShort);
}

TEST_CASE("parse_waves SongTooShort leaves waves[] cleared even "
          "when caller poisoned the bank")
{
    std::vector<std::uint8_t> short_song(kDecompressedSongSizeBytes - 1, 0);
    RackboyBank bank{};
    bank.waves[100].occupied   = true;
    bank.waves[100].samples[7] = 0x77;
    bank.pulse1_count          = 4;
    bank.loaded                = true;

    CHECK_EQ(parse_waves(short_song.data(), short_song.size(), &bank),
             WavesError::SongTooShort);

    CHECK_FALSE(bank.waves[100].occupied);
    CHECK_EQ(bank.waves[100].samples[7], 0);
    CHECK_EQ(bank.pulse1_count, 4);
    CHECK(bank.loaded);
}

TEST_CASE("parse_waves accepts an oversized song (reads only first 32 KiB)")
{
    std::vector<std::uint8_t> big(kDecompressedSongSizeBytes * 2, 0);
    allocate_wave(big, /*slot=*/11);
    // Stamp bogus bytes past the 32 KiB boundary — must not
    // affect the result.
    for (std::size_t i = kDecompressedSongSizeBytes; i < big.size(); ++i) {
        big[i] = 0xCD;
    }

    RackboyBank bank{};
    CHECK_EQ(parse_waves(big.data(), big.size(), &bank),
             WavesError::Ok);
    check_wave_matches_expected(bank.waves[11], /*slot=*/11);
}

TEST_CASE("parse_waves accepts exactly kDecompressedSongSizeBytes bytes")
{
    // Locks the `<` vs `<=` boundary in the SongTooShort
    // guard. make_empty_song() also returns exactly
    // kDecompressedSongSizeBytes bytes, but an explicit
    // exact-fit case pins the comparator against a future
    // refactor that flips `< kDecompressedSongSizeBytes` to
    // `<= kDecompressedSongSizeBytes`.
    std::vector<std::uint8_t> exact(kDecompressedSongSizeBytes, 0);
    allocate_wave(exact, /*slot=*/3);

    RackboyBank bank{};
    CHECK_EQ(parse_waves(exact.data(), exact.size(), &bank),
             WavesError::Ok);
    check_wave_matches_expected(bank.waves[3], /*slot=*/3);
}

TEST_CASE("parse_waves leaves every wave slot cleared on an empty song")
{
    // All-zero song -> every slot is inferred unused. Every
    // `waves[i]` must be fully zeroed and marked unoccupied.
    auto song = make_empty_song();
    RackboyBank bank{};

    // Poison every wave slot so we can prove wave_frame_reset
    // ran on the happy path too, not just on error returns.
    for (std::size_t k = 0; k < kMaxWaveFrames; ++k) {
        bank.waves[k].occupied   = true;
        bank.waves[k].samples[0] = 0x42;
    }

    CHECK_EQ(parse_waves(song.data(), song.size(), &bank),
             WavesError::Ok);
    for (std::size_t k = 0; k < kMaxWaveFrames; ++k) {
        CAPTURE(k);
        check_wave_zeroed(bank.waves[k]);
    }
}

TEST_CASE("parse_waves round-trips one populated wave byte-for-byte")
{
    auto song = make_empty_song();
    allocate_wave(song, /*slot=*/42);

    RackboyBank bank{};
    CHECK_EQ(parse_waves(song.data(), song.size(), &bank),
             WavesError::Ok);

    check_wave_matches_expected(bank.waves[42], /*slot=*/42);

    // Every other slot remains zeroed / unoccupied.
    for (std::size_t k = 0; k < kMaxWaveFrames; ++k) {
        if (k == 42) continue;
        CAPTURE(k);
        check_wave_zeroed(bank.waves[k]);
    }
}

TEST_CASE("parse_waves handles multiple non-adjacent populated slots")
{
    // Wave pool is indexed — slots 0, 7, 128, and 254 must
    // all land in the matching waves[i] with their respective
    // content intact. Every other slot is left zeroed.
    auto song = make_empty_song();
    allocate_wave(song, 0);
    allocate_wave(song, 7);
    allocate_wave(song, 128);
    allocate_wave(song, 254);

    RackboyBank bank{};
    CHECK_EQ(parse_waves(song.data(), song.size(), &bank),
             WavesError::Ok);

    check_wave_matches_expected(bank.waves[0],   0);
    check_wave_matches_expected(bank.waves[7],   7);
    check_wave_matches_expected(bank.waves[128], 128);
    check_wave_matches_expected(bank.waves[254], 254);

    for (std::size_t k = 0; k < kMaxWaveFrames; ++k) {
        if (k == 0 || k == 7 || k == 128 || k == 254) continue;
        CAPTURE(k);
        check_wave_zeroed(bank.waves[k]);
    }
}

TEST_CASE("parse_waves fills all 255 wave slots when every slot is "
          "populated")
{
    auto song = make_empty_song();
    for (std::size_t k = 0; k < kTotalWaveFrameSlots; ++k) {
        allocate_wave(song, k);
    }

    RackboyBank bank{};
    CHECK_EQ(parse_waves(song.data(), song.size(), &bank),
             WavesError::Ok);

    for (std::size_t k = 0; k < kMaxWaveFrames; ++k) {
        CAPTURE(k);
        check_wave_matches_expected(bank.waves[k], k);
    }
}

TEST_CASE("parse_waves marks any-non-zero frame occupied and all-zero "
          "frame unoccupied even on one-byte differences")
{
    // Slot 5 has one non-zero byte at index 3. That should be
    // enough to flip occupied=true. Slot 6 has 16 zero bytes
    // and should stay unoccupied.
    auto song = make_empty_song();
    song[kWaveFramePoolOffset + 5 * kWaveFrameSlotStride + 3] = 0x01;
    // Slot 6 is left all-zero by make_empty_song.

    RackboyBank bank{};
    CHECK_EQ(parse_waves(song.data(), song.size(), &bank),
             WavesError::Ok);

    CHECK(bank.waves[5].occupied);
    for (std::size_t b = 0; b < kWaveFrameBytes; ++b) {
        CAPTURE(b);
        CHECK_EQ(bank.waves[5].samples[b],
                 b == 3 ? std::uint8_t{0x01} : std::uint8_t{0x00});
    }

    check_wave_zeroed(bank.waves[6]);
}

TEST_CASE("parse_waves slot addressing pins the (slot, byte) "
          "order, not (byte, slot)")
{
    // Catches a bug where the inner loop accidentally swaps
    // slot and byte indices at the pool read site. Stamp slot
    // 1 with byte-position-dependent content that is
    // asymmetric under swap; a swap would scatter the bytes
    // across multiple slots instead of into slot 1's samples
    // array.
    auto song = make_empty_song();
    for (std::size_t b = 0; b < kWaveFrameSlotStride; ++b) {
        song[kWaveFramePoolOffset + 1 * kWaveFrameSlotStride + b] =
            static_cast<std::uint8_t>((b << 4) | 0xA);
    }

    RackboyBank bank{};
    CHECK_EQ(parse_waves(song.data(), song.size(), &bank),
             WavesError::Ok);

    CHECK(bank.waves[1].occupied);
    for (std::size_t b = 0; b < kWaveFrameBytes; ++b) {
        CAPTURE(b);
        CHECK_EQ(bank.waves[1].samples[b],
                 static_cast<std::uint8_t>((b << 4) | 0xA));
    }

    // Slots 0 and 2 stay zeroed — proves the parser did not
    // smear slot 1's content into adjacent slots.
    check_wave_zeroed(bank.waves[0]);
    check_wave_zeroed(bank.waves[2]);
}

TEST_CASE("parse_waves does not touch instrument pools, tables, "
          "or loaded flag")
{
    // The parser is expected to run in a pipeline AFTER
    // parse_instruments / parse_tables. It must not clobber
    // the per-channel pools, the table sub-array, or the
    // `loaded` flag that earlier parser passes set.
    auto song = make_empty_song();
    allocate_wave(song, /*slot=*/0);

    RackboyBank bank{};
    // Populate the bank as if parse_instruments and
    // parse_tables had already run.
    bank.pulse1_count           = 3;
    bank.wave_count             = 1;
    bank.loaded                 = true;
    bank.tables[9].occupied     = true;
    bank.tables[9].rows[2].cmd1 = 0x55;
    for (std::size_t k = 0; k < 16; ++k) {
        bank.pulse1_pool[0].raw[k] = 0x55;
    }

    CHECK_EQ(parse_waves(song.data(), song.size(), &bank),
             WavesError::Ok);

    // Wave pass did its job.
    check_wave_matches_expected(bank.waves[0], /*slot=*/0);

    // Instrument pools, tables[], and loaded untouched.
    CHECK_EQ(bank.pulse1_count, 3);
    CHECK_EQ(bank.wave_count,   1);
    CHECK(bank.loaded);
    CHECK(bank.tables[9].occupied);
    CHECK_EQ(bank.tables[9].rows[2].cmd1, 0x55);
    for (std::size_t k = 0; k < 16; ++k) {
        CAPTURE(k);
        CHECK_EQ(bank.pulse1_pool[0].raw[k], 0x55);
    }
}

TEST_CASE("parse_waves round-trips 0x00 and 0xFF boundary sample bytes")
{
    // The existing allocate_wave helper maxes at 0x1F and the
    // (slot, byte) ordering case tops out at 0xFA, so a pure
    // 0xFF wave is otherwise untested. Locks "no signed /
    // masked compare regression" on the sample copy path, and
    // doubles as an end-to-end occupancy sanity check for an
    // all-0xFF slot sitting next to an all-zero slot.
    auto song = make_empty_song();
    for (std::size_t b = 0; b < kWaveFrameSlotStride; ++b) {
        song[kWaveFramePoolOffset
             + 17 * kWaveFrameSlotStride + b] = 0xFF;
    }

    RackboyBank bank{};
    CHECK_EQ(parse_waves(song.data(), song.size(), &bank),
             WavesError::Ok);

    CHECK(bank.waves[17].occupied);
    for (std::size_t b = 0; b < kWaveFrameBytes; ++b) {
        CAPTURE(b);
        CHECK_EQ(bank.waves[17].samples[b], 0xFF);
    }

    // Slot 16 stays zeroed — proves the 0xFF pattern did not
    // bleed upstream via a signed off-by-one. Slot 18 same.
    check_wave_zeroed(bank.waves[16]);
    check_wave_zeroed(bank.waves[18]);
}

TEST_CASE("waves_error_string returns a non-null, distinct, "
          "non-empty string for every error")
{
    const WavesError all[] = {
        WavesError::Ok,
        WavesError::NullSong,
        WavesError::NullOutBank,
        WavesError::SongTooShort,
    };
    constexpr std::size_t kAllCount = sizeof(all) / sizeof(all[0]);

    const char* seen[kAllCount] = {};
    std::size_t seen_count = 0;
    for (WavesError e : all) {
        const char* s = waves_error_string(e);
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

TEST_CASE("waves_error_string returns a non-null string for an "
          "out-of-range enum value")
{
    // Log-replay tooling may deserialize a corrupt int. The
    // fallback in waves_error_string must still return a
    // printable string rather than nullptr or crash so the
    // logger can emit "unknown" instead of faulting. Locks the
    // contract documented in rackboy_sav_waves.cpp's fallback
    // comment.
    const auto corrupt = static_cast<WavesError>(9999);
    const char* s = waves_error_string(corrupt);
    REQUIRE(s != nullptr);
    CHECK(s[0] != '\0');
}
