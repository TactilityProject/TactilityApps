#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert Maki SVG icons (CC0) into 8-bit alpha masks in a C++ header.

Usage: python tools/make_poi_icons.py [--size 15] [--out main/Source/render/PoiIconData.h] [names...]
"""
import argparse, math, os, re, struct, zlib
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(ROOT, 'reference', 'maki', 'icons')
DEFAULT = """cafe restaurant fast-food bar beer bus rail rail-metro rail-light bicycle bicycle-share car car-rental fuel charging-station parking shop grocery bakery butcher convenience pharmacy hospital doctor dentist veterinary bank lodging campsite museum theatre cinema art-gallery castle monument attraction zoo park playground swimming stadium golf library college school town-hall police fire-station post toilet drinking-water information religious-christian religious-jewish religious-muslim place-of-worship cemetery ice-cream alcohol-shop music clothing-store laundry harbor aerialway entrance marker hairdresser optician mobile-phone furniture garden-centre hardware jewelry-store shoe gift florist car-repair suitcase ferry airport viewpoint natural-feature waste-basket""".split()

NUM = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')


def tokenize(d):
    for m in re.finditer(r'([A-Za-z])|([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)', d):
        yield m.group(1) if m.group(1) else float(m.group(2))


def arc_points(x1, y1, rx, ry, phi, fa, fs, x2, y2, n=12):
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    rx, ry = abs(rx), abs(ry)
    p = math.radians(phi)
    cp, sp = math.cos(p), math.sin(p)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p = cp * dx + sp * dy
    y1p = -sp * dx + cp * dy
    lam = x1p ** 2 / rx ** 2 + y1p ** 2 / ry ** 2
    if lam > 1:
        s = math.sqrt(lam)
        rx *= s
        ry *= s
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0, num / den)) if den else 0
    if fa == fs:
        co = -co
    cxp = co * rx * y1p / ry
    cyp = -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2

    def ang(ux, uy, vx, vy):
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not fs and dt > 0:
        dt -= 2 * math.pi
    elif fs and dt < 0:
        dt += 2 * math.pi
    pts = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        pts.append((cp * rx * math.cos(t) - sp * ry * math.sin(t) + cx,
                    sp * rx * math.cos(t) + cp * ry * math.sin(t) + cy))
    pts[-1] = (x2, y2)
    return pts


def bez(p0, p1, p2, p3, n=12):
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                    u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
    return out


def parse_path(d):
    """Returns a list of polygons (point lists), one per subpath."""
    toks = list(tokenize(d))
    i = 0
    polys = []
    cur = []
    x = y = sx = sy = 0.0
    cmd = None
    lc = lq = None

    def num():
        nonlocal i
        v = toks[i]
        i += 1
        return v

    while i < len(toks):
        if isinstance(toks[i], str):
            cmd = toks[i]
            i += 1
            if cmd in 'Zz':
                if cur:
                    polys.append(cur)
                    cur = []
                x, y = sx, sy
                lc = lq = None
                continue
        elif cmd is None:
            break
        rel = cmd.islower()
        C = cmd.upper()
        ox, oy = (x, y) if rel else (0, 0)
        if C == 'M':
            if cur:
                polys.append(cur)
            x = num() + ox
            y = num() + oy
            sx, sy = x, y
            cur = [(x, y)]
            cmd = 'l' if rel else 'L'
            lc = lq = None
            continue
        if not cur:
            cur = [(x, y)]
        if C == 'L':
            x = num() + ox
            y = num() + oy
            cur.append((x, y))
            lc = lq = None
        elif C == 'H':
            x = num() + ox
            cur.append((x, y))
            lc = lq = None
        elif C == 'V':
            y = num() + oy
            cur.append((x, y))
            lc = lq = None
        elif C in 'CS':
            if C == 'C':
                c1 = (num() + ox, num() + oy)
            else:
                c1 = (2 * x - lc[0], 2 * y - lc[1]) if lc else (x, y)
            c2 = (num() + ox, num() + oy)
            e = (num() + ox, num() + oy)
            cur += bez((x, y), c1, c2, e)
            lc = c2
            lq = None
            x, y = e
        elif C in 'QT':
            if C == 'Q':
                q = (num() + ox, num() + oy)
            else:
                q = (2 * x - lq[0], 2 * y - lq[1]) if lq else (x, y)
            e = (num() + ox, num() + oy)
            c1 = (x + 2 / 3 * (q[0] - x), y + 2 / 3 * (q[1] - y))
            c2 = (e[0] + 2 / 3 * (q[0] - e[0]), e[1] + 2 / 3 * (q[1] - e[1]))
            cur += bez((x, y), c1, c2, e)
            lq = q
            lc = None
            x, y = e
        elif C == 'A':
            rx, ry, phi, fa, fs = num(), num(), num(), num(), num()
            e = (num() + ox, num() + oy)
            cur += arc_points(x, y, rx, ry, phi, int(fa), int(fs), e[0], e[1])
            x, y = e
            lc = lq = None
        else:
            raise ValueError('bad command ' + cmd)
    if cur:
        polys.append(cur)
    return polys


def mul(a, b):
    return [a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],
            a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
            a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]]


def parse_transform(t):
    m = [1, 0, 0, 1, 0, 0]
    for name, args in re.findall(r'(\w+)\s*\(([^)]*)\)', t or ''):
        v = [float(a) for a in NUM.findall(args)]
        if name == 'translate':
            n = [1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0]
        elif name == 'scale':
            n = [v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0]
        elif name == 'matrix':
            n = v
        else:
            continue
        m = mul(m, n)
    return m


def load_icon(path):
    root = ET.parse(path).getroot()
    vb = [float(v) for v in re.split(r'[ ,]+', root.get('viewBox', '0 0 15 15').strip())]
    shapes = []

    def walk(el, m):
        tag = el.tag.split('}')[-1]
        if el.get('transform'):
            m = mul(m, parse_transform(el.get('transform')))
        skip = el.get('fill') == 'none' or el.get('opacity') in ('0', '0.0')

        def f(k):
            return float(el.get(k, 0))

        polys = None
        if not skip:
            if tag == 'path':
                polys = parse_path(el.get('d', ''))
            elif tag == 'circle':
                cx, cy, r = f('cx'), f('cy'), f('r')
                polys = [[(cx + r * math.cos(2 * math.pi * k / 48), cy + r * math.sin(2 * math.pi * k / 48))
                          for k in range(48)]]
            elif tag == 'rect':
                x, y, w, h = f('x'), f('y'), f('width'), f('height')
                polys = [[(x, y), (x + w, y), (x + w, y + h), (x, y + h)]]
            elif tag == 'polygon':
                v = [float(a) for a in NUM.findall(el.get('points', ''))]
                polys = [list(zip(v[0::2], v[1::2]))]
        if polys:
            polys = [[(m[0] * px + m[2] * py + m[4], m[1] * px + m[3] * py + m[5]) for px, py in p] for p in polys]
            shapes.append((polys, el.get('fill-rule') == 'evenodd'))
        for c in el:
            walk(c, m)

    walk(root, [1, 0, 0, 1, 0, 0])
    return vb, shapes


def rasterise(vb, shapes, size, ss=4):
    sx = size / vb[2]
    sy = size / vb[3]
    W = size * ss
    grid = [[0] * W for _ in range(W)]
    for polys, eo in shapes:
        edges = []
        for p in polys:
            pts = [((px - vb[0]) * sx * ss, (py - vb[1]) * sy * ss) for px, py in p]
            for k in range(len(pts)):
                a, b = pts[k], pts[(k + 1) % len(pts)]
                if a[1] != b[1]:
                    edges.append((a, b))
        for row in range(W):
            yc = row + 0.5
            xs = []
            for a, b in edges:
                if (a[1] <= yc < b[1]) or (b[1] <= yc < a[1]):
                    xs.append((a[0] + (yc - a[1]) * (b[0] - a[0]) / (b[1] - a[1]), 1 if b[1] > a[1] else -1))
            xs.sort()
            wind = 0
            for k in range(len(xs) - 1):
                wind = (wind ^ 1) if eo else (wind + xs[k][1])
                if wind:
                    x0 = max(0, math.ceil(xs[k][0] - 0.5))
                    x1 = min(W, math.ceil(xs[k + 1][0] - 0.5))
                    for cx in range(x0, x1):
                        grid[row][cx] = 1
    out = []
    for py in range(size):
        for px in range(size):
            c = sum(grid[py * ss + j][px * ss + i] for j in range(ss) for i in range(ss))
            out.append(round(c * 255 / (ss * ss)))
    return out


def write_png(path, w, h, gray):
    raw = b''.join(b'\x00' + bytes(gray[y * w:(y + 1) * w]) for y in range(h))

    def ch(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    with open(path, 'wb') as fh:
        fh.write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)) +
                 ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--size', type=int, default=15)
    ap.add_argument('--out', default=os.path.join(ROOT, 'main', 'Source', 'render', 'PoiIconData.h'))
    ap.add_argument('names', nargs='*')
    a = ap.parse_args()
    names = a.names or DEFAULT
    icons = {}
    skipped = []
    for n in names:
        p = os.path.join(ICON_DIR, n + '.svg')
        if not os.path.exists(p):
            skipped.append(n)
            continue
        vb, shapes = load_icon(p)
        icons[n] = rasterise(vb, shapes, a.size)
    S = a.size
    L = ['// SPDX-License-Identifier: GPL-3.0-or-later',
         '// Alpha masks generated from Maki icons (https://github.com/mapbox/maki, CC0 1.0)',
         '// by tools/make_poi_icons.py. Never edit by hand.',
         '#pragma once', '#include <cstdint>', '', 'namespace maps {', '',
         f'constexpr int POI_ICON_SIZE = {S};', '',
         'struct PoiIconMask { const char* name; uint8_t alpha[POI_ICON_SIZE * POI_ICON_SIZE]; };', '',
         '/** Sorted by name, for a binary search. */', 'constexpr PoiIconMask POI_ICON_MASKS[] = {']
    for n in sorted(icons):
        L.append(f'    {{ "{n}", {{')
        for r in range(S):
            L.append('        ' + ', '.join(str(v) for v in icons[n][r * S:(r + 1) * S]) + ',')
        L.append('    } },')
    L += ['};', '', 'constexpr int POI_ICON_COUNT = sizeof(POI_ICON_MASKS) / sizeof(POI_ICON_MASKS[0]);', '', '}', '']
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', newline='\n') as fh:
        fh.write('\n'.join(L))
    cols = 12
    cell = S + 2
    rows = max(1, (len(icons) + cols - 1) // cols)
    sc = 4
    W = cols * cell * sc
    H = rows * cell * sc
    img = [255] * (W * H)
    for k, n in enumerate(sorted(icons)):
        ox = (k % cols) * cell + 1
        oy = (k // cols) * cell + 1
        for y in range(S):
            for x in range(S):
                v = 255 - icons[n][y * S + x]
                for j in range(sc):
                    for i in range(sc):
                        img[((oy + y) * sc + j) * W + (ox + x) * sc + i] = v
    os.makedirs(os.path.join(ROOT, 'out'), exist_ok=True)
    write_png(os.path.join(ROOT, 'out', 'poi-icons-preview.png'), W, H, img)
    print(f'icons: {len(icons)}, mask bytes: {len(icons) * S * S}, skipped: {skipped}')


main()
