# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the compact TactilityMaps region catalog from Geofabrik's index.

Usage:
    python tools/build_catalog.py <index-v1.json> <regions.bin> [--report]

The input is Geofabrik's index-v1.json (GeoJSON FeatureCollection with region
outlines as MultiPolygons). The output is the little-endian "TMRC" version 1
format used by the device: a header, fixed-size region records, varint-encoded
outlines and UTF-8 names.

The outlines are an OpenStreetMap produced work in terms of the Open Database
License 1.0 (Geofabrik's polygons repository).

Region outlines © OpenStreetMap contributors, from Geofabrik GmbH (download.geofabrik.de), ODbL 1.0.
"""

import argparse
import json
import math
import struct
import sys

MAGIC = b"TMRC"
FORMAT_VERSION = 1
MAX_POINTS = 1500
MAX_NAME_BYTES = 63
UNIT = 10000.0  # units of 1e-4 degree per degree

LEVEL_TOLERANCE = {0: 0.05, 1: 0.01, 2: 0.005, 3: 0.005}

LICENCE = ("Region outlines © OpenStreetMap contributors, from Geofabrik GmbH "
           "(download.geofabrik.de), ODbL 1.0")


def varint_encode(value):
    """Encode a non-negative integer as unsigned LEB128."""
    if value < 0:
        raise ValueError("varint value must be non-negative")
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def varint_decode(data, pos):
    """Decode one unsigned LEB128 value; return (value, new_pos)."""
    result = 0
    shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            return result, pos
        shift += 7


def zigzag_encode(value):
    """Map a signed 32-bit integer to an unsigned one."""
    return (value << 1) ^ (value >> 31)


def zigzag_decode(value):
    """Inverse of zigzag_encode."""
    return (value >> 1) ^ -(value & 1)


def _dist_sq_point_segment(point, start, end):
    px, py = point
    ax, ay = start
    bx, by = end
    dx = bx - ax
    dy = by - ay
    if dx == 0.0 and dy == 0.0:
        return (px - ax) ** 2 + (py - ay) ** 2
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0
    cx = ax + t * dx
    cy = ay + t * dy
    return (px - cx) ** 2 + (py - cy) ** 2


def douglas_peucker(points, tolerance):
    """Simplify an open polyline, keeping points farther than tolerance."""
    if len(points) < 3:
        return list(points)
    tol_sq = tolerance * tolerance
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        first, last = stack.pop()
        if last <= first + 1:
            continue
        max_dist = -1.0
        split = -1
        for i in range(first + 1, last):
            dist = _dist_sq_point_segment(points[i], points[first], points[last])
            if dist > max_dist:
                max_dist = dist
                split = i
        if max_dist > tol_sq:
            keep[split] = True
            stack.append((first, split))
            stack.append((split, last))
    return [points[i] for i in range(len(points)) if keep[i]]


def simplify_ring(ring, tolerance):
    """Simplify one ring (implicitly closed) to at least four points."""
    points = list(ring)
    if len(points) > 1 and points[0] == points[-1]:
        points.pop()
    if len(points) <= 4:
        return points
    simplified = douglas_peucker(points, tolerance)
    if len(simplified) >= 4:
        return simplified
    n = len(points)
    indexes = sorted({int(round(k * (n - 1) / 3.0)) for k in range(4)})
    return [points[i] for i in indexes]


def _ring_bbox(ring):
    xs = [p[0] for p in ring]
    ys = [p[1] for p in ring]
    return min(xs), min(ys), max(xs), max(ys)


def _bbox_area(box):
    return max(0.0, box[2] - box[0]) * max(0.0, box[3] - box[1])


def filter_tiny_rings(rings, tolerance):
    """Drop tiny islands, but always keep the largest-bbox ring."""
    if not rings:
        return []
    boxes = [_ring_bbox(ring) for ring in rings]
    largest = max(range(len(rings)), key=lambda i: _bbox_area(boxes[i]))
    kept = []
    for i, ring in enumerate(rings):
        if i == largest:
            kept.append(ring)
            continue
        width = boxes[i][2] - boxes[i][0]
        height = boxes[i][3] - boxes[i][1]
        if width < 2.0 * tolerance and height < 2.0 * tolerance:
            continue
        kept.append(ring)
    return kept


def quantise(value):
    """Convert degrees to 1e-4 units, rounding halves away from zero."""
    scaled = value * UNIT
    if scaled >= 0.0:
        return int(math.floor(scaled + 0.5))
    return int(math.ceil(scaled - 0.5))


def quantise_ring(ring):
    """Quantise a ring and drop consecutive duplicates and the closing point."""
    out = []
    for lon, lat in ring:
        point = (quantise(lon), quantise(lat))
        if not out or out[-1] != point:
            out.append(point)
    if len(out) > 1 and out[0] == out[-1]:
        out.pop()
    return out


def _geometry_rings(geometry):
    """Extract every ring from a GeoJSON Polygon or MultiPolygon."""
    if not isinstance(geometry, dict):
        return []
    coords = geometry.get("coordinates") or []
    if geometry.get("type") == "Polygon":
        polygons = [coords]
    else:
        polygons = coords
    rings = []
    for polygon in polygons:
        for ring in polygon:
            points = [(float(p[0]), float(p[1])) for p in ring if len(p) >= 2]
            if len(points) >= 2:
                rings.append(points)
    return rings


def _trim_to_limit(rings, limit):
    """Last-resort fallback: keep the largest rings until the point budget fits."""
    if not rings:
        return rings
    order = sorted(range(len(rings)),
                   key=lambda i: _bbox_area(_ring_bbox(rings[i])), reverse=True)
    kept = [rings[order[0]]]
    total = len(rings[order[0]])
    for i in order[1:]:
        if total + len(rings[i]) <= limit:
            kept.append(rings[i])
            total += len(rings[i])
    return kept


def _points_bbox(rings):
    if not rings:
        return 0, 0, 0, 0
    xs = []
    ys = []
    for ring in rings:
        for lon, lat in ring:
            xs.append(lon)
            ys.append(lat)
    return min(xs), min(ys), max(xs), max(ys)


def prepare_rings(geometry, level):
    """Simplify, quantise and bound the geometry of one region."""
    original = _geometry_rings(geometry)
    tolerance = LEVEL_TOLERANCE.get(level, LEVEL_TOLERANCE[3])
    kept = []
    while True:
        simplified = [simplify_ring(ring, tolerance) for ring in original]
        kept = filter_tiny_rings(simplified, tolerance)
        if sum(len(ring) for ring in kept) <= MAX_POINTS:
            break
        tolerance *= 2.0
        if tolerance > 1e7:
            break
    if sum(len(ring) for ring in kept) > MAX_POINTS:
        kept = _trim_to_limit(kept, MAX_POINTS)
    quantised = [quantise_ring(ring) for ring in kept]
    # A ring needs three corners to enclose anything
    quantised = [ring for ring in quantised if len(ring) >= 3]
    return quantised, _points_bbox(quantised)


def extract_features(doc):
    """Read the properties and geometry we need from a FeatureCollection."""
    features = []
    for feature in doc.get("features", []):
        props = feature.get("properties") or {}
        region_id = props.get("id")
        if region_id is None:
            continue
        geometry = feature.get("geometry")
        if not isinstance(geometry, dict) or "coordinates" not in geometry:
            continue
        name = props.get("name")
        if name is None:
            name = region_id
        features.append({
            "id": region_id,
            "parent": props.get("parent"),
            "name": str(name),
            "geometry": geometry,
        })
    return features


def compute_levels(by_id):
    """Return id -> depth, with None for features whose parent chain is broken."""
    levels = {}
    for region_id in by_id:
        if region_id in levels:
            continue
        chain = []
        seen = set()
        current = region_id
        base = None
        ok = True
        while current not in levels:
            if current in seen:
                ok = False
                break
            seen.add(current)
            feature = by_id.get(current)
            if feature is None:
                ok = False
                break
            chain.append(current)
            parent = feature.get("parent")
            if not parent:
                base = -1
                break
            current = parent
        else:
            base = levels[current]
        if not ok or base is None:
            for item in chain:
                levels[item] = None
        else:
            depth = base
            for item in reversed(chain):
                depth += 1
                levels[item] = depth
    return levels


def build_catalog(doc):
    """Turn a decoded Geofabrik index into ordered, ready-to-encode regions."""
    by_id = {}
    for feature in extract_features(doc):
        by_id[feature["id"]] = feature
    levels = compute_levels(by_id)
    valid = [feature for feature in by_id.values()
             if levels.get(feature["id"]) is not None]
    valid_ids = {feature["id"] for feature in valid}

    children = {region_id: [] for region_id in valid_ids}
    roots = []
    for feature in valid:
        parent = feature.get("parent")
        if not parent:
            roots.append(feature["id"])
        else:
            children[parent].append(feature["id"])

    def sort_key(region_id):
        name = by_id[region_id]["name"]
        return name.casefold(), name

    for region_id in children:
        children[region_id].sort(key=sort_key)
    roots.sort(key=sort_key)

    ordered = []

    def visit(region_id):
        ordered.append(region_id)
        for child in children[region_id]:
            visit(child)

    for root in roots:
        visit(root)

    index = {region_id: i for i, region_id in enumerate(ordered)}
    regions = []
    for region_id in ordered:
        feature = by_id[region_id]
        level = min(levels[region_id], 3)
        rings, bbox = prepare_rings(feature["geometry"], level)
        parent = feature.get("parent")
        parent_index = index[parent] if parent and parent in index else 0xFFFF
        regions.append({
            "id": region_id,
            "parent": parent_index,
            "level": level,
            "name": feature["name"],
            "rings": rings,
            "bbox": bbox,
            "child_count": len(children[region_id]),
        })
    return regions


def truncate_name(name):
    """Encode the name and cut it to 63 bytes at a UTF-8 character boundary."""
    raw = name.encode("utf-8")
    if len(raw) <= MAX_NAME_BYTES:
        return raw
    raw = raw[:MAX_NAME_BYTES]
    while raw:
        try:
            raw.decode("utf-8")
            return raw
        except UnicodeDecodeError:
            raw = raw[:-1]
    return raw


def encode_catalog(regions):
    """Serialise ordered regions into the TMRC version 1 byte format."""
    count = len(regions)
    outline_offset = 16 + 32 * count

    names = bytearray()
    name_entries = []
    for region in regions:
        raw = truncate_name(region["name"])
        name_entries.append((raw, len(names)))
        names += raw

    outlines = bytearray()
    outline_offsets = []
    for region in regions:
        outline_offsets.append(len(outlines))
        for ring in region["rings"]:
            outlines += varint_encode(len(ring))
            if not ring:
                continue
            prev_lon, prev_lat = ring[0]
            outlines += varint_encode(zigzag_encode(prev_lon))
            outlines += varint_encode(zigzag_encode(prev_lat))
            for lon, lat in ring[1:]:
                outlines += varint_encode(zigzag_encode(lon - prev_lon))
                outlines += varint_encode(zigzag_encode(lat - prev_lat))
                prev_lon, prev_lat = lon, lat

    name_offset = outline_offset + len(outlines)
    header = struct.pack("<4sHHII", MAGIC, FORMAT_VERSION, count,
                         outline_offset, name_offset)

    records = bytearray()
    for i, region in enumerate(regions):
        raw, name_rel = name_entries[i]
        west, south, east, north = region["bbox"]
        records += struct.pack("<HBBIiiiiIHH",
                               region["parent"], region["level"], len(raw),
                               name_rel, west, south, east, north,
                               outline_offsets[i], len(region["rings"]),
                               region["child_count"])
    return bytes(header + records + outlines + names)


def _region_points(region):
    return sum(len(ring) for ring in region["rings"])


def print_report(regions, data, path):
    """Print per-level statistics and the ten point-heaviest regions."""
    per_level = {}
    for region in regions:
        count, points = per_level.get(region["level"], (0, 0))
        per_level[region["level"]] = (count + 1, points + _region_points(region))

    for level in sorted(per_level):
        count, points = per_level[level]
        print("level %d: %d regions, %d points, %.1f KB total"
              % (level, count, points, len(data) / 1024.0))

    print("Top regions by points:")
    for region in sorted(regions, key=_region_points, reverse=True)[:10]:
        print("  %6d  %s" % (_region_points(region), region["name"]))
    print(LICENCE)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Build the TactilityMaps region catalog from Geofabrik's index.")
    parser.add_argument("index", help="path to index-v1.json")
    parser.add_argument("out", help="path to write the regions.bin catalog")
    parser.add_argument("--report", action="store_true",
                        help="print per-level statistics and the largest regions")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        with open(args.index, "r", encoding="utf-8") as handle:
            doc = json.load(handle)
    except (OSError, ValueError) as error:
        print("error: cannot read %s: %s" % (args.index, error), file=sys.stderr)
        return 1

    regions = build_catalog(doc)
    data = encode_catalog(regions)
    try:
        with open(args.out, "wb") as handle:
            handle.write(data)
    except OSError as error:
        print("error: cannot write %s: %s" % (args.out, error), file=sys.stderr)
        return 1

    if args.report:
        print_report(regions, data, args.out)
    else:
        print("Wrote %s (%.1f KB)" % (args.out, len(data) / 1024.0))
        print(LICENCE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
