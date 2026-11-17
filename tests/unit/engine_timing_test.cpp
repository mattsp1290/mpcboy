// WP6 test 9 (engine end to end) and the Engine-level clock behaviour: the
// tick clock drives MonoVoice through Engine::render, register writes are
// stamped with the APU frame and converted back to host frames by the test.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "doctest/doctest.h"
#include "engine_test_support.h"

using namespace mpcboy;
using namespace mpcboy_test;

namespace {

constexpr uint16_t NR13 = 0xFF13, NR14 = 0xFF14;

struct Transport
{
    double bpm = 120.0;
    bool tempoValid = true;
    bool playing = false;
    bool ppqValid = false;
    double ppqStart = 0.0;   // ppq at host frame 0
};

// Engine with the clock enabled, spy installed, rendering blocks of `block`
// frames from absolute frame 0.
struct ClockRig
{
    std::unique_ptr<Engine> e;
    std::vector<W> log;
    EngineSettings s;
    Transport tr;
    long long pos = 0;

    ClockRig()
    {
        e = std::make_unique<Engine>();
        rackboy_apu_set_write_spy(e->apu(), &ClockRig::spy, this);
    }
    ~ClockRig() { rackboy_apu_set_write_spy(e->apu(), nullptr, nullptr); }
    static void spy(void* u, uint16_t a, uint8_t v)
    {
        auto* r = static_cast<ClockRig*>(u);
        r->log.push_back({r->e->apuFrame(), a, v});
    }

    // `evs` are MIDI only (absolute frames).
    void run(long long frames, std::vector<Ev> evs, int block)
    {
        std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
        std::vector<EngineMidiEvent> midi;
        size_t ei = 0;
        const long long end = pos + frames;
        while (pos < end)
        {
            const int n = static_cast<int>(std::min<long long>(block, end - pos));
            midi.clear();
            while (ei < evs.size() && evs[ei].frame < pos + n)
            {
                EngineMidiEvent m;
                m.offset = static_cast<int>(evs[ei].frame - pos);
                m.status = evs[ei].status; m.data1 = evs[ei].d1; m.data2 = evs[ei].d2;
                midi.push_back(m);
                ++ei;
            }
            EngineBlock b;
            b.midi = midi.empty() ? nullptr : midi.data();
            b.numMidi = static_cast<int>(midi.size());
            b.bpm = tr.bpm; b.tempoValid = tr.tempoValid; b.playing = tr.playing; b.ppqValid = tr.ppqValid;
            b.ppqPos = tr.ppqStart + static_cast<double>(pos) / 44100.0 * tr.bpm / 60.0;
            e->render(b, L.data(), R.data(), n);
            pos += n;
        }
    }
};

double apuToHost(uint64_t apu) { return static_cast<double>(apu) * 147.0 / 160.0; }

}  // namespace

TEST_CASE("engine timing 9: Octave table at 120 BPM, 24/beat: row k's write lands at round(k * 918.75) +- 2 host frames")
{
    const int blocks[5] = {32, 128, 512, 1000, 77};
    for (int blk : blocks)
    {
        CAPTURE(blk);
        ClockRig r;
        r.s.ch[0].tableSlot = 0;
        for (int i = 1; i < 4; ++i) r.s.ch[i].enabled = false;
        r.e->applySettings(r.s);
        setTable(r.e->ownBank().tables[0], octave());
        const long long noteFrame = 0;
        r.run(44100 * 5, {noteOn(noteFrame, 60)}, blk);
        const auto fw = freqWrites(r.log, NR13, NR14);
        // trigger + tick 0 at the note-on, then one write per tick
        REQUIRE(fw.size() > 100);
        CHECK(fw[0].trigger);
        const int row[3] = {0, 12, 0};   // rows after Octave's hop: +12, -12 (back to 0), +12, ...
        double worst = 0.0;
        for (size_t k = 0; k + 1 < fw.size(); ++k)
        {
            // fw[1 + k] is tick k
            const double expected = std::round(static_cast<double>(k) * 918.75) + static_cast<double>(noteFrame);
            worst = std::max(worst, std::fabs(apuToHost(fw[1 + k].apu) - expected));
        }
        std::printf("[engine timing 9] block %d: worst deviation %.2f host frames over %zu ticks\n", blk, worst, fw.size() - 1);
        CHECK(worst <= 2.0);
        // rows: tick 1 is +12, tick 2 is back to +0, then 1, 2, ... alternate
        CHECK(fw[2].freq == rackboy::core::pitch_square_11bit(-12 + 12));
        CHECK(fw[3].freq == rackboy::core::pitch_square_11bit(-12));
        CHECK(fw[4].freq == rackboy::core::pitch_square_11bit(0));
        (void)row;
    }
}

TEST_CASE("engine timing: ticks follow the note-on, not the block grid (note at an odd offset)")
{
    ClockRig r;
    r.s.ch[0].tableSlot = 0;
    for (int i = 1; i < 4; ++i) r.s.ch[i].enabled = false;
    r.e->applySettings(r.s);
    setTable(r.e->ownBank().tables[0], octave());
    r.run(44100 * 3, {noteOn(1234, 60)}, 128);
    const auto fw = freqWrites(r.log, NR13, NR14);
    REQUIRE(fw.size() > 50);
    double worst = 0.0;
    for (size_t k = 0; k + 1 < fw.size(); ++k)
        worst = std::max(worst, std::fabs(apuToHost(fw[1 + k].apu) - (1234.0 + std::round(static_cast<double>(k) * 918.75))));
    CHECK(worst <= 2.0);
}

// In Sync/Grid phase the note owns the grid tick it lands on. MIDI is applied
// before a tick at the same frame, so without this the note's row 0 and the grid
// tick's row 1 would hit the same APU frame and row 0 would never be heard (most
// sequencer notes are quantised onto grid ticks). A grid tick at the note-on
// frame, or within half a period after it, is therefore absorbed as the note's
// own tick 0; the first row 1 comes at the following grid tick, so row 0 sounds
// for about one period.
TEST_CASE("engine timing 7: a grid tick at (or just after) the note-on belongs to row 0; row 1 waits for the next grid tick")
{
    for (int noteFrame : {100, 99, 1000})   // on the tick (tick k = 24 at 100.5), one frame before it, 19 frames before tick k = 25
    {
        CAPTURE(noteFrame);
        ClockRig r;
        r.s.ch[0].tableSlot = 0;
        for (int i = 1; i < 4; ++i) r.s.ch[i].enabled = false;
        r.s.tick.phase = TickPhase::Grid;
        r.e->applySettings(r.s);
        setTable(r.e->ownBank().tables[0], octave());
        r.tr.playing = true;
        r.tr.ppqValid = true;
        r.tr.ppqStart = 1.0 - 100.5 / 22050.0;   // grid tick k = 24 at host frame 100.5, k = 25 at 1019.25
        r.run(2048, {noteOn(noteFrame, 60)}, 2048);
        const auto fw = freqWrites(r.log, NR13, NR14);
        REQUIRE(fw.size() == (noteFrame == 1000 ? 3u : 4u));   // trigger, row 0, row 1 (and row 2 at the grid tick after it, 1938)
        CHECK(fw[0].trigger);
        CHECK(fw[1].freq == rackboy::core::pitch_square_11bit(-12));        // row 0 (tick 0)
        CHECK(fw[2].freq == rackboy::core::pitch_square_11bit(-12 + 12));   // row 1
        const double next = noteFrame == 1000 ? 1019.25 + 918.75 : 1019.25;   // the first grid tick not absorbed
        CHECK(std::fabs(apuToHost(fw[2].apu) - next) <= 2.0);
        CHECK(fw[1].apu < fw[2].apu);
        CHECK(fw[0].apu == fw[1].apu);   // row 0 is written at the trigger, not later
    }
}

TEST_CASE("engine timing: a legato glide does not reset the tick phase; a retrigger does")
{
    for (int legato = 0; legato < 2; ++legato)
    {
        CAPTURE(legato);
        ClockRig r;
        r.s.ch[0].tableSlot = 0;
        for (int i = 1; i < 4; ++i) r.s.ch[i].enabled = false;
        r.s.retrigger = legato ? RetriggerMode::Legato : RetriggerMode::Always;
        r.e->applySettings(r.s);
        setTable(r.e->ownBank().tables[0], octave());
        r.run(44100, {noteOn(0, 60), noteOn(3000, 64)}, 128);
        const auto fw = freqWrites(r.log, NR13, NR14);
        // find the tick writes after frame 3000 + 100 (skipping the glide / retrigger writes at 3000)
        std::vector<double> after;
        for (const auto& w : fw)
        {
            const double h = apuToHost(w.apu);
            if (h > 3100.0) after.push_back(h);
        }
        REQUIRE(after.size() > 5);
        if (legato)
        {
            // phase unchanged: still k * 918.75 (k = 4: 3675, 5: 4593.75, ...)
            CHECK(std::fabs(after[0] - 3675.0) <= 2.0);
            CHECK(std::fabs(after[1] - 4593.75) <= 2.0);
        }
        else
        {
            // restarted at 3000: first tick one period later
            CHECK(std::fabs(after[0] - (3000.0 + 918.75)) <= 2.0);
        }
    }
}

TEST_CASE("engine timing: tempo change doubles the table rate (S5 through the engine)")
{
    double interval[3];
    const double bpms[3] = {60.0, 120.0, 240.0};
    for (int i = 0; i < 3; ++i)
    {
        ClockRig r;
        r.tr.bpm = bpms[i];
        r.s.ch[0].tableSlot = 0;
        for (int c = 1; c < 4; ++c) r.s.ch[c].enabled = false;
        r.e->applySettings(r.s);
        setTable(r.e->ownBank().tables[0], octave());
        r.run(44100 * 10, {noteOn(0, 60)}, 128);
        const auto fw = freqWrites(r.log, NR13, NR14);
        REQUIRE(fw.size() > 20);
        const size_t last = fw.size() - 1;
        interval[i] = (apuToHost(fw[last].apu) - apuToHost(fw[2].apu)) / static_cast<double>(last - 2);
    }
    std::printf("[engine timing S5] table row interval at 60/120/240 BPM: %.2f %.2f %.2f host frames\n", interval[0], interval[1], interval[2]);
    CHECK(std::fabs(interval[1] - interval[2] * 2.0) <= 1.0);
    CHECK(std::fabs(interval[0] - interval[2] * 4.0) <= 1.0);
}
