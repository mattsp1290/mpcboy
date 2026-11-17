// WP11 tests 1-4 (06-lsdj-sav-import.md): the LSDJ .sav import of tables and
// waves (parsing, volume column, kits, rejected files).
//
// Test 4's "under ASan/UBSan" is covered by the native-ubsan preset; the ASan and
// TSan runs are tracked as pending host gates in docs/DEVICE_TESTS.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "doctest/doctest.h"
#include "MpcBoyPlugin.h"
#include "common/fixture_loader.h"
#include "content_store.h"
#include "factory_content.h"
#include "import_worker.h"
#include "plugin_test_support.h"
#include "rackboy_sav_compression.h"
#include "rackboy_sav_header.h"
#include "rackboy_sav_tables.h"
#include "rackboy_sav_validate.h"
#include "rackboy_sav_waves.h"
#include "sav_fixture_support.h"
#include "sav_import.h"
#include "state_codec.h"

using namespace mpcboy;
using namespace mpcboy_test;
namespace fs = std::filesystem;
namespace sav = rackboy::sav;

// ============================== test 1 ==========================================

TEST_CASE("import 1: the fixture imports with exactly parse_tables / parse_waves output (no tables, default waves)")
{
    const std::vector<uint8_t> fx = fixtureSav();
    const std::vector<uint8_t> song = fixtureSong(fx);
    auto bank = std::make_unique<sav::RackboyBank>();
    sav::bank_reset(*bank);
    REQUIRE(sav::parse_tables(song.data(), song.size(), bank.get()) == sav::TablesError::Ok);
    REQUIRE(sav::parse_waves(song.data(), song.size(), bank.get()) == sav::WavesError::Ok);

    ImportReport rep;
    const Content c = imported(fx, &rep);
    CHECK(rep.tablesAllocated == 0);
    CHECK(rep.tablesImported == 0);
    CHECK(rep.tablesDropped == 0);
    for (int t = 0; t < kNumTables; ++t)
        for (int r = 0; r < 16; ++r)
        {
            const auto& row = c.tables.tables[t].rows[r];
            CHECK(row.transpose == 0);
            CHECK(row.cmd1 == 0);
            CHECK(row.cmd2 == 0);
            CHECK_FALSE(row.has_vol);
        }
    int occupied = 0;
    for (int w = 0; w < kNumWaves; ++w)
    {
        if (bank->waves[w].occupied) ++occupied;
        CHECK(std::memcmp(c.waves.waves[w], bank->waves[w].samples, kWaveBytes) == 0);
    }
    CHECK(rep.wavesImported == occupied);
    CHECK(occupied > 0);   // the default wave fills every slot in the fixture
}

TEST_CASE("import 1: a song with 24 allocated tables maps the first 16 in allocation order; columns and waves equal the parsers' output")
{
    const std::vector<uint8_t> fx = fixtureSav();
    const TableSong ts = makeTableSong(fixtureSong(fx), 7);
    const std::vector<uint8_t> file = savFromSong(ts.song);

    // Sanity: the writer's encoding round-trips through the vendored decompressor.
    CHECK(fixtureSong(file) == ts.song);

    auto bank = std::make_unique<sav::RackboyBank>();
    sav::bank_reset(*bank);
    REQUIRE(sav::parse_tables(ts.song.data(), ts.song.size(), bank.get()) == sav::TablesError::Ok);
    REQUIRE(sav::parse_waves(ts.song.data(), ts.song.size(), bank.get()) == sav::WavesError::Ok);

    ImportReport rep;
    const Content c = imported(file, &rep);
    CHECK(rep.tablesAllocated == 24);
    CHECK(rep.tablesImported == 16);
    CHECK(rep.tablesDropped == 8);
    CHECK(rep.wavesImported == ts.expectWavesImported);
    CHECK(rep.wavesImported == 13);
    CHECK(rep.unsupportedCommands == ts.expectUnsupported);
    CHECK(rep.unsupportedCommands > 0);
    CHECK(rep.wavesBeyond16 == ts.expectWavesBeyond);
    CHECK(rep.wavesBeyond16 > 0);

    for (int k = 0; k < 16; ++k)
    {
        const int t = ts.allocated[static_cast<size_t>(k)];
        CAPTURE(k);
        CAPTURE(t);
        for (int r = 0; r < 16; ++r)
        {
            const auto& got = c.tables.tables[k].rows[r];
            const auto& ref = bank->tables[t].rows[r];
            CHECK(got.transpose == ref.transpose);
            CHECK(got.cmd1 == ref.cmd1);
            CHECK(got.cmd1_param == ref.cmd1_param);
            CHECK(got.cmd2 == ref.cmd2);
            CHECK(got.cmd2_param == ref.cmd2_param);
            // And straight from the file bytes.
            const size_t idx = static_cast<size_t>(t * 16 + r);
            CHECK(static_cast<uint8_t>(got.transpose) == ts.song[sav::kTableTransposeOffset + idx]);
            CHECK(got.cmd1 == ts.song[sav::kTableCmd1Offset + idx]);
            CHECK(got.cmd2_param == ts.song[sav::kTableCmd2ValOffset + idx]);
        }
    }
    for (int w = 0; w < kNumWaves; ++w)
    {
        CAPTURE(w);
        if (bank->waves[w].occupied) CHECK(std::memcmp(c.waves.waves[w], bank->waves[w].samples, kWaveBytes) == 0);
        else for (int j = 0; j < kWaveBytes; ++j) CHECK(c.waves.waves[w][j] == 0);
    }

    // Fewer than 16 allocated: the rest are cleared.
    TableSong few = makeTableSong(fixtureSong(fx), 9);
    for (int t = 0; t < 32; ++t) few.song[sav::kTableAllocMapOffset + static_cast<size_t>(t)] = (t == 5 || t == 20) ? 1 : 0;
    const Content c2 = imported(savFromSong(few.song));
    for (int r = 0; r < 16; ++r)
    {
        CHECK(static_cast<uint8_t>(c2.tables.tables[0].rows[r].transpose) ==
              few.song[sav::kTableTransposeOffset + static_cast<size_t>(5 * 16 + r)]);
        CHECK(static_cast<uint8_t>(c2.tables.tables[1].rows[r].transpose) ==
              few.song[sav::kTableTransposeOffset + static_cast<size_t>(20 * 16 + r)]);
        for (int k = 2; k < 16; ++k)
        {
            CHECK(c2.tables.tables[k].rows[r].transpose == 0);
            CHECK(c2.tables.tables[k].rows[r].cmd1 == 0);
            CHECK_FALSE(c2.tables.tables[k].rows[r].has_vol);
        }
    }
}

// ============================== test 2 ==========================================

TEST_CASE("import 2: volume columns equal the bytes at 0x1690; 0x00 imports as has_vol = false")
{
    const std::vector<uint8_t> fx = fixtureSav();
    const TableSong ts = makeTableSong(fixtureSong(fx), 3);
    TableSong v = ts;
    // Hand-pick the interesting bytes in the first imported table (file slot 0).
    v.song[sav::kTableEnvelopeOffset + 0] = 0x00;
    v.song[sav::kTableEnvelopeOffset + 1] = 0x01;
    v.song[sav::kTableEnvelopeOffset + 2] = 0x0F;
    v.song[sav::kTableEnvelopeOffset + 3] = 0x80;
    v.song[sav::kTableEnvelopeOffset + 4] = 0xFF;
    const Content c = imported(savFromSong(v.song));
    CHECK_FALSE(c.tables.tables[0].rows[0].has_vol);
    CHECK(c.tables.tables[0].rows[0].vol == 0x00);
    CHECK(c.tables.tables[0].rows[1].has_vol);
    CHECK(c.tables.tables[0].rows[1].vol == 0x01);
    CHECK(c.tables.tables[0].rows[2].vol == 0x0F);
    CHECK(c.tables.tables[0].rows[3].vol == 0x80);
    CHECK(c.tables.tables[0].rows[4].vol == 0xFF);
    int zeros = 0;
    for (int k = 0; k < 16; ++k)
    {
        const int t = ts.allocated[static_cast<size_t>(k)];
        for (int r = 0; r < 16; ++r)
        {
            const uint8_t b = v.song[sav::kTableEnvelopeOffset + static_cast<size_t>(t * 16 + r)];
            const auto& row = c.tables.tables[k].rows[r];
            CHECK(row.vol == b);
            CHECK(row.has_vol == (b != 0x00));
            if (b == 0) ++zeros;
        }
    }
    CHECK(zeros >= 16 * 4 - 4);   // every fourth row of makeTableSong is empty (minus the rows set by hand)
}

// ============================== test 3 ==========================================

TEST_CASE("import 3: a kit instrument (or an unknown instrument type) does not stop tables and waves from importing")
{
    const std::vector<uint8_t> fx = fixtureSav();
    const TableSong ts = makeTableSong(fixtureSong(fx), 5);
    const std::vector<uint8_t> baseline = savFromSong(ts.song);

    TableSong kit = ts;   // instrument 5: allocated, type byte 0x02 = kit
    kit.song[sav::kInstrumentAllocMapOffset + 5] = sav::kInstrumentAllocated;
    kit.song[sav::kInstrumentParamsOffset + 5 * sav::kInstrumentSlotStride] = sav::kInstrumentTypeKit;
    const std::vector<uint8_t> kitSav = savFromSong(kit.song);
    CHECK(encoded(imported(kitSav)) == encoded(imported(baseline)));

    TableSong bad = ts;   // instrument 6: allocated with a type byte load_rackboy_sav rejects
    bad.song[sav::kInstrumentAllocMapOffset + 6] = sav::kInstrumentAllocated;
    bad.song[sav::kInstrumentParamsOffset + 6 * sav::kInstrumentSlotStride] = 0x7F;
    const std::vector<uint8_t> badSav = savFromSong(bad.song);
    {
        auto bank = std::make_unique<sav::RackboyBank>();
        auto names = std::make_unique<sav::RackboyBankNames>();
        const std::vector<uint8_t> song = fixtureSong(badSav);
        sav::LoadInputs in{badSav.data(), badSav.size(), song.data(), song.size(), nullptr, 0};
        CHECK(sav::load_rackboy_sav(in, bank.get(), names.get()).phase != sav::LoadPhase::Ok);   // the full loader fails ...
    }
    CHECK(encoded(imported(badSav)) == encoded(imported(baseline)));   // ... the import does not
}

// ============================== test 4 ==========================================

TEST_CASE("import 4: truncated, wrong-version, wrong-magic, damaged-chain and random files are rejected without changing the output")
{
    const std::vector<uint8_t> fx = fixtureSav();
    const std::vector<uint8_t> good = savFromSong(makeTableSong(fixtureSong(fx), 1).song);

    const Content keep = sentinelContent();
    const std::vector<uint8_t> before = encoded(keep);
    ImportReport repKeep;
    repKeep.tablesImported = 99;

    auto rejects = [&](const std::vector<uint8_t>& bytes, ImportResult expect) {
        Content out = keep;
        ImportReport rep = repKeep;
        const ImportResult r = import_sav_bytes(bytes.data(), bytes.size(), &out, &rep);
        CAPTURE(static_cast<int>(r));
        return r == expect && encoded(out) == before && rep.tablesImported == 99;
    };

    CHECK(rejects(std::vector<uint8_t>(good.begin(), good.end() - 1), ImportResult::BadSize));   // truncated by 1
    CHECK(rejects(std::vector<uint8_t>(good.begin(), good.begin() + 1000), ImportResult::BadSize));
    CHECK(rejects(std::vector<uint8_t>(), ImportResult::BadSize));
    {
        std::vector<uint8_t> big = good;
        big.push_back(0);
        CHECK(rejects(big, ImportResult::BadSize));   // one byte too long
    }
    {
        std::vector<uint8_t> b = good;
        b[0x8100] = 0x1A;   // wrong LSDJ version
        CHECK(rejects(b, ImportResult::WrongVersion));
        b[0x8100] = 0x1C;
        CHECK(rejects(b, ImportResult::WrongVersion));
    }
    {
        std::vector<uint8_t> b = good;
        b[0x813E] = 'x';    // magic
        CHECK(rejects(b, ImportResult::NotSav));
    }
    {
        std::vector<uint8_t> b = good;
        b[0x8140] = 1;   // active project slot is not 0
        CHECK(rejects(b, ImportResult::SlotNotZero));
    }
    {
        std::vector<uint8_t> b = good;
        std::fill(b.begin() + static_cast<long>(sav::kBlockAllocTableOffset),
                  b.begin() + static_cast<long>(sav::kBlockAllocTableOffset + sav::kBlockAllocTableLen), uint8_t{0xFF});
        CHECK(rejects(b, ImportResult::NoBlocks));
    }
    {
        std::vector<uint8_t> b = good;   // block 0 switches to itself without output: cyclic chain
        const size_t o = sav::compression::kBlocksOffset;
        b[o] = 0xE0;
        b[o + 1] = 0x00;
        CHECK(rejects(b, ImportResult::DecompressFailed));
        b[o + 1] = 0xFE;   // switch to a block beyond the file
        CHECK(rejects(b, ImportResult::DecompressFailed));
    }
    std::mt19937 rng(2024);
    for (int i = 0; i < 25; ++i)
    {
        std::vector<uint8_t> rnd(131072);
        for (auto& x : rnd) x = static_cast<uint8_t>(rng());
        CHECK(rejects(rnd, ImportResult::NotSav));   // random 128 KB: the magic is not "jk"
    }
    // Random bytes behind a valid header: whatever happens, the output is untouched or fully valid.
    for (int i = 0; i < 25; ++i)
    {
        std::vector<uint8_t> b = good;
        for (size_t k = sav::compression::kBlocksOffset; k < b.size(); ++k) b[k] = static_cast<uint8_t>(rng());
        Content out = keep;
        const ImportResult r = import_sav_bytes(b.data(), b.size(), &out, nullptr);
        if (r != ImportResult::Ok) CHECK(encoded(out) == before);
    }
    // The path form: a missing file and a directory.
    Content out = keep;
    CHECK(import_sav(g_folder.root + "/nope.sav", &out, nullptr) == ImportResult::ReadFailed);
    CHECK(import_sav(g_folder.root, &out, nullptr) != ImportResult::Ok);
    CHECK(encoded(out) == before);
    CHECK(std::string(importResultName(ImportResult::WrongVersion)).size() > 0);
}
