// WP5 tests 1-4: the mpcboy table-engine extensions (volume column, P, delayed
// K, W retrigger). All run through Engine with hand-placed ticks (tickAt).
#include <cmath>
#include <cstdio>
#include <vector>

#include "doctest/doctest.h"
#include "engine_test_support.h"

using namespace mpcboy;
using namespace mpcboy_test;

namespace {

constexpr uint16_t NR12 = 0xFF12, NR13 = 0xFF13, NR14 = 0xFF14, NR30 = 0xFF1A, NR34 = 0xFF1E;

double centsOfSquare(uint16_t x) { return 1200.0 * std::log2((131072.0 / (2048.0 - x)) / 523.2511306011972); }

Row row(int tr = 0) { return {static_cast<int8_t>(tr), 0, 0, 0, 0}; }

}  // namespace

TEST_CASE("table ext 1: volume column sets NRx2 and retriggers; F0 -> 80 lowers the level by 6 dB")
{
    Rig r;
    r.only(0);
    r.s.ch[0].tableSlot = 0;
    r.apply();
    auto& t = r.e->ownBank().tables[0];
    setTable(t, {row(), row()});
    t.rows[0].vol = 0xF0; t.rows[0].has_vol = true;
    t.rows[1].vol = 0x80; t.rows[1].has_vol = true;
    r.run(4500, {noteOn(0, 60), tickAt(2000)});
    const double before = rmsDb(r.L, 600, 1900), after = rmsDb(r.L, 2600, 3900);
    std::printf("[table ext 1] before %.2f dBFS, after %.2f dBFS, delta %.2f dB\n", before, after, before - after);
    CHECK(before - after > 5.0);
    CHECK(before - after < 7.0);
    // the volume column wrote NR12 = 0x80 with a trigger
    bool saw80 = false;
    for (size_t i = 0; i + 1 < r.log.size(); ++i)
        if (r.log[i].addr == NR12 && r.log[i].v == 0x80)
        {
            for (size_t j = i + 1; j < r.log.size(); ++j)
                if (r.log[j].addr == NR14) { saw80 = (r.log[j].v & 0x80) != 0; break; }
        }
    CHECK(saw80);
}

TEST_CASE("table ext 2: P04 over 16 ticks raises the pitch by 4 semitones; P00 holds")
{
    for (int hold = 0; hold < 2; ++hold)
    {
        CAPTURE(hold);
        Rig r;
        r.only(0);
        r.s.ch[0].tableSlot = 0;
        r.apply();
        std::vector<Row> rows(16, row());
        rows[0] = {0, 'P', 4, 0, 0};
        if (hold) rows[8] = {0, 'P', 0, 0, 0};
        setTable(r.e->ownBank().tables[0], rows);
        std::vector<Ev> evs = {noteOn(0, 60)};
        for (int k = 1; k <= 16; ++k) evs.push_back(tickAt(k * 1000));
        r.run(17500, evs);
        const auto fw = freqWrites(r.log, NR13, NR14);
        REQUIRE(fw.size() == 18);   // trigger, tick 0, ticks 1..16
        const double base = centsOfSquare(fw[1].freq);   // tick 0: accumulator still 0
        if (!hold)
        {
            const double gained = centsOfSquare(fw[17].freq) - base;
            std::printf("[table ext 2] 16 ticks of P04: %.1f cents\n", gained);
            CHECK(std::fabs(gained - 400.0) <= 10.0);
        }
        else
        {
            // P00 on row 8 (tick 8) keeps the offset: 8 ticks of 4/16 st = 200 cents
            CHECK(std::fabs(centsOfSquare(fw[9].freq) - base - 200.0) <= 10.0);
            for (size_t i = 10; i < fw.size(); ++i) CHECK(fw[i].freq == fw[9].freq);
        }
    }
}

TEST_CASE("table ext 3: K03 kills on tick 3 exactly; K00 kills at once")
{
    for (int k = 0; k < 2; ++k)
    {
        CAPTURE(k);
        Rig r;
        r.only(0);
        r.s.ch[0].tableSlot = 0;
        r.apply();
        std::vector<Row> rows(16, row());
        rows[0] = {0, 'K', static_cast<uint8_t>(k ? 0 : 3), 0, 0};
        setTable(r.e->ownBank().tables[0], rows);
        r.run(6000, {noteOn(0, 60), tickAt(1000), tickAt(2000), tickAt(3000), tickAt(4000)});
        std::vector<uint64_t> kills;
        for (const W& w : r.log)
            if (w.addr == NR12 && w.v == 0x00) kills.push_back(w.apu);
        REQUIRE(kills.size() == 1);
        if (k == 0)
            CHECK(kills[0] == 3265);   // round(3000 * 160 / 147): tick 3, not tick 1 or 2
        else
            CHECK(kills[0] == 0);      // tick 0 (the note-on itself)
        CHECK(rmsDb(r.L, 5000, 5900) < -90.0);   // after the DC blocker's tail has died away
        if (k == 0) CHECK(rmsDb(r.L, 600, 2800) > -40.0);   // still sounding before tick 3
    }
}

TEST_CASE("table ext 4: W02 loads bank wave 2, then retriggers CH3 and the channel keeps sounding")
{
    Rig r;
    r.only(2);
    r.s.ch[2].tableSlot = 0;
    r.apply();
    auto& bank = r.e->ownBank();
    setTable(bank.tables[0], {row(), {0, 'W', 2, 0, 0}});
    uint8_t saw[32];
    for (int i = 0; i < 32; ++i) saw[i] = static_cast<uint8_t>(i * 15 / 31);
    for (int j = 0; j < 16; ++j) bank.waves[2].samples[j] = static_cast<uint8_t>((saw[2 * j] << 4) | saw[2 * j + 1]);
    bank.waves[2].occupied = true;
    r.run(6000, {noteOn(0, 60), tickAt(1000)});   // tick 0 = row 0, tick 1000 = row 1 (W02)
    // spy: wave RAM writes (0xFF30..0xFF3F) after the tick are followed by an NR34 trigger
    const uint64_t tickApu = 1088;   // round(1000 * 160 / 147)
    size_t firstRam = r.log.size(), lastRam = 0;
    for (size_t i = 0; i < r.log.size(); ++i)
        if (r.log[i].apu >= tickApu && r.log[i].addr >= 0xFF30 && r.log[i].addr <= 0xFF3F)
        {
            if (firstRam == r.log.size()) firstRam = i;
            lastRam = i;
        }
    REQUIRE(firstRam < r.log.size());
    for (int j = 0; j < 16; ++j) CHECK(r.log[firstRam + static_cast<size_t>(j)].v == bank.waves[2].samples[j]);
    size_t trig = r.log.size();
    for (size_t i = lastRam + 1; i < r.log.size(); ++i)
        if (r.log[i].addr == NR34 && (r.log[i].v & 0x80)) { trig = i; break; }
    CHECK(trig < r.log.size());
    CHECK(r.log[firstRam - 1].addr == NR30);   // the helper's DAC-off write precedes the RAM bytes
    // 50 ms after the retrigger the wave channel is still audible
    const double db = rmsDb(r.L, 1100 + 300, 1100 + 300 + 2205);
    std::printf("[table ext 4] wave RMS 50 ms after W02: %.2f dBFS\n", db);
    CHECK(db > -40.0);

    // Unoccupied slot: W is ignored (no wave RAM write after the tick, no retrigger)
    Rig q;
    q.only(2);
    q.s.ch[2].tableSlot = 0;
    q.apply();
    setTable(q.e->ownBank().tables[0], {row(), {0, 'W', 5, 0, 0}});
    q.run(2000, {noteOn(0, 60), tickAt(1000)});
    for (const W& w : q.log) CHECK_FALSE((w.apu >= tickApu && w.addr >= 0xFF30 && w.addr <= 0xFF3F));
}
