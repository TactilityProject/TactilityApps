// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/InflateReader.h"
#include "GrowArray.h"
#include "RouteGraph.h"
#include "RouteProfile.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace maps {

/** A point in global z14 graph units: x = tile x * 4096 + x in the tile, the same for y. */
struct RoutePoint {
    int32_t x;
    int32_t y;
};

RoutePoint routePointFrom(double lat, double lon);
void routePointTo(const RoutePoint& point, double& lat, double& lon);
/** Metres per graph unit at the latitude of a point. */
double routeMetresPerUnit(int32_t y);

/** One stretch of a route on one road: where it starts in the route's points, its name and class. */
struct RouteStep {
    uint32_t point;
    /** Offset into Route::names, the name's length (0: no name). */
    uint16_t nameOffset;
    uint8_t nameLength;
    uint8_t roadClass;
    uint8_t flags;
};

/** A found route: its line, the distance from the start at each point, its roads, length and time. */
struct Route {
    RouteProfile profile = RouteProfile::Walk;
    /** Where the route was asked for: the start and goal before snapping to roads. */
    RoutePoint from {};
    RoutePoint to {};
    GrowArray<RoutePoint> points;
    GrowArray<float> distances;
    GrowArray<RouteStep> steps;
    GrowArray<char> names;
    float lengthM = 0.0f;
    float seconds = 0.0f;

    void clear();
    bool isEmpty() const { return points.size() < 2; }
    /** The name of a step, "" when it has none. */
    const char* stepName(const RouteStep& step, char* out, size_t size) const;
};

/** Gives the search the graph files of z14 tiles. */
class GraphSource {
public:
    virtual ~GraphSource() = default;
    /** Reads the .prg file of a tile into file. False when there is none (yet). */
    virtual bool readGraph(const TileKey& tile, GrowArray<uint8_t>& file) = 0;
    /** Whether the device runs short of memory: the search then stops as too long rather than starve the app. */
    virtual bool memoryShort() { return false; }
};

enum class RouteResult : uint8_t { Found, NoRoadNearStart, NoRoadNearGoal, NoPath, TooLong, NoMemory, Cancelled };
const char* routeResultText(RouteResult result);

struct RouteStats {
    uint32_t totalMs;
    uint32_t loadMs;
    uint32_t settled;
    uint32_t discovered;
    uint32_t tilesRead;
    uint32_t tilesMissing;
    uint32_t peakBytes;
    uint32_t graphBytes;
    /** 2 when the first pass ran out of node states and a second, goal-directed pass ran (see RouteSearch::find). */
    uint32_t passes;
};

/**
 * A* over the tiles' graphs, read as the search reaches them (per-tile node ids; a hash map from tile and node to the
 * search state, so nothing is sized for the whole region). Border nodes of neighbouring tiles join when they lie
 * within GRAPH_JOIN_UNITS of each other on the shared side, on the same level. Start and goal snap to the nearest
 * road the profile may use within SNAP_METRES (CAR_SNAP_METRES for a car) that is not a small island (a few nodes leading nowhere). The search stops cleanly with TooLong when it reaches its node cap or
 * memory budget, after a second goal-directed pass on the tiles it has read (see find()).
 */
class RouteSearch {
public:
    static constexpr float SNAP_METRES = 200.0f;
    static constexpr float CAR_SNAP_METRES = 500.0f;
    /** Joining needs 2 units: neighbouring tiles clip the same road up to 2 units apart (host/routing/check_prg.py). */
    static constexpr int JOIN_UNITS = 2;

    RouteSearch() = default;
    ~RouteSearch();
    RouteSearch(const RouteSearch&) = delete;
    RouteSearch& operator=(const RouteSearch&) = delete;

    /**
     * Takes up to memoryBytes of large memory for the search: the node states, the queue and the tiles' graphs. Returns
     * false when not even a small search fits.
     */
    bool begin(size_t memoryBytes);
    /** Gives the memory back. */
    void end();

    RouteResult find(GraphSource& source, double fromLat, double fromLon, double toLat, double toLon, RouteProfile profile, Route& route,
        RouteStats* stats);
    /** Makes a running find() return Cancelled soon. */
    void cancel() { cancelled = true; }
    /** The node states the memory allows, for the log. */
    uint32_t nodeCapacity() const { return maxNodes; }

private:
    struct Tile;
    struct Entry {
        uint32_t key;
        float g;
        uint32_t parent;
        uint16_t edge;
        uint8_t closed;
        /** 1: the edge was used backwards; 2: a join across tiles (no edge); 4: from the start point. */
        uint8_t via;
    };
    struct QueueItem {
        float f;
        uint32_t key;
    };
    struct Snap {
        int slot;
        uint16_t edge;
        /** Units along the edge's shape from its from-node, and the shape's length in units. */
        float along;
        float total;
        float distanceM;
        RoutePoint point;
    };

    int tileSlot(GraphSource& source, const TileKey& tile);
    bool loadTile(GraphSource& source, const TileKey& tile, Tile& out);
    bool snap(GraphSource& source, const RoutePoint& point, RouteProfile profile, Snap& out);
    bool isIsland(const Tile& tile, uint16_t edge, RouteProfile islandProfile) const;
    /** The node a tile's node joins across one side; limit when the next tile could not be read. */
    bool joinAcross(GraphSource& source, int slot, uint16_t node, int side, uint32_t& joined, bool& limit);
    /**
     * Whether the roads around a snapped point reach fewer than COMPONENT_NODES nodes (the states mark them), driving
     * away from it, or towards it for a goal.
     */
    bool smallComponent(GraphSource& source, const Snap& at, RouteProfile componentProfile, bool towards, bool fresh);
    /** Moves a snapped point off a small piece of road to the nearest road outside it. Leaves the states empty. */
    bool snapOutside(GraphSource& source, const RoutePoint& point, RouteProfile snapProfile, bool towards, Snap& out);
    Entry* entry(uint32_t key, bool create);
    void resetStates();
    bool push(float f, uint32_t key);
    bool pop(QueueItem& out);
    void relax(uint32_t key, float g, uint32_t parent, uint16_t edge, uint8_t via, const RoutePoint& goal, float scale);
    RoutePoint globalOf(uint32_t key) const;
    bool assemble(GraphSource& source, const Snap& start, const Snap& goal, uint32_t last, uint8_t lastSide, bool direct, Route& route);
    void release();

    Tile* tiles = nullptr;
    int tileCount = 0;
    Entry* table = nullptr;
    uint32_t tableCapacity = 0;
    uint32_t used = 0;
    uint32_t maxNodes = 0;
    QueueItem* queue = nullptr;
    uint32_t queueSize = 0;
    uint32_t queueCapacity = 0;
    size_t budget = 0;
    size_t graphBytes = 0;
    size_t peakGraphBytes = 0;
    bool tilesFull = false;
    /** snap() leaves out edges with a state at either end (the small piece found by smallComponent()). */
    bool excludeSeen = false;
    bool budgetHit = false;
    GrowArray<uint8_t> fileBuffer;
    GrowArray<uint8_t> bodyBuffer;
    InflateReader inflater;
    bool inflaterReady = false;
    RouteProfile profile = RouteProfile::Walk;
    std::atomic<bool> cancelled { false };
    RouteStats counters {};
    uint32_t loadMs = 0;
};

} // namespace maps
