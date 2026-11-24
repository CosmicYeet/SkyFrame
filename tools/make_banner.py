"""Writes docs/banner.svg, the README header: SkyFrame's radar and jet (the same design as
the app icon, tools/make_touch_icon.py) beside the name.
Usage: python tools/make_banner.py docs/banner.svg"""
import math, sys

W, H = 1280, 400
CX, CY, R = 1015, 200, 165                     # radar center and radius

def polar(deg, r):
    a = math.radians(deg)
    return CX + math.sin(a) * r, CY - math.cos(a) * r

# Jet seen from above, nose up, length 1 (right half; mirrored) - as in make_touch_icon.py.
HALF = [(0, -.5), (.035, -.46), (.05, -.39), (.05, -.12), (.47, .09), (.47, .16), (.05, .07),
        (.042, .30), (.17, .41), (.17, .47), (.02, .44), (0, .47)]
JET = HALF + [(-x, y) for x, y in reversed(HALF[1:-1])]
def jet_path(length, heading, at, dx=0, dy=0):
    a = math.radians(heading); s, c = math.sin(a), math.cos(a)
    pts = [(at[0] + (x * c - y * s) * length + dx, at[1] + (x * s + y * c) * length + dy) for x, y in JET]
    return "M" + "L".join(f"{x:.1f} {y:.1f}" for x, y in pts) + "Z"

out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">',
       '<defs>',
       '<linearGradient id="sky" x1="0" y1="0" x2="1" y2="1">'
       '<stop offset="0" stop-color="#0d2a4a"/><stop offset="1" stop-color="#030b17"/></linearGradient>',
       f'<radialGradient id="glow" gradientUnits="userSpaceOnUse" cx="{CX}" cy="{CY}" r="{R * 1.45:.0f}">'
       '<stop offset="0" stop-color="#1b5c86" stop-opacity=".55"/><stop offset="1" stop-color="#1b5c86" stop-opacity="0"/></radialGradient>',
       '</defs>',
       f'<rect width="{W}" height="{H}" rx="28" fill="url(#sky)"/>',
       f'<rect width="{W}" height="{H}" rx="28" fill="url(#glow)"/>']

# Sweep: thin wedges fading out behind the leading edge.
EDGE, SPAN, STEPS = 82, 115, 23
for i in range(STEPS):
    a0, a1 = EDGE - SPAN * (i + 1) / STEPS, EDGE - SPAN * i / STEPS
    (x0, y0), (x1, y1) = polar(a0, R), polar(a1, R)
    op = 0.62 * (1 - i / STEPS) ** 1.6
    out.append(f'<path d="M{CX} {CY}L{x0:.1f} {y0:.1f}A{R} {R} 0 0 1 {x1:.1f} {y1:.1f}Z" fill="#38f0a8" fill-opacity="{op:.3f}"/>')
ex, ey = polar(EDGE, R)
out.append(f'<line x1="{CX}" y1="{CY}" x2="{ex:.1f}" y2="{ey:.1f}" stroke="#38f0a8" stroke-width="3" stroke-linecap="round"/>')

for r, w, o in ((R, 4, .8), (R * 2 / 3, 2, .38), (R / 3, 2, .32)):
    out.append(f'<circle cx="{CX}" cy="{CY}" r="{r:.1f}" fill="none" stroke="#5fd4ff" stroke-width="{w}" stroke-opacity="{o}"/>')
out.append(f'<path d="M{CX - R} {CY}H{CX + R}M{CX} {CY - R}V{CY + R}" stroke="#5fd4ff" stroke-opacity=".16" stroke-width="1.5"/>')
out.append(f'<circle cx="{CX}" cy="{CY}" r="6" fill="#38f0a8"/>')

# Other traffic: small blips, as on the radar.
for deg, rr in ((200, .62), (305, .78), (140, .4), (250, .9)):
    x, y = polar(deg, R * rr)
    out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" fill="#7cf5a3" fill-opacity=".85"/>')

# The jet, with a soft shadow.
at = (CX + 52, CY - 52)
out.append(f'<path d="{jet_path(178, 45, at, 7, 11)}" fill="#000" fill-opacity=".35"/>')
out.append(f'<path d="{jet_path(178, 45, at)}" fill="#fff"/>')

# Name and tagline.
font = "font-family=\"-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif\""
out += [f'<text x="92" y="186" {font} font-size="104" font-weight="700" fill="#ffffff" letter-spacing="-2">SkyFrame</text>',
        f'<text x="96" y="244" {font} font-size="30" fill="#b8d7ec">Live air-traffic radar, weather &amp; family photos</text>',
        f'<text x="96" y="290" {font} font-size="21" fill="#5fd4ff" letter-spacing="1">ESP32-S3  •  2.8" round touch display  •  ESP-IDF + LVGL</text>',
        '</svg>']

with open(sys.argv[1], "w", encoding="utf-8") as f: f.write("\n".join(out) + "\n")
print("wrote", sys.argv[1])
