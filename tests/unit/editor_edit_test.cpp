// UI WP3: the Mac editor's edit plumbing on MpcBoyPlugin (00-design.md "Thread and edit
// contract"): begin / automate / end pairing for 0..50, none for 51..62, cell edits that land on
// the cell captured at click time, and the drain on process()'s early-return paths. The editor
// calls run on a second thread (the Mac main thread); the test thread is the audio thread.
// The Imp Go editor test lives in import_plugin_test.cpp (it needs the import folder rig).
#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

#include "doctest/doctest.h"
#include "MpcBoyPlugin.h"
#include "plugin_test_support.h"

using namespace mpcboy;
using namespace mpcboy_test;

namespace {

struct HostCall
{
    int opcode, index;
    float opt;
};
std::vector<HostCall> g_calls;

VstIntPtr fakeHost(AEffect*, VstInt32 opcode, VstInt32 index, VstIntPtr, void*, float opt)
{
    if (opcode == audioMasterVersion) return 2400;
    g_calls.push_back({opcode, index, opt});
    return 0;
}

struct HostedRig : PluginRig
{
    AEffect effect{};
    HostedRig()
    {
        p->host_ = &fakeHost;
        p->effect_ = &effect;
        g_calls.clear();
    }
    ~HostedRig()
    {
        p->host_ = nullptr;
        p->effect_ = nullptr;
    }
};

int count(int opcode, int index = -1)
{
    return static_cast<int>(std::count_if(g_calls.begin(), g_calls.end(), [&](const HostCall& c) {
        return c.opcode == opcode && (index < 0 || c.index == index);
    }));
}

// Runs `fn` on another thread (the editor's main thread) and waits for it.
template <typename Fn>
void onEditorThread(Fn&& fn)
{
    std::thread t(std::forward<Fn>(fn));
    t.join();
}

float normOf(MpcBoyPlugin& p, int index, float plain) { return p.param(index).toNormalised(plain); }

const rackboy::sav::RackboyTableRow& publishedRow(MpcBoyPlugin& p, int table, int row, Content& scratch)
{
    p.content().readPublished(scratch);
    return scratch.tables.tables[table].rows[row];
}

}  // namespace

TEST_CASE("editor edits: 0..50 send begin / automate / end, paired per parameter")
{
    HostedRig r;
    r.block();
    g_calls.clear();
    onEditorThread([&] {
        r.p->editorBeginEdit(P_Volume);
        r.p->editorBeginEdit(P_Volume);   // already open: nothing
        r.p->editorSet(P_Volume, 0.25f);
        r.p->editorSet(P_Volume, 0.5f);
        r.p->editorEndEdit(P_Volume);
        r.p->editorEndEdit(P_Volume);     // not open: nothing
        r.p->editorEndEdit(P1_Duty);      // never opened: nothing
    });
    CHECK(count(audioMasterBeginEdit, P_Volume) == 1);
    CHECK(count(audioMasterEndEdit, P_Volume) == 1);
    CHECK(count(audioMasterEndEdit) == 1);
    REQUIRE(count(audioMasterAutomate, P_Volume) == 2);
    CHECK(g_calls[1].opt == doctest::Approx(0.25f));
    CHECK(g_calls[2].opt == doctest::Approx(0.5f));
    // Order: begin, automate, automate, end.
    REQUIRE(g_calls.size() == 4);
    CHECK(g_calls[0].opcode == audioMasterBeginEdit);
    CHECK(g_calls[1].opcode == audioMasterAutomate);
    CHECK(g_calls[3].opcode == audioMasterEndEdit);
    CHECK(r.p->getNormalised(P_Volume) == doctest::Approx(0.5f));

    // A discrete click: begin, automate, end together.
    g_calls.clear();
    onEditorThread([&] {
        r.p->editorBeginEdit(P1_On);
        r.p->editorSet(P1_On, 0.0f);
        r.p->editorEndEdit(P1_On);
    });
    REQUIRE(g_calls.size() == 3);
    CHECK(g_calls[0].opcode == audioMasterBeginEdit);
    CHECK(g_calls[1].opcode == audioMasterAutomate);
    CHECK(g_calls[2].opcode == audioMasterEndEdit);
    CHECK(r.p->getInt(P1_On) == 0);

    // endAllEditorEdits closes every open edit (editor close, cancelled tracking) exactly once.
    g_calls.clear();
    onEditorThread([&] {
        r.p->editorBeginEdit(P_BendRange);
        r.p->editorBeginEdit(P1_Tsp);
        r.p->editorBeginEdit(NZ_Tbl);
        CHECK(r.p->editorEditOpen(P1_Tsp));
        r.p->endAllEditorEdits();
        CHECK_FALSE(r.p->editorEditOpen(P1_Tsp));
        r.p->endAllEditorEdits();
    });
    CHECK(count(audioMasterBeginEdit) == 3);
    CHECK(count(audioMasterEndEdit) == 3);
    CHECK(count(audioMasterEndEdit, NZ_Tbl) == 1);

    // Out of range indices are ignored.
    g_calls.clear();
    onEditorThread([&] {
        r.p->editorBeginEdit(-1);
        r.p->editorSet(999, 1.0f);
        r.p->editorEndEdit(kTotalParams);
    });
    CHECK(g_calls.empty());
}

TEST_CASE("editor edits: 51..62 send no begin, automate or end")
{
    HostedRig r;
    r.block();
    g_calls.clear();
    onEditorThread([&] {
        for (int i = ED_Table; i <= Imp_Rescan; ++i)
        {
            r.p->editorBeginEdit(i);
            r.p->editorSet(i, i >= Imp_Go ? 1.0f : 0.5f);
            r.p->editorEndEdit(i);
        }
        r.p->endAllEditorEdits();
    });
    CHECK(g_calls.empty());
    for (int i = ED_Table; i <= Imp_Rescan; ++i) CHECK_FALSE(r.p->editorEditOpen(i));
}

TEST_CASE("editor edits: a cell edit lands on the row captured at click time even after the cursor moved")
{
    PluginRig r;
    r.p->set(ED_Table, 3);   // table index 2
    r.p->set(ED_Row, 2);
    r.block();
    Content c;
    const int tsp7 = std::clamp<int>(publishedRow(*r.p, 2, 7, c).transpose, -48, 48);
    const int tsp2 = std::clamp<int>(publishedRow(*r.p, 2, 2, c).transpose, -48, 48);
    const int target = tsp2 == 11 ? 12 : 11;
    REQUIRE(target != tsp7);

    onEditorThread([&] { r.p->editorBeginEdit(ED_Tsp); });   // mouse-down on row 2
    r.p->set(ED_Row, 7);                                        // the cursor moves (host / automation)
    r.block();
    CHECK(r.p->getInt(ED_Tsp) == tsp7);
    onEditorThread([&] {
        r.p->editorSet(ED_Tsp, normOf(*r.p, ED_Tsp, static_cast<float>(target)));   // the drag
        r.p->editorEndEdit(ED_Tsp);
    });
    r.block();   // drained: applied to live
    r.block();   // published
    CHECK(publishedRow(*r.p, 2, 2, c).transpose == target);
    CHECK(std::clamp<int>(publishedRow(*r.p, 2, 7, c).transpose, -48, 48) == tsp7);   // row 7 untouched
    CHECK(r.p->getInt(ED_Tsp) == tsp7);   // the display resynced to the cursor's cell

    // Without an open edit the cell under the current cursor is used, and a value equal to what
    // the cursor already shows still applies to a captured, different row.
    onEditorThread([&] { r.p->editorSet(ED_Val1, normOf(*r.p, ED_Val1, 0x42)); });
    r.block();
    r.block();
    CHECK(publishedRow(*r.p, 2, 7, c).cmd1_param == 0x42);
    CHECK(r.p->getInt(ED_Val1) == 0x42);
    r.p->set(ED_Row, 2);
    r.block();
    REQUIRE(publishedRow(*r.p, 2, 2, c).cmd1_param != 0x42);
    onEditorThread([&] { r.p->editorBeginEdit(ED_Val1); });   // captured on row 2
    r.p->set(ED_Row, 7);
    r.block();
    REQUIRE(r.p->getInt(ED_Val1) == 0x42);   // what the cursor shows on row 7
    onEditorThread([&] {
        r.p->editorSet(ED_Val1, normOf(*r.p, ED_Val1, 0x42));
        r.p->editorEndEdit(ED_Val1);
    });
    r.block();
    r.block();
    CHECK(publishedRow(*r.p, 2, 2, c).cmd1_param == 0x42);
}

TEST_CASE("editor edits: the drain also runs on process()'s early-return paths")
{
    PluginRig r;
    r.p->set(ED_Table, 1);
    r.p->set(ED_Row, 4);
    r.block();
    onEditorThread([&] { r.p->editorSet(ED_Val2, normOf(*r.p, ED_Val2, 0x5a)); });
    // No outputs: process() returns before the render.
    mpc::ProcessContext ctx;
    ctx.numOutputs = 0;
    ctx.outputs = nullptr;
    ctx.numFrames = 64;
    r.p->process(ctx);
    CHECK(r.p->content().live().tables.tables[0].rows[4].cmd2_param == 0x5a);

    // Unsupported rate (silence path).
    r.p->prepare(88200.0, 512);
    r.block();
    onEditorThread([&] { r.p->editorSet(ED_Val2, normOf(*r.p, ED_Val2, 0x21)); });
    r.block();
    CHECK(r.p->content().live().tables.tables[0].rows[4].cmd2_param == 0x21);
    r.p->prepare(44100.0, 512);
    r.block();
    r.block();
    Content c;
    CHECK(publishedRow(*r.p, 0, 4, c).cmd2_param == 0x21);
}

TEST_CASE("editor edits: host writes to 51..58 from a non-audio thread are still rejected")
{
    PluginRig r;
    r.p->set(ED_Table, 1);
    r.p->set(ED_Row, 0);
    r.block();
    Content before;
    r.p->content().readPublished(before);
    onEditorThread([&] {
        // As AEffect::setParameter from the host's UI thread.
        for (int i = ED_Vol; i <= ED_Val2; ++i) r.p->setNormalised(i, 0.77f);
    });
    r.block();
    r.block();
    Content after;
    r.p->content().readPublished(after);
    CHECK(std::memcmp(&before.tables, &after.tables, sizeof(before.tables)) == 0);
}
