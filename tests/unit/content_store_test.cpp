// WP5 tests 5-8: ContentStore handoff, bank adapter, factory content, and the
// Engine's use of an attached store.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

#include "bank_adapter.h"
#include "content_store.h"
#include "doctest/doctest.h"
#include "engine_test_support.h"
#include "factory_content.h"

using namespace mpcboy;
using namespace mpcboy_test;

namespace {

// Test content whose every byte (outside table 15) derives from one value, so
// any torn snapshot is detectable.
void fillPattern(Content& c, uint32_t n)
{
    const uint8_t b = static_cast<uint8_t>(n);
    clearContent(c);
    for (int t = 0; t < kNumTables; ++t)
    {
        c.tables.tables[t].occupied = true;
        for (auto& r : c.tables.tables[t].rows)
        {
            r.transpose = static_cast<int8_t>(b);
            r.cmd1 = b; r.cmd1_param = b; r.cmd2 = b; r.cmd2_param = b;
            r.vol = b; r.has_vol = (b & 1) != 0;
        }
    }
    for (auto& w : c.waves.waves)
        for (auto& x : w) x = b;
}

// Table 15 carries the audio thread's edits and is excluded from the check.
bool consistent(const Content& c)
{
    const uint8_t b = static_cast<uint8_t>(c.waves.waves[0][0]);
    for (int t = 0; t < kNumTables - 1; ++t)
        for (const auto& r : c.tables.tables[t].rows)
            if (static_cast<uint8_t>(r.transpose) != b || r.cmd1 != b || r.cmd1_param != b || r.cmd2 != b ||
                r.cmd2_param != b || r.vol != b || r.has_vol != ((b & 1) != 0))
                return false;
    for (const auto& w : c.waves.waves)
        for (uint8_t x : w)
            if (x != b) return false;
    return true;
}

struct AudioThreadGuard
{
    explicit AudioThreadGuard(ContentStore& s) : store(s) { store.markAudioThread(true); }
    ~AudioThreadGuard() { store.markAudioThread(false); }
    ContentStore& store;
};

}  // namespace

TEST_CASE("content 5: staging, published reads and audio-thread edits race-free over 10,000 blocks")
{
    auto store = std::make_unique<ContentStore>();
    std::atomic<bool> done{false};
    std::atomic<uint32_t> staged{0}, reads{0}, torn{0};

    std::thread stager([&] {
        Content c;
        uint32_t n = 1;
        while (!done.load(std::memory_order_acquire))
        {
            fillPattern(c, n++);
            store->stage(c);
            staged.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    std::thread reader([&] {   // saveExtraState / formatValue
        auto out = std::make_unique<Content>();
        while (!done.load(std::memory_order_acquire))
        {
            store->readPublished(*out);
            if (!consistent(*out)) torn.fetch_add(1, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    {
        AudioThreadGuard audio(*store);
        for (int block = 0; block < 10000; ++block)
        {
            store->editLive([&](Content& c) { c.tables.tables[kNumTables - 1].rows[0].transpose = static_cast<int8_t>(block); });
            store->beginBlock();
            if ((block & 7) == 0) std::this_thread::yield();
        }
        store->beginBlock();
    }
    done.store(true, std::memory_order_release);
    stager.join();
    reader.join();
    std::printf("[content 5] staged %u, reads %u, torn %u\n", staged.load(), reads.load(), torn.load());
    CHECK(torn.load() == 0);
    CHECK(staged.load() > 0);
    CHECK(reads.load() > 0);
    // the final published snapshot equals live
    Content pub;
    store->readPublished(pub);
    CHECK(std::memcmp(&pub, &store->live(), sizeof(Content)) == 0);
}

TEST_CASE("content 6: cell edits from a non-audio thread are ignored; on the audio thread they reach live and, after one block, published")
{
    ContentStore store;
    auto edit = [](Content& c) { c.tables.tables[3].rows[5].transpose = 7; };
    bool applied = true;
    std::thread other([&] { applied = store.editLive(edit); });
    other.join();
    CHECK_FALSE(applied);
    CHECK(store.rejectedEdits() == 1);
    CHECK(store.live().tables.tables[3].rows[5].transpose == 0);

    AudioThreadGuard audio(store);
    CHECK(store.editLive(edit));
    CHECK(store.live().tables.tables[3].rows[5].transpose == 7);
    Content pub;
    store.readPublished(pub);
    CHECK(pub.tables.tables[3].rows[5].transpose == 0);   // not published yet
    store.beginBlock();
    store.readPublished(pub);
    CHECK(pub.tables.tables[3].rows[5].transpose == 7);
    CHECK(store.bank().tables[3].rows[5].transpose == 7);   // the bank view follows too
}

TEST_CASE("content 7: two bulk loads in a row, the second wins, and the writer never waits for the audio thread")
{
    ContentStore store;
    Content a, b;
    fillPattern(a, 11);
    fillPattern(b, 22);
    auto fut = std::async(std::launch::async, [&] { store.stage(a); store.stage(b); });
    REQUIRE(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);   // no audio block ran
    AudioThreadGuard audio(store);
    CHECK(store.beginBlock());
    CHECK(static_cast<uint8_t>(store.live().waves.waves[0][0]) == 22);
    Content pub;
    store.readPublished(pub);
    CHECK(consistent(pub));
    CHECK(pub.waves.waves[5][5] == 22);
    CHECK_FALSE(store.beginBlock());   // nothing left staged
}

TEST_CASE("content 8: factory content, bank adapter occupied flags")
{
    auto c = std::make_unique<Content>();
    loadFactoryContent(*c);
    for (int t = 0; t < 8; ++t)
    {
        CAPTURE(t);
        bool nonEmpty = false;
        for (const auto& r : c->tables.tables[t].rows)
            nonEmpty |= r.transpose != 0 || r.cmd1 != 0 || r.cmd2 != 0 || r.has_vol;
        CHECK(nonEmpty);
        CHECK(std::string(factoryTableName(t)).size() > 0);
        CHECK(std::string(factoryTableName(t)).size() <= 8);
        int lo = 99, hi = -1;
        for (int i = 0; i < 32; ++i)
        {
            const int v = waveSample(c->waves.waves[t], i);
            CHECK(v >= 0);
            CHECK(v <= 15);
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        CHECK(hi > lo);   // not a constant
        CHECK(std::string(factoryWaveName(t)).size() > 0);
    }
    for (int t = 8; t < 16; ++t)
    {
        bool empty = true;
        for (const auto& r : c->tables.tables[t].rows)
            empty &= r.transpose == 0 && r.cmd1 == 0 && r.cmd2 == 0 && !r.has_vol;
        CHECK(empty);
    }
    // spot-check the documented rows
    CHECK(c->tables.tables[0].rows[1].transpose == 4);
    CHECK(c->tables.tables[0].rows[3].cmd1 == 'H');
    CHECK(c->tables.tables[5].rows[0].vol == 0xF0);
    CHECK(c->tables.tables[5].rows[15].vol == 0x00);
    CHECK(c->tables.tables[5].rows[15].cmd1 == 'H');
    CHECK(c->tables.tables[6].rows[0].cmd1 == 'P');
    CHECK(c->tables.tables[6].rows[0].cmd2 == 'K');

    auto bank = std::make_unique<rackboy::sav::RackboyBank>();
    initBank(*bank, *c);
    for (int i = 0; i < 16; ++i)
    {
        CHECK(bank->tables[i].occupied);
        CHECK(bank->waves[i].occupied);
    }
    for (size_t i = 16; i < rackboy::sav::kMaxTables; ++i) CHECK_FALSE(bank->tables[i].occupied);
    for (size_t i = 16; i < rackboy::sav::kMaxWaveFrames; ++i) CHECK_FALSE(bank->waves[i].occupied);
    CHECK(std::memcmp(bank->waves[3].samples, c->waves.waves[3], 16) == 0);
    CHECK(bank->tables[2].rows[1].transpose == 12);
}

TEST_CASE("content: an Engine with an attached store plays staged tables and waves from the next block")
{
    auto store = std::make_unique<ContentStore>();
    Rig r;
    r.e->attachContent(store.get());
    r.only(0);
    r.s.ch[0].tableSlot = 3;
    r.apply();
    // empty content: the table slot has no rows, so the pitch stays on the note
    r.run(1000, {noteOn(0, 60), tickAt(500)});
    auto fw = freqWrites(r.log, 0xFF13, 0xFF14);
    for (const auto& w : fw) CHECK(w.freq == rackboy::core::pitch_square_11bit(-12));

    Content c;
    loadFactoryContent(c);
    c.tables.tables[3] = c.tables.tables[2];   // Octave in slot 3
    store->stage(c);
    const size_t mark = r.log.size();
    r.run(2000, {noteOn(0, 60), tickAt(500)});   // new block: staged content is consumed first
    fw = freqWrites(r.log, 0xFF13, 0xFF14, mark);
    REQUIRE(fw.size() >= 3);
    CHECK(fw[2].freq == rackboy::core::pitch_square_11bit(-12 + 12));   // row 1 of Octave
}

TEST_CASE("content: readLatest returns a staged load before the audio thread consumes it, then the published content")
{
    ContentStore store;
    Content a, b, out;
    fillPattern(a, 3);
    fillPattern(b, 7);
    store.setLive(a);

    store.readLatest(out);
    CHECK(std::memcmp(&out, &a, sizeof(Content)) == 0);

    store.stage(b);   // a load with no audio block yet
    store.readLatest(out);
    CHECK(std::memcmp(&out, &b, sizeof(Content)) == 0);

    store.markAudioThread();
    store.beginBlock();   // consumes the staged load
    store.readLatest(out);
    CHECK(std::memcmp(&out, &b, sizeof(Content)) == 0);
    store.readPublished(out);
    CHECK(std::memcmp(&out, &b, sizeof(Content)) == 0);
}
