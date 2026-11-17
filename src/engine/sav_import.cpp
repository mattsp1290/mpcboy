#include "sav_import.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "rackboy_sav_compression.h"
#include "rackboy_sav_header.h"
#include "rackboy_sav_tables.h"
#include "rackboy_sav_types.h"
#include "rackboy_sav_waves.h"

namespace mpcboy {

namespace {

bool isSupportedCommand(uint8_t c)
{
    switch (c)
    {
        case 'D': case 'E': case 'H': case 'K': case 'L': case 'N':
        case 'P': case 'R': case 'S': case 'V': case 'W':
            return true;
        default:
            return false;
    }
}

}  // namespace

const char* importResultName(ImportResult r)
{
    switch (r)
    {
        case ImportResult::Ok: return "ok";
        case ImportResult::ReadFailed: return "cannot read the file";
        case ImportResult::BadSize: return "not a 128 KiB .sav";
        case ImportResult::NotSav: return "not an LSDJ .sav";
        case ImportResult::WrongVersion: return "not LSDJ 9.4.0";
        case ImportResult::SlotNotZero: return "active project is not slot 0";
        case ImportResult::NoBlocks: return "project slot 0 has no data";
        case ImportResult::DecompressFailed: return "damaged data";
        case ImportResult::ParseFailed: return "tables or waves unreadable";
    }
    return "unknown";
}

ImportResult import_sav_bytes(const uint8_t* sav, size_t size, Content* out, ImportReport* report)
{
    using namespace rackboy::sav;
    if (size != kSavFileSize) return ImportResult::BadSize;
    if (sav == nullptr) return ImportResult::ReadFailed;

    switch (validate_header(sav, size))
    {
        case HeaderError::Ok: break;
        case HeaderError::VersionMismatch: return ImportResult::WrongVersion;
        case HeaderError::ActiveSlotNotZero: return ImportResult::SlotNotZero;
        case HeaderError::SizeMismatch: return ImportResult::BadSize;
        default: return ImportResult::NotSav;
    }

    const size_t block = first_block_for_slot(sav, 0);
    if (block >= kBlockAllocTableLen) return ImportResult::NoBlocks;

    std::string err;
    const auto song = compression::decompress(sav, size, static_cast<uint8_t>(block), &err);
    if (!song || song->size() < kDecompressedSongSizeBytes) return ImportResult::DecompressFailed;

    // RackboyBank is ~12 KB: keep it off the worker's stack.
    auto bank = std::make_unique<RackboyBank>();
    bank_reset(*bank);
    if (parse_tables(song->data(), song->size(), bank.get()) != TablesError::Ok) return ImportResult::ParseFailed;
    if (parse_waves(song->data(), song->size(), bank.get()) != WavesError::Ok) return ImportResult::ParseFailed;

    Content c;
    clearContent(c);
    ImportReport rep;
    int slot = 0;
    for (size_t t = 0; t < kTotalTableSlots; ++t)
    {
        if (!bank->tables[t].occupied) continue;
        ++rep.tablesAllocated;
        if (slot >= kNumTables)
        {
            ++rep.tablesDropped;
            continue;
        }
        c.tables.tables[slot] = bank->tables[t];
        c.tables.tables[slot].occupied = true;
        ++slot;
        ++rep.tablesImported;
        for (const RackboyTableRow& row : bank->tables[t].rows)
        {
            const uint8_t cmds[2] = {row.cmd1, row.cmd2};
            const uint8_t vals[2] = {row.cmd1_param, row.cmd2_param};
            for (int k = 0; k < 2; ++k)
            {
                if (cmds[k] == 0) continue;
                if (!isSupportedCommand(cmds[k])) ++rep.unsupportedCommands;
                else if (cmds[k] == 'W' && vals[k] >= kNumWaves) ++rep.wavesBeyond16;
            }
        }
    }
    for (int w = 0; w < kNumWaves; ++w)
    {
        if (!bank->waves[w].occupied) continue;
        std::memcpy(c.waves.waves[w], bank->waves[w].samples, kWaveBytes);
        ++rep.wavesImported;
    }

    if (out) *out = c;
    if (report) *report = rep;
    return ImportResult::Ok;
}

ImportResult import_sav(const std::string& path, Content* out, ImportReport* report)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return ImportResult::ReadFailed;
    // Size first: never slurp an arbitrary large file.
    std::vector<uint8_t> buf(kSavFileSize + 1);
    const size_t n = std::fread(buf.data(), 1, buf.size(), f);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) return ImportResult::ReadFailed;
    if (n != kSavFileSize) return ImportResult::BadSize;
    return import_sav_bytes(buf.data(), n, out, report);
}

}  // namespace mpcboy
