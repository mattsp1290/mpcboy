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

TEST_CASE("plugin 1: 10,000 random blobs never crash")
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
    CHECK(p[P1_SwShift].name == "PU1 Swp Amt");
    CHECK(p[P1_Tbl].choices.size() == 17);
    CHECK(p[WV_Wave].choices.size() == 16);
    CHECK(p[P_TblRate].choices.size() == 10);
    CHECK(p[P_TblRate].choices[3] == "24/beat");
    CHECK(p[ED_Cmd1].choices.size() == 12);
#ifdef MPCBOY_CPU_PROBE
    CHECK(p.back().id == "dbg_us");
#else
    for (const auto& q : p) CHECK(q.id != "dbg_us");
#endif
}

// The parameter freeze rule (04-plugin-params-ui-state.md): ids are never renamed or
// reordered; new parameters are only appended (and this list grows with them).
TEST_CASE("plugin 2: the ordered list of all 63 parameter ids is frozen")
{
    static const char* const kIds[] = {
        "bendrng", "vol", "retrig", "noteoff", "velvol", "tblmode",
        "tblrate", "tblphase", "freehz", "p1_on", "p1_vol", "p1_envdir",
        "p1_envspd", "p1_duty", "p1_tsp", "p1_fine", "p1_pan", "p1_len",
        "p1_swtime", "p1_swdir", "p1_swshift", "p1_tbl", "p2_on", "p2_vol",
        "p2_envdir", "p2_envspd", "p2_duty", "p2_tsp", "p2_fine", "p2_pan",
        "p2_len", "p2_tbl", "wv_on", "wv_level", "wv_wave", "wv_tsp",
        "wv_fine", "wv_pan", "wv_len", "wv_tbl", "nz_on", "nz_vol",
        "nz_envdir", "nz_envspd", "nz_width", "nz_pmode", "nz_shape", "nz_tsp",
        "nz_pan", "nz_len", "nz_tbl", "ed_tbl", "ed_row", "ed_vol",
        "ed_tsp", "ed_cmd1", "ed_val1", "ed_cmd2", "ed_val2", "imp_file",
        "imp_go", "imp_undo", "imp_rescan"};
    static_assert(sizeof(kIds) / sizeof(kIds[0]) == 63, "the golden list has 63 ids");
    const std::vector<mpc::Param> p = buildParams();
    REQUIRE(p.size() >= 63);
    for (size_t i = 0; i < 63; ++i)
    {
        CAPTURE(i);
        CHECK(p[i].id == kIds[i]);
    }
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
    v[P1_EnvDir] = 1; v[P1_SwDir] = 0;   /* Swp Dir: Down, Up */ v[P1_Pan] = 3; v[P1_Tbl] = 16; v[P2_Pan] = 1; v[P2_Tbl] = 1;
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
    CHECK(formatParamValue(ED_Vol, -1, s)); CHECK(s == "Keep");
    CHECK(formatParamValue(ED_Vol, 0xA5, s)); CHECK(s == "A5");
    CHECK(formatParamValue(ED_Vol, 0, s)); CHECK(s == "00");
    CHECK(formatParamValue(ED_Val1, 255, s)); CHECK(s == "FF");
    CHECK(formatParamValue(ED_Val2, 10, s)); CHECK(s == "0A");
    CHECK(formatParamValue(P1_Len, 0, s)); CHECK(s == "Off");
    CHECK(formatParamValue(NZ_Len, 17, s)); CHECK(s == "66ms");
    CHECK_FALSE(formatParamValue(P_Volume, -6, s));
    CHECK(formatParamValue(ED_Tsp, 5, s)); CHECK(s == "+5 st");
}

namespace {

// Text the MPC shows for `plain`: formatParamValue, else the SDK's default formatting.
std::string valueText(const std::vector<mpc::Param>& p, int index, float plain)
{
    std::string s;
    if (!formatParamValue(index, plain, s)) s = p[static_cast<size_t>(index)].display(plain);
    return s;
}

}  // namespace

// Redesign step 1 (.agents/plans/mpcboy-gb-ui/redesign/agreed-terms.md).
TEST_CASE("plugin 2: agreed terms - every display name, in index order")
{
    static const char* const kNames[] = {
        "Bend Range", "Volume", "Retrigger", "Note Off", "Vel to Level", "Table Sync",
        "Table Rate", "Table Start", "Free Rate",
        "PU1 On", "PU1 Level", "PU1 Env Dir", "PU1 Env Time", "PU1 Duty", "PU1 Semi", "PU1 Fine", "PU1 Pan",
        "PU1 Length", "PU1 Swp Time", "PU1 Swp Dir", "PU1 Swp Amt", "PU1 Table",
        "PU2 On", "PU2 Level", "PU2 Env Dir", "PU2 Env Time", "PU2 Duty", "PU2 Semi", "PU2 Fine", "PU2 Pan",
        "PU2 Length", "PU2 Table",
        "WAV On", "WAV Level", "WAV Shape", "WAV Semi", "WAV Fine", "WAV Pan", "WAV Length", "WAV Table",
        "NOI On", "NOI Level", "NOI Env Dir", "NOI Env Time", "NOI Mode", "NOI Pitch", "NOI FixPitch",
        "NOI Semi", "NOI Pan", "NOI Length", "NOI Table",
        "Edit Table", "Edit Row", "Row Vol", "Row Semi", "Row Cmd1", "Row Val1", "Row Cmd2", "Row Val2",
        "Import File", "Import Now", "Import Undo", "Import Scan"};
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == 63, "63 names");
    const std::vector<mpc::Param> p = buildParams();
    REQUIRE(p.size() >= 63);
    for (size_t i = 0; i < 63; ++i)
    {
        CAPTURE(i);
        CHECK(p[i].name == kNames[i]);
        CHECK(p[i].name.size() <= 12);
    }
}

TEST_CASE("plugin 2: agreed terms - names are at most 12 characters, every value text at most 8")
{
    const std::vector<mpc::Param> p = buildParams();
    for (int i = 0; i < kNumParams; ++i)
    {
        const mpc::Param& q = p[static_cast<size_t>(i)];
        CAPTURE(q.name);
        CHECK(q.name.size() <= 12);
        if (i == Imp_File) continue;   // the plugin formats it from the scan: at most 8 bytes (import_worker)
        for (const std::string& c : q.choices) { CAPTURE(c); CHECK(c.size() <= 8); }
        if (q.kind == mpc::Param::Kind::Float)
        {
            for (float v : {q.min, q.def, q.max, 0.0f}) { const std::string t = valueText(p, i, v); CAPTURE(t); CHECK(t.size() <= 8); }
            continue;
        }
        for (int v = static_cast<int>(q.min); v <= static_cast<int>(q.max); ++v)
        {
            const std::string t = valueText(p, i, static_cast<float>(v));
            CAPTURE(t);
            CHECK(t.size() <= 8);
            if (q.kind == mpc::Param::Kind::Int && q.max > q.min)
            {
                // Pure function of the value: format another value in between, then this one again.
                const int other = v < static_cast<int>(q.max) ? v + 1 : v - 1;
                (void)valueText(p, i, static_cast<float>(other));
                CHECK(valueText(p, i, static_cast<float>(v)) == t);
            }
        }
    }
}

TEST_CASE("plugin 2: agreed terms - choice labels and value text")
{
    const std::vector<mpc::Param> p = buildParams();
    auto labels = [&](int i) { return p[static_cast<size_t>(i)].choices; };
    using V = std::vector<std::string>;
    CHECK(labels(P1_Pan) == V{"Mute", "Left", "Both", "Right"});
    CHECK(labels(NZ_Pan) == labels(P1_Pan));
    CHECK(labels(WV_Level) == V{"Mute", "25%", "50%", "100%"});
    CHECK(labels(P1_EnvDir) == V{"Down", "Up"});
    CHECK(labels(P1_SwDir) == V{"Down", "Up"});
    CHECK(labels(NZ_Width) == V{"Hiss 15", "Buzz 7"});
    CHECK(labels(NZ_PitchMode) == V{"Track", "Fixed"});
    CHECK(labels(P_TblClock) == V{"Tempo", "Free"});
    CHECK(labels(P_TblPhase) == V{"Note", "Grid"});
    CHECK(labels(P_NoteOff) == V{"Cut", "Hold"});
    CHECK(labels(P_TblRate).front() == "96/beat");
    CHECK(labels(P_TblRate).back() == "3/beat");
    CHECK(labels(P1_Duty) == V{"12.5%", "25%", "50%", "75%"});
    CHECK(labels(ED_Cmd1) == V{"-", "D Delay", "E Env", "H Hop", "K Kill", "L Slide", "N Noise", "P Pitch", "R Retrig", "S Sweep", "V Vib", "W Wave"});
    CHECK(labels(ED_Cmd2) == labels(ED_Cmd1));
    // Wave and table names, user slots numbered.
    const V wave = labels(WV_Wave);
    CHECK(wave[0] == "Saw"); CHECK(wave[7] == "Organ"); CHECK(wave[8] == "Wave 09"); CHECK(wave[15] == "Wave 16");
    const V table = labels(P1_Tbl);
    CHECK(table[0] == "Off"); CHECK(table[1] == "MajArp"); CHECK(table[8] == "Hat");
    CHECK(table[9] == "Table 09"); CHECK(table[16] == "Table 16");
    CHECK(labels(NZ_Tbl) == table);

    CHECK(valueText(p, P1_EnvSpd, 0) == "Off"); CHECK(valueText(p, P1_EnvSpd, 7) == "7");
    CHECK(valueText(p, NZ_EnvSpd, 0) == "Off");
    CHECK(valueText(p, P1_SwTime, 0) == "Off"); CHECK(valueText(p, P1_SwTime, 3) == "3");
    CHECK(valueText(p, P1_SwShift, 0) == "Off"); CHECK(valueText(p, P1_SwShift, 7) == "7");
    CHECK(valueText(p, P1_Tsp, 12) == "+12 st"); CHECK(valueText(p, P1_Tsp, -24) == "-24 st"); CHECK(valueText(p, WV_Tsp, 0) == "0 st");
    CHECK(valueText(p, P1_Fine, 25) == "+25 ct"); CHECK(valueText(p, P2_Fine, -50) == "-50 ct"); CHECK(valueText(p, WV_Fine, 0) == "0 ct");
    CHECK(valueText(p, ED_Tsp, 48) == "+48 st");
    CHECK(valueText(p, ED_Vol, -1) == "Keep"); CHECK(valueText(p, ED_Vol, 0x2A) == "2A");
    CHECK(valueText(p, P_FreeHz, 59.73f) == "59.73Hz");
    CHECK(valueText(p, Imp_Go, 0) == "Off"); CHECK(valueText(p, Imp_Undo, 1) == "On");   // triggers stay plain toggles
    CHECK(p[Imp_Go].kind == mpc::Param::Kind::Bool);
}

TEST_CASE("plugin 2: agreed terms - Length text in milliseconds, per channel")
{
    const std::vector<mpc::Param> p = buildParams();
    // Pulse and Noise count 1/256 s per step (1..64: 4..250 ms), Wave 4/256 s per step (16..1000 ms).
    for (int idx : {P1_Len, P2_Len, NZ_Len})
    {
        CHECK(valueText(p, idx, 0) == "Off");
        CHECK(valueText(p, idx, 1) == "4ms");
        CHECK(valueText(p, idx, 2) == "8ms");
        CHECK(valueText(p, idx, 32) == "125ms");
        CHECK(valueText(p, idx, 64) == "250ms");
    }
    CHECK(valueText(p, WV_Len, 0) == "Off");
    CHECK(valueText(p, WV_Len, 1) == "16ms");
    CHECK(valueText(p, WV_Len, 2) == "31ms");
    CHECK(valueText(p, WV_Len, 32) == "500ms");
    CHECK(valueText(p, WV_Len, 64) == "1000ms");
    // Increasing with the value, on every channel.
    for (int idx : {P1_Len, WV_Len})
    {
        int prev = 0;
        for (int v = 1; v <= 64; ++v)
        {
            const int ms = std::stoi(valueText(p, idx, static_cast<float>(v)));
            CHECK(ms > prev);
            prev = ms;
        }
    }
}

TEST_CASE("plugin 2: agreed terms - Swp Amt, Swp Dir and NOI FixPitch behaviour")
{
    const std::vector<mpc::Param> p = buildParams();
    auto defaults = [&] {
        std::vector<float> v(kNumEngineParams);
        for (int i = 0; i < kNumEngineParams; ++i) v[static_cast<size_t>(i)] = p[static_cast<size_t>(i)].def;
        return v;
    };
    // Swp Amt: Off, 1..7; the register shift is 8 - amt (Off: 0), so a higher amount is a bigger step.
    for (int amt = 0; amt <= 7; ++amt)
    {
        std::vector<float> v = defaults();
        v[P1_SwShift] = static_cast<float>(amt);
        const EngineSettings s = proposedSettings(v.data());
        CAPTURE(amt);
        CHECK(s.ch[0].sweepShift == (amt == 0 ? 0 : 8 - amt));
    }
    // Swp Dir: Down first (0), Up second (1, the default); NR10 bit 3 is set for Down.
    {
        std::vector<float> v = defaults();
        CHECK(p[P1_SwDir].def == 1.0f);
        CHECK_FALSE(proposedSettings(v.data()).ch[0].sweepDown);
        v[P1_SwDir] = 0;
        CHECK(proposedSettings(v.data()).ch[0].sweepDown);
        v[P1_SwDir] = 1;
        CHECK_FALSE(proposedSettings(v.data()).ch[0].sweepDown);
    }
    // NOI FixPitch: pitch-sorted, strictly increasing frequency (262144 / (r * 2^(s+1)), r = 0.5 for code 0),
    // duplicates collapsed. The engine setting keeps the register index shift * 8 + divisor code.
    {
        std::vector<float> v = defaults();
        const int n = noiseFixPitchCount();
        CHECK(p[NZ_Shape].max == static_cast<float>(n - 1));
        CHECK(proposedSettings(v.data()).ch[3].noiseFixedShape == 40);   // the old default (8192 Hz)
        CHECK(noiseFixPitchRegIndex(static_cast<int>(p[NZ_Shape].def)) == 40);
        v[NZ_Shape] = static_cast<float>(n - 1);
        CHECK(proposedSettings(v.data()).ch[3].noiseFixedShape == 0);    // shift 0, divisor code 0: the highest
        v[NZ_Shape] = 0;
        CHECK(proposedSettings(v.data()).ch[3].noiseFixedShape == 111);  // shift 13, divisor code 7: the lowest
        auto hz = [](int idx) { const int s = idx / 8, r = idx % 8; return 262144.0 / ((r == 0 ? 0.5 : r) * (1 << (s + 1))); };
        std::set<int> regs;
        for (int k = 0; k < n; ++k)
        {
            const int idx = noiseFixPitchRegIndex(k);
            CAPTURE(k);
            CHECK(idx >= 0); CHECK(idx <= 111);
            CHECK(noiseFixPitchHz(k) == doctest::Approx(hz(idx)));   // the table's frequency is the register's
            if (k > 0) CHECK(noiseFixPitchHz(k) > noiseFixPitchHz(k - 1));   // strictly increasing
            regs.insert(idx);
        }
        CHECK(static_cast<int>(regs.size()) == n);
        // Every register frequency is represented.
        std::set<double> freqs;
        for (int i = 0; i < 112; ++i) freqs.insert(hz(i));
        CHECK(static_cast<int>(freqs.size()) == n);
        // The Hat program's old register index 12 keeps its frequency.
        CHECK(noiseFixPitchHz(noiseFixPitchValueNear(12)) == doctest::Approx(hz(12)));
    }
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
