// src/ui/ui_model.h - the Mac editor's platform-free interaction model (UI WP2).
//
// Everything the editor needs that is not drawing or Cocoa: layout access over the generated
// tables in ui_layout.h, hit-testing per widget kind (from the component child bounds), mouse
// gestures -> normalised values, film-strip frames, visibleWhen, the tab bar, the TABLE page's
// LSDJ-style text grid and pixel-font text layout. The MPC firmware has its own interaction;
// this copies its documented feel (vertical drag for knobs, horizontal for faders, tap for
// segments and toggles) and is unit-tested in tests/unit/ui_model_test.cpp.
//
// Coordinates: "tab" coordinates are design units inside the 1280 x 628 page; "window"
// coordinates are the editor view (1280 x 704: the page at kContentY plus the tab bar). The Mac
// editor draws one point per design unit.
//
// No SDK or Cocoa headers; the only dependency is gbcore's Content type (table_bank.h).
#pragma once

#include <cstdint>

#include "ui_layout.h"

namespace mpcboy {
struct Content;
}

namespace mpcboy::ui {

// ---- parameters (filled by the caller from the plugin's parameter table) --------------------
struct ParamInfo
{
    int steps = 0;            // 0 = continuous; >= 2 = that many discrete values (int/choice/bool)
    float defaultNorm = 0.0f; // double-click reset value
};

// ---- layout access --------------------------------------------------------------------------
const Page& page(int index);                   // clamped to 0 .. kNumPages - 1
const ComponentDef& componentOf(const Control& c);
// First child of the given type (and, for buttons, buttonId >= 0 selects that button).
const ChildDef* findChild(const Control& c, ChildType type, int buttonId = -1);
Rect childRect(const Control& c, const ChildDef& child);   // tab coordinates
bool contains(const Rect& r, int x, int y);
const Asset* findAsset(const char* file);                    // nullptr when unknown
Rect contentRect();                                          // the page inside the window
Rect tabButtonRect(int index);                               // window coordinates

// ---- values ---------------------------------------------------------------------------------
int stepOf(float norm, int steps);         // 0 .. steps - 1 (0 when steps < 2)
float normOfStep(int step, int steps);     // clamped
float quantize(float norm, int steps);     // snap to a step (steps < 2: clamp only)
int filmFrame(float norm, int frames);     // film-strip frame for a value
// visibleWhen: true when always visible or the driving parameter sits on the given choice.
bool isVisible(const Control& c, const float* norms, int numNorms);

// ---- hit-testing ----------------------------------------------------------------------------
enum class Part : std::uint8_t
{
    None,
    Knob,     // knob kinds: the dial and its labels; vertical drag
    Fader,    // fader kinds: the fader strip; absolute horizontal
    Toggle,   // toggle: the button and its labels; click flips
    Segment,  // radioN: one segment button; click picks it
    Slider    // sliderWithFocus(+Menu): click steps, horizontal drag steps through the values
};

struct Hit
{
    int control = -1;   // index into page(p).controls, -1 = none
    Part part = Part::None;
    int segment = -1;   // Part::Segment: the segment (choice) index
};

// Tab coordinates. Hidden controls (visibleWhen) are never hit.
Hit hitTest(int pageIndex, int x, int y, const float* norms, int numNorms);
// Window coordinates: the tab button under the point, or -1.
int hitTab(int x, int y);

// Fader travel inside the fader child (design units, from ui_art.render_fader: art columns 4..119
// at 2 design units per art pixel, measured at pixel centres).
constexpr int kFaderTravelX0 = 9;
constexpr int kFaderTravelLen = 230;
constexpr int kKnobDragRange = 200;   // design units of vertical drag for the full range
constexpr int kFineDivisor = 10;      // fine mode (shift) divides the drag speed
constexpr int kSliderStepPx = 16;     // horizontal drag per step on a slider
constexpr int kClickSlop = 3;         // movement below this is a click

// ---- gestures -------------------------------------------------------------------------------
// One mouse interaction on one control: press -> drag* -> release (or cancel). Each call that
// returns true has written the value to apply to `out` (normalised, already quantised).
class Gesture
{
public:
    // Starts a gesture on `hit` (from hitTest on the same page). `start` is the parameter's
    // current normalised value. Returns false when the hit starts nothing.
    bool press(const Control& c, const Hit& hit, int x, int y, bool fine, float start,
               const ParamInfo& info, float& out);
    bool drag(int x, int y, bool fine, float& out);
    bool release(int x, int y, bool fine, float& out);
    void cancel() { active_ = false; }

    bool active() const { return active_; }
    int param() const { return param_; }
    Part part() const { return part_; }
    // True for toggles and segments: the edit is complete on press (begin, set and end together).
    bool discrete() const { return part_ == Part::Toggle || part_ == Part::Segment; }

private:
    bool emit(float v, float& out);

    bool active_ = false;
    Part part_ = Part::None;
    int param_ = -1;
    ParamInfo info_;
    Rect area_{0, 0, 0, 0};   // fader child (tab coordinates)
    int pressX_ = 0, pressY_ = 0, lastX_ = 0, lastY_ = 0;
    bool moved_ = false;
    float acc_ = 0.0f;        // unquantised value being dragged
    float startValue_ = 0.0f;
    float last_ = 0.0f;       // last emitted value
};

// ---- pixel font ---------------------------------------------------------------------------
// Glyph for a character: lower case maps to upper case, anything missing to '?'.
const PixelGlyph& glyph(char ch);
int textWidth(const char* text, int scale);
// Font cell scale for a label of the given (MPC) font height: the previews' rule.
inline int labelScale(float fontHeight) { return fontHeight < 24.0f ? 2 : 3; }
struct Point
{
    int x, y;
};
// Top-left of `text` inside `bounds` (justified horizontally, centred vertically).
Point placeText(const char* text, const Rect& bounds, int scale, Justify justify);

// Calls fill(x, y, w, h) once per horizontal run of lit font pixels, each font pixel being
// scale x scale. Runs are clipped to `clip` when it is non-null.
template <typename Fill>
void rasterizeText(const char* text, int x, int y, int scale, const Rect* clip, Fill&& fill)
{
    if (text == nullptr) return;
    for (int n = 0; text[n] != '\0'; ++n)
    {
        const PixelGlyph& g = glyph(text[n]);
        const int gx = x + n * kPixelFontAdvance * scale;
        for (int r = 0; r < kPixelFontHeight; ++r)
        {
            int c = 0;
            while (c < kPixelFontWidth)
            {
                if (!(g.rows[r] & (1 << (kPixelFontWidth - 1 - c)))) { ++c; continue; }
                int e = c;
                while (e < kPixelFontWidth && (g.rows[r] & (1 << (kPixelFontWidth - 1 - e)))) ++e;
                int rx = gx + c * scale, ry = y + r * scale, rw = (e - c) * scale, rh = scale;
                if (clip != nullptr)
                {
                    const int x0 = rx > clip->x ? rx : clip->x;
                    const int y0 = ry > clip->y ? ry : clip->y;
                    const int x1 = (rx + rw) < (clip->x + clip->w) ? (rx + rw) : (clip->x + clip->w);
                    const int y1 = (ry + rh) < (clip->y + clip->h) ? (ry + rh) : (clip->y + clip->h);
                    rx = x0; ry = y0; rw = x1 - x0; rh = y1 - y0;
                }
                if (rw > 0 && rh > 0) fill(rx, ry, rw, rh);
                c = e;
            }
        }
    }
}

// ---- TABLE page text grid (LSDJ style) ------------------------------------------------------
constexpr int kTableLineChars = 32;   // characters per text line (plus NUL)
struct TableLine
{
    char text[kTableLineChars + 1];   // columns at their character positions, spaces between
    int row = -1;                     // table row shown (-1: the header)
    bool cursor = false;              // the Edit Row row (drawn inverted)
};
struct TableView
{
    TableLine header;
    TableLine rows[16];
    int numRows = 0;                  // kTableGrid.visibleRows
};

// Cell text, the same rules as the plugin's formatValue for the editor parameters:
// volume "--" or 2-digit hex, transpose +/-NN (clamped to -48..48), command letter or '-',
// command value 2-digit hex. `buf` holds at least 4 characters.
void formatVolume(bool hasVol, int vol, char* buf);
void formatTranspose(int semis, char* buf);
void formatCommand(std::uint8_t cmdByte, char* buf);
void formatHex2(int value, char* buf);

// First row of the visible window: the cursor sits on the 4th line where the table allows.
int tableFirstRow(int cursorRow);
// The header plus kTableGrid.visibleRows rows around `cursorRow` of table `table` (0-based).
void buildTableView(const Content& content, int table, int cursorRow, TableView& out);
// Tab coordinates of a character cell: column `charCol`, line 0 = header, 1.. = rows.
int tableCharX(int charCol);
int tableLineY(int line);
// Highlight bands (tab coordinates), as the generated MPC art draws them.
Rect tableHeaderBand();
Rect tableCursorBand(int line);

}  // namespace mpcboy::ui
