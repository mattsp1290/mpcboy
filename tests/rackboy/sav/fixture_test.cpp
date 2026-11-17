// tests/sav/fixture_test.cpp
//
// End-to-end doctest suite that exercises the full
// rackboy_sav pipeline (header -> decompress -> instruments
// -> tables -> waves -> facade) against the populated
// `tests/fixtures/rackboy_v1.sav` fixture and the
// expected-values table in `tests/fixtures/expected.json`.
// Bead rackboy-byp.
//
// Scope
// -----
// Every other tests/sav/*.cpp file in this directory stamps
// a synthetic in-memory buffer with exactly the bytes the
// parser under test reads. That keeps the individual
// sub-parser tests fast, deterministic, and hermetic —
// each one locks a specific (offset, byte) invariant.
// These fixture-backed tests are the opposite extreme:
// they load a real 128 KiB LSDJ `.sav` produced by the
// pinned `rackboy-fixture-gen` binary against the pinned
// D-002 extractor commit, pipe it through every parser in
// the same order the production load path will, and
// cross-check the bank + names output against the
// `expected.json` values committed alongside the fixture.
//
// If a future refactor breaks any parser's integration with
// the others — or if the fixture is regenerated against a
// different extractor commit without updating the
// `expected.json` values — these tests are what catches it.
//
// Why the block-chain decompressor
// --------------------------------
// `parse_instruments`, `parse_tables`, and `parse_waves`
// all consume a *decompressed* 32 KiB song blob. That
// decompressor lives in `rackboy_sav_compression.{h,cpp}`
// and is promoted to a first-class rackboy_sav module
// under bead rackboy-cbo, so it is picked up by the sav
// test module's `$(wildcard rackboy_sav/*.cpp)` scan with
// no special entry in `tests/Makefile`'s
// `sav_extra_impl_sources`. It is also linked into the
// VCV plugin directly by `rackboy-klf`.
//
// Fixture acceptance checks
// -------------------------
// Each invariant in
// `tests/fixtures/expected.json::parser_acceptance_checks`
// has a corresponding `TEST_CASE` in this file (the
// current suite has 18 cases — see individual sections
// below for the groupings; a few acceptance-check rows
// collapse into one TEST_CASE where a single assertion
// locks multiple rows). A failure reports which
// invariant broke via the CAPTURE stream attached to the
// per-case assertions.
//
// Fixture integrity
// -----------------
// `tests/fixtures/expected.json` pins `fixture_sha256`
// as a binding fact, and `tests/fixtures/README.md`
// warns that drift in the fixture must be investigated
// before committing. A cryptographic SHA-256 would be
// the most faithful integrity check, but rackboy
// deliberately does not link any crypto library (neither
// production nor tests), so the fixture-integrity test
// below computes a 64-bit FNV-1a hash instead. FNV-1a
// is not cryptographically secure but IS a
// deterministic, every-byte sensitive fingerprint: any
// change to any byte of the fixture flips the hash, and
// it takes ~15 lines of code with no dependency. When
// the fixture is legitimately regenerated, update both
// the `fixture_sha256` field in `expected.json` AND the
// `kExpectedFixtureFnv1a64` constant below in the same
// commit.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "doctest/doctest.h"

#include "common/fixture_loader.h"
#include "rackboy_sav_header.h"
#include "rackboy_sav_instruments.h"
#include "rackboy_sav_tables.h"
#include "rackboy_sav_types.h"
#include "rackboy_sav_validate.h"
#include "rackboy_sav_waves.h"
#include "rackboy_sav_compression.h"

using rackboy::sav::HeaderError;
using rackboy::sav::HeaderInfo;
using rackboy::sav::InstrumentsError;
using rackboy::sav::kActiveSlotOffset;
using rackboy::sav::kDecompressedSongSizeBytes;
using rackboy::sav::kHeaderVersionLsdj940;
using rackboy::sav::kHeaderVersionOffset;
using rackboy::sav::kInstrumentAllocMapLen;
using rackboy::sav::kInstrumentAllocMapOffset;
using rackboy::sav::kInstrumentAllocated;
using rackboy::sav::kInstrumentParamsOffset;
using rackboy::sav::kInstrumentRecordBytes;
using rackboy::sav::kInstrumentTypeNoise;
using rackboy::sav::kInstrumentTypePulse;
using rackboy::sav::kInstrumentTypeWave;
using rackboy::sav::kInstrumentUnallocated;
using rackboy::sav::kMagicByte0;
using rackboy::sav::kMagicByte1;
using rackboy::sav::kMagicOffset;
using rackboy::sav::kSavSizeBytes;
using rackboy::sav::kSlot0NameOffset;
using rackboy::sav::kSlot0NameLen;
using rackboy::sav::kTableAllocMapLen;
using rackboy::sav::kTableAllocMapOffset;
using rackboy::sav::kWaveFrameBytes;
using rackboy::sav::load_rackboy_sav;
using rackboy::sav::LoadInputs;
using rackboy::sav::LoadPhase;
using rackboy::sav::LoadResult;
using rackboy::sav::parse_instruments;
using rackboy::sav::parse_tables;
using rackboy::sav::parse_waves;
using rackboy::sav::RackboyBank;
using rackboy::sav::RackboyBankNames;
using rackboy::sav::read_header;
using rackboy::sav::TablesError;
using rackboy::sav::validate_header;
using rackboy::sav::WavesError;

namespace rb_compression = rackboy::sav::compression;

namespace {

// ---------------------------------------------------------------------------
// Expected values (mirrored from tests/fixtures/expected.json)
// ---------------------------------------------------------------------------
//
// Declared first so the helpers below (fixture loader,
// block-allocation walker) can share the block-alloc
// constants without a duplicate in-function copy.

constexpr std::uint8_t  kExpectedActiveSlot  = 0;
constexpr std::uint8_t  kExpectedVersionByte = 0x1B;
constexpr std::size_t   kExpectedOwnedBlocks = 11;
constexpr std::size_t   kBlockAllocOffset    = 0x8141;
constexpr std::size_t   kBlockAllocLen       = 0xBF;  // 191

// kFixtureSize and kExpectedFixtureFnv1a64 now live in
// tests/common/fixture_loader.h; the constants were hoisted in
// iteration 63 so both this suite and
// tests/table/fixture_integration_test.cpp share one canonical
// definition. Local aliases keep the existing call sites short.
using rackboy::tests::common::kExpectedFixtureFnv1a64;
using rackboy::tests::common::kFixtureSize;

// ---------------------------------------------------------------------------
// Fixture loading
// ---------------------------------------------------------------------------

// Try each candidate path in order and return the first
// one whose file opens successfully. Needed because the
// doctest binary may be launched from several different
// cwds depending on how the user runs it:
//
//   - `make -C tests test-sav` / `make -C tests test`:
//     cwd is `tests/`, fixture at `fixtures/...`
//   - direct invocation from the repo root:
//     cwd is repo root, fixture at `tests/fixtures/...`
//   - direct invocation from `tests/build/`:
//     cwd is `tests/build/`, fixture at `../fixtures/...`
//   - direct invocation from `tests/build/sav/` (how a
//     debugger lldb / gdb typically lands):
//     cwd is `tests/build/sav/`, fixture at
//     `../../fixtures/...`
//
// The sibling `tests/sav/header_test.cpp::try_load_fixture`
// already uses the same four-path set — keep them in sync.
// load_fixture_bytes, fnv1a_64, and find_first_block_for_slot moved to
// tests/common/fixture_loader.{h,cpp} in iteration 63 (bead
// rackboy-8jy). Using-declarations below keep existing call sites in
// this file short — they resolve to the shared module's copy so both
// this suite and tests/table/fixture_integration_test.cpp share one
// canonical implementation + one authoritative SHA fingerprint.
//
// The "candidate path" convention (four cwds depending on how the
// test binary is launched) and the "first match" chain-start
// assumption previously documented here now live in the shared
// module's header. If either changes, update there.
using rackboy::tests::common::find_first_block_for_slot;
using rackboy::tests::common::fnv1a_64;
using rackboy::tests::common::load_fixture_bytes;

// Decompress slot 0 of the fixture using the
// rackboy_sav_compression LSDJ block-chain decompressor.
// Fails the test case on any decompressor error.
std::vector<std::uint8_t> decompress_slot0(
    const std::vector<std::uint8_t>& sav)
{
    const std::uint8_t start_block =
        find_first_block_for_slot(sav, /*slot=*/0);
    std::string err;
    auto maybe =
        rb_compression::decompress(sav.data(), sav.size(),
                                      start_block, &err);
    if (!maybe.has_value()) {
        FAIL("block-chain decompressor failed: " << err);
    }
    return std::move(*maybe);
}

// "RACKBOY\0" — the 8-byte slot 0 project name written
// by rackboy-fixture-gen. LSDJ reserves exactly 8 bytes
// per slot (NAME_LEN). The trailing `\0` is the short-
// name pad byte.
constexpr char kExpectedSlot0Name[kSlot0NameLen] = {
    'R', 'A', 'C', 'K', 'B', 'O', 'Y', '\0'
};

// Per-slot expected values from
// `expected.json::instruments.per_slot`. Rows are in
// the on-disk alloc-map order: slot 0 (Pulse1), slot 1
// (Pulse2), slot 2 (Wave), slot 3 (Noise).
struct ExpectedInstrument {
    std::uint8_t type_byte;
    std::uint8_t param_bytes[kInstrumentRecordBytes];
};

// Source of truth: tests/fixtures/expected.json
// "instruments"."per_slot"[i]."param_bytes_hex". Every
// byte below is hand-transcribed from that JSON and must
// be updated in lockstep with any fixture regeneration.
constexpr ExpectedInstrument kExpectedInstruments[4] = {
    // Slot 0 — Pulse1:
    // param_bytes_hex "00f000ff000000830000000000000000"
    {kInstrumentTypePulse,
     {0x00, 0xF0, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x83,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    // Slot 1 — Pulse2:
    // param_bytes_hex "00e0007f000000430000000000000000"
    {kInstrumentTypePulse,
     {0x00, 0xE0, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x43,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    // Slot 2 — Wave:
    // param_bytes_hex "01030000000000000000000400000000"
    {kInstrumentTypeWave,
     {0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}},
    // Slot 3 — Noise:
    // param_bytes_hex "03f000ff550000000000000000000000"
    {kInstrumentTypeNoise,
     {0x03, 0xF0, 0x00, 0xFF, 0x55, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

}  // namespace

// ===========================================================================
// Fixture integrity — every-byte drift detector
// ===========================================================================

TEST_CASE("fixture: FNV-1a 64-bit fingerprint matches "
          "kExpectedFixtureFnv1a64")
{
    // Deterministic every-byte drift detector. expected.json
    // pins a cryptographic SHA-256 as the source of truth,
    // but rackboy deliberately does not link any crypto
    // library, so this test uses a 64-bit FNV-1a hash
    // instead. FNV-1a is not cryptographically secure but
    // IS sensitive to any single-byte flip across the
    // entire 131072-byte buffer, which is exactly what
    // we need: catch drift between the committed
    // fixture and the hand-mirrored expected values in
    // this file and in expected.json.
    //
    // Update protocol on a legitimate fixture regen:
    //
    //   1. Regenerate `tests/fixtures/rackboy_v1.sav`
    //      via the pinned rackboy-fixture-gen binary.
    //   2. Compute the new FNV-1a with:
    //        python3 -c "import sys;\
    //          h=0xcbf29ce484222325;\
    //          p=0x100000001b3;\
    //          m=0xffffffffffffffff;\
    //          [exec('h^=b; h=(h*p)&m') \
    //             for b in open(sys.argv[1],'rb').read()];\
    //          print(hex(h))" \
    //        tests/fixtures/rackboy_v1.sav
    //   3. Update `kExpectedFixtureFnv1a64` here.
    //   4. Update `fixture_sha256` in
    //      `tests/fixtures/expected.json` with `sha256sum
    //      tests/fixtures/rackboy_v1.sav`.
    //   5. Walk the hand-mirrored per-slot expected
    //      values in `kExpectedInstruments[]` below and
    //      update any that drifted.
    auto sav = load_fixture_bytes();
    REQUIRE_EQ(sav.size(), kFixtureSize);
    const std::uint64_t actual = fnv1a_64(sav);
    CAPTURE(actual);
    CAPTURE(kExpectedFixtureFnv1a64);
    CHECK_EQ(actual, kExpectedFixtureFnv1a64);
}

// ===========================================================================
// Container / header layer checks (parser_acceptance_checks[0..6])
// ===========================================================================

TEST_CASE("fixture: file size is exactly 131072 bytes")
{
    auto sav = load_fixture_bytes();
    CHECK_EQ(sav.size(), kFixtureSize);
}

TEST_CASE("fixture: 'jk' magic bytes at 0x813E")
{
    auto sav = load_fixture_bytes();
    CHECK_EQ(sav[kMagicOffset + 0], kMagicByte0);
    CHECK_EQ(sav[kMagicOffset + 1], kMagicByte1);
}

TEST_CASE("fixture: active project slot byte at 0x8140 is 0")
{
    auto sav = load_fixture_bytes();
    CHECK_EQ(sav[kActiveSlotOffset], kExpectedActiveSlot);
}

TEST_CASE("fixture: slot 0 name at [0x8000, 0x8008) is 'RACKBOY\\0'")
{
    auto sav = load_fixture_bytes();
    for (std::size_t i = 0; i < kSlot0NameLen; ++i) {
        CAPTURE(i);
        CHECK_EQ(sav[kSlot0NameOffset + i],
                 static_cast<std::uint8_t>(kExpectedSlot0Name[i]));
    }
}

TEST_CASE("fixture: header version byte at 0x8100 is 0x1B (LSDJ 9.4.0)")
{
    auto sav = load_fixture_bytes();
    CHECK_EQ(sav[kHeaderVersionOffset], kExpectedVersionByte);
}

TEST_CASE("fixture: validate_header returns Ok and read_header "
          "populates the expected HeaderInfo fields")
{
    auto sav = load_fixture_bytes();
    CHECK_EQ(validate_header(sav.data(), sav.size()),
             HeaderError::Ok);

    HeaderInfo info{};
    CHECK_EQ(read_header(sav.data(), sav.size(), &info),
             HeaderError::Ok);
    CHECK_EQ(info.active_slot,    kExpectedActiveSlot);
    CHECK_EQ(info.header_version, kExpectedVersionByte);
    CHECK(info.magic_ok);
}

TEST_CASE("fixture: block allocation table owns blocks 0..10 "
          "for slot 0, rest are 0xFF")
{
    // expected.json documents the literal layout as
    // "slot_0_block_ids_with_owner_zero": [0..10] and
    // byte_range 0x8141..=0x814B owned, 0x814C..=0x81FF
    // unused. Check the literal layout rather than
    // aggregate counts so a future drift that swaps
    // owned indices (e.g. the authoring tool starts at
    // block 5 instead of block 0) fails with a precise
    // per-index report.
    auto sav = load_fixture_bytes();
    for (std::size_t i = 0; i < kBlockAllocLen; ++i) {
        CAPTURE(i);
        const std::uint8_t b = sav[kBlockAllocOffset + i];
        if (i < kExpectedOwnedBlocks) {
            CHECK_EQ(b, 0x00);
        } else {
            CHECK_EQ(b, 0xFF);
        }
    }
}

// ===========================================================================
// Decompression layer (parser_acceptance_checks[7..12])
// ===========================================================================

TEST_CASE("fixture: block-chain decompressor produces exactly 32768 bytes")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);
    CHECK_EQ(song.size(), kDecompressedSongSizeBytes);
}

TEST_CASE("fixture: decompressed byte at 0x7FFF is 0x1B "
          "(authoritative LSDJ version byte)")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);
    CHECK_EQ(song[0x7FFF], kExpectedVersionByte);
}

TEST_CASE("fixture: decompressed instrument alloc map at [0x2040, 0x2044) "
          "is 0x01, rest of the map is 0x00")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);
    for (std::size_t i = 0; i < kInstrumentAllocMapLen; ++i) {
        CAPTURE(i);
        const std::uint8_t b =
            song[kInstrumentAllocMapOffset + i];
        if (i < 4) {
            CHECK_EQ(b, kInstrumentAllocated);
        } else {
            CHECK_EQ(b, kInstrumentUnallocated);
        }
    }
}

TEST_CASE("fixture: decompressed table alloc map at [0x2020, 0x2040) "
          "is all zeros (no tables allocated)")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);
    for (std::size_t i = 0; i < kTableAllocMapLen; ++i) {
        CAPTURE(i);
        CHECK_EQ(song[kTableAllocMapOffset + i], 0x00);
    }
}

TEST_CASE("fixture: decompressed instrument records at 0x3080 "
          "have the expected type bytes per slot")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);
    for (std::size_t slot = 0; slot < 4; ++slot) {
        CAPTURE(slot);
        const std::uint8_t type_byte =
            song[kInstrumentParamsOffset
                 + slot * kInstrumentRecordBytes];
        CHECK_EQ(type_byte,
                 kExpectedInstruments[slot].type_byte);
    }
}

// ===========================================================================
// Sub-parser wiring
// ===========================================================================

TEST_CASE("fixture: parse_instruments populates the expected per-channel "
          "pools with matching raw bytes")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);

    RackboyBank bank{};
    const InstrumentsError ie =
        parse_instruments(song.data(), song.size(), &bank);
    CHECK_EQ(ie, InstrumentsError::Ok);

    // Per the iter 28 split convention, every Pulse
    // record (type 0x00) lands in pulse1_pool until the
    // chain-based pulse1/pulse2 split bead lands, so the
    // fixture's two Pulses both sit in pulse1_pool even
    // though expected.json documents them as Pulse1 +
    // Pulse2 by authoring intent.
    CHECK_EQ(bank.pulse1_count, 2);
    CHECK_EQ(bank.pulse2_count, 0);
    CHECK_EQ(bank.wave_count,   1);
    CHECK_EQ(bank.noise_count,  1);
    CHECK(bank.loaded);

    // Slot 0 — Pulse1 authoring intent.
    for (std::size_t i = 0; i < kInstrumentRecordBytes; ++i) {
        CAPTURE(i);
        CHECK_EQ(bank.pulse1_pool[0].raw[i],
                 kExpectedInstruments[0].param_bytes[i]);
    }
    // Slot 1 — Pulse2 authoring intent (lands in
    // pulse1_pool slot 1 today).
    for (std::size_t i = 0; i < kInstrumentRecordBytes; ++i) {
        CAPTURE(i);
        CHECK_EQ(bank.pulse1_pool[1].raw[i],
                 kExpectedInstruments[1].param_bytes[i]);
    }
    // Slot 2 — Wave.
    for (std::size_t i = 0; i < kInstrumentRecordBytes; ++i) {
        CAPTURE(i);
        CHECK_EQ(bank.wave_pool[0].raw[i],
                 kExpectedInstruments[2].param_bytes[i]);
    }
    // Slot 3 — Noise.
    for (std::size_t i = 0; i < kInstrumentRecordBytes; ++i) {
        CAPTURE(i);
        CHECK_EQ(bank.noise_pool[0].raw[i],
                 kExpectedInstruments[3].param_bytes[i]);
    }
}

TEST_CASE("fixture: parse_tables leaves every table slot unoccupied")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);

    RackboyBank bank{};
    // parse_tables runs in the pipeline AFTER
    // parse_instruments, which is responsible for
    // bank_reset. Mimic that ordering here so the test
    // matches the facade's invariant: the table parser
    // sees a freshly-reset bank when called standalone.
    CHECK_EQ(parse_instruments(song.data(), song.size(), &bank),
             InstrumentsError::Ok);

    const TablesError te =
        parse_tables(song.data(), song.size(), &bank);
    CHECK_EQ(te, TablesError::Ok);
    for (std::size_t t = 0; t < kTableAllocMapLen; ++t) {
        CAPTURE(t);
        CHECK_FALSE(bank.tables[t].occupied);
    }
}

TEST_CASE("fixture: parse_waves produces a non-empty wave pool "
          "(default-wave expansion from the 0xE0 0xF0 opcode)")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);

    RackboyBank bank{};
    CHECK_EQ(parse_instruments(song.data(), song.size(), &bank),
             InstrumentsError::Ok);
    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);

    const WavesError we =
        parse_waves(song.data(), song.size(), &bank);
    CHECK_EQ(we, WavesError::Ok);

    // The decompressor expands the default-wave pattern
    // (triangle) into the wave frame pool via the
    // 0xE0 0xF0 opcode at fixture-author time, so the
    // fixture's wave pool is non-empty even though no
    // Wave instrument explicitly authors any wave
    // frames. Check that slot 0's 16 bytes match the
    // canonical `kDefaultWave` pattern byte-for-byte —
    // this locks two invariants at once:
    //
    //   1. parse_waves actually copies the expanded
    //      bytes into bank.waves[0].samples (not just
    //      sets occupied=true).
    //   2. The block-chain decompressor is expanding the
    //      right pattern, matching
    //      compression.rs::DEFAULT_WAVE.
    CHECK(bank.waves[0].occupied);
    for (std::size_t b = 0; b < kWaveFrameBytes; ++b) {
        CAPTURE(b);
        CHECK_EQ(bank.waves[0].samples[b],
                 rackboy::sav::compression::kDefaultWave[b]);
    }
}

// ===========================================================================
// Facade end-to-end
// ===========================================================================

TEST_CASE("fixture: load_rackboy_sav(facade) returns Ok and populates "
          "the expected bank state")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);

    RackboyBank bank{};
    RackboyBankNames names{};

    LoadInputs in{};
    in.raw_sav               = sav.data();
    in.raw_sav_len           = sav.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();
    in.sidecar_json          = nullptr;
    in.sidecar_json_len      = 0;

    const LoadResult r =
        load_rackboy_sav(in, &bank, &names);
    CHECK_EQ(r.phase, LoadPhase::Ok);
    CHECK_EQ(r.sub_error_code, 0);

    CHECK_EQ(bank.pulse1_count, 2);
    CHECK_EQ(bank.wave_count,   1);
    CHECK_EQ(bank.noise_count,  1);
    CHECK(bank.loaded);

    // No sidecar -> every name slot remains empty.
    CHECK_EQ(names.pulse1[0].text[0], '\0');
    CHECK_EQ(names.wave  [0].text[0], '\0');
}

TEST_CASE("fixture: load_rackboy_sav applies a synthetic sidecar on "
          "top of the fixture")
{
    auto sav  = load_fixture_bytes();
    auto song = decompress_slot0(sav);

    // Hand-author a sidecar targeting the two pulses,
    // the wave, and the noise. All four names fit under
    // the 15-byte printable budget.
    const std::string sidecar =
        "{"
        "\"PU1-00\": \"LEAD1\","
        "\"PU1-01\": \"LEAD2\","
        "\"WV-00\":  \"PAD\","
        "\"NO-00\":  \"KICK\""
        "}";

    RackboyBank bank{};
    RackboyBankNames names{};

    LoadInputs in{};
    in.raw_sav               = sav.data();
    in.raw_sav_len           = sav.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();
    in.sidecar_json          = sidecar.data();
    in.sidecar_json_len      = sidecar.size();

    const LoadResult r =
        load_rackboy_sav(in, &bank, &names);
    CHECK_EQ(r.phase, LoadPhase::Ok);

    CHECK_EQ(std::strcmp(names.pulse1[0].text, "LEAD1"), 0);
    CHECK_EQ(std::strcmp(names.pulse1[1].text, "LEAD2"), 0);
    CHECK_EQ(std::strcmp(names.wave  [0].text, "PAD"),   0);
    CHECK_EQ(std::strcmp(names.noise [0].text, "KICK"),  0);
}

// ===========================================================================
// Negative fixture: a mutated copy surfaces header errors
// ===========================================================================

TEST_CASE("fixture: mutating the magic bytes produces "
          "HeaderValidation/MagicMismatch through the facade")
{
    auto sav = load_fixture_bytes();
    // Corrupt the 'jk' magic — validate_header rejects
    // the buffer before the facade ever touches the
    // decompressed song, so we hand it a cheap zero
    // buffer rather than paying for a real decompress.
    // This is deliberate: the test is specifically
    // exercising the header phase in isolation, and a
    // real decompress here would mask "did the facade
    // short-circuit before step 4?" regressions.
    std::vector<std::uint8_t> song(
        kDecompressedSongSizeBytes, 0);
    sav[kMagicOffset + 0] = 0xDE;
    sav[kMagicOffset + 1] = 0xAD;

    RackboyBank bank{};
    RackboyBankNames names{};

    LoadInputs in{};
    in.raw_sav               = sav.data();
    in.raw_sav_len           = sav.size();
    in.decompressed_song     = song.data();
    in.decompressed_song_len = song.size();
    in.sidecar_json          = nullptr;
    in.sidecar_json_len      = 0;

    const LoadResult r =
        load_rackboy_sav(in, &bank, &names);
    CHECK_EQ(r.phase, LoadPhase::HeaderValidation);
    CHECK_EQ(r.sub_error_code,
             static_cast<int>(HeaderError::MagicMismatch));
    // Soft-fail invariant: on a header-phase failure,
    // the bank is left in a freshly-reset state.
    CHECK_FALSE(bank.loaded);
    CHECK_EQ(bank.pulse1_count, 0);
}
