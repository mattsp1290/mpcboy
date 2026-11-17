// rackboy_sav/rackboy_sav_tables.cpp
//
// Implementation of the table block parser declared in
// rackboy_sav_tables.h. Bead rackboy-u4v.
//
// The function shape is: null guards -> clear the bank's
// `tables[]` array -> walk the 32-byte alloc map once, and for
// every allocated slot, transpose the five column-major bytes
// per row into the row-major RackboyTableRow struct. No
// allocation, no exceptions, no STL containers, no recursion.

#include "rackboy_sav_tables.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "rackboy_sav_types.h"

namespace rackboy {
namespace sav {

// Confirm the platform uses two's-complement signed integers.
// C++20 mandates this, but rackboy targets C++17 for the Daisy
// toolchain. The asserts are free and turn the uint8_t -> int8_t
// cast on the transpose read below into a build-time-checked
// operation on every rackboy target.
static_assert(static_cast<std::int8_t>(
                  static_cast<std::uint8_t>(0xFF)) == -1,
              "rackboy requires a two's-complement platform.");
static_assert(static_cast<std::int8_t>(
                  static_cast<std::uint8_t>(0x80)) == -128,
              "rackboy requires a two's-complement platform.");

namespace {

// Compile-time-bounds-proven byte read from one of the five
// 512-byte LSDJ table columns. `column_offset` MUST be one of
// the five `kTable*Offset` constants in rackboy_sav_tables.h;
// the file-scope static_asserts there prove
// `column_offset + kTableColumnBytes <= kDecompressedSongSizeBytes`,
// so any (table, row) pair with `table < kTotalTableSlots` and
// `row < kTableRowsPerTable` is safe after the runtime
// `song_len >= kDecompressedSongSizeBytes` guard.
//
// The DEBUG-mode assert is belt-and-braces: the static asserts
// make this never fire in release, but a refactor that changes
// one constant without the other will catch here in unit tests.
inline std::uint8_t column_byte(const std::uint8_t* song,
                                std::size_t         column_offset,
                                std::size_t         table,
                                std::size_t         row) noexcept
{
#ifndef NDEBUG
    assert(table < kTotalTableSlots);
    assert(row   < kTableRowsPerTable);
#endif
    return song[column_offset
                + table * kTableRowsPerTable
                + row];
}

}  // namespace

TablesError parse_tables(const std::uint8_t* song,
                         std::size_t         song_len,
                         RackboyBank*        out_bank) noexcept
{
    // (1) Parameter-level guard: out_bank first so a null bank
    //     pointer cannot have its memory touched.
    if (out_bank == nullptr) {
        return TablesError::NullOutBank;
    }

    // (2) Reset the tables[] sub-array unconditionally (only
    //     after the out_bank null check). Every code path below
    //     this line leaves tables[] in a known state — either
    //     freshly cleared (on any error return) or fully
    //     populated (on Ok). Instrument pools and the `loaded`
    //     flag are NEVER touched: that is the instrument
    //     parser's job and parse_tables is a pipeline composition
    //     partner, not a bank owner.
    for (std::size_t t = 0; t < kTotalTableSlots; ++t) {
        table_reset(out_bank->tables[t]);
    }

    // (3) Song-side guards. After A-1 these happen AFTER the
    //     table_reset loop so a caller who poisoned tables[]
    //     still gets a cleared sub-array on every error path.
    if (song == nullptr) {
        return TablesError::NullSong;
    }
    if (song_len < kDecompressedSongSizeBytes) {
        return TablesError::SongTooShort;
    }

    // (3) Walk the alloc map and transpose every allocated
    //     table's rows from the column-major on-disk layout
    //     into the row-major RackboyTable::rows view. The
    //     static_asserts in the header prove every song[offset]
    //     read is in-bounds after the size check above.
    for (std::size_t t = 0; t < kTotalTableSlots; ++t) {
        const std::uint8_t alloc_byte =
            song[kTableAllocMapOffset + t];
        if (alloc_byte == 0) {
            // Slot is free — leave it in its just-reset state.
            continue;
        }

        // `table_reset` above already cleared this slot; we now
        // fill it with the 16 rows of on-disk data. `occupied`
        // is flipped to `true` as the last write to this slot so
        // a reader that sees `occupied == true` is guaranteed to
        // also see the accompanying row content.
        RackboyTable& table_out = out_bank->tables[t];

        for (std::size_t r = 0; r < kTableRowsPerTable; ++r) {
            RackboyTableRow& row_out = table_out.rows[r];

            // LSDJ transpose is signed in [-48, 48] in practice;
            // the bytes are two's-complement. The file-scope
            // static_asserts above confirm the platform matches
            // that representation, so the uint8_t -> int8_t
            // cast below is well-defined on every rackboy target.
            const std::uint8_t raw_transpose =
                column_byte(song, kTableTransposeOffset, t, r);
            row_out.transpose =
                static_cast<std::int8_t>(raw_transpose);

            row_out.cmd1 =
                column_byte(song, kTableCmd1Offset,    t, r);
            row_out.cmd1_param =
                column_byte(song, kTableCmd1ValOffset, t, r);
            row_out.cmd2 =
                column_byte(song, kTableCmd2Offset,    t, r);
            row_out.cmd2_param =
                column_byte(song, kTableCmd2ValOffset, t, r);
        }

        table_out.occupied = true;
    }

    return TablesError::Ok;
}

const char* tables_error_string(TablesError e) noexcept
{
    // No default case on purpose — -Wswitch-enum catches a
    // future enumerator addition at compile time.
    switch (e) {
        case TablesError::Ok:
            return "ok";
        case TablesError::NullSong:
            return "null decompressed-song buffer";
        case TablesError::NullOutBank:
            return "null output bank";
        case TablesError::SongTooShort:
            return "song buffer shorter than 32 KiB";
    }
    // Fallback is only reachable if a caller cast an arbitrary
    // int to TablesError — e.g. log-replay tooling deserializing
    // a corrupt log line. The TablesError enum value contract in
    // the header pins enumerators 0..3, so any other value is a
    // deserialization bug. We still return a string instead of
    // asserting because logging tools would rather print
    // "unknown" than crash.
    return "unknown TablesError value";
}

}  // namespace sav
}  // namespace rackboy
