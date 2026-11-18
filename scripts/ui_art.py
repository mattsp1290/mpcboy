"""ui_art.py - pixel art for the mpcboy Game Boy UI (used by scripts/build_ui.py).

Everything is drawn on a small RGBA pixel grid ("art pixels") and scaled up by an
integer factor (2 design px per art pixel), so edges stay crisp. There is no
anti-aliasing and no floating-point blending in the art itself: output is a pure
function of the inputs, so the PNGs are byte-reproducible. Shading uses ordered
(Bayer) dithering between palette colours.

Standard library only; Python 3.9+.
"""

import math

SCALE = 2  # design px per art pixel

TRANSPARENT = b"\0\0\0\0"

# Ordered dithering threshold matrix (4 x 4 Bayer).
_BAYER = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))


def rgb(hex6, alpha=255):
    v = int(hex6[-6:], 16)
    return bytes(((v >> 16) & 255, (v >> 8) & 255, v & 255, alpha))


def mixc(a, b, t):
    return bytes(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3)) + bytes((a[3],))


def dither(x, y, t):
    """True where an ordered dither of coverage t (0..1) is lit."""
    return t > (_BAYER[y & 3][x & 3] + 0.5) / 16.0


def tone(x, y, v, ramp, sharp=2.5):
    """Pick from ramp (list of colours) at fractional index v, dithering between neighbours.

    sharp > 1 narrows the dithered band around each step, so most of a surface is flat
    colour with a short dithered transition (reads as pixel art, not noise)."""
    v = max(0.0, min(len(ramp) - 1.0, v))
    i = int(math.floor(v))
    if i >= len(ramp) - 1:
        return ramp[-1]
    f = max(0.0, min(1.0, (v - i - 0.5) * sharp + 0.5))
    return ramp[i + 1] if dither(x, y, f) else ramp[i]


class Pix:
    """A w x h RGBA image in a bytearray."""
    __slots__ = ("w", "h", "buf")

    def __init__(self, w, h, colour=None):
        self.w, self.h = w, h
        self.buf = bytearray((colour or TRANSPARENT) * (w * h))

    def copy(self):
        p = Pix.__new__(Pix)
        p.w, p.h, p.buf = self.w, self.h, bytearray(self.buf)
        return p

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * 4
            self.buf[i:i + 4] = c

    def get(self, x, y):
        i = (y * self.w + x) * 4
        return bytes(self.buf[i:i + 4])

    def rect(self, x, y, w, h, c):
        x0, x1 = max(0, x), min(self.w, x + w)
        if x1 <= x0:
            return
        run = c * (x1 - x0)
        for yy in range(max(0, y), min(self.h, y + h)):
            i = (yy * self.w + x0) * 4
            self.buf[i:i + len(run)] = run

    def frame(self, x, y, w, h, c):
        self.rect(x, y, w, 1, c)
        self.rect(x, y + h - 1, w, 1, c)
        self.rect(x, y, 1, h, c)
        self.rect(x + w - 1, y, 1, h, c)

    def blit(self, src, ox, oy, clip=None):
        """Source-over with src alpha (art is opaque or transparent; partial alpha is blended)."""
        cx0, cy0, cx1, cy1 = clip if clip else (0, 0, self.w, self.h)
        cx0, cy0, cx1, cy1 = max(cx0, 0), max(cy0, 0), min(cx1, self.w), min(cy1, self.h)
        sb, db = src.buf, self.buf
        for y in range(src.h):
            ty = y + oy
            if ty < cy0 or ty >= cy1:
                continue
            for x in range(src.w):
                tx = x + ox
                if tx < cx0 or tx >= cx1:
                    continue
                si = (y * src.w + x) * 4
                a = sb[si + 3]
                if a == 0:
                    continue
                di = (ty * self.w + tx) * 4
                if a == 255:
                    db[di:di + 4] = sb[si:si + 4]
                else:
                    k = a / 255.0
                    for j in range(3):
                        db[di + j] = int(round(sb[si + j] * k + db[di + j] * (1.0 - k)))
                    db[di + 3] = max(db[di + 3], a)

    def scaled(self, s=SCALE):
        out = Pix(self.w * s, self.h * s)
        rw = self.w * 4
        orw = out.w * 4
        for y in range(self.h):
            row = self.buf[y * rw:(y + 1) * rw]
            srow = b"".join(bytes(row[i:i + 4]) * s for i in range(0, rw, 4))
            for k in range(s):
                o = (y * s + k) * orw
                out.buf[o:o + orw] = srow
        return out

    def rows(self):
        rw = self.w * 4
        return [bytes(self.buf[y * rw:(y + 1) * rw]) for y in range(self.h)]


def strip_rows(frames):
    """Vertical film strip rows from a list of equally sized Pix frames."""
    rows = []
    for f in frames:
        rows.extend(f.rows())
    return rows


# --------------------------------------------------------------------------------------------
# 5 x 7 pixel font (shared with the Mac editor through src/ui/generated/pixel_font.inc)
# --------------------------------------------------------------------------------------------
FONT_W, FONT_H, FONT_ADVANCE = 5, 7, 6

_GLYPH_ART = {
    "A": ".###. #...# #...# ##### #...# #...# #...#",
    "B": "####. #...# #...# ####. #...# #...# ####.",
    "C": ".###. #...# #.... #.... #.... #...# .###.",
    "D": "####. #...# #...# #...# #...# #...# ####.",
    "E": "##### #.... #.... ####. #.... #.... #####",
    "F": "##### #.... #.... ####. #.... #.... #....",
    "G": ".###. #...# #.... #.### #...# #...# .####",
    "H": "#...# #...# #...# ##### #...# #...# #...#",
    "I": ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###.",
    "J": "..### ...#. ...#. ...#. ...#. #..#. .##..",
    "K": "#...# #..#. #.#.. ##... #.#.. #..#. #...#",
    "L": "#.... #.... #.... #.... #.... #.... #####",
    "M": "#...# ##.## #.#.# #.#.# #...# #...# #...#",
    "N": "#...# #...# ##..# #.#.# #..## #...# #...#",
    "O": ".###. #...# #...# #...# #...# #...# .###.",
    "P": "####. #...# #...# ####. #.... #.... #....",
    "Q": ".###. #...# #...# #...# #.#.# #..#. .##.#",
    "R": "####. #...# #...# ####. #.#.. #..#. #...#",
    "S": ".#### #.... #.... .###. ....# ....# ####.",
    "T": "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#..",
    "U": "#...# #...# #...# #...# #...# #...# .###.",
    "V": "#...# #...# #...# #...# #...# .#.#. ..#..",
    "W": "#...# #...# #.#.# #.#.# #.#.# #.#.# .#.#.",
    "X": "#...# #...# .#.#. ..#.. .#.#. #...# #...#",
    "Y": "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#..",
    "Z": "##### ....# ...#. ..#.. .#... #.... #####",
    "0": ".###. #...# #..## #.#.# ##..# #...# .###.",
    "1": "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###.",
    "2": ".###. #...# ....# ...#. ..#.. .#... #####",
    "3": "##### ...#. ..#.. ...#. ....# #...# .###.",
    "4": "...#. ..##. .#.#. #..#. ##### ...#. ...#.",
    "5": "##### #.... ####. ....# ....# #...# .###.",
    "6": "..##. .#... #.... ####. #...# #...# .###.",
    "7": "##### ....# ...#. ..#.. .#... .#... .#...",
    "8": ".###. #...# #...# .###. #...# #...# .###.",
    "9": ".###. #...# #...# .#### ....# ...#. .##..",
    " ": "..... ..... ..... ..... ..... ..... .....",
    ".": "..... ..... ..... ..... ..... .##.. .##..",
    ",": "..... ..... ..... ..... .##.. ..#.. .#...",
    "-": "..... ..... ..... ##### ..... ..... .....",
    "+": "..... ..#.. ..#.. ##### ..#.. ..#.. .....",
    "/": "..... ....# ...#. ..#.. .#... #.... .....",
    ":": "..... .##.. .##.. ..... .##.. .##.. .....",
    "%": "##... ##..# ...#. ..#.. .#... #..## ...##",
    "#": ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#.",
    ">": ".#... ..#.. ...#. ....# ...#. ..#.. .#...",
    "<": "...#. ..#.. .#... #.... .#... ..#.. ...#.",
    "_": "..... ..... ..... ..... ..... ..... #####",
    "?": ".###. #...# ....# ...#. ..#.. ..... ..#..",
    "!": "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#..",
    "(": "...#. ..#.. .#... .#... .#... ..#.. ...#.",
    ")": ".#... ..#.. ...#. ...#. ...#. ..#.. .#...",
    "'": "..#.. ..#.. .#... ..... ..... ..... .....",
    "=": "..... ..... ##### ..... ##### ..... .....",
    "*": "..... ..#.. #.#.# .###. #.#.# ..#.. .....",
}


def _parse_glyphs():
    glyphs = {}
    for ch, art in _GLYPH_ART.items():
        rows = art.split()
        assert len(rows) == FONT_H and all(len(r) == FONT_W for r in rows), ch
        glyphs[ch] = tuple(int(r.replace("#", "1").replace(".", "0"), 2) for r in rows)
    return glyphs


GLYPHS = _parse_glyphs()  # char -> 7 row bitmasks, bit 4 = leftmost column


def glyph_for(ch):
    ch = ch.upper()
    return GLYPHS.get(ch, GLYPHS["?"])


def text_width(text, scale=1):
    return (len(text) * FONT_ADVANCE - 1) * scale if text else 0


def draw_text(pix, text, x, y, scale, colour, shear=False, clip=None):
    """Draws text with its top-left at (x, y); each font pixel is scale x scale. Returns the width."""
    cx0, cy0, cx1, cy1 = clip if clip else (0, 0, pix.w, pix.h)
    for n, ch in enumerate(text):
        rows = glyph_for(ch)
        gx = x + n * FONT_ADVANCE * scale
        for r, bits in enumerate(rows):
            off = ((FONT_H - 1 - r) // 3) * scale if shear else 0
            for c in range(FONT_W):
                if bits & (1 << (FONT_W - 1 - c)):
                    px, py = gx + c * scale + off, y + r * scale
                    for yy in range(py, py + scale):
                        if cy0 <= yy < cy1:
                            for xx in range(px, px + scale):
                                if cx0 <= xx < cx1:
                                    pix.set(xx, yy, colour)
    return text_width(text, scale)


# --------------------------------------------------------------------------------------------
# 3 x 5 micro font (Q-Link badges)
# --------------------------------------------------------------------------------------------
_MICRO_ART = {
    "Q": ".##. #..# #..# #.#. .#.#", "0": "### #.# #.# #.# ###", "1": ".#. ##. .#. .#. ###",
    "2": "### ..# ### #.. ###", "3": "### ..# .## ..# ###", "4": "#.# #.# ### ..# ..#",
    "5": "### #.. ### ..# ###", "6": "### #.. ### #.# ###", "7": "### ..# .#. .#. .#.",
    "8": "### #.# ### #.# ###", "9": "### #.# ### ..# ###",
}
MICRO = {ch: tuple(r for r in art.split()) for ch, art in _MICRO_ART.items()}


def micro_width(text, scale=1):
    return (sum(len(MICRO[ch][0]) + 1 for ch in text) - 1) * scale if text else 0


def draw_micro(pix, text, x, y, scale, colour):
    cx = 0
    for ch in text:
        rows = MICRO[ch]
        for r, row in enumerate(rows):
            for c, bit in enumerate(row):
                if bit == "#":
                    pix.rect(x + (cx + c) * scale, y + r * scale, scale, scale, colour)
        cx += len(rows[0]) + 1
    return micro_width(text, scale)


# --------------------------------------------------------------------------------------------
# Palette ("gb.palette" roles plus the channel / DMG accent colours "gb.colours")
# --------------------------------------------------------------------------------------------
class Palette:
    def __init__(self, spec, colours):
        c = {k: rgb(v) for k, v in spec.items()}
        self.overlay = c["overlay"]
        self.bezel = c["bezel"]
        self.plate = c["plate"]
        self.plate_hi = c["plateHighlight"]
        self.shadow = c["plateShadow"]
        self.text = c["text"]
        self.text_dim = c["textDim"]
        self.shell = c["shell"]
        self.shell_hi = c["shellHighlight"]
        self.shell_dark = c["shellDark"]
        self.pointer = c["pointer"]
        self.rubber = c["rubber"]
        self.rubber_sel = c["rubberSelected"]
        self.track = c["arcTrack"]
        self.magenta = c["magenta"]
        self.lcd_field = c["lcdField"]
        self.lcd_ink = c["lcdInk"]
        self.lcd_cursor = c["lcdCursor"]
        self.lcd_cursor_ink = c["lcdCursorInk"]
        self.lcd_beat = c["lcdBeat"]
        self.cartridge = c["cartridge"]
        self.tab_on_text = c["tabOnText"]
        self.colours = {k: rgb(v) for k, v in colours.items()}
        # Derived shades, all computed from the palette above.
        self.recess = mixc(self.bezel, self.overlay, 0.55)
        self.decal = mixc(self.text_dim, self.bezel, 0.45)
        self.rubber_hi = mixc(self.rubber, self.text, 0.16)
        self.rubber_lo = mixc(self.rubber, self.shadow, 0.35)
        self.led_off = mixc(self.overlay, self.rubber, 0.35)
        self.lcd_dim = mixc(self.lcd_field, self.lcd_ink, 0.38)

    def accent(self, name):
        return self.colours[name]


def lighter(c, t=0.45):
    return mixc(c, rgb("ffffff"), t)


def darker(c, t=0.35):
    return mixc(c, rgb("000000"), t)


# --------------------------------------------------------------------------------------------
# Knob: 45 x 45 art px (90 x 90 design): shell-plastic cap, navy pointer, stepped arc
# --------------------------------------------------------------------------------------------
KNOB_ART = 45
_KC = 22  # centre pixel
_SWEEP = 135.0
_ARC_SEGMENTS = 28            # the arc is drawn as discrete pixel steps
_ARC_STEP = 2.0 * _SWEEP / _ARC_SEGMENTS


def _ang(dx, dy):
    """Degrees, 0 at 12 o'clock, clockwise positive."""
    return math.degrees(math.atan2(dx, -dy))


def _knob_static(P):
    img = Pix(KNOB_ART, KNOB_ART)
    ring = []
    body_r, out_r = 14.6, 15.6
    ramp = [P.shell_dark, P.shell, P.shell_hi]
    for y in range(KNOB_ART):
        for x in range(KNOB_ART):
            dx, dy = x - _KC, y - _KC
            d = math.hypot(dx, dy)
            a = _ang(dx, dy)
            if 18.4 <= d <= 21.5 and -_SWEEP - 0.01 <= a <= _SWEEP + 0.01:
                k = min(_ARC_SEGMENTS - 1, int((a + _SWEEP) // _ARC_STEP))
                frac = (a + _SWEEP) - k * _ARC_STEP
                if frac < 3.4:  # the 1-pixel gap between steps
                    continue
                img.set(x, y, P.track)
                ring.append((x, y, k))
                continue
            if d <= body_r:
                l = (-0.55 * dx - 0.83 * dy) / body_r  # lit from the top-left
                if d <= 10.2:
                    img.set(x, y, tone(x, y, 1.15 - 0.55 * l, ramp))   # dished face: reversed shading
                elif d <= 13.4:
                    img.set(x, y, tone(x, y, 1.0 + 1.1 * l, ramp))
                else:
                    img.set(x, y, P.shell_dark if l < 0.35 else P.shell)  # rim
            elif d <= out_r:
                img.set(x, y, P.shadow)
            elif math.hypot(dx - 1, dy - 1.5) <= out_r:
                img.set(x, y, P.shadow)  # drop shadow, bottom right
    return img, ring


def _seg_dist(px, py, x0, y0, x1, y1):
    vx, vy = x1 - x0, y1 - y0
    ll = vx * vx + vy * vy or 1.0
    t = max(0.0, min(1.0, ((px - x0) * vx + (py - y0) * vy) / ll))
    return math.hypot(px - (x0 + vx * t), py - (y0 + vy * t))


def render_knob(P, frames, bipolar, accent):
    static, ring = _knob_static(P)
    tip = lighter(accent, 0.4)
    centre = _ARC_SEGMENTS // 2
    out = []
    for i in range(frames):
        t = i / (frames - 1.0)
        a = -_SWEEP + 2.0 * _SWEEP * t
        img = static.copy()
        pos = (a + _SWEEP) / _ARC_STEP  # value position in segments
        if bipolar:
            lo, hi = sorted((centre, pos))
            lit = [k for k in range(_ARC_SEGMENTS) if abs(pos - centre) > 0.25 and k + 1 > lo and k < hi]
        else:
            lit = [k for k in range(_ARC_SEGMENTS) if i > 0 and k < pos]
        lit = set(lit)
        end = max(lit) if lit and (not bipolar or pos >= centre) else (min(lit) if lit else None)
        for x, y, k in ring:
            if k in lit:
                img.set(x, y, tip if k == end else accent)
        if bipolar:
            img.rect(_KC, 0, 1, 3, P.text_dim)
        # pointer: a navy bar from the centre towards the value
        s, c = math.sin(math.radians(a)), -math.cos(math.radians(a))
        x0, y0 = _KC + s * 2.5, _KC + c * 2.5
        x1, y1 = _KC + s * 12.2, _KC + c * 12.2
        for y in range(_KC - 14, _KC + 15):
            for x in range(_KC - 14, _KC + 15):
                if _seg_dist(x, y, x0, y0, x1, y1) <= 1.05:
                    img.set(x, y, P.pointer)
        out.append(img.scaled())
    return out


# --------------------------------------------------------------------------------------------
# Level fader: 124 x 18 art px (248 x 36 design): 15 discrete pixel cells in a dark well
# --------------------------------------------------------------------------------------------
FADER_W, FADER_H = 124, 18
FADER_CELLS = 15
_CELL_W, _CELL_PITCH, _CELL_X0 = 7, 8, 3     # cells at art x 3 + 8k .. +6 (k = 0..14): x 3..121
_CELL_Y0, _CELL_H = 5, 8
# Value travel (design units, relative to the fader): the left edge of the first cell to the
# right edge of the last. Emitted into ui_layout.inc for the editor's fader mapping.
FADER_TRAVEL_X0 = _CELL_X0 * SCALE
FADER_TRAVEL_LEN = ((FADER_CELLS - 1) * _CELL_PITCH + _CELL_W) * SCALE


def fader_lit_cells(t, bipolar):
    """The cells lit at normalised value t: unipolar 0..15 cells (value k of 0..15 lights k cells)."""
    if not bipolar:
        return set(range(int(math.floor(t * FADER_CELLS + 0.5))))
    pos = t * FADER_CELLS        # 0..15, centre 7.5
    lo, hi = sorted((7.5, pos))
    return {k for k in range(FADER_CELLS) if k + 0.5 > lo and k + 0.5 < hi + 0.001 and abs(pos - 7.5) > 0.25}


def _fader_static(P, bipolar):
    img = Pix(FADER_W, FADER_H)
    img.rect(0, 2, FADER_W, FADER_H - 4, P.shadow)               # the well
    img.rect(1, 3, FADER_W - 2, FADER_H - 6, P.overlay)
    img.rect(1, FADER_H - 3, FADER_W - 2, 1, P.plate_hi)        # lip catching the light
    for k in range(FADER_CELLS):
        img.rect(_CELL_X0 + k * _CELL_PITCH, _CELL_Y0, _CELL_W, _CELL_H, P.track)
    if bipolar:
        mid = _CELL_X0 + 7 * _CELL_PITCH + 3
        img.rect(mid, 0, 1, 2, P.text_dim)
        img.rect(mid, FADER_H - 2, 1, 2, P.text_dim)
    return img


def render_fader(P, frames, bipolar, accent):
    static = _fader_static(P, bipolar)
    hi = lighter(accent, 0.35)
    out = []
    for i in range(frames):
        t = i / (frames - 1.0)
        img = static.copy()
        for k in sorted(fader_lit_cells(t, bipolar)):
            x = _CELL_X0 + k * _CELL_PITCH
            img.rect(x, _CELL_Y0, _CELL_W, _CELL_H, accent)
            img.rect(x, _CELL_Y0, _CELL_W, 1, hi)
        out.append(img.scaled())
    return out


# --------------------------------------------------------------------------------------------
# Toggle: 32 x 16 art px (64 x 32 design) rubber pill; shell thumb; page-colour LED when on
# --------------------------------------------------------------------------------------------
def _pill_inside(x, y, w, h):
    r = (h - 1) / 2.0
    cy = (h - 1) / 2.0
    if x < r:
        return math.hypot(x - r, y - cy) <= r + 0.3
    if x > w - 1 - r:
        return math.hypot(x - (w - 1 - r), y - cy) <= r + 0.3
    return 0 <= y < h


def render_toggle(P, on, accent=None):
    w, h = 32, 16
    img = Pix(w, h)
    for y in range(h):
        for x in range(w):
            if _pill_inside(x, y, w, h):
                inner = _pill_inside(x - 1, y - 1, w - 2, h - 2) if 0 < x < w - 1 and 0 < y < h - 1 else False
                if not inner:
                    img.set(x, y, P.shadow)
                elif y <= 2:
                    img.set(x, y, P.rubber_hi)
                elif y >= h - 3:
                    img.set(x, y, P.rubber_lo)
                else:
                    img.set(x, y, P.rubber)
    # LED on the free side of the track
    lx = 6 if on else w - 10
    led = accent if on else P.led_off
    img.rect(lx, 6, 4, 4, P.shadow)
    img.rect(lx + 1, 6, 2, 4, led)
    img.rect(lx, 7, 4, 2, led)
    if on:
        img.set(lx + 1, 7, lighter(accent, 0.55))
    # thumb
    r = 5.2
    cx = w - 1 - 7.5 if on else 7.5
    cy = 7.5
    ramp = [P.shell_dark, P.shell, P.shell_hi]
    for y in range(h):
        for x in range(w):
            d = math.hypot(x - cx, y - cy)
            if d <= r:
                l = (-(x - cx) * 0.6 - (y - cy) * 0.8) / r
                img.set(x, y, tone(x, y, 1.0 + 1.1 * l, ramp))
            elif d <= r + 1.0:
                img.set(x, y, P.shadow)
    return img.scaled()


# --------------------------------------------------------------------------------------------
# Segments: w x 20 art px. Both states are dark rubber (the one caption colour reads on both);
# selected = lighter rubber, a page-colour bar along the bottom and a lit corner LED.
# --------------------------------------------------------------------------------------------
SEG_BAR_ROWS = 4


def render_segment(P, w_design, on, accent=None):
    w, h = w_design // 2, 20
    img = Pix(w, h)
    body = P.rubber_sel if on else P.rubber
    for y in range(h):
        for x in range(w):
            if (x in (0, w - 1)) and (y in (0, h - 1)):
                continue
            if x == 0 or y == 0 or x == w - 1 or y == h - 1:
                img.set(x, y, P.shadow)
            elif y == 1:
                img.set(x, y, mixc(body, P.text, 0.18))
            elif y == h - 2:
                img.set(x, y, mixc(body, P.shadow, 0.4))
            else:
                img.set(x, y, body)
    if on:
        hi = lighter(accent, 0.4)
        img.rect(1, h - 1 - SEG_BAR_ROWS, w - 2, SEG_BAR_ROWS, accent)  # rows 15..18
        img.rect(1, h - 1 - SEG_BAR_ROWS, w - 2, 1, hi)
        img.rect(2, 2, 3, 3, accent)                                     # corner LED
        img.set(2, 2, hi)
    else:
        img.rect(2, 2, 3, 3, P.led_off)
    return img.scaled()


# --------------------------------------------------------------------------------------------
# Page backgrounds (composed at design resolution; every shape is in 2-unit "art pixels")
# --------------------------------------------------------------------------------------------
BG_W, BG_H = 1280, 628
WORDMARK_RIGHT = 1008        # design; left of the Q-Link sidebar (x >= 1025)
TITLE_X, TITLE_Y, TITLE_SCALE = 28, 6, 4
LEGEND_SCALE = 2             # section legends: one font pixel = one art pixel
BAND_RULE_GAP = 20           # top band: title end to the rule
BAND_TEXT_Y = 15             # top band: legends' top edge


def draw_plate(img, P, x, y, w, h, stripe=None):
    """A raised slot plate: plate fill, 1 art-px highlight (top/left) and shadow (bottom/right)."""
    img.rect(x, y, w, h, P.plate)
    img.rect(x, y, w, 2, P.plate_hi)
    img.rect(x, y, 2, h, P.plate_hi)
    img.rect(x, y + h - 2, w, 2, P.shadow)
    img.rect(x + w - 2, y, 2, h, P.shadow)
    if stripe is not None:
        img.rect(x + 2, y, w - 4, 4, stripe)


def draw_recess(img, P, x, y, w, h, text=""):
    """A blank slot: a flat recess (shadow top/left, highlight bottom/right) with an optional decal."""
    img.rect(x, y, w, h, P.recess)
    img.rect(x, y, w, 2, P.shadow)
    img.rect(x, y, 2, h, P.shadow)
    img.rect(x, y + h - 2, w, 2, P.bezel)
    img.rect(x + w - 2, y, 2, h, P.bezel)
    if text:
        tw = text_width(text, 2)
        draw_text(img, text, x + (w - tw) // 2, y + (h - FONT_H * 2) // 2, 2, P.decal)


def draw_badge(img, P, text, right, bottom):
    """A Q-Link number badge, right/bottom aligned (design units), in the 3 x 5 micro font."""
    s = 2
    draw_micro(img, text, right - micro_width(text, s), bottom - 5 * s, s, P.text_dim)


def draw_cartridge(img, P, x, y, w, h, accent):
    """The navy "cartridge label" window behind a firmware menu slider's value."""
    img.rect(x, y, w, h, P.shadow)
    img.rect(x + 2, y + 2, w - 4, h - 4, P.cartridge)
    img.rect(x + 2, y + 2, w - 4, 2, accent)
    # a small down-pointing caret at the right: the menu opens on a tap
    cx, cy = x + w - 18, y + h // 2 - 2
    for r in range(3):
        img.rect(cx + r * 2, cy + r * 2, 10 - r * 4, 2, P.text_dim)


def draw_legend(img, P, text, x, y, right, accent):
    """A section legend in the page colour, then a rule to `right`."""
    w = draw_text(img, text, x, y, LEGEND_SCALE, accent)
    rx = x + w + 12
    if right - rx > 8:
        img.rect(rx, y + 6, right - rx, 2, P.plate_hi)


def render_background(P, title, wordmark, accent, band_legends=()):
    """The page field and its top band. band_legends: (text, x) legends printed on the band's rule."""
    img = Pix(BG_W, BG_H, P.bezel)
    # top band: power LED, title, rule (with any row-0 legends on it), wordmark
    img.rect(10, 16, 10, 10, P.shadow)
    img.rect(12, 18, 6, 6, accent)
    img.rect(12, 18, 2, 2, lighter(accent, 0.6))
    draw_text(img, title, TITLE_X + 2, TITLE_Y + 2, TITLE_SCALE, P.shadow)
    tw = draw_text(img, title, TITLE_X, TITLE_Y, TITLE_SCALE, P.text)
    ww = text_width(wordmark, 3) + 6
    wx = WORDMARK_RIGHT - ww
    draw_text(img, wordmark, wx + 2, 12, 3, P.shadow, shear=True)
    draw_text(img, wordmark, wx, 10, 3, P.text_dim, shear=True)
    x0, x1 = TITLE_X + tw + BAND_RULE_GAP, wx - 16
    stops = []
    for text, lx in band_legends:
        lx = max(lx, x0 + 16)
        stops.append((lx, lx + text_width(text, LEGEND_SCALE), text))
    x = x0
    for a, b, text in sorted(stops):
        if a - 12 - x > 4:
            img.rect(x, 18, a - 12 - x, 2, accent)
            img.rect(x, 24, a - 12 - x, 2, P.plate_hi)
        draw_text(img, text, a, BAND_TEXT_Y, LEGEND_SCALE, accent)
        x = b + 12
    if x1 - x > 4:
        img.rect(x, 18, x1 - x, 2, accent)
        img.rect(x, 24, x1 - x, 2, P.plate_hi)
    return img


# --------------------------------------------------------------------------------------------
# LCD panels (negative DMG): a bezel and a dark green field
# --------------------------------------------------------------------------------------------
LCD_INSET = 4 * SCALE  # design px from the LCD panel edge to its field


def render_lcd_panel(P, w_design, h_design):
    """A negative-DMG LCD with a slate bezel, sized to the design bounds (rounded down to art px)."""
    w, h = w_design // 2, h_design // 2
    img = Pix(w, h)
    img.frame(0, 0, w, h, P.shadow)
    img.rect(1, 1, w - 2, 1, P.plate_hi)
    img.rect(1, 1, 1, h - 2, P.plate_hi)
    img.rect(1, h - 2, w - 2, 1, P.plate)
    img.rect(w - 2, 1, 1, h - 2, P.plate)
    img.rect(2, 2, w - 4, h - 4, P.plate)
    img.frame(3, 3, w - 6, h - 6, P.overlay)
    img.rect(4, 4, w - 8, h - 8, P.lcd_field)
    return img.scaled(), LCD_INSET


def render_cursor(P, scale):
    """Row cursor for the table grid: a 5 x 7 font-cell arrow in LCD ink, at the grid's scale."""
    img = Pix(FONT_W, FONT_H)
    rows = ("#....", "##...", "###..", "####.", "###..", "##...", "#....")
    for y, r in enumerate(rows):
        for x, ch in enumerate(r):
            if ch == "#":
                img.set(x, y, P.lcd_ink)
    return img.scaled(scale)


# --------------------------------------------------------------------------------------------
# LCD film strips (design sizes are twice the art sizes). Opaque on the LCD field unless noted.
# --------------------------------------------------------------------------------------------
STRIP_ART = {  # art kind -> (art w, art h, frames)
    "duty": (100, 30, 4),
    "waveShape": (100, 30, 16),
    "envTime": (84, 30, 8),
    "envDir": (14, 30, 2),
    "ledLcd": (30, 10, 2),
    "ledPlate": (10, 8, 2),
}

# The factory waves (src/engine/factory_content.cpp makeWave): 32 samples of 0..15 each.
def factory_wave(which):
    out = []
    organ = [math.sin(2 * math.pi * i / 32.0) + 0.5 * math.sin(4 * math.pi * i / 32.0)
             + 0.33 * math.sin(6 * math.pi * i / 32.0) for i in range(32)]
    lo, hi = min(organ), max(organ)
    for i in range(32):
        if which == 0:
            v = i * 15 // 31
        elif which == 1:
            v = 15 if i < 16 else 0
        elif which == 2:
            v = i if i < 16 else 31 - i
        elif which == 3:
            v = int(math.floor(7.5 + 7.5 * math.sin(2 * math.pi * i / 32.0) + 0.5))
        elif which == 4:
            v = 15 if i < 8 else 0
        elif which == 5:
            v = 15 if i < 4 else 0
        elif which == 6:
            v = int(math.floor(15.0 * math.sin(math.pi * i / 16.0) + 0.5)) if i < 16 else 0
        else:
            v = int(math.floor(15.0 * (organ[i] - lo) / (hi - lo) + 0.5))
        out.append(max(0, min(15, v)))
    return out


def _lcd_frame(P, kind):
    w, h, _ = STRIP_ART[kind]
    return Pix(w, h, P.lcd_field)


def _baseline(img, P, y):
    for x in range(0, img.w, 2):
        img.set(x, y, P.lcd_dim)


def render_duty_strip(P):
    """Duty 12.5 / 25 / 50 / 75 %: two periods of the pulse wave."""
    frames = []
    for duty in (0.125, 0.25, 0.5, 0.75):
        img = _lcd_frame(P, "duty")
        top, bot, period, x0 = 3, 25, 46, 4
        _baseline(img, P, bot + 2)
        x = x0
        for _ in range(2):
            hi_w = max(2, int(round(period * duty)))
            img.rect(x, top, 2, bot - top + 2, P.lcd_ink)            # rising edge
            img.rect(x, top, hi_w + 2, 2, P.lcd_ink)                 # high
            img.rect(x + hi_w, top, 2, bot - top + 2, P.lcd_ink)     # falling edge
            img.rect(x + hi_w, bot, period - hi_w + 2, 2, P.lcd_ink)  # low
            x += period
        frames.append(img.scaled())
    return frames


def render_wave_strip(P):
    """WAV Shape: the eight factory waves as 32-step bars; user slots 09..16 as a label."""
    frames = []
    for k in range(16):
        img = _lcd_frame(P, "waveShape")
        bot = 27
        if k < 8:
            s = factory_wave(k)
            for i, v in enumerate(s):
                x = 2 + i * 3
                hgt = 2 + int(round(v * 1.5))
                img.rect(x, bot - hgt, 3, hgt, P.lcd_beat)
                img.rect(x, bot - hgt, 3, 2, P.lcd_ink)
            _baseline(img, P, bot + 1)
        else:
            _baseline(img, P, bot + 1)
            text = "USER %02d" % (k + 1)
            tw = text_width(text, 1)
            draw_text(img, text, (img.w - tw) // 2, 10, 1, P.lcd_ink)
        frames.append(img.scaled())
    return frames


def render_env_strip(P):
    """Env Time Off, 1..7: the 15-step volume staircase; longer steps for higher times (Off = flat)."""
    frames = []
    for k in range(8):
        img = _lcd_frame(P, "envTime")
        bot = 27
        _baseline(img, P, bot + 1)
        if k == 0:
            img.rect(2, 4, img.w - 4, 2, P.lcd_ink)
            draw_text(img, "OFF", img.w - 22, 10, 1, P.lcd_dim)
        else:
            length = 12 + k * 10                       # art px for the 15 steps
            for x in range(length):
                level = 15 - int(x * 15 / length)
                hgt = max(1, int(round(level * 1.5)))
                img.rect(2 + x, bot - hgt, 1, hgt, P.lcd_beat if x % 2 else P.lcd_ink)
                img.rect(2 + x, bot - hgt, 1, 2, P.lcd_ink)
        frames.append(img.scaled())
    return frames


def render_envdir_strip(P):
    """Env Dir Down / Up: an arrow."""
    frames = []
    for up in (False, True):
        img = _lcd_frame(P, "envDir")
        cx = 7
        img.rect(cx - 1, 6, 2, 18, P.lcd_ink)
        for r in range(5):
            y = (6 + r) if up else (23 - r)
            img.rect(cx - 1 - r, y, 2 + 2 * r, 1, P.lcd_ink)
        frames.append(img.scaled())
    return frames


def render_led_strip(P, accent, with_text):
    """Channel On LED (Off / On). with_text: the LCD variant, LED plus ON/OFF in LCD ink."""
    frames = []
    kind = "ledLcd" if with_text else "ledPlate"
    w, h, _ = STRIP_ART[kind]
    for on in (False, True):
        img = Pix(w, h, P.lcd_field) if with_text else Pix(w, h)
        lx, ly = (1, 1) if with_text else (0, 0)
        lw, lh = (8, 8) if with_text else (10, 8)
        img.rect(lx, ly, lw, lh, P.shadow)
        img.rect(lx + 1, ly + 1, lw - 2, lh - 2, accent if on else P.led_off)
        if on:
            img.rect(lx + 2, ly + 2, 2, 2, lighter(accent, 0.6))
        if with_text:
            draw_text(img, "ON" if on else "OFF", 12, 2, 1, P.lcd_ink if on else P.lcd_dim)
        frames.append(img.scaled())
    return frames


# --------------------------------------------------------------------------------------------
# Channel LCD (row 0 of the channel pages) and the TABLE LCD's printed content
# --------------------------------------------------------------------------------------------
def draw_lcd_caption(img, P, text, x, y):
    draw_text(img, text, x, y, 2, P.lcd_ink)


def draw_noise_motif(img, P, x, y, w, h):
    """A static noise burst: pseudo-random bars (fixed LFSR, so the art is reproducible)."""
    lfsr = 0x7FFF
    bot = y + h - 6
    for i in range(0, w - 4, 4):
        bit = (lfsr ^ (lfsr >> 1)) & 1
        lfsr = (lfsr >> 1) | (bit << 14)
        hgt = 6 + (lfsr % 23) * 2
        img.rect(x + 2 + i, bot - hgt, 2, hgt, P.lcd_ink if (i // 4) % 3 else P.lcd_beat)
    for i in range(0, w, 4):
        img.rect(x + i, bot + 2, 2, 2, P.lcd_dim)


def draw_channel_lcd(img, P, bounds, tag, motif, static_fallbacks):
    """The channel monitor printed on a channel page's row-0 LCD (bounds: the panel, design).

    static_fallbacks: [(Pix, x, y)] film-strip frames baked in when the live strips are not used."""
    panel, _ = render_lcd_panel(P, bounds[2], bounds[3])
    img.blit(panel, bounds[0], bounds[1])
    x0, y0 = bounds[0], bounds[1]
    draw_text(img, tag, x0 + 16, y0 + 14, 4, P.lcd_ink)
    for dx in (104, 568):
        for yy in range(y0 + 14, y0 + bounds[3] - 14, 4):
            img.rect(x0 + dx, yy, 2, 2, P.lcd_dim)
    if motif == "noise":
        draw_lcd_caption(img, P, "NOISE", x0 + 122, y0 + 12)
        draw_noise_motif(img, P, x0 + 122, y0 + 32, 200, 60)
    if motif == "wave":
        draw_lcd_caption(img, P, "NO ENVELOPE", x0 + 346, y0 + 12)
        for i in range(0, 200, 4):
            img.rect(x0 + 346 + i, y0 + 62, 2, 2, P.lcd_dim)
    for frame, fx, fy in static_fallbacks:
        img.blit(frame, fx, fy)


def draw_command_card(img, P, x, y, w, entries):
    """The TABLE LCD's printed command reference: a title, then letter keys in three columns."""
    draw_text(img, "COMMANDS", x, y, 2, P.lcd_ink)
    tw = text_width("COMMANDS", 2)
    for i in range(x + tw + 12, x + w, 4):
        img.rect(i, y + 6, 2, 2, P.lcd_dim)
    cols = 3
    col_w = w // cols
    for i, e in enumerate(entries):
        cx = x + (i % cols) * col_w
        cy = y + 28 + (i // cols) * 36
        letter, word = e.split(" ", 1)
        img.rect(cx, cy - 4, 20, 22, P.lcd_ink)
        draw_text(img, letter, cx + 5, cy, 2, P.lcd_field)
        draw_text(img, word, cx + 30, cy, 2, P.lcd_ink)


# --------------------------------------------------------------------------------------------
# Editor tab bar (Mac only): shell plastic, rubber tabs, magenta active tab, colour underlines
# --------------------------------------------------------------------------------------------
def render_tab_bar(P, width, height, underlines=()):
    """underlines: (x, y, w, colour) design rects under each tab button."""
    w, h = width // 2, height // 2
    img = Pix(w, h)
    ramp = [P.shell_dark, P.shell, P.shell_hi]
    for y in range(h):
        for x in range(w):
            v = 1.0 + (0.6 if y < 3 else 0.0) - (0.6 if y >= h - 3 else 0.0)
            img.set(x, y, tone(x, y, v, ramp))
    img.rect(0, 0, w, 1, P.shadow)
    img.rect(0, 1, w, 1, P.plate)
    img = img.scaled()
    for x, y, uw, c in underlines:
        img.rect(x, y, uw, 4, c)
        img.rect(x, y + 4, uw, 2, darker(c, 0.4))
    return img


def render_tab_button(P, width, height, on):
    w, h = width // 2, height // 2
    img = Pix(w, h)
    body = P.magenta if on else P.rubber
    for y in range(h):
        for x in range(w):
            if not _pill_inside(x, y, w, h):
                continue
            inner = 0 < x < w - 1 and 0 < y < h - 1 and _pill_inside(x - 1, y - 1, w - 2, h - 2)
            if not inner:
                img.set(x, y, P.shadow)
            elif y <= 2:
                img.set(x, y, mixc(body, P.text, 0.25))
            elif y >= h - 3:
                img.set(x, y, mixc(body, P.shadow, 0.35))
            else:
                img.set(x, y, body)
    return img.scaled()
