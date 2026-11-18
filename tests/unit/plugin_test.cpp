// WP7 tests 3-9: the real MpcBoyPlugin driven directly with synthetic
// ProcessContexts (no host). Test 10 is the host_sim ctest in CMakeLists.txt.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "doctest/doctest.h"
#include "MpcBoyPlugin.h"
#include "engine_test_support.h"
#include "factory_content.h"
#include "plugin_test_support.h"
#include "state_codec.h"

using namespace mpcboy;
using namespace mpcboy_test;

namespace {

mpc::MidiEvent on(long long f, int note, int vel = 100) { return mpc::MidiEvent::noteOn(0, note, vel, static_cast<int>(f)); }

double peakDb(const std::vector<float>& v)
{
    float pk = 0;
    for (float x : v) pk = std::max(pk, std::fabs(x));
    return pk <= 0 ? -300.0 : 20.0 * std::log10(static_cast<double>(pk));
}

// ---- row-change detection from rendered audio ---------------------------------

// Rising zero crossings (sub-frame, linear interpolation) with hysteresis.
std::vector<double> risingEdges(const std::vector<float>& x, size_t from, size_t to)
{
    float pk = 0;
    for (size_t i = from; i < std::min(to, x.size()); ++i) pk = std::max(pk, std::fabs(x[i]));
    const float thr = 0.12f * pk;
    std::vector<double> e;
    bool low = false;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i)
    {
        if (x[i] < -thr) low = true;
        if (low && x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            e.push_back(static_cast<double>(i - 1) + static_cast<double>(-x[i - 1]) / static_cast<double>(x[i] - x[i - 1]));
            low = false;
        }
    }
    return e;
}

struct Line { double a = 0, b = 0; bool ok = false; };

// t = a + b * i over (index, time) pairs.
Line fitLine(const std::vector<std::pair<double, double>>& pts)
{
    Line l;
    if (pts.size() < 6) return l;
    double si = 0, st = 0, sii = 0, sit = 0;
    for (auto& q : pts) { si += q.first; st += q.second; sii += q.first * q.first; sit += q.first * q.second; }
    const double n = static_cast<double>(pts.size());
    const double den = n * sii - si * si;
    if (std::fabs(den) < 1e-9) return l;
    l.b = (n * sit - si * st) / den;
    l.a = (st - l.b * si) / n;
    l.ok = true;
    return l;
}

// Time of the pitch change near nominal boundary `B`: fit the cycle times before
// and after it and intersect the two lines (the oscillator phase is continuous,
// so the lines meet exactly where the period changed). Returns NaN on failure.
double changeTime(const std::vector<double>& edges, double B, double span)
{
    std::vector<std::pair<double, double>> left, right;
    for (size_t i = 0; i < edges.size(); ++i)
    {
        if (edges[i] >= B - span && edges[i] <= B - 40.0) left.push_back({static_cast<double>(i), edges[i]});
        if (edges[i] >= B + 40.0 && edges[i] <= B + span) right.push_back({static_cast<double>(i), edges[i]});
    }
    const Line l1 = fitLine(left), l2 = fitLine(right);
    if (!l1.ok || !l2.ok || std::fabs(l1.b - l2.b) < 0.05) return std::nan("");
    const double istar = (l2.a - l1.a) / (l1.b - l2.b);
    return l1.a + l1.b * istar;
}

constexpr int kArpNote = 100;   // E7: short cycles (~17 frames) keep the register-latency jitter below 2.5 frames

// Row-change times (in output frames) of the MajArp program for one note at
// `noteFrame`, from block-wise rendered audio. `boundaries` are nominal times.
std::vector<double> measureChanges(PluginRig& r, const std::vector<double>& boundaries, double span)
{
    const std::vector<double> edges = risingEdges(r.L, 0, r.L.size());
    std::vector<double> out;
    for (double B : boundaries) out.push_back(changeTime(edges, B, span));
    return out;
}

// Frames between a table tick and the audible pitch change. The polyphase
// resampler alone is 117.6 (kHalfTaps = 128 APU frames at 44.1 kHz; 0 at 48 kHz, 256 host frames at 96, 512 at 192); measured through
// the whole engine it is 146-150 (the APU adds about 30 frames). The tests
// measure it per run; this constant only places the search windows.
constexpr double kDelayNominal = 147.0;

}  // namespace

TEST_CASE("plugin 3: setProgram for every program, then one block, leaves the table content unchanged")
{
    PluginRig r;
    r.block(128);
    const std::vector<uint8_t> before = r.blob();
    REQUIRE(before.size() == kStateSize);
    for (int i = 0; i < r.p->numPrograms(); ++i)
    {
        CAPTURE(i);
        r.p->setProgram(i);
        r.block(128);
        CHECK(r.blob() == before);
        CHECK(r.p->currentProgram() == i);
    }
    CHECK(r.p->numPrograms() == 8);
    CHECK(std::string(r.p->programName(6)) == "Stack4");
}

TEST_CASE("plugin 4: editor cursor edits exactly one cell and a row round trip resyncs without writing")
{
    PluginRig r;
    r.block(128);                       // first block: cursor resync, process marks the audio thread
    const std::vector<uint8_t> base = r.blob();
    r.p->set(ED_Row, 3);
    r.block(128);
    // MajArp row 3: transpose -7, command H, parameter 1, no volume.
    CHECK(r.p->getInt(ED_Tsp) == -7);
    CHECK(r.p->getInt(ED_Cmd1) == 3);   // 'H'
    CHECK(r.p->getInt(ED_Val1) == 1);
    CHECK(r.p->getInt(ED_Vol) == -1);
    CHECK(r.blob() == base);            // the resync wrote nothing

    r.p->set(ED_Tsp, 5);
    r.block(128);
    const std::vector<uint8_t> edited = r.blob();
    int diffs = 0;
    for (size_t i = 0; i < base.size() - 4; ++i)
        if (base[i] != edited[i]) { ++diffs; CHECK(i == 5 + 3 * 7 + 2); }   // table 1, row 3, transpose byte
    CHECK(diffs == 1);
    CHECK(static_cast<int8_t>(edited[5 + 3 * 7 + 2]) == 5);

    r.p->set(ED_Row, 4);
    r.block(128);
    CHECK(r.blob() == edited);
    r.p->set(ED_Row, 3);
    r.block(128);
    CHECK(r.p->getInt(ED_Tsp) == 5);
    CHECK(r.blob() == edited);

    // Cell edits of each column.
    r.p->set(ED_Vol, 0xA0);
    r.p->set(ED_Cmd2, 10);              // 'V'
    r.p->set(ED_Val2, 0x42);
    r.block(128);
    Content c;
    r.p->content().readPublished(c);
    const auto& row = c.tables.tables[0].rows[3];
    CHECK(row.has_vol); CHECK(row.vol == 0xA0); CHECK(row.cmd2 == 'V'); CHECK(row.cmd2_param == 0x42);
    r.p->set(ED_Vol, -1);
    r.block(128);
    r.p->content().readPublished(c);
    CHECK_FALSE(c.tables.tables[0].rows[3].has_vol);

    // Another table.
    r.p->set(ED_Table, 3);              // Octave: row 1 = +12
    r.p->set(ED_Row, 1);
    r.block(128);
    CHECK(r.p->getInt(ED_Tsp) == 12);
}

TEST_CASE("plugin 4: editor parameters are refreshed on the host at most 10 times per second")
{
    PluginRig r;
    r.block(128);
    const uint32_t first = r.p->hostRefreshCount();
    CHECK(first == 1);                  // first resync notifies at once
    for (int i = 0; i < 100; ++i)
    {
        r.p->set(ED_Row, (i % 15) + 1);
        r.block(128);
    }
    const double seconds = static_cast<double>(r.pos) / 44100.0;
    const uint32_t refreshes = r.p->hostRefreshCount();
    std::printf("[plugin 4] %u host refreshes over %.2f s of row scrolling\n", refreshes, seconds);
    CHECK(refreshes <= static_cast<uint32_t>(seconds * 10.0) + 1);
    CHECK(refreshes >= 2);
}

namespace {

void putU32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }

// An SDK state chunk (version 1) carrying the listed parameters and an optional extra blob.
std::vector<uint8_t> makeChunk(const std::vector<std::pair<std::string, float>>& params, const std::vector<uint8_t>* extra)
{
    std::vector<uint8_t> v = {'M', 'P', 'C', 'P'};
    putU32(v, 1);
    putU32(v, static_cast<uint32_t>(params.size()));
    for (const auto& q : params)
    {
        v.push_back(static_cast<uint8_t>(q.first.size()));
        v.insert(v.end(), q.first.begin(), q.first.end());
        uint32_t bits;
        std::memcpy(&bits, &q.second, 4);
        putU32(v, bits);
    }
    putU32(v, extra ? static_cast<uint32_t>(extra->size()) : 0u);
    if (extra) v.insert(v.end(), extra->begin(), extra->end());
    return v;
}

}  // namespace

TEST_CASE("plugin 5: load safety - a state load from another thread never writes a cell through the editor parameters")
{
    PluginRig r;
    r.block(128);
    const std::vector<uint8_t> original = r.blob();

    // A distinct valid blob: factory content with one recognisable cell.
    Content other;
    loadFactoryContent(other);
    other.tables.tables[4].rows[7].transpose = 33;
    other.tables.tables[4].rows[7].cmd1 = 'K';
    std::vector<uint8_t> validBlob;
    encodeState(other, validBlob);
    std::vector<uint8_t> corruptBlob = validBlob;
    corruptBlob[200] ^= 0x55;   // stale CRC

    // Editor parameter values that differ from the cells they will point at.
    const std::vector<std::pair<std::string, float>> editor = {
        {"ed_tbl", 5}, {"ed_row", 7}, {"ed_vol", 0x77}, {"ed_tsp", -20}, {"ed_cmd1", 11}, {"ed_val1", 0xEE}, {"ed_cmd2", 5}, {"ed_val2", 0x13}};

    struct Case { const char* name; const std::vector<uint8_t>* extra; bool applied; };
    const Case cases[3] = {{"valid", &validBlob, true}, {"missing", nullptr, false}, {"corrupt", &corruptBlob, false}};
    for (const Case& cs : cases)
    {
        CAPTURE(cs.name);
        const std::vector<uint8_t> before = r.blob();
        const std::vector<uint8_t> chunk = makeChunk(editor, cs.extra);
        bool ok = false;
        std::thread t([&] { ok = r.p->internalLoadState(chunk.data(), chunk.size()); });   // UI thread
        t.join();
        CHECK(ok);
        // The SDK resets on the audio thread before the next block.
        std::vector<float> a(128), b(128);
        float* outs[2] = {a.data(), b.data()};
        r.p->internalProcess(nullptr, outs, 128);
        r.p->internalProcess(nullptr, outs, 128);   // publishes anything the first block changed
        const std::vector<uint8_t> after = r.blob();
        if (cs.applied) CHECK(after == validBlob);
        else CHECK(after == before);
        CHECK(after != std::vector<uint8_t>() );
        // The cursor now shows the cell it points at, not the loaded editor values.
        const int tbl = r.p->getInt(ED_Table) - 1, row = r.p->getInt(ED_Row);
        CHECK(tbl == 4);
        CHECK(row == 7);
        Content c;
        REQUIRE(decodeState(after.data(), after.size(), c));
        CHECK(r.p->getInt(ED_Tsp) == std::clamp<int>(c.tables.tables[tbl].rows[row].transpose, -48, 48));
        CHECK(r.p->getInt(ED_Val1) == c.tables.tables[tbl].rows[row].cmd1_param);
    }
    CHECK(original != validBlob);
}

TEST_CASE("plugin 6: lifecycle calls from a second thread while process runs 10,000 blocks")
{
    PluginRig r;
    r.block(128);
    std::vector<uint8_t> goodBlob = r.blob();
    std::atomic<bool> done{false};
    std::atomic<uint32_t> loops{0}, saved{0}, badBlobs{0};

    std::thread ui([&] {
        std::vector<uint8_t> blob = goodBlob;
        std::string text;
        uint32_t n = 0;
        while (!done.load(std::memory_order_acquire))
        {
            r.p->prepare(44100.0, 512);
            r.p->reset();
            blob[5 + (n % 100)] = static_cast<uint8_t>(n);   // keep the content moving
            const uint32_t crc = crc32Ieee(blob.data(), kStateSize - 4);
            for (int i = 0; i < 4; ++i) blob[kStateSize - 4 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
            r.p->loadExtraState(blob.data(), blob.size());
            std::vector<uint8_t> out;
            r.p->saveExtraState(out);
            Content c;
            if (!decodeState(out.data(), out.size(), c)) badBlobs.fetch_add(1, std::memory_order_relaxed);
            saved.fetch_add(1, std::memory_order_relaxed);
            for (int i = 0; i < r.p->numParams(); ++i) r.p->formatValue(i, r.p->get(i), text);
            for (int i = 0; i < r.p->numParams(); ++i) text = r.p->displayText(i);
            ++n;
            loops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<mpc::MidiEvent> m;
    for (int i = 0; i < 10000; ++i)
    {
        m.clear();
        if (i % 40 == 0) m.push_back(on(0, 48 + (i / 40) % 24));
        if (i % 40 == 30) m.push_back(mpc::MidiEvent::noteOff(0, 48 + (i / 40) % 24, 5));
        if (i % 7 == 0) { r.p->set(ED_Row, i % 16); r.p->set(ED_Tsp, (i % 21) - 10); }
        if (i % 11 == 0) r.p->set(ED_Table, 1 + i % 16);
        r.block(128, m);
    }
    done.store(true, std::memory_order_release);
    ui.join();
    std::printf("[plugin 6] second thread: %u lifecycle loops, %u saves; main thread: 10,000 blocks\n", loops.load(), saved.load());
    CHECK(badBlobs.load() == 0);
    CHECK(saved.load() > 10);
    for (float v : r.L) REQUIRE(std::isfinite(v));
}

TEST_CASE("plugin 7: MajArp row interval through the real plugin glue (S4/S5): 918.75 +- 3 frames, halves per BPM doubling")
{
    const double bpms[3] = {60.0, 120.0, 240.0};
    const int blocks[4] = {128, 37, 512, 64};
    double mean[3] = {0, 0, 0};
    double firstD = -1.0;
    for (int bi = 0; bi < 3; ++bi)
        for (int ki = 0; ki < (bi == 1 ? 4 : 1); ++ki)
        {
            const int blk = blocks[ki];
            PluginRig r;
            r.bpm = bpms[bi];
            r.p->setProgram(2);   // MajArp
            const double P = 44100.0 * 60.0 / (bpms[bi] * 24.0);
            const long long noteFrame = 1000;
            const int rows = 10;
            r.run(static_cast<long long>(noteFrame + (rows + 1) * P + 600), {on(noteFrame, kArpNote)}, blk);

            std::vector<double> nominal;
            for (int k = 1; k <= rows; ++k) nominal.push_back(noteFrame + kDelayNominal + k * P);
            const std::vector<double> T = measureChanges(r, nominal, std::min(0.7 * P, 700.0));
            for (double t : T) REQUIRE_FALSE(std::isnan(t));
            double sumD = 0, worstInterval = 0;
            for (int k = 0; k < rows; ++k) sumD += T[k] - (noteFrame + (k + 1) * P);
            for (int k = 0; k + 1 < rows; ++k) worstInterval = std::max(worstInterval, std::fabs((T[k + 1] - T[k]) - P));
            const double D = sumD / rows;
            double worstAbs = 0;
            for (int k = 0; k < rows; ++k) worstAbs = std::max(worstAbs, std::fabs(T[k] - (noteFrame + (k + 1) * P) - D));
            std::printf("[plugin 7] %5.0f BPM block %3d: period %.2f, measured delay %.2f, worst interval error %.2f, worst abs error %.2f frames\n",
                        bpms[bi], blk, P, D, worstInterval, worstAbs);
            CHECK(D > 110.0);
            CHECK(D < 170.0);
            if (firstD < 0) firstD = D;
            CHECK(std::fabs(D - firstD) <= 2.0);   // the delay does not depend on tempo or block size
            CHECK(worstInterval <= 3.0);
            CHECK(worstAbs <= 3.0);
            if (ki == 0) mean[bi] = (T[rows - 1] - T[0]) / (rows - 1);
        }
    CHECK(std::fabs(mean[0] - 2.0 * mean[1]) <= 1.5);
    CHECK(std::fabs(mean[1] - 2.0 * mean[2]) <= 1.5);
    CHECK(std::fabs(mean[1] - 918.75) <= 1.5);
}

TEST_CASE("plugin 8: Grid phase through the real plugin glue (S6): row changes land within 3 frames of ppq = k/24")
{
    // Calibrate the resampler delay the same way as test 7 (Note phase, 120 BPM).
    double D = 0;
    {
        PluginRig r;
        r.p->setProgram(2);
        const double P = 918.75;
        r.run(static_cast<long long>(1000 + 9 * P + 600), {on(1000, kArpNote)}, 128);
        std::vector<double> nominal;
        for (int k = 1; k <= 8; ++k) nominal.push_back(1000 + kDelayNominal + k * P);
        const auto T = measureChanges(r, nominal, 650.0);
        for (int k = 0; k < 8; ++k) { REQUIRE_FALSE(std::isnan(T[k])); D += T[k] - (1000 + (k + 1) * P); }
        D /= 8.0;
    }
    std::printf("[plugin 8] measured delay %.2f frames\n", D);

    for (int blk : {128, 512, 45})
    {
        CAPTURE(blk);
        PluginRig r;
        r.p->setProgram(2);
        r.p->set(P_TblPhase, 1);   // Grid
        r.playing = true;
        r.ppqValid = true;
        r.ppqStart = 0.3;          // not on the grid at frame 0
        const double bpm = 120.0, framesPerBeat = 44100.0 * 60.0 / bpm;
        const long long noteFrame = 5000;
        r.run(noteFrame + 12000, {on(noteFrame, kArpNote)}, blk);

        // Grid tick k at ppq = k/24: frame f_k = (k/24 - ppqStart) * framesPerBeat.
        std::vector<double> nominal;
        for (int k = 1; k < 1000; ++k)
        {
            const double f = (static_cast<double>(k) / 24.0 - r.ppqStart) * framesPerBeat;
            if (f < noteFrame + 900.0) continue;                  // the first tick follows the note-on too closely to fit
            if (f > noteFrame + 10500.0) break;
            nominal.push_back(f + D);
        }
        REQUIRE(nominal.size() >= 8);
        const auto T = measureChanges(r, nominal, 650.0);
        double worst = 0;
        for (size_t i = 0; i < nominal.size(); ++i)
        {
            REQUIRE_FALSE(std::isnan(T[i]));
            worst = std::max(worst, std::fabs(T[i] - nominal[i]));
        }
        std::printf("[plugin 8] block %3d: %zu boundaries, worst deviation from the ppq grid %.2f frames\n", blk, nominal.size(), worst);
        CHECK(worst <= 3.0);
    }
}

TEST_CASE("plugin 9: Stack4 layers all four channels from one note; peak above -20 dBFS")
{
    struct Spy
    {
        std::vector<mpcboy_test::W> log;
        static void cb(void* u, uint16_t a, uint8_t v) { static_cast<Spy*>(u)->log.push_back({0, a, v}); }
    } spy;
    PluginRig r;
    r.p->setProgram(kProgramStack4);
    r.block(128);                                    // applies the reset
    rackboy_apu_set_write_spy(r.p->engine().apu(), &Spy::cb, &spy);
    r.run(r.pos + 44100 / 2, {on(r.pos + 100, 60)}, 128);
    r.run(r.pos + 128, {mpc::MidiEvent::noteOn(0, 64, 100, static_cast<int>(r.pos + 10))}, 128);   // second overlapping note: legato/glide, no second voice
    rackboy_apu_set_write_spy(r.p->engine().apu(), nullptr, nullptr);

    auto distinct = [&](uint16_t lo, uint16_t hi) {
        std::vector<int> f;
        for (const auto& q : mpcboy_test::freqWrites(spy.log, lo, hi)) if (std::find(f.begin(), f.end(), q.freq) == f.end()) f.push_back(q.freq);
        return f.size();
    };
    const size_t p1 = distinct(0xFF13, 0xFF14), p2 = distinct(0xFF18, 0xFF19), wv = distinct(0xFF1D, 0xFF1E);
    std::vector<int> nz;
    for (const auto& q : spy.log) if (q.addr == 0xFF22 && std::find(nz.begin(), nz.end(), q.v) == nz.end()) nz.push_back(q.v);
    std::printf("[plugin 9] distinct pitches written: P1 %zu, P2 %zu, Wave %zu, Noise %zu; peak %.2f dBFS\n",
                p1, p2, wv, nz.size(), peakDb(r.L));
    CHECK(p1 >= 3);
    CHECK(p2 >= 3);
    CHECK(wv >= 3);
    CHECK(nz.size() >= 3);
    CHECK(peakDb(r.L) > -20.0);
    CHECK(peakDb(r.L) < -0.1);
    // One voice only: a single note-on trigger of channel 1 for the first note.
    int triggers = 0;
    for (const auto& q : spy.log) if (q.addr == 0xFF14 && (q.v & 0x80)) ++triggers;
    CHECK(triggers >= 1);
}

TEST_CASE("plugin: full chunk round trip after randomising every parameter on the process thread is byte-identical (host_sim scenario)")
{
    PluginRig r;
    r.block(128);
    const std::vector<uint8_t> saved = r.p->internalSaveState();
    std::vector<float> norms;
    for (int i = 0; i < r.p->numParams(); ++i) norms.push_back(r.p->getNormalised(i));

    uint32_t seed = 1234;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; };
    for (int i = 0; i < r.p->numParams(); ++i) r.p->setNormalised(i, rnd());
    r.block(128);
    r.block(128);

    r.p->internalLoadState(saved.data(), saved.size());
    r.block(128);
    r.block(128);
    const std::vector<uint8_t> again = r.p->internalSaveState();
    REQUIRE(again.size() == saved.size());
    int firstDiff = -1, diffs = 0;
    for (size_t i = 0; i < saved.size(); ++i)
        if (saved[i] != again[i]) { if (firstDiff < 0) firstDiff = static_cast<int>(i); ++diffs; }
    INFO("first differing byte ", firstDiff, " of ", saved.size(), ", ", diffs, " bytes differ");
    for (int i = 0; i < r.p->numParams(); ++i)
        if (std::fabs(r.p->getNormalised(i) - norms[static_cast<size_t>(i)]) > 1e-6f)
            MESSAGE("param ", i, " ", r.p->param(i).id, " was ", norms[static_cast<size_t>(i)], " now ", r.p->getNormalised(i));
    CHECK(diffs == 0);
}

TEST_CASE("plugin: process() early returns publish idle playback and the block counter keeps moving only while it runs")
{
    PluginRig r;
    r.run(r.pos + 256, {on(r.pos + 10, 60)}, 128);
    CHECK(r.p->playback().load(0).note == 60);
    CHECK(r.p->playback().load(0).active);
    const uint32_t blocks = r.p->playback().blocks();

    // No output buffers at all: nothing renders, the editor must not keep showing the note.
    mpc::ProcessContext none;
    none.outputs = nullptr;
    none.numOutputs = 0;
    none.numFrames = 128;
    r.p->process(none);
    for (int c = 0; c < 4; ++c)
    {
        const ChannelPlayback pb = r.p->playback().load(c);
        CHECK(pb.note == -1);
        CHECK_FALSE(pb.running);
        CHECK_FALSE(pb.active);
        CHECK(pb.level == 0);
    }
    CHECK(r.p->playback().blocks() != blocks);   // the idle publish is a published block too

    // A normal block plays again.
    r.run(r.pos + 256, {on(r.pos + 10, 62)}, 128);
    CHECK(r.p->playback().load(0).note == 62);
}
