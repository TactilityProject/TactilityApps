// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SearchIndex.h"
#include "TextFold.h"

#include "../route/GrowArray.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * The query side of offline search (M6 study, sections 5 and 6): a set of open region indexes, the ranking of a typed
 * text over all of them, "nearby" lists by category and the recent searches. No Tactility, no file API: the indexes
 * are read through SearchFile objects that the caller provides and keeps alive.
 */

constexpr int SEARCH_MAX_INDEXES = 8;
/** Results whose names the first page needs (search() with max = this); more() fetches the next ones. */
constexpr int SEARCH_FIRST_PAGE = 8;
/** The size of a "nearby" list. */
constexpr int SEARCH_NEARBY = 20;
constexpr int SEARCH_NAME_BYTES = 96;
constexpr int SEARCH_TYPE_BYTES = 28;
constexpr int SEARCH_ALSO_BYTES = 32;
constexpr int SEARCH_ALSO_COUNT = 2;
constexpr int SEARCH_HEADER_BYTES = 128;
/** The settlement a result lies in ("Holice"). */
constexpr int SEARCH_PLACE_BYTES = 40;
/** A type line with its settlement: "Street · Holice". */
constexpr int SEARCH_LINE_BYTES = SEARCH_TYPE_BYTES + SEARCH_PLACE_BYTES + 4;

/** A position in global z14 tile units (the unit of the index points). */
struct SearchOrigin {
    int32_t x;
    int32_t y;
};

/** The ranking groups in the order the results come: the study's recommendation 8. */
enum class ResultGroup : uint8_t {
    /** Street and number found. */
    Address,
    /** The query is a category word: the nearest of that category. */
    Category,
    /** The name equals the query. */
    ExactName,
    /** Cities, towns, villages, suburbs. */
    Place,
    /** Parks, water, peaks, airports. */
    Natural,
    Street,
    /** Points of interest whose name starts with the query. */
    Poi,
    /** A nearby() list. */
    Nearby,
};

struct SearchResult {
    /** The display name (UTF-8); for an address the address text, "Štefánikova 4". An unnamed point shows its type. */
    char name[SEARCH_NAME_BYTES];
    /** The type in plain words: "Cafe", "Street", "Village", "Address" ... */
    char type[SEARCH_TYPE_BYTES];
    /**
     * The city, town, village or hamlet the result lies in, estimated from the place points of the open indexes (the
     * nearest by distance over the class's size, as the village rule scores them); empty for places themselves, when
     * no settlement is near, or when the name already starts with it ("Holice 238").
     */
    char place[SEARCH_PLACE_BYTES];
    /** Up to two alternative village names ("Also: ..."), empty when the index does not carry them (it does not yet). */
    char also[SEARCH_ALSO_COUNT][SEARCH_ALSO_BYTES];
    /** The point in global z14 units. */
    int32_t x;
    int32_t y;
    /** The bounding box of a street (hasBox), else the point. */
    int32_t minX, minY, maxX, maxY;
    bool hasBox;
    /** From the search origin: metres on the ground, and degrees clockwise from north (0..359). */
    uint32_t distanceM;
    uint16_t bearing;
    /** An address 50..100 m from its street (the index does not carry the flag yet: always false). */
    bool lowConfidence;
    /** The address uses an estimated village name (the index does not carry it yet: always false). */
    bool villageEstimated;
    ResultGroup group;
    /** The slot of the index (addIndex() result) that holds it. */
    uint8_t slot;
};

/** What a search did, for the UI header, the tests and the measurements. */
struct SearchStats {
    /** Reads made through the SearchFile objects: calls, distinct 4 KB blocks, runs of consecutive blocks. */
    uint32_t readCalls;
    uint32_t readBlocks;
    uint32_t readRuns;
    /** Microseconds by the caller's clock (0 without one). */
    uint32_t elapsedUs;
    /** Ranked results found (more() can fetch them), and per group. */
    uint32_t total;
    uint32_t groupCount[8];
    uint32_t indexesSearched;
    /** Fewer than 3 characters: nothing was searched. */
    bool tooShort;
    /** No result for the text: the longest prefix that has some was used. */
    bool backedOff;
    /** The folded text that was looked up (after a back-off: the prefix that has results). */
    char key[96];
    /** "Showing results for 'stefan'" after a back-off, else empty. */
    char header[SEARCH_HEADER_BYTES];
};

/** A clock in microseconds, supplied by the caller (monotonic; CPU time when the platform has it). */
using SearchClock = uint64_t (*)();

/** A "nearby" category: a plain-word name and the class-table labels it covers. */
struct SearchCategory {
    const char* name;
    /** Labels of the index class table: "poi/cafe/cafe" matches itself; "poi/restaurant/" followed by a star matches all subclasses. */
    const char* const* labels;
    uint8_t labelCount;
};

class SearchEngine {
public:
    using Result = SearchResult;

    SearchEngine();
    ~SearchEngine();
    SearchEngine(const SearchEngine&) = delete;
    SearchEngine& operator=(const SearchEngine&) = delete;

    // ---- the open indexes. A region that is cut into several block files adds each file as its own index.

    /**
     * Opens an index over the file (kept, not owned: it must outlive the engine or removeAll()). profile is the
     * region's transliteration profile; slashSecond tells that "conscription/street" numbers have the street number
     * second (the writer's slashSecond flag, not stored in the file). The indexes should be added nearest to the map
     * centre first: that order breaks ties between equally distant results. Returns the slot, or -1 (all slots used, or
     * the file is not an index).
     */
    int addIndex(SearchFile* file, const char* regionName, Translit profile, bool slashSecond);
    void removeAll();
    int indexCount() const { return slotCount; }
    const char* regionName(int slot) const;
    const SearchIndexReader* reader(int slot) const;
    /** New priority order: the slots, nearest first; slots that are not listed go last in their old order. */
    void reorder(const int* slots, int count);

    /** Keeps the name group area and/or the key pages of an index in memory (the reader's loadResident). */
    bool loadResident(int slot, bool nameGroups, bool keyPages);
    /** Gives the memory of every index's resident pages back (routing needs it); queries read the files again. */
    void dropResident();

    /** The clock for SearchStats::elapsedUs. */
    void setClock(SearchClock clockFunction) { clock = clockFunction; }
    /** How many name groups one index contributes at most per query (default 64; a bigger number ranks better, reads more). */
    void setCandidateLimit(uint32_t limit);

    // ---- search

    /**
     * Searches all indexes for the typed text and ranks the results: exact address (street and number) -> category
     * word -> exact names -> places -> streets -> points of interest by prefix; inside a group the nearest to origin
     * first, and many results of one name nearest first. Fewer than 3 characters search nothing. When nothing is
     * found the longest prefix that has results is used and stats->header says so. Returns the first results, at most
     * max (use SEARCH_FIRST_PAGE); the rest is kept for more().
     */
    int search(const char* typed, SearchOrigin origin, SearchResult* out, int max, SearchStats* stats = nullptr);
    /** The next results of the last search(), at most max; 0 at the end. Names of unnamed-lookup results are read here. */
    int more(SearchResult* out, int max);
    /** How many ranked results the last search or nearby() kept. */
    int resultCount() const { return static_cast<int>(ranked.size()); }

    // ---- nearby

    /** The categories of the nearby list: about 12, built from the study's most common POI classes. */
    static const SearchCategory* categories(int* count);
    /** The nearest max (at most 64) points of a category from origin; replaces the results that more() continues. */
    int nearby(int category, SearchOrigin origin, SearchResult* out, int max, SearchStats* stats = nullptr);

private:
    /** A settlement or suburb point of an index: position, name group, class size in metres. */
    struct PlacePoint {
        int32_t x, y;
        uint32_t ng;
        float scale;
        /** A city or town (gets its suburb in front: "Ružinov, Bratislava"), a village or hamlet, a suburb. */
        enum Level : uint8_t { Big, Small, Suburb } level;
    };
    struct BestPlace {
        int slot = -1;
        uint32_t ng = 0;
        PlacePoint::Level level = PlacePoint::Small;
    };
    /** The best scored place of the levels from..to within the score limit. */
    BestPlace bestPlace(int32_t x, int32_t y, PlacePoint::Level from, PlacePoint::Level to, float limit);
    struct Slot {
        GrowArray<PlacePoint> places;
        bool placesLoaded = false;
        SearchFile* file = nullptr;
        SearchIndexReader reader;
        char region[32] = {};
        Translit profile = Translit::Russian;
        bool slashSecond = false;
    };
    /** A ranked result before its name is final: the result and what ordering and the lazy name need. */
    struct Candidate {
        SearchResult result;
        uint64_t distanceSq;
        /** Name group offset when the name is read on demand (nameLazy), else unused. */
        uint32_t ng;
        uint8_t rank;
        uint8_t priority;
        bool nameLazy;
        /** A settlement or other place: gets no place of its own. */
        bool isPlace;
    };
    struct Query;

    int priorityOf(int slotIndex) const;
    void startSearch(SearchOrigin origin);
    void setGeo(SearchResult& result, uint64_t& distanceSq) const;
    void fillStats(SearchStats* stats, uint64_t startUs, bool tooShort);
    bool collectAddresses(const Query& query, SearchOrigin origin);
    void collectNames(const char* const* keys, int keyCount, SearchOrigin origin);
    bool prefixExists(const char* key);
    void rankCandidates(size_t cap);
    bool materialise(Candidate& candidate, SearchResult& out);
    void loadPlaces(Slot& slot);
    void findPlace(SearchResult& result);
    int takeResults(SearchResult* out, int max);

    Slot* slots[SEARCH_MAX_INDEXES] = {};
    uint8_t order[SEARCH_MAX_INDEXES] = {};
    int slotCount = 0;
    SearchClock clock = nullptr;
    uint32_t candidateLimit = 64;

    SearchOrigin currentOrigin = {};
    /** Metres of one z14 unit at the latitude of the origin. */
    double unitMetres = 0.4;
    GrowArray<Candidate> ranked;
    size_t cursor = 0;
};

/** The type line a list shows: "Street · Holice", or the type alone without a place. */
void searchTypeLine(const char* type, const char* place, char* out, size_t size);

/** The last chosen results, newest first: what the search sheet offers before the user types. */
class RecentSearches {
public:
    static constexpr int CAPACITY = 10;
    struct Entry {
        char name[SEARCH_NAME_BYTES];
        /** The type line with the settlement (searchTypeLine). */
        char type[SEARCH_LINE_BYTES];
        int32_t x;
        int32_t y;
    };
    /** Writes bytes somewhere (a file): false on failure. */
    using WriteBytes = bool (*)(void* context, const void* data, size_t size);
    /** Reads up to capacity bytes: the count, 0 at the end. */
    using ReadBytes = size_t (*)(void* context, void* out, size_t capacity);

    void clear() { used = 0; }
    int count() const { return used; }
    /** 0 is the newest. */
    const Entry& at(int index) const { return entries[index]; }
    /** A chosen result goes to the front; the same name at the same point moves instead of repeating; the oldest drops. */
    void add(const SearchResult& result);
    /** Text format, a header line then one line per entry: x TAB y TAB type TAB name. */
    bool save(WriteBytes write, void* context) const;
    /** Replaces the list; false (and an empty list) when the data is not this format. */
    bool load(ReadBytes read, void* context);

private:
    Entry entries[CAPACITY] = {};
    int used = 0;
};

} // namespace maps
