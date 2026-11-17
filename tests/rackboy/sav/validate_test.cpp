// tests/sav/validate_test.cpp
//
// Unit tests for rackboy_sav/rackboy_sav_validate.{h,cpp}
// — the top-level load façade for bead rackboy-3c5.
//
// The façade is pure composition of the five lower-level
// rackboy_sav parsers; these tests lock:
//
//   1. The phase-tagged `LoadResult` wiring for every
//      sub-parser error that is actually reachable
//      through the façade with a (raw_sav,
//      decompressed_song) input pair. NOTE: the
//      TableBlockParse / WaveFramePoolParse phases are
//      NOT exercised end-to-end — their only sub-error
//      codes (NullSong / NullOutBank / SongTooShort) are
//      all subsumed by parse_instruments running first
//      in the pipeline, so there is no realistic input
//      pair that can fire them past the instrument
//      phase. The phase + sub_error_code wiring for
//      those two branches is locked only indirectly via
//      the `load_error_string` round-trip test near the
//      bottom of this file. See the matching NOTE
//      comments in `rackboy_sav_validate.cpp` at the
//      `parse_tables` / `parse_waves` error handlers.
//   2. The façade's own argument contract (null guards,
//      sidecar pointer/length coupling).
//   3. The happy path on a fully synthetic raw sav +
//      decompressed song + sidecar, end to end.
//   4. The soft-fail state invariant: on every non-Ok
//      return other than the two output-pointer null
//      guards, both *out_bank and *out_names are left in
//      a freshly-reset state.
//   5. The `load_error_string` contract: non-null,
//      non-empty, pairwise-distinct strings across
//      representative `(phase, sub_error_code)` pairs,
//      plus a non-null fallback for an out-of-range
//      LoadPhase.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "doctest/doctest.h"

#include "rackboy_sav_header.h"
#include "rackboy_sav_instruments.h"
#include "rackboy_sav_sidecar.h"
#include "rackboy_sav_tables.h"
#include "rackboy_sav_types.h"
#include "rackboy_sav_validate.h"
#include "rackboy_sav_waves.h"

using rackboy::sav::ArgumentError;
using rackboy::sav::HeaderError;
using rackboy::sav::InstrumentsError;
using rackboy::sav::kActiveSlotOffset;
using rackboy::sav::kDecompressedSongSizeBytes;
using rackboy::sav::kHeaderVersionLsdj940;
using rackboy::sav::kHeaderVersionOffset;
using rackboy::sav::kInstrumentAllocMapOffset;
using rackboy::sav::kInstrumentParamsOffset;
using rackboy::sav::kInstrumentTypePulse;
using rackboy::sav::kMagicByte0;
using rackboy::sav::kMagicByte1;
using rackboy::sav::kMagicOffset;
using rackboy::sav::kMaxInstrumentsPerChannel;
using rackboy::sav::kSavSizeBytes;
using rackboy::sav::load_error_string;
using rackboy::sav::load_rackboy_sav;
using rackboy::sav::LoadInputs;
using rackboy::sav::LoadPhase;
using rackboy::sav::LoadResult;
using rackboy::sav::RackboyBank;
using rackboy::sav::RackboyBankNames;
using rackboy::sav::SidecarError;
using rackboy::sav::TablesError;
using rackboy::sav::WavesError;

namespace {

// Build a 131072-byte raw sav that passes every
// header-layer check:
//   - exact size
//   - 'jk' magic at 0x813E
//   - active slot byte == 0
//   - LSDJ 9.4.0 version byte (0x1B) at 0x8100
std::vector<std::uint8_t> make_valid_raw_sav()
{
    std::vector<std::uint8_t> sav(kSavSizeBytes, 0);
    sav[kMagicOffset + 0]      = kMagicByte0;
    sav[kMagicOffset + 1]      = kMagicByte1;
    sav[kActiveSlotOffset]     = 0;
    sav[kHeaderVersionOffset]  = kHeaderVersionLsdj940;
    return sav;
}

// Build a 32 KiB decompressed song blob with zero
// instruments / tables / waves. This is a LEGITIMATE
// empty-song state — parse_instruments accepts an empty
// alloc map, parse_tables and parse_waves ditto. Used as
// the baseline for tests that only want to exercise the
// façade's pipeline ordering, not the content of the
// bank.
std::vector<std::uint8_t> make_empty_decompressed_song()
{
    return std::vector<std::uint8_t>(kDecompressedSongSizeBytes, 0);
}

// Stamp a single Pulse1 instrument at alloc map slot 0
// so parse_instruments produces a non-empty pool and the
// test can verify the façade actually ran the sub-parsers.
void stamp_one_pulse1_instrument(
    std::vector<std::uint8_t>& song)
{
    song[kInstrumentAllocMapOffset + 0] = 0x01;
    // Byte 0 of the 16-byte record is the type byte.
    song[kInstrumentParamsOffset + 0]   = kInstrumentTypePulse;
    // Fill the rest of the record with a distinctive
    // pattern so the test can verify the raw bytes came
    // through verbatim.
    for (std::size_t i = 1; i < 16; ++i) {
        song[kInstrumentParamsOffset + i] =
            static_cast<std::uint8_t>(0xA0 + i);
    }
}

// Convenience: call load_rackboy_sav with a raw sav +
// decompressed song + no sidecar. Returns the LoadResult
// and writes into the caller's bank / names.
LoadResult load_no_sidecar(
    const std::vector<std::uint8_t>& raw_sav,
    const std::vector<std::uint8_t>& song,
    RackboyBank&                      bank,
    RackboyBankNames&                 names)
{
    LoadInputs in{};
    in.raw_sav               = raw_sav.data();
    in.raw_sav_len           = raw_sav.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();
    in.sidecar_json          = nullptr;
    in.sidecar_json_len      = 0;
    return load_rackboy_sav(in, &bank, &names);
}

// Same, with a sidecar buffer.
LoadResult load_with_sidecar(
    const std::vector<std::uint8_t>& raw_sav,
    const std::vector<std::uint8_t>& song,
    const std::string&               sidecar,
    RackboyBank&                      bank,
    RackboyBankNames&                 names)
{
    LoadInputs in{};
    in.raw_sav               = raw_sav.data();
    in.raw_sav_len           = raw_sav.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();
    in.sidecar_json          = sidecar.data();
    in.sidecar_json_len      = sidecar.size();
    return load_rackboy_sav(in, &bank, &names);
}

// Poison the bank so a subsequent test can prove
// bank_reset fired on the error path.
void poison_bank(RackboyBank& b)
{
    b.pulse1_count = 7;
    b.wave_count   = 3;
    b.loaded       = true;
    b.tables[0].occupied = true;
    for (std::size_t k = 0; k < 16; ++k) {
        b.pulse1_pool[0].raw[k] = 0x5A;
    }
}

// Poison the names struct so a subsequent test can prove
// names_reset fired on the error path.
void poison_names(RackboyBankNames& n)
{
    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        for (std::size_t b = 0; b < 16; ++b) {
            n.pulse1[i].text[b] = 'A';
            n.pulse2[i].text[b] = 'B';
            n.wave  [i].text[b] = 'C';
            n.noise [i].text[b] = 'D';
        }
    }
}

// Assert the bank is in a fully-reset state.
void check_bank_reset(const RackboyBank& b)
{
    CHECK_EQ(b.pulse1_count, 0);
    CHECK_EQ(b.pulse2_count, 0);
    CHECK_EQ(b.wave_count,   0);
    CHECK_EQ(b.noise_count,  0);
    CHECK_FALSE(b.loaded);
    // Spot-check a few pool / table / wave slots for zero
    // bytes rather than the full O(n) walk — bank_reset's
    // own tests already cover the exhaustive case.
    CHECK_EQ(b.pulse1_pool[0].raw[0], 0);
    CHECK_EQ(b.tables[0].occupied, false);
    CHECK_EQ(b.waves[0].occupied,  false);
}

void check_names_reset(const RackboyBankNames& n)
{
    CHECK_EQ(n.pulse1[0].text[0], '\0');
    CHECK_EQ(n.pulse2[0].text[0], '\0');
    CHECK_EQ(n.wave  [0].text[0], '\0');
    CHECK_EQ(n.noise [0].text[0], '\0');
}

}  // namespace

// ---------------------------------------------------------------------------
// Output-pointer null guards (ArgumentValidation phase,
// NullOutBank / NullOutNames sub-errors)
// ---------------------------------------------------------------------------

TEST_CASE("load_rackboy_sav rejects null out_bank and does "
          "not touch a surviving poisoned out_names")
{
    // Poison-and-check on the short-circuit path: the
    // header docstring promises that NullOutBank /
    // NullOutNames short-circuit BEFORE any reset runs.
    // A future refactor that wired reset_outputs in
    // ahead of the null guards would silently regress
    // the "do not touch memory" promise. Locking it with
    // a poisoned survivor is cheap and matches the
    // poison-and-check pattern already used on the
    // non-short-circuit error paths.
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    LoadInputs in{};
    in.raw_sav               = raw.data();
    in.raw_sav_len           = raw.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();

    RackboyBankNames names;
    poison_names(names);

    const LoadResult r =
        load_rackboy_sav(in, nullptr, &names);
    CAPTURE(static_cast<int>(r.phase));
    CAPTURE(r.sub_error_code);
    CHECK_EQ(r.phase, LoadPhase::ArgumentValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(ArgumentError::NullOutBank));

    // The poisoned names struct must survive unchanged —
    // the null out_bank guard has to short-circuit
    // before any reset_outputs call could run.
    CHECK_EQ(names.pulse1[0].text[0], 'A');
    CHECK_EQ(names.pulse2[0].text[0], 'B');
    CHECK_EQ(names.wave  [0].text[0], 'C');
    CHECK_EQ(names.noise [0].text[0], 'D');
}

TEST_CASE("load_rackboy_sav rejects null out_names and does "
          "not touch a surviving poisoned out_bank")
{
    // Mirror of the previous test on the second short-
    // circuit path: out_names null with a poisoned
    // out_bank. Locks the "out_names null short-circuits
    // before touching out_bank" half of the contract.
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    LoadInputs in{};
    in.raw_sav               = raw.data();
    in.raw_sav_len           = raw.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();

    RackboyBank bank{};
    poison_bank(bank);

    const LoadResult r =
        load_rackboy_sav(in, &bank, nullptr);
    CAPTURE(static_cast<int>(r.phase));
    CAPTURE(r.sub_error_code);
    CHECK_EQ(r.phase, LoadPhase::ArgumentValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(ArgumentError::NullOutNames));

    // The poisoned bank must survive unchanged.
    CHECK_EQ(bank.pulse1_count, 7);
    CHECK_EQ(bank.wave_count,   3);
    CHECK(bank.loaded);
    CHECK(bank.tables[0].occupied);
    CHECK_EQ(bank.pulse1_pool[0].raw[0], 0x5A);
}

TEST_CASE("load_rackboy_sav out_bank null guard fires before out_names")
{
    // Both pointers null — we expect NullOutBank because
    // the header documents out_bank as the first guard.
    // Locks the contract so a future edit can't
    // accidentally reorder. (No memory to poison here
    // because both destinations are null; the
    // preceding two tests cover the one-null-one-
    // poisoned combinations.)
    LoadInputs in{};
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    in.raw_sav               = raw.data();
    in.raw_sav_len           = raw.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();

    const LoadResult r =
        load_rackboy_sav(in, nullptr, nullptr);
    CAPTURE(static_cast<int>(r.phase));
    CAPTURE(r.sub_error_code);
    CHECK_EQ(r.phase, LoadPhase::ArgumentValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(ArgumentError::NullOutBank));
}

// ---------------------------------------------------------------------------
// Input buffer null guards
// ---------------------------------------------------------------------------

TEST_CASE("load_rackboy_sav rejects null raw_sav")
{
    auto song = make_empty_decompressed_song();
    LoadInputs in{};
    in.raw_sav               = nullptr;
    in.raw_sav_len           = 0;
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();

    RackboyBank bank{};
    RackboyBankNames names{};
    poison_bank(bank);
    poison_names(names);

    const LoadResult r =
        load_rackboy_sav(in, &bank, &names);
    CHECK_EQ(r.phase, LoadPhase::ArgumentValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(ArgumentError::NullRawSav));
    check_bank_reset(bank);
    check_names_reset(names);
}

TEST_CASE("load_rackboy_sav rejects null decompressed_song")
{
    auto raw = make_valid_raw_sav();
    LoadInputs in{};
    in.raw_sav               = raw.data();
    in.raw_sav_len           = raw.size();
    in.decompressed_song     = nullptr;
    in.decompressed_song_len = 0;

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_rackboy_sav(in, &bank, &names);
    CHECK_EQ(r.phase, LoadPhase::ArgumentValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(
                 ArgumentError::NullDecompressedSong));
}

TEST_CASE("load_rackboy_sav rejects inconsistent sidecar "
          "(null pointer, non-zero length)")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    LoadInputs in{};
    in.raw_sav               = raw.data();
    in.raw_sav_len           = raw.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();
    in.sidecar_json          = nullptr;
    in.sidecar_json_len      = 5;

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_rackboy_sav(in, &bank, &names);
    CHECK_EQ(r.phase, LoadPhase::ArgumentValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(
                 ArgumentError::InconsistentSidecarArgs));
}

TEST_CASE("load_rackboy_sav rejects inconsistent sidecar "
          "(non-null pointer, zero length)")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    const std::string fake_sidecar = "{}";
    LoadInputs in{};
    in.raw_sav               = raw.data();
    in.raw_sav_len           = raw.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();
    in.sidecar_json          = fake_sidecar.data();
    in.sidecar_json_len      = 0;

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_rackboy_sav(in, &bank, &names);
    CHECK_EQ(r.phase, LoadPhase::ArgumentValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(
                 ArgumentError::InconsistentSidecarArgs));
}

// ---------------------------------------------------------------------------
// HeaderValidation phase — propagates HeaderError
// ---------------------------------------------------------------------------

TEST_CASE("load_rackboy_sav surfaces HeaderError::SizeMismatch")
{
    std::vector<std::uint8_t> short_raw(kSavSizeBytes - 1, 0);
    auto song = make_empty_decompressed_song();

    RackboyBank bank{};
    RackboyBankNames names{};
    poison_bank(bank);
    poison_names(names);

    const LoadResult r =
        load_no_sidecar(short_raw, song, bank, names);
    CHECK_EQ(r.phase, LoadPhase::HeaderValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(HeaderError::SizeMismatch));
    check_bank_reset(bank);
    check_names_reset(names);
}

TEST_CASE("load_rackboy_sav surfaces HeaderError::MagicMismatch")
{
    auto raw  = make_valid_raw_sav();
    raw[kMagicOffset + 0] = 0xFF;  // corrupt the 'j'
    auto song = make_empty_decompressed_song();

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_no_sidecar(raw, song, bank, names);
    CHECK_EQ(r.phase, LoadPhase::HeaderValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(HeaderError::MagicMismatch));
}

TEST_CASE("load_rackboy_sav surfaces HeaderError::VersionMismatch")
{
    auto raw  = make_valid_raw_sav();
    raw[kHeaderVersionOffset] = 0x1A;
    auto song = make_empty_decompressed_song();

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_no_sidecar(raw, song, bank, names);
    CHECK_EQ(r.phase, LoadPhase::HeaderValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(HeaderError::VersionMismatch));
}

TEST_CASE("load_rackboy_sav surfaces HeaderError::ActiveSlotNotZero")
{
    auto raw  = make_valid_raw_sav();
    raw[kActiveSlotOffset] = 1;  // valid slot but not zero
    auto song = make_empty_decompressed_song();

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_no_sidecar(raw, song, bank, names);
    CHECK_EQ(r.phase, LoadPhase::HeaderValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(HeaderError::ActiveSlotNotZero));
}

// ---------------------------------------------------------------------------
// InstrumentBlockParse phase
// ---------------------------------------------------------------------------

TEST_CASE("load_rackboy_sav surfaces InstrumentsError::SongTooShort")
{
    auto raw = make_valid_raw_sav();
    std::vector<std::uint8_t> short_song(
        kDecompressedSongSizeBytes - 1, 0);

    RackboyBank bank{};
    RackboyBankNames names{};
    poison_bank(bank);

    const LoadResult r =
        load_no_sidecar(raw, short_song, bank, names);
    CHECK_EQ(r.phase, LoadPhase::InstrumentBlockParse);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(InstrumentsError::SongTooShort));
    check_bank_reset(bank);
}

TEST_CASE("load_rackboy_sav surfaces InstrumentsError::AllocMapInvalid")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    // Alloc map accepts only 0x00 / 0x01; 0x02 fires
    // AllocMapInvalid.
    song[kInstrumentAllocMapOffset + 3] = 0x02;

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_no_sidecar(raw, song, bank, names);
    CHECK_EQ(r.phase, LoadPhase::InstrumentBlockParse);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(
                 InstrumentsError::AllocMapInvalid));
}

// ---------------------------------------------------------------------------
// SidecarParse phase
// ---------------------------------------------------------------------------

TEST_CASE("load_rackboy_sav surfaces SidecarError::InvalidJson")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    const std::string garbage = "not-json";

    RackboyBank bank{};
    RackboyBankNames names{};
    poison_bank(bank);
    poison_names(names);

    const LoadResult r =
        load_with_sidecar(raw, song, garbage, bank, names);
    CHECK_EQ(r.phase, LoadPhase::SidecarParse);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(SidecarError::InvalidJson));
    check_bank_reset(bank);
    check_names_reset(names);
}

TEST_CASE("load_rackboy_sav surfaces SidecarError::UnknownChannel")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    const std::string bad_channel = "{\"XX-00\": \"oops\"}";

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_with_sidecar(raw, song, bad_channel, bank, names);
    CHECK_EQ(r.phase, LoadPhase::SidecarParse);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(SidecarError::UnknownChannel));
}

TEST_CASE("load_rackboy_sav surfaces SidecarError::SlotOutOfRange")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    // Slot 99 > kMaxInstrumentsPerChannel - 1 (63).
    const std::string bad_slot = "{\"PU1-99\": \"x\"}";

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_with_sidecar(raw, song, bad_slot, bank, names);
    CHECK_EQ(r.phase, LoadPhase::SidecarParse);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(SidecarError::SlotOutOfRange));
}

TEST_CASE("load_rackboy_sav surfaces SidecarError::NameTooLong")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    // 16 ASCII bytes + null terminator = 17; exceeds
    // kMaxInstrumentNameBytes (16) by one. Picks the
    // first integer bound past the limit rather than
    // something wildly oversized, so a future tightening
    // of the limit still trips this test for the same
    // reason.
    const std::string too_long =
        "{\"PU1-00\": \"0123456789ABCDEF\"}";

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_with_sidecar(raw, song, too_long, bank, names);
    CHECK_EQ(r.phase, LoadPhase::SidecarParse);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(SidecarError::NameTooLong));
}

// ---------------------------------------------------------------------------
// Happy paths
// ---------------------------------------------------------------------------
//
// NOTE: no TableBlockParse / WaveFramePoolParse phase
// tests here — those sub-parsers currently share a
// failure-mode space (NullSong / NullOutBank /
// SongTooShort) entirely subsumed by parse_instruments
// running first in the pipeline, so there is no (valid
// instrument block, invalid table block) input pair the
// façade could be fed. The phase + sub_error_code wiring
// for those two branches is locked only indirectly by
// the load_error_string phase-round-trip test at the
// bottom of this file. If TablesError / WavesError gains
// an enumerator that can fire on a song with a valid
// instrument block, add direct end-to-end tests for
// those branches.

TEST_CASE("load_rackboy_sav accepts a fully empty valid sav "
          "(no sidecar)")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();

    RackboyBank bank{};
    RackboyBankNames names{};
    poison_bank(bank);
    poison_names(names);

    const LoadResult r =
        load_no_sidecar(raw, song, bank, names);
    CHECK_EQ(r.phase, LoadPhase::Ok);
    CHECK_EQ(r.sub_error_code, 0);

    // Empty instrument pools are a legal state.
    CHECK_EQ(bank.pulse1_count, 0);
    CHECK_EQ(bank.pulse2_count, 0);
    CHECK_EQ(bank.wave_count,   0);
    CHECK_EQ(bank.noise_count,  0);
    // But the bank is marked loaded, which is the signal
    // the audio thread uses to distinguish "fresh from
    // power-on" from "just loaded an empty song".
    CHECK(bank.loaded);
    // No sidecar -> every name slot empty.
    check_names_reset(names);
}

TEST_CASE("load_rackboy_sav populates the bank when the "
          "decompressed song has one Pulse1 instrument")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    stamp_one_pulse1_instrument(song);

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_no_sidecar(raw, song, bank, names);
    CHECK_EQ(r.phase, LoadPhase::Ok);
    CHECK_EQ(bank.pulse1_count, 1);
    CHECK_EQ(bank.pulse2_count, 0);
    CHECK(bank.loaded);

    // Byte 0 of the record is the type byte (pulse = 0x00).
    CHECK_EQ(bank.pulse1_pool[0].raw[0],
             kInstrumentTypePulse);
    // Byte 1..15 should match the pattern
    // stamp_one_pulse1_instrument wrote.
    for (std::size_t i = 1; i < 16; ++i) {
        CAPTURE(i);
        CHECK_EQ(bank.pulse1_pool[0].raw[i],
                 static_cast<std::uint8_t>(0xA0 + i));
    }
}

TEST_CASE("load_rackboy_sav applies a sidecar on the "
          "happy path")
{
    auto raw  = make_valid_raw_sav();
    auto song = make_empty_decompressed_song();
    stamp_one_pulse1_instrument(song);
    const std::string sidecar = "{\"PU1-00\": \"LEAD\"}";

    RackboyBank bank{};
    RackboyBankNames names{};
    const LoadResult r =
        load_with_sidecar(raw, song, sidecar, bank, names);
    CHECK_EQ(r.phase, LoadPhase::Ok);
    CHECK_EQ(bank.pulse1_count, 1);
    CHECK_EQ(std::strcmp(names.pulse1[0].text, "LEAD"), 0);
    // Other slots stay empty.
    CHECK_EQ(names.pulse1[1].text[0], '\0');
    CHECK_EQ(names.wave  [0].text[0], '\0');
}

// ---------------------------------------------------------------------------
// load_error_string
// ---------------------------------------------------------------------------

TEST_CASE("load_error_string returns non-null, non-empty, "
          "pairwise-distinct strings across phases")
{
    // One representative sub-error per phase, picked so
    // the (phase, sub_error_code) pairs are distinct but
    // several of the sub-errors share the same underlying
    // sub-parser meaning. The same-name sub-errors are
    // the whole reason we need phase context in the
    // returned string: a caller logging
    // `(TableBlockParse, SongTooShort)` vs
    // `(WaveFramePoolParse, SongTooShort)` must see two
    // different human-readable messages, not the same
    // "song buffer shorter than 32 KiB" string for both.
    const LoadResult all[] = {
        {LoadPhase::Ok,                   0},
        {LoadPhase::ArgumentValidation,
         static_cast<int>(ArgumentError::NullOutBank)},
        {LoadPhase::HeaderValidation,
         static_cast<int>(HeaderError::SizeMismatch)},
        {LoadPhase::InstrumentBlockParse,
         static_cast<int>(InstrumentsError::SongTooShort)},
        {LoadPhase::TableBlockParse,
         static_cast<int>(TablesError::SongTooShort)},
        {LoadPhase::WaveFramePoolParse,
         static_cast<int>(WavesError::SongTooShort)},
        {LoadPhase::SidecarParse,
         static_cast<int>(SidecarError::InvalidJson)},
    };
    constexpr std::size_t kAllCount =
        sizeof(all) / sizeof(all[0]);

    const char* seen[kAllCount] = {};
    std::size_t seen_count = 0;
    for (const LoadResult r : all) {
        CAPTURE(static_cast<int>(r.phase));
        CAPTURE(r.sub_error_code);
        const char* s = load_error_string(r);
        REQUIRE(s != nullptr);
        CHECK(s[0] != '\0');
        for (std::size_t i = 0; i < seen_count; ++i) {
            CAPTURE(i);
            CAPTURE(seen[i]);
            CAPTURE(s);
            CHECK(std::strcmp(seen[i], s) != 0);
        }
        seen[seen_count++] = s;
    }
    CHECK_EQ(seen_count, kAllCount);
}

TEST_CASE("load_error_string returns a non-null fallback "
          "for an out-of-range LoadPhase")
{
    // Log-replay tooling may deserialize a corrupt int
    // into the phase field. The fallback must return a
    // printable string rather than nullptr or crash.
    LoadResult corrupt{};
    corrupt.phase          = static_cast<LoadPhase>(9999);
    corrupt.sub_error_code = 0;
    const char* s = load_error_string(corrupt);
    REQUIRE(s != nullptr);
    CHECK(s[0] != '\0');
}

TEST_CASE("load_error_string returns a non-null fallback "
          "for an out-of-range ArgumentError sub-code")
{
    // The ArgumentValidation phase dispatches through its
    // own switch; verify its fallback branch is also
    // non-crashing.
    LoadResult corrupt{};
    corrupt.phase          = LoadPhase::ArgumentValidation;
    corrupt.sub_error_code = 9999;
    const char* s = load_error_string(corrupt);
    REQUIRE(s != nullptr);
    CHECK(s[0] != '\0');
}
