// WP11 tests 1-8 (06-lsdj-sav-import.md): the LSDJ .sav import of tables and waves,
// its undo, the trigger debounce and the worker scan. File-based tests use a
// temporary plugin folder through MPC_PLUGIN_FOLDER (set before any test runs).
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
#include "rackboy_sav_compression.h"
#include "rackboy_sav_header.h"
#include "rackboy_sav_tables.h"
#include "rackboy_sav_validate.h"
#include "rackboy_sav_waves.h"
#include "sav_import.h"
#include "state_codec.h"

using namespace mpcboy;
namespace fs = std::filesystem;
namespace sav = rackboy::sav;

namespace {

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
TempFolder g_folder;

void clearImportDir()
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(g_folder.importDir, ec)) fs::remove_all(e.path(), ec);
}

void writeFile(const std::string& dir, const std::string& name, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(dir + "/" + name, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

template <typename Fn>
bool waitFor(Fn&& fn, int timeoutMs = 3000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end)
    {
        if (fn()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fn();
}

// ---- building .sav variants from the fixture -----------------------------------

std::vector<uint8_t> fixtureSav() { return rackboy::tests::common::load_fixture_bytes(); }

std::vector<uint8_t> fixtureSong(const std::vector<uint8_t>& savBytes)
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
std::vector<uint8_t> savFromSong(const std::vector<uint8_t>& song)
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

bool isSupportedCmd(uint8_t c)
{
    return c == 'D' || c == 'E' || c == 'H' || c == 'K' || c == 'L' || c == 'N' || c == 'P' || c == 'R' || c == 'S' ||
           c == 'V' || c == 'W';
}

// A song with 24 allocated tables (slots with t % 4 != 3), recognisable content
// in every row, kit-free instruments, and waves 0..20.
struct TableSong
{
    std::vector<uint8_t> song;
    std::vector<int> allocated;   // file table slots in allocation order
    int expectUnsupported = 0, expectWavesBeyond = 0, expectWavesImported = 0;
};

TableSong makeTableSong(const std::vector<uint8_t>& base, uint8_t seed)
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
                if (!isSupportedCmd(cs[q])) ++ts.expectUnsupported;
                else if (cs[q] == 'W' && vs[q] >= 16) ++ts.expectWavesBeyond;
            }
        }
    }
    for (int w = 0; w < 16; ++w) ts.expectWavesImported += (w % 5 == 4) ? 0 : 1;
    return ts;
}

std::vector<uint8_t> encoded(const Content& c, const std::string& name = std::string())
{
    std::vector<uint8_t> v;
    encodeState(c, v, name);
    return v;
}

Content imported(const std::vector<uint8_t>& savBytes, ImportReport* rep = nullptr)
{
    Content c;
    REQUIRE(import_sav_bytes(savBytes.data(), savBytes.size(), &c, rep) == ImportResult::Ok);
    return c;
}

Content importedFromFile(const std::string& path)
{
    Content c;
    REQUIRE(import_sav(path, &c, nullptr) == ImportResult::Ok);
    return c;
}

Content sentinelContent()
{
    Content c;
    loadFactoryContent(c);
    c.tables.tables[15].rows[15].transpose = 41;
    return c;
}

// ---- plugin rig ---------------------------------------------------------------

// Fake time source for the trigger debounce: tests advance it explicitly instead of sleeping.
long long g_fakeNs = 1'000'000'000LL;
long long fakeNow() { return g_fakeNs; }
void advanceMs(int ms) { g_fakeNs += static_cast<long long>(ms) * 1'000'000LL; }

struct Rig
{
    std::unique_ptr<MpcBoyPlugin> p;
    std::vector<float> L, R;
    Rig()
    {
        p = std::make_unique<MpcBoyPlugin>();
        p->setClockForTest(&fakeNow);
        p->prepare(44100.0, 512);
    }
    void block(int n = 128, const std::vector<mpc::MidiEvent>& midi = {})
    {
        const size_t old = L.size();
        L.resize(old + static_cast<size_t>(n));
        R.resize(old + static_cast<size_t>(n));
        float* outs[2] = {L.data() + old, R.data() + old};
        mpc::ProcessContext ctx;
        ctx.outputs = outs;
        ctx.numOutputs = 2;
        ctx.numFrames = n;
        ctx.midi = midi.empty() ? nullptr : midi.data();
        ctx.numMidiEvents = static_cast<int>(midi.size());
        ctx.sampleRate = 44100.0;
        ctx.transport.valid = true;
        ctx.transport.bpm = 120.0;
        ctx.transport.tempoValid = true;
        p->process(ctx);
    }
    std::vector<uint8_t> blob() const
    {
        std::vector<uint8_t> v;
        p->saveExtraState(v);
        return v;
    }
    // Rescan and wait until the worker has listed the files.
    void rescan()
    {
        const uint32_t before = p->importer().scansDone();
        p->importer().requestScan();
        REQUIRE(waitFor([&] { return p->importer().scansDone() > before; }));
    }
};

}  // namespace

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

// ============================== test 5 ==========================================

TEST_CASE("import 5: ContentStore undo restores the previous content exactly; a second undo re-applies the import")
{
    auto store = std::make_unique<ContentStore>();
    store->markAudioThread(true);
    Content factory;
    loadFactoryContent(factory);
    store->setLive(factory);
    const std::vector<uint8_t> fx = fixtureSav();
    const Content a = imported(savFromSong(makeTableSong(fixtureSong(fx), 2).song));
    const Content b = imported(savFromSong(makeTableSong(fixtureSong(fx), 8).song));

    store->requestUndo();   // nothing to undo yet: ignored
    store->beginBlock();
    CHECK_FALSE(store->undoAvailable());
    CHECK(encoded(store->live()) == encoded(factory));

    store->stage(a);   // a plain (state-load) stage never creates an undo copy
    store->beginBlock();
    CHECK_FALSE(store->undoAvailable());
    CHECK(encoded(store->live()) == encoded(a));

    store->setLive(factory);
    const uint32_t gen0 = store->bulkGeneration();
    store->stageImport(a);
    store->beginBlock();
    CHECK(store->undoAvailable());
    CHECK(encoded(store->live()) == encoded(a));
    CHECK(store->bulkGeneration() == gen0 + 1);

    // Live edits after the import are lost by undo (the undo copy is from before the import).
    store->editLive([](Content& c) { c.tables.tables[3].rows[2].transpose = 55; });
    store->requestUndo();
    store->beginBlock();
    CHECK(encoded(store->live()) == encoded(factory));
    Content pub;
    store->readPublished(pub);
    CHECK(encoded(pub) == encoded(factory));
    CHECK(store->bulkGeneration() == gen0 + 2);

    // Undo is a swap: a second undo brings back exactly what the first one replaced
    // (the import together with the edit made after it).
    Content aEdited = a;
    aEdited.tables.tables[3].rows[2].transpose = 55;
    store->requestUndo();
    store->beginBlock();
    CHECK(encoded(store->live()) == encoded(aEdited));

    store->stageImport(b);   // a new import: undo now returns to the content just before it
    store->beginBlock();
    CHECK(encoded(store->live()) == encoded(b));
    store->requestUndo();
    store->beginBlock();
    CHECK(encoded(store->live()) == encoded(aEdited));

    // Bank view follows: table 0 of a carries the imported row data.
    CHECK(store->bank().tables[0].rows[1].cmd1 == a.tables.tables[0].rows[1].cmd1);
    store->markAudioThread(false);
}

TEST_CASE("import 5: a project load after an import ends the undo history")
{
    auto store = std::make_unique<ContentStore>();
    store->markAudioThread(true);
    Content factory;
    loadFactoryContent(factory);
    store->setLive(factory);
    const std::vector<uint8_t> fx = fixtureSav();
    const Content a = imported(savFromSong(makeTableSong(fixtureSong(fx), 2).song));
    Content other = factory;
    other.tables.tables[4].rows[3].transpose = 17;

    store->stageImport(a);
    store->beginBlock();
    REQUIRE(store->undoAvailable());

    store->stage(other);   // project / preset load
    store->beginBlock();
    CHECK(encoded(store->live()) == encoded(other));
    CHECK_FALSE(store->undoAvailable());

    store->requestUndo();
    store->beginBlock();
    CHECK(encoded(store->live()) == encoded(other));   // the other project's content survives
    CHECK_FALSE(store->undoAvailable());

    // An undo requested in the same block as a load is dropped too.
    store->stageImport(a);
    store->beginBlock();
    store->requestUndo();
    store->stage(other);
    store->beginBlock();
    CHECK(encoded(store->live()) == encoded(other));
    store->markAudioThread(false);
}

TEST_CASE("import 5: through the plugin, import then imp_undo restores the previous content and the editor cursor follows")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    const TableSong ts = makeTableSong(fixtureSong(fx), 4);
    writeFile(g_folder.importDir, "song.sav", savFromSong(ts.song));
    Rig r;
    r.block();
    r.rescan();
    const std::vector<uint8_t> before = r.blob();
    r.p->set(ED_Table, 1);
    r.p->set(ED_Row, 3);
    r.block();

    r.p->set(Imp_File, 1);
    r.p->set(Imp_Go, 1);
    r.block();   // process() turns the accepted edge into the import request
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 1; }));
    r.block();   // the audio thread consumes the staged import
    r.block();
    const std::vector<uint8_t> after = r.blob();
    CHECK(after != before);
    Content got;
    std::string name;
    REQUIRE(decodeState(after.data(), after.size(), got, &name));
    CHECK(name == "song.sav");
    const ImportReport rep = r.p->importer().lastReport();
    CHECK(rep.tablesImported == 16);
    // The cursor shows the imported cell: table 1 is file table 0.
    const auto& row = got.tables.tables[0].rows[3];
    CHECK(r.p->getInt(ED_Tsp) == std::clamp<int>(row.transpose, -48, 48));
    CHECK(r.p->getInt(ED_Val1) == row.cmd1_param);
    CHECK(r.p->getInt(Imp_Go) == 0);   // the trigger went back to 0

    advanceMs(300);   // clear of the debounce
    r.p->set(Imp_Undo, 1);
    r.block();
    r.block();
    CHECK(r.p->getInt(Imp_Undo) == 0);
    std::vector<uint8_t> undone = r.blob();
    Content u;
    std::string uname;
    REQUIRE(decodeState(undone.data(), undone.size(), u, &uname));
    Content b0;
    std::string n0;
    REQUIRE(decodeState(before.data(), before.size(), b0, &n0));
    CHECK(encoded(u) == encoded(b0));   // content exactly as before the import
    const auto& row0 = u.tables.tables[0].rows[3];
    CHECK(r.p->getInt(ED_Tsp) == std::clamp<int>(row0.transpose, -48, 48));
    clearImportDir();
}

// ============================== test 6 ==========================================

TEST_CASE("import 6: two imp_go edges 100 ms apart run one import; a later edge runs another; a second request during an import is ignored")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "a.sav", savFromSong(makeTableSong(fixtureSong(fx), 6).song));
    Rig r;
    r.block();
    r.rescan();
    REQUIRE(r.p->importer().fileCount() == 1);
    CHECK(r.p->importer().importsDone() == 0);

    r.p->set(Imp_Go, 1);
    r.block();
    CHECK(r.p->getInt(Imp_Go) == 0);
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 1; }));
    advanceMs(100);   // fake time: independent of how long the import took
    r.p->set(Imp_Go, 1);   // the second press of one tap
    r.block();
    CHECK(r.p->getInt(Imp_Go) == 0);   // it is reset like any edge ...
    CHECK_FALSE(r.p->importer().busy());         // ... but no request was made (an accepted one is busy or done)
    CHECK(r.p->importer().importsDone() == 1);   // ... so it did nothing
    CHECK(r.p->importer().importsFailed() == 0);

    advanceMs(300);
    r.p->set(Imp_Go, 1);   // a deliberate third press, well after 250 ms
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 2; }));

    // Holding the parameter high is not a second edge.
    advanceMs(300);
    r.p->set(Imp_Go, 1);
    r.p->set(Imp_Go, 1);
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 3; }));
    CHECK_FALSE(r.p->importer().busy());
    CHECK(r.p->importer().importsDone() == 3);

    // The worker itself ignores a request while one is pending or running.
    const uint32_t d0 = r.p->importer().importsDone();
    const bool first = r.p->importer().requestImport(0);
    const bool second = r.p->importer().requestImport(0);
    CHECK(first);
    CHECK_FALSE(second);
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == d0 + 1; }));
    CHECK_FALSE(r.p->importer().busy());
    CHECK(r.p->importer().importsDone() == d0 + 1);
    clearImportDir();
}

TEST_CASE("import 6: imp_rescan debounces the same way and picks up new files; imp_file text shows names, - when empty, ERR after a failure")
{
    clearImportDir();
    Rig r;
    r.block();
    r.rescan();
    CHECK(r.p->importer().fileCount() == 0);
    CHECK(r.p->displayText(Imp_File) == "-");

    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "zeta_long_name.sav", savFromSong(makeTableSong(fixtureSong(fx), 6).song));
    writeFile(g_folder.importDir, "Alpha.SAV", fx);
    writeFile(g_folder.importDir, "broken.sav", std::vector<uint8_t>(5000, 0x55));

    const uint32_t scans = r.p->importer().scansDone();
    r.p->set(Imp_Rescan, 1);
    r.block();
    CHECK(r.p->getInt(Imp_Rescan) == 0);
    REQUIRE(waitFor([&] { return r.p->importer().scansDone() > scans; }));
    REQUIRE(r.p->importer().fileCount() == 3);
    CHECK(r.p->importer().fileName(0) == "Alpha.SAV");   // sorted by name, case-insensitively
    CHECK(r.p->importer().fileName(1) == "broken.sav");
    CHECK(r.p->importer().fileName(2) == "zeta_long_name.sav");
    r.p->set(Imp_File, 1);
    CHECK(r.p->displayText(Imp_File) == "Alpha");
    r.p->set(Imp_File, 3);
    CHECK(r.p->displayText(Imp_File) == "zeta_lon");   // extension stripped, cut at 8 bytes
    r.p->set(Imp_File, 40);
    CHECK(r.p->displayText(Imp_File) == "-");

    // A failing import: nothing changes and the text shows ERR <code> until the next scan.
    const std::vector<uint8_t> before = r.blob();
    advanceMs(300);
    r.p->set(Imp_File, 2);
    r.p->set(Imp_Go, 1);
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().importsFailed() == 1; }));
    r.block();
    r.block();
    CHECK(r.blob() == before);
    CHECK(r.p->importer().lastErrorCode() == static_cast<int>(ImportResult::BadSize));
    CHECK(r.p->displayText(Imp_File) == "ERR 2");
    advanceMs(300);
    const uint32_t scans2 = r.p->importer().scansDone();
    r.p->set(Imp_Rescan, 1);
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().scansDone() > scans2; }));
    CHECK(r.p->displayText(Imp_File) == "broken");
    clearImportDir();
}

// ============================== worker scan =====================================

TEST_CASE("import: the scan lists only regular .sav files at depth 1: no symlinks, no dot-files, no subfolders, at most 64, sorted")
{
    const std::string dir = g_folder.root + "/scan_case";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir + "/sub");
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(dir, "b.sav", fx);
    writeFile(dir, "A.sav", fx);
    writeFile(dir, "c.txt", fx);
    writeFile(dir, ".hidden.sav", fx);
    writeFile(dir, "._b.sav", fx);
    writeFile(dir + "/sub", "deep.sav", fx);
    fs::create_directories(dir + "/folder.sav");              // a directory named like a save
    fs::create_symlink(dir + "/b.sav", dir + "/link.sav", ec);   // a symbolic link to a save
    fs::create_directory_symlink(dir + "/sub", dir + "/linkdir", ec);
    ContentStore store;
    {
        ImportWorker w(dir, store);
        REQUIRE(waitFor([&] { return w.scansDone() >= 1; }));
        REQUIRE(w.fileCount() == 2);
        CHECK(w.fileName(0) == "A.sav");
        CHECK(w.fileName(1) == "b.sav");
        CHECK(w.fileName(2) == "");
    }   // joined here
    for (int i = 0; i < 70; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "many_%02d.sav", i);
        writeFile(dir, n, fx);
    }
    {
        ImportWorker w(dir, store);
        REQUIRE(waitFor([&] { return w.scansDone() >= 1; }));
        CHECK(w.fileCount() == 64);
        CHECK(w.fileName(0) == "A.sav");
        // A symbolic link is never imported even when asked for by index.
        CHECK(w.displayText(0) == "A");
    }
    {
        ImportWorker missing(g_folder.root + "/no_such_folder", store);   // no Content/Import: an empty list
        REQUIRE(waitFor([&] { return missing.scansDone() >= 1; }));
        CHECK(missing.fileCount() == 0);
        CHECK(missing.displayText(0) == "-");
        CHECK(missing.requestImport(0));   // nothing at index 0: no change, no error
        REQUIRE(waitFor([&] { return !missing.busy(); }));
        CHECK(missing.importsDone() == 0);
        CHECK(missing.importsFailed() == 0);
    }
    fs::remove_all(dir, ec);
}

// ============================== test 7 ==========================================

TEST_CASE("import 7: the engine renders while imports and undos run on the worker; every snapshot is whole")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "a.sav", savFromSong(makeTableSong(fixtureSong(fx), 2).song));
    writeFile(g_folder.importDir, "b.sav", savFromSong(makeTableSong(fixtureSong(fx), 9).song));
    Rig r;
    r.block();
    r.rescan();
    REQUIRE(r.p->importer().fileCount() == 2);

    Content factory;
    loadFactoryContent(factory);
    const std::vector<uint8_t> okFactory = encoded(factory);
    const std::vector<uint8_t> okA = encoded(importedFromFile(g_folder.importDir + "/a.sav"));
    const std::vector<uint8_t> okB = encoded(importedFromFile(g_folder.importDir + "/b.sav"));

    std::atomic<bool> done{false};
    std::atomic<uint32_t> requested{0}, torn{0}, snaps{0};
    std::thread ui([&] {
        uint32_t n = 0;
        while (!done.load(std::memory_order_acquire))
        {
            if (r.p->importer().requestImport(static_cast<int>(n % 2))) requested.fetch_add(1, std::memory_order_relaxed);
            if (n % 3 == 0) r.p->content().requestUndo();
            std::vector<uint8_t> blob;
            r.p->saveExtraState(blob);
            Content c;
            if (!decodeState(blob.data(), blob.size(), c)) torn.fetch_add(1, std::memory_order_relaxed);
            else
            {
                const std::vector<uint8_t> e = encoded(c);
                if (e != okFactory && e != okA && e != okB) torn.fetch_add(1, std::memory_order_relaxed);
            }
            snaps.fetch_add(1, std::memory_order_relaxed);
            ++n;
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });

    // Notes and table edits on the "audio" thread while the worker imports.
    std::vector<mpc::MidiEvent> m;
    r.p->set(P1_Tbl, 2);
    r.p->set(WV_On, 1);
    r.p->set(WV_Tbl, 3);
    for (int i = 0; i < 6000; ++i)
    {
        m.clear();
        if (i % 40 == 0) m.push_back(mpc::MidiEvent::noteOn(0, 48 + (i / 40) % 24, 100, 0));
        if (i % 40 == 30) m.push_back(mpc::MidiEvent::noteOff(0, 48 + (i / 40) % 24, 0));
        r.block(128, m);
        if (i % 500 == 499) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    done.store(true, std::memory_order_release);
    ui.join();
    std::printf("[import 7] worker requested %u imports; second thread took %u snapshots; main thread rendered 6,000 blocks\n",
                requested.load(), snaps.load());
    CHECK(torn.load() == 0);
    CHECK(requested.load() > 2);
    REQUIRE(waitFor([&] { return !r.p->importer().busy(); }));
    CHECK(r.p->importer().importsDone() == requested.load());   // every accepted request completed
    for (float v : r.L) REQUIRE(std::isfinite(v));
    clearImportDir();
}

// ============================== test 8 ==========================================

TEST_CASE("import 8: the imported file name rides in the state, comes back on load and selects the file in imp_file after a scan")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "alpha.sav", fx);
    writeFile(g_folder.importDir, "mine.sav", savFromSong(makeTableSong(fixtureSong(fx), 12).song));
    writeFile(g_folder.importDir, "zulu.sav", fx);

    std::vector<uint8_t> blob;
    {
        Rig r;
        r.block();
        r.rescan();
        r.p->set(Imp_File, 2);
        r.p->set(Imp_Go, 1);
        r.block();
        REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 1; }));
        r.block();
        r.block();
        blob = r.blob();
        Content c;
        std::string name;
        REQUIRE(decodeState(blob.data(), blob.size(), c, &name));
        CHECK(name == "mine.sav");
        CHECK(blob.size() == kStateSize + 8);
        CHECK(blob[4] == 2);
    }
    {
        Rig r2;
        r2.block();
        r2.rescan();
        r2.p->set(Imp_File, 3);   // some other selection saved in the project
        r2.p->loadExtraState(blob.data(), blob.size());
        r2.p->reset();
        r2.block();
        r2.block();
        CHECK(r2.p->importer().lastImportedName() == "mine.sav");
        CHECK(r2.blob() == blob);   // content, name and CRC round-trip byte for byte
        // The worker resolves the name against its list and the plugin selects it.
        CHECK(waitFor([&] { r2.block(); return r2.p->getInt(Imp_File) == 2; }));
        CHECK(r2.p->displayText(Imp_File) == "mine");
        // No import ran on load.
        CHECK(r2.p->importer().importsDone() == 0);
    }
    {
        // A project saved with a name that is no longer in the folder keeps the index it had.
        Rig r3;
        r3.block();
        std::filesystem::remove(g_folder.importDir + "/mine.sav");
        r3.rescan();
        r3.p->set(Imp_File, 3);
        r3.p->loadExtraState(blob.data(), blob.size());
        r3.rescan();
        for (int i = 0; i < 20; ++i) r3.block();
        CHECK(r3.p->getInt(Imp_File) == 3);
        CHECK(r3.p->importer().lastImportedName() == "mine.sav");
    }
    {
        // A v1 blob (no name field) is rejected: nothing changes.
        Rig r4;
        r4.block();
        const std::vector<uint8_t> keep = r4.blob();
        std::vector<uint8_t> v1(blob.begin(), blob.begin() + 2053);
        v1[4] = 1;
        v1.resize(2057);
        const uint32_t crc = crc32Ieee(v1.data(), 2053);
        for (int i = 0; i < 4; ++i) v1[2053 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
        r4.p->loadExtraState(v1.data(), v1.size());
        r4.block();
        r4.block();
        CHECK(r4.blob() == keep);
    }
    clearImportDir();
}

TEST_CASE("import: a trigger set to 1 off the process thread (as a project or preset load does) never starts an import")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    const TableSong ts = makeTableSong(fixtureSong(fx), 4);
    writeFile(g_folder.importDir, "song.sav", savFromSong(ts.song));
    Rig r;
    r.block();
    r.rescan();
    r.p->set(Imp_File, 1);

    std::thread loader([&] { r.p->set(Imp_Go, 1); r.p->set(Imp_Undo, 1); });
    loader.join();
    r.block();
    CHECK_FALSE(r.p->importer().busy());
    r.block();
    r.block();
    CHECK(r.p->importer().importsDone() == 0);
    CHECK(r.p->getInt(Imp_Go) == 0);     // still reset to 0 by process()
    CHECK(r.p->getInt(Imp_Undo) == 0);

    r.p->set(Imp_Go, 1);                 // a real tap, on the process thread, still works
    r.block();
    CHECK(waitFor([&] { return r.p->importer().importsDone() == 1; }));
}

TEST_CASE("import: a trigger restored as 1 by a state load on the process thread is cancelled by loadExtraState")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "song.sav", savFromSong(makeTableSong(fixtureSong(fx), 4).song));
    Rig r;
    r.block();
    r.rescan();
    r.p->set(Imp_File, 1);

    std::vector<uint8_t> blob;
    r.p->saveExtraState(blob);
    // The SDK writes a chunk's parameters first (here: the trigger arrives as an edge on the
    // audio-marked thread) and calls loadExtraState() afterwards.
    r.p->set(Imp_Go, 1);
    r.p->loadExtraState(blob.data(), blob.size());
    r.block();
    r.block();
    CHECK_FALSE(r.p->importer().busy());
    CHECK(r.p->importer().importsDone() == 0);
    CHECK(r.p->getInt(Imp_Go) == 0);   // still written back to 0
    clearImportDir();
}

TEST_CASE("import: the saved import name follows the live content (undo, a load that replaces a pending import)")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "song.sav", savFromSong(makeTableSong(fixtureSong(fx), 4).song));
    Rig r;
    r.block();
    r.rescan();
    r.p->set(Imp_File, 1);
    const std::vector<uint8_t> fresh = r.blob();   // no import yet

    r.p->set(Imp_Go, 1);
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 1; }));
    {
        // Between staging and the next audio block the staged import already names the saved state.
        Content c;
        std::string name;
        const std::vector<uint8_t> b = r.blob();
        REQUIRE(decodeState(b.data(), b.size(), c, &name));
        CHECK(name == "song.sav");
    }
    r.block();
    CHECK(r.p->importer().lastImportedName() == "song.sav");

    advanceMs(300);
    r.p->set(Imp_Undo, 1);
    r.block();
    r.block();
    CHECK(r.p->importer().lastImportedName() == "");   // the content is back to before the import

    advanceMs(300);
    r.p->set(Imp_Undo, 1);   // undo again re-applies the import
    r.block();
    r.block();
    CHECK(r.p->importer().lastImportedName() == "song.sav");

    // A project load ends the history: the name is the one stored in the loaded project.
    r.p->loadExtraState(fresh.data(), fresh.size());
    CHECK(r.p->importer().lastImportedName() == "");
    r.block();
    r.block();
    CHECK(r.p->importer().lastImportedName() == "");
    clearImportDir();
}
