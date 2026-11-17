// WP11 test 5 (06-lsdj-sav-import.md): ContentStore undo of an import.
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
#include "plugin_test_support.h"
#include "rackboy_sav_compression.h"
#include "rackboy_sav_header.h"
#include "rackboy_sav_tables.h"
#include "rackboy_sav_validate.h"
#include "rackboy_sav_waves.h"
#include "sav_fixture_support.h"
#include "sav_import.h"
#include "state_codec.h"

using namespace mpcboy;
using namespace mpcboy_test;
namespace fs = std::filesystem;
namespace sav = rackboy::sav;

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
