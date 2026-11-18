// UI WP2: the Mac editor's platform-free interaction model (src/ui/ui_model.h) and the
// parameter name / index agreement between the generated layout and the plugin (design step 10).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "factory_content.h"
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
        // Dynamics (film strips, value labels) are bound by index too.
        for (int i = 0; i < p.numDynamics; ++i)
        {
            const ui::Dynamic& d = p.dynamics[i];
            CAPTURE(d.paramName);
            REQUIRE(d.param >= 0);
            REQUIRE(d.param < kNumParams);
            CHECK(params()[static_cast<size_t>(d.param)].name == d.paramName);
        }
    }
    CHECK(checked == 71);
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
    CHECK(r.x == 23 + 15);    // MAIN row 3 (MASTER), column 0
    CHECK(r.y == 497 + 54);
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

    // Toggle (PU1 On lives on PULSE 1, row 0 column 3).
    const ui::Control& p1 = ui::page(1).controls[controlOf(1, P1_On)];
    p = centreOf(ui::childRect(p1, *ui::findChild(p1, ui::ChildType::Button)));
    CHECK(ui::hitTest(1, p.x, p.y, v.data(), n).part == ui::Part::Toggle);

    // Slider (menu) visible in Tempo only; in Free its slot belongs to Free Rate (shared slot).
    const ui::Control& rate = main.controls[controlOf(0, P_TblRate)];
    p = centreOf(ui::childRect(rate, *ui::findChild(rate, ui::ChildType::Slider)));
    v[P_TblClock] = 0.0f;
    CHECK(ui::hitTest(0, p.x, p.y, v.data(), n).part == ui::Part::Slider);
    v[P_TblClock] = 1.0f;
    CHECK(ui::hitTest(0, p.x, p.y, v.data(), n).control != controlOf(0, P_TblRate));
    const ui::Control& fh = main.controls[controlOf(0, P_FreeHz)];
    p = centreOf(ui::childRect(fh, *ui::findChild(fh, ui::ChildType::Knob)));
    CHECK(ui::hitTest(0, p.x, p.y, v.data(), n).part == ui::Part::Knob);

    // The row-0 LCD of a channel page, a blank slot, and outside the slots.
    CHECK(ui::hitTest(1, 300, 100, v.data(), n).control == -1);
    CHECK(ui::hitTest(2, 100, 550, v.data(), n).control == -1);   // PULSE 2: SWEEP: PU1 ONLY
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
    const int idx = controlOf(1, P1_On);
    const ui::Control& c = ui::page(1).controls[idx];
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

TEST_CASE("ui: TABLE grid constants come from the generated layout and fit the panel")
{
    // The live grid's numbers are generated (grid.live in ui/mpcboy_ui.json); ui_model.cpp
    // static_asserts the fit, this pins the layout the editor relies on.
    const ui::Rect panel = ui::kTableGrid.panelBounds;
    static_assert(ui::kGridRows == 16, "the editor's table has 16 rows");
    CHECK(ui::kGridHeaderHeight + 1 + ui::kGridRows * ui::kGridPitch <= panel.h - 2 * ui::kGridFieldInset);
    CHECK(ui::kGridPitch >= ui::kPixelFontHeight * ui::kGridScale);
    // The longest text line fits inside the panel.
    CHECK(ui::gridCharX(ui::kGridLastChar) <= panel.x + panel.w);
    // The MPC art's text origin is inside the panel.
    CHECK(ui::kTableGrid.textX > panel.x);
    CHECK(ui::kTableGrid.textY > panel.y);
}

TEST_CASE("ui: redesign layout - MAIN mixer rows, channel-page grammar, TABLE LCD at columns 0-1")
{
    // MAIN: LEVEL / TABLES / TABLE CLOCK / MASTER, one row each.
    const int levels[] = {P1_Vol, P2_Vol, WV_Level, NZ_Vol};
    const int tables[] = {P1_Tbl, P2_Tbl, WV_Tbl, NZ_Tbl};
    const int clock[] = {P_TblClock, P_TblRate, P_TblPhase, P_VelVol};
    const int master[] = {P_Volume, P_BendRange, P_Retrigger, P_NoteOff};
    const int* rows[] = {levels, tables, clock, master};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            const ui::Control& k = ui::page(0).controls[controlOf(0, rows[r][c])];
            CHECK(k.slot.x == 23 + 320 * c);
            CHECK(k.slot.y == (r == 0 ? 45 : r == 1 ? 196 : r == 2 ? 346 : 497));
        }
    // Channel pages: row 0 is the LCD (no controls in columns 0-2) and the On switch in column 3;
    // column 3 is always On / Table / Length / Pan.
    const int col3[4][4] = {{P1_On, P1_Tbl, P1_Len, P1_Pan}, {P2_On, P2_Tbl, P2_Len, P2_Pan},
                            {WV_On, WV_Tbl, WV_Len, WV_Pan}, {NZ_On, NZ_Tbl, NZ_Len, NZ_Pan}};
    for (int pg = 1; pg <= 4; ++pg)
    {
        const ui::Page& p = ui::page(pg);
        CAPTURE(p.name);
        REQUIRE(p.numPanels == 1);
        CHECK(p.panels[0].kind == ui::PanelKind::LcdChannel);
        CHECK(p.panels[0].bounds.x == 23);
        CHECK(p.panels[0].bounds.w == 983 - 23 - 42);   // columns 0-2
        for (int i = 0; i < p.numControls; ++i)
            CHECK_FALSE((p.controls[i].slot.y == 45 && p.controls[i].slot.x < 983));
        for (int r = 0; r < 4; ++r)
        {
            const ui::Control& k = p.controls[controlOf(pg, col3[pg - 1][r])];
            CHECK(k.slot.x == 983);
            CHECK(k.slot.y == (r == 0 ? 45 : r == 1 ? 196 : r == 2 ? 346 : 497));
        }
    }
    // TABLE: the LCD covers columns 0-1 of rows 0-1, left of the Q-Link sidebar (x >= 1025).
    const ui::Page& t = ui::page(5);
    CHECK(ui::kTableGrid.page == 5);
    CHECK(ui::kTableGrid.panelBounds.x == 23);
    CHECK(ui::kTableGrid.panelBounds.y == 45);
    CHECK(ui::kTableGrid.panelBounds.x + ui::kTableGrid.panelBounds.w < 1025);
    for (int i = 0; i < t.numControls; ++i)
        CHECK_FALSE(ui::contains(ui::kTableGrid.panelBounds, t.controls[i].slot.x + 1, t.controls[i].slot.y + 1));
    CHECK(t.controls[controlOf(5, ED_Table)].slot.x == 663);
    CHECK(t.controls[controlOf(5, ED_Row)].slot.x == 983);
    CHECK(t.controls[controlOf(5, Imp_File)].slot.y == 497);
}

TEST_CASE("ui: overlapped slot - Table Rate and Free Rate share one slot with complementary visibleWhen")
{
    std::vector<float> v = defaults();
    const int n = static_cast<int>(v.size());
    const int ri = controlOf(0, P_TblRate), fi = controlOf(0, P_FreeHz);
    const ui::Control& rate = ui::page(0).controls[ri];
    const ui::Control& free = ui::page(0).controls[fi];
    CHECK(rate.slot.x == free.slot.x);
    CHECK(rate.slot.y == free.slot.y);
    CHECK(rate.slot.w == free.slot.w);
    CHECK(rate.slot.h == free.slot.h);
    CHECK(rate.visibleWhen.param == P_TblClock);
    CHECK(free.visibleWhen.param == P_TblClock);
    CHECK(rate.visibleWhen.choice != free.visibleWhen.choice);
    const ui::Point c = centreOf(rate.slot);
    for (int sync = 0; sync < 2; ++sync)
    {
        v[P_TblClock] = static_cast<float>(sync);
        // Exactly one of the two is visible, and a click in the slot reaches that one.
        CHECK(ui::isVisible(rate, v.data(), n) != ui::isVisible(free, v.data(), n));
        const ui::Point k = centreOf(ui::childRect(free, *ui::findChild(free, ui::ChildType::Knob)));
        const ui::Hit hk = ui::hitTest(0, k.x, k.y, v.data(), n);
        const ui::Point s = centreOf(ui::childRect(rate, *ui::findChild(rate, ui::ChildType::Slider)));
        const ui::Hit hs = ui::hitTest(0, s.x, s.y, v.data(), n);
        if (sync == 0)
        {
            CHECK(hs.control == ri);
            CHECK(hs.part == ui::Part::Slider);
            CHECK(hk.control != fi);
        }
        else
        {
            CHECK(hk.control == fi);
            CHECK(hk.part == ui::Part::Knob);
            CHECK(hs.control != ri);
        }
        CHECK(ui::hitTest(0, c.x, c.y, v.data(), n).control != (sync == 0 ? fi : ri));
    }
}

TEST_CASE("ui: Q-Links bank by screen row; badges follow slot_qlink; focus rectangles wrap one row each")
{
    CHECK(ui::kQLinkBankDirection == ui::BankDirection::Row);
    // Bank order for "Row" is bottom row first (ui-reference 8.3): kQLinkBounds[b] wraps screen row 3 - b.
    for (int b = 0; b < 4; ++b)
    {
        const ui::Rect q = ui::kQLinkBounds[b];
        const int row = 3 - b;
        const int ys[] = {45, 196, 346, 497};
        CHECK(q.y < ys[row]);
        CHECK(q.y + q.h > ys[row] + 107);
        CHECK(q.x < 23);
        CHECK(q.x + q.w > 983 + 278);
        if (b > 0) CHECK(q.y + q.h <= ui::kQLinkBounds[b - 1].y);   // no overlap between rows
    }
    for (int pg = 0; pg < ui::kNumPages; ++pg)
    {
        const ui::Page& p = ui::page(pg);
        CAPTURE(p.name);
        bool used[17] = {};
        for (int i = 0; i < p.numControls; ++i)
        {
            const ui::Control& c = p.controls[i];
            const int row = c.slot.y == 45 ? 0 : c.slot.y == 196 ? 1 : c.slot.y == 346 ? 2 : 3;
            const int col = (c.slot.x - 23) / 320;
            if (c.qlink == 0)
            {
                CHECK(c.visibleWhen.param >= 0);   // only an overlapped control gives up its Q-Link
                continue;
            }
            CHECK(c.qlink == 13 + col - 4 * row);
            CHECK_FALSE(used[c.qlink]);
            used[c.qlink] = true;
            // The control's row rectangle is the one for its bank.
            const ui::Rect q = ui::kQLinkBounds[(c.qlink - 1) / 4];
            CHECK(ui::contains(q, c.slot.x, c.slot.y));
            CHECK(ui::contains(q, c.slot.x + c.slot.w - 1, c.slot.y + c.slot.h - 1));
        }
    }
    CHECK(ui::page(0).controls[controlOf(0, P_TblRate)].qlink == 6);
    CHECK(ui::page(0).controls[controlOf(0, P_FreeHz)].qlink == 0);
}

TEST_CASE("ui: per-page kit variants and accents")
{
    const ui::Page& main = ui::page(0);
    CHECK(main.accent == ui::page(5).accent);   // MAIN and TABLE: the DMG accent
    for (int a = 1; a <= 4; ++a)
        for (int b = a + 1; b <= 4; ++b) CHECK(ui::page(a).accent != ui::page(b).accent);
    // MAIN's channel strips carry their channel colour; the rest of MAIN the page colour.
    const int levels[] = {P1_Vol, P2_Vol, WV_Level, NZ_Vol};
    for (int ch = 0; ch < 4; ++ch) CHECK(main.controls[controlOf(0, levels[ch])].accent == ui::page(1 + ch).accent);
    CHECK(main.controls[controlOf(0, P_Volume)].accent == main.accent);
    // Each page's kit images are its colour's variants (knob_uni_pu1.png ...).
    const char* suffix[] = {"_dmg.png", "_pu1.png", "_pu2.png", "_wav.png", "_noi.png", "_dmg.png"};
    for (int pg = 1; pg < ui::kNumPages; ++pg)
    {
        const ui::Page& p = ui::page(pg);
        for (int i = 0; i < p.numControls; ++i)
        {
            const ui::ComponentDef& d = ui::componentOf(p.controls[i]);
            for (int k = 0; k < d.numChildren; ++k)
            {
                const ui::ChildDef& ch = d.children[k];
                if (ch.type != ui::ChildType::Knob && ch.type != ui::ChildType::Button) continue;
                const std::string img = ch.image;
                CAPTURE(img);
                CHECK(img.size() > 8);
                CHECK(img.compare(img.size() - 8, 8, suffix[pg]) == 0);
                const ui::Asset* a = ui::findAsset(ch.image);
                REQUIRE(a != nullptr);
                if (ch.offImage != nullptr) CHECK(ui::findAsset(ch.offImage) != nullptr);
            }
        }
    }
}

TEST_CASE("ui: MPC dynamics - film strips match their parameter's steps; TABLE labels are left to the live grid")
{
    int strips = 0, labels = 0;
    for (int pg = 0; pg < ui::kNumPages; ++pg)
    {
        const ui::Page& p = ui::page(pg);
        for (int i = 0; i < p.numDynamics; ++i)
        {
            const ui::Dynamic& d = p.dynamics[i];
            CAPTURE(d.paramName);
            CHECK(d.bounds.x >= 0);
            CHECK(d.bounds.y >= 0);
            CHECK(d.bounds.x + d.bounds.w <= ui::kTabWidth);
            CHECK(d.bounds.y + d.bounds.h <= ui::kTabHeight);
            if (d.kind == ui::DynamicKind::FilmStrip)
            {
                ++strips;
                const ui::Asset* a = ui::findAsset(d.image);
                REQUIRE(a != nullptr);
                CHECK(a->frames == d.frames);
                CHECK(a->width == d.bounds.w);
                CHECK(a->height == d.bounds.h);
                CHECK(infoOf(d.param).steps == d.frames);   // every value has its own frame
                for (int s = 0; s < d.frames; ++s) CHECK(ui::filmFrame(ui::normOfStep(s, d.frames), d.frames) == s);
                CHECK(d.editor);
            }
            else
            {
                ++labels;
                CHECK(d.editor == (pg != ui::kTableGrid.page));
                if (!d.editor) CHECK(ui::contains(ui::kTableGrid.panelBounds, d.bounds.x, d.bounds.y));
            }
        }
    }
    CHECK(strips == 4 + 4 + 4 + 2 + 3);   // MAIN LEDs; PU1, PU2: LED, duty, env, dir; WAV: LED, shape; NOI: LED, env, dir
    CHECK(labels == 4 + 7);               // the channel LCDs' table names; TABLE's cursor row
}

TEST_CASE("ui: the level fader's 15 cells - every level 0..15 lands on a frame that lights that many cells")
{
    CHECK(ui::kFaderCells == 15);
    const ui::ParamInfo lvl = infoOf(P1_Vol);
    REQUIRE(lvl.steps == 16);
    for (int k = 0; k < lvl.steps; ++k)
    {
        const int f = ui::filmFrame(ui::normOfStep(k, lvl.steps), 127);
        CHECK(static_cast<int>(std::floor(f / 126.0 * ui::kFaderCells + 0.5)) == k);
    }
}

// ---- redesign step 3: the Mac editor's live LCDs, TABLE grid, breadcrumb, captions ----------
TEST_CASE("ui: TABLE grid - 16 rows fit the LCD panel; hex row numbers, beat rows, column caret extents")
{
    const ui::GridLayout g = ui::gridLayout();
    const ui::Rect pb = ui::kTableGrid.panelBounds;
    // Inside the panel at cols 0-1, rows 0-1, left of the MPC Q-Link sidebar.
    CHECK(pb.x == 23);
    CHECK(pb.w == 598);
    CHECK(g.field.x >= pb.x);
    CHECK(g.field.y >= pb.y);
    CHECK(g.field.x + g.field.w <= pb.x + pb.w);
    CHECK(g.field.y + g.field.h <= pb.y + pb.h);
    // Every row band is inside the field, rows are contiguous, glyphs fit their band.
    for (int r = 0; r < ui::kGridRows; ++r)
    {
        CAPTURE(r);
        const ui::Rect b = ui::gridRowBand(r);
        CHECK(b.y == g.rowsY + r * ui::kGridPitch);
        CHECK(b.h == ui::kGridPitch);
        CHECK(b.y + b.h <= g.field.y + g.field.h);
        CHECK(ui::gridTextY(r) >= b.y);
        CHECK(ui::gridTextY(r) + ui::kPixelFontHeight * ui::kGridScale <= b.y + b.h);
        CHECK(b.x + b.w <= g.side.x);
    }
    CHECK(g.headerY >= g.field.y);
    CHECK(g.rowsY >= g.headerY + 7 * ui::kGridHeaderScale);
    // The side column (name, chips) is inside the field and right of the text.
    CHECK(g.side.w >= 150);
    CHECK(g.side.x + g.side.w <= g.field.x + g.field.w);
    // Beat rows every 4th row.
    CHECK(ui::isBeatRow(0));
    CHECK(ui::isBeatRow(4));
    CHECK(ui::isBeatRow(12));
    CHECK_FALSE(ui::isBeatRow(3));
    // Column extents tile the row left to right; the caret covers exactly the column's text.
    for (int c = 0; c + 1 < ui::kTableGrid.numColumns; ++c)
    {
        CAPTURE(c);
        const ui::Rect a = ui::gridCellRect(5, c), b = ui::gridCellRect(5, c + 1);
        CHECK(a.x + a.w == b.x);
        const ui::Rect caret = ui::gridCaretRect(5, c);
        CHECK(caret.x >= a.x);
        CHECK(caret.x + caret.w <= a.x + a.w);
    }
    CHECK(ui::gridEditorColumn(0) == -1);
    CHECK(ui::gridEditorColumn(1) == 0);   // VOL -> Row Vol
    CHECK(ui::gridEditorColumn(6) == 5);   // VAL 2 -> Row Val2
}

TEST_CASE("ui: TABLE grid - rows show the factory table in hex; the cursor row is flagged")
{
    Content c;
    loadFactoryContent(c);
    ui::TableLine rows[ui::kGridRows];
    ui::buildGridRows(c, 0, 2, rows);   // MajArp, cursor on row 2
    for (int r = 0; r < ui::kGridRows; ++r)
    {
        CAPTURE(r);
        CHECK(rows[r].row == r);
        CHECK(rows[r].cursor == (r == 2));
        char hex[3];
        ui::formatHex2(r, hex);
        CHECK(std::strncmp(rows[r].text + ui::kTableGrid.columns[0].x, hex, 2) == 0);
    }
    CHECK(std::string(rows[10].text).substr(ui::kTableGrid.columns[0].x, 2) == "0A");
    CHECK(std::string(rows[3].text).find("-07") != std::string::npos);
    CHECK(std::string(rows[3].text).find("H") != std::string::npos);
}

TEST_CASE("ui: TABLE grid - a click maps to its row and column; outside the rows nothing")
{
    for (int r = 0; r < ui::kGridRows; ++r)
        for (int c = 0; c < ui::kTableGrid.numColumns; ++c)
        {
            CAPTURE(r);
            CAPTURE(c);
            const ui::Rect cell = ui::gridCellRect(r, c);
            const ui::GridHit h = ui::hitGrid(cell.x + cell.w / 2, cell.y + cell.h / 2);
            CHECK(h.row == r);
            CHECK(h.column == c);
            // Band edges: the first and last unit of a band belong to that row.
            CHECK(ui::hitGrid(cell.x + 1, cell.y).row == r);
            CHECK(ui::hitGrid(cell.x + 1, cell.y + cell.h - 1).row == r);
        }
    const ui::GridLayout g = ui::gridLayout();
    CHECK(ui::hitGrid(g.field.x + 20, g.headerY + 2).row == -1);    // the header
    CHECK(ui::hitGrid(g.side.x + 10, g.rowsY + 30).row == -1);      // the side column
    CHECK(ui::hitGrid(700, 100).row == -1);                         // Edit Table's slot
    // The grid sits on the TABLE page's LCD, where no control is hit.
    const std::vector<float> d = defaults();
    const ui::Rect cell = ui::gridCellRect(7, 2);
    CHECK(ui::hitTest(ui::kTableGrid.page, cell.x + 4, cell.y + 4, d.data(), static_cast<int>(d.size())).control == -1);
}

TEST_CASE("ui: breadcrumb text and its top-band area")
{
    char b[32];
    ui::channelBreadcrumb(b, sizeof(b), 0, "MajArp", 2);
    CHECK(std::string(b) == "PU1 > MAJARP ROW 02");
    ui::channelBreadcrumb(b, sizeof(b), 2, "Table 09", 11);
    CHECK(std::string(b) == "WAV > TABLE 09 ROW 0B");
    ui::channelBreadcrumb(b, sizeof(b), 3, "Hat", -1);   // assigned, not running
    CHECK(std::string(b) == "NOI > HAT");
    ui::channelBreadcrumb(b, sizeof(b), 1, nullptr, 4);
    CHECK(std::string(b) == "PU2 > TABLE OFF");
    ui::channelBreadcrumb(b, sizeof(b), 1, "", 4);
    CHECK(std::string(b) == "PU2 > TABLE OFF");
    ui::tableBreadcrumb(b, sizeof(b), 1, 2);
    CHECK(std::string(b) == "TABLE 01 > ROW 02");
    ui::tableBreadcrumb(b, sizeof(b), 16, 15);
    CHECK(std::string(b) == "TABLE 16 > ROW 0F");
    char small[6];
    ui::channelBreadcrumb(small, sizeof(small), 0, "MajArp", 2);
    CHECK(std::string(small) == "PU1 >");   // truncated, terminated

    for (int p = 0; p < ui::kNumPages; ++p)
    {
        CAPTURE(p);
        const ui::Rect r = ui::breadcrumbRect(p);
        // After the title, in the top band, left of the wordmark and the TABLE page's legend.
        CHECK(r.x >= ui::kTitleX + ui::textWidth(ui::page(p).title, ui::kTitleScale));
        CHECK(r.y >= 0);
        CHECK(r.y + r.h <= ui::page(p).controls[0].slot.y + 8);
        CHECK(r.x + r.w <= 662);
    }
}

TEST_CASE("ui: channel LCD - page channels, sections inside the panel, note names, envelope, noise")
{
    CHECK(ui::pageChannel(0) == -1);   // MAIN
    CHECK(ui::pageChannel(1) == 0);
    CHECK(ui::pageChannel(2) == 1);
    CHECK(ui::pageChannel(3) == 2);
    CHECK(ui::pageChannel(4) == 3);
    CHECK(ui::pageChannel(5) == -1);   // TABLE has a table LCD, not a channel LCD
    for (int p = 1; p <= 4; ++p)
    {
        CAPTURE(p);
        const ui::Panel* panel = ui::channelPanel(p);
        REQUIRE(panel != nullptr);
        const ui::ChannelLcdLayout l = ui::channelLcdLayout(panel->bounds);
        const ui::Rect* secs[5] = {&l.tag, &l.tone, &l.env, &l.note, &l.table};
        for (int i = 0; i < 5; ++i)
        {
            CHECK(secs[i]->x >= l.field.x);
            CHECK(secs[i]->x + secs[i]->w <= l.field.x + l.field.w);
            if (i > 0) CHECK(secs[i]->x > secs[i - 1]->x + secs[i - 1]->w);
        }
        // Sections are the generated ones (panel-relative), dividers sit between neighbouring sections.
        for (int i = 0; i < 5; ++i)
        {
            CHECK(secs[i]->x == panel->bounds.x + ui::kLcdSections[i].x);
            CHECK(secs[i]->w == ui::kLcdSections[i].w);
        }
        for (int i = 0; i < 4; ++i)
        {
            CHECK(l.dividers[i] == panel->bounds.x + ui::kLcdDividers[i]);
            CHECK(l.dividers[i] > secs[i]->x + secs[i]->w);
            CHECK(l.dividers[i] < secs[i + 1]->x);
        }
        // The MPC table label spans the note and table sections (same x as the Mac note section).
        for (int i = 0; i < ui::page(p).numDynamics; ++i)
        {
            const ui::Dynamic& d = ui::page(p).dynamics[i];
            if (d.kind == ui::DynamicKind::ValueLabel)
            {
                CHECK(d.bounds.x == l.note.x);
                CHECK(d.bounds.x + d.bounds.w == l.table.x + l.table.w);
            }
        }
        // The table window (+-3 rows at scale 1) fits the field height.
        CHECK(ui::kLcdTableRows * ui::kLcdTablePitch <= l.field.h);
        CHECK(l.table.w >= 200);
    }
    CHECK(ui::lcdWindowFirst(0) == 0);
    CHECK(ui::lcdWindowFirst(2) == 0);
    CHECK(ui::lcdWindowFirst(7) == 4);
    CHECK(ui::lcdWindowFirst(15) == 9);
    char n[4];
    ui::noteName(60, n);
    CHECK(std::string(n) == "C-4");
    ui::noteName(61, n);
    CHECK(std::string(n) == "C#4");
    ui::noteName(69, n);
    CHECK(std::string(n) == "A-4");
    ui::noteName(-1, n);
    CHECK(std::string(n) == "---");
    CHECK(ui::envelopeLevel(15, false, 3, 4) == 11);
    CHECK(ui::envelopeLevel(15, false, 3, 40) == 0);
    CHECK(ui::envelopeLevel(2, true, 1, 20) == 15);
    CHECK(ui::envelopeLevel(9, false, 0, 7) == 9);   // no envelope: hold
    std::uint8_t a[127], b2[127];
    ui::noiseMotif(true, a, 127);
    ui::noiseMotif(false, b2, 127);
    int ones = 0;
    for (std::uint8_t v : b2) ones += v;
    CHECK(ones > 20);
    CHECK(ones < 107);
    CHECK(std::memcmp(a, b2, sizeof(a)) != 0);
}

TEST_CASE("ui: caption fit - 4-way captions condense to fit their segment, others keep the normal layout")
{
    int condensed = 0;
    for (int p = 0; p < ui::kNumPages; ++p)
    {
        const ui::Page& pg = ui::page(p);
        for (int i = 0; i < pg.numControls; ++i)
            for (int k = 0; k < pg.controls[i].numCaptions; ++k)
            {
                const ui::Caption& cap = pg.controls[i].captions[k];
                CAPTURE(cap.text);
                std::string up = cap.text;
                for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                const ui::TextStyle st = ui::captionStyle(cap);
                CHECK(ui::textWidth(up.c_str(), st) <= cap.bounds.w - 2 * ui::kCaptionMargin);
                CHECK(st.scale == ui::labelScale(cap.fontHeight));   // condensed, never smaller
                if (st.proportional) ++condensed;
                // The normal layout whenever it fits.
                if (ui::textWidth(up.c_str(), ui::labelScale(cap.fontHeight)) <= cap.bounds.w - 2 * ui::kCaptionMargin)
                    CHECK_FALSE(st.proportional);
            }
    }
    CHECK(condensed > 0);
    // The ladder: normal, proportional with a font-pixel gap, then a one-unit gap, then a smaller scale.
    CHECK_FALSE(ui::fitText("RIGHT", 58, 2).proportional);
    CHECK(ui::fitText("RIGHT", 50, 2).proportional);
    CHECK(ui::textWidth("RIGHT", ui::fitText("RIGHT", 50, 2)) <= 50);
    CHECK(ui::textWidth("12.5%", ui::fitText("12.5%", 50, 2)) <= 50);
    CHECK(ui::fitText("12.5%", 50, 2).scale == 2);
    CHECK(ui::fitText("ABCDEFGHIJ", 40, 2).scale == 1);
    // Proportional width: glyph ink widths plus the gaps.
    const ui::TextStyle prop{2, 0, true, 1};
    const int iw = ui::glyphSpan(ui::glyph('I')).width;
    CHECK(ui::textWidth("II", prop) == 2 * iw * 2 + 1);
    // Rasterising a condensed caption stays inside its measured width.
    const ui::Rect box{0, 0, 200, 40};
    const ui::TextStyle st = ui::fitText("RIGHT", 50, 2);
    int maxX = 0;
    ui::rasterizeText("RIGHT", 0, 0, st, &box, [&](int x, int, int w, int) { maxX = std::max(maxX, x + w); });
    CHECK(maxX == ui::textWidth("RIGHT", st));
}

TEST_CASE("ui: MAIN header-band On toggles - the strip LEDs on MAIN only, grown hit rects")
{
    const ui::Page& main = ui::page(0);
    int toggles = 0;
    for (int i = 0; i < main.numDynamics; ++i)
    {
        const ui::Dynamic& d = main.dynamics[i];
        if (!ui::isHeaderToggle(0, d)) continue;
        ++toggles;
        const ui::Rect r = ui::headerToggleRect(d);
        CHECK(ui::hitHeaderToggle(0, r.x + r.w / 2, r.y + r.h / 2) == i);
        CHECK(ui::hitHeaderToggle(0, d.bounds.x - ui::kHeaderToggleSlop, d.bounds.y) == i);
        // Not a control's hit area: the strip's fader / segments sit lower.
        const std::vector<float> dv = defaults();
        CHECK(ui::hitTest(0, d.bounds.x + 2, d.bounds.y + 2, dv.data(), static_cast<int>(dv.size())).control == -1);
    }
    CHECK(toggles == 4);
    const int ons[4] = {P1_On, P2_On, WV_On, NZ_On};
    for (int k = 0, n = 0; k < main.numDynamics; ++k)
        if (ui::isHeaderToggle(0, main.dynamics[k])) CHECK(main.dynamics[k].param == ons[n++]);
    // Channel pages: the LED is inside the LCD, so it is no toggle there.
    for (int p = 1; p < ui::kNumPages; ++p)
        for (int i = 0; i < ui::page(p).numDynamics; ++i) CHECK_FALSE(ui::isHeaderToggle(p, ui::page(p).dynamics[i]));
    CHECK(ui::hitHeaderToggle(0, 640, 400) == -1);
}
