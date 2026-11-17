// rackboy_sav/rackboy_sav_tables.h
//
// Table block parser: walks the table allocation map of a
// decompressed LSDJ song blob, pulls each allocated table's 16
// rows out of the column-major on-disk layout, and writes them
// into the row-major `RackboyTable` entries of a
// `rackboy::sav::RackboyBank`. Bead rackboy-u4v.
//
// What a "table" is
// -----------------
// LSDJ tables are 16-row expression programs a `T` command on an
// instrument row triggers. Each row carries one signed transpose
// offset and two command slots; each command slot is a raw
// `(letter_code, parameter)` byte pair. The per-command-letter
// semantics (A/L/V/H/...) are interpreted by rackboy_table
// (bead rackboy-tj3) at audio-thread playback time; THIS module
// does not decode command letters, it only shuffles bytes from
// the LSDJ on-disk column-major layout into the row-major
// `RackboyTableRow` struct the audio thread consumes.
//
// On-disk vs in-memory layout
// ---------------------------
// LSDJ stores each of the four command columns (plus the
// transpose column) as its own parallel array, 32 tables × 16
// rows = 512 bytes per array. Byte offsets in the decompressed
// song (from docs/COMPATIBILITY.md §"Decompressed song layout"):
//
//     Transposes      0x3480
//     cmd1 (letter)   0x3680
//     cmd1_val        0x3880
//     cmd2 (letter)   0x3A80
//     cmd2_val        0x3C80
//
// For table T, row R the five on-disk bytes live at:
//     0x3480 + T*16 + R   (transpose)
//     0x3680 + T*16 + R   (cmd1)
//     0x3880 + T*16 + R   (cmd1_val)
//     0x3A80 + T*16 + R   (cmd2)
//     0x3C80 + T*16 + R   (cmd2_val)
//
// The in-memory `RackboyTableRow` packs those five bytes
// row-major (transpose, cmd1, cmd1_param, cmd2, cmd2_param),
// which is what the audio thread's table interpreter reads. The
// parser's job is exactly this transposition — no decoding, no
// filtering, no masking.
//
// Allocation map
// --------------
// The 32-byte map at 0x2020 names which tables are in use.
// `0x00 == free`, any non-zero byte == allocated. LSDJ writes
// `0x01` as the canonical allocated value; rackboy accepts any
// non-zero byte as allocated to match the
// `docs/COMPATIBILITY.md` §"Decompressed song layout" contract
// which says "alloc maps are 0/1 presence bits ... non-zero =
// allocated". Tables that are free keep
// `RackboyTable::occupied == false` and their row content stays
// zeroed from the per-slot `table_reset` call the parser runs
// before walking the song.
//
// Note: `parse_instruments` is currently stricter on the
// instrument alloc map — it returns `AllocMapInvalid` on any
// byte that is not exactly 0x00 or 0x01. The asymmetry is
// tracked as a follow-up consistency bead; per the
// COMPATIBILITY.md text above the "any non-zero" reading is the
// authoritative one for both maps.
//
// The alloc map is indexed by table slot (0..31), NOT by the
// dense-prefix convention used for per-channel instrument pools.
// Instruments reference tables by slot number, so holes in the
// table array are meaningful; a parser that compacted into a
// dense prefix would break the instrument -> table slot linkage.
//
// Input contract
// --------------
//
//   song     — pointer to the first byte of the decompressed
//              song. Reads at most `kDecompressedSongSizeBytes`.
//              May be null (returns `NullSong`).
//   song_len — length of `song`. Must be >=
//              `kDecompressedSongSizeBytes`. Larger buffers are
//              accepted; only the first 32 KiB are read.
//   bank     — destination bank. May NOT be null.
//
// Output contract
// ---------------
// The parser updates the `tables[]` array of `bank` in place:
// allocated slots have their rows populated and `occupied = true`;
// unallocated slots are cleared (occupied = false, rows zeroed).
// The parser does NOT touch per-channel instrument pools or the
// `loaded` flag — that is the instrument parser's job, and the
// table parser is expected to run AFTER `parse_instruments` in a
// combined pipeline. The parser also does NOT call `bank_reset`
// first, because that would clobber instrument pools that the
// instrument pass has already populated.
//
// Soft-fail contract
// ------------------
// Every failure mode resets the `tables[]` sub-array via
// `table_reset` on each slot, so the bank's table sub-array is
// always in a known-clean state on return. Instrument pools and
// the `loaded` flag are NEVER touched — that is the instrument
// parser's job. The sole exception is `NullOutBank`: when the
// caller passes a null bank pointer the parser returns
// immediately without touching any memory, because there is no
// bank to reset.
//
// This parser is *typically* run after `parse_instruments` in a
// combined pipeline, but it is also safe to call standalone —
// a poisoned `tables[]` sub-array is cleared on every error
// path, not just the Ok path.
//
// `noexcept`; no allocation; every `song[offset]` read is
// bounds-proven at compile time against `kDecompressedSongSizeBytes`.

#ifndef RACKBOY_SAV_RACKBOY_SAV_TABLES_H_
#define RACKBOY_SAV_RACKBOY_SAV_TABLES_H_

#include <cstddef>
#include <cstdint>

#include "rackboy_sav_instruments.h"  // kDecompressedSongSizeBytes
#include "rackboy_sav_types.h"

namespace rackboy {
namespace sav {

// ---------------------------------------------------------------------------
// Decompressed-song table-layout constants
//
// Mirrored from rackboy_sav/spike/spike_parser.cpp and the LSDJ
// wire contract at docs/COMPATIBILITY.md §"Decompressed song
// layout". Per the "Mirror policy: spike-vs-production layout
// constants" section of docs/DECISIONS.md D-007, the spike-vs-
// production duplication is intentional and the spike is canonical;
// any change here must land in the same commit as the matching
// edit in rackboy_sav/spike/spike_parser.cpp.
// ---------------------------------------------------------------------------

// Table alloc map: 32 bytes at [0x2020, 0x2040). Byte at
// `kTableAllocMapOffset + slot` is `0x00` for free and any
// non-zero value for allocated. This module treats any non-zero
// byte as allocated, not just `0x01`, because LSDJ documents the
// map as a presence bit rather than a strict 0/1 boolean.
constexpr std::size_t kTableAllocMapOffset = 0x2020;
constexpr std::size_t kTableAllocMapLen    = 32;

// Total number of tables in the decompressed song. Must match
// `kMaxTables` in rackboy_sav_types.h so the row-major RackboyBank
// has room for every on-disk table.
constexpr std::size_t kTotalTableSlots = kTableAllocMapLen;

// Per-column on-disk offsets. Each column is a parallel 32×16
// byte array; index with `column_offset + table*16 + row`.
// Source: docs/COMPATIBILITY.md §"Decompressed song layout".
// MPCBOY LOCAL MOD: the table envelope (volume) column, 32 tables x 16 rows.
// Same offset as the extractor's TABLE_ENVELOPE_OFFSET (lsdj-sav project.rs).
constexpr std::size_t kTableEnvelopeOffset  = 0x1690;
constexpr std::size_t kTableTransposeOffset = 0x3480;
constexpr std::size_t kTableCmd1Offset      = 0x3680;
constexpr std::size_t kTableCmd1ValOffset   = 0x3880;
constexpr std::size_t kTableCmd2Offset      = 0x3A80;
constexpr std::size_t kTableCmd2ValOffset   = 0x3C80;

// Per-table row stride in each column. All five columns use the
// same stride (16 rows per table), so the parser uses this
// constant everywhere.
constexpr std::size_t kTableRowsPerTable = kTableRowCount;  // 16

// Size (in bytes) of a single column. The five columns occupy
// the five contiguous 512-byte runs 0x3480..0x3D80. A runtime
// read past column_offset + column_size is a bug in the offset
// arithmetic; the static_asserts below prove every read is
// in-bounds.
constexpr std::size_t kTableColumnBytes =
    kTotalTableSlots * kTableRowsPerTable;

static_assert(kTableColumnBytes == 512,
              "LSDJ 9.4.0 pins each table column at 32 tables * 16 "
              "rows = 512 bytes.");

// Every column must fit inside the decompressed song blob. The
// parser's `song_len >= kDecompressedSongSizeBytes` check is the
// only runtime guard; these static_asserts turn that check into
// a complete bounds proof for every read below.
static_assert(kTableAllocMapOffset + kTableAllocMapLen
              <= kDecompressedSongSizeBytes,
              "Table alloc map must fit inside the decompressed "
              "song blob.");
static_assert(kTableEnvelopeOffset  + kTableColumnBytes
              <= kDecompressedSongSizeBytes,
              "envelope column must lie inside the decompressed song.");
static_assert(kTableTransposeOffset + kTableColumnBytes
              <= kDecompressedSongSizeBytes,
              "Table transpose column must fit inside the "
              "decompressed song blob.");
static_assert(kTableCmd1Offset      + kTableColumnBytes
              <= kDecompressedSongSizeBytes,
              "Table cmd1 column must fit inside the decompressed "
              "song blob.");
static_assert(kTableCmd1ValOffset   + kTableColumnBytes
              <= kDecompressedSongSizeBytes,
              "Table cmd1_val column must fit inside the "
              "decompressed song blob.");
static_assert(kTableCmd2Offset      + kTableColumnBytes
              <= kDecompressedSongSizeBytes,
              "Table cmd2 column must fit inside the decompressed "
              "song blob.");
static_assert(kTableCmd2ValOffset   + kTableColumnBytes
              <= kDecompressedSongSizeBytes,
              "Table cmd2_val column must fit inside the "
              "decompressed song blob.");

// Tables-per-bank and rows-per-table have to stay aligned between
// the on-disk layout (kTableAllocMapLen) and the in-memory pool
// (kMaxTables in rackboy_sav_types.h). If either moves, this fires.
static_assert(kTotalTableSlots == kMaxTables,
              "kTotalTableSlots must equal kMaxTables so every "
              "on-disk table has a matching in-memory slot.");
static_assert(kTableRowsPerTable == kTableRowCount,
              "kTableRowsPerTable must equal kTableRowCount so the "
              "in-memory RackboyTable::rows layout matches the "
              "on-disk column stride.");

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

// Soft-fail return code for `parse_tables`. Values stable across
// rackboy v1 for log-replay tooling (enforced by the
// static_assert block below).
enum class TablesError : int {
    Ok            = 0,

    // `song == nullptr`.
    NullSong      = 1,

    // `out_bank == nullptr`.
    NullOutBank   = 2,

    // `song_len < kDecompressedSongSizeBytes`.
    SongTooShort  = 3,
};

static_assert(static_cast<int>(TablesError::Ok)           == 0,
              "TablesError::Ok must stay at 0.");
static_assert(static_cast<int>(TablesError::NullSong)     == 1,
              "TablesError::NullSong must stay at 1.");
static_assert(static_cast<int>(TablesError::NullOutBank)  == 2,
              "TablesError::NullOutBank must stay at 2.");
static_assert(static_cast<int>(TablesError::SongTooShort) == 3,
              "TablesError::SongTooShort must stay at 3.");

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Parse the table block of a decompressed LSDJ song blob into
// the `tables[]` array of a RackboyBank.
//
// Parameters:
//   song      — pointer to the first byte of the decompressed
//               song. Reads at most 32 KiB. May be null (returns
//               `NullSong`).
//   song_len  — length of `song`. Must be >=
//               `kDecompressedSongSizeBytes`. Larger buffers are
//               accepted; only the first 32 KiB are read.
//   out_bank  — destination bank. May NOT be null; passing null
//               returns `NullOutBank` without touching any
//               memory. The parser rewrites the `tables[]`
//               array in place but does NOT touch the per-
//               channel instrument pools or `loaded` flag — it
//               is expected to run in a combined pipeline with
//               `parse_instruments`.
//
// Returns: `TablesError::Ok` on success.
//
// Every on-disk `song[offset]` read is bounds-proven at compile
// time against `kDecompressedSongSizeBytes`; the only runtime
// guard is the `song_len` length check.
TablesError parse_tables(const std::uint8_t* song,
                         std::size_t         song_len,
                         RackboyBank*        out_bank) noexcept;

// Return a short human-readable string for a TablesError.
// Static-lifetime storage; callers do NOT free the returned
// pointer.
const char* tables_error_string(TablesError e) noexcept;

}  // namespace sav
}  // namespace rackboy

#endif  // RACKBOY_SAV_RACKBOY_SAV_TABLES_H_
