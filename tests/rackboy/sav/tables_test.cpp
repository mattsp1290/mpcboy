// tests/sav/tables_test.cpp
//
// Unit tests for rackboy_sav/rackboy_sav_tables.{h,cpp} — the
// table block parser for rackboy-u4v.
//
// Tests build a synthetic 32 KiB "decompressed song" blob and
// stamp only the bytes the parser reads (alloc map + five
// parallel column arrays). Lock every error code, every
// "occupied/unoccupied" transition, and the column-major ->
// row-major transposition invariant.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "doctest/doctest.h"

#include "rackboy_sav_instruments.h"  // kDecompressedSongSizeBytes
#include "rackboy_sav_tables.h"
#include "rackboy_sav_types.h"

using rackboy::sav::kDecompressedSongSizeBytes;
using rackboy::sav::kMaxTables;
using rackboy::sav::kTableAllocMapLen;
using rackboy::sav::kTableAllocMapOffset;
using rackboy::sav::kTableCmd1Offset;
using rackboy::sav::kTableCmd1ValOffset;
using rackboy::sav::kTableCmd2Offset;
using rackboy::sav::kTableCmd2ValOffset;
using rackboy::sav::kTableRowCount;
using rackboy::sav::kTableRowsPerTable;
using rackboy::sav::kTableTransposeOffset;
using rackboy::sav::kTotalTableSlots;
using rackboy::sav::parse_tables;
using rackboy::sav::RackboyBank;
using rackboy::sav::RackboyTable;
using rackboy::sav::TablesError;
using rackboy::sav::tables_error_string;

namespace {

std::vector<std::uint8_t> make_empty_song()
{
    return std::vector<std::uint8_t>(kDecompressedSongSizeBytes, 0);
}

// Stamp an allocated table with a deterministic per-row pattern
// into the five column-major arrays. Each row's five bytes are
// derived from (table, row) so a round-trip through the parser
// can be checked byte-for-byte.
//
// Pattern design:
//   transpose  = (int8_t)(table * 3 + row - 40)  // negative + positive
//   cmd1       = 0x40 | (table & 0x0F)            // letter-ish
//   cmd1_param = 0x80 + row
//   cmd2       = 0x50 | (row & 0x0F)
//   cmd2_param = table * 16 + row + 1             // never zero
//
// None of the values are zero except possibly transpose (that is
// expected — a legitimate LSDJ table row can have transpose 0).
// The values are deliberately chosen so transpose can be both
// negative and positive across the table to exercise the uint8_t
// -> int8_t cast at reader time.
void allocate_table(std::vector<std::uint8_t>& song,
                    std::size_t                table,
                    std::uint8_t               alloc_byte = 0x01)
{
    song[kTableAllocMapOffset + table] = alloc_byte;
    for (std::size_t row = 0; row < kTableRowsPerTable; ++row) {
        const std::size_t slot_row = table * kTableRowsPerTable + row;

        const std::int8_t transpose =
            static_cast<std::int8_t>(table * 3 + row - 40);
        // Two's-complement bit pattern for the signed value.
        song[kTableTransposeOffset + slot_row] =
            static_cast<std::uint8_t>(transpose);

        song[kTableCmd1Offset    + slot_row] =
            static_cast<std::uint8_t>(0x40 | (table & 0x0F));
        song[kTableCmd1ValOffset + slot_row] =
            static_cast<std::uint8_t>(0x80 + row);
        song[kTableCmd2Offset    + slot_row] =
            static_cast<std::uint8_t>(0x50 | (row & 0x0F));
        song[kTableCmd2ValOffset + slot_row] =
            static_cast<std::uint8_t>(table * 16 + row + 1);
    }
}

// Mirror of `allocate_table`'s per-row formulas. Returns the
// five expected bytes for a given (table, row) pair so a test
// CHECK can assert the parser wrote the exact bytes we stamped.
struct ExpectedRow {
    std::int8_t  transpose;
    std::uint8_t cmd1;
    std::uint8_t cmd1_param;
    std::uint8_t cmd2;
    std::uint8_t cmd2_param;
};

ExpectedRow expected_row(std::size_t table, std::size_t row)
{
    ExpectedRow e{};
    e.transpose  =
        static_cast<std::int8_t>(table * 3 + row - 40);
    e.cmd1       =
        static_cast<std::uint8_t>(0x40 | (table & 0x0F));
    e.cmd1_param =
        static_cast<std::uint8_t>(0x80 + row);
    e.cmd2       =
        static_cast<std::uint8_t>(0x50 | (row & 0x0F));
    e.cmd2_param =
        static_cast<std::uint8_t>(table * 16 + row + 1);
    return e;
}

void check_table_matches_expected(const RackboyTable& t,
                                   std::size_t         table_idx)
{
    CHECK(t.occupied);
    for (std::size_t row = 0; row < kTableRowsPerTable; ++row) {
        CAPTURE(table_idx);
        CAPTURE(row);
        const ExpectedRow e = expected_row(table_idx, row);
        CHECK_EQ(t.rows[row].transpose,  e.transpose);
        CHECK_EQ(t.rows[row].cmd1,       e.cmd1);
        CHECK_EQ(t.rows[row].cmd1_param, e.cmd1_param);
        CHECK_EQ(t.rows[row].cmd2,       e.cmd2);
        CHECK_EQ(t.rows[row].cmd2_param, e.cmd2_param);
    }
}

// Confirm every row of a table is zeroed (the post-table_reset
// state). Used on unallocated slots and on allocated slots that
// a test expects to have been cleared.
void check_table_zeroed(const RackboyTable& t)
{
    CHECK_FALSE(t.occupied);
    for (std::size_t row = 0; row < kTableRowsPerTable; ++row) {
        CAPTURE(row);
        CHECK_EQ(t.rows[row].transpose,  0);
        CHECK_EQ(t.rows[row].cmd1,       0);
        CHECK_EQ(t.rows[row].cmd1_param, 0);
        CHECK_EQ(t.rows[row].cmd2,       0);
        CHECK_EQ(t.rows[row].cmd2_param, 0);
    }
}

}  // namespace

TEST_CASE("parse_tables rejects null out_bank")
{
    auto song = make_empty_song();
    CHECK_EQ(parse_tables(song.data(), song.size(), nullptr),
             TablesError::NullOutBank);
}

TEST_CASE("parse_tables rejects null song")
{
    RackboyBank bank{};
    CHECK_EQ(parse_tables(nullptr, kDecompressedSongSizeBytes, &bank),
             TablesError::NullSong);
}

TEST_CASE("parse_tables NullSong leaves tables[] cleared even when "
          "caller poisoned the bank")
{
    RackboyBank bank{};
    // Poison: pretend a previous run left junk in tables[].
    bank.tables[5].occupied           = true;
    bank.tables[5].rows[0].transpose  = 42;
    bank.tables[5].rows[0].cmd1       = 0xAA;
    bank.tables[5].rows[0].cmd1_param = 0xBB;

    // Poison the instrument-side state the tables parser must
    // NOT touch so we lock the cross-pipeline invariant on the
    // error path too.
    bank.pulse1_count = 7;
    bank.loaded       = true;

    CHECK_EQ(parse_tables(nullptr, kDecompressedSongSizeBytes, &bank),
             TablesError::NullSong);

    // tables[5] is back to its post-table_reset state.
    CHECK_FALSE(bank.tables[5].occupied);
    CHECK_EQ(bank.tables[5].rows[0].transpose,  0);
    CHECK_EQ(bank.tables[5].rows[0].cmd1,       0);
    CHECK_EQ(bank.tables[5].rows[0].cmd1_param, 0);

    // Instrument-side state untouched — the tables parser owns
    // tables[] only.
    CHECK_EQ(bank.pulse1_count, 7);
    CHECK(bank.loaded);
}

TEST_CASE("parse_tables rejects a too-short song")
{
    std::vector<std::uint8_t> short_song(kDecompressedSongSizeBytes - 1, 0);
    RackboyBank bank{};
    CHECK_EQ(parse_tables(short_song.data(), short_song.size(), &bank),
             TablesError::SongTooShort);
}

TEST_CASE("parse_tables SongTooShort leaves tables[] cleared even "
          "when caller poisoned the bank")
{
    std::vector<std::uint8_t> short_song(kDecompressedSongSizeBytes - 1, 0);
    RackboyBank bank{};
    bank.tables[10].occupied     = true;
    bank.tables[10].rows[3].cmd2 = 0x77;
    bank.pulse1_count            = 4;
    bank.loaded                  = true;

    CHECK_EQ(parse_tables(short_song.data(), short_song.size(), &bank),
             TablesError::SongTooShort);

    CHECK_FALSE(bank.tables[10].occupied);
    CHECK_EQ(bank.tables[10].rows[3].cmd2, 0);
    CHECK_EQ(bank.pulse1_count, 4);
    CHECK(bank.loaded);
}

TEST_CASE("parse_tables accepts an oversized song (reads only first 32 KiB)")
{
    std::vector<std::uint8_t> big(kDecompressedSongSizeBytes * 2, 0);
    allocate_table(big, /*table=*/5);
    // Stamp bogus bytes past the 32 KiB boundary — must not
    // affect the result.
    for (std::size_t i = kDecompressedSongSizeBytes; i < big.size(); ++i) {
        big[i] = 0xCD;
    }

    RackboyBank bank{};
    CHECK_EQ(parse_tables(big.data(), big.size(), &bank),
             TablesError::Ok);
    check_table_matches_expected(bank.tables[5], /*table_idx=*/5);
}

TEST_CASE("parse_tables leaves every table slot cleared on an empty song")
{
    // Zero alloc map -> no tables. Every `tables[i]` must be
    // fully zeroed and marked unoccupied.
    auto song = make_empty_song();
    RackboyBank bank{};

    // Poison every table slot so we can prove table_reset ran.
    for (std::size_t t = 0; t < kMaxTables; ++t) {
        bank.tables[t].occupied             = true;
        bank.tables[t].rows[0].transpose    = 42;
        bank.tables[t].rows[0].cmd1         = 0xAA;
    }

    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);
    for (std::size_t t = 0; t < kMaxTables; ++t) {
        CAPTURE(t);
        check_table_zeroed(bank.tables[t]);
    }
}

TEST_CASE("parse_tables round-trips an allocated table byte-for-byte")
{
    auto song = make_empty_song();
    allocate_table(song, /*table=*/7);

    RackboyBank bank{};
    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);

    check_table_matches_expected(bank.tables[7], /*table_idx=*/7);

    // Every other table must be left zeroed / unoccupied.
    for (std::size_t t = 0; t < kMaxTables; ++t) {
        if (t == 7) continue;
        CAPTURE(t);
        check_table_zeroed(bank.tables[t]);
    }
}

TEST_CASE("parse_tables handles multiple non-adjacent allocated tables")
{
    // Allocation map is indexed (not dense prefix) — tables at
    // slots 3, 10, and 31 must all land in bank.tables[3],
    // bank.tables[10], bank.tables[31] with their respective
    // content intact. Every other slot is left zeroed.
    auto song = make_empty_song();
    allocate_table(song, 3);
    allocate_table(song, 10);
    allocate_table(song, 31);

    RackboyBank bank{};
    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);

    check_table_matches_expected(bank.tables[3],  3);
    check_table_matches_expected(bank.tables[10], 10);
    check_table_matches_expected(bank.tables[31], 31);

    for (std::size_t t = 0; t < kMaxTables; ++t) {
        if (t == 3 || t == 10 || t == 31) continue;
        CAPTURE(t);
        check_table_zeroed(bank.tables[t]);
    }
}

TEST_CASE("parse_tables fills all 32 table slots when every slot is allocated")
{
    auto song = make_empty_song();
    for (std::size_t t = 0; t < kTableAllocMapLen; ++t) {
        allocate_table(song, t);
    }

    RackboyBank bank{};
    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);

    for (std::size_t t = 0; t < kMaxTables; ++t) {
        CAPTURE(t);
        check_table_matches_expected(bank.tables[t], t);
    }
}

TEST_CASE("parse_tables treats any non-zero alloc byte as allocated")
{
    // LSDJ documents the table alloc map as "0x00 = free,
    // non-zero = allocated". Rackboy mirrors that contract:
    // allocating with 0xFF should produce the same result as
    // 0x01. This test pins the contract.
    auto song = make_empty_song();
    allocate_table(song, /*table=*/4, /*alloc_byte=*/0xFF);

    RackboyBank bank{};
    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);
    check_table_matches_expected(bank.tables[4], /*table_idx=*/4);
}

TEST_CASE("parse_tables preserves a negative transpose byte's signed interpretation")
{
    // Stamp a specific table with rows whose transpose bytes
    // span the full int8_t range, including 0x80 (min), 0xFF
    // (-1), 0x7F (max). Parser must preserve the signed
    // interpretation — a naive `(int8_t)` cast of 0x80 is
    // implementation-defined in C++17 but well-defined in
    // practice on every rackboy target.
    auto song = make_empty_song();
    song[kTableAllocMapOffset + 2] = 0x01;

    // Hand-stamp rows 0..15 with specific transpose bytes.
    const std::uint8_t transpose_bytes[16] = {
        0x00, 0x01, 0x7F, 0x80, 0xFF, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0xE0, 0xD0, 0xC0, 0xB0, 0xA0,
    };
    for (std::size_t row = 0; row < 16; ++row) {
        const std::size_t slot_row = 2 * kTableRowsPerTable + row;
        song[kTableTransposeOffset + slot_row] =
            transpose_bytes[row];
        // Other columns zero is fine for this check.
    }

    RackboyBank bank{};
    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);
    CHECK(bank.tables[2].occupied);

    // Expected signed values:
    //   0x00=0, 0x01=1, 0x7F=127, 0x80=-128, 0xFF=-1, 0x02=2,
    //   0x03=3, 0x04=4, 0x05=5, 0x06=6, 0x07=7, 0xE0=-32,
    //   0xD0=-48, 0xC0=-64, 0xB0=-80, 0xA0=-96.
    const std::int8_t expected[16] = {
        0, 1, 127, -128, -1, 2, 3, 4,
        5, 6, 7, -32, -48, -64, -80, -96,
    };
    for (std::size_t row = 0; row < 16; ++row) {
        CAPTURE(row);
        CHECK_EQ(bank.tables[2].rows[row].transpose,
                 expected[row]);
    }
}

TEST_CASE("parse_tables row-major transposition pins the "
          "(table, row) order, not (row, table)")
{
    // Catches a bug where the inner loop accidentally swaps
    // the table and row indices at the column read site. Stamp
    // the cmd1 column of table 1 with row-dependent bytes that
    // are asymmetric under (t, r) swap: a swap would write
    // ((1 << 4) | 0xA) == 0x1A to every row, which fails every
    // CHECK below.
    auto song = make_empty_song();
    song[kTableAllocMapOffset + 1] = 0x01;

    for (std::size_t row = 0; row < kTableRowsPerTable; ++row) {
        const std::size_t slot_row = 1 * kTableRowsPerTable + row;
        song[kTableCmd1Offset + slot_row] =
            static_cast<std::uint8_t>((row << 4) | 0xA);
    }

    RackboyBank bank{};
    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);
    CHECK(bank.tables[1].occupied);
    for (std::size_t row = 0; row < kTableRowsPerTable; ++row) {
        CAPTURE(row);
        CHECK_EQ(bank.tables[1].rows[row].cmd1,
                 static_cast<std::uint8_t>((row << 4) | 0xA));
    }
}

TEST_CASE("parse_tables transpose cast preserves the full "
          "uint8_t -> int8_t mapping for every byte value")
{
    // 256 byte values in 16 rows/table = 16 tables to sweep the
    // full range in a single parse call.
    auto song = make_empty_song();
    for (std::size_t t = 0; t < 16; ++t) {
        song[kTableAllocMapOffset + t] = 0x01;
        for (std::size_t row = 0; row < kTableRowsPerTable; ++row) {
            const std::size_t slot_row = t * kTableRowsPerTable + row;
            const std::uint8_t byte =
                static_cast<std::uint8_t>(t * 16 + row);
            song[kTableTransposeOffset + slot_row] = byte;
        }
    }

    RackboyBank bank{};
    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);

    for (std::size_t t = 0; t < 16; ++t) {
        for (std::size_t row = 0; row < kTableRowsPerTable; ++row) {
            CAPTURE(t);
            CAPTURE(row);
            const std::uint8_t byte =
                static_cast<std::uint8_t>(t * 16 + row);
            const std::int8_t expected =
                static_cast<std::int8_t>(byte);
            CHECK_EQ(bank.tables[t].rows[row].transpose, expected);
        }
    }
}

TEST_CASE("parse_tables does not touch instrument pools or loaded flag")
{
    // The parser is expected to run in a pipeline AFTER
    // parse_instruments. It must not clobber the per-channel
    // pools or the `loaded` flag that parse_instruments set.
    auto song = make_empty_song();
    allocate_table(song, /*table=*/0);

    RackboyBank bank{};
    // Populate the bank as if parse_instruments had run.
    bank.pulse1_count = 3;
    bank.wave_count   = 1;
    bank.loaded       = true;
    for (std::size_t k = 0; k < 16; ++k) {
        bank.pulse1_pool[0].raw[k] = 0x55;
    }

    CHECK_EQ(parse_tables(song.data(), song.size(), &bank),
             TablesError::Ok);

    // Table pass did its job.
    check_table_matches_expected(bank.tables[0], /*table_idx=*/0);

    // Instrument pools and `loaded` untouched.
    CHECK_EQ(bank.pulse1_count, 3);
    CHECK_EQ(bank.wave_count,   1);
    CHECK(bank.loaded);
    for (std::size_t k = 0; k < 16; ++k) {
        CAPTURE(k);
        CHECK_EQ(bank.pulse1_pool[0].raw[k], 0x55);
    }
}

TEST_CASE("tables_error_string returns a non-null, distinct, non-empty "
          "string for every error")
{
    const TablesError all[] = {
        TablesError::Ok,
        TablesError::NullSong,
        TablesError::NullOutBank,
        TablesError::SongTooShort,
    };
    constexpr std::size_t kAllCount = sizeof(all) / sizeof(all[0]);

    const char* seen[kAllCount] = {};
    std::size_t seen_count = 0;
    for (TablesError e : all) {
        const char* s = tables_error_string(e);
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
