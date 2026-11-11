// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/MvtDecoder.h"
#include "../render/PreparedTile.h"
#include "../tiles/TileMath.h"
#include "GrowArray.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * The routing graph of one z14 tile, stored as <y>.prg next to the original (M5c-1). It is made on the device from the
 * tile's `transportation` layer, with the rules of the PC study (docs/M5c-routing-study.md, host/routing/build_graph.py):
 *  - lines are clipped to the tile; a clipped segment lying on the x = 4096 or y = 4096 side belongs to the neighbour;
 *  - classes motorway ... path are kept, `*_construction` and rail, ferry, transit, pier, aerialway, raceway are not;
 *  - a dead-end way end within GRAPH_SNAP_UNITS of a segment of another way on the same level is moved onto it, and
 *    the foot point becomes a shared vertex (simplified tiles often lose the junction vertex of the through road);
 *  - a vertex is a node where two or more way vertices share the coordinate, or where a way ends; vertices at one
 *    coordinate join when they have the same level (brunnel, layer), except that a way end joins all of them;
 *  - edges run from node to node with their shape points, length in decimetres, class, oneway and level flags, and the
 *    street name found next to them in `transportation_name`.
 * Way ends on the tile's sides are border nodes: when the search loads a neighbour, border nodes on the shared side
 * within GRAPH_JOIN_UNITS of each other and on the same level are the same point (RouteSearch).
 *
 * Layout (little endian): an 80-byte header, then the body, raw or as one zlib stream:
 *   header: "TMPG", u16 version, u16 flags (1 deflated), u8 z, u8 0, u16 extent, u32 x, u32 y, u32 source size,
 *           u32 node count, u32 edge count, u32 shape bytes, u32 name bytes, u32 body size, u32 stored body size,
 *           char source version[32]
 *   body:   nodes (u16 x, u16 y, u8 level), edges (GRAPH_EDGE_BYTES each: u16 from, u16 to, u16 length in dm,
 *           u8 class, u8 flags, u16 name (byte offset + 1 into the names, 0 for none), u32 shape offset, u16 shape
 *           point count), shapes (zigzag varint deltas x, y of the points between the ends, from the from-node),
 *           names (u8 length, UTF-8 bytes, each)
 * A oneway road against its drawing direction (oneway = -1) is stored reversed, so from -> to is always the way to go.
 */
constexpr uint16_t GRAPH_FORMAT_VERSION = PREPARED_GRAPH_FORMAT_VERSION;
constexpr int GRAPH_EXTENT = 4096;
constexpr size_t GRAPH_HEADER_SIZE = 80;
constexpr size_t GRAPH_NODE_BYTES = 5;
constexpr size_t GRAPH_EDGE_BYTES = 16;
constexpr int GRAPH_SNAP_UNITS = 2;
constexpr int GRAPH_JOIN_UNITS = 1;
/** An edge's name is the nearest street name line within this many units of its middle. */
constexpr int GRAPH_NAME_UNITS = 12;

enum class RoadClass : uint8_t { Motorway, Trunk, Primary, Secondary, Tertiary, Minor, Service, Track, Path, Count };

enum GraphEdgeFlag : uint8_t {
    EDGE_ONEWAY = 1,
    EDGE_BRIDGE = 2,
    EDGE_TUNNEL = 4,
    EDGE_RAMP = 8,
    /** Path subclasses, for later use by the profiles. */
    EDGE_STEPS = 16,
    EDGE_FOOTWAY = 32,
    EDGE_CYCLEWAY = 64,
};

/** The road class of a transportation feature, Count for features that are not roads to route on. */
RoadClass roadClassOf(Value featureClass);
const char* roadClassName(RoadClass roadClass);

struct GraphHeader {
    uint16_t version;
    uint16_t flags;
    TileKey tile;
    int extent;
    uint32_t sourceSize;
    uint32_t nodeCount;
    uint32_t edgeCount;
    uint32_t shapeBytes;
    uint32_t nameBytes;
    uint32_t bodySize;
    uint32_t storedSize;
    char sourceVersion[PREPARED_VERSION_LENGTH + 1];
};

bool readGraphHeader(const uint8_t* data, size_t size, GraphHeader& header);
bool isGraphCurrent(const GraphHeader& header, const TileKey& tile, uint32_t sourceSize);

struct GraphNode {
    uint16_t x;
    uint16_t y;
    /** brunnel << 4 | (layer + 8): bridge 1, tunnel 2. */
    uint8_t level;
};

struct GraphEdge {
    uint16_t from;
    uint16_t to;
    uint16_t lengthDm;
    uint8_t roadClass;
    uint8_t flags;
    uint16_t name;
    uint32_t shapeOffset;
    uint16_t shapeCount;
};

struct GraphStats {
    uint32_t totalMs;
    uint32_t ways;
    uint32_t points;
    uint32_t snappedToVertex;
    uint32_t snappedToSegment;
    uint32_t nodes;
    uint32_t edges;
    uint32_t named;
    uint32_t borderNodes;
    uint32_t bodyBytes;
    uint32_t storedBytes;
};

/**
 * Builds the graph file of a tile from its vector tile, with the decoder of a render lane. Its working memory (a dense
 * city tile: a few hundred KB) is kept between tiles until trim().
 */
class GraphBuilder final : private GeometrySink {
public:
    GraphBuilder() = default;
    GraphBuilder(const GraphBuilder&) = delete;
    GraphBuilder& operator=(const GraphBuilder&) = delete;

    bool build(MvtDecoder& decoder, ByteInput* input, const TileKey& tile, uint32_t sourceSize, const char* sourceVersion, bool deflate,
        ByteWriter& out, GraphStats* stats);
    const char* failure() const { return failureText; }
    void trim();

private:
    struct Point {
        int16_t x;
        int16_t y;
    };
    struct Way {
        uint32_t first;
        uint32_t count;
        uint8_t roadClass;
        uint8_t flags;
        int8_t oneway;
        uint8_t level;
    };
    struct NameLine {
        uint32_t first;
        uint32_t count;
        uint32_t name;
        uint8_t roadClass;
    };
    struct Vertex {
        uint32_t key;
        uint32_t point;
    };

    bool beginFeature(const FeatureInfo& info) override;
    void beginRing() override;
    void points(const TilePoint* points, int count) override;
    void endRing(bool closed) override;
    void endFeature() override {}

    void clipAndAdd();
    void addPart(const Point* points, size_t count);
    bool indexPoints();
    void snapJunctions();
    bool makeNodes();
    bool makeEdges();
    bool buildNameGrid();
    uint16_t nameNear(int32_t x, int32_t y, uint8_t roadClass);
    uint32_t internName(const char* text, size_t length);
    bool write(const char* sourceVersion, uint32_t sourceSize, bool deflate, ByteWriter& out);

    const char* failureText = "";
    TileKey tile {};
    int extent = GRAPH_EXTENT;
    float scale = 1.0f;
    // Current feature
    bool inNames = false;
    Way current {};
    uint32_t currentName = 0;
    GrowArray<Point> ring;
    GrowArray<Point> clipped;
    int decoderExtent = GRAPH_EXTENT;
    float nameScale = 1.0f;
    // Ways and their points
    GrowArray<Way> ways;
    GrowArray<Point> wayPoints;
    GrowArray<uint32_t> pointWay;
    GrowArray<Vertex> vertices;
    /** Per point: how many way vertices share its coordinate. */
    GrowArray<uint16_t> shared;
    /** Per point: its node, or UINT32_MAX. */
    GrowArray<uint32_t> nodeOf;
    GrowArray<GraphNode> nodes;
    GrowArray<GraphEdge> edges;
    ByteWriter shapes;
    ByteWriter names;
    // Street name lines and a grid of their segments
    GrowArray<NameLine> nameLines;
    GrowArray<Point> namePoints;
    GrowArray<uint32_t> namePointLine;
    GrowArray<uint32_t> cellStart;
    GrowArray<uint32_t> cellItems;
    GraphStats counters {};
};

/** A graph file read into memory, with the adjacency the search needs. */
struct TileGraph {
    TileKey tile {};
    GrowArray<GraphNode> nodes;
    GrowArray<GraphEdge> edges;
    /** Per node: its first entry in adjacent, nodeCount + 1 entries. */
    GrowArray<uint32_t> firstAdjacent;
    /** Edge index << 1 | 1 when the edge is used from its to-node (backwards). */
    GrowArray<uint32_t> adjacent;
    GrowArray<uint8_t> shapes;
    GrowArray<uint8_t> names;
    /** Border nodes per side (0 left x = 0, 1 top y = 0, 2 right x = 4096, 3 bottom y = 4096), sorted by position. */
    GrowArray<uint32_t> border[4];

    void clear();
    size_t bytes() const;
    /** The name of an edge, empty when it has none. */
    const char* name(uint32_t edge, uint8_t& length) const;
    /** Calls point(x, y) for the shape of an edge from from to to (or back), in tile units, ends included. */
    template <typename Point>
    void shape(uint32_t edge, bool backwards, Point point) const;
};

/** Parses a .prg file (inflating with inflater when deflated) into graph. */
bool loadTileGraph(const uint8_t* data, size_t size, InflateReader& inflater, TileGraph& graph);

// ---------------------------------------------------------------------------------------------------------------

namespace graph_detail {
inline bool readVarint(const uint8_t*& data, const uint8_t* end, uint32_t& value) {
    value = 0;
    for (int shift = 0; shift < 35 && data < end; shift += 7) {
        const uint8_t byte = *data++;
        value |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}
inline int32_t unzigzag(uint32_t value) {
    return static_cast<int32_t>((value >> 1) ^ (~(value & 1) + 1));
}
} // namespace graph_detail

template <typename Point>
void TileGraph::shape(uint32_t edge, bool backwards, Point point) const {
    const GraphEdge& e = edges[edge];
    const GraphNode& a = nodes[e.from];
    const GraphNode& b = nodes[e.to];
    // 2 KB of stack; longer shapes are cut (the builder splits longer edges)
    constexpr int MAX = 256;
    int32_t xs[MAX];
    int32_t ys[MAX];
    int count = 0;
    xs[count] = a.x;
    ys[count] = a.y;
    count++;
    const uint8_t* cursor = shapes.begin() + e.shapeOffset;
    const uint8_t* end = shapes.end();
    int32_t x = a.x;
    int32_t y = a.y;
    for (int i = 0; i < e.shapeCount && count < MAX - 1; i++) {
        uint32_t dx;
        uint32_t dy;
        if (!graph_detail::readVarint(cursor, end, dx) || !graph_detail::readVarint(cursor, end, dy)) {
            break;
        }
        x += graph_detail::unzigzag(dx);
        y += graph_detail::unzigzag(dy);
        xs[count] = x;
        ys[count] = y;
        count++;
    }
    xs[count] = b.x;
    ys[count] = b.y;
    count++;
    for (int i = 0; i < count; i++) {
        const int j = backwards ? count - 1 - i : i;
        point(xs[j], ys[j]);
    }
}

} // namespace maps
