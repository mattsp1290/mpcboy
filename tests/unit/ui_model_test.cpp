// UI WP2: the Mac editor's platform-free interaction model (src/ui/ui_model.h) and the
// parameter name / index agreement between the generated layout and the plugin (design step 10).
#include <cstring>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "param_bridge.h"
#include "table_bank.h"
#include "ui_model.h"

using namespace mpcboy;
namespace ui = mpcboy::ui;

namespace {

const std::vector<mpc::Param>& params()
{
    static const std::vector<mpc::Param> p = buildParams();
    return p;
}

ui::ParamInfo infoOf(int index)
{
    const mpc::Param& p = params()[static_cast<size_t>(index)];
    ui::ParamInfo i;
    switch (p.kind)
    {
        case mpc::Param::Kind::Bool: i.steps = 2; break;
        case mpc::Param::Kind::Choice: i.steps = static_cast<int>(p.choices.size()); break;
        case mpc::Param::Kind::Int: i.steps = static_cast<int>(p.max - p.min) + 1; break;
        default: i.steps = 0; break;
    }
    i.defaultNorm = p.toNormalised(p.def);
    return i;
}

std::vector<float> defaults()
{
    std::vector<float> v;
    for (const mpc::Param& p : params()) v.push_back(p.toNormalised(p.def));
    return v;
}

// Index of the control on `page` that drives parameter `param`.
int controlOf(int page, int param)
{
    const ui::Page& p = ui::page(page);
    for (int i = 0; i < p.numControls; ++i)
        if (p.controls[i].param == param) return i;
    return -1;
}

ui::Point centreOf(const ui::Rect& r) { return {r.x + r.w / 2, r.y + r.h / 2}; }

}  // namespace

TEST_CASE("ui 10: every control's parameter index has the name the plugin reports (layout / dump drift)")
{
    REQUIRE(ui::kNumPages == 6);
    int checked = 0;
    for (int pg = 0; pg < ui::kNumPages; ++pg)
    {
        const ui::Page& p = ui::page(pg);
        CAPTURE(p.name);
        for (int i = 0; i < p.numControls; ++i)
        {
            const ui::Control& c = p.controls[i];
            CAPTURE(c.paramName);
            REQUIRE(c.param >= 0);
            REQUIRE(c.param < kNumParams);
            CHECK(params()[static_cast<size_t>(c.param)].name == c.paramName);
            REQUIRE(c.component >= 0);
            REQUIRE(c.component < ui::kNumComponents);
            // Segment captions are the parameter's choices, in order.
            if (c.numCaptions > 0)
            {
                const mpc::Param& q = params()[static_cast<size_t>(c.param)];
                REQUIRE(static_cast<int>(q.choices.size()) == c.numCaptions);
                for (int k = 0; k < c.numCaptions; ++k) CHECK(q.choices[static_cast<size_t>(k)] == c.captions[k].text);
            }
            // visibleWhen points at a choice parameter with that many choices.
            if (c.visibleWhen.param >= 0)
            {
                const mpc::Param& d = params()[static_cast<size_t>(c.visibleWhen.param)];
                CHECK(static_cast<int>(d.choices.size()) == c.visibleWhen.count);
                CHECK(c.visibleWhen.choice < c.visibleWhen.count);
            }
            ++checked;
        }
    }
    CHECK(checked == 67);
}

TEST_CASE("ui: layout access - pages, components, child rects, assets, tab buttons")
{
    CHECK(std::strcmp(ui::page(-3).name, "MAIN") == 0);
    CHECK(std::strcmp(ui::page(99).name, "TABLE") == 0);
    const ui::Control& vol = ui::page(0).controls[controlOf(0, P_Volume)];
    CHECK(vol.kind == ui::WidgetKind::FaderUni);
    const ui::ChildDef* fader = ui::findChild(vol, ui::ChildType::Knob);
    REQUIRE(fader != nullptr);
    const ui::Rect r = ui::childRect(vol, *fader);
    CHECK(r.x == 23 + 15);
    CHECK(r.y == 45 + 54);
    CHECK(r.w == 248);
    CHECK(r.h == 36);
    const ui::Asset* a = ui::findAsset(fader->image);
    REQUIRE(a != nullptr);
    CHECK(a->frames == 127);
    CHECK(ui::findAsset("nope.png") == nullptr);
    const ui::Control& retrig = ui::page(0).controls[controlOf(0, P_Retrigger)];
    const ui::ChildDef* seg1 = ui::findChild(retrig, ui::ChildType::Button, 1);
    REQUIRE(seg1 != nullptr);
    CHECK(seg1->buttonId == 1);
    CHECK(seg1->buttonsInGroup == 2);
    // Tab buttons: inside the bar, non-overlapping, in page order.
    for (int i = 0; i < ui::kNumPages; ++i)
    {
        const ui::Rect b = ui::tabButtonRect(i);
        CHECK(b.y >= ui::kTabBar.bounds.y);
        CHECK(b.y + b.h <= ui::kTabBar.bounds.y + ui::kTabBar.bounds.h);
        CHECK(b.x + b.w <= ui::kEditorWidth);
        if (i > 0) CHECK(b.x >= ui::tabButtonRect(i - 1).x + ui::tabButtonRect(i - 1).w);
    }
    CHECK(ui::contentRect().h + ui::kTabBar.bounds.h == ui::kEditorHeight);
}

TEST_CASE("ui: values - steps, quantise, film-strip frames")
{
    CHECK(ui::stepOf(0.0f, 4) == 0);
    CHECK(ui::stepOf(1.0f, 4) == 3);
    CHECK(ui::stepOf(0.5f, 4) == 2);   // 1.5 rounds away from zero
    CHECK(ui::stepOf(-1.0f, 4) == 0);
    CHECK(ui::stepOf(0.7f, 0) == 0);
    CHECK(ui::normOfStep(2, 3) == doctest::Approx(1.0f));
    CHECK(ui::normOfStep(1, 3) == doctest::Approx(0.5f));
    CHECK(ui::normOfStep(9, 3) == doctest::Approx(1.0f));
    CHECK(ui::quantize(0.4f, 3) == doctest::Approx(0.5f));
    CHECK(ui::quantize(0.4f, 0) == doctest::Approx(0.4f));
    CHECK(ui::quantize(1.4f, 0) == doctest::Approx(1.0f));
    CHECK(ui::filmFrame(0.0f, 127) == 0);
    CHECK(ui::filmFrame(1.0f, 127) == 126);
    CHECK(ui::filmFrame(0.5f, 127) == 63);
    // Every integer step of a knob parameter lands on a distinct frame where there are enough.
    const ui::ParamInfo bend = infoOf(P_BendRange);   // 0..24
    CHECK(bend.steps == 25);
    int last = -1;
    for (int s = 0; s < bend.steps; ++s)
    {
        const int f = ui::filmFrame(ui::normOfStep(s, bend.steps), 127);
        CHECK(f > last);
        last = f;
    }
}

TEST_CASE("ui: visibleWhen follows the driving choice parameter")
{
    std::vector<float> v = defaults();
    const ui::Page& main = ui::page(0);
    const ui::Control& rate = main.controls[controlOf(0, P_TblRate)];
    const ui::Control& freeHz = main.controls[controlOf(0, P_FreeHz)];
    const ui::Control& vol = main.controls[controlOf(0, P_Volume)];
    v[P_TblClock] = 0.0f;   // Sync
    CHECK(ui::isVisible(rate, v.data(), static_cast<int>(v.size())));
    CHECK_FALSE(ui::isVisible(freeHz, v.data(), static_cast<int>(v.size())));
    CHECK(ui::isVisible(vol, v.data(), static_cast<int>(v.size())));
    v[P_TblClock] = 1.0f;   // Free
    CHECK_FALSE(ui::isVisible(rate, v.data(), static_cast<int>(v.size())));
    CHECK(ui::isVisible(freeHz, v.data(), static_cast<int>(v.size())));
    // NZ Shape only in Fixed pitch mode.
    const ui::Control& shape = ui::page(4).controls[controlOf(4, NZ_Shape)];
    v[NZ_PitchMode] = 0.0f;
    CHECK_FALSE(ui::isVisible(shape, v.data(), static_cast<int>(v.size())));
    v[NZ_PitchMode] = 1.0f;
    CHECK(ui::isVisible(shape, v.data(), static_cast<int>(v.size())));
    // No values: everything shows.
    CHECK(ui::isVisible(shape, nullptr, 0));
}

TEST_CASE("ui: hit-testing per widget kind uses the component child bounds; hidden controls are not hit")
{
    std::vector<float> v = defaults();
    const int n = static_cast<int>(v.size());
    const ui::Page& main = ui::page(0);

    // Fader: only the fader strip.
    const int volIdx = controlOf(0, P_Volume);
    const ui::Control& vol = main.controls[volIdx];
    ui::Point p = centreOf(ui::childRect(vol, *ui::findChild(vol, ui::ChildType::Knob)));
    ui::Hit h = ui::hitTest(0, p.x, p.y, v.data(), n);
    CHECK(h.control == volIdx);
    CHECK(h.part == ui::Part::Fader);
    h = ui::hitTest(0, vol.slot.x + 20, vol.slot.y + 20, v.data(), n);   // on the name label
    CHECK(h.part == ui::Part::None);

    // Knob: the dial and the labels.
    const int bendIdx = controlOf(0, P_BendRange);
    const ui::Control& bend = main.controls[bendIdx];
    p = centreOf(ui::childRect(bend, *ui::findChild(bend, ui::ChildType::Knob)));
    h = ui::hitTest(0, p.x, p.y, v.data(), n);
    CHECK(h.control == bendIdx);
    CHECK(h.part == ui::Part::Knob);
    p = centreOf(ui::childRect(bend, *ui::findChild(bend, ui::ChildType::Label)));
    CHECK(ui::hitTest(0, p.x, p.y, v.data(), n).part == ui::Part::Knob);

    // Segments: each button picks its choice; the gap between them hits nothing.
    const int retIdx = controlOf(0, P_Retrigger);
    const ui::Control& ret = main.controls[retIdx];
    for (int s = 0; s < 2; ++s)
    {
        p = centreOf(ui::childRect(ret, *ui::findChild(ret, ui::ChildType::Button, s)));
        h = ui::hitTest(0, p.x, p.y, v.data(), n);
        CHECK(h.control == retIdx);
        CHECK(h.part == ui::Part::Segment);
        CHECK(h.segment == s);
    }
    const ui::Rect s0 = ui::childRect(ret, *ui::findChild(ret, ui::ChildType::Button, 0));
    CHECK(ui::hitTest(0, s0.x + s0.w + 1, s0.y + 5, v.data(), n).part == ui::Part::None);

    // Toggle.
    const ui::Control& p1 = main.controls[controlOf(0, P1_On)];
    p = centreOf(ui::childRect(p1, *ui::findChild(p1, ui::ChildType::Button)));
    CHECK(ui::hitTest(0, p.x, p.y, v.data(), n).part == ui::Part::Toggle);

    // Slider (menu) visible in Sync only; Free Hz shares nothing with it.
    const ui::Control& rate = main.controls[controlOf(0, P_TblRate)];
    p = centreOf(ui::childRect(rate, *ui::findChild(rate, ui::ChildType::Slider)));
    v[P_TblClock] = 0.0f;
    CHECK(ui::hitTest(0, p.x, p.y, v.data(), n).part == ui::Part::Slider);
    v[P_TblClock] = 1.0f;
    CHECK(ui::hitTest(0, p.x, p.y, v.data(), n).part == ui::Part::None);
    const ui::Control& fh = main.controls[controlOf(0, P_FreeHz)];
    p = centreOf(ui::childRect(fh, *ui::findChild(fh, ui::ChildType::Knob)));
    CHECK(ui::hitTest(0, p.x, p.y, v.data(), n).part == ui::Part::Knob);

    // Empty slot and outside the page.
    CHECK(ui::hitTest(0, 1100, 550, v.data(), n).control == -1);
    CHECK(ui::hitTest(0, 5, 5, v.data(), n).control == -1);
}

TEST_CASE("ui: tab bar hit-testing and page switching")
{
    for (int i = 0; i < ui::kNumPages; ++i)
    {
        const ui::Point c = centreOf(ui::tabButtonRect(i));
        CHECK(ui::hitTab(c.x, c.y) == i);
    }
    const ui::Rect b0 = ui::tabButtonRect(0);
    CHECK(ui::hitTab(b0.x + b0.w + 2, b0.y + 10) == -1);   // between buttons
    CHECK(ui::hitTab(640, 300) == -1);                     // the page
    CHECK(ui::hitTab(b0.x + 5, ui::kEditorHeight + 3) == -1);
}

TEST_CASE("ui: knob gesture - vertical drag, 200 units full range, fine mode, clamped, quantised")
{
    const ui::Control& fh = ui::page(0).controls[controlOf(0, P_FreeHz)];
    ui::Hit hit{controlOf(0, P_FreeHz), ui::Part::Knob, -1};
    ui::Gesture g;
    float out = -1.0f;
    const ui::ParamInfo cont = infoOf(P_FreeHz);
    REQUIRE(cont.steps == 0);
    REQUIRE(g.press(fh, hit, 500, 300, false, 0.25f, cont, out));
    CHECK(g.active());
    CHECK(g.param() == P_FreeHz);
    CHECK_FALSE(g.discrete());
    CHECK(out == doctest::Approx(0.25f));
    CHECK(g.drag(500, 250, false, out));   // up 50 = +0.25
    CHECK(out == doctest::Approx(0.5f));
    CHECK(g.drag(500, 150, true, out));    // fine: up 100 = +0.05
    CHECK(out == doctest::Approx(0.55f));
    CHECK(g.drag(500, -1000, false, out));
    CHECK(out == doctest::Approx(1.0f));
    CHECK_FALSE(g.drag(500, -1100, false, out));   // clamped: no change
    CHECK(g.drag(500, -1000 + 200, false, out));   // the clamp does not build up a dead zone
    CHECK(out == doctest::Approx(0.0f));
    CHECK_FALSE(g.release(500, -800, false, out));
    CHECK_FALSE(g.active());

    // Discrete: Bend Range 0..24 moves in whole steps.
    const ui::ParamInfo bend = infoOf(P_BendRange);
    const ui::Control& bc = ui::page(0).controls[controlOf(0, P_BendRange)];
    REQUIRE(g.press(bc, {controlOf(0, P_BendRange), ui::Part::Knob, -1}, 0, 0, false, ui::normOfStep(2, 25), bend, out));
    CHECK(g.drag(0, -25, false, out));   // +0.125 = 3 steps
    CHECK(ui::stepOf(out, 25) == 5);
    CHECK(out == doctest::Approx(ui::normOfStep(5, 25)));
}

TEST_CASE("ui: bipolar knob - the centre is the default and the drag is symmetric")
{
    const int idx = controlOf(1, P1_Tsp);
    const ui::Control& c = ui::page(1).controls[idx];
    CHECK(c.kind == ui::WidgetKind::KnobBi);
    const ui::ParamInfo tsp = infoOf(P1_Tsp);   // -24..24
    CHECK(tsp.steps == 49);
    CHECK(tsp.defaultNorm == doctest::Approx(0.5f));
    CHECK(ui::filmFrame(tsp.defaultNorm, 127) == 63);   // the centre frame
    ui::Gesture g;
    float out = 0;
    REQUIRE(g.press(c, {idx, ui::Part::Knob, -1}, 0, 0, false, tsp.defaultNorm, tsp, out));
    CHECK(g.drag(0, 100, false, out));
    CHECK(out == doctest::Approx(0.0f));
    CHECK(g.drag(0, -100, false, out));
    CHECK(out == doctest::Approx(1.0f));
}

TEST_CASE("ui: fader gesture - absolute horizontal, fine is relative")
{
    const int idx = controlOf(0, P_Volume);
    const ui::Control& c = ui::page(0).controls[idx];
    const ui::Rect f = ui::childRect(c, *ui::findChild(c, ui::ChildType::Knob));
    const ui::ParamInfo info = infoOf(P_Volume);
    ui::Gesture g;
    float out = 0;
    REQUIRE(g.press(c, {idx, ui::Part::Fader, -1}, f.x + ui::kFaderTravelX0 + ui::kFaderTravelLen / 2, f.y + 5, false, 0.1f, info, out));
    CHECK(out == doctest::Approx(0.5f));
    CHECK(g.drag(f.x + ui::kFaderTravelX0, f.y + 5, false, out));
    CHECK(out == doctest::Approx(0.0f));
    CHECK(g.drag(f.x + f.w + 40, f.y + 90, false, out));
    CHECK(out == doctest::Approx(1.0f));
    CHECK(g.drag(f.x + f.w + 40 - ui::kFaderTravelLen, f.y, true, out));   // fine: relative, a tenth of the speed
    CHECK(out == doctest::Approx(0.9f));
    g.cancel();
    CHECK_FALSE(g.active());

    // Fine press keeps the value, then moves at a tenth of the speed.
    REQUIRE(g.press(c, {idx, ui::Part::Fader, -1}, f.x + 5, f.y + 5, true, 0.5f, info, out));
    CHECK(out == doctest::Approx(0.5f));
    CHECK(g.drag(f.x + 5 + ui::kFaderTravelLen, f.y + 5, true, out));
    CHECK(out == doctest::Approx(0.6f));
}

TEST_CASE("ui: toggle flips on click; segments pick their choice; both are discrete")
{
    const int idx = controlOf(0, P1_On);
    const ui::Control& c = ui::page(0).controls[idx];
    ui::Gesture g;
    float out = -1;
    REQUIRE(g.press(c, {idx, ui::Part::Toggle, -1}, 0, 0, false, 0.0f, infoOf(P1_On), out));
    CHECK(g.discrete());
    CHECK(out == 1.0f);
    REQUIRE(g.press(c, {idx, ui::Part::Toggle, -1}, 0, 0, false, 1.0f, infoOf(P1_On), out));
    CHECK(out == 0.0f);
    CHECK_FALSE(g.drag(100, 100, false, out));   // dragging does nothing

    const int dIdx = controlOf(1, P1_Duty);
    const ui::Control& duty = ui::page(1).controls[dIdx];
    CHECK(duty.kind == ui::WidgetKind::Radio4);
    for (int s = 0; s < 4; ++s)
    {
        REQUIRE(g.press(duty, {dIdx, ui::Part::Segment, s}, 0, 0, false, 0.0f, infoOf(P1_Duty), out));
        CHECK(g.discrete());
        CHECK(ui::stepOf(out, 4) == s);
        CHECK(out == doctest::Approx(ui::normOfStep(s, 4)));
    }
}

TEST_CASE("ui: menu slider - a click steps (shift steps back, both wrap); a drag steps by distance")
{
    const int idx = controlOf(0, P_TblRate);
    const ui::Control& c = ui::page(0).controls[idx];
    CHECK(c.menu);
    const ui::ParamInfo info = infoOf(P_TblRate);
    const int n = info.steps;
    REQUIRE(n >= 4);
    ui::Gesture g;
    float out = 0;
    // Click: +1.
    REQUIRE(g.press(c, {idx, ui::Part::Slider, -1}, 400, 400, false, ui::normOfStep(1, n), info, out));
    CHECK_FALSE(g.drag(401, 401, false, out));   // within the click slop
    CHECK(g.release(401, 401, false, out));
    CHECK(ui::stepOf(out, n) == 2);
    // Click at the last value wraps to the first.
    REQUIRE(g.press(c, {idx, ui::Part::Slider, -1}, 400, 400, false, 1.0f, info, out));
    CHECK(g.release(400, 400, false, out));
    CHECK(ui::stepOf(out, n) == 0);
    // Shift-click at the first value wraps to the last.
    REQUIRE(g.press(c, {idx, ui::Part::Slider, -1}, 400, 400, true, 0.0f, info, out));
    CHECK(g.release(400, 400, true, out));
    CHECK(ui::stepOf(out, n) == n - 1);
    // Drag 3 steps right, then back left past the start (clamped at 0).
    REQUIRE(g.press(c, {idx, ui::Part::Slider, -1}, 400, 400, false, ui::normOfStep(1, n), info, out));
    CHECK(g.drag(400 + 3 * ui::kSliderStepPx, 400, false, out));
    CHECK(ui::stepOf(out, n) == 4);
    CHECK(g.drag(400 - 5 * ui::kSliderStepPx, 400, false, out));
    CHECK(ui::stepOf(out, n) == 0);
    CHECK_FALSE(g.release(400 - 5 * ui::kSliderStepPx, 400, false, out));   // a drag is not a click
}

TEST_CASE("ui: pixel font - glyphs, width, placement and rasterisation")
{
    CHECK(&ui::glyph('a') == &ui::glyph('A'));
    CHECK(ui::glyph('~').ch == '?');
    CHECK(ui::glyph('>').ch == '>');
    CHECK(ui::textWidth("", 3) == 0);
    CHECK(ui::textWidth(nullptr, 3) == 0);
    CHECK(ui::textWidth("AB", 2) == 22);
    CHECK(ui::labelScale(20.4f) == 2);
    CHECK(ui::labelScale(25.5f) == 3);

    const ui::Rect b{100, 50, 100, 30};
    ui::Point p = ui::placeText("AB", b, 2, ui::Justify::Left);
    CHECK(p.x == 100);
    CHECK(p.y == 50 + (30 - 14) / 2);
    p = ui::placeText("AB", b, 2, ui::Justify::Right);
    CHECK(p.x == 200 - 22);
    p = ui::placeText("AB", b, 2, ui::Justify::Centre);
    CHECK(p.x == 100 + (100 - 22) / 2);
    // Wider than the bounds: floor division, as the generator's previews.
    p = ui::placeText("ABCDEFGHIJ", {0, 0, 10, 10}, 3, ui::Justify::Centre);
    CHECK(p.x == -(177 - 10 + 1) / 2);
    CHECK(p.y == -6);

    // Every lit pixel of "1" is covered exactly once.
    int bits = 0;
    for (int r = 0; r < ui::kPixelFontHeight; ++r)
        for (int c = 0; c < ui::kPixelFontWidth; ++c)
            if (ui::glyph('1').rows[r] & (1 << (ui::kPixelFontWidth - 1 - c))) ++bits;
    long long area = 0;
    int minX = 1 << 30, maxX = -1;
    ui::rasterizeText("1", 10, 20, 3, nullptr, [&](int x, int y, int w, int h) {
        area += static_cast<long long>(w) * h;
        minX = std::min(minX, x);
        maxX = std::max(maxX, x + w);
        CHECK(h == 3);
        CHECK(y >= 20);
        CHECK(y + h <= 20 + 21);
    });
    CHECK(area == bits * 9);
    CHECK(minX >= 10);
    CHECK(maxX <= 10 + 15);

    // Clipping.
    const ui::Rect clip{10, 20, 6, 6};
    area = 0;
    ui::rasterizeText("8", 10, 20, 3, &clip, [&](int x, int y, int w, int h) {
        CHECK(x >= 10);
        CHECK(y >= 20);
        CHECK(x + w <= 16);
        CHECK(y + h <= 26);
        area += static_cast<long long>(w) * h;
    });
    CHECK(area > 0);
    CHECK(area <= 36);
}

TEST_CASE("ui: table cell text follows the plugin's formatValue rules")
{
    char b[8];
    std::string fv;
    ui::formatVolume(false, 0x33, b);
    CHECK(std::string(b) == "--");
    // The grid keeps "--"; the Row Vol parameter reads "Keep" for the same "no volume column" state.
    REQUIRE(formatParamValue(ED_Vol, -1.0f, fv));
    CHECK(fv == "Keep");
    for (int v : {0, 1, 0x0f, 0x10, 0x7f, 0xa0, 0xff})
    {
        ui::formatVolume(true, v, b);
        REQUIRE(formatParamValue(ED_Vol, static_cast<float>(v), fv));
        CHECK(fv == b);
        ui::formatHex2(v, b);
        REQUIRE(formatParamValue(ED_Val1, static_cast<float>(v), fv));
        CHECK(fv == b);
    }
    ui::formatTranspose(0, b);
    CHECK(std::string(b) == "+00");
    ui::formatTranspose(-5, b);
    CHECK(std::string(b) == "-05");
    ui::formatTranspose(12, b);
    CHECK(std::string(b) == "+12");
    ui::formatTranspose(-100, b);
    CHECK(std::string(b) == "-48");
    // Command letters: the Row Cmd choices are the letter plus a word ("D Delay"); the grid shows the letter.
    const mpc::Param& cmd = params()[ED_Cmd1];
    for (int i = 0; i < kNumCmdChoices; ++i)
    {
        ui::formatCommand(cmdByteFromIndex(i), b);
        CHECK(cmd.choices[static_cast<size_t>(i)].substr(0, 1) == b);
    }
    ui::formatCommand('Z', b);
    CHECK(std::string(b) == "-");
}

TEST_CASE("ui: table view - header plus 8 rows around the cursor, cursor row flagged")
{
    Content c;
    clearContent(c);
    auto& row = c.tables.tables[2].rows[5];
    row.has_vol = true;
    row.vol = 0x1f;
    row.transpose = -5;
    row.cmd1 = 'K';
    row.cmd1_param = 0xa0;
    row.cmd2 = 'P';
    row.cmd2_param = 0x03;

    ui::TableView v;
    ui::buildTableView(c, 2, 5, v);
    CHECK(std::string(v.header.text) == " ROW VOL TSP  CMD VAL  CMD VAL");
    CHECK(v.header.row == -1);
    REQUIRE(v.numRows == 8);
    CHECK(v.rows[0].row == 2);
    CHECK(v.rows[7].row == 9);
    CHECK(v.rows[3].cursor);
    int cursors = 0;
    for (int i = 0; i < v.numRows; ++i) cursors += v.rows[i].cursor ? 1 : 0;
    CHECK(cursors == 1);
    CHECK(std::string(v.rows[3].text) == " 05  1F  -05  K   A0   P   03");
    CHECK(std::string(v.rows[0].text) == " 02  --  +00  -   00   -   00");
    // Column positions are the layout's character columns.
    for (int k = 0; k < ui::kTableGrid.numColumns; ++k)
        CHECK(v.header.text[ui::kTableGrid.columns[k].x] == ui::kTableGrid.columns[k].title[0]);

    // The window clamps at both ends.
    CHECK(ui::tableFirstRow(0) == 0);
    CHECK(ui::tableFirstRow(3) == 0);
    CHECK(ui::tableFirstRow(4) == 1);
    CHECK(ui::tableFirstRow(15) == 8);
    ui::buildTableView(c, 2, 15, v);
    CHECK(v.rows[7].row == 15);
    CHECK(v.rows[7].cursor);
    ui::buildTableView(c, 99, -4, v);   // clamped table and row
    CHECK(v.rows[0].row == 0);
    CHECK(v.rows[0].cursor);
}

TEST_CASE("ui: table grid geometry matches the LCD panel art")
{
    const ui::Rect panel = ui::kTableGrid.panelBounds;
    const ui::Rect hb = ui::tableHeaderBand();
    CHECK(hb.x == ui::kTableGrid.textX - 2);
    CHECK(hb.w == panel.w - 2 * (ui::kTableGrid.textX - panel.x) + 4);
    CHECK(hb.h == ui::kPixelFontHeight * ui::kTableGrid.charScale + 4);
    CHECK(hb.x + hb.w <= panel.x + panel.w);
    const ui::Rect cb = ui::tableCursorBand(8);
    CHECK(cb.y + cb.h <= panel.y + panel.h);
    CHECK(ui::tableLineY(1) == ui::kTableGrid.textY + ui::kTableGrid.rowPitch);
    CHECK(ui::tableCharX(1) == ui::kTableGrid.textX + 18);
    // The longest line fits inside the panel.
    CHECK(ui::tableCharX(30) <= panel.x + panel.w);
}
