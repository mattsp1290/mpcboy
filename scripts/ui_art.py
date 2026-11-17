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
# Palette
# --------------------------------------------------------------------------------------------
class Palette:
    def __init__(self, spec):
        c = {k: rgb(v) for k, v in spec.items()}
        self.darkest = c["lcdDarkest"]   # 0f380f
        self.dark = c["lcdDark"]         # 306230
        self.light = c["lcdLight"]       # 8bac0f
        self.lightest = c["lcdLightest"]  # 9bbc0f
        self.panel = c["lcdPanel"]       # LCD panel behind the slot grid, between darkest and dark
        self.shell = c["shellLight"]     # c4c0b8
        self.shell_dark = c["shellDark"]  # 8b8680
        self.slate = c["labelAccent"]    # 5c5a6e
        self.magenta = c["magenta"]      # 8e1c4f
        self.caption = c["caption"]      # e0f8d0
        # Derived shades (dither partners), all computed from the palette above.
        self.shadow = mixc(self.darkest, rgb("000000"), 0.35)
        self.shell_hi = mixc(self.shell, rgb("ffffff"), 0.45)
        self.slate_dark = mixc(self.slate, self.darkest, 0.45)
        self.slate_hi = mixc(self.slate, self.shell_dark, 0.5)
        self.magenta_hi = mixc(self.magenta, rgb("ffffff"), 0.28)
        self.magenta_dark = mixc(self.magenta, rgb("000000"), 0.45)
        self.track = mixc(self.dark, self.light, 0.18)  # unlit knob arc
        self.dot = mixc(self.panel, self.dark, 0.45)
        self.field_dot = mixc(self.darkest, self.dark, 0.35)


# --------------------------------------------------------------------------------------------
# Knob: 45 x 45 art px (90 x 90 design), round grey dial, LCD-green value arc, dark notch
# --------------------------------------------------------------------------------------------
KNOB_ART = 45
_KC = 22  # centre pixel
_SWEEP = 135.0


def _ang(dx, dy):
    """Degrees, 0 at 12 o'clock, clockwise positive."""
    return math.degrees(math.atan2(dx, -dy))


def _knob_static(P):
    img = Pix(KNOB_ART, KNOB_ART)
    ring = []
    body_r, out_r = 14.6, 15.6
    ramp = [P.slate, P.shell_dark, P.shell, P.shell_hi]
    for y in range(KNOB_ART):
        for x in range(KNOB_ART):
            dx, dy = x - _KC, y - _KC
            d = math.hypot(dx, dy)
            a = _ang(dx, dy)
            if 18.5 <= d <= 21.4 and -_SWEEP - 0.01 <= a <= _SWEEP + 0.01:
                img.set(x, y, P.track)
                ring.append((x, y, a, d))
                continue
            sd = math.hypot(dx - 1, dy - 1.5)
            if d <= body_r:
                l = (-0.55 * dx - 0.83 * dy) / body_r  # lit from the top-left
                if d <= 9.6:
                    img.set(x, y, tone(x, y, 2.0 - 0.8 * l, ramp))   # dished face: reversed shading
                elif d <= 12.6:
                    img.set(x, y, tone(x, y, 1.6 + 1.4 * l, ramp))
                else:
                    # knurled rim: crisp alternating teeth every 15 degrees
                    v = int(round(1.3 + 1.2 * l)) - (1 if int((a + 180.0) // 15.0) % 2 else 0)
                    img.set(x, y, ramp[max(0, min(3, v))])
            elif d <= out_r:
                img.set(x, y, P.shadow)
            elif sd <= out_r:
                img.set(x, y, P.shadow)
    return img, ring


def _seg_dist(px, py, x0, y0, x1, y1):
    vx, vy = x1 - x0, y1 - y0
    ll = vx * vx + vy * vy or 1.0
    t = max(0.0, min(1.0, ((px - x0) * vx + (py - y0) * vy) / ll))
    return math.hypot(px - (x0 + vx * t), py - (y0 + vy * t))


def render_knob(P, frames, bipolar):
    static, ring = _knob_static(P)
    out = []
    for i in range(frames):
        t = i / (frames - 1.0)
        a = -_SWEEP + 2.0 * _SWEEP * t
        img = static.copy()
        lo, hi = (min(0.0, a), max(0.0, a)) if bipolar else (-_SWEEP - 1.0, a)
        for x, y, pa, d in ring:
            lit = lo <= pa <= hi and (not bipolar or abs(a) > 0.5)
            if lit:
                img.set(x, y, P.lightest if d <= 20.4 else P.light)
            if abs(pa - a) <= 3.6:
                img.set(x, y, P.caption)
        if bipolar:
            for y in range(0, 4):
                img.set(_KC, y, P.caption)
        # notch pointer
        s, c = math.sin(math.radians(a)), -math.cos(math.radians(a))
        x0, y0 = _KC + s * 3.0, _KC + c * 3.0
        x1, y1 = _KC + s * 12.4, _KC + c * 12.4
        for y in range(_KC - 14, _KC + 15):
            for x in range(_KC - 14, _KC + 15):
                if _seg_dist(x, y, x0, y0, x1, y1) <= 1.0:
                    img.set(x, y, P.darkest)
        out.append(img.scaled())
    return out


# --------------------------------------------------------------------------------------------
# Fader: 124 x 18 art px (248 x 36 design), segmented LCD bar in a slate housing, grey cap
# --------------------------------------------------------------------------------------------
FADER_W, FADER_H = 124, 18
_FX0, _FX1 = 4, 119  # value travel (inclusive pixel columns)


def _fader_static(P, bipolar):
    img = Pix(FADER_W, FADER_H)
    # housing
    img.rect(1, 4, FADER_W - 2, 10, P.slate)
    img.rect(1, 4, FADER_W - 2, 1, P.slate_hi)
    img.rect(1, 13, FADER_W - 2, 1, P.slate_dark)
    img.frame(0, 3, FADER_W, 12, P.shadow)
    img.rect(3, 6, FADER_W - 6, 6, P.shadow)  # the well
    for x in range(_FX0, _FX1 + 1):
        if (x - _FX0) % 4 != 3:
            img.rect(x, 7, 1, 4, P.dark)
    if bipolar:
        mid = (_FX0 + _FX1) // 2
        img.rect(mid, 0, 1, 3, P.light)
        img.rect(mid, 15, 1, 3, P.light)
    return img


def render_fader(P, frames, bipolar):
    static = _fader_static(P, bipolar)
    mid = (_FX0 + _FX1) // 2
    out = []
    for i in range(frames):
        t = i / (frames - 1.0)
        xv = _FX0 + int(round(t * (_FX1 - _FX0)))
        img = static.copy()
        lo, hi = (min(mid, xv), max(mid, xv)) if bipolar else (_FX0, xv)
        for x in range(lo, hi + 1):
            if (x - _FX0) % 4 != 3:
                img.rect(x, 7, 1, 4, P.lightest)
                img.set(x, 7, P.caption)
        # cap: 5 px wide, full height, grey with a centre groove
        cx = xv - 2
        img.rect(cx - 1, 0, 7, FADER_H, P.shadow)
        img.rect(cx, 1, 5, FADER_H - 2, P.shell)
        img.rect(cx, 1, 1, FADER_H - 2, P.shell_hi)
        img.rect(cx + 4, 1, 1, FADER_H - 2, P.shell_dark)
        img.rect(cx, 1, 5, 1, P.shell_hi)
        img.rect(cx, FADER_H - 2, 5, 1, P.shell_dark)
        img.rect(cx + 2, 4, 1, FADER_H - 8, P.magenta)
        out.append(img.scaled())
    return out


# --------------------------------------------------------------------------------------------
# Toggle: 32 x 16 art px (64 x 32 design) pill; magenta track when on
# --------------------------------------------------------------------------------------------
def _pill_inside(x, y, w, h):
    r = (h - 1) / 2.0
    cy = (h - 1) / 2.0
    if x < r:
        return math.hypot(x - r, y - cy) <= r + 0.3
    if x > w - 1 - r:
        return math.hypot(x - (w - 1 - r), y - cy) <= r + 0.3
    return 0 <= y < h


def render_toggle(P, on):
    w, h = 32, 16
    img = Pix(w, h)
    track = [P.magenta_dark, P.magenta, P.magenta_hi] if on else [P.slate_dark, P.slate, P.slate_hi]
    for y in range(h):
        for x in range(w):
            if _pill_inside(x, y, w, h):
                inner = _pill_inside(x - 1, y - 1, w - 2, h - 2) if 0 < x < w - 1 and 0 < y < h - 1 else False
                if not inner:
                    img.set(x, y, P.shadow)
                else:
                    v = 1.0 + (0.8 if y <= 3 else 0.0) - (0.8 if y >= h - 4 else 0.0)
                    img.set(x, y, tone(x, y, v, track))
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
                img.set(x, y, tone(x, y, 1.1 + 1.2 * l, ramp))
            elif d <= r + 1.0:
                img.set(x, y, P.shadow)
    if on:
        # a lit "LED" dash on the free side of the track
        img.rect(6, 7, 4, 2, P.caption)
    else:
        img.rect(w - 10, 7, 4, 2, P.slate_dark)
    return img.scaled()


# --------------------------------------------------------------------------------------------
# Segments: w x 20 art px; off = raised mid-green key, on = pressed dark key with a corner marker
# --------------------------------------------------------------------------------------------
def render_segment(P, w_design, on):
    w, h = w_design // 2, 20
    img = Pix(w, h)
    for y in range(h):
        for x in range(w):
            corner = (x in (0, w - 1)) and (y in (0, h - 1))
            if corner:
                continue
            edge = x == 0 or y == 0 or x == w - 1 or y == h - 1
            if on:
                if edge:
                    img.set(x, y, P.light)
                elif y == 1:
                    img.set(x, y, P.shadow)
                else:
                    img.set(x, y, P.darkest)
            else:
                if edge:
                    img.set(x, y, P.shadow)
                elif y == 1:
                    img.set(x, y, mixc(P.dark, P.light, 0.45))
                elif y >= h - 3:
                    img.set(x, y, tone(x, y, 0.5 if y == h - 3 else 0.0, [mixc(P.dark, P.darkest, 0.55), P.dark]))
                else:
                    img.set(x, y, P.dark)
    if on:
        for (x, y) in ((2, 2), (3, 2), (4, 2), (2, 3), (3, 3), (2, 4)):
            img.set(x, y, P.lightest)
    return img.scaled()


# --------------------------------------------------------------------------------------------
# Page background: 640 x 314 art px (1280 x 628 design)
# --------------------------------------------------------------------------------------------
BG_W, BG_H = 640, 314
# LCD panel behind the slot grid (art px, inclusive start / exclusive end).
# Slots span design x 23..1261, y 45..604; the panel sits 11 px outside left/right/bottom and 5 px above.
PANEL = (6, 20, 636, 308)
WORDMARK_RIGHT = 504  # art px; design 1008, left of the Q-Link sidebar (x >= 1025)


def _bevel_frame(img, x0, y0, x1, y1, P):
    """A 2 px slate bezel around [x0, x1) x [y0, y1), lit top-left."""
    img.frame(x0 - 3, y0 - 3, x1 - x0 + 6, y1 - y0 + 6, P.shadow)
    for k in (1, 2):
        xa, ya, xb, yb = x0 - k, y0 - k, x1 + k - 1, y1 + k - 1
        img.rect(xa, ya, xb - xa + 1, 1, P.slate_hi if k == 2 else P.slate)
        img.rect(xa, ya, 1, yb - ya + 1, P.slate_hi if k == 2 else P.slate)
        img.rect(xa, yb, xb - xa + 1, 1, P.slate_dark)
        img.rect(xb, ya, 1, yb - ya + 1, P.slate_dark)


def render_background(P, title, wordmark):
    img = Pix(BG_W, BG_H, P.darkest)
    # subtle dot grid on the field
    for y in range(1, BG_H, 4):
        for x in range(1, BG_W, 4):
            img.set(x, y, P.field_dot)
    x0, y0, x1, y1 = PANEL
    _bevel_frame(img, x0, y0, x1, y1, P)
    img.rect(x0, y0, x1 - x0, y1 - y0, P.panel)
    # LCD dot matrix inside the panel
    for y in range(y0 + 2, y1, 3):
        for x in range(x0 + 2, x1, 3):
            img.set(x, y, P.dot)
    # power LED + title
    img.rect(8, 6, 3, 3, P.magenta_hi)
    img.set(8, 6, P.caption)
    img.frame(7, 5, 5, 5, P.shadow)
    tx = 16
    draw_text(img, title, tx + 1, 3, 2, P.shadow)
    w = draw_text(img, title, tx, 2, 2, P.lightest)
    # wordmark (slanted), right-aligned left of the Q-Link sidebar
    ww = text_width(wordmark, 1) + 2
    wx = WORDMARK_RIGHT - ww
    draw_text(img, wordmark, wx + 1, 6, 1, P.shadow, shear=True)
    draw_text(img, wordmark, wx, 5, 1, P.light, shear=True)
    # two thin rules between the title and the wordmark
    rx0, rx1 = tx + w + 10, wx - 8
    if rx1 - rx0 > 8:
        img.rect(rx0, 7, rx1 - rx0, 1, P.magenta)
        img.rect(rx0, 10, rx1 - rx0, 1, P.slate)
    return img.scaled()


# --------------------------------------------------------------------------------------------
# TABLE page LCD panel (classic light LCD), the row cursor, the editor tab bar
# --------------------------------------------------------------------------------------------
LCD_INSET = 4 * SCALE  # design px from the LCD panel edge to its text area


def render_lcd_panel(P, w_design, h_design):
    """Light LCD with a slate bezel, sized to the design bounds (rounded down to art px)."""
    w, h = w_design // 2, h_design // 2
    img = Pix(w, h)
    img.frame(0, 0, w, h, P.shadow)
    img.frame(1, 1, w - 2, h - 2, P.slate_hi)
    img.rect(2, h - 2, w - 3, 1, P.slate_dark)
    img.rect(w - 2, 2, 1, h - 3, P.slate_dark)
    img.frame(2, 2, w - 4, h - 4, P.slate)
    img.rect(3, 3, w - 6, h - 6, P.lightest)
    img.frame(3, 3, w - 6, h - 6, P.light)  # LCD edge shade
    return img.scaled(), LCD_INSET


def render_cursor(P, scale):
    """Row cursor for the table grid: a 5 x 7 font-cell arrow, at the grid's character scale."""
    img = Pix(FONT_W, FONT_H)
    rows = ("#....", "##...", "###..", "####.", "###..", "##...", "#....")
    for y, r in enumerate(rows):
        for x, ch in enumerate(r):
            if ch == "#":
                img.set(x, y, P.darkest)
    return img.scaled(scale)


def render_tab_bar(P, width, height):
    w, h = width // 2, height // 2
    img = Pix(w, h)
    ramp = [P.shell_dark, P.shell, P.shell_hi]
    for y in range(h):
        for x in range(w):
            v = 1.0 + (0.6 if y < 3 else 0.0) - (0.6 if y >= h - 3 else 0.0)
            img.set(x, y, tone(x, y, v, ramp))
    img.rect(0, 0, w, 1, P.shadow)
    img.rect(0, 1, w, 1, P.slate)
    return img.scaled()


def render_tab_button(P, width, height, on):
    w, h = width // 2, height // 2
    img = Pix(w, h)
    ramp = [P.magenta_dark, P.magenta, P.magenta_hi] if on else [P.slate_dark, P.slate, P.slate_hi]
    for y in range(h):
        for x in range(w):
            if not _pill_inside(x, y, w, h):
                continue
            inner = 0 < x < w - 1 and 0 < y < h - 1 and _pill_inside(x - 1, y - 1, w - 2, h - 2)
            if not inner:
                img.set(x, y, P.shadow)
            else:
                v = 1.0 + (0.9 if y <= 3 else 0.0) - (0.9 if y >= h - 4 else 0.0)
                img.set(x, y, tone(x, y, v, ramp))
    return img.scaled()
