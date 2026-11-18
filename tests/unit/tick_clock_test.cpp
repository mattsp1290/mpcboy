// WP6 tests 1-8 (TickClock output in host frames) plus label and cap checks.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "doctest/doctest.h"
#include "tick_clock.h"

using namespace mpcboy;

namespace {

constexpr double kSr = 44100.0;
const int kBlockSizes[7] = {32, 64, 96, 128, 192, 256, 512};

// Collects absolute tick frames; the note-on lands in the first block at `noteOffset`.
struct Harness
{
    TickClock clock;
    long long pos = 0;
    std::vector<double> ticks;       // absolute host frames (integer positions)
    std::mt19937 rng{12345};

    int nextBlock() { return kBlockSizes[rng() % 7]; }

    void block(int frames, double bpm, bool tempoValid, double ppq = 0.0, bool ppqValid = false, bool playing = false,
               int noteOffset = -1)
    {
        TickBlockInput in;
        in.frames = frames; in.bpm = bpm; in.tempoValid = tempoValid;
        in.ppqPos = ppq; in.ppqValid = ppqValid; in.playing = playing;
        clock.beginBlock(in);
        if (noteOffset >= 0) clock.noteOn(noteOffset);
        while (clock.peek() >= 0)
        {
            ticks.push_back(static_cast<double>(pos + clock.peek()));
            clock.pop();
        }
        clock.endBlock();
        pos += frames;
    }
};

}  // namespace

TEST_CASE("tick clock 1: constant tempo, tick k within 1 frame of k * 44100 * 60 / (bpm * tpb) for 10 minutes")
{
    const double bpms[5] = {40, 60, 120, 240, 300};
    const int tpbs[2] = {24, 96};
    for (double bpm : bpms)
        for (int tpb : tpbs)
        {
            CAPTURE(bpm); CAPTURE(tpb);
            Harness h;
            h.clock.setSettings(TickMode::Sync, tpb, TickPhase::Note, 59.73);
            const long long total = static_cast<long long>(600 * kSr);
            const double period = kSr * 60.0 / (bpm * tpb);
            // first block: note-on at offset 17
            const long long noteFrame = 17;
            h.block(128, bpm, true, 0, false, false, 17);
            while (h.pos < total) h.block(h.nextBlock(), bpm, true);
            const double expectedCount = std::floor((static_cast<double>(h.pos) - noteFrame - 1e-9) / period);
            REQUIRE(h.ticks.size() >= static_cast<size_t>(expectedCount) - 1);
            double worst = 0.0;
            for (size_t i = 0; i < h.ticks.size(); ++i)
            {
                const double exact = static_cast<double>(noteFrame) + static_cast<double>(i + 1) * period;
                worst = std::max(worst, std::fabs(h.ticks[i] - exact));
            }
            CHECK(worst <= 1.0 + 1e-6);
            CHECK(static_cast<double>(h.ticks.size()) >= expectedCount - 1);
            CHECK(static_cast<double>(h.ticks.size()) <= expectedCount + 1);
        }
}

TEST_CASE("tick clock 2: doubling the tempo halves the tick interval (60 / 120 / 240 BPM)")
{
    double mean[3];
    const double bpms[3] = {60, 120, 240};
    for (int i = 0; i < 3; ++i)
    {
        Harness h;
        h.block(128, bpms[i], true, 0, false, false, 0);
        while (h.pos < static_cast<long long>(60 * kSr)) h.block(h.nextBlock(), bpms[i], true);
        REQUIRE(h.ticks.size() > 100);
        mean[i] = (h.ticks.back() - h.ticks.front()) / static_cast<double>(h.ticks.size() - 1);
    }
    std::printf("[tick clock 2] mean interval 60/120/240 BPM: %.3f %.3f %.3f frames\n", mean[0], mean[1], mean[2]);
    CHECK(std::fabs(mean[1] - mean[2] * 2.0) <= 1.0);
    CHECK(std::fabs(mean[0] - mean[2] * 4.0) <= 1.0);
    CHECK(std::fabs(mean[1] - 918.75) <= 1.0);
}

TEST_CASE("tick clock 3: tempo change mid-note keeps the phase fraction, no double or missing tick")
{
    Harness h;
    double bpm = 120.0;
    h.block(128, bpm, true, 0, false, false, 0);
    const long long changeAt = 128 * 70;     // a block boundary: the new tempo arrives with that block
    while (h.pos < changeAt) h.block(128, bpm, true);
    REQUIRE(h.pos == changeAt);
    // predicted: ticks at k * 918.75 up to the change, then the remaining distance doubles
    double nextOld = 918.75;
    while (nextOld < static_cast<double>(changeAt)) nextOld += 918.75;   // first old-tempo tick at or after the change
    std::vector<double> expect;
    for (double t = 918.75; t < static_cast<double>(changeAt); t += 918.75) expect.push_back(t);
    double t = static_cast<double>(changeAt) + (nextOld - static_cast<double>(changeAt)) * 2.0;
    bpm = 60.0;
    while (h.pos < 128 * 400) h.block(128, bpm, true);
    for (; t < static_cast<double>(h.pos); t += 1837.5) expect.push_back(t);
    REQUIRE(h.ticks.size() == expect.size());   // none missing, none doubled
    for (size_t i = 0; i < expect.size(); ++i) CHECK(std::fabs(h.ticks[i] - expect[i]) <= 1.0);
    // intervals: before == 918.75, after == 1837.5 (within a frame)
    for (size_t i = 1; i < h.ticks.size(); ++i)
    {
        const double d = h.ticks[i] - h.ticks[i - 1];
        if (h.ticks[i] < static_cast<double>(changeAt)) CHECK(std::fabs(d - 918.75) <= 1.0);
        else if (h.ticks[i - 1] > nextOld + 2.0 * 0.0 + (nextOld - static_cast<double>(changeAt)) * 1.0 + 1.0) CHECK(std::fabs(d - 1837.5) <= 1.0);
    }
}

TEST_CASE("tick clock 4: grid ticks land on ppq = k / ticksPerBeat within 1 frame")
{
    const double bpms[3] = {120.0, 97.3, 233.0};
    const int tpbs[3] = {24, 96, 12};
    for (int c = 0; c < 3; ++c)
    {
        CAPTURE(c);
        Harness h;
        const double bpm = bpms[c];
        const int tpb = tpbs[c];
        h.clock.setSettings(TickMode::Sync, tpb, TickPhase::Grid, 59.73);
        while (h.pos < static_cast<long long>(300 * kSr))
        {
            const double ppq = static_cast<double>(h.pos) / kSr * bpm / 60.0;
            h.block(h.nextBlock(), bpm, true, ppq, true, true);
        }
        REQUIRE(h.ticks.size() > 100);
        double worst = 0.0;
        for (size_t k = 0; k < h.ticks.size(); ++k)
        {
            const double exact = static_cast<double>(k) / tpb * 60.0 / bpm * kSr;   // tick k <-> ppq k/tpb
            worst = std::max(worst, std::fabs(h.ticks[k] - exact));
        }
        CHECK(worst <= 1.0 + 1e-6);
    }
}

TEST_CASE("tick clock 5: a loop jump emits at most one tick, at the grid position after the new ppq")
{
    Harness h;
    h.clock.setSettings(TickMode::Sync, 24, TickPhase::Grid, 59.73);
    const double bpm = 120.0;
    // play up to ppq 7.99, one block there, then jump to 0.0
    long long frame = 0;
    for (; ; frame += 128)
    {
        const double ppq = static_cast<double>(frame) / kSr * bpm / 60.0;
        if (ppq >= 7.98) break;
        h.block(128, bpm, true, ppq, true, true);
    }
    h.ticks.clear();
    h.block(128, bpm, true, 7.99, true, true);   // may contain the tick at ppq 8.0
    const size_t before = h.ticks.size();
    CHECK(before <= 1);
    h.ticks.clear();
    const long long jumpPos = h.pos;
    h.block(128, bpm, true, 0.0, true, true);
    REQUIRE(h.ticks.size() <= 1);
    if (!h.ticks.empty())
        CHECK(std::fabs(h.ticks[0] - static_cast<double>(jumpPos) - 0.0) <= 1.0);   // grid tick 0 at ppq 0.0
    // continue a few blocks: spacing is the grid again (no burst)
    h.ticks.clear();
    for (int i = 1; i < 100; ++i)
        h.block(128, bpm, true, i * 128.0 / kSr * bpm / 60.0, true, true);
    for (size_t i = 1; i < h.ticks.size(); ++i) CHECK(std::fabs(h.ticks[i] - h.ticks[i - 1] - 918.75) <= 1.0);

    // a jump to ppq 3.53 with a 512 block: the skipped ticks are not emitted, exactly one tick follows
    Harness g;
    g.clock.setSettings(TickMode::Sync, 24, TickPhase::Grid, 59.73);
    g.block(512, bpm, true, 10.0, true, true);
    g.ticks.clear();
    const long long p0 = g.pos;
    g.block(512, bpm, true, 3.53, true, true);
    REQUIRE(g.ticks.size() == 1);
    const double exact = (85.0 / 24.0 - 3.53) * 0.5 * kSr;   // next grid tick k = 85
    CHECK(std::fabs(g.ticks[0] - static_cast<double>(p0) - exact) <= 1.0);
}

TEST_CASE("tick clock 6: an invalid tempo uses the last valid one, 120 from a cold start")
{
    Harness h;
    h.block(128, 0.0, false, 0, false, false, 0);
    CHECK(h.clock.bpmUsed() == 120.0);
    CHECK(std::fabs(h.clock.periodFrames() - 918.75) < 1e-9);
    h.block(128, 60.0, true);
    CHECK(h.clock.bpmUsed() == 60.0);
    h.block(128, 200.0, false);   // not valid: keep 60
    CHECK(h.clock.bpmUsed() == 60.0);
    h.block(128, 0.5, true);      // out of range even if flagged valid
    CHECK(h.clock.bpmUsed() == 60.0);
    h.block(128, 1000.0, true);
    CHECK(h.clock.bpmUsed() == 60.0);
    h.block(128, 90.0, true);
    CHECK(h.clock.bpmUsed() == 90.0);

    // spacing after a cold start with invalid tempo is the 120 BPM spacing
    Harness c;
    c.block(128, 77.0, false, 0, false, false, 0);
    while (c.pos < 44100 * 10) c.block(128, 77.0, false);
    for (size_t i = 1; i < c.ticks.size(); ++i) CHECK(std::fabs(c.ticks[i] - c.ticks[i - 1] - 918.75) <= 1.0);
}

TEST_CASE("tick clock 7: a note-on retriggers the phase; a grid tick at the note-on is absorbed by the note")
{
    // Note phase: the first tick after a note-on is one period later, whatever ran before.
    Harness h;
    h.block(128, 120.0, true, 0, false, false, 0);
    h.block(128, 120.0, true);
    h.ticks.clear();
    h.block(1024, 120.0, true, 0, false, false, 100);   // retrigger at offset 100
    REQUIRE(h.ticks.size() >= 1);
    CHECK(std::fabs(h.ticks[0] - (static_cast<double>(h.pos - 1024) + 100 + 918.75)) <= 1.0);

    // Grid phase: the schedule stays on the grid, but the note owns the grid tick it lands on
    // (at the note-on frame or up to half a period after it): that tick is consumed, so row 0 is
    // heard for about one period instead of being overwritten at once by row 1.
    const double ppq0 = 1.0 - 100.5 / 22050.0;   // grid tick k = 24 at host frame 100.5, k = 25 at 1019.25, k = 26 at 1938
    struct Case { int noteOffset; double firstTick; size_t count; };
    // The note on the tick, and one frame before it (the engine's ordering test covers a note
    // well after earlier ticks were consumed: this harness calls noteOn before consuming any).
    for (const Case& k : {Case{100, 1019.0, 2}, Case{99, 1019.0, 2}})
    {
        CAPTURE(k.noteOffset);
        Harness g;
        g.clock.setSettings(TickMode::Sync, 24, TickPhase::Grid, 59.73);
        g.block(2048, 120.0, true, ppq0, true, true, k.noteOffset);
        REQUIRE(g.ticks.size() == k.count);
        CHECK(g.ticks[0] == k.firstTick);
    }
}

TEST_CASE("tick clock 8: Free runs at 59.73 Hz, ignores the transport, and restarts on a note-on")
{
    Harness h;
    h.clock.setSettings(TickMode::Free, 24, TickPhase::Note, 59.73);
    h.block(128, 120.0, true, 0, false, false, 0);
    std::mt19937 r(7);
    while (h.pos < static_cast<long long>(60 * kSr))
        h.block(h.nextBlock(), 40.0 + static_cast<double>(r() % 260), true, 0.0, true, true);
    std::printf("[tick clock 8] Free 59.73 Hz for 60 s: %zu ticks (+ the note-on tick 0)\n", h.ticks.size());
    CHECK(h.ticks.size() + 1 >= 3583);
    CHECK(h.ticks.size() + 1 <= 3585);
    for (size_t i = 1; i < h.ticks.size(); ++i) CHECK(std::fabs(h.ticks[i] - h.ticks[i - 1] - kSr / 59.73) <= 1.0);
}

TEST_CASE("tick clock: ticks-per-beat labels, and at most 64 ticks per block")
{
    CHECK(kNumTickChoices == 10);
    CHECK(kTicksPerBeatChoices[kDefaultTickChoice] == 24);
    CHECK(std::string(kTickChoiceLabels[kDefaultTickChoice]) == "24/beat");
    CHECK(std::string(kTickChoiceLabels[0]) == "96/beat");
    CHECK(std::string(kTickChoiceLabels[9]) == "3/beat");
    for (int i = 1; i < kNumTickChoices; ++i) CHECK(kTicksPerBeatChoices[i] < kTicksPerBeatChoices[i - 1]);

    TickClock c;
    c.setSettings(TickMode::Sync, 96, TickPhase::Note, 59.73);
    int out[200];
    TickBlockInput warm; warm.frames = 32; warm.bpm = 998.0; warm.tempoValid = true;
    c.beginBlock(warm); c.noteOn(0); c.endBlock();
    TickBlockInput in; in.frames = 4096; in.bpm = 998.0; in.tempoValid = true;
    const int n = c.generate(in, out, 200);
    CHECK(n == TickClock::kMaxTicksPerBlock);
    CHECK(c.droppedTicks() > 0);
    for (int i = 1; i < n; ++i) CHECK(out[i] > out[i - 1]);
    // the phase keeps going afterwards
    TickBlockInput next; next.frames = 128; next.bpm = 998.0; next.tempoValid = true;
    CHECK(c.generate(next, out, 200) > 0);
}
