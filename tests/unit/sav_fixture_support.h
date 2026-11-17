// Shared fixtures for the .sav import tests (sav_import_test.cpp,
// import_undo_test.cpp, import_plugin_test.cpp): a temporary plugin folder,
// builders for .sav variants derived from the rackboy fixture, and small
// content helpers. Everything is inline; including this header sets
// MPC_PLUGIN_FOLDER from a static initialiser (before any plugin is built).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "doctest/doctest.h"
#include "common/fixture_loader.h"
#include "content_store.h"
#include "factory_content.h"
#include "rackboy_sav_compression.h"
#include "rackboy_sav_header.h"
#include "rackboy_sav_tables.h"
#include "rackboy_sav_validate.h"
#include "rackboy_sav_waves.h"
#include "sav_import.h"
#include "state_codec.h"

namespace mpcboy_test {

using namespace mpcboy;
namespace fs = std::filesystem;
namespace sav = rackboy::sav;

// ---- temporary plugin folder -----------------------------------------------------

struct TempFolder
{
    std::string root;
    std::string importDir;
    TempFolder()
    {
        std::string base = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp";
        if (!base.empty() && base.back() == '/') base.pop_back();
        std::string tmpl = base + "/mpcboy_import_test_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data()) == nullptr) std::abort();
        root = buf.data();
        importDir = root + "/Content/Import";
        fs::create_directories(importDir);
        std::ofstream(root + "/plugin.json") << "{}\n";
        // mpc::pluginFolder() caches its first answer, so this must happen before
        // any plugin is constructed (a static initialiser runs before main()).
        setenv("MPC_PLUGIN_FOLDER", root.c_str(), 1);
    }
    ~TempFolder()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};
inline TempFolder g_folder;

inline void clearImportDir()
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(g_folder.importDir, ec)) fs::remove_all(e.path(), ec);
}

inline void writeFile(const std::string& dir, const std::string& name, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(dir + "/" + name, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// ---- building .sav variants from the fixture -----------------------------------

inline std::vector<uint8_t> fixtureSav() { return rackboy::tests::common::load_fixture_bytes(); }

inline std::vector<uint8_t> fixtureSong(const std::vector<uint8_t>& savBytes)
{
    const size_t block = sav::first_block_for_slot(savBytes.data(), 0);
    REQUIRE(block < sav::kBlockAllocTableLen);
    std::string err;
    auto song = sav::compression::decompress(savBytes.data(), savBytes.size(), static_cast<uint8_t>(block), &err);
    REQUIRE(song.has_value());
    REQUIRE(song->size() == sav::compression::kSongSize);
    return *song;
}

// LSDJ block-chain encoder: RLE runs of >= 4, E0 xx block switches, E0 FF end.
// Only the writer of test files; the importer uses the vendored decompressor.
inline std::vector<uint8_t> savFromSong(const std::vector<uint8_t>& song)
{
    std::vector<uint8_t> out = fixtureSav();
    std::fill(out.begin() + static_cast<long>(sav::compression::kBlocksOffset), out.end(), uint8_t{0});
    std::fill(out.begin() + static_cast<long>(sav::kBlockAllocTableOffset),
              out.begin() + static_cast<long>(sav::kBlockAllocTableOffset + sav::kBlockAllocTableLen), uint8_t{0xFF});

    std::vector<std::vector<uint8_t>> toks;
    for (size_t i = 0; i < song.size();)
    {
        const uint8_t b = song[i];
        size_t run = 1;
        while (i + run < song.size() && song[i + run] == b && run < 255) ++run;
        if (run >= 4 && b != 0xC0) { toks.push_back({0xC0, b, static_cast<uint8_t>(run)}); i += run; }
        else if (b == 0xC0) { toks.push_back({0xC0, 0xC0}); ++i; }
        else if (b == 0xE0) { toks.push_back({0xE0, 0xE0}); ++i; }
        else { toks.push_back({b}); ++i; }
    }
    const size_t B = sav::compression::kBlockSize;
    size_t block = 0, pos = 0;
    auto at = [&](size_t blk, size_t p) -> uint8_t& { return out[sav::compression::kBlocksOffset + blk * B + p]; };
    out[sav::kBlockAllocTableOffset + 0] = 0;
    for (const auto& t : toks)
    {
        if (pos + t.size() + 2 > B)
        {
            at(block, pos) = 0xE0;
            at(block, pos + 1) = static_cast<uint8_t>(block + 1);
            ++block;
            REQUIRE(block < sav::kBlockAllocTableLen);
            out[sav::kBlockAllocTableOffset + block] = 0;
            pos = 0;
        }
        for (uint8_t v : t) at(block, pos++) = v;
    }
    at(block, pos) = 0xE0;
    at(block, pos + 1) = 0xFF;
    return out;
}

// A song with 24 allocated tables (slots with t % 4 != 3), recognisable content
// in every row, kit-free instruments, and waves 0..20.
struct TableSong
{
    std::vector<uint8_t> song;
    std::vector<int> allocated;   // file table slots in allocation order
    int expectUnsupported = 0, expectWavesBeyond = 0, expectWavesImported = 0;
};

inline TableSong makeTableSong(const std::vector<uint8_t>& base, uint8_t seed)
{
    TableSong ts;
    ts.song = base;
    std::vector<uint8_t>& s = ts.song;
    const char letters[] = {'D', 'E', 'H', 'K', 'L', 'N', 'P', 'R', 'S', 'V', 'W', 'C', 'T', 'A', 'X', 0};
    for (int t = 0; t < 32; ++t)
    {
        const bool alloc = (t % 4) != 3;
        s[sav::kTableAllocMapOffset + static_cast<size_t>(t)] = alloc ? 1 : 0;
        if (alloc) ts.allocated.push_back(t);
        for (int r = 0; r < 16; ++r)
        {
            const size_t idx = static_cast<size_t>(t * 16 + r);
            const uint8_t h = static_cast<uint8_t>(t * 37 + r * 11 + seed);
            s[sav::kTableEnvelopeOffset + idx] = (r % 4 == 0) ? 0x00 : static_cast<uint8_t>(h | 1);
            s[sav::kTableTransposeOffset + idx] = static_cast<uint8_t>(h ^ 0x5A);
            s[sav::kTableCmd1Offset + idx] = static_cast<uint8_t>(letters[(t + r) % 16]);
            s[sav::kTableCmd1ValOffset + idx] = h;
            s[sav::kTableCmd2Offset + idx] = (r % 3 == 0) ? 'W' : 0;
            s[sav::kTableCmd2ValOffset + idx] = static_cast<uint8_t>((r * 3) % 40);
        }
    }
    for (int w = 0; w < 21; ++w)
        for (int j = 0; j < 16; ++j)
            s[0x6000 + static_cast<size_t>(w) * 16 + static_cast<size_t>(j)] =
                (w < 16 && w % 5 == 4) ? 0 : static_cast<uint8_t>(w * 16 + j + 1 + seed);
    // Expected report, counted independently from the song bytes.
    for (int k = 0; k < 16; ++k)
    {
        const int t = ts.allocated[static_cast<size_t>(k)];
        for (int r = 0; r < 16; ++r)
        {
            const size_t idx = static_cast<size_t>(t * 16 + r);
            const uint8_t c1 = s[sav::kTableCmd1Offset + idx], v1 = s[sav::kTableCmd1ValOffset + idx];
            const uint8_t c2 = s[sav::kTableCmd2Offset + idx], v2 = s[sav::kTableCmd2ValOffset + idx];
            const uint8_t cs[2] = {c1, c2}, vs[2] = {v1, v2};
            for (int q = 0; q < 2; ++q)
            {
                if (cs[q] == 0) continue;
                if (!isSupportedCommand(cs[q])) ++ts.expectUnsupported;
                else if (cs[q] == 'W' && vs[q] >= 16) ++ts.expectWavesBeyond;
            }
        }
    }
    for (int w = 0; w < 16; ++w) ts.expectWavesImported += (w % 5 == 4) ? 0 : 1;
    return ts;
}

inline std::vector<uint8_t> encoded(const Content& c, const std::string& name = std::string())
{
    std::vector<uint8_t> v;
    encodeState(c, v, name);
    return v;
}

inline Content imported(const std::vector<uint8_t>& savBytes, ImportReport* rep = nullptr)
{
    Content c;
    REQUIRE(import_sav_bytes(savBytes.data(), savBytes.size(), &c, rep) == ImportResult::Ok);
    return c;
}

inline Content importedFromFile(const std::string& path)
{
    Content c;
    REQUIRE(import_sav(path, &c, nullptr) == ImportResult::Ok);
    return c;
}

inline Content sentinelContent()
{
    Content c;
    loadFactoryContent(c);
    c.tables.tables[15].rows[15].transpose = 41;
    return c;
}

}  // namespace mpcboy_test
