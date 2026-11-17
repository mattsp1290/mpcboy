// src/engine/sav_import.h - import tables and waves from an LSDJ 9.4.0 .sav (WP11,
// 06-lsdj-sav-import.md). Allocates (decompression, file read): call it from a
// worker thread only, never from the audio thread.
//
// Not load_rackboy_sav: import must not depend on instruments or the sidecar, so
// this runs the header check, the block-chain decompressor, parse_tables and
// parse_waves directly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "table_bank.h"

namespace mpcboy {

// Stable codes: they are shown to the user as "ERR <code>".
enum class ImportResult : int
{
    Ok = 0,
    ReadFailed = 1,         // cannot open / read the file
    BadSize = 2,            // not exactly 131072 bytes
    NotSav = 3,             // header magic / active-slot check failed
    WrongVersion = 4,       // header version byte is not 0x1B (LSDJ 9.4.0)
    SlotNotZero = 5,        // active project slot is not 0
    NoBlocks = 6,           // no block allocated to slot 0
    DecompressFailed = 7,   // block chain is damaged
    ParseFailed = 8,        // parse_tables / parse_waves rejected the song
};

struct ImportReport
{
    int tablesAllocated = 0;      // allocated tables in the file (0..32)
    int tablesImported = 0;       // mapped to slots 1..16
    int tablesDropped = 0;        // allocated tables beyond the first 16
    int wavesImported = 0;        // non-empty waves among waves 0..15
    int unsupportedCommands = 0;  // non-zero command bytes outside D E H K L N P R S V W (imported tables only)
    int wavesBeyond16 = 0;        // W commands pointing at waves >= 16 (no-ops here)
};

constexpr size_t kSavFileSize = 131072;

// Reads `path`, then import_sav_bytes. `out` and `report` are written only on
// Ok (a failed import leaves both untouched). Either may be null.
ImportResult import_sav(const std::string& path, Content* out, ImportReport* report);

// Same from a file image already in memory.
ImportResult import_sav_bytes(const uint8_t* sav, size_t size, Content* out, ImportReport* report);

const char* importResultName(ImportResult r);

}  // namespace mpcboy
