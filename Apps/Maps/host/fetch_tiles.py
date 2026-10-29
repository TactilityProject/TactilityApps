# SPDX-License-Identifier: GPL-3.0-or-later
#!/usr/bin/env python3
"""Small, polite OpenFreeMap vector tile fetcher for TactilityMaps.

Usage:
    python host/fetch_tiles.py --out <root> --region <name> \
        --bbox west,south,east,north --minzoom 0 --maxzoom 14

Tiles are stored byte-for-byte as received (gzip-compressed MVT) in
<root>/maps/<region>/<z>/<x>/<y>.pbf, plus <root>/maps/<region>/meta.json.

Rules enforced (OpenFreeMap terms):
  * Tile URL template is read from the TileJSON at TILEJSON_URL, never hard-coded.
  * Never talks to openstreetmap.org servers.
  * Identifying User-Agent on every request.
  * Refuses jobs above MAX_TILES tiles (all zooms combined).
  * Strictly sequential requests.
"""

import argparse
import datetime
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.request

TILEJSON_URL = "https://tiles.openfreemap.org/planet"
USER_AGENT = "TactilityMaps/0.1 (+https://github.com/AdaSzi/TactilityMaps)"
MAX_TILES = 3000
MAX_BYTES = 50 * 1000 * 1000  # OpenFreeMap: 50 MB per download
MAX_ZOOM = 14            # OpenFreeMap vector tiles end at z14
MAX_LAT = 85.05112878    # Web Mercator limit
REGION_RE = re.compile(r"^[a-z0-9_-]{1,8}$")
REQUEST_DELAY_S = 0.05   # small pause between requests, on top of being sequential
TIMEOUT_S = 30
RETRIES = 2


def lonlat_to_tile(lon, lat, z):
    """Standard slippy-map tile index for a point at zoom z (clamped to the grid)."""
    lat = max(-MAX_LAT, min(MAX_LAT, lat))
    n = 1 << z
    x = int(math.floor((lon + 180.0) / 360.0 * n))
    lat_rad = math.radians(lat)
    y = int(math.floor((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n))
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def tile_range(bbox, z):
    """Return (x0, x1, y0, y1) inclusive for the bbox at zoom z."""
    west, south, east, north = bbox
    x0, y0 = lonlat_to_tile(west, north, z)   # north-west corner: smallest x and y
    x1, y1 = lonlat_to_tile(east, south, z)   # south-east corner: largest x and y
    return x0, x1, y0, y1


def plan(bbox, minzoom, maxzoom):
    """Return list of (z, x0, x1, y0, y1, count) and the total count."""
    rows = []
    total = 0
    for z in range(minzoom, maxzoom + 1):
        x0, x1, y0, y1 = tile_range(bbox, z)
        count = (x1 - x0 + 1) * (y1 - y0 + 1)
        rows.append((z, x0, x1, y0, y1, count))
        total += count
    return rows, total


class NoDecodeRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Default redirect handling; urllib never decodes gzip bodies on its own."""


def make_request(url, gzip_tiles):
    headers = {"User-Agent": USER_AGENT}
    if gzip_tiles:
        headers["Accept-Encoding"] = "gzip"
    return urllib.request.Request(url, headers=headers)


def http_get(url, gzip_tiles):
    """GET url. Returns (status, body_bytes). 404 is returned as (404, b'')."""
    last_err = None
    for attempt in range(RETRIES + 1):
        try:
            with urllib.request.urlopen(make_request(url, gzip_tiles), timeout=TIMEOUT_S) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            if e.code in (204, 404):
                return e.code, b""
            last_err = e
            if e.code < 500 and e.code != 429:
                break
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            last_err = e
        time.sleep(1.0 + attempt * 2.0)
    raise RuntimeError("request failed for %s: %s" % (url, last_err))


def get_template():
    status, body = http_get(TILEJSON_URL, False)
    tj = json.loads(body.decode("utf-8"))
    template = tj["tiles"][0]
    if "openstreetmap.org" in template.lower():
        raise RuntimeError("refusing tile server on openstreetmap.org: " + template)
    for key in ("{z}", "{x}", "{y}"):
        if key not in template:
            raise RuntimeError("tile template lacks %s: %s" % (key, template))
    return template


def parse_args(argv):
    p = argparse.ArgumentParser(description="Polite OpenFreeMap vector tile downloader.")
    p.add_argument("--out", required=True, help="output root (tiles go to <out>/maps/<region>/...)")
    p.add_argument("--region", required=True, help="region name, 1-8 chars of [a-z0-9_-]")
    p.add_argument("--bbox", required=True, help="west,south,east,north in degrees")
    p.add_argument("--minzoom", type=int, default=0)
    p.add_argument("--maxzoom", type=int, default=14)
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    if not REGION_RE.match(args.region):
        print("error: region must be 1-8 chars of [a-z0-9_-]", file=sys.stderr)
        return 2
    try:
        bbox = [float(v) for v in args.bbox.split(",")]
        if len(bbox) != 4:
            raise ValueError
    except ValueError:
        print("error: --bbox must be west,south,east,north", file=sys.stderr)
        return 2
    west, south, east, north = bbox
    if not (-180 <= west < east <= 180 and -90 <= south < north <= 90):
        print("error: invalid bbox (need -180<=west<east<=180, -90<=south<north<=90)", file=sys.stderr)
        return 2
    if not (0 <= args.minzoom <= args.maxzoom <= MAX_ZOOM):
        print("error: need 0 <= minzoom <= maxzoom <= %d" % MAX_ZOOM, file=sys.stderr)
        return 2

    rows, total = plan(bbox, args.minzoom, args.maxzoom)
    print("Planned tiles: %d (limit %d)" % (total, MAX_TILES))
    for z, x0, x1, y0, y1, count in rows:
        print("  z%-2d x %d..%d y %d..%d : %d" % (z, x0, x1, y0, y1, count))
    if total > MAX_TILES:
        print("Refusing: %d tiles exceeds the limit of %d." % (total, MAX_TILES))
        return 1

    template = get_template()
    print("Tile template:", template)

    region_dir = os.path.join(args.out, "maps", args.region)
    os.makedirs(region_dir, exist_ok=True)

    downloaded = 0
    skipped = 0
    empty = []
    total_bytes = 0
    per_zoom = {z: [0, 0] for z in range(args.minzoom, args.maxzoom + 1)}  # z -> [count, bytes]
    failed = 0

    def write_meta():
        meta = {
            "name": args.region,
            "bbox": bbox,
            "minzoom": args.minzoom,
            "maxzoom": args.maxzoom,
            "source": template,
            "date": datetime.date.today().isoformat(),
            "total": total,
            "done": downloaded + skipped + len(empty),
            "bytes": total_bytes,
        }
        tmp = os.path.join(region_dir, "meta.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(meta, f)
        os.replace(tmp, os.path.join(region_dir, "meta.json"))

    try:
        for z, x0, x1, y0, y1, _count in rows:
            for x in range(x0, x1 + 1):
                xdir = os.path.join(region_dir, str(z), str(x))
                for y in range(y0, y1 + 1):
                    path = os.path.join(xdir, "%d.pbf" % y)
                    if os.path.exists(path):
                        skipped += 1
                        size = os.path.getsize(path)
                        per_zoom[z][0] += 1
                        per_zoom[z][1] += size
                        continue
                    url = template.replace("{z}", str(z)).replace("{x}", str(x)).replace("{y}", str(y))
                    try:
                        status, body = http_get(url, True)
                    except RuntimeError as e:
                        print("  ERROR z%d/%d/%d: %s" % (z, x, y, e), file=sys.stderr)
                        failed += 1
                        time.sleep(REQUEST_DELAY_S)
                        continue
                    if status in (204, 404) or (status == 200 and len(body) == 0):
                        empty.append((z, x, y, status))
                        print("  empty tile z%d/%d/%d (HTTP %d)" % (z, x, y, status))
                        time.sleep(REQUEST_DELAY_S)
                        continue
                    os.makedirs(xdir, exist_ok=True)
                    tmp = path + ".tmp"
                    with open(tmp, "wb") as f:
                        f.write(body)
                    os.replace(tmp, path)
                    downloaded += 1
                    total_bytes += len(body)
                    if total_bytes > MAX_BYTES:
                        print("Stopping: downloaded bytes exceed the %d byte limit." % MAX_BYTES)
                        failed += 1
                        raise SystemExit(1)
                    per_zoom[z][0] += 1
                    per_zoom[z][1] += len(body)
                    time.sleep(REQUEST_DELAY_S)
            print("zoom %d done (downloaded so far: %d, skipped: %d)" % (z, downloaded, skipped))
    finally:
        write_meta()

    print()
    print("Tiles downloaded: %d" % downloaded)
    print("Tiles skipped (already present): %d" % skipped)
    print("Empty tiles (204/404): %d" % len(empty))
    print("Failed requests: %d" % failed)
    print("Total bytes (downloaded this run): %d" % total_bytes)
    all_bytes = sum(v[1] for v in per_zoom.values())
    print("Total bytes on disk for this region: %d" % all_bytes)
    print()
    print("%-5s %8s %14s" % ("zoom", "tiles", "avg bytes"))
    for z in sorted(per_zoom):
        cnt, size = per_zoom[z]
        avg = size // cnt if cnt else 0
        print("%-5d %8d %14d" % (z, cnt, avg))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
