#!/usr/bin/env python3
"""Draw the WormOS wallpaper as a PNG: wallpaper.py OUT [W H].

Layered translucent ribbons on a deep violet gradient. They pinch together
towards a bright throat on the right, the profile of a wormhole seen from
the side. Needs pycairo.
"""

import math
import random
import sys

import cairo


def rgb(hexcolour, alpha=1.0):
    v = int(hexcolour, 16)
    return ((v >> 16) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255, alpha)


TX, TY = 0.70, 0.55   # the throat, as a fraction of the size


def background(cr, w, h):
    g = cairo.LinearGradient(0, 0, w, h)
    g.add_color_stop_rgba(0, *rgb("1a1433"))
    g.add_color_stop_rgba(0.5, *rgb("100c1e"))
    g.add_color_stop_rgba(1, *rgb("0a0812"))
    cr.set_source(g)
    cr.paint()
    for x, y, r, c, a in ((TX, TY, 0.95, "6d28d9", 0.22),
                          (0.05, 0.0, 0.8, "3b2a8f", 0.18),
                          (0.95, 1.05, 0.7, "a21caf", 0.14)):
        glow = cairo.RadialGradient(w * x, h * y, 0, w * x, h * y, h * r)
        glow.add_color_stop_rgba(0, *rgb(c, a))
        glow.add_color_stop_rgba(1, *rgb(c, 0))
        cr.set_source(glow)
        cr.paint()


def edge(w, h, k, side, n=400):
    """One ribbon edge: a funnel profile pinched at the throat."""
    tx, ty = w * TX, h * TY
    pts = []
    for i in range(n + 1):
        x = -0.1 * w + 1.2 * w * i / n
        d = (x - tx) / w
        open_ = (abs(d) ** 1.45) * (1.9 if d < 0 else 3.2)
        # The two halves differ a little; ribbons sway as they open.
        lift = 0.85 if side < 0 else 1.15
        sway = 0.10 * d * d * math.sin(3.0 * d + k * 2.0)
        y = ty + h * (side * (0.004 + open_ * k * lift) + sway - 0.08 * d)
        pts.append((x, y))
    return pts


def path(cr, pts):
    cr.move_to(*pts[0])
    for p in pts[1:]:
        cr.line_to(*p)


def ribbons(cr, w, h):
    layers = [  # inner, outer edge factor, colour, strength
        (0.26, 0.70, "8b5cf6", 1.0),
        (0.46, 1.05, "d946ef", 0.6),
        (0.72, 1.45, "6366f1", 0.75),
        (1.10, 2.10, "a855f7", 0.45),
    ]
    slices = 48
    for side in (-1, 1):
        for inner_k, outer_k, c, s in layers:
            # Slices across the ribbon, brightest along the inner edge and
            # fading out, so it has one sharp edge and one soft one.
            ks = [inner_k + (outer_k - inner_k) * (j / slices) ** 1.3
                  for j in range(slices + 1)]
            edges = [edge(w, h, k, side) for k in ks]
            for j in range(slices):
                fade = math.exp(-3.0 * j / slices)
                path(cr, edges[j])
                for p in reversed(edges[j + 1]):
                    cr.line_to(*p)
                cr.close_path()
                cr.set_source(glow_along(w, c, 0.55 * s * fade))
                cr.fill()
            path(cr, edges[0])
            cr.set_source(glow_along(w, "f5ebff", 0.65 * s))
            cr.set_line_width(max(1.0, h / 1200))
            cr.stroke()


def glow_along(w, c, a):
    """A horizontal gradient that peaks at the throat."""
    g = cairo.LinearGradient(0, 0, w, 0)
    g.add_color_stop_rgba(0, *rgb(c, 0))
    g.add_color_stop_rgba(0.30, *rgb(c, 0.10 * a))
    g.add_color_stop_rgba(TX, *rgb(c, a))
    g.add_color_stop_rgba(0.92, *rgb(c, 0.25 * a))
    g.add_color_stop_rgba(1, *rgb(c, 0.08 * a))
    return g


def throat(cr, w, h):
    tx, ty = w * TX, h * TY
    cr.save()
    cr.translate(tx, ty)
    cr.scale(1.0, 0.45)
    g = cairo.RadialGradient(0, 0, 0, 0, 0, h * 0.10)
    g.add_color_stop_rgba(0, *rgb("ffffff", 0.75))
    g.add_color_stop_rgba(0.2, *rgb("f0abfc", 0.35))
    g.add_color_stop_rgba(1, *rgb("a855f7", 0))
    cr.set_source(g)
    cr.paint()
    cr.restore()


def grain(cr, w, h):
    """Fine noise against banding in the dark gradients."""
    size = 256
    tile = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
    data = tile.get_data()
    rnd = random.Random(7)
    for i in range(size * size):
        v = rnd.randrange(256)
        a = 6
        p = v * a // 255
        data[4 * i:4 * i + 4] = bytes((p, p, p, a))
    tile.mark_dirty()
    pattern = cairo.SurfacePattern(tile)
    pattern.set_extend(cairo.EXTEND_REPEAT)
    cr.set_source(pattern)
    cr.set_operator(cairo.OPERATOR_OVER)
    cr.paint()


def main():
    out = sys.argv[1]
    w, h = (int(v) for v in sys.argv[2:4]) if len(sys.argv) > 3 else (2560, 1440)
    surface = cairo.ImageSurface(cairo.FORMAT_RGB24, w, h)
    cr = cairo.Context(surface)
    background(cr, w, h)
    cr.set_operator(cairo.OPERATOR_ADD)
    ribbons(cr, w, h)
    throat(cr, w, h)
    cr.set_operator(cairo.OPERATOR_OVER)
    grain(cr, w, h)
    surface.write_to_png(out)


if __name__ == "__main__":
    main()
