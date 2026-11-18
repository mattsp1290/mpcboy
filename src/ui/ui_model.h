// src/ui/ui_model.h - the Mac editor's platform-free interaction model (UI WP2).
//
// Everything the editor needs that is not drawing or Cocoa: layout access over the generated
// tables in ui_layout.h, hit-testing per widget kind (from the component child bounds), mouse
// gestures -> normalised values, film-strip frames, visibleWhen, the tab bar, the TABLE page's
// LSDJ-style text grid and pixel-font text layout. The MPC firmware has its own interaction;
// this copies its documented feel (vertical drag for knobs, horizontal for faders, tap for
// segments and toggles) and is unit-tested in tests/unit/ui_model_test.cpp.
//
// Coordinates: "tab" coordinates are design units inside the kTabWidth x kTabHeight page; "window"
// coordinates are the editor view (kEditorWidth x kEditorHeight: the page at kContentY plus the
// tab bar). The Mac editor draws one point per design unit.
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

// Fader travel inside the fader child: kFaderTravelX0 / kFaderTravelLen (generated from
// ui_art.render_fader's 15 cells, see ui_layout.inc).
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
inline int labelScale(float fontHeight) { return fontHeight < kLabelScaleThreshold ? 2 : 3; }
struct Point
{
    int x, y;
};
// Top-left of `text` inside `bounds` (justified horizontally, centred vertically).
Point placeText(const char* text, const Rect& bounds, int scale, Justify justify);

// A pixel-font layout: font pixels of `scale` x `scale` design units. Monospaced (the normal
// layout): one character every `advance` design units. Proportional (condensed captions): each
// glyph takes only its lit columns, followed by `gap` design units (a space: 3 font pixels).
struct TextStyle
{
    int scale = 2;
    int advance = kPixelFontAdvance * 2;
    bool proportional = false;
    int gap = 0;
};
inline TextStyle textStyle(int scale) { return {scale, kPixelFontAdvance * scale, false, 0}; }
// Lit column extent of a glyph: first lit column and the number of columns (a blank glyph:
// 0 and 3).
struct GlyphSpan
{
    int first, width;
};
GlyphSpan glyphSpan(const PixelGlyph& g);
int textWidth(const char* text, const TextStyle& style);
Point placeText(const char* text, const Rect& bounds, const TextStyle& style, Justify justify);
// The widest layout of `text` at `scale` that fits `maxWidth`, in whole design units: the
// normal layout; else proportional with a one-font-pixel gap; else proportional with a
// one-unit gap; else the same ladder at the next smaller scale (scale 1 is returned even when
// it does not fit).
TextStyle fitText(const char* text, int maxWidth, int scale);
// Segment captions keep this many design units clear on each side of their box (the segment
// art's rounded edge).
constexpr int kCaptionMargin = 4;
// The layout of a segment caption: its label scale, condensed when the text would not fit.
TextStyle captionStyle(const Caption& c);

// Calls fill(x, y, w, h) once per horizontal run of lit font pixels, each font pixel being
// scale x scale. Runs are clipped to `clip` when it is non-null.
template <typename Fill>
void rasterizeText(const char* text, int x, int y, const TextStyle& style, const Rect* clip, Fill&& fill);
template <typename Fill>
void rasterizeText(const char* text, int x, int y, int scale, const Rect* clip, Fill&& fill)
{
    rasterizeText(text, x, y, textStyle(scale), clip, static_cast<Fill&&>(fill));
}
template <typename Fill>
void rasterizeText(const char* text, int x, int y, const TextStyle& style, const Rect* clip, Fill&& fill)
{
    if (text == nullptr) return;
    const int scale = style.scale;
    int pen = x;
    for (int n = 0; text[n] != '\0'; ++n)
    {
        const PixelGlyph& g = glyph(text[n]);
        int gx = pen;
        if (style.proportional)
        {
            const GlyphSpan sp = glyphSpan(g);
            gx = pen - sp.first * scale;
            pen += sp.width * scale + style.gap;
        }
        else
        {
            pen += style.advance;
        }
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

// ---- TABLE page text lines (LSDJ style) -----------------------------------------------------
// kTableLineChars (generated): characters per text line (plus NUL)
struct TableLine
{
    char text[kTableLineChars + 1];   // columns at their character positions, spaces between
    int row = -1;                     // table row shown
    bool cursor = false;              // the Edit Row row (drawn inverted)
};

// Cell text, the same rules as the plugin's formatValue for the editor parameters:
// volume "--" or 2-digit hex, transpose +/-NN (clamped to -48..48), command letter or '-',
// command value 2-digit hex. `buf` holds at least 4 characters.
void formatVolume(bool hasVol, int vol, char* buf);
void formatTranspose(int semis, char* buf);
void formatCommand(std::uint8_t cmdByte, char* buf);
void formatHex2(int value, char* buf);

// ---- TABLE page live grid (Mac editor; redesign step 3) ---------------------------------
// All kGridRows (16) rows of the edited table inside the LCD panel (kTableGrid.panelBounds, cols 0-1 x rows
// 0-1): a scale-1 column header, then one band per row at kGridScale with hex row numbers, beat
// rows every 4th row, the inverted cursor row, playhead rows and a channel-marker gutter left of
// the row numbers. Text columns follow kTableColumns (character positions); a side column right
// of the text holds the table's name and "used by" chips. Tab coordinates.
// kGridRows, kGridScale, kGridPitch, kGridHeaderScale, kGridHeaderHeight, kGridFieldInset,
// kGridMarkerStep and kGridLastChar are generated (ui_layout.inc, from "grid.live" in
// ui/mpcboy_ui.json); ui_model.cpp static_asserts that the grid fits the panel.
struct GridLayout
{
    Rect panel;    // kTableGrid.panelBounds
    Rect field;    // the LCD field (panel minus kGridFieldInset)
    int textX;     // x of character column 0 (kTableColumns count from here)
    int headerY;   // top of the header text
    int rowsY;     // top of row 0's band
    int markerX;   // first channel marker
    Rect side;     // the info column right of the text
};
GridLayout gridLayout();
int gridCharX(int charCol);
Rect gridRowBand(int row);           // the full band of a row (beat / cursor / playhead fill)
int gridTextY(int row);              // top of a row's glyphs
inline bool isBeatRow(int row) { return row % 4 == 0; }
// Column c (0 = ROW, 1..6 = the six cell columns, as kTableColumns): its clickable extent on
// a row, from one character left of its text to one character left of the next column's.
Rect gridCellRect(int row, int column);
// The cell text and caret extent of a column (exactly its characters).
Rect gridCaretRect(int row, int column);
struct GridHit
{
    int row = -1;      // 0..15, -1 = not on a row
    int column = -1;   // 0..6 as gridCellRect
};
GridHit hitGrid(int x, int y);
// Grid column 1..6 -> editor cell column 0..5 (Row Vol .. Row Val2); -1 for the ROW column.
inline int gridEditorColumn(int column) { return column >= 1 && column <= 6 ? column - 1 : -1; }
// The 16 row lines of table `table` (0-based) in kTableColumns positions; `cursorRow` flagged.
void buildGridRows(const Content& content, int table, int cursorRow, TableLine out[kGridRows]);

// ---- channel LCD (Mac editor; pages with an LcdChannel panel) ---------------------------
// Channel index (0 PU1, 1 PU2, 2 WAV, 3 NOI) of a page's channel LCD, or -1 when the page has
// none. Taken from the tag of the page's first parameter name ("PU1 On" -> 0).
int pageChannel(int pageIndex);
const Panel* channelPanel(int pageIndex);   // the LcdChannel panel, or nullptr
const char* channelTag(int channel);        // "PU1", "PU2", "WAV", "NOI"
// Sections of the channel LCD (tab coordinates), left to right: tag + LED, tone (duty / wave
// shape / noise motif), envelope, note + level, table window. `dividers` are the dotted rules.
// The section x positions, widths and the dividers are generated (kLcdSections, kLcdDividers):
// the MPC's film strips and the printed LCD art use the same values.
constexpr int kLcdTableRows = 7;            // the table window: the playhead row +-3
constexpr int kLcdTablePitch = 12;          // scale-1 rows
struct ChannelLcdLayout
{
    Rect field, tag, tone, env, note, table;
    int dividers[4];
};
ChannelLcdLayout channelLcdLayout(const Rect& panel);
// First row of the table window around `row`, clamped so the window stays inside 0..15.
int lcdWindowFirst(int row);
// "C-3", "C#3" (MIDI 60 = C3, matching Ableton Live); "---" for a negative note. Octaves below 0
// (MIDI 0..23, below the Game Boy's playable range) show '-' in place of the digit, as in "C--".
// `out` holds at least 4 characters.
void noteName(int midiNote, char* out);
// Envelope level after `step` envelope steps from `level` (0..15) moving up or down; time 0
// keeps the level (no envelope).
int envelopeLevel(int level, bool up, int time, int step);
// The first `n` output bits of the noise LFSR (15-bit, or 7-bit when `width7`), from the
// power-on state: 1 = high.
void noiseMotif(bool width7, std::uint8_t* bits, int n);

// ---- top band breadcrumb (Mac editor) -----------------------------------------------------
// Channel pages: "PU1 > MAJARP ROW 02" (`row` < 0: the table is not running: "PU1 > MAJARP");
// `tableName` nullptr or "": "PU1 > TABLE OFF". TABLE: "TABLE 01 > ROW 02" (`table1` 1..16).
// Upper case; truncated to `size` - 1 characters.
void channelBreadcrumb(char* out, int size, int channel, const char* tableName, int row);
void tableBreadcrumb(char* out, int size, int table1, int row);
constexpr int kBreadcrumbMaxChars = 24;
// The top-band area the breadcrumb owns on a page (after the title, on the rule): it is redrawn
// with the band background and the text at kBandTextY / kBandTextScale.
Rect breadcrumbRect(int pageIndex);

// ---- MAIN header-band On toggles (Mac editor) ----------------------------------------------
// The page's two-frame film-strip LEDs outside every panel (MAIN's strip headers) act as On
// toggles on the Mac; the MPC shows them only. Hit rect: the LED grown by kHeaderToggleSlop.
constexpr int kHeaderToggleSlop = 6;
bool isHeaderToggle(int pageIndex, const Dynamic& d);
Rect headerToggleRect(const Dynamic& d);
// Index into page(pageIndex).dynamics of the toggle under the point, or -1.
int hitHeaderToggle(int pageIndex, int x, int y);

}  // namespace mpcboy::ui
