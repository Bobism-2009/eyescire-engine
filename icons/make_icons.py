"""Editor chrome icons.

Drawn on a 24-unit grid at 1024px and downscaled, so strokes stay even and
corners stay round at toolbar size. Glyphs are white with straight alpha;
the editor tints them.
"""

import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

N = 1024
OUT = 128
G = 24.0
HERE = os.path.dirname(os.path.abspath(__file__))
STROKE = 1.9


def px(v):
    return v * N / G


class Icon:
    def __init__(self):
        self.alpha = Image.new("L", (N, N), 0)

    def _mask(self):
        im = Image.new("L", (N, N), 0)
        return im, ImageDraw.Draw(im)

    def stamp(self, mask, alpha=255):
        if alpha < 255:
            mask = mask.point(lambda p, a=alpha: p * a // 255)
        self.alpha = ImageChops.lighter(self.alpha, mask)

    def erase(self, mask):
        self.alpha = ImageChops.multiply(self.alpha, ImageChops.invert(mask))

    def fill_ellipse(self, box, alpha=255):
        m, d = self._mask()
        d.ellipse(box, fill=255)
        self.stamp(m, alpha)

    def fill_rrect(self, box, radius, alpha=255):
        m, d = self._mask()
        d.rounded_rectangle(box, radius=radius, fill=255)
        self.stamp(m, alpha)

    def fill_poly(self, pts, alpha=255):
        m, d = self._mask()
        d.polygon(pts, fill=255)
        self.stamp(m, alpha)

    def punch_ellipse(self, box):
        m, d = self._mask()
        d.ellipse(box, fill=255)
        self.erase(m)

    def punch_rrect(self, box, radius):
        m, d = self._mask()
        d.rounded_rectangle(box, radius=radius, fill=255)
        self.erase(m)

    def capsule(self, x0, y0, x1, y1, radius, alpha=255):
        m, d = self._mask()
        width = max(1, int(round(radius * 2)))
        d.line([(x0, y0), (x1, y1)], fill=255, width=width)
        d.ellipse((x0 - radius, y0 - radius, x0 + radius, y0 + radius), fill=255)
        d.ellipse((x1 - radius, y1 - radius, x1 + radius, y1 + radius), fill=255)
        self.stamp(m, alpha)

    def stroke(self, pts, width, alpha=255):
        radius = width / 2.0
        i = 0
        while i < len(pts) - 1:
            self.capsule(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], radius, alpha)
            i = i + 1

    def gstroke(self, pts, width=STROKE, alpha=255):
        self.stroke([(px(x), px(y)) for x, y in pts], px(width), alpha)

    def arc(self, cx, cy, radius, a0, a1, width, alpha=255):
        """Clockwise from a0 to a1. Angles are Pillow degrees: 0 is east."""
        m, d = self._mask()
        outer = radius + width / 2.0
        d.pieslice((cx - outer, cy - outer, cx + outer, cy + outer), a0, a1, fill=255)
        inner = radius - width / 2.0
        if inner > 1:
            d.ellipse((cx - inner, cy - inner, cx + inner, cy + inner), fill=0)
        half = width / 2.0
        for ang in (a0, a1):
            rad = math.radians(ang)
            x = cx + radius * math.cos(rad)
            y = cy + radius * math.sin(rad)
            d.ellipse((x - half, y - half, x + half, y + half), fill=255)
        self.stamp(m, alpha)

    def garc(self, cx, cy, radius, a0, a1, width=STROKE, alpha=255):
        self.arc(px(cx), px(cy), px(radius), a0, a1, px(width), alpha)

    def earc(self, box, a0, a1, width, alpha=255):
        """Elliptical clockwise arc. box is the outer bounds of the stroke."""
        x0, y0, x1, y1 = box
        m, d = self._mask()
        d.pieslice((x0, y0, x1, y1), a0, a1, fill=255)
        d.ellipse((x0 + width, y0 + width, x1 - width, y1 - width), fill=0)
        cx = (x0 + x1) / 2.0
        cy = (y0 + y1) / 2.0
        rx = (x1 - x0) / 2.0 - width / 2.0
        ry = (y1 - y0) / 2.0 - width / 2.0
        half = width / 2.0
        for ang in (a0, a1):
            rad = math.radians(ang)
            x = cx + rx * math.cos(rad)
            y = cy + ry * math.sin(rad)
            d.ellipse((x - half, y - half, x + half, y + half), fill=255)
        self.stamp(m, alpha)

    def gearc(self, box, a0, a1, width=STROKE, alpha=255):
        self.earc(tuple(px(v) for v in box), a0, a1, px(width), alpha)

    def head(self, tip, direction, length, spread, alpha=255):
        dx, dy = direction
        norm = math.hypot(dx, dy) or 1.0
        dx, dy = dx / norm, dy / norm
        px_, py_ = -dy, dx
        base_x = tip[0] - dx * length
        base_y = tip[1] - dy * length
        left = (base_x + px_ * spread, base_y + py_ * spread)
        right = (base_x - px_ * spread, base_y - py_ * spread)
        self.fill_poly([tip, left, right], alpha)

    def arc_head(self, cx, cy, radius, ang, forward, length, spread, alpha=255):
        """Triangle whose base sits on the arc at ang. forward follows the clockwise tangent."""
        rad = math.radians(ang)
        x = cx + radius * math.cos(rad)
        y = cy + radius * math.sin(rad)
        tx, ty = -math.sin(rad), math.cos(rad)
        if not forward:
            tx, ty = -tx, -ty
        tip = (x + tx * length, y + ty * length)
        px_, py_ = -ty, tx
        left = (x + px_ * spread, y + py_ * spread)
        right = (x - px_ * spread, y - py_ * spread)
        self.fill_poly([tip, left, right], alpha)

    def ring(self, box, width, alpha=255):
        self.fill_ellipse(box, alpha)
        x0, y0, x1, y1 = box
        self.punch_ellipse((x0 + width, y0 + width, x1 - width, y1 - width))

    def gring(self, box, width=STROKE, alpha=255):
        self.ring(tuple(px(v) for v in box), px(width), alpha)

    def frame(self, box, radius, width, alpha=255):
        self.fill_rrect(box, radius, alpha)
        x0, y0, x1, y1 = box
        inner_r = max(0, radius - width)
        self.punch_rrect((x0 + width, y0 + width, x1 - width, y1 - width), inner_r)

    def gframe(self, box, radius, width=STROKE, alpha=255):
        self.frame(tuple(px(v) for v in box), px(radius), px(width), alpha)

    def gfill_rrect(self, box, radius, alpha=255):
        self.fill_rrect(tuple(px(v) for v in box), px(radius), alpha)

    def gfill_ellipse(self, box, alpha=255):
        self.fill_ellipse(tuple(px(v) for v in box), alpha)

    def gpunch_ellipse(self, box):
        self.punch_ellipse(tuple(px(v) for v in box))

    def save(self, name):
        rgb = Image.new("RGB", (N, N), (255, 255, 255))
        im = Image.merge("RGBA", (*rgb.split(), self.alpha))
        im.resize((OUT, OUT), Image.Resampling.LANCZOS).save(os.path.join(HERE, name + ".png"))
        return im


def cube_icon(ox=0.0, oy=0.0):
    ic = Icon()
    a = (12 + ox, 3.6 + oy)
    b = (19.6 + ox, 7.6 + oy)
    c = (12 + ox, 11.6 + oy)
    d = (4.4 + ox, 7.6 + oy)
    e = (12 + ox, 19.6 + oy)
    f = (4.4 + ox, 15.6 + oy)
    g = (19.6 + ox, 15.6 + oy)

    def P(*pts):
        return [(px(x), px(y)) for x, y in pts]

    ic.fill_poly(P(d, c, e, f), 205)
    ic.fill_poly(P(b, g, e, c), 140)
    ic.fill_poly(P(a, b, c, d), 255)
    gap = px(0.55)
    for p, q in ((b, c), (d, c), (c, e)):
        m, dr = ic._mask()
        rad = gap
        dr.line([(px(p[0]), px(p[1])), (px(q[0]), px(q[1]))], fill=255, width=int(round(rad * 2)))
        dr.ellipse((px(p[0]) - rad, px(p[1]) - rad, px(p[0]) + rad, px(p[1]) + rad), fill=255)
        dr.ellipse((px(q[0]) - rad, px(q[1]) - rad, px(q[0]) + rad, px(q[1]) + rad), fill=255)
        ic.erase(m)
    return ic


def folder_icon(slash):
    ic = Icon()
    ic.gfill_rrect((3.4, 8.6, 20.6, 20.2), 1.7)
    ic.gfill_rrect((3.4, 5.2, 11.4, 10.4), 1.3)
    if slash:
        m, d = ic._mask()
        x0, y0 = px(6.2), px(17.4)
        x1, y1 = px(17.8), px(7.2)
        rad = px(0.85)
        width = max(1, int(round(rad * 2)))
        d.line([(x0, y0), (x1, y1)], fill=255, width=width)
        d.ellipse((x0 - rad, y0 - rad, x0 + rad, y0 + rad), fill=255)
        d.ellipse((x1 - rad, y1 - rad, x1 + rad, y1 + rad), fill=255)
        ic.erase(m)
    return ic


def almond(cx, cy, hw, hh, power, n):
    pts = []
    i = 0
    while i <= n:
        t = -1.0 + 2.0 * i / n
        curve = (1.0 - t * t) ** power
        pts.append((cx + t * hw, cy - hh * curve))
        i += 1
    i = n
    while i >= 0:
        t = -1.0 + 2.0 * i / n
        curve = (1.0 - t * t) ** power
        pts.append((cx + t * hw, cy + hh * curve * 0.92))
        i -= 1
    return pts


def _lerp(a, b, t):
    return int(a + (b - a) * t)


def _color_at(stops, t):
    if t <= stops[0][0]:
        return stops[0][1]
    i = 0
    while i < len(stops) - 1:
        t0, c0 = stops[i]
        t1, c1 = stops[i + 1]
        if t <= t1:
            u = 0 if t1 == t0 else (t - t0) / (t1 - t0)
            return tuple(_lerp(c0[k], c1[k], u) for k in range(4))
        i += 1
    return stops[-1][1]


def _radial(size, cx, cy, radius, stops):
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    steps = 96
    i = steps
    while i >= 0:
        t = i / steps
        col = _color_at(stops, t)
        r = max(radius * t, 0.6)
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=col)
        i -= 1
    return layer


def _clip(im, mask):
    out = im.copy()
    out.putalpha(ImageChops.multiply(out.getchannel("A"), mask))
    return out


def eyescire_mark():
    """App mark: a lit accent tile and a glass eye. Drawn at 1024, saved at 512."""
    S = 1024
    tile = Image.new("L", (S, S), 0)
    radius = int(S * 0.225)
    ImageDraw.Draw(tile).rounded_rectangle((1, 1, S - 2, S - 2), radius=radius, fill=255)

    grad = Image.new("RGBA", (S, S))
    pix = grad.load()
    y = 0
    while y < S:
        x = 0
        while x < S:
            t = (y * 0.78 + x * 0.22) / S
            if t > 1:
                t = 1
            pix[x, y] = (
                _lerp(138, 28, t),
                _lerp(190, 86, t),
                _lerp(255, 214, t),
                255,
            )
            x += 1
        y += 1
    base = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    base.paste(grad, mask=tile)

    glow = _radial(S, S * 0.32, S * 0.24, S * 0.72, [
        (0.0, (255, 255, 255, 78)),
        (0.45, (255, 255, 255, 18)),
        (1.0, (255, 255, 255, 0)),
    ])
    base = Image.alpha_composite(base, _clip(glow, tile))

    rim = Image.new("L", (S, S), 0)
    ImageDraw.Draw(rim).rounded_rectangle((8, 8, S - 9, S - 9), radius=radius - 6, fill=255)
    rim = ImageChops.subtract(tile, rim)
    rim_img = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    rim_a = Image.new("L", (S, S), 0)
    rp = rim_a.load()
    y = 0
    while y < S:
        fade = 1.0 - (y / S)
        if fade < 0:
            fade = 0
        x = 0
        while x < S:
            rp[x, y] = int(rim.getpixel((x, y)) * fade * 0.55)
            x += 1
        y += 1
    rim_img.putalpha(rim_a)
    base = Image.alpha_composite(base, rim_img)

    cx, cy = S * 0.50, S * 0.505
    hw, hh = S * 0.335, S * 0.205
    shape = almond(cx, cy, hw, hh, 0.62, 180)
    eye_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(eye_mask).polygon(shape, fill=255)

    shadow = eye_mask.filter(ImageFilter.GaussianBlur(radius=S * 0.018))
    sh = Image.new("RGBA", (S, S), (8, 24, 64, 0))
    sh.putalpha(shadow.point(lambda p: p * 90 // 255))
    sh = sh.transform(sh.size, Image.Transform.AFFINE, (1, 0, 0, 0, 1, -S * 0.012))
    base = Image.alpha_composite(base, _clip(sh, tile))

    sclera = Image.new("RGBA", (S, S), (255, 255, 255, 255))
    shade = Image.new("L", (S, S), 0)
    sp = shade.load()
    y = 0
    while y < S:
        t = (y - (cy - hh)) / (hh * 2.1)
        if t < 0:
            t = 0
        if t > 1:
            t = 1
        v = int((t ** 1.6) * 150)
        x = 0
        while x < S:
            sp[x, y] = v
            x += 1
        y += 1
    shade = ImageChops.multiply(shade, eye_mask)
    sclera_shade = Image.new("RGBA", (S, S), (150, 176, 214, 0))
    sclera_shade.putalpha(shade)
    eye = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    eye.paste(sclera, mask=eye_mask)
    eye = Image.alpha_composite(eye, sclera_shade)

    lid = Image.new("L", (S, S), 0)
    ImageDraw.Draw(lid).polygon(shape, fill=255)
    lid = lid.crop((0, 0, S, int(cy))).resize((S, S), Image.Resampling.BILINEAR)
    lid = lid.filter(ImageFilter.GaussianBlur(radius=S * 0.02))
    lid = ImageChops.multiply(lid, eye_mask)
    lid_img = Image.new("RGBA", (S, S), (20, 52, 120, 0))
    lid_img.putalpha(lid.point(lambda p: p * 70 // 255))
    eye = Image.alpha_composite(eye, lid_img)

    ir = S * 0.112
    light = (cx - ir * 0.22, cy - ir * 0.28)
    iris = _radial(S, light[0], light[1], ir * 1.15, [
        (0.00, (214, 236, 255, 255)),
        (0.28, (92, 164, 255, 255)),
        (0.62, (36, 104, 230, 255)),
        (0.86, (16, 58, 168, 255)),
        (1.00, (8, 28, 96, 255)),
    ])
    iris_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(iris_mask).ellipse((cx - ir, cy - ir, cx + ir, cy + ir), fill=255)
    iris_mask = ImageChops.multiply(iris_mask, eye_mask)
    iris = _clip(iris, iris_mask)

    pr = ir * 0.40
    pupil = _radial(S, cx, cy + pr * 0.08, pr, [
        (0.00, (6, 10, 18, 255)),
        (0.72, (6, 10, 18, 255)),
        (1.00, (12, 28, 64, 255)),
    ])
    pupil_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(pupil_mask).ellipse((cx - pr, cy - pr, cx + pr, cy + pr), fill=255)
    pupil = _clip(pupil, ImageChops.multiply(pupil_mask, iris_mask))

    spec = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(spec)
    gx, gy = cx - pr * 0.15, cy - pr * 0.85
    sd.ellipse((gx - ir * 0.20, gy - ir * 0.11, gx + ir * 0.20, gy + ir * 0.11), fill=(255, 255, 255, 230))
    spec = spec.filter(ImageFilter.GaussianBlur(radius=S * 0.004))
    dot = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(dot).ellipse(
        (gx - ir * 0.045, gy - ir * 0.07, gx + ir * 0.02, gy + ir * 0.0),
        fill=(255, 255, 255, 255),
    )
    spec = Image.alpha_composite(spec, dot)
    spec = _clip(spec, iris_mask)

    bow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(bow).ellipse(
        (cx - ir * 0.62, cy + ir * 0.18, cx + ir * 0.62, cy + ir * 0.72),
        fill=(255, 255, 255, 70),
    )
    bow = bow.filter(ImageFilter.GaussianBlur(radius=S * 0.006))
    bow = _clip(bow, iris_mask)
    bow = _clip(bow, ImageChops.invert(pupil_mask))

    eye = Image.alpha_composite(eye, iris)
    eye = Image.alpha_composite(eye, pupil)
    eye = Image.alpha_composite(eye, bow)
    eye = Image.alpha_composite(eye, spec)
    eye = _clip(eye, eye_mask)

    out = Image.alpha_composite(base, eye)
    out.resize((512, 512), Image.Resampling.LANCZOS).save(os.path.join(HERE, "eyescire.png"))


def build_all():
    ic = Icon()
    ic.gstroke([(6.2, 6.2), (17.8, 17.8)], 2.15)
    ic.gstroke([(17.8, 6.2), (6.2, 17.8)], 2.15)
    ic.save("close")

    ic = Icon()
    ic.gstroke([(5.6, 9.0), (12, 15.2), (18.4, 9.0)], 2.15)
    ic.save("chevron_down")

    ic = Icon()
    ic.gstroke([(9.0, 5.6), (15.2, 12), (9.0, 18.4)], 2.15)
    ic.save("chevron_right")

    ic = Icon()
    ic.fill_poly([(px(8.2), px(4.6)), (px(19.4), px(12)), (px(8.2), px(19.4))])
    ic.save("play")

    ic = Icon()
    ic.gfill_rrect((6.6, 4.6, 10.2, 19.4), 1.5)
    ic.gfill_rrect((13.8, 4.6, 17.4, 19.4), 1.5)
    ic.save("pause")

    ic = Icon()
    ic.gfill_rrect((5.4, 5.4, 18.6, 18.6), 3.2)
    ic.save("stop")

    ic = Icon()
    ic.gstroke([(12, 4.8), (12, 19.2)], 2.15)
    ic.gstroke([(4.8, 12), (19.2, 12)], 2.15)
    ic.save("plus")

    ic = Icon()
    ic.gstroke([(4.8, 12), (19.2, 12)], 2.15)
    ic.save("minus")

    ic = Icon()
    ic.gfill_rrect((4.6, 3.6, 19.4, 20.4), 2.0)
    ic.punch_rrect((px(7.4), px(5.4), px(16.6), px(10.2)), px(0.8))
    ic.punch_rrect((px(7.0), px(13.6), px(17.0), px(17.8)), px(0.9))
    ic.gfill_rrect((9.2, 6.2, 13.2, 9.2), 0.45)
    ic.save("save")

    ic = Icon()
    ic.capsule(px(15.8), px(19.2), px(9.4), px(10.6), px(0.85))
    head, hd = ic._mask()
    hd.rounded_rectangle((px(4.6), px(8.2), px(15.6), px(12.4)), radius=px(1.15), fill=255)
    head = head.rotate(40, resample=Image.Resampling.BICUBIC, center=(px(10.1), px(10.3)))
    ic.stamp(head)
    ic.save("build")

    ic = Icon()
    ic.gframe((4.6, 4.6, 19.4, 19.4), 2.6, 1.75)
    ic.gstroke([(12, 8.6), (12, 15.4)], 1.6)
    ic.gstroke([(8.6, 12), (15.4, 12)], 1.6)
    ic.save("add_rect")

    ic = Icon()
    ic.gstroke([(4.4, 7.2), (14.2, 7.2)], 1.7)
    ic.gstroke([(4.4, 11.2), (12.2, 11.2)], 1.7)
    ic.gstroke([(4.4, 15.2), (10.2, 15.2)], 1.7)
    ic.gstroke([(16.2, 14.6), (16.2, 19.6)], 1.45)
    ic.gstroke([(13.7, 17.1), (18.7, 17.1)], 1.45)
    ic.save("add_label")

    ic = Icon()
    ic.gstroke([(5.2, 7.4), (18.2, 7.4)], 1.9)
    ic.gstroke([(5.2, 12), (15.4, 12)], 1.9)
    ic.gstroke([(5.2, 16.6), (12.6, 16.6)], 1.9)
    ic.save("label")

    ic = Icon()
    ic.garc(12, 12.4, 6.5, 128, 400, 1.9)
    ic.arc_head(px(12), px(12.4), px(6.5), 128, False, px(3.3), px(2.15))
    hi = ic.save("undo")
    rgb = hi.convert("RGBA")
    rgb.transpose(Image.Transpose.FLIP_LEFT_RIGHT).resize((OUT, OUT), Image.Resampling.LANCZOS).save(
        os.path.join(HERE, "redo.png")
    )

    ic = Icon()
    ic.gstroke([(8.2, 12), (15.8, 12)], 1.55)
    ic.gstroke([(12, 8.2), (12, 15.8)], 1.55)
    ic.head((px(4.5), px(12)), (-1, 0), px(3.5), px(2.15))
    ic.head((px(19.5), px(12)), (1, 0), px(3.5), px(2.15))
    ic.head((px(12), px(4.5)), (0, -1), px(3.5), px(2.15))
    ic.head((px(12), px(19.5)), (0, 1), px(3.5), px(2.15))
    ic.save("move")

    ic = Icon()
    ic.garc(12, 12, 6.7, 40, 300, 1.9)
    ic.arc_head(px(12), px(12), px(6.7), 40, False, px(3.3), px(2.15))
    ic.save("rotate")

    ic = Icon()
    w = 1.9
    ic.gstroke([(6.2, 10.4), (6.2, 6.2), (10.4, 6.2)], w)
    ic.gstroke([(13.6, 6.2), (17.8, 6.2), (17.8, 10.4)], w)
    ic.gstroke([(17.8, 13.6), (17.8, 17.8), (13.6, 17.8)], w)
    ic.gstroke([(10.4, 17.8), (6.2, 17.8), (6.2, 13.6)], w)
    ic.save("scale")

    ic = Icon()
    for x, top in ((7.3, 4.6), (10.15, 3.5), (13.0, 3.9), (15.85, 5.0)):
        ic.capsule(px(x), px(top), px(x), px(13.2), px(1.05))
    ic.gfill_rrect((5.6, 11.4, 18.0, 20.4), 2.5)
    ic.capsule(px(5.7), px(13.6), px(3.2), px(16.8), px(1.05))
    ic.save("pan")

    ic = Icon()
    ic.gfill_rrect((3.8, 8.8, 20.2, 20.0), 1.5)
    board, bd = ic._mask()
    bd.rounded_rectangle((px(3.8), px(4.2), px(20.2), px(10.2)), radius=px(1.3), fill=255)
    stripe, sd = ic._mask()
    rad = px(0.85)
    sd.line([(px(7.2), px(10.0)), (px(11.4), px(4.4))], fill=255, width=int(round(rad * 2)))
    sd.line([(px(12.2), px(10.0)), (px(16.6), px(4.4))], fill=255, width=int(round(rad * 2)))
    board = ImageChops.multiply(board, ImageChops.invert(stripe))
    ic.stamp(board)
    ic.save("scene")

    ic = Icon()
    ic.gframe((5.4, 2.6, 18.6, 21.4), 1.45, 1.55)
    ic.gstroke([(8.0, 8.2), (15.6, 8.2)], 1.45)
    ic.gstroke([(8.0, 11.6), (13.2, 11.6)], 1.45)
    ic.gstroke([(8.0, 15.0), (15.0, 15.0)], 1.45)
    ic.save("script")

    cube_icon().save("cube")

    ic = cube_icon(-2.2, -2.4)
    ic.gring((15.4, 16.6, 19.0, 20.2), 1.15)
    ic.gring((17.8, 18.6, 21.6, 22.4), 1.15)
    ic.save("prefab")

    folder_icon(False).save("project")
    folder_icon(True).save("missing")

    ic = Icon()
    ic.gring((5.2, 5.2, 18.8, 18.8), 1.9)
    ic.save("empty")

    ic = Icon()
    ic.gframe((5.0, 5.0, 19.0, 19.0), 2.4, 1.9)
    ic.save("rect")

    ic = Icon()
    ic.gframe((4.0, 4.2, 20.0, 19.8), 1.7, 1.7)
    ic.gstroke([(4.0, 8.6), (20.0, 8.6)], 1.7)
    ic.save("panel")

    ic = Icon()
    ic.gframe((3.2, 7.4, 20.8, 16.6), 2.3, 1.75)
    ic.save("button")

    ic = Icon()
    ic.gfill_rrect((3.2, 8.6, 20.8, 19.2), 2.0)
    ic.gfill_rrect((7.6, 5.4, 14.6, 10.2), 1.1)
    ic.gpunch_ellipse((8.2, 10.0, 15.8, 17.6))
    ic.gring((9.3, 11.1, 14.7, 16.5), 1.15)
    ic.save("camera")

    ic = Icon()
    ic.gframe((3.6, 3.8, 20.4, 20.2), 1.6, 1.55)
    ic.gfill_ellipse((6.6, 6.4, 9.6, 9.4))
    ic.fill_poly([
        (px(6.2), px(16.8)),
        (px(9.6), px(12.2)),
        (px(12.2), px(14.6)),
        (px(17.6), px(9.4)),
        (px(17.8), px(16.8)),
    ])
    ic.save("sprite")

    ic = Icon()
    ic.gfill_ellipse((8.3, 8.3, 15.7, 15.7))
    for i in range(8):
        ang = math.radians(i * 45)
        x0 = 12 + math.cos(ang) * 5.15
        y0 = 12 + math.sin(ang) * 5.15
        x1 = 12 + math.cos(ang) * 8.7
        y1 = 12 + math.sin(ang) * 8.7
        ic.capsule(px(x0), px(y0), px(x1), px(y1), px(0.72))
    ic.save("light_point")

    ic = Icon()
    ic.gstroke([(12, 3.8), (4.6, 19.2)], 1.8)
    ic.gstroke([(4.6, 19.2), (19.4, 19.2)], 1.8)
    ic.gstroke([(19.4, 19.2), (12, 3.8)], 1.8)
    ic.save("light_spot")

    ic = Icon()
    ic.gring((3.4, 6.6, 12.6, 15.8), 1.7)
    ic.capsule(px(13.4), px(11.2), px(17.2), px(11.2), px(0.85))
    ic.head((px(20.6), px(11.2)), (1, 0), px(3.2), px(2.05))
    ic.save("light_dir")

    ic = Icon()
    ic.gring((4.2, 4.2, 19.8, 19.8), 1.7)
    ic.gring((8.4, 8.4, 15.6, 15.6), 1.7)
    ic.save("light_ambient")

    ic = Icon()
    ic.gfill_ellipse((4.2, 4.2, 19.8, 19.8), 145)
    cap, cd = ic._mask()
    cd.ellipse((px(4.6), px(4.4), px(14.8), px(14.6)), fill=255)
    cut, kd = ic._mask()
    kd.ellipse((px(7.6), px(7.4), px(19.4), px(19.2)), fill=255)
    cap = ImageChops.multiply(cap, ImageChops.invert(cut))
    ic.stamp(cap, 255)
    ic.save("sphere")

    ic = Icon()
    top = (5.6, 3.0, 18.4, 8.8)
    width = 1.6
    ic.gring(top, width)
    half = width / 2.0
    left = top[0] + half
    right = top[2] - half
    equator = (top[1] + top[3]) / 2.0
    bottom_y = 16.4
    ic.capsule(px(left), px(equator - 0.15), px(left), px(bottom_y), px(half))
    ic.capsule(px(right), px(equator - 0.15), px(right), px(bottom_y), px(half))
    tall = top[3] - top[1]
    ic.gearc((top[0], bottom_y - tall / 2.0, top[2], bottom_y + tall / 2.0), 0, 180, width)
    ic.save("cylinder")

    ic = Icon()
    ic.gstroke([(12, 3.6), (20.2, 15.4)], 1.45)
    ic.gstroke([(20.2, 15.4), (3.8, 15.4)], 1.45)
    ic.gstroke([(3.8, 15.4), (12, 3.6)], 1.45)
    ic.gstroke([(12, 3.6), (12, 20.4)], 1.45)
    ic.gstroke([(3.8, 15.4), (12, 20.4)], 1.45)
    ic.gstroke([(20.2, 15.4), (12, 20.4)], 1.45)
    ic.save("model")


if __name__ == "__main__":
    eyescire_mark()
    build_all()
