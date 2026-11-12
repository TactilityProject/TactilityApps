// SPDX-License-Identifier: GPL-3.0-or-later
#include "RouteGraph.h"

#include "../platform/Platform.h"

#include <miniz.h>
// miniz maps inflate to mz_inflate, which hides MvtDecoder::inflate
#ifdef inflate
#undef inflate
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace maps {

namespace {

constexpr char MAGIC[4] = { 'T', 'M', 'P', 'G' };
constexpr int GRID_CELL = 32;
constexpr int GRID_SIZE = GRAPH_EXTENT / GRID_CELL;
/** Longer edges are split by a node, so a shape fits TileGraph::shape()'s buffer. */
constexpr uint32_t MAX_SHAPE_POINTS = 250;
constexpr double EARTH_CIRCUMFERENCE = 40075016.686;
constexpr double PI_D = 3.14159265358979323846;

/** Rounds to the nearest integer: lroundf is not exported to apps by the firmware. */
/** hypot and hypotf are not exported to apps either. */
template <typename T>
T distanceOf(T dx, T dy) {
    return std::sqrt(dx * dx + dy * dy);
}

int32_t roundUnit(float value) {
    return static_cast<int32_t>(std::floor(value + 0.5f));
}

uint16_t readU16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t readU32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

uint32_t pointKey(int32_t x, int32_t y) {
    return (static_cast<uint32_t>(x + 1) << 16) | static_cast<uint32_t>(y + 1);
}

int deflateCallback(const void* buffer, int length, void* user) {
    auto* out = static_cast<ByteWriter*>(user);
    out->putBytes(buffer, static_cast<size_t>(length));
    return out->failed() ? 0 : 1;
}

bool textIs(const char* text, uint16_t length, const char* value) {
    return text != nullptr && strlen(value) == length && memcmp(text, value, length) == 0;
}

uint8_t levelOf(Value brunnel, int8_t layer) {
    const uint8_t kind = brunnel == Value::bridge ? 1 : (brunnel == Value::tunnel ? 2 : 0);
    return static_cast<uint8_t>((kind << 4) | ((layer + 8) & 15));
}

} // namespace

RoadClass roadClassOf(Value featureClass) {
    switch (featureClass) {
        case Value::motorway:
            return RoadClass::Motorway;
        case Value::trunk:
            return RoadClass::Trunk;
        case Value::primary:
            return RoadClass::Primary;
        case Value::secondary:
            return RoadClass::Secondary;
        case Value::tertiary:
            return RoadClass::Tertiary;
        case Value::minor:
            return RoadClass::Minor;
        case Value::service:
            return RoadClass::Service;
        case Value::track:
            return RoadClass::Track;
        case Value::path:
            return RoadClass::Path;
        default:
            return RoadClass::Count;
    }
}

const char* roadClassName(RoadClass roadClass) {
    static const char* const NAMES[] = { "motorway", "trunk", "primary", "secondary", "tertiary", "minor", "service", "track", "path" };
    return roadClass < RoadClass::Count ? NAMES[static_cast<int>(roadClass)] : "";
}

bool readGraphHeader(const uint8_t* data, size_t size, GraphHeader& header) {
    if (data == nullptr || size < GRAPH_HEADER_SIZE || memcmp(data, MAGIC, 4) != 0) {
        return false;
    }
    header.version = readU16(data + 4);
    header.flags = readU16(data + 6);
    header.tile.z = data[8];
    header.extent = readU16(data + 10);
    header.tile.x = static_cast<int>(readU32(data + 12));
    header.tile.y = static_cast<int>(readU32(data + 16));
    header.sourceSize = readU32(data + 20);
    header.nodeCount = readU32(data + 24);
    header.edgeCount = readU32(data + 28);
    header.shapeBytes = readU32(data + 32);
    header.nameBytes = readU32(data + 36);
    header.bodySize = readU32(data + 40);
    header.storedSize = readU32(data + 44);
    memcpy(header.sourceVersion, data + 48, PREPARED_VERSION_LENGTH);
    header.sourceVersion[PREPARED_VERSION_LENGTH] = '\0';
    return true;
}

bool isGraphCurrent(const GraphHeader& header, const TileKey& tile, uint32_t sourceSize) {
    return header.version == GRAPH_FORMAT_VERSION && header.tile == tile && header.sourceSize == sourceSize && header.extent == GRAPH_EXTENT;
}

// ---------------------------------------------------------------------------------------------------------------

void GraphBuilder::trim() {
    ring.release();
    ways.release();
    wayPoints.release();
    pointWay.release();
    vertices.release();
    shared.release();
    nodeOf.release();
    nodes.release();
    edges.release();
    shapes.release();
    names.release();
    nameLines.release();
    namePoints.release();
    namePointLine.release();
    cellStart.release();
    cellItems.release();
}

bool GraphBuilder::beginFeature(const FeatureInfo& info) {
    if (info.geometryType != GEOMETRY_LINE) {
        return false;
    }
    if (info.layer == Layer::TransportationName) {
        const RoadClass roadClass = roadClassOf(info.featureClass);
        if (info.name == nullptr || info.nameLength == 0 || info.nameLength > 255) {
            return false;
        }
        inNames = true;
        currentName = internName(info.name, info.nameLength);
        current = {};
        current.roadClass = static_cast<uint8_t>(roadClass);
        scale = nameScale;
        return currentName != 0;
    }
    const RoadClass roadClass = roadClassOf(info.featureClass);
    if (roadClass == RoadClass::Count) {
        return false;
    }
    inNames = false;
    current = {};
    current.roadClass = static_cast<uint8_t>(roadClass);
    current.flags = static_cast<uint8_t>((info.brunnel == Value::bridge ? EDGE_BRIDGE : 0) | (info.brunnel == Value::tunnel ? EDGE_TUNNEL : 0) |
        (info.ramp ? EDGE_RAMP : 0) | (textIs(info.subclassText, info.subclassTextLength, "steps") ? EDGE_STEPS : 0) |
        (textIs(info.subclassText, info.subclassTextLength, "footway") || textIs(info.subclassText, info.subclassTextLength, "pedestrian") ?
                EDGE_FOOTWAY :
                0) |
        (textIs(info.subclassText, info.subclassTextLength, "cycleway") ? EDGE_CYCLEWAY : 0));
    current.oneway = info.oneway;
    current.level = levelOf(info.brunnel, info.layerNumber);
    scale = static_cast<float>(GRAPH_EXTENT) / static_cast<float>(decoderExtent);
    counters.ways++;
    return true;
}

void GraphBuilder::beginRing() {
    ring.clear();
}

void GraphBuilder::points(const TilePoint* chunk, int count) {
    for (int i = 0; i < count; i++) {
        // Lines reach into the tile's buffer: up to a few hundred units outside, well inside int16
        const auto x = static_cast<int32_t>(roundUnit(chunk[i].x * scale));
        const auto y = static_cast<int32_t>(roundUnit(chunk[i].y * scale));
        const Point point { static_cast<int16_t>(std::clamp<int32_t>(x, -30000, 30000)), static_cast<int16_t>(std::clamp<int32_t>(y, -30000, 30000)) };
        if (ring.size() > 0 && ring[ring.size() - 1].x == point.x && ring[ring.size() - 1].y == point.y) {
            continue;
        }
        ring.push(point);
    }
}

void GraphBuilder::endRing(bool closed) {
    (void)closed;
    if (ring.size() < 2) {
        return;
    }
    if (inNames) {
        NameLine line { static_cast<uint32_t>(namePoints.size()), static_cast<uint32_t>(ring.size()), currentName, current.roadClass };
        for (const Point& point : ring) {
            namePoints.push(point);
        }
        nameLines.push(line);
        return;
    }
    clipAndAdd();
}

void GraphBuilder::clipAndAdd() {
    // Liang-Barsky per segment, intersections rounded, as in the study
    const int lo = 0;
    const int hi = GRAPH_EXTENT;
    GrowArray<Point>& piece = clipped;
    piece.clear();
    for (size_t i = 0; i + 1 < ring.size(); i++) {
        const float x0 = ring[i].x;
        const float y0 = ring[i].y;
        const float dx = ring[i + 1].x - x0;
        const float dy = ring[i + 1].y - y0;
        float t0 = 0.0f;
        float t1 = 1.0f;
        bool inside = true;
        const float p[4] = { -dx, dx, -dy, dy };
        const float q[4] = { x0 - lo, hi - x0, y0 - lo, hi - y0 };
        for (int k = 0; k < 4 && inside; k++) {
            if (p[k] == 0.0f) {
                inside = q[k] >= 0.0f;
            } else {
                const float r = q[k] / p[k];
                if (p[k] < 0.0f) {
                    if (r > t1) {
                        inside = false;
                    } else {
                        t0 = std::max(t0, r);
                    }
                } else {
                    if (r < t0) {
                        inside = false;
                    } else {
                        t1 = std::min(t1, r);
                    }
                }
            }
        }
        if (!inside) {
            if (piece.size() >= 2) {
                addPart(piece.begin(), piece.size());
            }
            piece.clear();
            continue;
        }
        const Point a { static_cast<int16_t>(roundUnit(x0 + t0 * dx)), static_cast<int16_t>(roundUnit(y0 + t0 * dy)) };
        const Point b { static_cast<int16_t>(roundUnit(x0 + t1 * dx)), static_cast<int16_t>(roundUnit(y0 + t1 * dy)) };
        if (a.x == b.x && a.y == b.y) {
            continue;
        }
        if (piece.size() > 0 && piece[piece.size() - 1].x == a.x && piece[piece.size() - 1].y == a.y) {
            piece.push(b);
        } else {
            if (piece.size() >= 2) {
                addPart(piece.begin(), piece.size());
            }
            piece.clear();
            piece.push(a);
            piece.push(b);
        }
    }
    if (piece.size() >= 2) {
        addPart(piece.begin(), piece.size());
    }
}

void GraphBuilder::addPart(const Point* points, size_t count) {
    // Half-open ownership: a segment lying on the x = 4096 or y = 4096 side belongs to the neighbour tile
    size_t start = 0;
    for (size_t i = 0; i + 1 <= count; i++) {
        const bool last = i + 1 == count;
        const bool onFar = !last && ((points[i].x == GRAPH_EXTENT && points[i + 1].x == GRAPH_EXTENT) ||
            (points[i].y == GRAPH_EXTENT && points[i + 1].y == GRAPH_EXTENT));
        if (!onFar && !last) {
            continue;
        }
        const size_t end = last ? count : i + 1;
        if (end - start >= 2) {
            Way way = current;
            way.first = static_cast<uint32_t>(wayPoints.size());
            way.count = static_cast<uint32_t>(end - start);
            for (size_t j = start; j < end; j++) {
                wayPoints.push(points[j]);
            }
            ways.push(way);
            counters.points += way.count;
        }
        start = i + 1;
    }
}

bool GraphBuilder::indexPoints() {
    // Every way vertex by its coordinate: sorted, so the vertices sharing one are next to each other
    const size_t count = wayPoints.size();
    if (!pointWay.resize(count) || !vertices.resize(count) || !shared.resize(count)) {
        return false;
    }
    for (size_t w = 0; w < ways.size(); w++) {
        for (uint32_t j = 0; j < ways[w].count; j++) {
            pointWay[ways[w].first + j] = static_cast<uint32_t>(w);
        }
    }
    for (size_t i = 0; i < count; i++) {
        vertices[i] = { pointKey(wayPoints[i].x, wayPoints[i].y), static_cast<uint32_t>(i) };
    }
    std::sort(vertices.begin(), vertices.end(), [](const Vertex& a, const Vertex& b) {
        return a.key != b.key ? a.key < b.key : a.point < b.point;
    });
    for (size_t i = 0; i < count;) {
        size_t j = i;
        while (j < count && vertices[j].key == vertices[i].key) {
            j++;
        }
        for (size_t k = i; k < j; k++) {
            shared[vertices[k].point] = static_cast<uint16_t>(std::min<size_t>(j - i, 65535));
        }
        i = j;
    }
    return true;
}

void GraphBuilder::snapJunctions() {
    // A grid of the segments, 32 units a cell
    const size_t cellCount = static_cast<size_t>(GRID_SIZE) * GRID_SIZE;
    if (!cellStart.resize(cellCount + 1)) {
        return;
    }
    auto cellsOf = [](const Point& a, const Point& b, int& left, int& top, int& right, int& bottom) {
        left = std::clamp(std::min<int>(a.x, b.x) / GRID_CELL, 0, GRID_SIZE - 1);
        right = std::clamp(std::max<int>(a.x, b.x) / GRID_CELL, 0, GRID_SIZE - 1);
        top = std::clamp(std::min<int>(a.y, b.y) / GRID_CELL, 0, GRID_SIZE - 1);
        bottom = std::clamp(std::max<int>(a.y, b.y) / GRID_CELL, 0, GRID_SIZE - 1);
    };
    for (const Way& way : ways) {
        for (uint32_t j = 0; j + 1 < way.count; j++) {
            int left;
            int top;
            int right;
            int bottom;
            cellsOf(wayPoints[way.first + j], wayPoints[way.first + j + 1], left, top, right, bottom);
            for (int cy = top; cy <= bottom; cy++) {
                for (int cx = left; cx <= right; cx++) {
                    cellStart[static_cast<size_t>(cy) * GRID_SIZE + cx + 1]++;
                }
            }
        }
    }
    for (size_t i = 1; i <= cellCount; i++) {
        cellStart[i] += cellStart[i - 1];
    }
    if (!cellItems.resize(cellStart[cellCount])) {
        return;
    }
    {
        GrowArray<uint32_t> fill;
        if (!fill.resize(cellCount)) {
            return;
        }
        for (const Way& way : ways) {
            for (uint32_t j = 0; j + 1 < way.count; j++) {
                int left;
                int top;
                int right;
                int bottom;
                cellsOf(wayPoints[way.first + j], wayPoints[way.first + j + 1], left, top, right, bottom);
                for (int cy = top; cy <= bottom; cy++) {
                    for (int cx = left; cx <= right; cx++) {
                        const size_t cell = static_cast<size_t>(cy) * GRID_SIZE + cx;
                        cellItems[cellStart[cell] + fill[cell]++] = way.first + j;
                    }
                }
            }
        }
    }
    struct Insert {
        uint32_t segment;
        float t;
        Point point;
    };
    struct Move {
        uint32_t point;
        Point to;
    };
    GrowArray<Insert> inserts;
    GrowArray<Move> moves;
    for (size_t w = 0; w < ways.size(); w++) {
        const Way& way = ways[w];
        for (int end = 0; end < 2; end++) {
            const uint32_t index = end == 0 ? way.first : way.first + way.count - 1;
            if (shared[index] != 1) {
                continue;
            }
            const Point p = wayPoints[index];
            float bestDistance = static_cast<float>(GRAPH_SNAP_UNITS) + 1e-3f;
            uint32_t bestSegment = UINT32_MAX;
            float bestT = 0.0f;
            const int left = std::clamp((p.x - GRAPH_SNAP_UNITS) / GRID_CELL, 0, GRID_SIZE - 1);
            const int right = std::clamp((p.x + GRAPH_SNAP_UNITS) / GRID_CELL, 0, GRID_SIZE - 1);
            const int top = std::clamp((p.y - GRAPH_SNAP_UNITS) / GRID_CELL, 0, GRID_SIZE - 1);
            const int bottom = std::clamp((p.y + GRAPH_SNAP_UNITS) / GRID_CELL, 0, GRID_SIZE - 1);
            for (int cy = top; cy <= bottom; cy++) {
                for (int cx = left; cx <= right; cx++) {
                    const size_t cell = static_cast<size_t>(cy) * GRID_SIZE + cx;
                    for (uint32_t k = cellStart[cell]; k < cellStart[cell + 1]; k++) {
                        const uint32_t segment = cellItems[k];
                        const uint32_t otherWay = pointWay[segment];
                        if (otherWay == w || ways[otherWay].level != way.level) {
                            continue;
                        }
                        const Point a = wayPoints[segment];
                        const Point b = wayPoints[segment + 1];
                        const float dx = static_cast<float>(b.x - a.x);
                        const float dy = static_cast<float>(b.y - a.y);
                        const float lengthSquared = dx * dx + dy * dy;
                        float t = lengthSquared > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared : 0.0f;
                        t = std::clamp(t, 0.0f, 1.0f);
                        const float distance = distanceOf(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
                        if (distance < bestDistance) {
                            bestDistance = distance;
                            bestSegment = segment;
                            bestT = t;
                        }
                    }
                }
            }
            if (bestSegment == UINT32_MAX) {
                continue;
            }
            const Point a = wayPoints[bestSegment];
            const Point b = wayPoints[bestSegment + 1];
            const Point q { static_cast<int16_t>(roundUnit(a.x + bestT * (b.x - a.x))), static_cast<int16_t>(roundUnit(a.y + bestT * (b.y - a.y))) };
            if ((q.x == a.x && q.y == a.y) || (q.x == b.x && q.y == b.y)) {
                counters.snappedToVertex++;
            } else {
                counters.snappedToSegment++;
                inserts.push({ bestSegment, bestT, q });
            }
            moves.push({ index, q });
        }
    }
    for (const Move& move : moves) {
        wayPoints[move.point] = move.to;
    }
    if (inserts.size() == 0 && moves.size() == 0) {
        return;
    }
    // Rebuild the point list with the inserted foot points, dropping repeated points
    std::sort(inserts.begin(), inserts.end(), [](const Insert& a, const Insert& b) {
        return a.segment != b.segment ? a.segment < b.segment : a.t < b.t;
    });
    GrowArray<Point> rebuilt;
    GrowArray<Way> kept;
    size_t next = 0;
    for (const Way& way : ways) {
        Way copy = way;
        copy.first = static_cast<uint32_t>(rebuilt.size());
        for (uint32_t j = 0; j < way.count; j++) {
            const uint32_t index = way.first + j;
            const Point p = wayPoints[index];
            if (rebuilt.size() == copy.first || rebuilt[rebuilt.size() - 1].x != p.x || rebuilt[rebuilt.size() - 1].y != p.y) {
                rebuilt.push(p);
            }
            while (next < inserts.size() && inserts[next].segment == index) {
                const Point q = inserts[next].point;
                if (rebuilt[rebuilt.size() - 1].x != q.x || rebuilt[rebuilt.size() - 1].y != q.y) {
                    rebuilt.push(q);
                }
                next++;
            }
        }
        copy.count = static_cast<uint32_t>(rebuilt.size() - copy.first);
        if (copy.count >= 2) {
            kept.push(copy);
        } else {
            while (rebuilt.size() > copy.first) {
                rebuilt.pop();
            }
        }
    }
    wayPoints.clear();
    for (const Point& p : rebuilt) {
        wayPoints.push(p);
    }
    ways.clear();
    for (const Way& way : kept) {
        ways.push(way);
    }
}

bool GraphBuilder::makeNodes() {
    if (!indexPoints() || !nodeOf.resize(wayPoints.size())) {
        return false;
    }
    std::fill(nodeOf.begin(), nodeOf.end(), UINT32_MAX);
    nodes.clear();
    auto isEnd = [&](uint32_t point) {
        const Way& way = ways[pointWay[point]];
        return point == way.first || point == way.first + way.count - 1;
    };
    // Union-find over the vertices at one coordinate: same level, or one of them a way end
    constexpr size_t MAX_GROUP = 64;
    uint8_t parent[MAX_GROUP];
    auto find = [&](uint8_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    const size_t count = vertices.size();
    for (size_t i = 0; i < count;) {
        size_t j = i;
        while (j < count && vertices[j].key == vertices[i].key) {
            j++;
        }
        const size_t size = std::min(j - i, MAX_GROUP);
        if (size >= 2) {
            for (size_t a = 0; a < size; a++) {
                parent[a] = static_cast<uint8_t>(a);
            }
            for (size_t a = 0; a < size; a++) {
                for (size_t b = a + 1; b < size; b++) {
                    const uint32_t pa = vertices[i + a].point;
                    const uint32_t pb = vertices[i + b].point;
                    if (isEnd(pa) || isEnd(pb) || ways[pointWay[pa]].level == ways[pointWay[pb]].level) {
                        parent[find(static_cast<uint8_t>(a))] = find(static_cast<uint8_t>(b));
                    }
                }
            }
            for (size_t a = 0; a < size; a++) {
                const uint8_t root = find(static_cast<uint8_t>(a));
                // A group of one mid vertex is no node (a bridge over a road)
                bool alone = true;
                for (size_t b = 0; b < size && alone; b++) {
                    alone = b == a || find(static_cast<uint8_t>(b)) != root;
                }
                const uint32_t point = vertices[i + a].point;
                if (alone && !isEnd(point)) {
                    continue;
                }
                // The group's node: the one of its first member
                uint32_t node = UINT32_MAX;
                for (size_t b = 0; b < a && node == UINT32_MAX; b++) {
                    if (find(static_cast<uint8_t>(b)) == root) {
                        node = nodeOf[vertices[i + b].point];
                    }
                }
                if (node == UINT32_MAX) {
                    node = static_cast<uint32_t>(nodes.size());
                    const Point p = wayPoints[point];
                    nodes.push({ static_cast<uint16_t>(p.x), static_cast<uint16_t>(p.y), ways[pointWay[point]].level });
                }
                nodeOf[point] = node;
            }
        }
        i = j;
    }
    // Way ends not shared with anything
    for (const Way& way : ways) {
        for (int end = 0; end < 2; end++) {
            const uint32_t point = end == 0 ? way.first : way.first + way.count - 1;
            if (nodeOf[point] == UINT32_MAX) {
                const Point p = wayPoints[point];
                nodeOf[point] = static_cast<uint32_t>(nodes.size());
                nodes.push({ static_cast<uint16_t>(p.x), static_cast<uint16_t>(p.y), way.level });
            }
        }
        // Long stretches without a node get one, so every shape fits the reader's buffer
        uint32_t since = 0;
        for (uint32_t j = 1; j + 1 < way.count; j++) {
            const uint32_t point = way.first + j;
            if (nodeOf[point] != UINT32_MAX) {
                since = 0;
            } else if (++since >= MAX_SHAPE_POINTS) {
                const Point p = wayPoints[point];
                nodeOf[point] = static_cast<uint32_t>(nodes.size());
                nodes.push({ static_cast<uint16_t>(p.x), static_cast<uint16_t>(p.y), way.level });
                since = 0;
            }
        }
    }
    return !nodes.failed() && nodes.size() <= 65535;
}

bool GraphBuilder::makeEdges() {
    edges.clear();
    shapes.clear();
    // Metres per unit at a tile row: Mercator scale at its latitude
    const double worldUnits = std::ldexp(static_cast<double>(GRAPH_EXTENT), tile.z);
    auto metresPerUnit = [&](double y) {
        const double globalY = tile.y * static_cast<double>(GRAPH_EXTENT) + y;
        const double latitude = std::atan(std::sinh(PI_D * (1.0 - 2.0 * globalY / worldUnits)));
        return EARTH_CIRCUMFERENCE / worldUnits * std::cos(latitude);
    };
    for (const Way& way : ways) {
        uint32_t start = way.first;
        double length = 0.0;
        for (uint32_t j = 1; j < way.count; j++) {
            const uint32_t point = way.first + j;
            const Point a = wayPoints[point - 1];
            const Point b = wayPoints[point];
            length += distanceOf(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y)) * metresPerUnit((a.y + b.y) * 0.5);
            if (nodeOf[point] == UINT32_MAX) {
                continue;
            }
            const uint32_t from = nodeOf[start];
            const uint32_t to = nodeOf[point];
            if (from != to) {
                const bool reversed = way.oneway < 0;
                GraphEdge edge {};
                edge.from = static_cast<uint16_t>(reversed ? to : from);
                edge.to = static_cast<uint16_t>(reversed ? from : to);
                edge.lengthDm = static_cast<uint16_t>(std::min(65535.0, std::round(length * 10.0)));
                edge.roadClass = way.roadClass;
                edge.flags = static_cast<uint8_t>(way.flags | (way.oneway != 0 ? EDGE_ONEWAY : 0));
                edge.shapeOffset = static_cast<uint32_t>(shapes.size());
                edge.shapeCount = static_cast<uint16_t>(point - start - 1);
                // Deltas from the edge's from-node along the way to go
                int32_t x = reversed ? wayPoints[point].x : wayPoints[start].x;
                int32_t y = reversed ? wayPoints[point].y : wayPoints[start].y;
                for (uint32_t k = 1; k + 1 <= point - start; k++) {
                    const uint32_t index = reversed ? point - k : start + k;
                    shapes.putZigzag(wayPoints[index].x - x);
                    shapes.putZigzag(wayPoints[index].y - y);
                    x = wayPoints[index].x;
                    y = wayPoints[index].y;
                }
                // The name next to the edge's middle point
                const uint32_t middle = start + (point - start) / 2;
                const Point m0 = wayPoints[middle];
                const Point m1 = wayPoints[std::min(middle + 1, point)];
                edge.name = nameNear((m0.x + m1.x) / 2, (m0.y + m1.y) / 2, way.roadClass);
                counters.named += edge.name != 0 ? 1 : 0;
                edges.push(edge);
            }
            start = point;
            length = 0.0;
        }
    }
    return !edges.failed() && !shapes.failed();
}

uint32_t GraphBuilder::internName(const char* text, size_t length) {
    // Offset + 1 of the name in the name bytes; the same name once
    const uint8_t* bytes = names.bytes();
    size_t at = 0;
    while (at < names.size()) {
        const size_t stored = bytes[at];
        if (stored == length && memcmp(bytes + at + 1, text, length) == 0) {
            return static_cast<uint32_t>(at + 1);
        }
        at += 1 + stored;
    }
    if (names.size() + 1 + length >= 65535) {
        return 0;
    }
    const auto offset = static_cast<uint32_t>(names.size());
    names.put(static_cast<uint8_t>(length));
    names.putBytes(text, length);
    return names.failed() ? 0 : offset + 1;
}

bool GraphBuilder::buildNameGrid() {
    // The street name segments by grid cell, like the snap grid: an edge looks only at the cells around its middle
    const size_t cellCount = static_cast<size_t>(GRID_SIZE) * GRID_SIZE;
    cellStart.clear();
    cellItems.clear();
    if (!cellStart.resize(cellCount + 1) || !namePointLine.resize(namePoints.size())) {
        return false;
    }
    for (size_t i = 0; i < nameLines.size(); i++) {
        for (uint32_t j = 0; j < nameLines[i].count; j++) {
            namePointLine[nameLines[i].first + j] = static_cast<uint32_t>(i);
        }
    }
    auto forCells = [&](uint32_t segment, auto visit) {
        const Point a = namePoints[segment];
        const Point b = namePoints[segment + 1];
        const int left = std::clamp(std::min<int>(a.x, b.x) / GRID_CELL, 0, GRID_SIZE - 1);
        const int right = std::clamp(std::max<int>(a.x, b.x) / GRID_CELL, 0, GRID_SIZE - 1);
        const int top = std::clamp(std::min<int>(a.y, b.y) / GRID_CELL, 0, GRID_SIZE - 1);
        const int bottom = std::clamp(std::max<int>(a.y, b.y) / GRID_CELL, 0, GRID_SIZE - 1);
        for (int cy = top; cy <= bottom; cy++) {
            for (int cx = left; cx <= right; cx++) {
                visit(static_cast<size_t>(cy) * GRID_SIZE + cx);
            }
        }
    };
    for (const NameLine& line : nameLines) {
        for (uint32_t j = 0; j + 1 < line.count; j++) {
            forCells(line.first + j, [&](size_t cell) { cellStart[cell + 1]++; });
        }
    }
    for (size_t i = 1; i <= cellCount; i++) {
        cellStart[i] += cellStart[i - 1];
    }
    GrowArray<uint32_t> fill;
    if (!cellItems.resize(cellStart[cellCount]) || !fill.resize(cellCount)) {
        return false;
    }
    for (const NameLine& line : nameLines) {
        for (uint32_t j = 0; j + 1 < line.count; j++) {
            const uint32_t segment = line.first + j;
            forCells(segment, [&](size_t cell) { cellItems[cellStart[cell] + fill[cell]++] = segment; });
        }
    }
    return true;
}

uint16_t GraphBuilder::nameNear(int32_t x, int32_t y, uint8_t roadClass) {
    // The nearest street name line; a line of the same class wins against one of another class up to 2 units nearer
    float best = static_cast<float>(GRAPH_NAME_UNITS) + 1.0f;
    uint32_t bestName = 0;
    if (cellStart.size() == 0) {
        return 0;
    }
    const int left = std::clamp(static_cast<int>(x - GRAPH_NAME_UNITS) / GRID_CELL, 0, GRID_SIZE - 1);
    const int right = std::clamp(static_cast<int>(x + GRAPH_NAME_UNITS) / GRID_CELL, 0, GRID_SIZE - 1);
    const int top = std::clamp(static_cast<int>(y - GRAPH_NAME_UNITS) / GRID_CELL, 0, GRID_SIZE - 1);
    const int bottom = std::clamp(static_cast<int>(y + GRAPH_NAME_UNITS) / GRID_CELL, 0, GRID_SIZE - 1);
    for (int cy = top; cy <= bottom; cy++) {
        for (int cx = left; cx <= right; cx++) {
            const size_t cell = static_cast<size_t>(cy) * GRID_SIZE + cx;
            for (uint32_t k = cellStart[cell]; k < cellStart[cell + 1]; k++) {
                const uint32_t segment = cellItems[k];
                const NameLine& line = nameLines[namePointLine[segment]];
                const Point a = namePoints[segment];
                const Point b = namePoints[segment + 1];
                const float dx = static_cast<float>(b.x - a.x);
                const float dy = static_cast<float>(b.y - a.y);
                const float lengthSquared = dx * dx + dy * dy;
                float t = lengthSquared > 0.0f ? ((x - a.x) * dx + (y - a.y) * dy) / lengthSquared : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                const float distance = distanceOf(x - (a.x + t * dx), y - (a.y + t * dy));
                const float score = distance - (line.roadClass == roadClass ? 2.0f : 0.0f);
                if (distance <= GRAPH_NAME_UNITS && score < best) {
                    best = score;
                    bestName = line.name;
                }
            }
        }
    }
    return static_cast<uint16_t>(bestName);
}

bool GraphBuilder::build(MvtDecoder& decoder, ByteInput* input, const TileKey& sourceTile, uint32_t sourceSize, const char* sourceVersion,
    bool deflate, ByteWriter& out, GraphStats* stats) {
    const uint32_t start = nowMs();
    tile = sourceTile;
    counters = {};
    failureText = "";
    ways.clear();
    wayPoints.clear();
    nameLines.clear();
    namePoints.clear();
    names.clear();
    cellStart.clear();
    cellItems.clear();
    if (!decoder.scan(input, false)) {
        failureText = "reading the tile";
        return false;
    }
    decoderExtent = decoder.extent(Layer::Transportation);
    if (decoderExtent <= 0) {
        decoderExtent = GRAPH_EXTENT;
    }
    const int nameExtent = decoder.extent(Layer::TransportationName);
    const Layer layers[2] = { Layer::Transportation, Layer::TransportationName };
    // The name lines need their own scale: set per feature from the layer
    nameScale = nameExtent > 0 ? static_cast<float>(GRAPH_EXTENT) / static_cast<float>(nameExtent) : 1.0f;
    if (!decoder.sweep(layers, 2, *this)) {
        failureText = "decoding the roads";
        return false;
    }
    if (ways.failed() || wayPoints.failed() || namePoints.failed() || nameLines.failed() || names.failed()) {
        failureText = "no memory for the roads";
        return false;
    }
    if (!indexPoints()) {
        failureText = "no memory for the junctions";
        return false;
    }
    snapJunctions();
    // Each working array goes as soon as it is not needed: a dense city tile left too little PSRAM for the rest
    cellStart.release();
    cellItems.release();
    if (!makeNodes()) {
        failureText = "too many junctions, or no memory for them";
        return false;
    }
    vertices.release();
    shared.release();
    pointWay.release();
    if (!buildNameGrid()) {
        failureText = "no memory for the street names";
        return false;
    }
    if (!makeEdges()) {
        failureText = "no memory for the edges";
        return false;
    }
    nodeOf.release();
    cellStart.release();
    cellItems.release();
    namePointLine.release();
    ways.release();
    wayPoints.release();
    nameLines.release();
    namePoints.release();
    ring.release();
    clipped.release();
    for (const GraphNode& node : nodes) {
        counters.borderNodes += node.x == 0 || node.y == 0 || node.x == GRAPH_EXTENT || node.y == GRAPH_EXTENT ? 1 : 0;
    }
    counters.nodes = static_cast<uint32_t>(nodes.size());
    counters.edges = static_cast<uint32_t>(edges.size());
    if (!write(sourceVersion, sourceSize, deflate, out)) {
        return false;
    }
    if (stats != nullptr) {
        *stats = counters;
        stats->totalMs = nowMs() - start;
    }
    return true;
}

bool GraphBuilder::write(const char* sourceVersion, uint32_t sourceSize, bool deflate, ByteWriter& out) {
    const size_t bodySize = nodes.size() * GRAPH_NODE_BYTES + edges.size() * GRAPH_EDGE_BYTES + shapes.size() + names.size();
    out.clear();
    out.putBytes(MAGIC, 4);
    out.putU16(GRAPH_FORMAT_VERSION);
    out.putU16(deflate ? PREPARED_FLAG_DEFLATED : 0);
    out.put(static_cast<uint8_t>(tile.z));
    out.put(0);
    out.putU16(GRAPH_EXTENT);
    out.putU32(static_cast<uint32_t>(tile.x));
    out.putU32(static_cast<uint32_t>(tile.y));
    out.putU32(sourceSize);
    out.putU32(static_cast<uint32_t>(nodes.size()));
    out.putU32(static_cast<uint32_t>(edges.size()));
    out.putU32(static_cast<uint32_t>(shapes.size()));
    out.putU32(static_cast<uint32_t>(names.size()));
    out.putU32(static_cast<uint32_t>(bodySize));
    out.putU32(0);
    char version[PREPARED_VERSION_LENGTH] = {};
    if (sourceVersion != nullptr) {
        const size_t length = strlen(sourceVersion);
        memcpy(version, sourceVersion, length < PREPARED_VERSION_LENGTH ? length : PREPARED_VERSION_LENGTH);
    }
    out.putBytes(version, PREPARED_VERSION_LENGTH);
    // The body in one buffer first: small next to the tile (a dense city tile about 100 KB)
    ByteWriter body;
    if (!body.reserve(bodySize)) {
        failureText = "no memory for the graph file";
        return false;
    }
    for (const GraphNode& node : nodes) {
        body.putU16(node.x);
        body.putU16(node.y);
        body.put(node.level);
    }
    for (const GraphEdge& edge : edges) {
        body.putU16(edge.from);
        body.putU16(edge.to);
        body.putU16(edge.lengthDm);
        body.put(edge.roadClass);
        body.put(edge.flags);
        body.putU16(edge.name);
        body.putU32(edge.shapeOffset);
        body.putU16(edge.shapeCount);
    }
    body.putBytes(shapes.bytes(), shapes.size());
    body.putBytes(names.bytes(), names.size());
    if (body.failed()) {
        failureText = "no memory for the graph file";
        return false;
    }
    // The compressor needs a 320 KB block: the rest is in the body now
    edges.release();
    shapes.release();
    names.release();
    if (!deflate) {
        out.putBytes(body.bytes(), body.size());
    } else {
        auto* compressor = static_cast<tdefl_compressor*>(largeAlloc(sizeof(tdefl_compressor)));
        bool ok = compressor != nullptr && tdefl_init(compressor, deflateCallback, &out, 128 | TDEFL_WRITE_ZLIB_HEADER) == TDEFL_STATUS_OKAY &&
            tdefl_compress_buffer(compressor, body.bytes(), body.size(), TDEFL_FINISH) == TDEFL_STATUS_DONE;
        largeFree(compressor);
        if (!ok) {
            failureText = "no 320 KB block of memory for the compressor";
            return false;
        }
    }
    if (out.failed()) {
        failureText = "no memory for the graph file";
        return false;
    }
    out.setU32(44, static_cast<uint32_t>(out.size() - GRAPH_HEADER_SIZE));
    counters.bodyBytes = static_cast<uint32_t>(bodySize);
    counters.storedBytes = static_cast<uint32_t>(out.size() - GRAPH_HEADER_SIZE);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------

void TileGraph::clear() {
    nodes.clear();
    edges.clear();
    firstAdjacent.clear();
    adjacent.clear();
    shapes.clear();
    names.clear();
    for (auto& side : border) {
        side.clear();
    }
}

size_t TileGraph::bytes() const {
    size_t total = nodes.allocated() + edges.allocated() + firstAdjacent.allocated() + adjacent.allocated() + shapes.allocated() + names.allocated();
    for (const auto& side : border) {
        total += side.allocated();
    }
    return total;
}

const char* TileGraph::name(uint32_t edge, uint8_t& length) const {
    const uint16_t offset = edges[edge].name;
    if (offset == 0 || offset > names.size()) {
        length = 0;
        return "";
    }
    length = names[offset - 1];
    if (offset + length > names.size()) {
        length = 0;
        return "";
    }
    return reinterpret_cast<const char*>(names.begin() + offset);
}

bool loadTileGraph(const uint8_t* data, size_t size, InflateReader& inflater, TileGraph& graph) {
    GraphHeader header {};
    graph.clear();
    if (!readGraphHeader(data, size, header) || header.version != GRAPH_FORMAT_VERSION || GRAPH_HEADER_SIZE + header.storedSize > size ||
        header.nodeCount > 65535) {
        return false;
    }
    const size_t expected = header.nodeCount * GRAPH_NODE_BYTES + header.edgeCount * GRAPH_EDGE_BYTES + header.shapeBytes + header.nameBytes;
    if (expected != header.bodySize) {
        return false;
    }
    const uint8_t* body = data + GRAPH_HEADER_SIZE;
    GrowArray<uint8_t> inflated;
    if ((header.flags & PREPARED_FLAG_DEFLATED) != 0) {
        if (!inflated.resize(header.bodySize)) {
            return false;
        }
        MemoryInput stored(body, header.storedSize);
        if (!inflater.begin(&stored) || inflater.read(inflated.begin(), header.bodySize) != header.bodySize || inflater.failed()) {
            return false;
        }
        body = inflated.begin();
    } else if (header.storedSize != header.bodySize) {
        return false;
    }
    graph.tile = header.tile;
    if (!graph.nodes.resize(header.nodeCount) || !graph.edges.resize(header.edgeCount) || !graph.shapes.resize(header.shapeBytes) ||
        !graph.names.resize(header.nameBytes) || !graph.firstAdjacent.resize(header.nodeCount + 1)) {
        return false;
    }
    const uint8_t* cursor = body;
    for (uint32_t i = 0; i < header.nodeCount; i++, cursor += GRAPH_NODE_BYTES) {
        graph.nodes[i] = { readU16(cursor), readU16(cursor + 2), cursor[4] };
    }
    for (uint32_t i = 0; i < header.edgeCount; i++, cursor += GRAPH_EDGE_BYTES) {
        GraphEdge& edge = graph.edges[i];
        edge.from = readU16(cursor);
        edge.to = readU16(cursor + 2);
        edge.lengthDm = readU16(cursor + 4);
        edge.roadClass = cursor[6];
        edge.flags = cursor[7];
        edge.name = readU16(cursor + 8);
        edge.shapeOffset = readU32(cursor + 10);
        edge.shapeCount = readU16(cursor + 14);
        if (edge.from >= header.nodeCount || edge.to >= header.nodeCount || edge.shapeOffset > header.shapeBytes) {
            graph.clear();
            return false;
        }
    }
    if (header.shapeBytes > 0) {
        memcpy(graph.shapes.begin(), cursor, header.shapeBytes);
    }
    cursor += header.shapeBytes;
    if (header.nameBytes > 0) {
        memcpy(graph.names.begin(), cursor, header.nameBytes);
    }
    // Adjacency: every edge from its from-node, and from its to-node backwards (a oneway edge too: the profile decides)
    for (uint32_t i = 0; i < header.edgeCount; i++) {
        graph.firstAdjacent[graph.edges[i].from + 1]++;
        graph.firstAdjacent[graph.edges[i].to + 1]++;
    }
    for (uint32_t i = 1; i <= header.nodeCount; i++) {
        graph.firstAdjacent[i] += graph.firstAdjacent[i - 1];
    }
    if (!graph.adjacent.resize(static_cast<size_t>(header.edgeCount) * 2)) {
        graph.clear();
        return false;
    }
    {
        GrowArray<uint32_t> fill;
        if (!fill.resize(header.nodeCount)) {
            graph.clear();
            return false;
        }
        for (uint32_t i = 0; i < header.edgeCount; i++) {
            const GraphEdge& edge = graph.edges[i];
            graph.adjacent[graph.firstAdjacent[edge.from] + fill[edge.from]++] = i << 1;
            graph.adjacent[graph.firstAdjacent[edge.to] + fill[edge.to]++] = (i << 1) | 1;
        }
    }
    // Border nodes per side, sorted along it
    for (uint32_t i = 0; i < header.nodeCount; i++) {
        const GraphNode& node = graph.nodes[i];
        if (node.x == 0) {
            graph.border[0].push(i);
        }
        if (node.y == 0) {
            graph.border[1].push(i);
        }
        if (node.x == GRAPH_EXTENT) {
            graph.border[2].push(i);
        }
        if (node.y == GRAPH_EXTENT) {
            graph.border[3].push(i);
        }
    }
    for (int side = 0; side < 4; side++) {
        const bool vertical = side == 0 || side == 2;
        std::sort(graph.border[side].begin(), graph.border[side].end(), [&](uint32_t a, uint32_t b) {
            return vertical ? graph.nodes[a].y < graph.nodes[b].y : graph.nodes[a].x < graph.nodes[b].x;
        });
    }
    return !graph.adjacent.failed();
}

} // namespace maps
