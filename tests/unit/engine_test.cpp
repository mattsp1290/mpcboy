// WP4 tests 1-11 (test 12 is the whole vendored suite, run by ctest) plus
// a few extra engine tests (note stack, APU frame mapping, MIDI/tick order).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "engine_test_support.h"
#include "instrument_encoder.h"
#include "note_stack.h"
#include "rackboy_table.h"

using namespace mpcboy;
using namespace mpcboy_test;
using rackboy::core::kBaseMidiNote;

namespace {

constexpr uint16_t NR13 = 0xFF13, NR14 = 0xFF14, NR23 = 0xFF18, NR24 = 0xFF19, NR33 = 0xFF1D, NR34 = 0xFF1E,
                   NR43 = 0xFF22, NR44 = 0xFF23;

double hzOfSquare(uint16_t x) { return 131072.0 / (2048.0 - x); }
double centsOfSquare(uint16_t x) { return 1200.0 * std::log2(hzOfSquare(x) / 523.2511306011972); }

// The vendored vibrato shaper, restated from its documentation (V command):
// triangle in [-128, +128], scaled by depth / 128 with truncation.
int vibratoSemis(uint8_t phase, uint8_t depth)
{
    if (depth == 0) return 0;
    int tri;
    if (phase < 64) tri = phase * 2;
    else if (phase < 128) tri = (128 - phase) * 2;
    else if (phase < 192) tri = -(phase - 128) * 2;
    else tri = -(256 - phase) * 2;
    return tri * depth / 128;
}

// Last sample index with |x[n] - x[n-1]| above `thr`, or -1.
long lastEdge(const std::vector<float>& v, size_t from, float thr)
{
    long last = -1;
    for (size_t i = std::max<size_t>(from, 1); i < v.size(); ++i)
        if (std::fabs(v[i] - v[i - 1]) > thr) last = static_cast<long>(i);
    return last;
}

// Rising zero crossings with linear interpolation; returns frequency in Hz.
double zeroCrossHz(const std::vector<float>& v, size_t from, size_t to)
{
    double first = -1, last = -1;
    int n = 0;
    for (size_t i = from + 1; i < to; ++i)
    {
        if (v[i - 1] < 0.0f && v[i] >= 0.0f)
        {
            const double t = static_cast<double>(i - 1) + (0.0 - v[i - 1]) / (static_cast<double>(v[i]) - v[i - 1]);
            if (first < 0) first = t;
            last = t;
            ++n;
        }
    }
    return n < 2 ? 0.0 : (n - 1) * 44100.0 / (last - first);
}

}  // namespace

// ---------------------------------------------------------------------------
TEST_CASE("engine 1: note-on triggers NR14, NR24, NR34 and NR44 at the same APU frame")
{
    Rig r;
    r.apply();
    r.run(400, {noteOn(150, 60)});
    uint64_t frame[4] = {~0ull, ~0ull, ~0ull, ~0ull};
    int count[4] = {0, 0, 0, 0};
    const uint16_t addrs[4] = {NR14, NR24, NR34, NR44};
    for (const W& w : r.log)
        for (int c = 0; c < 4; ++c)
            if (w.addr == addrs[c] && (w.v & 0x80)) { frame[c] = w.apu; ++count[c]; }
    for (int c = 0; c < 4; ++c)
    {
        CAPTURE(c);
        CHECK(count[c] == 1);
    }
    CHECK(frame[0] == frame[1]);
    CHECK(frame[0] == frame[2]);
    CHECK(frame[0] == frame[3]);
    // host frame 150 -> APU frame round(150 * 160 / 147) = 163 (block 0 starts at APU frame 0)
    CHECK(frame[0] == 163);
}

TEST_CASE("engine 2: legato glide keeps the table cursor and adds no trigger; Always restarts the table")
{
    for (int mode = 0; mode < 2; ++mode)
    {
        const bool legato = mode == 0;
        CAPTURE(legato);
        Rig r;
        setTable(r.e->ownBank().tables[0], majArp());
        r.only(0);
        r.s.ch[0].tableSlot = 0;
        r.s.retrigger = legato ? RetriggerMode::Legato : RetriggerMode::Always;
        r.apply();

        // note 60 at 0; ticks 1 (row 1: +4) and 2 (row 2: +3); note 62 at 3500; tick at 4500.
        r.run(3400, {noteOn(0, 60), tickAt(1000), tickAt(2000)});
        const size_t mark = r.log.size();
        r.run(2000, {noteOn(100, 62), tickAt(1100)});   // 3500 and 4500 absolute
        const auto fw = freqWrites(r.log, NR13, NR14, mark);
        REQUIRE(fw.size() >= 2);
        if (legato)
        {
            // glide: note 62 + accumulated transposes (+0 +4 +3), no trigger bit
            CHECK(fw[0].freq == rackboy::core::pitch_square_11bit(62 - kBaseMidiNote + 0 + 4 + 3));
            CHECK_FALSE(fw[0].trigger);
            // the next row continues from the same cursor: row 3 (-7)
            CHECK(fw[1].freq == rackboy::core::pitch_square_11bit(62 - kBaseMidiNote + 4 + 3 - 7));
            CHECK_FALSE(fw[1].trigger);
        }
        else
        {
            // retrigger: trigger bit again, tick 0 applies row 0, the next tick row 1
            CHECK(fw[0].trigger);
            CHECK(fw[0].freq == rackboy::core::pitch_square_11bit(62 - kBaseMidiNote));
            REQUIRE(fw.size() >= 3);
            CHECK(fw[1].freq == rackboy::core::pitch_square_11bit(62 - kBaseMidiNote));   // tick 0 (row 0, +0)
            CHECK_FALSE(fw[1].trigger);
            CHECK(fw[2].freq == rackboy::core::pitch_square_11bit(62 - kBaseMidiNote + 4));   // row 1
        }
    }
}

TEST_CASE("engine 3: each channel alone is audible, all four off is silent")
{
    for (int ch = 0; ch <= 4; ++ch)
    {
        CAPTURE(ch);
        Rig r;
        if (ch < 4) r.only(ch);
        else for (int i = 0; i < 4; ++i) r.s.ch[i].enabled = false;
        r.apply();
        r.run(22050 + 2000, {noteOn(0, 60)});
        const double db = rmsDb(r.L, 2000, r.L.size());
        std::printf("[engine 3] channel %d alone: %.2f dBFS\n", ch, db);
        if (ch < 4) CHECK(db > -40.0);
        else CHECK(db < -90.0);
    }
}

TEST_CASE("engine 4: four channels with four tables follow their own table")
{
    Rig r;
    auto& tb = r.e->ownBank().tables;
    setTable(tb[0], majArp());
    setTable(tb[1], minArp());
    setTable(tb[2], octave());
    setTable(tb[3], {{0, 0, 0, 0, 0}, {2, 0, 0, 0, 0}, {5, 0, 0, 0, 0}, {-7, 'H', 1, 0, 0}});
    for (int c = 0; c < 4; ++c) r.s.ch[c].tableSlot = static_cast<int8_t>(c);
    r.apply();
    std::vector<Ev> evs = {noteOn(0, 60)};
    constexpr int kTicks = 9;
    for (int k = 1; k <= kTicks; ++k) evs.push_back(tickAt(k * 600));
    r.run(kTicks * 600 + 300, evs);

    const std::vector<std::vector<Row>> rows = {majArp(), minArp(), octave(),
                                                {{0, 0, 0, 0, 0}, {2, 0, 0, 0, 0}, {5, 0, 0, 0, 0}, {-7, 'H', 1, 0, 0}}};
    for (int c = 0; c < 4; ++c)
    {
        CAPTURE(c);
        // expected pitch offsets: trigger, then rows 0.. (tick 0 at note-on, then one per tick)
        std::vector<int> expectOff;
        int off = 60 - kBaseMidiNote, cursor = 0;
        expectOff.push_back(off);   // trigger write
        for (int t = 0; t <= kTicks; ++t)
        {
            const Row& row = rows[static_cast<size_t>(c)][static_cast<size_t>(cursor)];
            off += row.tr;
            expectOff.push_back(off);
            cursor = row.c1 == 'H' ? row.p1 : cursor + 1;
        }
        if (c < 3)
        {
            const uint16_t lo[3] = {NR13, NR23, NR33}, hi[3] = {NR14, NR24, NR34};
            const auto fw = freqWrites(r.log, lo[c], hi[c]);
            REQUIRE(fw.size() == expectOff.size());
            for (size_t i = 0; i < fw.size(); ++i)
            {
                const uint16_t want = c == 2 ? rackboy::core::pitch_wave_11bit(static_cast<int16_t>(expectOff[i]))
                                             : rackboy::core::pitch_square_11bit(static_cast<int16_t>(expectOff[i]));
                CHECK(fw[i].freq == want);
            }
        }
        else
        {
            std::vector<uint8_t> nr43;
            for (const W& w : r.log) if (w.addr == NR43) nr43.push_back(w.v);
            REQUIRE(nr43.size() == expectOff.size());
            for (size_t i = 0; i < nr43.size(); ++i)
                CHECK(nr43[i] == rackboy::core::pitch_noise_nr43(static_cast<int16_t>(expectOff[i])));
        }
    }
}

TEST_CASE("engine 5: Note Off = Cut silences the output within 10 ms plus the resampler delay")
{
    for (int ch = 0; ch < 4; ++ch)
    {
        CAPTURE(ch);
        Rig r;
        r.only(ch);
        r.apply();
        const long offFrame = 8000;
        r.run(offFrame + 8000, {noteOn(0, 60), noteOff(offFrame, 60)});
        // last frame whose magnitude exceeds -90 dBFS
        long last = -1;
        for (size_t i = 0; i < r.L.size(); ++i)
            if (std::fabs(r.L[i]) > 3.1623e-5f) last = static_cast<long>(i);
        const double afterMs = static_cast<double>(last - offFrame) / 44.1;
        std::printf("[engine 5] channel %d: last sample above -90 dBFS is %.2f ms after the note-off\n", ch, afterMs);
        // 10 ms + resampler group delay (~2.7 ms)
        CHECK(afterMs < 10.0 + 128.0 * 147.0 / 160.0 / 44.1);
    }
}

TEST_CASE("engine 6: cents pitch functions agree with the semitone functions")
{
    using namespace rackboy::core;
    for (int n = -36; n <= 36; ++n)
    {
        CAPTURE(n);
        CHECK(pitch_square_11bit_cents(n * 100) == pitch_square_11bit(static_cast<int16_t>(n)));
        CHECK(pitch_wave_11bit_cents(n * 100) == pitch_wave_11bit(static_cast<int16_t>(n)));
    }
    // monotone in cents across a wide range
    uint16_t prev = 0;
    for (int c = -3000; c <= 3000; c += 7)
    {
        const uint16_t v = pitch_square_11bit_cents(c);
        CHECK(v >= prev);
        prev = v;
    }
}

TEST_CASE("engine 7: pitch bend +8191 with range 2 raises Pulse 1 by 200 +- 10 cents")
{
    auto measure = [](bool withBend) {
        Rig r;
        r.only(0);
        r.s.bendRange = 2;
        r.apply();
        std::vector<Ev> evs = {noteOn(0, 72)};
        if (withBend) evs.push_back(bend(10, 8191));
        r.run(4410 + 22050, evs);
        return zeroCrossHz(r.L, 4410, 4410 + 22050);
    };
    const double f0 = measure(false), f1 = measure(true);
    const double cents = 1200.0 * std::log2(f1 / f0);
    std::printf("[engine 7] f0 %.2f Hz, f1 %.2f Hz, bend %.2f cents\n", f0, f1, cents);
    CHECK(f0 > 500.0);
    CHECK(cents == doctest::Approx(200.0).epsilon(0.0).scale(1.0).epsilon(10.0 / 200.0));
}

TEST_CASE("engine 8: bend, fine tune, vibrato and slide compose as documented")
{
    Rig r;
    // row 0: V 4,2 ; row 1: +7 with L 04 (slide from the previous pitch to +7 over 4 ticks); rest empty
    setTable(r.e->ownBank().tables[0],
             {{0, 'V', 0x42, 0, 0}, {7, 'L', 4, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
              {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}});
    r.only(0);
    r.s.ch[0].tableSlot = 0;
    r.s.ch[0].fineCents = 23;
    r.s.bendRange = 2;
    r.apply();

    int vibSeen = 0, slideSeen = 0, bendSeen = 0, checks = 0;
    auto check = [&]() {
        const auto fw = freqWrites(r.log, NR13, NR14);
        REQUIRE(!fw.empty());
        const auto& rg = r.e->voice().regs(0);
        const auto& vs = r.e->voice().voice(0);
        const int vib = vs.vibrato_speed ? vibratoSemis(vs.vibrato_phase, vs.vibrato_depth) : 0;
        const double expect = (rg.pitch_offset + vib) * 100.0 + 23.0 + r.e->voice().bendCents();
        const double got = centsOfSquare(fw.back().freq);
        CAPTURE(rg.pitch_offset);
        CAPTURE(vib);
        CAPTURE(r.e->voice().bendCents());
        CHECK(std::fabs(got - expect) <= 5.0);
        vibSeen += vib != 0;
        slideSeen += vs.slide_ticks_remaining > 0;
        bendSeen += r.e->voice().bendCents() != 0;
        ++checks;
    };
    r.run(100, {noteOn(10, 55)});
    check();
    for (int k = 1; k <= 14; ++k)
    {
        if (k == 5) { r.run(100, {bend(10, 4096)}); check(); }
        if (k == 9) { r.run(100, {bend(10, -8192)}); check(); }
        if (k == 12) { r.run(100, {bend(10, 0)}); check(); }
        r.run(500, {tickAt(10)});
        check();
    }
    CHECK(checks == 18);
    CHECK(vibSeen > 0);
    CHECK(slideSeen > 0);
    CHECK(bendSeen > 0);
}

TEST_CASE("engine 9: a table E retrigger after the note's Length expired sounds for Length again, not for the hardware maximum")
{
    constexpr long kDelay = 118;   // 44.1 kHz resampler group delay (117.6 host frames)
    Rig r;
    setTable(r.e->ownBank().tables[0], {{0, 0, 0, 0, 0}, {0, 'E', 0xF0, 0, 0}});
    r.only(0);
    r.s.ch[0].length = 4;   // 4/256 s = 15.6 ms
    r.s.ch[0].tableSlot = 0;
    r.apply();
    r.run(22050, {noteOn(0, 60), tickAt(13230)});   // the retrigger at 300 ms, long after the first sound ended
    const long end = lastEdge(r.L, 0, 0.005f);
    const double ms = static_cast<double>(end - kDelay) / 44.1;
    std::printf("[engine 9] E retrigger at 300 ms with length 4: sound ends at %.2f ms\n", ms);
    CHECK(ms == doctest::Approx(300.0 + 15.6).epsilon(10.0 / 315.6));
}

TEST_CASE("engine 9: length silences Pulse 1 after 62.5 ms and Wave after 250 ms")
{
    constexpr long kDelay = 118;   // 44.1 kHz resampler group delay (117.6), host frames
    auto pulseEnd = [&](int variant) {
        Rig r;
        setTable(r.e->ownBank().tables[0], octave());
        r.only(0);
        r.s.ch[0].length = 16;
        r.s.ch[0].tableSlot = variant == 1 ? 0 : -1;
        r.apply();
        std::vector<Ev> evs = {noteOn(0, 60)};
        if (variant == 1) for (int k = 1; k < 8; ++k) evs.push_back(tickAt(k * 441));
        if (variant == 2) evs.push_back(bend(100, 4096));
        r.run(22050, evs);
        // the length bit must ride on every NR14 write
        for (const auto& f : freqWrites(r.log, NR13, NR14)) CHECK(f.lengthBit);
        return lastEdge(r.L, 0, 0.005f);
    };
    for (int variant = 0; variant < 3; ++variant)
    {
        const long end = pulseEnd(variant);
        const double ms = static_cast<double>(end - kDelay) / 44.1;
        std::printf("[engine 9] pulse length 16, case %d: sound ends at %.2f ms\n", variant, ms);
        CHECK(ms == doctest::Approx(62.5).epsilon(5.0 / 62.5));
    }

    Rig r;
    r.only(2);
    r.s.ch[2].length = 16;
    r.apply();
    r.run(30000, {noteOn(0, 60)});
    const long end = lastEdge(r.L, 0, 0.002f);
    const double ms = static_cast<double>(end - kDelay) / 44.1;
    std::printf("[engine 9] wave length 16: sound ends at %.2f ms\n", ms);
    CHECK(ms == doctest::Approx(250.0).epsilon(10.0 / 250.0));

    Rig n;
    n.only(3);
    n.s.ch[3].length = 16;
    n.apply();
    n.run(22050, {noteOn(0, 60)});
    const long nend = lastEdge(n.L, 0, 0.002f);
    const double nms = static_cast<double>(nend - kDelay) / 44.1;
    std::printf("[engine 9] noise length 16: sound ends at %.2f ms\n", nms);
    CHECK(nms == doctest::Approx(62.5).epsilon(5.0 / 62.5));
}

TEST_CASE("engine 10: noise Track follows the note and keeps the width bit; Fixed ignores the table")
{
    {
        Rig r;
        r.only(3);
        r.s.ch[3].noiseWidth7 = true;
        r.apply();
        std::vector<Ev> evs;
        for (int n = 60; n <= 84; ++n) evs.push_back(noteOn((n - 60) * 300, n));
        r.run(26 * 300, evs);
        std::vector<uint8_t> v;
        for (const W& w : r.log) if (w.addr == NR43) v.push_back(w.v);
        REQUIRE(v.size() == 25);
        static const int kDiv[8] = {8, 16, 32, 48, 64, 80, 96, 112};
        double prev = 0.0;
        for (size_t i = 0; i < v.size(); ++i)
        {
            CAPTURE(i);
            CHECK((v[i] & 0x08) == 0x08);
            const double f = 524288.0 / (kDiv[v[i] & 7] * std::pow(2.0, (v[i] >> 4) + 1));
            CHECK(f >= prev);
            prev = f;
        }
        CHECK(prev > 524288.0 / (112 * std::pow(2.0, 14)));
    }
    {
        Rig r;
        setTable(r.e->ownBank().tables[0], {{0, 0, 0, 0, 0}, {5, 0, 0, 0, 0}, {-3, 'V', 0x44, 0, 0}, {9, 'H', 1, 0, 0}});
        r.only(3);
        r.s.ch[3].noisePitchMode = NoisePitchMode::Fixed;
        r.s.ch[3].noiseFixedShape = 40;   // shift 5, divisor code 0
        r.s.ch[3].tableSlot = 0;
        r.apply();
        std::vector<Ev> evs = {noteOn(0, 60), bend(50, 5000)};
        for (int k = 1; k <= 8; ++k) evs.push_back(tickAt(k * 400));
        r.run(4000, evs);
        std::vector<uint8_t> v;
        for (const W& w : r.log) if (w.addr == NR43) v.push_back(w.v);
        REQUIRE(v.size() == 1);   // the trigger only: transposes, vibrato and bend never rewrite NR43
        CHECK(v[0] == 0x50);
    }
}

TEST_CASE("engine 11: pan L sends Pulse 1 to the left channel only")
{
    Rig r;
    r.only(0);
    r.s.ch[0].pan = Pan::L;
    r.apply();
    r.run(22050, {noteOn(0, 60)});
    const double l = rmsDb(r.L, 2000, r.L.size()), rr = rmsDb(r.R, 2000, r.R.size());
    std::printf("[engine 11] pan L: left %.2f dBFS, right %.2f dBFS\n", l, rr);
    CHECK(rr < -90.0);
    CHECK(l > -40.0);
}

// ---------------------------------------------------------------------------
// extras

TEST_CASE("note stack: last note priority, 17th note replaces the oldest, one voice only")
{
    NoteStack st;
    for (int n = 40; n < 56; ++n) st.push(n);
    CHECK(st.size() == 16);
    st.push(99);
    CHECK(st.size() == 16);
    CHECK(st.top() == 99);
    CHECK_FALSE(st.remove(40));   // the oldest was dropped
    CHECK(st.remove(99));
    CHECK(st.top() == 55);
    st.push(50);                  // moves an existing note to the top
    CHECK(st.top() == 50);
    CHECK(st.size() == 15);
    st.clear();
    CHECK(st.empty());

    // Overlapping notes do not add voices: legato second note writes no trigger bit.
    Rig r;
    r.s.retrigger = RetriggerMode::Legato;
    r.apply();
    r.run(2000, {noteOn(0, 60), noteOn(500, 64), noteOff(1000, 64), noteOff(1500, 60)});
    size_t triggers = 0;
    for (const W& w : r.log) triggers += (w.addr == NR14 && (w.v & 0x80));
    CHECK(triggers == 1);
    // releasing the top note glides back to 60: last NR13/14 write is note 60 again
    const auto fw = freqWrites(r.log, NR13, NR14);
    REQUIRE(fw.size() >= 3);
    CHECK(fw[2].freq == rackboy::core::pitch_square_11bit(60 - kBaseMidiNote));
}

TEST_CASE("engine: Note Off = Ring keeps ticking, Cut stops the tables")
{
    for (int ring = 0; ring < 2; ++ring)
    {
        Rig r;
        setTable(r.e->ownBank().tables[0], majArp());
        r.only(0);
        r.s.ch[0].tableSlot = 0;
        r.s.noteOff = ring ? NoteOffMode::Ring : NoteOffMode::Cut;
        r.apply();
        r.run(2000, {noteOn(0, 60), noteOff(500, 60), tickAt(1000), tickAt(1500)});
        const auto fw = freqWrites(r.log, NR13, NR14);
        // trigger + tick 0 always; two later ticks only in Ring mode
        CHECK(fw.size() == (ring ? 4u : 2u));
    }
}

TEST_CASE("engine: MIDI is applied before a tick at the same offset; events map to APU frame round(t*160/147)")
{
    Rig r;
    setTable(r.e->ownBank().tables[0], majArp());
    r.only(0);
    r.s.ch[0].tableSlot = 0;
    r.apply();
    r.run(256, {noteOn(100, 60), tickAt(100)});
    const auto fw = freqWrites(r.log, NR13, NR14);
    REQUIRE(fw.size() == 3);
    CHECK(fw[0].trigger);                                              // note-on
    CHECK(fw[1].freq == rackboy::core::pitch_square_11bit(-12));       // row 0 (tick 0)
    CHECK(fw[2].freq == rackboy::core::pitch_square_11bit(-12 + 4));   // row 1 (the tick)
    CHECK(fw[0].apu == 109);    // round(100 * 160 / 147) = 109
    CHECK(fw[2].apu == 109);
}

TEST_CASE("engine: velocity scales the initial volume when Vel>Vol is on")
{
    Rig r;
    r.only(0);
    r.s.velToVol = true;
    r.apply();
    r.run(300, {noteOn(0, 60, 64)});
    uint8_t nr12 = 0xFF;
    for (const W& w : r.log) if (w.addr == 0xFF12) nr12 = w.v;
    CHECK((nr12 >> 4) == 8);   // round(15 * 64 / 127) = 8
    Rig q;
    q.only(0);
    q.s.velToVol = true;
    q.apply();
    q.run(300, {noteOn(0, 60, 1)});
    for (const W& w : q.log) if (w.addr == 0xFF12) nr12 = w.v;
    CHECK((nr12 >> 4) == 1);   // minimum 1
}

TEST_CASE("engine: reset restores the power-on sequence and a long block is chunked")
{
    Rig r;
    r.run(100, {noteOn(0, 60)});
    r.log.clear();
    r.e->reset();
    const std::vector<W> init = r.log;
    // NR50, NR51 and wave RAM (NR30 0, 16 bytes, NR30 restore) were written (rackboy_apu_init itself writes NR52 = 0x80)
    REQUIRE(init.size() >= 2);
    CHECK(init[0].addr == 0xFF24);
    CHECK(init[0].v == 0x77);
    CHECK(init[1].addr == 0xFF25);
    CHECK(init[1].v == 0x00);
    // 1000 host frames in one call (> 512) must equal the same audio in 128-frame blocks
    Rig a, b;
    a.run(1000, {noteOn(0, 60)}, 1000);
    b.run(1000, {noteOn(0, 60)}, 125);
    CHECK(rmsDb(a.L, 0, 1000) > -40.0);
    double maxDiff = 0.0;
    for (size_t i = 0; i < 1000; ++i) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(a.L[i] - b.L[i])));
    CHECK(maxDiff == 0.0);
}

TEST_CASE("engine: the frame sequencer runs (envelope decay and sweep work through the engine)")
{
    // Upstream SameBoy clocks length/envelope/sweep from the CPU's DIV counter;
    // rackboy has none, so Engine enables the sequencer in run_stereo.
    {
        Rig r;
        r.only(0);
        r.s.ch[0].volume = 15;
        r.s.ch[0].envPeriod = 1;   // one volume step per 1/64 s, decreasing
        r.apply();
        r.run(44100, {noteOn(0, 60)});
        const double early = rmsDb(r.L, 500, 2500), late = rmsDb(r.L, 40000, 44000);
        std::printf("[engine envelope] early %.2f dBFS, late %.2f dBFS\n", early, late);
        CHECK(late < early - 12.0);
    }
    {
        // Pulse 1 sweep up (shift 1, time 1) walks the frequency register upward.
        Rig r;
        r.only(0);
        r.s.ch[0].sweepTime = 1;
        r.s.ch[0].sweepShift = 1;
        r.s.ch[0].sweepDown = false;
        r.apply();
        r.run(20000, {noteOn(0, 48)});
        double fEarly = zeroCrossHz(r.L, 800, 2600), fLate = zeroCrossHz(r.L, 4000, 6000);
        std::printf("[engine sweep] early %.1f Hz, then %.1f Hz\n", fEarly, fLate);
        CHECK(fEarly > 0.0);
        CHECK(fLate > fEarly * 1.2);
    }
}

TEST_CASE("engine: Pulse 1 sweep Off (Amt Off or Time Off) never silences a high note")
{
    // Sweep time > 0 with shift 0 would run the hardware overflow check (freq + freq >> 0) and
    // silence high notes; "Off" must write NR10 fully disabled.
    struct Case { int time; int shift; bool down; };
    const Case cases[] = {{3, 0, false}, {7, 0, false}, {3, 0, true}, {0, 3, false}, {0, 3, true}};
    for (const Case& c : cases)
    {
        CAPTURE(c.time);
        CAPTURE(c.shift);
        CAPTURE(c.down);
        Rig r;
        r.only(0);
        r.s.ch[0].sweepTime = static_cast<uint8_t>(c.time);
        r.s.ch[0].sweepShift = static_cast<uint8_t>(c.shift);
        r.s.ch[0].sweepDown = c.down;
        r.apply();
        r.run(30000, {noteOn(0, 100)});
        CHECK(rmsDb(r.L, 25000, 29000) > -40.0);   // still sounding long after the sweep clocks
        bool nr10 = false;
        for (const W& w : r.log)
            if (w.addr == 0xFF10) { nr10 = true; CHECK(w.v == 0); }
        CHECK(nr10);
    }
}
