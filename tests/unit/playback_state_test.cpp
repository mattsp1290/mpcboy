// UI redesign step 3: the engine publishes each channel's table playback for the UI thread
// (src/engine/playback_state.h): playhead row, running flag, table slot, channel note, live
// level and the wave RAM, as relaxed atomics written once per render().
#include <cstring>
#include <thread>

#include "doctest/doctest.h"
#include "engine_test_support.h"
#include "playback_state.h"
#include "table_bank.h"

using namespace mpcboy;
using namespace mpcboy_test;

TEST_CASE("playback: pack / unpack round-trips every field")
{
    ChannelPlayback p;
    p.row = 13;
    p.running = true;
    p.table = 15;
    p.note = 127;
    p.level = 9;
    p.active = true;
    CHECK(PlaybackState::unpack(PlaybackState::pack(p)) == p);
    ChannelPlayback idle;   // defaults: no table, no note
    CHECK(PlaybackState::unpack(PlaybackState::pack(idle)) == idle);
    CHECK(PlaybackState::unpack(PlaybackState::pack(idle)).note == -1);
    CHECK(PlaybackState::unpack(PlaybackState::pack(idle)).table == -1);
}

TEST_CASE("playback: the playhead follows the table rows, the hop and the note; idle after reset")
{
    Rig r;
    setTable(r.e->ownBank().tables[0], majArp());   // rows 0 +0, 1 +4, 2 +3, 3 -7 H01
    r.s.ch[0].tableSlot = 0;
    r.s.ch[1].tableSlot = -1;
    r.s.ch[2].transpose = 12;
    r.apply();

    // Before any note: nothing runs, no note.
    r.run(128);
    for (int c = 0; c < 4; ++c)
    {
        const ChannelPlayback p = r.e->playback().load(c);
        CHECK_FALSE(p.running);
        CHECK(p.note == -1);
        CHECK(p.level == 0);
    }

    // Note on at frame 0: tick 0 applies row 0.
    r.run(128, {noteOn(0, 60)});
    ChannelPlayback p0 = r.e->playback().load(0);
    CHECK(p0.running);
    CHECK(p0.table == 0);
    CHECK(p0.row == 0);
    CHECK(p0.note == 60);
    CHECK(p0.active);
    CHECK(p0.level == 15);   // default volume 15, read from the APU envelope
    CHECK_FALSE(r.e->playback().load(1).running);   // no table on PU2
    CHECK(r.e->playback().load(1).table == -1);
    CHECK(r.e->playback().load(1).note == 60);
    CHECK(r.e->playback().load(2).note == 72);      // channel transpose included

    // Each tick advances the playhead: rows 1, 2, 3, then the hop back to row 1.
    const int expect[] = {1, 2, 3, 1, 2};
    for (int k = 0; k < 5; ++k)
    {
        r.run(128, {tickAt(10)});
        CAPTURE(k);
        CHECK(r.e->playback().load(0).row == expect[k]);
        CHECK(r.e->playback().load(0).running);
    }

    // A legato glide moves the note, not the playhead.
    r.s.retrigger = RetriggerMode::Legato;
    r.apply();
    r.run(128, {noteOn(0, 64)});
    CHECK(r.e->playback().load(0).note == 64);
    CHECK(r.e->playback().load(0).row == 2);

    // Cut note-off stops the table; reset clears everything.
    r.run(128, {noteOff(0, 64), noteOff(1, 60)});
    CHECK_FALSE(r.e->playback().load(0).running);
    r.e->reset();
    const ChannelPlayback after = r.e->playback().load(0);
    CHECK_FALSE(after.running);
    CHECK(after.note == -1);
    CHECK(after.table == -1);
}

TEST_CASE("playback: a D delay holds the playhead on the delaying row")
{
    Rig r;
    setTable(r.e->ownBank().tables[2], {{0, 'D', 2, 0, 0}, {5, 0, 0, 0, 0}, {0, 'H', 0, 0, 0}});
    r.only(0);
    r.s.ch[0].tableSlot = 2;
    r.apply();
    r.run(128, {noteOn(0, 48)});
    CHECK(r.e->playback().load(0).row == 0);
    r.run(128, {tickAt(5)});   // held by D02
    CHECK(r.e->playback().load(0).row == 0);
    r.run(128, {tickAt(5)});   // still held
    CHECK(r.e->playback().load(0).row == 0);
    r.run(128, {tickAt(5)});   // row 1
    CHECK(r.e->playback().load(0).row == 1);
}

TEST_CASE("playback: the published wave RAM is the selected wave once WAV triggers; levels track the envelope")
{
    Rig r;
    uint8_t want[kWaveBytes];
    for (int i = 0; i < kWaveBytes; ++i) want[i] = static_cast<uint8_t>(0x10 * (i & 15) + (15 - (i & 15)));
    std::memcpy(r.e->ownBank().waves[3].samples, want, sizeof(want));
    r.only(2);
    r.s.ch[2].waveSlot = 3;
    r.apply();
    r.run(128, {noteOn(0, 60)});
    uint8_t got[kWaveBytes];
    r.e->playback().loadWave(got);
    CHECK(std::memcmp(got, want, sizeof(want)) == 0);
    const ChannelPlayback w = r.e->playback().load(2);
    CHECK(w.active);
    CHECK(w.level == 15);   // default Wave level 100 %
    CHECK_FALSE(r.e->playback().load(0).active);   // PU1 off
    CHECK(r.e->playback().load(0).level == 0);
}

TEST_CASE("playback: a reader on another thread sees consistent channel words while blocks render")
{
    Rig r;
    setTable(r.e->ownBank().tables[0], majArp());
    for (int c = 0; c < 4; ++c) r.s.ch[c].tableSlot = 0;
    r.apply();
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::thread reader([&] {
        while (!stop.load())
        {
            for (int c = 0; c < 4; ++c)
            {
                const ChannelPlayback p = r.e->playback().load(c);
                if (p.row < 0 || p.row > 15 || p.level < 0 || p.level > 15 || p.table < -1 || p.table > 15) ++bad;
                if (p.running && p.table != 0) ++bad;
            }
        }
    });
    std::vector<Ev> evs = {noteOn(0, 60)};
    for (int k = 1; k <= 40; ++k) evs.push_back(tickAt(k * 300));
    r.run(40 * 300 + 128, evs, 64);
    stop = true;
    reader.join();
    CHECK(bad.load() == 0);
}
