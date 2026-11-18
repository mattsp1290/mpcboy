// src/plugin/mac_editor.mm - see mac_editor.h. Objective-C++, ARC (-fobjc-arc).
//
// Pieces:
//   Art        decodes each ui/art PNG once (ImageIO) and crops film-strip frames on demand.
//   Snapshot   what the editor shows: page, every parameter's normalised value and display
//              text, the live channel LCD, the TABLE page grid (ContentStore::readPublished and
//              MpcBoyPlugin::playback()) and the top-band breadcrumb. Rebuilt at 30 Hz; only
//              controls whose value, text or visibility changed, and the LCD / grid / breadcrumb
//              rects whose content changed, are invalidated.
//   drawEditor one drawing routine for the view and the offscreen renderer: flipped
//              coordinates, one unit per design unit, no interpolation, rects snapped to device
//              pixels, all text in the shared 5 x 7 pixel font at integer scales.
//   EditorView the NSView: mouse -> ui::Gesture -> MpcBoyPlugin editor edit API. Its 30 Hz
//              NSTimer (NSRunLoopCommonModes) targets a proxy holding a weak view reference,
//              so neither the view nor the plugin is retained by the run loop.
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "mac_editor.h"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "MpcBoyPlugin.h"
#include "editor_actions.h"
#include "editor_sync.h"
#include "mpc_resources.h"
#include "param_bridge.h"
#include "table_bank.h"
#include "ui_model.h"

#if !__has_feature(objc_arc)
#error "mac_editor.mm must be compiled with -fobjc-arc"
#endif

// Unique Objective-C class names per build (the runtime has one flat class namespace: two
// plugins, or two versions of this one, in one host must not collide).
#ifndef MPCBOY_OBJC_PREFIX
#define MPCBOY_OBJC_PREFIX MPCBOY_dev_
#endif
#define MB_CAT2(a, b) a##b
#define MB_CAT(a, b) MB_CAT2(a, b)
#define MBEditorView MB_CAT(MPCBOY_OBJC_PREFIX, EditorView)
#define MBTimerProxy MB_CAT(MPCBOY_OBJC_PREFIX, TimerProxy)

namespace ui = mpcboy::ui;

namespace {

constexpr int kW = ui::kEditorWidth;
constexpr int kH = ui::kEditorHeight;

// ---- art ------------------------------------------------------------------------------------
struct Strip
{
    CGImageRef image = nullptr;          // the whole PNG
    int w = 0, h = 0, frames = 1;        // one frame
    std::vector<CGImageRef> frameCache;  // cropped on demand
};

class Art
{
public:
    ~Art()
    {
        for (auto& kv : strips_)
        {
            for (CGImageRef f : kv.second.frameCache)
                if (f) CGImageRelease(f);
            if (kv.second.image) CGImageRelease(kv.second.image);
        }
    }

    // Loads every asset the layout names. Missing or mis-sized files are left out: the drawing
    // code falls back to flat colours for them.
    void load(const std::string& dir)
    {
        if (loaded_) return;
        loaded_ = true;
        for (int i = 0; i < ui::kNumAssets; ++i)
        {
            const ui::Asset& a = ui::kAssets[i];
            const std::string path = dir + "/" + a.file;
            CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                                   static_cast<CFIndex>(path.size()), false);
            if (!url) continue;
            CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
            CFRelease(url);
            if (!src) continue;
            NSDictionary* opts = @{(__bridge NSString*)kCGImageSourceShouldCacheImmediately: @YES};
            CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, (__bridge CFDictionaryRef)opts);
            CFRelease(src);
            if (!img) continue;
            if (static_cast<int>(CGImageGetWidth(img)) != a.width ||
                static_cast<int>(CGImageGetHeight(img)) != a.height * a.frames)
            {
                CGImageRelease(img);   // not what the layout expects: use the fallback
                continue;
            }
            Strip s;
            s.image = img;
            s.w = a.width;
            s.h = a.height;
            s.frames = a.frames;
            s.frameCache.assign(static_cast<size_t>(a.frames), nullptr);
            strips_[a.file] = std::move(s);
        }
    }

    size_t count() const { return strips_.size(); }

    // The frame image, or nullptr when the file is missing.
    CGImageRef frame(const char* file, int index)
    {
        if (file == nullptr) return nullptr;
        auto it = strips_.find(file);
        if (it == strips_.end()) return nullptr;
        Strip& s = it->second;
        if (s.frames <= 1) return s.image;
        index = index < 0 ? 0 : (index >= s.frames ? s.frames - 1 : index);
        CGImageRef& f = s.frameCache[static_cast<size_t>(index)];
        if (!f) f = CGImageCreateWithImageInRect(s.image, CGRectMake(0, static_cast<CGFloat>(index * s.h), s.w, s.h));
        return f;
    }

private:
    bool loaded_ = false;
    std::map<std::string, Strip> strips_;
};

std::string upper(const std::string& s)
{
    std::string u = s;
    for (char& c : u)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return u;
}

// ---- snapshot -------------------------------------------------------------------------------
// Plain-old-data views, zeroed before they are filled and compared with memcmp: a refresh
// invalidates a rect only when what it draws changed.
constexpr int kTblParam[4] = {mpcboy::P1_Tbl, mpcboy::P2_Tbl, mpcboy::WV_Tbl, mpcboy::NZ_Tbl};
constexpr int kOnParam[4] = {mpcboy::P1_On, mpcboy::P2_On, mpcboy::WV_On, mpcboy::NZ_On};

// The live channel LCD of a channel page (row 0, cols 0-2).
struct ChannelLcdData
{
    int channel;            // 0..3, -1: the page has none
    int on;
    int tone;               // PU: duty 0..3; NOI: mode (0 = 15-bit, 1 = 7-bit)
    int level, envUp, envTime;   // the envelope parameters (WAV: level = Wave Level step 0..3)
    std::uint8_t wave[mpcboy::kWaveBytes];   // WAV: the bytes drawn (live wave RAM while active)
    char toneText[24];
    char envText[24];
    char note[4];
    int liveLevel;
    int table;              // the channel's Table choice: 0 = off, else slot + 1
    int running, playRow, first;
    char tableName[24];
    char rows[ui::kLcdTableRows][24];
};

// The TABLE page grid.
struct GridData
{
    int table;              // 0-based
    int cursor;             // Edit Row
    int caret;              // grid column 1..6 (the Row control a grid drag edits)
    ui::TableLine rows[ui::kGridRows];
    int playRow[4];         // the row each channel plays in this table, -1 = not playing it
    int usedBy[4];          // channel's Table is this table
    char name[24];
};

struct Snapshot
{
    int page = 0;
    int numParams = 0;
    float norm[mpcboy::kTotalParams] = {};
    std::string value[mpcboy::kTotalParams];
    std::string name[mpcboy::kTotalParams];
    bool hasTable = false;
    GridData grid;
    ChannelLcdData lcd;
    char crumb[32];
};

void upperInto(char* out, size_t n, const std::string& s)
{
    size_t i = 0;
    for (; i + 1 < n && i < s.size(); ++i)
    {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out[i] = c;
    }
    out[i] = '\0';
}

// The compact LCD row: "02 0F +03 H01".
void lcdRowText(const rackboy::sav::RackboyTableRow& row, int r, char* out, size_t n)
{
    char vol[4], tsp[5], cmd[2], val[4];
    ui::formatVolume(row.has_vol, row.vol, vol);
    ui::formatTranspose(row.transpose, tsp);
    ui::formatCommand(row.cmd1, cmd);
    ui::formatHex2(row.cmd1_param, val);
    std::snprintf(out, n, "%02X %s %s %s%s", r & 15, vol, tsp, cmd, val);
}

void buildLcd(MpcBoyPlugin& p, const Snapshot& s, const mpcboy::Content& content, int ch, ChannelLcdData& d)
{
    using namespace mpcboy;
    std::memset(&d, 0, sizeof(d));
    d.channel = ch;
    if (ch < 0) return;
    const ChannelPlayback pb = p.playback().load(ch);
    d.on = p.getInt(kOnParam[ch]);
    if (ch == 2)
    {
        d.level = p.getInt(WV_Level);
        const int shape = std::clamp(p.getInt(WV_Wave), 0, kNumWaves - 1);
        if (pb.active) p.playback().loadWave(d.wave);
        else std::memcpy(d.wave, content.waves.waves[shape], kWaveBytes);
        std::snprintf(d.toneText, sizeof(d.toneText), "WAVE %s", upper(s.value[WV_Wave]).c_str());
        std::snprintf(d.envText, sizeof(d.envText), "LEVEL %s", upper(s.value[WV_Level]).c_str());
    }
    else
    {
        const int vol = ch == 0 ? P1_Vol : ch == 1 ? P2_Vol : NZ_Vol;
        const int dir = ch == 0 ? P1_EnvDir : ch == 1 ? P2_EnvDir : NZ_EnvDir;
        const int spd = ch == 0 ? P1_EnvSpd : ch == 1 ? P2_EnvSpd : NZ_EnvSpd;
        d.level = p.getInt(vol);
        d.envUp = p.getInt(dir);
        d.envTime = p.getInt(spd);
        if (ch == 3)
        {
            d.tone = p.getInt(NZ_Width);
            std::snprintf(d.toneText, sizeof(d.toneText), "NOISE %s", upper(s.value[NZ_Width]).c_str());
        }
        else
        {
            const int duty = ch == 0 ? P1_Duty : P2_Duty;
            d.tone = p.getInt(duty);
            std::snprintf(d.toneText, sizeof(d.toneText), "DUTY %s", upper(s.value[duty]).c_str());
        }
        if (d.envTime > 0) std::snprintf(d.envText, sizeof(d.envText), "ENV %02d %s %d", d.level, d.envUp ? "UP" : "DOWN", d.envTime);
        else std::snprintf(d.envText, sizeof(d.envText), "ENV %02d HOLD", d.level);
    }
    ui::noteName(pb.note, d.note);
    d.liveLevel = pb.level;
    d.table = p.getInt(kTblParam[ch]);
    if (d.table <= 0) return;
    const int slot = std::clamp(d.table - 1, 0, kNumTables - 1);
    upperInto(d.tableName, sizeof(d.tableName), s.value[kTblParam[ch]]);
    d.running = pb.running && pb.table == slot;
    d.playRow = d.running ? pb.row : -1;
    d.first = ui::lcdWindowFirst(d.running ? pb.row : 0);
    const rackboy::sav::RackboyTable& t = content.tables.tables[slot];
    for (int i = 0; i < ui::kLcdTableRows; ++i) lcdRowText(t.rows[d.first + i], d.first + i, d.rows[i], sizeof(d.rows[i]));
}

void buildGrid(MpcBoyPlugin& p, const Snapshot& s, const mpcboy::Content& content, int caret, GridData& g)
{
    using namespace mpcboy;
    std::memset(&g, 0, sizeof(g));
    g.table = std::clamp(p.getInt(ED_Table) - 1, 0, kNumTables - 1);
    g.cursor = std::clamp(p.getInt(ED_Row), 0, 15);
    g.caret = caret;
    ui::buildGridRows(content, g.table, g.cursor, g.rows);
    for (int ch = 0; ch < 4; ++ch)
    {
        const ChannelPlayback pb = p.playback().load(ch);
        g.playRow[ch] = pb.running && pb.table == g.table ? pb.row : -1;
        g.usedBy[ch] = p.getInt(kTblParam[ch]) == g.table + 1;
    }
    // The table's name: its label in the Table choice lists.
    const mpc::Param& tp = p.param(P1_Tbl);
    if (static_cast<size_t>(g.table + 1) < tp.choices.size()) upperInto(g.name, sizeof(g.name), tp.choices[g.table + 1]);
    (void)s;
}

void takeSnapshot(MpcBoyPlugin& p, int page, int caret, mpcboy::Content& scratch, Snapshot& s)
{
    s.page = page;
    s.numParams = std::min(p.numParams(), static_cast<int>(mpcboy::kTotalParams));
    for (int i = 0; i < s.numParams; ++i)
    {
        s.norm[i] = p.getNormalised(i);
        s.value[i] = p.displayText(i);
        if (s.name[i].empty()) s.name[i] = p.param(i).name;
    }
    s.hasTable = page == ui::kTableGrid.page;
    const int ch = ui::pageChannel(page);
    std::memset(s.crumb, 0, sizeof(s.crumb));
    if (s.hasTable || ch >= 0) p.content().readPublished(scratch);
    if (s.hasTable)
    {
        buildGrid(p, s, scratch, caret, s.grid);
        ui::tableBreadcrumb(s.crumb, sizeof(s.crumb), s.grid.table + 1, s.grid.cursor);
    }
    else
    {
        std::memset(&s.grid, 0, sizeof(s.grid));
    }
    buildLcd(p, s, scratch, ch, s.lcd);
    if (ch >= 0) ui::channelBreadcrumb(s.crumb, sizeof(s.crumb), ch, s.lcd.table > 0 ? s.lcd.tableName : nullptr, s.lcd.playRow);
}

ui::ParamInfo paramInfo(const mpc::Param& p)
{
    ui::ParamInfo i;
    switch (p.kind)
    {
        case mpc::Param::Kind::Bool: i.steps = 2; break;
        case mpc::Param::Kind::Choice: i.steps = static_cast<int>(p.choices.size()); break;
        case mpc::Param::Kind::Int: i.steps = static_cast<int>(std::lround(p.max - p.min)) + 1; break;
        default: i.steps = 0; break;
    }
    i.defaultNorm = p.toNormalised(p.def);
    return i;
}

// ---- drawing --------------------------------------------------------------------------------
void setFill(CGContextRef ctx, std::uint32_t argb)
{
    CGContextSetRGBFillColor(ctx, ((argb >> 16) & 255) / 255.0, ((argb >> 8) & 255) / 255.0, (argb & 255) / 255.0,
                             ((argb >> 24) & 255) / 255.0);
}

// Snap a user-space rect to whole device pixels.
CGRect snap(CGContextRef ctx, CGRect r)
{
    CGRect d = CGContextConvertRectToDeviceSpace(ctx, r);
    const CGFloat x0 = std::round(CGRectGetMinX(d)), y0 = std::round(CGRectGetMinY(d));
    const CGFloat x1 = std::round(CGRectGetMaxX(d)), y1 = std::round(CGRectGetMaxY(d));
    return CGContextConvertRectToUserSpace(ctx, CGRectMake(x0, y0, x1 - x0, y1 - y0));
}

void fillRect(CGContextRef ctx, int x, int y, int w, int h, std::uint32_t argb)
{
    if (w <= 0 || h <= 0) return;
    setFill(ctx, argb);
    CGContextFillRect(ctx, snap(ctx, CGRectMake(x, y, w, h)));
}

void frameRect(CGContextRef ctx, int x, int y, int w, int h, std::uint32_t argb)
{
    fillRect(ctx, x, y, w, 1, argb);
    fillRect(ctx, x, y + h - 1, w, 1, argb);
    fillRect(ctx, x, y, 1, h, argb);
    fillRect(ctx, x + w - 1, y, 1, h, argb);
}

// Draws an image with its top-left at (x, y) in the flipped context, at one unit per pixel.
void drawImage(CGContextRef ctx, CGImageRef img, int x, int y)
{
    if (!img) return;
    const CGFloat w = static_cast<CGFloat>(CGImageGetWidth(img)), h = static_cast<CGFloat>(CGImageGetHeight(img));
    const CGRect r = snap(ctx, CGRectMake(x, y, w, h));
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, r.origin.x, r.origin.y + r.size.height);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextDrawImage(ctx, CGRectMake(0, 0, r.size.width, r.size.height), img);
    CGContextRestoreGState(ctx);
}

void fillRun(CGContextRef ctx, int x, int y, int w, int h) { CGContextFillRect(ctx, snap(ctx, CGRectMake(x, y, w, h))); }

void drawText(CGContextRef ctx, const std::string& text, const ui::Rect& bounds, const ui::TextStyle& st, ui::Justify j,
              std::uint32_t argb, bool clip = true)
{
    const ui::Point p = ui::placeText(text.c_str(), bounds, st, j);
    setFill(ctx, argb);
    ui::rasterizeText(text.c_str(), p.x, p.y, st, clip ? &bounds : nullptr,
                      [&](int x, int y, int w, int h) { fillRun(ctx, x, y, w, h); });
}

void drawText(CGContextRef ctx, const std::string& text, const ui::Rect& bounds, int scale, ui::Justify j,
              std::uint32_t argb, bool clip = true)
{
    drawText(ctx, text, bounds, ui::textStyle(scale), j, argb, clip);
}

void drawTextAt(CGContextRef ctx, const char* text, int x, int y, int scale, std::uint32_t argb)
{
    setFill(ctx, argb);
    ui::rasterizeText(text, x, y, scale, nullptr, [&](int rx, int ry, int w, int h) { fillRun(ctx, rx, ry, w, h); });
}

// Channel colour: the accent of the page whose LCD shows that channel.
std::uint32_t channelColour(int ch)
{
    for (int i = 0; i < ui::kNumPages; ++i)
        if (ui::pageChannel(i) == ch) return ui::page(i).accent;
    return ui::kTheme.lcdInk;
}

// A dotted vertical rule (LCD section dividers).
void dottedRule(CGContextRef ctx, int x, int y0, int y1, std::uint32_t argb)
{
    for (int y = y0; y < y1; y += 4) fillRect(ctx, x, y, 2, 2, argb);
}

// ---- TABLE page grid ------------------------------------------------------------------------
void drawTableGrid(CGContextRef ctx, const GridData& g, int oy)
{
    const ui::Theme& t = ui::kTheme;
    const ui::GridLayout L = ui::gridLayout();
    const ui::TableColumn* cols = ui::kTableGrid.columns;
    const int ncols = ui::kTableGrid.numColumns;
    const ui::Rect f = L.field;
    fillRect(ctx, f.x, f.y + oy, f.w, f.h, t.lcdField);

    // Header: scale-1 column titles on a beat band; the caret column's title inverted.
    fillRect(ctx, f.x, f.y + oy, ui::gridRowBand(0).w, ui::kGridHeaderHeight, t.lcdBeat);
    for (int c = 0; c < ncols; ++c)
    {
        const int x = ui::gridCharX(cols[c].x);
        std::uint32_t ink = t.lcdInk;
        if (c == g.caret)
        {
            const int w = ui::textWidth(cols[c].title, ui::kGridHeaderScale);
            fillRect(ctx, x - 2, f.y + oy, w + 4, ui::kGridHeaderHeight, t.lcdCursor);
            ink = t.lcdCursorInk;
        }
        drawTextAt(ctx, cols[c].title, x, L.headerY + oy, ui::kGridHeaderScale, ink);
    }

    for (int r = 0; r < ui::kGridRows; ++r)
    {
        const ui::TableLine& l = g.rows[r];
        const ui::Rect band = ui::gridRowBand(r);
        bool playing = false;
        for (int ch = 0; ch < 4; ++ch) playing = playing || g.playRow[ch] == r;
        std::uint32_t ink = t.lcdInk;
        if (l.cursor) { fillRect(ctx, band.x, band.y + oy, band.w, band.h, t.lcdCursor); ink = t.lcdCursorInk; }
        else if (playing) { fillRect(ctx, band.x, band.y + oy, band.w, band.h, t.lcdPlayhead); ink = t.lcdPlayheadInk; }
        else if (ui::isBeatRow(r)) fillRect(ctx, band.x, band.y + oy, band.w, band.h, t.lcdBeat);
        const int ty = ui::gridTextY(r) + oy;
        drawTextAt(ctx, l.text, ui::gridCharX(0), ty, ui::kGridScale, ink);
        if (l.cursor && g.caret >= 1 && g.caret < ncols)
        {
            // Column caret: the selected cell of the cursor row, back in field colours.
            const ui::Rect cr = ui::gridCaretRect(r, g.caret);
            fillRect(ctx, cr.x, cr.y + oy, cr.w, cr.h, t.lcdField);
            char cell[8] = {};
            const int len = static_cast<int>(std::strlen(l.text));
            for (int k = 0; k < cols[g.caret].chars && k < 7; ++k)
            {
                const int at = cols[g.caret].x + k;
                cell[k] = at < len ? l.text[at] : ' ';
            }
            drawTextAt(ctx, cell, ui::gridCharX(cols[g.caret].x), ty, ui::kGridScale, t.lcdCursor);
        }
        // Playhead markers: one '>' per channel playing this row, stacked in channel colours.
        int k = 0;
        for (int ch = 0; ch < 4; ++ch)
            if (g.playRow[ch] == r) drawTextAt(ctx, ">", L.markerX + ui::kGridMarkerStep * k++, ty, ui::kGridScale, channelColour(ch));
    }

    // Side column: table number and name, the channels using it, the edit position.
    const ui::Rect sd = L.side;
    int y = sd.y + oy + 2;
    char b[32];
    std::snprintf(b, sizeof(b), "TABLE %02d", g.table + 1);
    drawTextAt(ctx, b, sd.x, y, 2, t.lcdInk);
    y += 20;
    drawTextAt(ctx, g.name, sd.x, y, 2, t.lcdPlayheadInk);
    y += 30;
    drawTextAt(ctx, "USED BY", sd.x, y, 1, t.lcdInk);
    y += 12;
    int n = 0;
    for (int ch = 0; ch < 4; ++ch)
    {
        if (!g.usedBy[ch]) continue;
        const int cx = sd.x + (n % 2) * 52, cy = y + (n / 2) * 24;
        fillRect(ctx, cx, cy, 46, 20, channelColour(ch));
        drawText(ctx, ui::channelTag(ch), {cx, cy, 46, 20}, 2, ui::Justify::Centre, t.tabOnText);
        ++n;
    }
    if (n == 0) drawTextAt(ctx, "NO CHANNEL", sd.x, y + 4, 1, t.lcdInk);
    y += 52;
    drawTextAt(ctx, "PLAYING", sd.x, y, 1, t.lcdInk);
    y += 12;
    int m = 0;
    for (int ch = 0; ch < 4; ++ch)
    {
        if (g.playRow[ch] < 0) continue;
        std::snprintf(b, sizeof(b), "%s %02X", ui::channelTag(ch), g.playRow[ch]);
        drawTextAt(ctx, ">", sd.x, y + m * 12, 1, channelColour(ch));
        drawTextAt(ctx, b, sd.x + 10, y + m * 12, 1, t.lcdPlayheadInk);
        ++m;
    }
    if (m == 0) drawTextAt(ctx, "STOPPED", sd.x, y, 1, t.lcdInk);
    const int by = sd.y + sd.h + oy - 40;
    std::snprintf(b, sizeof(b), "ROW %02X", g.cursor);
    drawTextAt(ctx, b, sd.x, by, 2, t.lcdInk);
    if (g.caret >= 1 && g.caret < ncols)
    {
        static const char* const kColNames[7] = {"ROW", "VOL", "SEMI", "CMD 1", "VAL 1", "CMD 2", "VAL 2"};
        std::snprintf(b, sizeof(b), "DRAG: %s", kColNames[g.caret]);
        drawTextAt(ctx, b, sd.x, by + 22, 1, t.lcdPlayheadInk);
    }
}

// ---- channel LCD ----------------------------------------------------------------------------
void drawChannelLcd(CGContextRef ctx, const ChannelLcdData& d, const ui::Rect& panel, std::uint32_t accent, int oy)
{
    const ui::Theme& t = ui::kTheme;
    const ui::ChannelLcdLayout L = ui::channelLcdLayout(panel);
    const ui::Rect f = L.field;
    fillRect(ctx, f.x, f.y + oy, f.w, f.h, t.lcdField);
    for (int i = 0; i < 4; ++i) dottedRule(ctx, L.dividers[i], f.y + 6 + oy, f.y + f.h - 4 + oy, t.lcdPlayhead);
    const int capY = f.y + 6 + oy;

    // Tag and On LED.
    drawTextAt(ctx, ui::channelTag(d.channel), L.tag.x, f.y + 8 + oy, 4, t.lcdInk);
    const int ly = f.y + f.h - 22 + oy;
    fillRect(ctx, L.tag.x, ly, 14, 12, d.on ? accent : t.lcdBeat);
    if (d.on) fillRect(ctx, L.tag.x + 2, ly + 2, 4, 4, 0xffffffffu);
    drawTextAt(ctx, d.on ? "ON" : "OFF", L.tag.x + 22, ly - 1, 2, d.on ? t.lcdInk : t.lcdPlayhead);

    // Tone: duty, wave shape (the wave bytes) or a noise motif.
    drawTextAt(ctx, d.toneText, L.tone.x, capY, 1, t.lcdInk);
    const ui::Rect tb = {L.tone.x, f.y + 22 + oy, L.tone.w, f.h - 30};
    const int lo = tb.y + tb.h - 2, hi = tb.y + 2;
    if (d.channel == 2)
    {
        const int sw = tb.w / 32;
        for (int i = 0; i < 32; ++i)
        {
            const int v = mpcboy::waveSample(d.wave, i);
            const int y = lo - v * (lo - hi) / 15;
            fillRect(ctx, tb.x + i * sw, y, sw, lo + 2 - y, t.lcdBeat);
            fillRect(ctx, tb.x + i * sw, y, sw, 2, t.lcdInk);
            if (i > 0)
            {
                const int pv = mpcboy::waveSample(d.wave, i - 1);
                const int py = lo - pv * (lo - hi) / 15;
                fillRect(ctx, tb.x + i * sw, std::min(py, y), 2, std::abs(py - y) + 2, t.lcdInk);
            }
        }
    }
    else if (d.channel == 3)
    {
        std::uint8_t bits[64];
        ui::noiseMotif(d.tone != 0, bits, 64);
        const int bw = 4;
        const int n = std::min(64, tb.w / bw);
        for (int i = 0; i < n; ++i)
        {
            const int y = bits[i] ? hi : lo;
            fillRect(ctx, tb.x + i * bw, y, bw, 2, t.lcdInk);
            if (i > 0 && bits[i] != bits[i - 1]) fillRect(ctx, tb.x + i * bw, hi, 2, lo - hi + 2, t.lcdInk);
        }
    }
    else
    {
        static const int kEighths[4] = {1, 2, 4, 6};   // 12.5 / 25 / 50 / 75 %
        const int period = tb.w / 2;
        const int high = period * kEighths[std::clamp(d.tone, 0, 3)] / 8;
        for (int k = 0; k < 2; ++k)
        {
            const int x0 = tb.x + k * period;
            fillRect(ctx, x0, hi, 2, lo - hi + 2, t.lcdInk);
            fillRect(ctx, x0, hi, high, 2, t.lcdInk);
            fillRect(ctx, x0 + high, hi, 2, lo - hi + 2, t.lcdInk);
            fillRect(ctx, x0 + high, lo, period - high, 2, t.lcdInk);
        }
        fillRect(ctx, tb.x + 2 * period, hi, 2, lo - hi + 2, t.lcdInk);
    }

    // Envelope steps over a fixed half-second window (Wave: its flat output level).
    drawTextAt(ctx, d.envText, L.env.x, capY, 1, t.lcdInk);
    const ui::Rect eb = {L.env.x, f.y + 22 + oy, L.env.w, f.h - 30};
    const int ebottom = eb.y + eb.h;
    if (d.channel == 2)
    {
        static const int kWaveLevels[4] = {0, 3, 7, 15};   // Mute, 25, 50, 100 %
        const int h = kWaveLevels[std::clamp(d.level, 0, 3)] * eb.h / 15;
        for (int x = eb.x; x + 4 <= eb.x + eb.w; x += 6) fillRect(ctx, x, ebottom - h, 4, h, t.lcdInk);
        if (h == 0) fillRect(ctx, eb.x, ebottom - 2, eb.w, 2, t.lcdPlayhead);
    }
    else
    {
        // A step lasts envTime / 64 s; the window shows 32 / envTime steps (0: no envelope).
        const int cw = d.envTime > 0 ? std::max(3, eb.w * d.envTime / 32) : 6;
        int step = 0;
        for (int x = eb.x; x + cw - 1 <= eb.x + eb.w; x += cw, ++step)
        {
            const int lv = ui::envelopeLevel(d.level, d.envUp != 0, d.envTime, step);
            const int h = lv * eb.h / 15;
            if (h > 0) fillRect(ctx, x, ebottom - h, cw - 1, h, t.lcdInk);
            else fillRect(ctx, x, ebottom - 2, cw - 1, 2, t.lcdPlayhead);
        }
    }

    // Note and live level.
    drawTextAt(ctx, "NOTE", L.note.x, capY, 1, t.lcdInk);
    drawTextAt(ctx, d.note, L.note.x, f.y + 22 + oy, 3, d.note[0] == '-' ? t.lcdPlayhead : t.lcdPlayheadInk);
    drawTextAt(ctx, "LEVEL", L.note.x, f.y + 52 + oy, 1, t.lcdInk);
    for (int i = 0; i < 15; ++i)
        fillRect(ctx, L.note.x + i * 6, f.y + 64 + oy, 5, 14, i < d.liveLevel ? t.lcdInk : t.lcdBeat);

    // Table window: the playhead row +-3.
    const ui::Rect tw = L.table;
    if (d.table <= 0)
    {
        drawTextAt(ctx, "TABLE", tw.x, capY, 1, t.lcdInk);
        drawText(ctx, "TABLE OFF", {tw.x, f.y + oy, tw.w, f.h}, 2, ui::Justify::Centre, t.lcdInk);
        return;
    }
    const int rowsH = ui::kLcdTablePitch * ui::kLcdTableRows;
    const int ry0 = f.y + (f.h - rowsH) / 2 + oy;
    const int listW = 128;
    for (int i = 0; i < ui::kLcdTableRows; ++i)
    {
        const int r = d.first + i;
        const int y = ry0 + i * ui::kLcdTablePitch;
        std::uint32_t ink = t.lcdInk;
        if (r == d.playRow)
        {
            fillRect(ctx, tw.x, y, listW, ui::kLcdTablePitch - 1, t.lcdPlayhead);
            ink = t.lcdPlayheadInk;
            drawTextAt(ctx, ">", tw.x + 2, y + 2, 1, accent);
        }
        else if (ui::isBeatRow(r))
        {
            fillRect(ctx, tw.x, y, listW, ui::kLcdTablePitch - 1, t.lcdBeat);
        }
        drawTextAt(ctx, d.rows[i], tw.x + 10, y + 2, 1, ink);
    }
    const int rx = tw.x + listW + 14;
    drawTextAt(ctx, d.tableName, rx, capY, 1, t.lcdInk);
    drawTextAt(ctx, "ROW", rx, f.y + 22 + oy, 1, t.lcdInk);
    char b[8];
    if (d.running) std::snprintf(b, sizeof(b), "%02X", d.playRow);
    else std::snprintf(b, sizeof(b), "--");
    drawTextAt(ctx, b, rx, f.y + 34 + oy, 4, d.running ? t.lcdPlayheadInk : t.lcdPlayhead);
    drawTextAt(ctx, d.running ? "PLAYING" : "STOPPED", rx, f.y + 72 + oy, 1, d.running ? t.lcdInk : t.lcdPlayhead);
}

// ---- top band breadcrumb ---------------------------------------------------------------------
void drawBreadcrumb(CGContextRef ctx, const char* text, int page)
{
    if (text[0] == '\0') return;
    const ui::Rect r = ui::breadcrumbRect(page);
    const int w = ui::textWidth(text, ui::kBandTextScale);
    // The rule restarts 12 units after the text, as it does after the band legends.
    fillRect(ctx, r.x, r.y, std::min(r.w, w + 24), r.h, ui::kTheme.bezel);
    drawTextAt(ctx, text, r.x + 12, ui::kBandTextY, ui::kBandTextScale, ui::kTheme.text);
}

void drawControl(CGContextRef ctx, Art& art, const Snapshot& s, const ui::Control& c, int oy)
{
    const ui::Theme& t = ui::kTheme;
    const ui::ComponentDef& d = ui::componentOf(c);
    const float v = c.param < s.numParams ? s.norm[c.param] : 0.0f;
    const std::string name = c.param < s.numParams ? upper(s.name[c.param]) : std::string();
    const std::string value = c.param < s.numParams ? upper(s.value[c.param]) : std::string();
    for (int i = 0; i < d.numChildren; ++i)
    {
        const ui::ChildDef& ch = d.children[i];
        if (ch.onlyWhenFocused) continue;
        ui::Rect r = ui::childRect(c, ch);
        r.y += oy;
        switch (ch.type)
        {
            case ui::ChildType::Knob:
            {
                const ui::Asset* a = ui::findAsset(ch.image);
                const int frames = a ? a->frames : ch.frames;
                if (CGImageRef f = art.frame(ch.image, ui::filmFrame(v, frames))) drawImage(ctx, f, r.x, r.y);
                else
                {
                    fillRect(ctx, r.x, r.y, r.w, r.h, t.rubber);
                    fillRect(ctx, r.x, r.y + r.h - 6, static_cast<int>(std::lround(r.w * v)), 6, c.accent);
                }
                break;
            }
            case ui::ChildType::Button:
            {
                const int steps = std::max(ch.buttonsInGroup, 2);
                const bool lit = ui::stepOf(v, steps) == ch.buttonId;
                const char* file = lit ? ch.image : ch.offImage;
                const ui::Asset* a = ui::findAsset(file);
                if (CGImageRef f = art.frame(file, 0))
                    drawImage(ctx, f, r.x + (r.w - a->width) / 2, r.y + (r.h - a->height) / 2);
                else
                {
                    const int w = a ? a->width : r.w, h = a ? a->height : r.h;
                    fillRect(ctx, r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h, lit ? c.accent : t.rubber);
                }
                break;
            }
            case ui::ChildType::Label:
            {
                const std::string& text = ch.label == ui::LabelKind::Name ? name : value;
                drawText(ctx, text, r, ui::labelScale(ch.fontHeight), ch.justify, ch.colour);
                break;
            }
            case ui::ChildType::Slider:
            {
                // The firmware draws its own (unthemable) slider; this matches the previews:
                // name, value and a thin value bar.
                drawText(ctx, name, {r.x, r.y, r.w, ui::kSliderRowHeight}, ui::labelScale(ui::kSliderNameFontHeight),
                         ui::Justify::Left, t.muted);
                drawText(ctx, value, {r.x, r.y + ui::kSliderRowHeight, r.w, ui::kSliderRowHeight},
                         ui::labelScale(ui::kSliderValueFontHeight), ui::Justify::Left, t.text);
                frameRect(ctx, r.x, r.y + r.h - ui::kSliderBarBottom, r.w, ui::kSliderBarHeight, t.muted);
                fillRect(ctx, r.x + 1, r.y + r.h - ui::kSliderBarBottom + 1, static_cast<int>((r.w - 2) * v),
                         ui::kSliderBarHeight - 2, c.accent);
                break;
            }
            default:
                break;   // focus rings and overlays belong to the MPC firmware
        }
    }
    for (int k = 0; k < c.numCaptions; ++k)
    {
        ui::Rect r = c.captions[k].bounds;
        r.y += oy;
        drawText(ctx, upper(c.captions[k].text), r, ui::captionStyle(c.captions[k]), ui::Justify::Centre, t.text);
    }
}

// True for a dynamic inside the page's live channel LCD (the editor draws the live LCD there).
bool insideChannelLcd(int page, const ui::Dynamic& d)
{
    const ui::Panel* p = ui::channelPanel(page);
    if (!p) return false;
    const ui::Rect& b = p->bounds;
    return d.bounds.x >= b.x && d.bounds.y >= b.y && d.bounds.x + d.bounds.w <= b.x + b.w && d.bounds.y + d.bounds.h <= b.y + b.h;
}

// The MPC's parameter-bound decorations (film strips, value labels), drawn from the same data.
// Those inside an LCD are skipped: the editor draws its live grid / channel LCD there instead
// (never both).
void drawDynamic(CGContextRef ctx, Art& art, const Snapshot& s, const ui::Dynamic& d, int oy)
{
    if (!d.editor || d.param >= s.numParams || insideChannelLcd(s.page, d)) return;
    const float v = s.norm[d.param];
    if (d.kind == ui::DynamicKind::FilmStrip)
    {
        const ui::Asset* a = ui::findAsset(d.image);
        const int frames = a ? a->frames : d.frames;
        if (CGImageRef f = art.frame(d.image, ui::filmFrame(v, frames))) drawImage(ctx, f, d.bounds.x, d.bounds.y + oy);
        else fillRect(ctx, d.bounds.x, d.bounds.y + oy, d.bounds.w, d.bounds.h, ui::kTheme.lcdField);
        return;
    }
    ui::Rect r = d.bounds;
    r.y += oy;
    drawText(ctx, upper(s.value[d.param]), r, ui::labelScale(d.fontHeight), d.justify, d.colour);
}

void drawTabBar(CGContextRef ctx, Art& art, int page)
{
    const ui::Theme& t = ui::kTheme;
    const ui::TabBar& tb = ui::kTabBar;
    if (CGImageRef bar = art.frame(tb.background, 0)) drawImage(ctx, bar, tb.bounds.x, tb.bounds.y);
    else fillRect(ctx, tb.bounds.x, tb.bounds.y, tb.bounds.w, tb.bounds.h, t.shell);
    for (int i = 0; i < ui::kNumPages; ++i)
    {
        const bool on = i == page;
        const ui::Rect b = ui::tabButtonRect(i);
        if (CGImageRef f = art.frame(on ? tb.onImage : tb.offImage, 0)) drawImage(ctx, f, b.x, b.y);
        else fillRect(ctx, b.x, b.y, b.w, b.h, on ? t.magenta : t.rubber);
        drawText(ctx, ui::page(i).title, b, ui::labelScale(ui::kTabTitleFontHeight), ui::Justify::Centre, on ? t.tabOnText : t.text);
    }
}

// The whole editor in a flipped context (origin top-left, one unit per design unit).
void drawEditor(CGContextRef ctx, Art& art, const Snapshot& s)
{
    CGContextSaveGState(ctx);   // leave the host's / caller's context state as found
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextSetShouldAntialias(ctx, false);
    CGContextSetAllowsAntialiasing(ctx, false);
    const ui::Page& pg = ui::page(s.page);
    const ui::Rect content = ui::contentRect();
    const int oy = content.y;
    if (CGImageRef bg = art.frame(pg.background, 0)) drawImage(ctx, bg, content.x, oy);
    else fillRect(ctx, content.x, content.y, content.w, content.h, ui::kTheme.background);
    if (s.hasTable) drawTableGrid(ctx, s.grid, oy);
    if (const ui::Panel* lcd = ui::channelPanel(s.page)) drawChannelLcd(ctx, s.lcd, lcd->bounds, pg.accent, oy);
    drawBreadcrumb(ctx, s.crumb, s.page);
    for (int i = 0; i < pg.numDynamics; ++i) drawDynamic(ctx, art, s, pg.dynamics[i], oy);
    for (int i = 0; i < pg.numControls; ++i)
    {
        const ui::Control& c = pg.controls[i];
        if (ui::isVisible(c, s.norm, s.numParams)) drawControl(ctx, art, s, c, oy);
    }
    drawTabBar(ctx, art, s.page);
    CGContextRestoreGState(ctx);
}

std::string defaultArtDir() { return mpc::pluginFolder() + "/ui"; }

}  // namespace

// ---- the view -------------------------------------------------------------------------------
@class MBEditorView;

@interface MBTimerProxy : NSObject
@property(nonatomic, weak) MBEditorView* view;
- (void)tick:(NSTimer*)timer;
@end

@interface MBEditorView : NSView
- (instancetype)initWithPlugin:(MpcBoyPlugin*)plugin art:(std::shared_ptr<Art>)art;
- (void)teardown;
- (void)refresh;
@end

@implementation MBTimerProxy
- (void)tick:(NSTimer*)timer
{
    MBEditorView* v = self.view;
    if (v) [v refresh];
    else [timer invalidate];
}
@end

@implementation MBEditorView
{
    MpcBoyPlugin* plugin_;            // raw back-pointer, cleared by teardown
    std::shared_ptr<Art> art_;
    std::unique_ptr<Snapshot> shown_;
    std::unique_ptr<Snapshot> next_;
    std::unique_ptr<mpcboy::Content> scratch_;
    int page_;
    int caret_;                       // TABLE grid column caret (1..6): the cell a grid drag edits
    NSTimer* timer_;
    ui::Gesture gesture_;
    id resignObserver_;
}

- (instancetype)initWithPlugin:(MpcBoyPlugin*)plugin art:(std::shared_ptr<Art>)art
{
    self = [super initWithFrame:NSMakeRect(0, 0, kW, kH)];
    if (self)
    {
        plugin_ = plugin;
        art_ = std::move(art);
        shown_ = std::make_unique<Snapshot>();
        next_ = std::make_unique<Snapshot>();
        scratch_ = std::make_unique<mpcboy::Content>();
        page_ = 0;
        caret_ = 1;
        takeSnapshot(*plugin_, page_, caret_, *scratch_, *shown_);
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }

- (void)startTimer
{
    if (timer_ || !plugin_) return;
    MBTimerProxy* proxy = [[MBTimerProxy alloc] init];
    proxy.view = self;
    timer_ = [NSTimer timerWithTimeInterval:1.0 / 30.0 target:proxy selector:@selector(tick:) userInfo:nil repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
}

- (void)stopTimer
{
    [timer_ invalidate];
    timer_ = nil;
}

- (void)endEdits
{
    if (gesture_.active()) gesture_.cancel();
    if (plugin_) plugin_->endAllEditorEdits();
}

- (void)teardown
{
    [self endEdits];
    [self stopTimer];
    if (resignObserver_)
    {
        [[NSNotificationCenter defaultCenter] removeObserver:resignObserver_];
        resignObserver_ = nil;
    }
    plugin_ = nullptr;
}

- (void)dealloc
{
    [self teardown];
}

- (void)viewWillMoveToWindow:(NSWindow*)window
{
    if (resignObserver_)
    {
        [[NSNotificationCenter defaultCenter] removeObserver:resignObserver_];
        resignObserver_ = nil;
    }
    if (window == nil)
    {
        [self endEdits];
        [self stopTimer];
    }
    [super viewWillMoveToWindow:window];
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    if (self.window && plugin_)
    {
        [self startTimer];
        __weak MBEditorView* weakSelf = self;
        resignObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidResignKeyNotification
                        object:self.window
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      MBEditorView* v = weakSelf;
                      if (v) [v endEdits];
                    }];
    }
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    if (!ctx) return;
    drawEditor(ctx, *art_, *shown_);
}

// Rebuild the snapshot and invalidate what changed.
- (void)refresh
{
    if (!plugin_) return;
    // Nothing to show while the window is hidden, minimised or fully covered (a page switch is
    // still taken, so clicks and the next draw see the page the user picked).
    if (self.window && !(self.window.occlusionState & NSWindowOcclusionStateVisible) && shown_->page == page_) return;
    takeSnapshot(*plugin_, page_, caret_, *scratch_, *next_);
    Snapshot& a = *shown_;
    Snapshot& b = *next_;
    if (a.page != b.page)
    {
        [self setNeedsDisplay:YES];
    }
    else
    {
        const ui::Page& pg = ui::page(b.page);
        const int oy = ui::contentRect().y;
        for (int i = 0; i < pg.numControls; ++i)
        {
            const ui::Control& c = pg.controls[i];
            const int p = c.param;
            if (p >= b.numParams) continue;
            const bool changed = a.norm[p] != b.norm[p] || a.value[p] != b.value[p] ||
                                 ui::isVisible(c, a.norm, a.numParams) != ui::isVisible(c, b.norm, b.numParams);
            if (changed) [self setNeedsDisplayInRect:NSMakeRect(c.slot.x, c.slot.y + oy, c.slot.w, c.slot.h)];
        }
        for (int i = 0; i < pg.numDynamics; ++i)
        {
            const ui::Dynamic& d = pg.dynamics[i];
            const int p = d.param;
            if (!d.editor || p >= b.numParams || insideChannelLcd(b.page, d)) continue;
            if (a.norm[p] != b.norm[p] || a.value[p] != b.value[p])
                [self setNeedsDisplayInRect:NSMakeRect(d.bounds.x, d.bounds.y + oy, d.bounds.w, d.bounds.h)];
        }
        // Live areas: only the rects whose content changed (the playhead moves at the table rate).
        if (b.hasTable && std::memcmp(&a.grid, &b.grid, sizeof(GridData)) != 0)
        {
            const ui::Rect pb = ui::kTableGrid.panelBounds;
            [self setNeedsDisplayInRect:NSMakeRect(pb.x, pb.y + oy, pb.w, pb.h)];
        }
        if (const ui::Panel* lcd = ui::channelPanel(b.page))
            if (std::memcmp(&a.lcd, &b.lcd, sizeof(ChannelLcdData)) != 0)
                [self setNeedsDisplayInRect:NSMakeRect(lcd->bounds.x, lcd->bounds.y + oy, lcd->bounds.w, lcd->bounds.h)];
        if (std::memcmp(a.crumb, b.crumb, sizeof(a.crumb)) != 0)
        {
            const ui::Rect r = ui::breadcrumbRect(b.page);
            [self setNeedsDisplayInRect:NSMakeRect(r.x, r.y, r.w, r.h)];
        }
    }
    std::swap(shown_, next_);
}

- (ui::Point)designPoint:(NSEvent*)event
{
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    return {static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))};
}

// Apply one value from a gesture: triggers always fire, everything else goes through as is.
- (void)apply:(int)param value:(float)v
{
    if (!plugin_) return;
    if (param >= mpcboy::Imp_Go && param <= mpcboy::Imp_Rescan) v = 1.0f;
    plugin_->editorSet(param, v);
    [self refresh];
}

- (void)mouseDown:(NSEvent*)event
{
    if (!plugin_) return;
    const ui::Point w = [self designPoint:event];
    const int tab = ui::hitTab(w.x, w.y);
    if (tab >= 0)
    {
        if (tab != page_)
        {
            [self endEdits];
            page_ = tab;
            [self refresh];
            [self setNeedsDisplay:YES];
        }
        return;
    }
    const ui::Rect content = ui::contentRect();
    if (!ui::contains(content, w.x, w.y)) return;
    const int x = w.x - content.x, y = w.y - content.y;
    const Snapshot& s = *shown_;
    // MAIN header-band On toggles (Mac only; the MPC shows the LEDs only).
    const int toggle = ui::hitHeaderToggle(page_, x, y);
    if (toggle >= 0)
    {
        [self endEdits];
        mpcboy::editorToggle(*plugin_, ui::page(page_).dynamics[toggle].param);
        [self refresh];
        return;
    }
    // TABLE grid: a click selects the row (Edit Row) and, on a cell column, the caret; a vertical
    // drag from the cell then edits that column's Row control like a knob.
    if (page_ == ui::kTableGrid.page)
    {
        const ui::GridHit gh = ui::hitGrid(x, y);
        if (gh.row >= 0)
        {
            [self endEdits];
            mpcboy::editorSelectRow(*plugin_, gh.row);
            const int col = ui::gridEditorColumn(gh.column);
            if (col >= 0)
            {
                caret_ = gh.column;
                [self startCellDrag:col row:gh.row x:x y:y];
            }
            [self refresh];
            return;
        }
    }
    const ui::Hit hit = ui::hitTest(page_, x, y, s.norm, s.numParams);
    if (hit.control < 0) return;
    const ui::Control& c = ui::page(page_).controls[hit.control];
    if (c.param >= plugin_->numParams()) return;
    const ui::ParamInfo info = paramInfo(plugin_->param(c.param));
    const bool fine = (event.modifierFlags & NSEventModifierFlagShift) != 0;

    if (event.clickCount == 2 && hit.part == ui::Part::Knob)
    {
        // Double-click: back to the default.
        gesture_.cancel();
        plugin_->editorBeginEdit(c.param);
        [self apply:c.param value:info.defaultNorm];
        plugin_->editorEndEdit(c.param);
        return;
    }
    [self endEdits];   // a stray open edit (mouse-up lost) never stays open
    float out = 0.0f;
    if (!gesture_.press(c, hit, x, y, fine, plugin_->getNormalised(c.param), info, out)) return;
    plugin_->editorBeginEdit(c.param);
    if (out != plugin_->getNormalised(c.param) || gesture_.discrete()) [self apply:c.param value:out];
    if (gesture_.discrete())
    {
        plugin_->editorEndEdit(c.param);
        gesture_.cancel();
    }
}

// Starts a knob-like gesture on Row Vol .. Row Val2 (`col` 0..5) for the clicked row. The start
// value is the cell's own (from the published content), not the Row parameter, which the audio
// thread resyncs to the new row only at its next block. Nothing is applied until the mouse moves.
- (void)startCellDrag:(int)col row:(int)row x:(int)x y:(int)y
{
    const int param = mpcboy::ED_Vol + col;
    mpcboy::Content& c = *scratch_;
    plugin_->content().readPublished(c);
    const int table = std::clamp(plugin_->getInt(mpcboy::ED_Table) - 1, 0, mpcboy::kNumTables - 1);
    const int value = mpcboy::EditorSync::cellValue(c.tables.tables[table].rows[row], col);
    const mpc::Param& pp = plugin_->param(param);
    ui::Control knob{};
    knob.param = param;
    knob.kind = ui::WidgetKind::KnobUni;
    ui::Hit hit;
    hit.part = ui::Part::Knob;
    float out = 0.0f;
    if (!gesture_.press(knob, hit, x, y, false, pp.toNormalised(static_cast<float>(value)), paramInfo(pp), out)) return;
    plugin_->editorBeginEdit(param);   // captures the cursor just set: this table, this row
}

- (void)mouseDragged:(NSEvent*)event
{
    if (!plugin_ || !gesture_.active()) return;
    const ui::Point w = [self designPoint:event];
    const ui::Rect content = ui::contentRect();
    float out = 0.0f;
    if (gesture_.drag(w.x - content.x, w.y - content.y, (event.modifierFlags & NSEventModifierFlagShift) != 0, out))
        [self apply:gesture_.param() value:out];
}

- (void)mouseUp:(NSEvent*)event
{
    if (!plugin_ || !gesture_.active()) return;
    const ui::Point w = [self designPoint:event];
    const ui::Rect content = ui::contentRect();
    const int param = gesture_.param();
    float out = 0.0f;
    if (gesture_.release(w.x - content.x, w.y - content.y, (event.modifierFlags & NSEventModifierFlagShift) != 0, out))
        [self apply:param value:out];
    plugin_->editorEndEdit(param);
}

@end

// ---- MacEditor ------------------------------------------------------------------------------
namespace mpcboy {

struct MacEditor::Impl
{
    std::shared_ptr<Art> art;
    MBEditorView* view = nil;
};

MacEditor::MacEditor(MpcBoyPlugin& plugin) : plugin_(plugin) {}

MacEditor::~MacEditor()
{
    close();
    delete impl_;
}

bool MacEditor::open(void* parentView)
{
    if (parentView == nullptr) return false;
    // The host passes an NSView (or, from some hosts, its NSWindow): check before using it.
    id obj = (__bridge id)parentView;
    NSView* parent = nil;
    if ([obj isKindOfClass:[NSView class]]) parent = obj;
    else if ([obj isKindOfClass:[NSWindow class]]) parent = [(NSWindow*)obj contentView];
    if (parent == nil) return false;
    if (!impl_) impl_ = new Impl();
    if (!impl_->art)
    {
        impl_->art = std::make_shared<Art>();
        impl_->art->load(artDir_.empty() ? defaultArtDir() : artDir_);
    }
    if (impl_->view)
    {
        if (impl_->view.superview == parent) return true;
        [impl_->view removeFromSuperview];
        [parent addSubview:impl_->view];
        return true;
    }
    impl_->view = [[MBEditorView alloc] initWithPlugin:&plugin_ art:impl_->art];
    [parent addSubview:impl_->view];
    return true;
}

void MacEditor::close()
{
    if (!impl_ || !impl_->view) return;
    MBEditorView* v = impl_->view;
    impl_->view = nil;
    [v teardown];
    [v removeFromSuperview];
}

bool MacEditor::isOpen() const { return impl_ && impl_->view; }

bool renderEditorPng(MpcBoyPlugin& plugin, int page, double scale, const std::string& artDir, const std::string& pngPath)
{
    @autoreleasepool
    {
        if (scale <= 0.0) return false;
        Art art;
        art.load(artDir.empty() ? defaultArtDir() : artDir);
        auto snap = std::make_unique<Snapshot>();
        auto scratch = std::make_unique<Content>();
        takeSnapshot(plugin, page, 1, *scratch, *snap);

        const size_t w = static_cast<size_t>(std::lround(kW * scale)), h = static_cast<size_t>(std::lround(kH * scale));
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(nullptr, w, h, 8, 0, cs,
                                                 static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);
        if (!ctx) return false;
        // Flipped, scaled user space: the same coordinates the view draws in.
        CGContextTranslateCTM(ctx, 0, static_cast<CGFloat>(h));
        CGContextScaleCTM(ctx, scale, -scale);
        drawEditor(ctx, art, *snap);
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        if (!img) return false;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(pngPath.c_str()),
                                                               static_cast<CFIndex>(pngPath.size()), false);
        bool ok = false;
        if (url)
        {
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
            if (dst)
            {
                CGImageDestinationAddImage(dst, img, nullptr);
                ok = CGImageDestinationFinalize(dst);
                CFRelease(dst);
            }
            CFRelease(url);
        }
        CGImageRelease(img);
        return ok && art.count() == static_cast<size_t>(ui::kNumAssets);
    }
}

}  // namespace mpcboy
