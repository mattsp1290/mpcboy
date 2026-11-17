// src/plugin/mac_editor.mm - see mac_editor.h. Objective-C++, ARC (-fobjc-arc).
//
// Pieces:
//   Art        decodes each ui/art PNG once (ImageIO) and crops film-strip frames on demand.
//   Snapshot   what the editor shows: page, every parameter's normalised value and display
//              text, and the TABLE page grid (ContentStore::readPublished). Rebuilt at 30 Hz;
//              only controls whose value, text or visibility changed are invalidated.
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

// ---- snapshot -------------------------------------------------------------------------------
struct Snapshot
{
    int page = 0;
    int numParams = 0;
    float norm[mpcboy::kTotalParams] = {};
    std::string value[mpcboy::kTotalParams];
    std::string name[mpcboy::kTotalParams];
    bool hasTable = false;
    ui::TableView table{};
};

void takeSnapshot(MpcBoyPlugin& p, int page, mpcboy::Content& scratch, Snapshot& s)
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
    if (s.hasTable)
    {
        p.content().readPublished(scratch);
        ui::buildTableView(scratch, p.getInt(mpcboy::ED_Table) - 1, p.getInt(mpcboy::ED_Row), s.table);
    }
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

std::string upper(const std::string& s)
{
    std::string u = s;
    for (char& c : u)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return u;
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

void drawText(CGContextRef ctx, const std::string& text, const ui::Rect& bounds, int scale, ui::Justify j,
              std::uint32_t argb, bool clip = true)
{
    const ui::Point p = ui::placeText(text.c_str(), bounds, scale, j);
    setFill(ctx, argb);
    ui::rasterizeText(text.c_str(), p.x, p.y, scale, clip ? &bounds : nullptr,
                      [&](int x, int y, int w, int h) { CGContextFillRect(ctx, snap(ctx, CGRectMake(x, y, w, h))); });
}

void drawTextAt(CGContextRef ctx, const char* text, int x, int y, int scale, std::uint32_t argb)
{
    setFill(ctx, argb);
    ui::rasterizeText(text, x, y, scale, nullptr,
                      [&](int rx, int ry, int w, int h) { CGContextFillRect(ctx, snap(ctx, CGRectMake(rx, ry, w, h))); });
}

void drawTableGrid(CGContextRef ctx, Art& art, const Snapshot& s, int oy)
{
    const ui::TableGrid& g = ui::kTableGrid;
    const ui::Theme& t = ui::kTheme;
    const ui::Rect pb = g.panelBounds;
    if (CGImageRef panel = art.frame(g.panelImage, 0)) drawImage(ctx, panel, pb.x, pb.y + oy);
    else
    {
        fillRect(ctx, pb.x, pb.y + oy, pb.w, pb.h, t.labelAccent);
        fillRect(ctx, pb.x + 6, pb.y + oy + 6, pb.w - 12, pb.h - 12, t.lcdLightest);
    }
    const ui::Rect hb = ui::tableHeaderBand();
    fillRect(ctx, hb.x, hb.y + oy, hb.w, hb.h, t.lcdLight);
    drawTextAt(ctx, s.table.header.text, ui::tableCharX(0), ui::tableLineY(0) + oy, g.charScale, t.lcdDarkest);
    for (int i = 0; i < s.table.numRows; ++i)
    {
        const ui::TableLine& l = s.table.rows[i];
        const int y = ui::tableLineY(i + 1) + oy;
        std::uint32_t ink = t.lcdDarkest;
        if (l.cursor)
        {
            const ui::Rect cb = ui::tableCursorBand(i + 1);
            fillRect(ctx, cb.x, cb.y + oy, cb.w, cb.h, t.lcdDarkest);
            ink = t.lcdLightest;
            if (CGImageRef cur = art.frame(g.cursorImage, 0)) drawImage(ctx, cur, ui::tableCharX(0), y);
            else drawTextAt(ctx, ">", ui::tableCharX(0), y, g.charScale, t.lcdDarkest);
        }
        drawTextAt(ctx, l.text, ui::tableCharX(0), y, g.charScale, ink);
    }
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
                    fillRect(ctx, r.x, r.y, r.w, r.h, t.lcdDark);
                    fillRect(ctx, r.x, r.y + r.h - 6, static_cast<int>(std::lround(r.w * v)), 6, t.lcdLightest);
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
                    fillRect(ctx, r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h, lit ? t.lcdDarkest : t.lcdDark);
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
                         ui::kSliderBarHeight - 2, t.text);
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
        drawText(ctx, upper(c.captions[k].text), r, ui::labelScale(c.captions[k].fontHeight), ui::Justify::Centre, t.caption);
    }
}

void drawTabBar(CGContextRef ctx, Art& art, int page)
{
    const ui::Theme& t = ui::kTheme;
    const ui::TabBar& tb = ui::kTabBar;
    if (CGImageRef bar = art.frame(tb.background, 0)) drawImage(ctx, bar, tb.bounds.x, tb.bounds.y);
    else fillRect(ctx, tb.bounds.x, tb.bounds.y, tb.bounds.w, tb.bounds.h, t.shellLight);
    for (int i = 0; i < ui::kNumPages; ++i)
    {
        const bool on = i == page;
        const ui::Rect b = ui::tabButtonRect(i);
        if (CGImageRef f = art.frame(on ? tb.onImage : tb.offImage, 0)) drawImage(ctx, f, b.x, b.y);
        else fillRect(ctx, b.x, b.y, b.w, b.h, on ? t.magenta : t.labelAccent);
        drawText(ctx, ui::page(i).title, b, ui::labelScale(ui::kTabTitleFontHeight), ui::Justify::Centre, on ? t.caption : t.shellLight);
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
    if (s.hasTable) drawTableGrid(ctx, art, s, oy);
    for (int i = 0; i < pg.numControls; ++i)
    {
        const ui::Control& c = pg.controls[i];
        if (ui::isVisible(c, s.norm, s.numParams)) drawControl(ctx, art, s, c, oy);
    }
    drawTabBar(ctx, art, s.page);
    CGContextRestoreGState(ctx);
}

bool sameLine(const ui::TableLine& a, const ui::TableLine& b)
{
    return a.row == b.row && a.cursor == b.cursor && std::strcmp(a.text, b.text) == 0;
}

bool sameTable(const ui::TableView& a, const ui::TableView& b)
{
    if (a.numRows != b.numRows || !sameLine(a.header, b.header)) return false;
    for (int i = 0; i < a.numRows; ++i)
        if (!sameLine(a.rows[i], b.rows[i])) return false;
    return true;
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
        takeSnapshot(*plugin_, page_, *scratch_, *shown_);
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
    // Nothing to show while the window is hidden, minimised or fully covered.
    if (self.window && !(self.window.occlusionState & NSWindowOcclusionStateVisible)) return;
    takeSnapshot(*plugin_, page_, *scratch_, *next_);
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
        if (b.hasTable && (!a.hasTable || !sameTable(a.table, b.table)))
        {
            const ui::Rect pb = ui::kTableGrid.panelBounds;
            [self setNeedsDisplayInRect:NSMakeRect(pb.x, pb.y + oy, pb.w, pb.h)];
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
        takeSnapshot(plugin, page, *scratch, *snap);

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
