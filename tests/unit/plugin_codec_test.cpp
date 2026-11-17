// WP7 tests 1 and 2: state_codec and param_bridge (no host, no threads).
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "param_bridge.h"
#include "state_codec.h"

using namespace mpcboy;

namespace {

Content randomContent(std::mt19937& rng)
{
    Content c;
    clearContent(c);
    for (auto& t : c.tables.tables)
    {
        t.occupied = true;
        for (auto& r : t.rows)
        {
            r.has_vol = (rng() & 1) != 0;
            r.vol = static_cast<uint8_t>(rng());
            r.transpose = static_cast<int8_t>(rng());
            r.cmd1 = static_cast<uint8_t>(rng());
            r.cmd1_param = static_cast<uint8_t>(rng());
            r.cmd2 = static_cast<uint8_t>(rng());
            r.cmd2_param = static_cast<uint8_t>(rng());
        }
    }
    for (auto& w : c.waves.waves)
        for (auto& b : w) b = static_cast<uint8_t>(rng());
    return c;
}

std::vector<uint8_t> encoded(const Content& c, const std::string& name = std::string())
{
    std::vector<uint8_t> v;
    encodeState(c, v, name);
    return v;
}

void fixCrc(std::vector<uint8_t>& b)
{
    const size_t n = b.size();
    const uint32_t crc = crc32Ieee(b.data(), n - 4);
    for (int i = 0; i < 4; ++i) b[n - 4 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
}

}  // namespace

TEST_CASE("plugin 1: state_codec CRC-32 is the IEEE check value")
{
    const char* s = "123456789";
    CHECK(crc32Ieee(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);
}

TEST_CASE("plugin 1: state_codec round-trips random content byte-exact and has the documented size")
{
    std::mt19937 rng(42);
    for (int i = 0; i < 50; ++i)
    {
        const Content c = randomContent(rng);
        const std::vector<uint8_t> blob = encoded(c);
        REQUIRE(blob.size() == 2058);   // v2: 2057 + the 1-byte name length (empty name)
        CHECK(std::memcmp(blob.data(), "MBST", 4) == 0);
        CHECK(blob[4] == 2);
        CHECK(blob[2053] == 0);
        Content d;
        REQUIRE(decodeState(blob.data(), blob.size(), d));
        CHECK(encoded(d) == blob);
        CHECK(std::memcmp(d.waves.waves, c.waves.waves, sizeof(c.waves.waves)) == 0);
    }
}

TEST_CASE("plugin 1: state_codec rejects each corruption and leaves the output untouched")
{
    std::mt19937 rng(7);
    const Content c = randomContent(rng);
    const std::vector<uint8_t> good = encoded(c);
    Content sentinel = randomContent(rng);
    const std::vector<uint8_t> before = encoded(sentinel);

    auto rejects = [&](const std::vector<uint8_t>& b) {
        Content out = sentinel;
        const bool ok = decodeState(b.data(), b.size(), out);
        return !ok && encoded(out) == before;
    };
    std::vector<uint8_t> b = good;
    b[0] ^= 0x01;                              // magic
    CHECK(rejects(b));
    b = good; b[4] = 3;                        // version
    fixCrc(b);                                 // CRC valid, version unknown
    CHECK(rejects(b));
    b = good; b[4] = 1;                        // version 1 is no longer accepted (WP11)
    fixCrc(b);
    CHECK(rejects(b));
    b = good; b.pop_back();                    // truncated by 1 byte
    CHECK(rejects(b));
    b = good; b.push_back(0);                  // one byte too long
    CHECK(rejects(b));
    b = good; b[b.size() - 1] ^= 0x80;         // flipped CRC
    CHECK(rejects(b));
    b = good; b[100] ^= 0x01;                  // flipped payload bit, stale CRC
    CHECK(rejects(b));
    CHECK(rejects(std::vector<uint8_t>{}));
    Content out;
    CHECK_FALSE(decodeState(nullptr, kStateSize, out));
    CHECK(decodeState(good.data(), good.size(), out));
}

TEST_CASE("plugin 1: a v1 blob (2057 bytes, no name field, valid CRC) is rejected")
{
    std::mt19937 rng(11);
    const Content c = randomContent(rng);
    std::vector<uint8_t> v2 = encoded(c);
    // Rebuild the v1 layout: version 1, no length byte, CRC over bytes 0..2052.
    std::vector<uint8_t> v1(v2.begin(), v2.begin() + 2053);
    v1[4] = 1;
    v1.resize(2057);
    const uint32_t crc = crc32Ieee(v1.data(), 2053);
    for (int i = 0; i < 4; ++i) v1[2053 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
    Content out;
    CHECK_FALSE(decodeState(v1.data(), v1.size(), out));
    CHECK_FALSE(decodeState(v1.data(), v1.size(), out, nullptr));
}

TEST_CASE("plugin 8: state v2 round-trips the imported file name (empty, short, UTF-8, 255 bytes) with the content")
{
    std::mt19937 rng(99);
    const std::string names[] = {"", "song.sav", "caf\xC3\xA9 \xE2\x99\xAA.sav", std::string(255, 'x')};
    for (const std::string& name : names)
    {
        CAPTURE(name.size());
        const Content c = randomContent(rng);
        const std::vector<uint8_t> blob = encoded(c, name);
        REQUIRE(blob.size() == kStateSize + name.size());
        CHECK(blob[2053] == name.size());
        Content d;
        std::string got = "sentinel";
        REQUIRE(decodeState(blob.data(), blob.size(), d, &got));
        CHECK(got == name);
        CHECK(encoded(d, got) == blob);
        CHECK(std::memcmp(d.waves.waves, c.waves.waves, sizeof(c.waves.waves)) == 0);

        // Each corruption of a name blob is rejected and leaves both outputs alone.
        std::string keep = "keep";
        Content untouched = randomContent(rng);
        const std::vector<uint8_t> before = encoded(untouched);
        std::vector<uint8_t> b = blob;
        b.pop_back();
        CHECK_FALSE(decodeState(b.data(), b.size(), untouched, &keep));
        b = blob; b.push_back(0);
        CHECK_FALSE(decodeState(b.data(), b.size(), untouched, &keep));
        b = blob; b[2053] = static_cast<uint8_t>(b[2053] + 1); fixCrc(b);   // length byte disagrees with the size
        CHECK_FALSE(decodeState(b.data(), b.size(), untouched, &keep));
        if (!name.empty())
        {
            b = blob; b[2054] ^= 0x01;   // a name byte flipped, stale CRC
            CHECK_FALSE(decodeState(b.data(), b.size(), untouched, &keep));
        }
        CHECK(keep == "keep");
        CHECK(encoded(untouched) == before);
    }
    // A name longer than 255 bytes is cut at 255.
    const Content c = randomContent(rng);
    const std::vector<uint8_t> blob = encoded(c, std::string(300, 'y'));
    CHECK(blob.size() == kStateSize + 255);
    CHECK(blob.size() <= kStateMaxSize);
}

TEST_CASE("plugin 1: 10,000 random blobs never crash (no ASan on this host: UBSan build covers UB)")
{
    std::mt19937 rng(1234);
    std::vector<uint8_t> blob;
    int accepted = 0;
    for (int i = 0; i < 10000; ++i)
    {
        const int kind = i % 4;
        if (kind == 0)
        {
            blob.assign(rng() % 4200, 0);
            for (auto& x : blob) x = static_cast<uint8_t>(rng());
        }
        else
        {
            blob = encoded(randomContent(rng));
            if (kind == 1) blob[rng() % blob.size()] ^= static_cast<uint8_t>(1 + rng() % 255);   // stale CRC
            else if (kind == 2) { blob[5 + rng() % 2000] = static_cast<uint8_t>(rng()); fixCrc(blob); }   // valid
            else blob.resize(rng() % (kStateSize + 3), 0xAA);
        }
        Content out;
        if (decodeState(blob.data(), blob.size(), out)) ++accepted;
    }
    CHECK(accepted > 2000);   // kind 2 blobs are valid
}

TEST_CASE("plugin 2: parameter table shape, names, ids and flags")
{
    const std::vector<mpc::Param> p = buildParams();
    REQUIRE(static_cast<int>(p.size()) == kTotalParams);
    CHECK(kNumParams == 63);
    CHECK(Imp_File == 59);
    CHECK(Imp_Rescan == 62);
    CHECK(p[Imp_File].id == "imp_file");
    CHECK(p[Imp_Go].id == "imp_go");
    CHECK(p[Imp_Undo].id == "imp_undo");
    CHECK(p[Imp_Rescan].id == "imp_rescan");
    CHECK(p[Imp_File].min == 1.0f);
    CHECK(p[Imp_File].max == 64.0f);
    CHECK(p[0].id == "bendrng");
    CHECK(p[P_Volume].name == "Volume");
    std::set<std::string> ids, names;
    for (size_t i = 0; i < p.size(); ++i)
    {
        CAPTURE(p[i].name);
        CHECK(p[i].name.size() <= 12);
        CHECK(ids.insert(p[i].id).second);
        CHECK(names.insert(p[i].name).second);
    }
    for (int i = ED_Table; i < kNumParams; ++i) CHECK_FALSE(p[static_cast<size_t>(i)].automatable);
    for (int i = 0; i < ED_Table; ++i) CHECK(p[static_cast<size_t>(i)].automatable);
    CHECK(p[ED_Table].id == "ed_tbl");
    CHECK(p[ED_Val2].id == "ed_val2");
    CHECK(p[P1_SwShift].name == "P1 SwpShift");
    CHECK(p[P1_Tbl].choices.size() == 17);
    CHECK(p[WV_Wave].choices.size() == 16);
    CHECK(p[P_TblRate].choices.size() == 10);
    CHECK(p[P_TblRate].choices[3] == "24/beat LSDJ");
    CHECK(p[ED_Cmd1].choices.size() == 12);
#ifdef MPCBOY_CPU_PROBE
    CHECK(p.back().id == "dbg_us");
#else
    for (const auto& q : p) CHECK(q.id != "dbg_us");
#endif
}

TEST_CASE("plugin 2: each parameter's default maps to the documented ChannelSettings default")
{
    const std::vector<mpc::Param> p = buildParams();
    float plain[kNumEngineParams];
    for (int i = 0; i < kNumEngineParams; ++i) plain[i] = p[static_cast<size_t>(i)].toPlain(p[static_cast<size_t>(i)].toNormalised(p[static_cast<size_t>(i)].def));
    const EngineSettings s = proposedSettings(plain);

    CHECK(s.bendRange == 2);
    CHECK(s.retrigger == RetriggerMode::Always);
    CHECK(s.noteOff == NoteOffMode::Cut);
    CHECK_FALSE(s.velToVol);
    CHECK(s.masterGain == 1.0f);
    CHECK(s.tick.enabled);
    CHECK(s.tick.mode == TickMode::Sync);
    CHECK(s.tick.ticksPerBeat == 24);
    CHECK(s.tick.phase == TickPhase::Note);
    CHECK(s.tick.freeHz == doctest::Approx(59.73f));
    CHECK(volumeToGain(plain[P_Volume]) == doctest::Approx(0.50119f).epsilon(1e-4));

    const ChannelSettings& a = s.ch[0];
    CHECK(a.enabled); CHECK(a.volume == 15); CHECK_FALSE(a.envUp); CHECK(a.envPeriod == 3);
    CHECK(a.duty == 2); CHECK(a.transpose == 0); CHECK(a.fineCents == 0); CHECK(a.pan == Pan::C);
    CHECK(a.length == 0); CHECK(a.sweepTime == 0); CHECK_FALSE(a.sweepDown); CHECK(a.sweepShift == 0);
    CHECK(a.tableSlot == -1);

    const ChannelSettings& b = s.ch[1];
    CHECK_FALSE(b.enabled); CHECK(b.volume == 15); CHECK_FALSE(b.envUp); CHECK(b.envPeriod == 3);
    CHECK(b.duty == 1); CHECK(b.transpose == 0); CHECK(b.fineCents == 0); CHECK(b.pan == Pan::C);
    CHECK(b.length == 0); CHECK(b.tableSlot == -1);

    const ChannelSettings& w = s.ch[2];
    CHECK_FALSE(w.enabled); CHECK(w.waveLevel == WaveLevel::P100); CHECK(w.waveSlot == 0);
    CHECK(w.transpose == -12); CHECK(w.fineCents == 0); CHECK(w.pan == Pan::C); CHECK(w.length == 0);
    CHECK(w.tableSlot == -1);

    const ChannelSettings& n = s.ch[3];
    CHECK_FALSE(n.enabled); CHECK(n.volume == 15); CHECK_FALSE(n.envUp); CHECK(n.envPeriod == 2);
    CHECK_FALSE(n.noiseWidth7); CHECK(n.noisePitchMode == NoisePitchMode::Track); CHECK(n.noiseFixedShape == 40);
    CHECK(n.transpose == 0); CHECK(n.pan == Pan::C); CHECK(n.length == 0); CHECK(n.tableSlot == -1);
}

TEST_CASE("plugin 2: non-default values map to the right settings (choice order, table and wave slots)")
{
    const std::vector<mpc::Param> p = buildParams();
    float v[kNumEngineParams];
    for (int i = 0; i < kNumEngineParams; ++i) v[i] = p[static_cast<size_t>(i)].def;
    v[P_TblClock] = 1; v[P_TblRate] = 0; v[P_TblPhase] = 1; v[P_Retrigger] = 1; v[P_NoteOff] = 1; v[P_VelVol] = 1;
    v[P1_EnvDir] = 1; v[P1_SwDir] = 1; v[P1_Pan] = 3; v[P1_Tbl] = 16; v[P2_Pan] = 1; v[P2_Tbl] = 1;
    v[WV_Level] = 1; v[WV_Wave] = 15; v[WV_Pan] = 0; v[NZ_Width] = 1; v[NZ_PitchMode] = 1;
    const EngineSettings s = proposedSettings(v);
    CHECK(s.tick.mode == TickMode::Free);
    CHECK(s.tick.ticksPerBeat == 96);
    CHECK(s.tick.phase == TickPhase::Grid);
    CHECK(s.retrigger == RetriggerMode::Legato);
    CHECK(s.noteOff == NoteOffMode::Ring);
    CHECK(s.velToVol);
    CHECK(s.ch[0].envUp); CHECK(s.ch[0].sweepDown); CHECK(s.ch[0].pan == Pan::R); CHECK(s.ch[0].tableSlot == 15);
    CHECK(s.ch[1].pan == Pan::L); CHECK(s.ch[1].tableSlot == 0);
    CHECK(s.ch[2].waveLevel == WaveLevel::P25); CHECK(s.ch[2].waveSlot == 15); CHECK(s.ch[2].pan == Pan::Off);
    CHECK(s.ch[3].noiseWidth7); CHECK(s.ch[3].noisePitchMode == NoisePitchMode::Fixed);
}

TEST_CASE("plugin 2: formatValue is a pure function of the value")
{
    std::string s;
    CHECK(formatParamValue(ED_Vol, -1, s)); CHECK(s == "--");
    CHECK(formatParamValue(ED_Vol, 0xA5, s)); CHECK(s == "A5");
    CHECK(formatParamValue(ED_Vol, 0, s)); CHECK(s == "00");
    CHECK(formatParamValue(ED_Val1, 255, s)); CHECK(s == "FF");
    CHECK(formatParamValue(ED_Val2, 10, s)); CHECK(s == "0A");
    CHECK(formatParamValue(P1_Len, 0, s)); CHECK(s == "Off");
    CHECK(formatParamValue(NZ_Len, 17, s)); CHECK(s == "17");
    CHECK_FALSE(formatParamValue(P_Volume, -6, s));
    CHECK_FALSE(formatParamValue(ED_Tsp, 5, s));
}

TEST_CASE("plugin 2: eight programs, values for 0..50 only, within range")
{
    const std::vector<mpc::Param> p = buildParams();
    const auto& defs = programDefs();
    REQUIRE(defs.size() == 8);
    const char* names[8] = {"Init", "ChipLead", "MajArp", "WaveBass", "Kick", "Hat", "Stack4", "Zap"};
    for (size_t i = 0; i < 8; ++i)
    {
        CHECK(std::string(defs[i].name) == names[i]);
        REQUIRE(static_cast<int>(defs[i].values.size()) == kNumEngineParams);
        for (int k = 0; k < kNumEngineParams; ++k)
        {
            CAPTURE(defs[i].name);
            CAPTURE(p[static_cast<size_t>(k)].name);
            CHECK(defs[i].values[static_cast<size_t>(k)] >= p[static_cast<size_t>(k)].min);
            CHECK(defs[i].values[static_cast<size_t>(k)] <= p[static_cast<size_t>(k)].max);
        }
    }
    // Stack4: all four channels on, four different tables.
    const auto& s4 = defs[kProgramStack4].values;
    CHECK(s4[P1_On] == 1); CHECK(s4[P2_On] == 1); CHECK(s4[WV_On] == 1); CHECK(s4[NZ_On] == 1);
    std::set<int> tables = {static_cast<int>(s4[P1_Tbl]), static_cast<int>(s4[P2_Tbl]),
                            static_cast<int>(s4[WV_Tbl]), static_cast<int>(s4[NZ_Tbl])};
    CHECK(tables.size() == 4);
    CHECK(tables.count(0) == 0);
}
