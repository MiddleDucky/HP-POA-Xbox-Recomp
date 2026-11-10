#!/usr/bin/env python3
"""The port's own icon, drawn from simple shapes (no game artwork): a wand with a gold sparkle on a tile in the
launcher's colours (launcher.cpp, DrawBackground), one palette a game.

    python tools/make_icon.py cos|ps|poa res/app.ico [preview.png]

Original artwork made for these repositories: the exes' icon (CMakeLists.txt embeds res/app.ico). Standard library
only: shapes are rasterised 4x supersampled, then box filtered to 16, 24, 32, 48, 64, 128 and 256 px PNG entries.
"""
import math
import struct
import sys
import zlib

SS = 4  # supersampling

# the launchers' backdrops, top and bottom (IM_COL32 in DrawBackground), and the title's gold (222, 186, 92)
PALETTES = {
    "cos": ((40, 96, 62), (16, 44, 28)),   # green
    "ps": ((124, 10, 2), (48, 4, 4)),      # Gryffindor scarlet
    "poa": ((84, 44, 128), (30, 14, 52)),  # purple
}
GOLD = (222 / 255, 186 / 255, 92 / 255, 1.0)
GOLD_LIGHT = (0.98, 0.90, 0.66, 1.0)
TOP = BOTTOM = None  # set from the palette in main()


def inside_round_rect(x, y, x0, y0, x1, y1, r):
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def inside_poly(x, y, pts):
    c = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


def star(cx, cy, r_out, r_in, n=4, rot=0.0):
    pts = []
    for k in range(2 * n):
        r = r_out if k % 2 == 0 else r_in
        a = rot + math.pi * k / n
        pts.append((cx + r * math.sin(a), cy - r * math.cos(a)))
    return pts


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(4))


def over(dst, src):
    a = src[3]
    return (src[0] * a + dst[0] * (1 - a), src[1] * a + dst[1] * (1 - a), src[2] * a + dst[2] * (1 - a),
            a + dst[3] * (1 - a))


def render(n):
    """the icon at n x n, RGBA floats 0..1, in a unit square scaled to n"""
    N = n * SS
    s = N / 256.0  # shapes are laid out on a 256 grid
    # the wand: from bottom left to top right, a rod with a lighter handle end
    ax, ay, bx, by = 62 * s, 200 * s, 176 * s, 86 * s
    dx, dy = bx - ax, by - ay
    length = math.hypot(dx, dy)
    ux, uy = dx / length, dy / length
    half = 9 * s
    big = star(186 * s, 74 * s, 54 * s, 13 * s, 4, 0.0)
    small1 = star(116 * s, 64 * s, 18 * s, 5 * s, 4, math.pi / 4)
    small2 = star(206 * s, 150 * s, 14 * s, 4 * s, 4, 0.0)
    glow_c, glow_r = (186 * s, 74 * s), 70 * s
    out = []
    for py in range(N):
        row = []
        for px in range(N):
            x, y = px + 0.5, py + 0.5
            c = (0.0, 0.0, 0.0, 0.0)
            if inside_round_rect(x, y, 8 * s, 8 * s, 248 * s, 248 * s, 52 * s):
                t = y / N
                c = mix(TOP, BOTTOM, t)
                # a soft glow behind the sparkle
                d = math.hypot(x - glow_c[0], y - glow_c[1]) / glow_r
                if d < 1:
                    c = over(c, (GOLD[0], GOLD[1], GOLD[2], 0.35 * (1 - d) ** 2))
            # the wand's rod: distance from the segment
            t = ((x - ax) * ux + (y - ay) * uy)
            if 0 <= t <= length:
                dist = abs((x - ax) * uy - (y - ay) * ux)
                taper = half * (1.0 - 0.35 * t / length)
                if taper < dist <= taper + 3 * s:  # a pale rim, so the wood stands out on the red tile too
                    c = over(c, (GOLD_LIGHT[0], GOLD_LIGHT[1], GOLD_LIGHT[2], 0.55))
                if dist <= taper:
                    wood = (0.42, 0.25, 0.13, 1.0) if t > length * 0.32 else (0.27, 0.15, 0.08, 1.0)
                    shade = 0.75 + 0.25 * (1 - dist / taper)
                    c = over(c, (wood[0] * shade, wood[1] * shade, wood[2] * shade, 1.0))
            for pts, col in ((big, GOLD), (small1, GOLD_LIGHT), (small2, GOLD_LIGHT)):
                if inside_poly(x, y, pts):
                    c = over(c, col)
            row.append(c)
        out.append(row)
    # box filter SS x SS
    px = []
    for y in range(n):
        line = []
        for x in range(n):
            acc = [0.0, 0.0, 0.0, 0.0]
            for yy in range(SS):
                for xx in range(SS):
                    p = out[y * SS + yy][x * SS + xx]
                    acc[0] += p[0] * p[3]; acc[1] += p[1] * p[3]; acc[2] += p[2] * p[3]; acc[3] += p[3]
            a = acc[3]
            if a:
                line.append((round(255 * acc[0] / a), round(255 * acc[1] / a), round(255 * acc[2] / a),
                             round(255 * a / (SS * SS))))
            else:
                line.append((0, 0, 0, 0))
        px.append(line)
    return px


def png(px, n):
    rows = b"".join(b"\0" + bytes(c for p in row for c in p) for row in px)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    global TOP, BOTTOM
    if len(sys.argv) < 3 or sys.argv[1] not in PALETTES:
        sys.exit(__doc__)
    top, bottom = PALETTES[sys.argv.pop(1)]
    TOP = tuple(v / 255 for v in top) + (1.0,)
    BOTTOM = tuple(v / 255 for v in bottom) + (1.0,)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [png(render(n), n) for n in sizes]
    head = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    entries = b""
    for n, im in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(im), off)
        off += len(im)
    with open(sys.argv[1], "wb") as f:
        f.write(head + entries + b"".join(images))
    if len(sys.argv) > 2:
        with open(sys.argv[2], "wb") as f:
            f.write(images[-1])
    print(f"{sys.argv[1]}: sizes {sizes}")


if __name__ == "__main__":
    main()
