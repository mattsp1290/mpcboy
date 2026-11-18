// src/ui/ui_model.cpp - see ui_model.h.
#include "ui_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "table_bank.h"

namespace mpcboy::ui {

static_assert(kTableGrid.numColumns == 7, "the grid formats ROW VOL SEMI CMD VAL CMD VAL");
static_assert(kTableGrid.visibleRows <= 16, "TableView holds at most 16 rows");

namespace {

constexpr int kTableRows = 16;   // rows per LSDJ table

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

const PixelGlyph* findGlyph(char ch)
{
    for (int i = 0; i < kPixelFontCount; ++i)
        if (kPixelFont[i].ch == ch) return &kPixelFont[i];
    return nullptr;
}

void put(char* line, int at, const char* s)
{
    for (int i = 0; s[i] != '\0' && at + i < kTableLineChars; ++i) line[at + i] = s[i];
}

void blank(TableLine& l)
{
    std::memset(l.text, ' ', kTableLineChars);
    l.text[kTableLineChars] = '\0';
}

void trimRight(TableLine& l)
{
    int n = kTableLineChars;
    while (n > 0 && l.text[n - 1] == ' ') --n;
    l.text[n] = '\0';
}

// One table row in kTableColumns positions (the LSDJ-style line both grids show).
void formatRowLine(const rackboy::sav::RackboyTableRow& row, int r, bool cursor, TableLine& l)
{
    const TableColumn* cols = kTableGrid.columns;
    blank(l);
    l.row = r;
    l.cursor = cursor;
    char b[8];
    formatHex2(r, b);                       put(l.text, cols[0].x, b);
    formatVolume(row.has_vol, row.vol, b);  put(l.text, cols[1].x, b);
    formatTranspose(row.transpose, b);      put(l.text, cols[2].x, b);
    formatCommand(row.cmd1, b);             put(l.text, cols[3].x, b);
    formatHex2(row.cmd1_param, b);          put(l.text, cols[4].x, b);
    formatCommand(row.cmd2, b);             put(l.text, cols[5].x, b);
    formatHex2(row.cmd2_param, b);          put(l.text, cols[6].x, b);
    trimRight(l);
}

}  // namespace

// ---- layout access --------------------------------------------------------------------------
const Page& page(int index) { return kPages[clampi(index, 0, kNumPages - 1)]; }

const ComponentDef& componentOf(const Control& c) { return kComponents[clampi(c.component, 0, kNumComponents - 1)]; }

const ChildDef* findChild(const Control& c, ChildType type, int buttonId)
{
    const ComponentDef& d = componentOf(c);
    for (int i = 0; i < d.numChildren; ++i)
    {
        const ChildDef& ch = d.children[i];
        if (ch.type != type) continue;
        if (buttonId >= 0 && ch.buttonId != buttonId) continue;
        return &ch;
    }
    return nullptr;
}

Rect childRect(const Control& c, const ChildDef& ch)
{
    return {c.slot.x + ch.bounds.x, c.slot.y + ch.bounds.y, ch.bounds.w, ch.bounds.h};
}

bool contains(const Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

const Asset* findAsset(const char* file)
{
    if (file == nullptr) return nullptr;
    for (int i = 0; i < kNumAssets; ++i)
        if (std::strcmp(kAssets[i].file, file) == 0) return &kAssets[i];
    return nullptr;
}

Rect contentRect() { return {0, kContentY, kTabWidth, kTabHeight}; }

Rect tabButtonRect(int index)
{
    return {kTabBar.bounds.x + kTabBar.firstButtonX + index * kTabBar.pitch, kTabBar.bounds.y + kTabBar.buttonY,
            kTabButtonSize.w, kTabButtonSize.h};
}

// ---- values ---------------------------------------------------------------------------------
int stepOf(float norm, int steps)
{
    if (steps < 2) return 0;
    return clampi(static_cast<int>(std::lround(clampf(norm, 0.0f, 1.0f) * static_cast<float>(steps - 1))), 0, steps - 1);
}

float normOfStep(int step, int steps)
{
    if (steps < 2) return 0.0f;
    return static_cast<float>(clampi(step, 0, steps - 1)) / static_cast<float>(steps - 1);
}

float quantize(float norm, int steps)
{
    norm = clampf(norm, 0.0f, 1.0f);
    return steps < 2 ? norm : normOfStep(stepOf(norm, steps), steps);
}

int filmFrame(float norm, int frames) { return stepOf(norm, frames); }

bool isVisible(const Control& c, const float* norms, int numNorms)
{
    const VisibleWhen& v = c.visibleWhen;
    if (v.param < 0) return true;
    if (norms == nullptr || v.param >= numNorms) return true;
    return stepOf(norms[v.param], v.count) == v.choice;
}

// ---- hit-testing ----------------------------------------------------------------------------
namespace {

bool hitAnyLabel(const Control& c, int x, int y)
{
    const ComponentDef& d = componentOf(c);
    for (int i = 0; i < d.numChildren; ++i)
        if (d.children[i].type == ChildType::Label && contains(childRect(c, d.children[i]), x, y)) return true;
    return false;
}

Hit hitControl(const Control& c, int index, int x, int y)
{
    Hit h;
    switch (c.kind)
    {
        case WidgetKind::KnobUni:
        case WidgetKind::KnobBi:
        {
            const ChildDef* k = findChild(c, ChildType::Knob);
            if ((k && contains(childRect(c, *k), x, y)) || hitAnyLabel(c, x, y)) h.part = Part::Knob;
            break;
        }
        case WidgetKind::FaderUni:
        case WidgetKind::FaderBi:
        {
            const ChildDef* f = findChild(c, ChildType::Knob);
            if (f && contains(childRect(c, *f), x, y)) h.part = Part::Fader;
            break;
        }
        case WidgetKind::Toggle:
        {
            const ChildDef* b = findChild(c, ChildType::Button);
            if ((b && contains(childRect(c, *b), x, y)) || hitAnyLabel(c, x, y)) h.part = Part::Toggle;
            break;
        }
        case WidgetKind::Radio2:
        case WidgetKind::Radio3:
        case WidgetKind::Radio4:
        {
            const ComponentDef& d = componentOf(c);
            for (int i = 0; i < d.numChildren; ++i)
            {
                const ChildDef& ch = d.children[i];
                if (ch.type == ChildType::Button && contains(childRect(c, ch), x, y))
                {
                    h.part = Part::Segment;
                    h.segment = ch.buttonId;
                    break;
                }
            }
            break;
        }
        case WidgetKind::SliderWithFocus:
        {
            const ChildDef* s = findChild(c, ChildType::Slider);
            if (s && contains(childRect(c, *s), x, y)) h.part = Part::Slider;
            break;
        }
    }
    if (h.part != Part::None) h.control = index;
    return h;
}

}  // namespace

Hit hitTest(int pageIndex, int x, int y, const float* norms, int numNorms)
{
    const Page& p = page(pageIndex);
    for (int i = 0; i < p.numControls; ++i)
    {
        const Control& c = p.controls[i];
        if (!contains(c.slot, x, y) || !isVisible(c, norms, numNorms)) continue;
        return hitControl(c, i, x, y);
    }
    return {};
}

int hitTab(int x, int y)
{
    if (!contains(kTabBar.bounds, x, y)) return -1;
    for (int i = 0; i < kNumPages; ++i)
        if (contains(tabButtonRect(i), x, y)) return i;
    return -1;
}

// ---- gestures -------------------------------------------------------------------------------
bool Gesture::emit(float v, float& out)
{
    v = quantize(v, info_.steps);
    if (v == last_) return false;
    last_ = v;
    out = v;
    return true;
}

bool Gesture::press(const Control& c, const Hit& hit, int x, int y, bool fine, float start,
                    const ParamInfo& info, float& out)
{
    active_ = false;
    if (hit.part == Part::None) return false;
    part_ = hit.part;
    param_ = c.param;
    info_ = info;
    pressX_ = lastX_ = x;
    pressY_ = lastY_ = y;
    moved_ = false;
    startValue_ = clampf(start, 0.0f, 1.0f);
    acc_ = startValue_;
    last_ = quantize(startValue_, info_.steps);
    active_ = true;
    out = last_;

    switch (part_)
    {
        case Part::Toggle:
            // A two-state flip; a multi-step parameter on a toggle advances and wraps.
            if (info_.steps > 2)
                out = last_ = normOfStep((stepOf(startValue_, info_.steps) + 1) % info_.steps, info_.steps);
            else
                out = last_ = startValue_ >= 0.5f ? 0.0f : 1.0f;
            return true;
        case Part::Segment:
        {
            const ChildDef* b = findChild(c, ChildType::Button, hit.segment);
            const int n = b ? std::max(b->buttonsInGroup, 2) : std::max(info_.steps, 2);
            out = last_ = quantize(normOfStep(hit.segment, n), info_.steps);
            return true;
        }
        case Part::Fader:
        {
            const ChildDef* f = findChild(c, ChildType::Knob);
            area_ = f ? childRect(c, *f) : c.slot;
            if (!fine)
            {
                acc_ = clampf(static_cast<float>(x - (area_.x + kFaderTravelX0)) / static_cast<float>(kFaderTravelLen), 0.0f, 1.0f);
                emit(acc_, out);
                out = last_;
            }
            return true;
        }
        case Part::Knob:
        case Part::Slider:
        default:
            return true;   // nothing changes until the mouse moves (or, for a slider, a click)
    }
}

bool Gesture::drag(int x, int y, bool fine, float& out)
{
    if (!active_) return false;
    if (std::abs(x - pressX_) >= kClickSlop || std::abs(y - pressY_) >= kClickSlop) moved_ = true;
    const int dx = x - lastX_, dy = y - lastY_;
    lastX_ = x;
    lastY_ = y;
    const float speed = fine ? 1.0f / static_cast<float>(kFineDivisor) : 1.0f;
    switch (part_)
    {
        case Part::Knob:
            acc_ = clampf(acc_ - static_cast<float>(dy) * speed / static_cast<float>(kKnobDragRange), 0.0f, 1.0f);
            return emit(acc_, out);
        case Part::Fader:
            if (fine)
                acc_ = clampf(acc_ + static_cast<float>(dx) * speed / static_cast<float>(kFaderTravelLen), 0.0f, 1.0f);
            else
                acc_ = clampf(static_cast<float>(x - (area_.x + kFaderTravelX0)) / static_cast<float>(kFaderTravelLen), 0.0f, 1.0f);
            return emit(acc_, out);
        case Part::Slider:
        {
            if (!moved_) return false;
            if (info_.steps >= 2)
            {
                const int steps = (x - pressX_) / kSliderStepPx;
                return emit(normOfStep(stepOf(startValue_, info_.steps) + steps, info_.steps), out);
            }
            acc_ = clampf(acc_ + static_cast<float>(dx) * speed / static_cast<float>(kFaderTravelLen), 0.0f, 1.0f);
            return emit(acc_, out);
        }
        default:
            return false;   // toggles and segments act on press only
    }
}

bool Gesture::release(int x, int y, bool fine, float& out)
{
    if (!active_) return false;
    bool changed = drag(x, y, fine, out);
    if (part_ == Part::Slider && !moved_)
    {
        // A click steps to the next value (fine/shift: the previous one), wrapping like a menu.
        if (info_.steps >= 2)
        {
            const int n = info_.steps;
            const int s = (stepOf(startValue_, n) + (fine ? n - 1 : 1)) % n;
            changed = emit(normOfStep(s, n), out);
        }
        else
        {
            changed = emit(clampf(startValue_ + (fine ? -0.05f : 0.05f), 0.0f, 1.0f), out);
        }
    }
    active_ = false;
    return changed;
}

// ---- pixel font -----------------------------------------------------------------------------
const PixelGlyph& glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (const PixelGlyph* g = findGlyph(ch)) return *g;
    static const PixelGlyph* q = findGlyph('?');
    return *q;
}

int textWidth(const char* text, int scale)
{
    const int n = text ? static_cast<int>(std::strlen(text)) : 0;
    return n > 0 ? (n * kPixelFontAdvance - 1) * scale : 0;
}

GlyphSpan glyphSpan(const PixelGlyph& g)
{
    int lo = kPixelFontWidth, hi = -1;
    for (int r = 0; r < kPixelFontHeight; ++r)
        for (int c = 0; c < kPixelFontWidth; ++c)
            if (g.rows[r] & (1 << (kPixelFontWidth - 1 - c))) { lo = std::min(lo, c); hi = std::max(hi, c); }
    if (hi < 0) return {0, 3};
    return {lo, hi - lo + 1};
}

int textWidth(const char* text, const TextStyle& st)
{
    const int n = text ? static_cast<int>(std::strlen(text)) : 0;
    if (n == 0) return 0;
    if (!st.proportional) return (n - 1) * st.advance + kPixelFontWidth * st.scale;
    int w = 0;
    for (int i = 0; i < n; ++i) w += glyphSpan(glyph(text[i])).width * st.scale + (i + 1 < n ? st.gap : 0);
    return w;
}

TextStyle fitText(const char* text, int maxWidth, int scale)
{
    for (int s = scale < 1 ? 1 : scale; s >= 1; --s)
    {
        const TextStyle ladder[3] = {textStyle(s), {s, 0, true, s}, {s, 0, true, 1}};
        for (const TextStyle& st : ladder)
            if (textWidth(text, st) <= maxWidth) return st;
    }
    return textStyle(1);
}

TextStyle captionStyle(const Caption& c)
{
    return fitText(c.text, c.bounds.w - 2 * kCaptionMargin, labelScale(c.fontHeight));
}

Point placeText(const char* text, const Rect& b, int scale, Justify justify)
{
    const int tw = textWidth(text, scale);
    // Floor division, as the generator's previews (Python //) place text.
    auto half = [](int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); };
    int x = b.x;
    if (justify == Justify::Right) x = b.x + b.w - tw;
    else if (justify == Justify::Centre) x = b.x + half(b.w - tw);
    return {x, b.y + half(b.h - kPixelFontHeight * scale)};
}

Point placeText(const char* text, const Rect& b, const TextStyle& st, Justify justify)
{
    const int tw = textWidth(text, st);
    auto half = [](int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); };
    int x = b.x;
    if (justify == Justify::Right) x = b.x + b.w - tw;
    else if (justify == Justify::Centre) x = b.x + half(b.w - tw);
    return {x, b.y + half(b.h - kPixelFontHeight * st.scale)};
}

// ---- TABLE page text grid -------------------------------------------------------------------
void formatVolume(bool hasVol, int vol, char* buf)
{
    if (!hasVol) { std::strcpy(buf, "--"); return; }
    formatHex2(vol, buf);
}

void formatTranspose(int semis, char* buf)
{
    const int v = clampi(semis, -48, 48);
    std::snprintf(buf, 4, "%c%02d", v < 0 ? '-' : '+', v < 0 ? -v : v);
}

void formatCommand(std::uint8_t cmdByte, char* buf)
{
    buf[0] = isSupportedCommand(cmdByte) ? static_cast<char>(cmdByte) : '-';
    buf[1] = '\0';
}

void formatHex2(int value, char* buf) { std::snprintf(buf, 3, "%02X", clampi(value, 0, 255)); }

int tableFirstRow(int cursorRow)
{
    const int visible = kTableGrid.visibleRows;
    return clampi(clampi(cursorRow, 0, kTableRows - 1) - 3, 0, kTableRows - visible);
}

void buildTableView(const Content& content, int table, int cursorRow, TableView& out)
{
    const TableColumn* cols = kTableGrid.columns;
    blank(out.header);
    out.header.row = -1;
    out.header.cursor = false;
    for (int i = 0; i < kTableGrid.numColumns; ++i) put(out.header.text, cols[i].x, cols[i].title);
    trimRight(out.header);

    const rackboy::sav::RackboyTable& t = content.tables.tables[clampi(table, 0, kNumTables - 1)];
    const int cursor = clampi(cursorRow, 0, kTableRows - 1);
    const int first = tableFirstRow(cursor);
    out.numRows = kTableGrid.visibleRows;
    for (int i = 0; i < out.numRows; ++i)
    {
        const int r = first + i;
        formatRowLine(t.rows[r], r, r == cursor, out.rows[i]);
    }
}

int tableCharX(int charCol) { return kTableGrid.textX + charCol * kPixelFontAdvance * kTableGrid.charScale; }

int tableLineY(int line) { return kTableGrid.textY + line * kTableGrid.rowPitch; }

namespace {
int innerWidth() { return kTableInnerWidth; }
int bandHeight() { return kPixelFontHeight * kTableGrid.charScale + 2 * kTableBandPad; }
}  // namespace

Rect tableHeaderBand()
{
    return {kTableGrid.textX - kTableBandPad, kTableGrid.textY - kTableBandPad, innerWidth() + 2 * kTableBandPad, bandHeight()};
}

Rect tableCursorBand(int line)
{
    const int adv = kPixelFontAdvance * kTableGrid.charScale;
    return {kTableGrid.textX + adv - kTableBandPad, tableLineY(line) - kTableBandPad, innerWidth() - adv + 2 * kTableBandPad,
            bandHeight()};
}

// ---- TABLE page live grid ---------------------------------------------------------------
namespace {
constexpr int kGridAdvance = kPixelFontAdvance * kGridScale;
constexpr int kGridLastChar = 30;   // the text uses character columns 0 .. 29
int gridTextRight() { return gridCharX(kGridLastChar); }
}  // namespace

GridLayout gridLayout()
{
    GridLayout g;
    g.panel = kTableGrid.panelBounds;
    g.field = {g.panel.x + kGridFieldInset, g.panel.y + kGridFieldInset, g.panel.w - 2 * kGridFieldInset,
               g.panel.h - 2 * kGridFieldInset};
    g.headerY = g.field.y + (kGridHeaderHeight - kPixelFontHeight * kGridHeaderScale) / 2;
    g.rowsY = g.field.y + kGridHeaderHeight + 1;
    g.markerX = g.field.x + 4;
    // Character column 1 (the row number) starts right of a gutter that holds four stacked
    // markers; column 0 is the (unused) cursor column of kTableColumns.
    const int col1 = g.markerX + 3 * kGridMarkerStep + kPixelFontWidth * kGridScale + 3;
    g.textX = col1 - kGridAdvance;
    const int sideX = g.textX + kGridLastChar * kGridAdvance + 12;
    g.side = {sideX, g.field.y + 4, g.field.x + g.field.w - 6 - sideX, g.field.h - 8};
    return g;
}

int gridCharX(int charCol) { return gridLayout().textX + charCol * kGridAdvance; }

Rect gridRowBand(int row)
{
    const GridLayout g = gridLayout();
    const int r = clampi(row, 0, kGridRows - 1);
    return {g.field.x, g.rowsY + r * kGridPitch, gridTextRight() + 4 - g.field.x, kGridPitch};
}

int gridTextY(int row) { return gridRowBand(row).y + (kGridPitch - kPixelFontHeight * kGridScale) / 2; }

Rect gridCellRect(int row, int column)
{
    const TableColumn* cols = kTableGrid.columns;
    const Rect band = gridRowBand(row);
    const int c = clampi(column, 0, kTableGrid.numColumns - 1);
    const int left = c == 0 ? band.x : gridCharX(cols[c].x - 1);
    const int right = c + 1 < kTableGrid.numColumns ? gridCharX(cols[c + 1].x - 1) : band.x + band.w;
    return {left, band.y, right - left, band.h};
}

Rect gridCaretRect(int row, int column)
{
    const TableColumn& col = kTableGrid.columns[clampi(column, 0, kTableGrid.numColumns - 1)];
    const Rect band = gridRowBand(row);
    return {gridCharX(col.x) - 2, band.y, col.chars * kGridAdvance - kGridScale + 4, band.h};
}

GridHit hitGrid(int x, int y)
{
    GridHit h;
    const GridLayout g = gridLayout();
    const Rect all = {g.field.x, g.rowsY, gridRowBand(0).w, kGridRows * kGridPitch};
    if (!contains(all, x, y)) return h;
    h.row = (y - g.rowsY) / kGridPitch;
    for (int c = 0; c < kTableGrid.numColumns; ++c)
        if (contains(gridCellRect(h.row, c), x, y)) { h.column = c; break; }
    return h;
}

void buildGridRows(const Content& content, int table, int cursorRow, TableLine out[kGridRows])
{
    const rackboy::sav::RackboyTable& t = content.tables.tables[clampi(table, 0, kNumTables - 1)];
    for (int r = 0; r < kGridRows; ++r) formatRowLine(t.rows[r], r, r == cursorRow, out[r]);
}

// ---- channel LCD ----------------------------------------------------------------------------
namespace {
constexpr int kNoiseWarmup = 40;
const char* const kChannelTags[4] = {"PU1", "PU2", "WAV", "NOI"};
}

const char* channelTag(int channel) { return kChannelTags[clampi(channel, 0, 3)]; }

const Panel* channelPanel(int pageIndex)
{
    const Page& p = page(pageIndex);
    for (int i = 0; i < p.numPanels; ++i)
        if (p.panels[i].kind == PanelKind::LcdChannel) return &p.panels[i];
    return nullptr;
}

int pageChannel(int pageIndex)
{
    const Page& p = page(pageIndex);
    if (channelPanel(pageIndex) == nullptr || p.numControls == 0) return -1;
    for (int c = 0; c < 4; ++c)
        if (std::strncmp(p.controls[0].paramName, kChannelTags[c], 3) == 0) return c;
    return -1;
}

ChannelLcdLayout channelLcdLayout(const Rect& panel)
{
    ChannelLcdLayout l;
    l.field = {panel.x + kLcdInset, panel.y + kLcdInset, panel.w - 2 * kLcdInset, panel.h - 2 * kLcdInset};
    const Rect& f = l.field;
    // Section widths left to right; the table window takes what is left.
    const int widths[4] = {96, 196, 182, 96};
    Rect* sections[5] = {&l.tag, &l.tone, &l.env, &l.note, &l.table};
    int x = f.x + 6;
    for (int i = 0; i < 5; ++i)
    {
        const int w = i < 4 ? widths[i] : f.x + f.w - 6 - x;
        *sections[i] = {x, f.y, w, f.h};
        if (i < 4)
        {
            l.dividers[i] = x + w + 4;
            x += w + 10;
        }
    }
    return l;
}

int lcdWindowFirst(int row) { return clampi(row - kLcdTableRows / 2, 0, kGridRows - kLcdTableRows); }

void noteName(int midiNote, char* out)
{
    static const char* const kNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
    if (midiNote < 0) { std::strcpy(out, "---"); return; }
    const int n = clampi(midiNote, 0, 127);
    const int oct = n / 12 - 1;
    out[0] = kNames[n % 12][0];
    out[1] = kNames[n % 12][1];
    out[2] = oct < 0 ? '-' : static_cast<char>('0' + oct);
    out[3] = '\0';
}

int envelopeLevel(int level, bool up, int time, int step)
{
    const int l = clampi(level, 0, 15);
    if (time <= 0 || step <= 0) return l;
    return up ? clampi(l + step, 0, 15) : clampi(l - step, 0, 15);
}

void noiseMotif(bool width7, std::uint8_t* bits, int n)
{
    // The hardware LFSR (feedback = bit 0 xor bit 1 into bit 14, and bit 6 in 7-bit mode),
    // stepped past its start-up run of ones so the motif looks like the noise it makes.
    unsigned lfsr = 0x7FFF;
    for (int i = -kNoiseWarmup; i < n; ++i)
    {
        const unsigned fb = (lfsr ^ (lfsr >> 1)) & 1u;
        lfsr = (lfsr >> 1) | (fb << 14);
        if (width7) lfsr = (lfsr & ~(1u << 6)) | (fb << 6);
        if (i >= 0) bits[i] = static_cast<std::uint8_t>(~lfsr & 1u);
    }
}

// ---- breadcrumb -----------------------------------------------------------------------------
namespace {
void appendUpper(char* out, int size, int& at, const char* s)
{
    for (int i = 0; s && s[i] != '\0' && at < size - 1; ++i)
    {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out[at++] = c;
    }
    out[at] = '\0';
}
}  // namespace

void channelBreadcrumb(char* out, int size, int channel, const char* tableName, int row)
{
    if (size <= 0) return;
    int at = 0;
    out[0] = '\0';
    appendUpper(out, size, at, channelTag(channel));
    appendUpper(out, size, at, " > ");
    if (tableName == nullptr || tableName[0] == '\0')
    {
        appendUpper(out, size, at, "TABLE OFF");
        return;
    }
    appendUpper(out, size, at, tableName);
    if (row < 0) return;
    char b[12];
    std::snprintf(b, sizeof(b), " ROW %02X", clampi(row, 0, 15));
    appendUpper(out, size, at, b);
}

void tableBreadcrumb(char* out, int size, int table1, int row)
{
    if (size <= 0) return;
    char b[32];
    std::snprintf(b, sizeof(b), "TABLE %02d > ROW %02X", clampi(table1, 1, kNumTables), clampi(row, 0, 15));
    int at = 0;
    out[0] = '\0';
    appendUpper(out, size, at, b);
}

Rect breadcrumbRect(int pageIndex)
{
    const int x = kTitleX + textWidth(page(pageIndex).title, kTitleScale) + kBandRuleGap;
    const int h = kPixelFontHeight * kBandTextScale + 6;
    return {x, kBandTextY - 3, 24 + kBreadcrumbMaxChars * kPixelFontAdvance * kBandTextScale, h};
}

// ---- header toggles -------------------------------------------------------------------------
bool isHeaderToggle(int pageIndex, const Dynamic& d)
{
    if (d.kind != DynamicKind::FilmStrip || d.frames != 2 || !d.editor) return false;
    const Page& p = page(pageIndex);
    for (int i = 0; i < p.numPanels; ++i)
    {
        const Rect& b = p.panels[i].bounds;
        if (d.bounds.x >= b.x && d.bounds.y >= b.y && d.bounds.x + d.bounds.w <= b.x + b.w &&
            d.bounds.y + d.bounds.h <= b.y + b.h)
            return false;
    }
    return true;
}

Rect headerToggleRect(const Dynamic& d)
{
    return {d.bounds.x - kHeaderToggleSlop, d.bounds.y - kHeaderToggleSlop, d.bounds.w + 2 * kHeaderToggleSlop,
            d.bounds.h + 2 * kHeaderToggleSlop};
}

int hitHeaderToggle(int pageIndex, int x, int y)
{
    const Page& p = page(pageIndex);
    for (int i = 0; i < p.numDynamics; ++i)
        if (isHeaderToggle(pageIndex, p.dynamics[i]) && contains(headerToggleRect(p.dynamics[i]), x, y)) return i;
    return -1;
}

}  // namespace mpcboy::ui
