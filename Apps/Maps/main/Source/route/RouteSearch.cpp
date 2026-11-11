// SPDX-License-Identifier: GPL-3.0-or-later
#include "RouteSearch.h"

#include "../platform/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace maps {

namespace {

constexpr uint32_t EMPTY = 0xFFFFFFFFu;
constexpr uint32_t NO_PARENT = 0xFFFFFFFEu;
constexpr int MAX_TILES = 160;
/** A road piece of fewer nodes that touches no tile side is an island, never snapped to (see isIsland()). */
constexpr int ISLAND_NODES = 40;
/** A start or goal whose roads reach fewer nodes than this, over tile sides too, moves to a road outside them. */
constexpr uint32_t COMPONENT_NODES = 300;
constexpr int SNAP_ATTEMPTS = 5;
/** How much more the estimate counts in the second pass (see find()). */
constexpr float RETRY_WEIGHT = 1.6f;
constexpr double PI_D = 3.14159265358979323846;
constexpr double EARTH_CIRCUMFERENCE = 40075016.686;
constexpr double WORLD_UNITS = 16384.0 * GRAPH_EXTENT;
constexpr uint8_t VIA_BACKWARDS = 1;
constexpr uint8_t VIA_JOIN = 2;
constexpr uint8_t VIA_START = 4;

uint16_t readU16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t readU32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

uint32_t keyOf(int slot, uint32_t node) {
    return (static_cast<uint32_t>(slot) << 16) | node;
}

int slotOf(uint32_t key) {
    return static_cast<int>(key >> 16);
}

uint16_t nodeOfKey(uint32_t key) {
    return static_cast<uint16_t>(key & 0xFFFF);
}

uint32_t hashOf(uint32_t key) {
    return key * 2654435761u;
}

float lengthOf(int32_t dx, int32_t dy) {
    return std::sqrt(static_cast<float>(dx) * dx + static_cast<float>(dy) * dy);
}

/** The shape of an edge as points with their distance in units from its from-node, into fixed arrays. */
struct ShapeBuffer {
    static constexpr int MAX = 260;
    int32_t x[MAX];
    int32_t y[MAX];
    float at[MAX];
    int count = 0;

    void load(const TileGraph& graph, uint32_t edge) {
        count = 0;
        graph.shape(edge, false, [&](int32_t px, int32_t py) {
            if (count < MAX) {
                x[count] = px;
                y[count] = py;
                at[count] = count == 0 ? 0.0f : at[count - 1] + lengthOf(px - x[count - 1], py - y[count - 1]);
                count++;
            }
        });
    }
    float total() const { return count > 0 ? at[count - 1] : 0.0f; }
    /** The point at distance d along the shape. */
    void pointAt(float d, int32_t& px, int32_t& py) const {
        for (int i = 1; i < count; i++) {
            if (at[i] >= d || i == count - 1) {
                const float span = at[i] - at[i - 1];
                const float t = span > 0.0f ? std::clamp((d - at[i - 1]) / span, 0.0f, 1.0f) : 0.0f;
                px = static_cast<int32_t>(std::floor(x[i - 1] + (x[i] - x[i - 1]) * t + 0.5f));
                py = static_cast<int32_t>(std::floor(y[i - 1] + (y[i] - y[i - 1]) * t + 0.5f));
                return;
            }
        }
        px = count > 0 ? x[0] : 0;
        py = count > 0 ? y[0] : 0;
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------

RoutePoint routePointFrom(double lat, double lon) {
    const double x = (lon + 180.0) / 360.0 * WORLD_UNITS;
    const double s = std::sin(lat * PI_D / 180.0);
    const double y = (0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * PI_D)) * WORLD_UNITS;
    return { static_cast<int32_t>(std::floor(x + 0.5)), static_cast<int32_t>(std::floor(y + 0.5)) };
}

void routePointTo(const RoutePoint& point, double& lat, double& lon) {
    lon = point.x / WORLD_UNITS * 360.0 - 180.0;
    lat = std::atan(std::sinh(PI_D * (1.0 - 2.0 * point.y / WORLD_UNITS))) * 180.0 / PI_D;
}

double routeMetresPerUnit(int32_t y) {
    const double latitude = std::atan(std::sinh(PI_D * (1.0 - 2.0 * y / WORLD_UNITS)));
    return EARTH_CIRCUMFERENCE / WORLD_UNITS * std::cos(latitude);
}

const char* routeProfileName(RouteProfile profile) {
    switch (profile) {
        case RouteProfile::Bike:
            return "Bike";
        case RouteProfile::Car:
            return "Car";
        default:
            return "Walk";
    }
}

float routeMaxKmh(RouteProfile profile) {
    switch (profile) {
        case RouteProfile::Bike:
            return BIKE_KMH;
        case RouteProfile::Car:
            // Not the motorway speed: with it the estimate is so weak in towns that the search reads most of the region.
            // Routes may come out a little slower than the best one, never through roads the profile cannot use
            return CAR_ESTIMATE_KMH;
        default:
            return WALK_KMH;
    }
}

float routeSeconds(RouteProfile profile, uint8_t roadClass, float lengthM) {
    float kmh = WALK_KMH;
    if (profile == RouteProfile::Bike) {
        kmh = BIKE_KMH;
    } else if (profile == RouteProfile::Car) {
        kmh = CAR_KMH[std::min<int>(roadClass, static_cast<int>(RoadClass::Count) - 1)];
    }
    return lengthM * 3.6f / kmh;
}

const char* routeResultText(RouteResult result) {
    switch (result) {
        case RouteResult::Found:
            return "Route found";
        case RouteResult::NoRoadNearStart:
            return "No road near the start in the downloaded map";
        case RouteResult::NoRoadNearGoal:
            return "No road near the destination in the downloaded map";
        case RouteResult::NoPath:
            return "No route found in the downloaded map";
        case RouteResult::TooLong:
            return "Route too long for this device; try a shorter one";
        case RouteResult::NoMemory:
            return "Not enough memory for a route now; try again";
        case RouteResult::Cancelled:
            return "Route search stopped";
    }
    return "";
}

void Route::clear() {
    points.clear();
    distances.clear();
    steps.clear();
    names.clear();
    lengthM = 0.0f;
    seconds = 0.0f;
}

const char* Route::stepName(const RouteStep& step, char* out, size_t size) const {
    const size_t length = std::min<size_t>(step.nameLength, size - 1);
    if (step.nameLength == 0 || step.nameOffset + step.nameLength > names.size()) {
        out[0] = '\0';
        return out;
    }
    memcpy(out, names.begin() + step.nameOffset, length);
    out[length] = '\0';
    return out;
}

// ---------------------------------------------------------------------------------------------------------------

struct RouteSearch::Tile {
    TileKey key;
    bool missing;
    uint32_t nodeCount;
    uint32_t edgeCount;
    uint8_t* block;
    size_t bytes;
    uint16_t* nodeX;
    uint16_t* nodeY;
    uint8_t* level;
    uint32_t* first;
    uint16_t* adjacent;
    uint16_t* from;
    uint16_t* to;
    uint16_t* length;
    uint8_t* roadClass;
    uint8_t* flags;
    uint16_t* border[4];
    uint32_t borderCount[4];
};

RouteSearch::~RouteSearch() {
    end();
}

bool RouteSearch::begin(size_t memoryBytes) {
    end();
    // Node states take a quarter of the budget, the queue half as much, the tiles' graphs the rest: on the T-Deck
    // the graphs ran out first with 40 % for the states (device bench, 11 Nov 2026). Any size, not a power of two:
    // rounding down halved the states with 1.8 MB, and car routes of 2 km became "too long" (user, 11 Nov 2026)
    const uint32_t capacity = static_cast<uint32_t>(std::clamp<size_t>(memoryBytes / 4 / sizeof(Entry), 4096, 1u << 20));
    table = static_cast<Entry*>(largeAlloc(static_cast<size_t>(capacity) * sizeof(Entry)));
    queueCapacity = capacity;
    queue = static_cast<QueueItem*>(largeAlloc(static_cast<size_t>(queueCapacity) * sizeof(QueueItem)));
    tiles = static_cast<Tile*>(largeAlloc(sizeof(Tile) * MAX_TILES));
    if (table == nullptr || queue == nullptr || tiles == nullptr) {
        end();
        return false;
    }
    tableCapacity = capacity;
    maxNodes = capacity / 4 * 3;
    const size_t fixed = static_cast<size_t>(capacity) * sizeof(Entry) + static_cast<size_t>(queueCapacity) * sizeof(QueueItem) + sizeof(Tile) * MAX_TILES;
    budget = memoryBytes > fixed ? memoryBytes - fixed : 0;
    tileCount = 0;
    if (!inflaterReady) {
        inflaterReady = inflater.init();
    }
    return inflaterReady;
}

void RouteSearch::release() {
    for (int i = 0; i < tileCount; i++) {
        largeFree(tiles[i].block);
    }
    tileCount = 0;
    graphBytes = 0;
}

void RouteSearch::end() {
    if (tiles != nullptr) {
        release();
    }
    largeFree(table);
    largeFree(queue);
    largeFree(tiles);
    table = nullptr;
    queue = nullptr;
    tiles = nullptr;
    fileBuffer.release();
    bodyBuffer.release();
    maxNodes = 0;
}

bool RouteSearch::loadTile(GraphSource& source, const TileKey& tile, Tile& out) {
    out = {};
    out.key = tile;
    out.missing = true;
    const uint32_t start = nowMs();
    const bool read = source.readGraph(tile, fileBuffer);
    loadMs += nowMs() - start;
    GraphHeader header {};
    if (!read || !readGraphHeader(fileBuffer.begin(), fileBuffer.size(), header) || header.version != GRAPH_FORMAT_VERSION ||
        !(header.tile == tile) || GRAPH_HEADER_SIZE + header.storedSize > fileBuffer.size() || header.nodeCount > 65535 ||
        header.edgeCount >= 32768 ||
        header.bodySize != header.nodeCount * GRAPH_NODE_BYTES + header.edgeCount * GRAPH_EDGE_BYTES + header.shapeBytes + header.nameBytes) {
        if (read) {
            logError("Route: graph of %d/%d/%d not usable (%u bytes, version %u)", tile.z, tile.x, tile.y, static_cast<unsigned>(fileBuffer.size()),
                static_cast<unsigned>(header.version));
        }
        counters.tilesMissing++;
        return true;
    }
    const uint8_t* body = fileBuffer.begin() + GRAPH_HEADER_SIZE;
    if ((header.flags & PREPARED_FLAG_DEFLATED) != 0) {
        // Only nodes and edges are needed: the shapes and names are read again for the route that is found
        const size_t wanted = header.nodeCount * GRAPH_NODE_BYTES + header.edgeCount * GRAPH_EDGE_BYTES;
        if (!bodyBuffer.resize(wanted)) {
            return false;
        }
        MemoryInput input(body, header.storedSize);
        if (!inflater.begin(&input) || inflater.read(bodyBuffer.begin(), wanted) != wanted || inflater.failed()) {
            counters.tilesMissing++;
            return true;
        }
        body = bodyBuffer.begin();
    }
    const uint32_t n = header.nodeCount;
    const uint32_t e = header.edgeCount;
    // One block: x, y, first (u32, aligned first), adjacency, edge fields, levels, classes, flags, borders
    const size_t bytes = n * 4u + (n + 1) * 4u + e * 4u + e * 6u + n + e * 2u + n * 2u * 2u + 64;
    if (graphBytes + bytes > budget || source.memoryShort()) {
        return false;
    }
    auto* block = static_cast<uint8_t*>(largeAlloc(bytes));
    if (block == nullptr) {
        return false;
    }
    uint8_t* cursor = block;
    auto take = [&](size_t size) {
        uint8_t* at = cursor;
        cursor += (size + 3) & ~static_cast<size_t>(3);
        return at;
    };
    out.first = reinterpret_cast<uint32_t*>(take((n + 1) * 4u));
    out.nodeX = reinterpret_cast<uint16_t*>(take(n * 2u));
    out.nodeY = reinterpret_cast<uint16_t*>(take(n * 2u));
    out.adjacent = reinterpret_cast<uint16_t*>(take(e * 4u));
    out.from = reinterpret_cast<uint16_t*>(take(e * 2u));
    out.to = reinterpret_cast<uint16_t*>(take(e * 2u));
    out.length = reinterpret_cast<uint16_t*>(take(e * 2u));
    out.level = take(n);
    out.roadClass = take(e);
    out.flags = take(e);
    out.block = block;
    out.bytes = bytes;
    out.nodeCount = n;
    out.edgeCount = e;
    out.missing = false;
    memset(out.first, 0, (n + 1) * 4u);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* node = body + i * GRAPH_NODE_BYTES;
        out.nodeX[i] = readU16(node);
        out.nodeY[i] = readU16(node + 2);
        out.level[i] = node[4];
    }
    const uint8_t* edges = body + n * GRAPH_NODE_BYTES;
    for (uint32_t i = 0; i < e; i++) {
        const uint8_t* edge = edges + i * GRAPH_EDGE_BYTES;
        out.from[i] = readU16(edge);
        out.to[i] = readU16(edge + 2);
        out.length[i] = readU16(edge + 4);
        out.roadClass[i] = edge[6];
        out.flags[i] = edge[7];
        if (out.from[i] >= n || out.to[i] >= n) {
            largeFree(block);
            out = {};
            out.key = tile;
            out.missing = true;
            counters.tilesMissing++;
            return true;
        }
        out.first[out.from[i] + 1]++;
        out.first[out.to[i] + 1]++;
    }
    for (uint32_t i = 1; i <= n; i++) {
        out.first[i] += out.first[i - 1];
    }
    // The adjacency is filled from the end of each node's range, counting first[node + 1] down to the node's start
    for (uint32_t i = 0; i < e; i++) {
        out.adjacent[--out.first[out.from[i] + 1]] = static_cast<uint16_t>(i << 1);
        out.adjacent[--out.first[out.to[i] + 1]] = static_cast<uint16_t>((i << 1) | 1);
    }
    // first[i + 1] now points at the start of node i's entries: shift back
    for (uint32_t i = 0; i < n; i++) {
        out.first[i] = out.first[i + 1];
    }
    out.first[n] = e * 2;
    // Border nodes per side, sorted along it (side 0 x = 0, 1 y = 0, 2 x = 4096, 3 y = 4096)
    uint16_t* borders = reinterpret_cast<uint16_t*>(cursor);
    size_t borderUsed = 0;
    for (int side = 0; side < 4; side++) {
        out.border[side] = borders + borderUsed;
        out.borderCount[side] = 0;
        for (uint32_t i = 0; i < n && borderUsed < n * 2u; i++) {
            const bool on = (side == 0 && out.nodeX[i] == 0) || (side == 1 && out.nodeY[i] == 0) || (side == 2 && out.nodeX[i] == GRAPH_EXTENT) ||
                (side == 3 && out.nodeY[i] == GRAPH_EXTENT);
            if (on) {
                out.border[side][out.borderCount[side]++] = static_cast<uint16_t>(i);
                borderUsed++;
            }
        }
        const bool vertical = side == 0 || side == 2;
        std::sort(out.border[side], out.border[side] + out.borderCount[side],
            [&](uint16_t a, uint16_t b) { return vertical ? out.nodeY[a] < out.nodeY[b] : out.nodeX[a] < out.nodeX[b]; });
    }
    graphBytes += bytes;
    peakGraphBytes = std::max(peakGraphBytes, graphBytes);
    counters.tilesRead++;
    return true;
}

int RouteSearch::tileSlot(GraphSource& source, const TileKey& tile) {
    for (int i = 0; i < tileCount; i++) {
        if (tiles[i].key == tile) {
            return tiles[i].missing ? -1 : i;
        }
    }
    // Once the memory or the tile list is full no further tile is read: the search ends as too long
    if (tilesFull || tileCount >= MAX_TILES) {
        tilesFull = true;
        return -2;
    }
    if (!loadTile(source, tile, tiles[tileCount])) {
        // Over the budget: the tile counts as missing (never read again), the search goes on around it. Short of
        // memory on the device: the search ends
        if (source.memoryShort()) {
            tilesFull = true;
            return -2;
        }
        tiles[tileCount] = {};
        tiles[tileCount].key = tile;
        tiles[tileCount].missing = true;
        tileCount++;
        budgetHit = true;
        return -1;
    }
    tileCount++;
    return tiles[tileCount - 1].missing ? -1 : tileCount - 1;
}

void RouteSearch::resetStates() {
    memset(static_cast<void*>(table), 0xFF, static_cast<size_t>(tableCapacity) * sizeof(Entry));
    used = 0;
    maxNodes = tableCapacity / 4 * 3;
    queueSize = 0;
}

RouteSearch::Entry* RouteSearch::entry(uint32_t key, bool create) {
    uint32_t index = static_cast<uint32_t>((static_cast<uint64_t>(hashOf(key)) * tableCapacity) >> 32);
    while (true) {
        Entry& slot = table[index];
        if (slot.key == key) {
            return &slot;
        }
        if (slot.key == EMPTY) {
            if (!create || used >= maxNodes) {
                return nullptr;
            }
            used++;
            slot = { key, INFINITY, NO_PARENT, 0, 0, 0 };
            return &slot;
        }
        index = index + 1 < tableCapacity ? index + 1 : 0;
    }
}

bool RouteSearch::push(float f, uint32_t key) {
    if (queueSize >= queueCapacity) {
        return false;
    }
    uint32_t i = queueSize++;
    while (i > 0) {
        const uint32_t parent = (i - 1) / 2;
        if (queue[parent].f <= f) {
            break;
        }
        queue[i] = queue[parent];
        i = parent;
    }
    queue[i] = { f, key };
    return true;
}

bool RouteSearch::pop(QueueItem& out) {
    if (queueSize == 0) {
        return false;
    }
    out = queue[0];
    const QueueItem last = queue[--queueSize];
    uint32_t i = 0;
    while (true) {
        uint32_t child = i * 2 + 1;
        if (child >= queueSize) {
            break;
        }
        if (child + 1 < queueSize && queue[child + 1].f < queue[child].f) {
            child++;
        }
        if (queue[child].f >= last.f) {
            break;
        }
        queue[i] = queue[child];
        i = child;
    }
    if (queueSize > 0) {
        queue[i] = last;
    }
    return true;
}

RoutePoint RouteSearch::globalOf(uint32_t key) const {
    const Tile& tile = tiles[slotOf(key)];
    const uint16_t node = nodeOfKey(key);
    return { tile.key.x * GRAPH_EXTENT + tile.nodeX[node], tile.key.y * GRAPH_EXTENT + tile.nodeY[node] };
}

bool RouteSearch::joinAcross(GraphSource& source, int slot, uint16_t node, int side, uint32_t& joined, bool& limit) {
    const Tile* tile = &tiles[slot];
    const uint16_t x = tile->nodeX[node];
    const uint16_t y = tile->nodeY[node];
    const bool on = (side == 0 && x == 0) || (side == 1 && y == 0) || (side == 2 && x == GRAPH_EXTENT) || (side == 3 && y == GRAPH_EXTENT);
    if (!on) {
        return false;
    }
    const uint8_t level = tile->level[node];
    const TileKey here = tile->key;
    const TileKey next { GRAPH_ZOOM, here.x + (side == 0 ? -1 : (side == 2 ? 1 : 0)), here.y + (side == 1 ? -1 : (side == 3 ? 1 : 0)) };
    const int nextSlot = tileSlot(source, next);
    if (nextSlot == -2) {
        limit = true;
        return false;
    }
    if (nextSlot < 0) {
        return false;
    }
    const Tile& neighbour = tiles[nextSlot];
    const int opposite = (side + 2) % 4;
    const bool vertical = side == 0 || side == 2;
    const int position = vertical ? y : x;
    int bestNode = -1;
    int bestGap = JOIN_UNITS + 1;
    for (uint32_t i = 0; i < neighbour.borderCount[opposite]; i++) {
        const uint16_t candidate = neighbour.border[opposite][i];
        const int at = vertical ? neighbour.nodeY[candidate] : neighbour.nodeX[candidate];
        const int gap = std::abs(at - position);
        if (at > position + JOIN_UNITS) {
            break;
        }
        if (gap < bestGap && neighbour.level[candidate] == level) {
            bestGap = gap;
            bestNode = candidate;
        }
    }
    if (bestNode < 0) {
        return false;
    }
    joined = keyOf(nextSlot, static_cast<uint32_t>(bestNode));
    return true;
}

bool RouteSearch::smallComponent(GraphSource& source, const Snap& at, RouteProfile componentProfile, bool towards, bool fresh) {
    // Breadth first over the roads the profile may use in their allowed direction (away from a start, towards a goal:
    // a start on a one-way piece that leads nowhere is as bad as an island), across tile sides, in the node table (its states
    // mark the nodes seen) with the queue's array as the FIFO: small when it runs out before COMPONENT_NODES. Not
    // fresh: the pieces found before stay marked, so snap() keeps leaving them out
    if (fresh) {
        resetStates();
    }
    uint32_t head = 0;
    uint32_t tail = 0;
    const uint32_t cap = std::min<uint32_t>(COMPONENT_NODES, queueCapacity);
    auto add = [&](uint32_t key) {
        if (entry(key, false) != nullptr) {
            return true;
        }
        if (tail >= cap || entry(key, true) == nullptr) {
            return false;
        }
        queue[tail++].key = key;
        return true;
    };
    // Seeded as the search is: a start leaves its road towards the to-node (and the from-node where it may go
    // backwards), a goal is reached from the from-node (and the to-node)
    const Tile& startTile = tiles[at.slot];
    const bool both = routeAllowsBackwards(componentProfile, startTile.flags[at.edge]);
    const uint16_t ahead = towards ? startTile.from[at.edge] : startTile.to[at.edge];
    const uint16_t behind = towards ? startTile.to[at.edge] : startTile.from[at.edge];
    if (!add(keyOf(at.slot, ahead)) || (both && !add(keyOf(at.slot, behind)))) {
        return false;
    }
    while (head < tail) {
        const uint32_t key = queue[head++].key;
        const int slot = slotOf(key);
        const uint16_t node = nodeOfKey(key);
        for (uint32_t k = tiles[slot].first[node]; k < tiles[slot].first[node + 1]; k++) {
            const Tile& tile = tiles[slot];
            const uint16_t edge = static_cast<uint16_t>(tile.adjacent[k] >> 1);
            if (!routeAllows(componentProfile, tile.roadClass[edge], tile.flags[edge])) {
                continue;
            }
            // Leaving this node along the edge is against its from-to direction when the entry is marked backwards;
            // arriving here along it (towards) is against it when the entry is not
            const bool backwards = (tile.adjacent[k] & 1) != 0;
            if (backwards != towards && !routeAllowsBackwards(componentProfile, tile.flags[edge])) {
                continue;
            }
            const uint16_t other = backwards ? tile.from[edge] : tile.to[edge];
            if (!add(keyOf(slot, other))) {
                return false;
            }
        }
        for (int side = 0; side < 4; side++) {
            uint32_t joined = EMPTY;
            bool limit = false;
            if (joinAcross(source, slot, node, side, joined, limit) && !add(joined)) {
                return false;
            }
            if (limit) {
                // Out of memory for tiles: not known, taken as part of the network
                return false;
            }
        }
    }
    return true;
}

bool RouteSearch::snapOutside(GraphSource& source, const RoutePoint& point, RouteProfile snapProfile, bool towards, Snap& out) {
    // A start or goal on a small piece of road (a service road in a pedestrian zone for a car) goes to the nearest road
    // outside it; an old town has several such pieces close together
    for (int attempt = 0; attempt < SNAP_ATTEMPTS && smallComponent(source, out, snapProfile, towards, attempt == 0); attempt++) {
        excludeSeen = true;
        Snap other {};
        const bool found = snap(source, point, snapProfile, other);
        excludeSeen = false;
        if (!found) {
            break;
        }
        out = other;
    }
    resetStates();
    return true;
}

bool RouteSearch::isIsland(const Tile& tile, uint16_t edge, RouteProfile islandProfile) const {
    // The nodes reached from the edge over roads the profile may use, either way: an island when they run out before
    // ISLAND_NODES and none lies on a side of the tile (where it would join the next tile). Found on the device: a
    // destination snapped to a 2-node footway, and the search read the whole city looking for a way to it.
    uint16_t seen[ISLAND_NODES];
    int count = 0;
    int next = 0;
    auto visit = [&](uint16_t node) {
        for (int i = 0; i < count; i++) {
            if (seen[i] == node) {
                return true;
            }
        }
        if (count >= ISLAND_NODES) {
            return false;
        }
        seen[count++] = node;
        return true;
    };
    visit(tile.from[edge]);
    visit(tile.to[edge]);
    while (next < count) {
        const uint16_t node = seen[next++];
        const uint16_t x = tile.nodeX[node];
        const uint16_t y = tile.nodeY[node];
        if (x == 0 || y == 0 || x == GRAPH_EXTENT || y == GRAPH_EXTENT) {
            return false;
        }
        for (uint32_t k = tile.first[node]; k < tile.first[node + 1]; k++) {
            const uint16_t other = static_cast<uint16_t>(tile.adjacent[k] >> 1);
            if (!routeAllows(islandProfile, tile.roadClass[other], tile.flags[other])) {
                continue;
            }
            const uint16_t far = (tile.adjacent[k] & 1) != 0 ? tile.from[other] : tile.to[other];
            if (!visit(far)) {
                // More than ISLAND_NODES: part of the network
                return false;
            }
        }
    }
    return true;
}

bool RouteSearch::snap(GraphSource& source, const RoutePoint& point, RouteProfile snapProfile, Snap& out) {
    // The tile of the point and the neighbours within reach
    const double metresPerUnit = routeMetresPerUnit(point.y);
    // A car may stop further from a destination off the roads (a park, a forest): the rest is on foot
    const float radius = snapProfile == RouteProfile::Car ? CAR_SNAP_METRES : SNAP_METRES;
    const auto reach = static_cast<int32_t>(radius / metresPerUnit) + 1;
    float best = radius;
    bool found = false;
    int islandSlot = -1;
    uint32_t islandEdge = 0;
    bool island = false;
    TileGraph graph;
    ShapeBuffer shape;
    const int left = (point.x - reach) / GRAPH_EXTENT;
    const int right = (point.x + reach) / GRAPH_EXTENT;
    const int top = (point.y - reach) / GRAPH_EXTENT;
    const int bottom = (point.y + reach) / GRAPH_EXTENT;
    for (int ty = top; ty <= bottom; ty++) {
        for (int tx = left; tx <= right; tx++) {
            const TileKey key { GRAPH_ZOOM, tx, ty };
            const int slot = tileSlot(source, key);
            if (slot < 0 || !source.readGraph(key, fileBuffer)) {
                continue;
            }
            // The full graph with shapes, for this tile only
            if (!loadTileGraph(fileBuffer.begin(), fileBuffer.size(), inflater, graph)) {
                continue;
            }
            const int32_t px = point.x - tx * GRAPH_EXTENT;
            const int32_t py = point.y - ty * GRAPH_EXTENT;
            for (uint32_t e = 0; e < graph.edges.size(); e++) {
                const GraphEdge& edge = graph.edges[e];
                if (!routeAllows(snapProfile, edge.roadClass, edge.flags)) {
                    continue;
                }
                shape.load(graph, e);
                for (int i = 1; i < shape.count; i++) {
                    const float ax = static_cast<float>(shape.x[i - 1]);
                    const float ay = static_cast<float>(shape.y[i - 1]);
                    const float dx = shape.x[i] - ax;
                    const float dy = shape.y[i] - ay;
                    const float lengthSquared = dx * dx + dy * dy;
                    float t = lengthSquared > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / lengthSquared : 0.0f;
                    t = std::clamp(t, 0.0f, 1.0f);
                    const float qx = ax + t * dx;
                    const float qy = ay + t * dy;
                    const float distance = std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy)) * static_cast<float>(metresPerUnit);
                    if (distance < best && excludeSeen &&
                        (entry(keyOf(slot, tiles[slot].from[e]), false) != nullptr || entry(keyOf(slot, tiles[slot].to[e]), false) != nullptr)) {
                        // In the small piece of road found before (snapOutside)
                        continue;
                    }
                    if (distance < best && !(slot == islandSlot && e == islandEdge && island)) {
                        // A road piece that leads nowhere (a few nodes, off the tile's sides) is no start or goal
                        if (slot != islandSlot || e != islandEdge) {
                            islandSlot = slot;
                            islandEdge = e;
                            island = isIsland(tiles[slot], static_cast<uint16_t>(e), snapProfile);
                        }
                        if (island) {
                            continue;
                        }
                        best = distance;
                        found = true;
                        out.slot = slot;
                        out.edge = static_cast<uint16_t>(e);
                        out.along = shape.at[i - 1] + t * (shape.at[i] - shape.at[i - 1]);
                        out.total = shape.total();
                        out.distanceM = distance;
                        out.point = { tx * GRAPH_EXTENT + static_cast<int32_t>(std::floor(qx + 0.5f)), ty * GRAPH_EXTENT + static_cast<int32_t>(std::floor(qy + 0.5f)) };
                    }
                }
            }
        }
    }
    return found;
}

void RouteSearch::relax(uint32_t key, float g, uint32_t parent, uint16_t edge, uint8_t via, const RoutePoint& goal, float scale) {
    Entry* state = entry(key, true);
    if (state == nullptr) {
        return;
    }
    if (state->closed != 0 || g >= state->g) {
        return;
    }
    state->g = g;
    state->parent = parent;
    state->edge = edge;
    state->via = via;
    const RoutePoint at = globalOf(key);
    const float h = lengthOf(goal.x - at.x, goal.y - at.y) * scale;
    if (!push(g + h, key)) {
        // A full queue ends the search as too long
        maxNodes = used;
    }
    counters.discovered++;
}

RouteResult RouteSearch::find(GraphSource& source, double fromLat, double fromLon, double toLat, double toLon, RouteProfile searchProfile,
    Route& route, RouteStats* stats) {
    const uint32_t started = nowMs();
    route.clear();
    counters = {};
    counters.passes = 1;
    loadMs = 0;
    cancelled = false;
    profile = searchProfile;
    if (table == nullptr) {
        return RouteResult::NoMemory;
    }
    release();
    tilesFull = false;
    budgetHit = false;
    peakGraphBytes = 0;
    resetStates();
    const RoutePoint from = routePointFrom(fromLat, fromLon);
    const RoutePoint to = routePointFrom(toLat, toLon);
    route.profile = profile;
    route.from = from;
    route.to = to;
    auto finish = [&](RouteResult result) {
        counters.totalMs = nowMs() - started;
        counters.loadMs = loadMs;
        counters.graphBytes = static_cast<uint32_t>(peakGraphBytes);
        counters.peakBytes = static_cast<uint32_t>(peakGraphBytes + static_cast<size_t>(used) * sizeof(Entry) + static_cast<size_t>(queueSize) * sizeof(QueueItem));
        if (stats != nullptr) {
            *stats = counters;
        }
        return result;
    };
    Snap start {};
    Snap goal {};
    // Seconds per unit of straight distance at the fastest speed: never more than the real time, so A* stays exact
    const float metresPerUnit = static_cast<float>(std::min(routeMetresPerUnit(from.y), routeMetresPerUnit(to.y))) * 0.995f;
    const float scale = metresPerUnit * 3.6f / routeMaxKmh(profile);

    float best = INFINITY;
    uint32_t last = EMPTY;
    uint8_t lastSide = 0;
    bool direct = false;
    bool limit = false;
    // A second pass when the first one ran out of node states or of memory for the tiles' graphs (then without a path
    // around the tiles it had to skip): the estimate counts more than the real distance, so the search heads for the
    // goal and settles and reads far fewer. The route may be a little longer than the best one (at most RETRY_WEIGHT
    // times, a few percent in practice), which beats "too long" for a city trip. Out of graph memory, the tiles are
    // read again from the start; out of node states, the tiles read stay.
    for (int pass = 0; pass < 2; pass++) {
        const float weighted = pass == 0 ? scale : scale * RETRY_WEIGHT;
        // Out of tile memory (the budget, or the device's free memory, tilesFull): the tiles go and are read again
        const bool readAgain = pass > 0 && (budgetHit || tilesFull);
        if (pass > 0) {
            if (readAgain) {
                release();
                budgetHit = false;
            }
            resetStates();
            tilesFull = false;
            counters.passes = 2;
            best = INFINITY;
            last = EMPTY;
            lastSide = 0;
            direct = false;
            limit = false;
        }
        if (pass == 0 || readAgain) {
            // A tile near the start or goal that did not fit in the memory is no missing road
            if (!snap(source, from, profile, start)) {
                return finish(budgetHit || tilesFull ? RouteResult::NoMemory : RouteResult::NoRoadNearStart);
            }
            if (!snap(source, to, profile, goal)) {
                return finish(budgetHit || tilesFull ? RouteResult::NoMemory : RouteResult::NoRoadNearGoal);
            }
            snapOutside(source, from, profile, false, start);
            snapOutside(source, to, profile, true, goal);
        }
        const RoutePoint goalPoint = goal.point;
        const Tile& startTile = tiles[start.slot];
        const float startCost = routeSeconds(profile, startTile.roadClass[start.edge], startTile.length[start.edge] / 10.0f);
        const float a = start.total > 0.0f ? start.along / start.total : 0.0f;
        const Tile& goalTile = tiles[goal.slot];
        const float goalCost = routeSeconds(profile, goalTile.roadClass[goal.edge], goalTile.length[goal.edge] / 10.0f);
        const float b = goal.total > 0.0f ? goal.along / goal.total : 0.0f;
        const bool startBackwards = routeAllowsBackwards(profile, startTile.flags[start.edge]);
        const bool goalBackwards = routeAllowsBackwards(profile, goalTile.flags[goal.edge]);
        const uint32_t goalFrom = keyOf(goal.slot, goalTile.from[goal.edge]);
        const uint32_t goalTo = keyOf(goal.slot, goalTile.to[goal.edge]);
        // Start and goal on the same road between the same two junctions
        if (start.slot == goal.slot && start.edge == goal.edge) {
            if (b >= a) {
                best = (b - a) * startCost;
                direct = true;
            } else if (startBackwards) {
                best = (a - b) * startCost;
                direct = true;
            }
        }
        relax(keyOf(start.slot, startTile.to[start.edge]), (1.0f - a) * startCost, NO_PARENT, start.edge, VIA_START, goalPoint, weighted);
        if (startBackwards) {
            relax(keyOf(start.slot, startTile.from[start.edge]), a * startCost, NO_PARENT, start.edge, VIA_START | VIA_BACKWARDS, goalPoint, weighted);
        }
        QueueItem item {};
        while (pop(item)) {
            if (item.f >= best) {
                break;
            }
            if ((counters.settled & 255) == 0 && cancelled) {
                return finish(RouteResult::Cancelled);
            }
            Entry* state = entry(item.key, false);
            if (state == nullptr || state->closed != 0) {
                continue;
            }
            state->closed = 1;
            counters.settled++;
            const float g = state->g;
            if (item.key == goalFrom && g + b * goalCost < best) {
                best = g + b * goalCost;
                last = item.key;
                lastSide = 0;
                direct = false;
            }
            if (item.key == goalTo && goalBackwards && g + (1.0f - b) * goalCost < best) {
                best = g + (1.0f - b) * goalCost;
                last = item.key;
                lastSide = 1;
                direct = false;
            }
            const int slot = slotOf(item.key);
            const uint16_t node = nodeOfKey(item.key);
            // Copies: loading a neighbour below may not move the tiles, but keeps the code clear of that question
            const Tile* tile = &tiles[slot];
            for (uint32_t k = tile->first[node]; k < tile->first[node + 1]; k++) {
                const uint16_t entryValue = tile->adjacent[k];
                const uint16_t edge = static_cast<uint16_t>(entryValue >> 1);
                const bool backwards = (entryValue & 1) != 0;
                if (!routeAllows(profile, tile->roadClass[edge], tile->flags[edge]) || (backwards && !routeAllowsBackwards(profile, tile->flags[edge]))) {
                    continue;
                }
                const uint16_t other = backwards ? tile->from[edge] : tile->to[edge];
                const float cost = routeSeconds(profile, tile->roadClass[edge], tile->length[edge] / 10.0f);
                relax(keyOf(slot, other), g + cost, item.key, edge, backwards ? VIA_BACKWARDS : 0, goalPoint, weighted);
            }
            // Across a tile side: the neighbour's border node at the same place and level is the same point
            for (int side = 0; side < 4 && !limit; side++) {
                uint32_t joined = EMPTY;
                if (joinAcross(source, slot, node, side, joined, limit)) {
                    relax(joined, g, item.key, 0, VIA_JOIN, goalPoint, weighted);
                }
            }
            if (used >= maxNodes || tilesFull) {
                limit = true;
                break;
            }
        }
        if (best != INFINITY || !(limit || budgetHit)) {
            break;
        }
    }
    if (best == INFINITY) {
        return finish(limit || budgetHit ? RouteResult::TooLong : RouteResult::NoPath);
    }
    route.seconds = best;
    if (!assemble(source, start, goal, last, lastSide, direct, route)) {
        route.clear();
        return finish(RouteResult::NoMemory);
    }
    return finish(RouteResult::Found);
}

bool RouteSearch::assemble(GraphSource& source, const Snap& start, const Snap& goal, uint32_t last, uint8_t lastSide, bool direct, Route& route) {
    // The hops from the start, newest first while walking back
    struct Hop {
        TileKey tile;
        uint16_t edge;
        uint8_t via;
    };
    GrowArray<Hop> hops;
    if (!direct) {
        uint32_t key = last;
        uint32_t guard = 0;
        while (key != NO_PARENT && guard++ < used + 2) {
            const Entry* state = entry(key, false);
            if (state == nullptr) {
                return false;
            }
            hops.push({ tiles[slotOf(key)].key, state->edge, state->via });
            key = state->parent;
        }
        std::reverse(hops.begin(), hops.end());
    }
    if (hops.failed()) {
        return false;
    }
    // The search's graphs and states are not needed any more: their memory goes to the full graphs read now
    const TileKey startTile = tiles[start.slot].key;
    const TileKey goalTile = tiles[goal.slot].key;
    release();
    TileGraph graph;
    TileKey graphTile { -1, -1, -1 };
    ShapeBuffer shape;
    auto useSlot = [&](const TileKey& tile) {
        if (tile == graphTile) {
            return true;
        }
        graphTile = { -1, -1, -1 };
        if (!source.readGraph(tile, fileBuffer) || !loadTileGraph(fileBuffer.begin(), fileBuffer.size(), inflater, graph)) {
            return false;
        }
        graphTile = tile;
        return true;
    };
    auto addPoint = [&](const TileKey& tile, int32_t x, int32_t y) {
        const RoutePoint point { tile.x * GRAPH_EXTENT + x, tile.y * GRAPH_EXTENT + y };
        if (route.points.size() > 0) {
            const RoutePoint& previous = route.points[route.points.size() - 1];
            if (previous.x == point.x && previous.y == point.y) {
                return;
            }
        }
        route.points.push(point);
    };
    auto addStep = [&](uint16_t edge) {
        uint8_t length = 0;
        const char* name = graph.name(edge, length);
        const GraphEdge& e = graph.edges[edge];
        const uint32_t pointIndex = route.points.size() > 0 ? static_cast<uint32_t>(route.points.size() - 1) : 0;
        if (route.steps.size() > 0) {
            const RouteStep& previous = route.steps[route.steps.size() - 1];
            if (previous.roadClass == e.roadClass && previous.nameLength == length &&
                (length == 0 || memcmp(route.names.begin() + previous.nameOffset, name, length) == 0)) {
                return;
            }
        }
        const auto offset = static_cast<uint16_t>(std::min<size_t>(route.names.size(), 65535));
        for (uint8_t i = 0; i < length && route.names.size() < 65000; i++) {
            route.names.push(name[i]);
        }
        route.steps.push({ pointIndex, offset, length, e.roadClass, e.flags });
    };
    // Part of an edge's shape from distance d0 to d1 along it (backwards when d0 > d1)
    auto addPart = [&](const TileKey& slot, uint16_t edge, float d0, float d1) {
        if (!useSlot(slot) || edge >= graph.edges.size()) {
            return false;
        }
        shape.load(graph, edge);
        addStep(edge);
        int32_t px;
        int32_t py;
        shape.pointAt(d0, px, py);
        addPoint(slot, px, py);
        if (d0 <= d1) {
            for (int i = 0; i < shape.count; i++) {
                if (shape.at[i] > d0 && shape.at[i] < d1) {
                    addPoint(slot, shape.x[i], shape.y[i]);
                }
            }
        } else {
            for (int i = shape.count - 1; i >= 0; i--) {
                if (shape.at[i] < d0 && shape.at[i] > d1) {
                    addPoint(slot, shape.x[i], shape.y[i]);
                }
            }
        }
        shape.pointAt(d1, px, py);
        addPoint(slot, px, py);
        return true;
    };
    route.points.push(route.from);
    if (direct) {
        if (!addPart(startTile, start.edge, start.along, goal.along)) {
            return false;
        }
    } else {
        for (size_t i = 0; i < hops.size(); i++) {
            const Hop& hop = hops[i];
            if ((hop.via & VIA_JOIN) != 0) {
                continue;
            }
            const bool backwards = (hop.via & VIA_BACKWARDS) != 0;
            if ((hop.via & VIA_START) != 0) {
                if (!addPart(hop.tile, hop.edge, start.along, backwards ? 0.0f : start.total)) {
                    return false;
                }
                continue;
            }
            if (!useSlot(hop.tile) || hop.edge >= graph.edges.size()) {
                return false;
            }
            shape.load(graph, hop.edge);
            if (!addPart(hop.tile, hop.edge, backwards ? shape.total() : 0.0f, backwards ? 0.0f : shape.total())) {
                return false;
            }
        }
        if (!addPart(goalTile, goal.edge, lastSide == 0 ? 0.0f : goal.total, goal.along)) {
            return false;
        }
    }
    const RoutePoint end = route.to;
    if (route.points.size() == 0 || route.points[route.points.size() - 1].x != end.x || route.points[route.points.size() - 1].y != end.y) {
        route.points.push(end);
    }
    if (!route.distances.resize(route.points.size())) {
        return false;
    }
    float distance = 0.0f;
    route.distances[0] = 0.0f;
    for (size_t i = 1; i < route.points.size(); i++) {
        const RoutePoint& a = route.points[i - 1];
        const RoutePoint& b = route.points[i];
        distance += lengthOf(b.x - a.x, b.y - a.y) * static_cast<float>(routeMetresPerUnit((a.y + b.y) / 2));
        route.distances[i] = distance;
    }
    route.lengthM = distance;
    return !route.points.failed() && !route.steps.failed() && !route.names.failed();
}

} // namespace maps
