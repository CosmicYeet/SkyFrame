"""Draws SkyFrame's app icon: a radar scope with a jet crossing its sweep. 180x180 is the
apple-touch-icon the device serves; other sizes are for the README (docs/icon.png).
Standard library only (4x4 supersampling for smooth edges, zlib for the PNG).
Usage: python make_touch_icon.py components/skyframe_portal/apple-touch-icon.png [size]"""
import math, struct, sys, zlib

SIZE, SS = (int(sys.argv[2]) if len(sys.argv) > 2 else 180), 4
K = SIZE / 180                                 # the design is drawn at 180 px
C = SIZE / 2                                   # radar center (iOS rounds the icon's corners)

def rgb(h): return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))
SKY_TOP, SKY_BOTTOM = rgb("#0d2a4a"), rgb("#030b17")
GLOW = rgb("#1b5c86")                          # soft light behind the scope
RING, SWEEP, WHITE, SHADOW = rgb("#5fd4ff"), rgb("#38f0a8"), (255, 255, 255), (0, 0, 0)

R_OUT = 70 * K                                 # scope radius in px
SWEEP_END, SWEEP_SPAN = 82.0, 115.0            # leading edge (deg clockwise from north), trail length

# Jet seen from above, nose up, length 1 (right half; mirrored for the left).
HALF = [(0, -.5), (.035, -.46), (.05, -.39), (.05, -.12), (.47, .09), (.47, .16), (.05, .07),
        (.042, .30), (.17, .41), (.17, .47), (.02, .44), (0, .47)]
JET = HALF + [(-x, y) for x, y in reversed(HALF[1:-1])]
JET_LEN, JET_HEADING, JET_AT = 78 * K, 45.0, (C + 22 * K, C - 22 * K)

def place(points, length, heading, at):
    a = math.radians(heading); s, c = math.sin(a), math.cos(a)
    return [(at[0] + (x * c - y * s) * length, at[1] + (x * s + y * c) * length) for x, y in points]
jet = place(JET, JET_LEN, JET_HEADING, JET_AT)
shadow = [(x + 3 * K, y + 5 * K) for x, y in jet]

def inside(poly, x, y):
    hit, j = False, len(poly) - 1
    for i in range(len(poly)):
        (xi, yi), (xj, yj) = poly[i], poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi: hit = not hit
        j = i
    return hit

def background(x, y):
    t = y / SIZE
    c = [SKY_TOP[i] + (SKY_BOTTOM[i] - SKY_TOP[i]) * t for i in range(3)]
    g = max(0.0, 1 - math.hypot(x - C, y - C) / (R_OUT * 1.25)) ** 2 * 0.55
    return [c[i] * (1 - g) + GLOW[i] * g for i in range(3)]

def layers(x, y):
    """(color, opacity) of each shape at (x, y), bottom to top."""
    dx, dy = x - C, y - C
    d = math.hypot(dx, dy)
    out = []
    if d <= R_OUT:                                                   # sweep: fades behind its edge
        behind = (SWEEP_END - math.degrees(math.atan2(dx, -dy))) % 360
        if behind <= SWEEP_SPAN:
            out.append((SWEEP, 0.72 * (1 - behind / SWEEP_SPAN) ** 1.6))
        if behind <= 1.6 or behind >= 359.6: out.append((SWEEP, 0.95))   # bright leading edge
    for r, w, a in ((R_OUT, 2.2 * K, 0.75), (R_OUT * 2 / 3, 1.4 * K, 0.35), (R_OUT / 3, 1.4 * K, 0.3)):
        if abs(d - r) <= w / 2: out.append((RING, a))
    if d <= R_OUT and (abs(dx) <= 0.6 * K or abs(dy) <= 0.6 * K): out.append((RING, 0.18))   # crosshair
    if d <= 3.2 * K: out.append((SWEEP, 1.0))                        # center
    if inside(shadow, x, y): out.append((SHADOW, 0.35))
    if inside(jet, x, y): out.append((WHITE, 1.0))
    return out

rows = []
for py in range(SIZE):
    row = bytearray([0])                                             # PNG filter: none
    for px in range(SIZE):
        acc = [0.0, 0.0, 0.0]
        for sy in range(SS):
            for sx in range(SS):
                x, y = px + (sx + .5) / SS, py + (sy + .5) / SS
                c = background(x, y)
                for color, a in layers(x, y):
                    c = [c[i] * (1 - a) + color[i] * a for i in range(3)]
                for i in range(3): acc[i] += c[i]
        row += bytes(min(255, round(v / (SS * SS))) for v in acc)
    rows.append(bytes(row))

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
with open(sys.argv[1], "wb") as f: f.write(png)
print(f"wrote {sys.argv[1]} ({len(png)} bytes)")
