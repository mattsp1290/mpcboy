// WP11 tests 5-8 (06-lsdj-sav-import.md) through the plugin and the import
// worker: undo, the trigger debounce, the worker scan, concurrent snapshots and
// the state round trip. File-based tests use a temporary plugin folder through
// MPC_PLUGIN_FOLDER (set before any test runs, see sav_fixture_support.h).
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

TEST_CASE("import 5: through the plugin, import then imp_undo restores the previous content and the editor cursor follows")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    const TableSong ts = makeTableSong(fixtureSong(fx), 4);
    writeFile(g_folder.importDir, "song.sav", savFromSong(ts.song));
    PluginRig r(&fakeNow);
    r.block();
    r.rescan();
    const std::vector<uint8_t> before = r.blob();
    r.p->set(ED_Table, 1);
    r.p->set(ED_Row, 3);
    r.block();

    r.p->set(Imp_File, 1);
    r.p->set(Imp_Go, 1);
    r.block();   // process() turns the accepted edge into the import request
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 1; }));
    r.block();   // the audio thread consumes the staged import
    r.block();
    const std::vector<uint8_t> after = r.blob();
    CHECK(after != before);
    Content got;
    std::string name;
    REQUIRE(decodeState(after.data(), after.size(), got, &name));
    CHECK(name == "song.sav");
    const ImportReport rep = r.p->importer().lastReport();
    CHECK(rep.tablesImported == 16);
    // The cursor shows the imported cell: table 1 is file table 0.
    const auto& row = got.tables.tables[0].rows[3];
    CHECK(r.p->getInt(ED_Tsp) == std::clamp<int>(row.transpose, -48, 48));
    CHECK(r.p->getInt(ED_Val1) == row.cmd1_param);
    CHECK(r.p->getInt(Imp_Go) == 0);   // the trigger went back to 0

    advanceMs(300);   // clear of the debounce
    r.p->set(Imp_Undo, 1);
    r.block();
    r.block();
    CHECK(r.p->getInt(Imp_Undo) == 0);
    std::vector<uint8_t> undone = r.blob();
    Content u;
    std::string uname;
    REQUIRE(decodeState(undone.data(), undone.size(), u, &uname));
    Content b0;
    std::string n0;
    REQUIRE(decodeState(before.data(), before.size(), b0, &n0));
    CHECK(encoded(u) == encoded(b0));   // content exactly as before the import
    const auto& row0 = u.tables.tables[0].rows[3];
    CHECK(r.p->getInt(ED_Tsp) == std::clamp<int>(row0.transpose, -48, 48));
    clearImportDir();
}

// ============================== test 6 ==========================================

TEST_CASE("import 6: two imp_go edges 100 ms apart run one import; a later edge runs another; a second request during an import is ignored")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "a.sav", savFromSong(makeTableSong(fixtureSong(fx), 6).song));
    PluginRig r(&fakeNow);
    r.block();
    r.rescan();
    REQUIRE(r.p->importer().fileCount() == 1);
    CHECK(r.p->importer().importsDone() == 0);

    r.p->set(Imp_Go, 1);
    r.block();
    CHECK(r.p->getInt(Imp_Go) == 0);
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 1; }));
    advanceMs(100);   // fake time: independent of how long the import took
    r.p->set(Imp_Go, 1);   // the second press of one tap
    r.block();
    CHECK(r.p->getInt(Imp_Go) == 0);   // it is reset like any edge ...
    CHECK_FALSE(r.p->importer().busy());         // ... but no request was made (an accepted one is busy or done)
    CHECK(r.p->importer().importsDone() == 1);   // ... so it did nothing
    CHECK(r.p->importer().importsFailed() == 0);

    advanceMs(300);
    r.p->set(Imp_Go, 1);   // a deliberate third press, well after 250 ms
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 2; }));

    // Holding the parameter high is not a second edge.
    advanceMs(300);
    r.p->set(Imp_Go, 1);
    r.p->set(Imp_Go, 1);
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 3; }));
    CHECK_FALSE(r.p->importer().busy());
    CHECK(r.p->importer().importsDone() == 3);

    // The worker itself ignores a request while one is pending or running.
    const uint32_t d0 = r.p->importer().importsDone();
    const bool first = r.p->importer().requestImport(0);
    const bool second = r.p->importer().requestImport(0);
    CHECK(first);
    CHECK_FALSE(second);
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == d0 + 1; }));
    CHECK_FALSE(r.p->importer().busy());
    CHECK(r.p->importer().importsDone() == d0 + 1);
    clearImportDir();
}

TEST_CASE("import 6: imp_rescan debounces the same way and picks up new files; imp_file text shows names, - when empty, ERR after a failure")
{
    clearImportDir();
    PluginRig r(&fakeNow);
    r.block();
    r.rescan();
    CHECK(r.p->importer().fileCount() == 0);
    CHECK(r.p->displayText(Imp_File) == "-");

    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "zeta_long_name.sav", savFromSong(makeTableSong(fixtureSong(fx), 6).song));
    writeFile(g_folder.importDir, "Alpha.SAV", fx);
    writeFile(g_folder.importDir, "broken.sav", std::vector<uint8_t>(5000, 0x55));

    const uint32_t scans = r.p->importer().scansDone();
    r.p->set(Imp_Rescan, 1);
    r.block();
    CHECK(r.p->getInt(Imp_Rescan) == 0);
    REQUIRE(waitFor([&] { return r.p->importer().scansDone() > scans; }));
    REQUIRE(r.p->importer().fileCount() == 3);
    CHECK(r.p->importer().fileName(0) == "Alpha.SAV");   // sorted by name, case-insensitively
    CHECK(r.p->importer().fileName(1) == "broken.sav");
    CHECK(r.p->importer().fileName(2) == "zeta_long_name.sav");
    r.p->set(Imp_File, 1);
    CHECK(r.p->displayText(Imp_File) == "Alpha");
    r.p->set(Imp_File, 3);
    CHECK(r.p->displayText(Imp_File) == "zeta_lon");   // extension stripped, cut at 8 bytes
    r.p->set(Imp_File, 40);
    CHECK(r.p->displayText(Imp_File) == "-");

    // A failing import: nothing changes and the text shows ERR <code> until the next scan.
    const std::vector<uint8_t> before = r.blob();
    advanceMs(300);
    r.p->set(Imp_File, 2);
    r.p->set(Imp_Go, 1);
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().importsFailed() == 1; }));
    r.block();
    r.block();
    CHECK(r.blob() == before);
    CHECK(r.p->importer().lastErrorCode() == static_cast<int>(ImportResult::BadSize));
    CHECK(r.p->displayText(Imp_File) == "ERR 2");
    advanceMs(300);
    const uint32_t scans2 = r.p->importer().scansDone();
    r.p->set(Imp_Rescan, 1);
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().scansDone() > scans2; }));
    CHECK(r.p->displayText(Imp_File) == "broken");
    clearImportDir();
}

// ============================== worker scan =====================================

TEST_CASE("import: the scan lists only regular .sav files at depth 1: no symlinks, no dot-files, no subfolders, at most 64, sorted")
{
    const std::string dir = g_folder.root + "/scan_case";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir + "/sub");
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(dir, "b.sav", fx);
    writeFile(dir, "A.sav", fx);
    writeFile(dir, "c.txt", fx);
    writeFile(dir, ".hidden.sav", fx);
    writeFile(dir, "._b.sav", fx);
    writeFile(dir + "/sub", "deep.sav", fx);
    fs::create_directories(dir + "/folder.sav");              // a directory named like a save
    fs::create_symlink(dir + "/b.sav", dir + "/link.sav", ec);   // a symbolic link to a save
    fs::create_directory_symlink(dir + "/sub", dir + "/linkdir", ec);
    ContentStore store;
    {
        ImportWorker w(dir, store);
        REQUIRE(waitFor([&] { return w.scansDone() >= 1; }));
        REQUIRE(w.fileCount() == 2);
        CHECK(w.fileName(0) == "A.sav");
        CHECK(w.fileName(1) == "b.sav");
        CHECK(w.fileName(2) == "");
    }   // joined here
    for (int i = 0; i < 70; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "many_%02d.sav", i);
        writeFile(dir, n, fx);
    }
    {
        ImportWorker w(dir, store);
        REQUIRE(waitFor([&] { return w.scansDone() >= 1; }));
        CHECK(w.fileCount() == 64);
        CHECK(w.fileName(0) == "A.sav");
        // A symbolic link is never imported even when asked for by index.
        CHECK(w.displayText(0) == "A");
    }
    {
        ImportWorker missing(g_folder.root + "/no_such_folder", store);   // no Content/Import: an empty list
        REQUIRE(waitFor([&] { return missing.scansDone() >= 1; }));
        CHECK(missing.fileCount() == 0);
        CHECK(missing.displayText(0) == "-");
        CHECK(missing.requestImport(0));   // nothing at index 0: no change, no error
        REQUIRE(waitFor([&] { return !missing.busy(); }));
        CHECK(missing.importsDone() == 0);
        CHECK(missing.importsFailed() == 0);
    }
    fs::remove_all(dir, ec);
}

// ============================== test 7 ==========================================

TEST_CASE("import 7: the engine renders while imports and undos run on the worker; every snapshot is whole")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "a.sav", savFromSong(makeTableSong(fixtureSong(fx), 2).song));
    writeFile(g_folder.importDir, "b.sav", savFromSong(makeTableSong(fixtureSong(fx), 9).song));
    PluginRig r(&fakeNow);
    r.block();
    r.rescan();
    REQUIRE(r.p->importer().fileCount() == 2);

    Content factory;
    loadFactoryContent(factory);
    const std::vector<uint8_t> okFactory = encoded(factory);
    const std::vector<uint8_t> okA = encoded(importedFromFile(g_folder.importDir + "/a.sav"));
    const std::vector<uint8_t> okB = encoded(importedFromFile(g_folder.importDir + "/b.sav"));

    std::atomic<bool> done{false};
    std::atomic<uint32_t> requested{0}, torn{0}, snaps{0};
    std::thread ui([&] {
        uint32_t n = 0;
        while (!done.load(std::memory_order_acquire))
        {
            if (r.p->importer().requestImport(static_cast<int>(n % 2))) requested.fetch_add(1, std::memory_order_relaxed);
            if (n % 3 == 0) r.p->content().requestUndo();
            std::vector<uint8_t> blob;
            r.p->saveExtraState(blob);
            Content c;
            if (!decodeState(blob.data(), blob.size(), c)) torn.fetch_add(1, std::memory_order_relaxed);
            else
            {
                const std::vector<uint8_t> e = encoded(c);
                if (e != okFactory && e != okA && e != okB) torn.fetch_add(1, std::memory_order_relaxed);
            }
            snaps.fetch_add(1, std::memory_order_relaxed);
            ++n;
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });

    // Notes and table edits on the "audio" thread while the worker imports.
    std::vector<mpc::MidiEvent> m;
    r.p->set(P1_Tbl, 2);
    r.p->set(WV_On, 1);
    r.p->set(WV_Tbl, 3);
    for (int i = 0; i < 6000; ++i)
    {
        m.clear();
        if (i % 40 == 0) m.push_back(mpc::MidiEvent::noteOn(0, 48 + (i / 40) % 24, 100, 0));
        if (i % 40 == 30) m.push_back(mpc::MidiEvent::noteOff(0, 48 + (i / 40) % 24, 0));
        r.block(128, m);
        if (i % 500 == 499) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    done.store(true, std::memory_order_release);
    ui.join();
    std::printf("[import 7] worker requested %u imports; second thread took %u snapshots; main thread rendered 6,000 blocks\n",
                requested.load(), snaps.load());
    CHECK(torn.load() == 0);
    CHECK(requested.load() > 2);
    REQUIRE(waitFor([&] { return !r.p->importer().busy(); }));
    CHECK(r.p->importer().importsDone() == requested.load());   // every accepted request completed
    for (float v : r.L) REQUIRE(std::isfinite(v));
    clearImportDir();
}

// ============================== test 8 ==========================================

TEST_CASE("import 8: the imported file name rides in the state, comes back on load and selects the file in imp_file after a scan")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "alpha.sav", fx);
    writeFile(g_folder.importDir, "mine.sav", savFromSong(makeTableSong(fixtureSong(fx), 12).song));
    writeFile(g_folder.importDir, "zulu.sav", fx);

    std::vector<uint8_t> blob;
    {
        PluginRig r(&fakeNow);
        r.block();
        r.rescan();
        r.p->set(Imp_File, 2);
        r.p->set(Imp_Go, 1);
        r.block();
        REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 1; }));
        r.block();
        r.block();
        blob = r.blob();
        Content c;
        std::string name;
        REQUIRE(decodeState(blob.data(), blob.size(), c, &name));
        CHECK(name == "mine.sav");
        CHECK(blob.size() == kStateSize + 8);
        CHECK(blob[4] == 2);
    }
    {
        PluginRig r2(&fakeNow);
        r2.block();
        r2.rescan();
        r2.p->set(Imp_File, 3);   // some other selection saved in the project
        r2.p->loadExtraState(blob.data(), blob.size());
        r2.p->reset();
        r2.block();
        r2.block();
        CHECK(liveName(*r2.p) == "mine.sav");
        CHECK(r2.blob() == blob);   // content, name and CRC round-trip byte for byte
        // The worker resolves the name against its list and the plugin selects it.
        CHECK(waitFor([&] { r2.block(); return r2.p->getInt(Imp_File) == 2; }));
        CHECK(r2.p->displayText(Imp_File) == "mine");
        // No import ran on load.
        CHECK(r2.p->importer().importsDone() == 0);
    }
    {
        // A project saved with a name that is no longer in the folder keeps the index it had.
        PluginRig r3(&fakeNow);
        r3.block();
        std::filesystem::remove(g_folder.importDir + "/mine.sav");
        r3.rescan();
        r3.p->set(Imp_File, 3);
        r3.p->loadExtraState(blob.data(), blob.size());
        r3.rescan();
        for (int i = 0; i < 20; ++i) r3.block();
        CHECK(r3.p->getInt(Imp_File) == 3);
        CHECK(liveName(*r3.p) == "mine.sav");
    }
    {
        // A v1 blob (no name field) is rejected: nothing changes.
        PluginRig r4(&fakeNow);
        r4.block();
        const std::vector<uint8_t> keep = r4.blob();
        std::vector<uint8_t> v1(blob.begin(), blob.begin() + 2053);
        v1[4] = 1;
        v1.resize(2057);
        const uint32_t crc = crc32Ieee(v1.data(), 2053);
        for (int i = 0; i < 4; ++i) v1[2053 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
        r4.p->loadExtraState(v1.data(), v1.size());
        r4.block();
        r4.block();
        CHECK(r4.blob() == keep);
    }
    clearImportDir();
}

TEST_CASE("import: a trigger set to 1 off the process thread (as a project or preset load does) never starts an import")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    const TableSong ts = makeTableSong(fixtureSong(fx), 4);
    writeFile(g_folder.importDir, "song.sav", savFromSong(ts.song));
    PluginRig r(&fakeNow);
    r.block();
    r.rescan();
    r.p->set(Imp_File, 1);

    std::thread loader([&] { r.p->set(Imp_Go, 1); r.p->set(Imp_Undo, 1); });
    loader.join();
    r.block();
    CHECK_FALSE(r.p->importer().busy());
    r.block();
    r.block();
    CHECK(r.p->importer().importsDone() == 0);
    CHECK(r.p->getInt(Imp_Go) == 0);     // still reset to 0 by process()
    CHECK(r.p->getInt(Imp_Undo) == 0);

    r.p->set(Imp_Go, 1);                 // a real tap, on the process thread, still works
    r.block();
    CHECK(waitFor([&] { return r.p->importer().importsDone() == 1; }));
}

TEST_CASE("import: a trigger restored as 1 by a state load on the process thread is cancelled by loadExtraState")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "song.sav", savFromSong(makeTableSong(fixtureSong(fx), 4).song));
    PluginRig r(&fakeNow);
    r.block();
    r.rescan();
    r.p->set(Imp_File, 1);

    std::vector<uint8_t> blob;
    r.p->saveExtraState(blob);
    // The SDK writes a chunk's parameters first (here: the trigger arrives as an edge on the
    // audio-marked thread) and calls loadExtraState() afterwards.
    r.p->set(Imp_Go, 1);
    r.p->loadExtraState(blob.data(), blob.size());
    r.block();
    r.block();
    CHECK_FALSE(r.p->importer().busy());
    CHECK(r.p->importer().importsDone() == 0);
    CHECK(r.p->getInt(Imp_Go) == 0);   // still written back to 0
    clearImportDir();
}

TEST_CASE("import: the saved import name follows the live content (undo, a load that replaces a pending import)")
{
    clearImportDir();
    const std::vector<uint8_t> fx = fixtureSav();
    writeFile(g_folder.importDir, "song.sav", savFromSong(makeTableSong(fixtureSong(fx), 4).song));
    PluginRig r(&fakeNow);
    r.block();
    r.rescan();
    r.p->set(Imp_File, 1);
    const std::vector<uint8_t> fresh = r.blob();   // no import yet

    r.p->set(Imp_Go, 1);
    r.block();
    REQUIRE(waitFor([&] { return r.p->importer().importsDone() == 1; }));
    {
        // Between staging and the next audio block the staged import already names the saved state.
        Content c;
        std::string name;
        const std::vector<uint8_t> b = r.blob();
        REQUIRE(decodeState(b.data(), b.size(), c, &name));
        CHECK(name == "song.sav");
    }
    r.block();
    CHECK(liveName(*r.p) == "song.sav");

    advanceMs(300);
    r.p->set(Imp_Undo, 1);
    r.block();
    r.block();
    CHECK(liveName(*r.p) == "");   // the content is back to before the import

    advanceMs(300);
    r.p->set(Imp_Undo, 1);   // undo again re-applies the import
    r.block();
    r.block();
    CHECK(liveName(*r.p) == "song.sav");

    // A project load ends the history: the name is the one stored in the loaded project.
    r.p->loadExtraState(fresh.data(), fresh.size());
    CHECK(liveName(*r.p) == "");
    r.block();
    r.block();
    CHECK(liveName(*r.p) == "");
    clearImportDir();
}
