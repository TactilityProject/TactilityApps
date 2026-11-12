// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchEngine.h"

#include "AddressMatch.h"
#include "QueryParse.h"

#include "../render/PoiTypes.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace maps {

namespace {

constexpr double PI_VALUE = 3.14159265358979323846;
/** The side of the world in z14 units: 2^14 tiles of 4096 units. */
constexpr double WORLD_UNITS = 16384.0 * 4096.0;
constexpr size_t KEY_BYTES = 256;
/** Candidates kept after a ranking, and the size at which the list is ranked and cut down in between. */
constexpr size_t MAX_RANKED = 300;
constexpr size_t PRUNE_AT = 1200;
/** Points of one name group (40 x Billa) and of a category run that are kept: the nearest. */
constexpr size_t NAME_POINTS = 64;
constexpr size_t CATEGORY_POINTS = 100;
constexpr size_t STREET_CLUSTERS = 16;
constexpr uint32_t RECORD_CHUNK = 512;
constexpr int MIN_CHARACTERS = 3;
constexpr int DUPLICATE_UNITS = 4;
/** A settlement counts while distance / class size is at most this (a village 3.2 km, a city 12 km). */
constexpr float PLACE_SCORE_LIMIT = 4.0f;
/** A suburb of the city or town counts while distance / 700 m is at most this (2.1 km). */
constexpr float SUBURB_SCORE_LIMIT = 3.0f;

// ---------------------------------------------------------------------------------------------------- text helpers

/** Copies text, cut on a UTF-8 character boundary to fit size (0-terminated). */
void copyText(char* out, size_t size, const char* text) {
    if (size == 0) {
        return;
    }
    size_t length = strlen(text);
    if (length >= size) {
        length = size - 1;
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
            length--;
        }
    }
    memcpy(out, text, length);
    out[length] = 0;
}

int characterCount(const char* text) {
    int count = 0;
    for (; *text != 0; text++) {
        if ((static_cast<unsigned char>(*text) & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

/** Byte length of the first n characters. */
size_t bytesOfCharacters(const char* text, int n) {
    size_t at = 0;
    int seen = 0;
    while (text[at] != 0) {
        if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80) {
            if (seen == n) {
                break;
            }
            seen++;
        }
        at++;
    }
    return at;
}

void trimBlanks(char* text) {
    size_t length = strlen(text);
    while (length > 0 && text[length - 1] == ' ') {
        text[--length] = 0;
    }
    size_t start = 0;
    while (text[start] == ' ') {
        start++;
    }
    if (start > 0) {
        memmove(text, text + start, length - start + 1);
    }
}

/** The lookup keys of a text: folded Latin, and for Greek and Cyrillic also the folded original script (first). */
int makeKeys(const char* text, char keys[2][KEY_BYTES]) {
    int count = 0;
    if (hasNonLatin(text)) {
        foldOriginal(text, keys[count], KEY_BYTES);
        trimBlanks(keys[count]);
        count++;
    }
    foldLatin(text, keys[count], KEY_BYTES);
    trimBlanks(keys[count]);
    if (count == 1 && strcmp(keys[0], keys[1]) == 0) {
        return 1;
    }
    return count + 1;
}

/** "cafe" -> "Cafe", "isolated_dwelling" -> "Isolated dwelling". */
void humanise(const char* text, char* out, size_t size) {
    size_t i = 0;
    for (; text[i] != 0 && text[i] != '/' && i + 1 < size; i++) {
        out[i] = text[i] == '_' ? ' ' : text[i];
    }
    out[i] = 0;
    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
}

/** The group and the plain-word type of a class table label ("poi/cafe/cafe", "place/town", "peak/peak"). */
ResultGroup describeLabel(const char* label, char* type, size_t typeSize) {
    if (strncmp(label, "poi/", 4) == 0) {
        const char* classText = label + 4;
        const char* slash = strchr(classText, '/');
        size_t classLength = slash != nullptr ? static_cast<size_t>(slash - classText) : strlen(classText);
        const char* sub = slash != nullptr ? slash + 1 : nullptr;
        size_t subLength = sub != nullptr ? strlen(sub) : 0;
        if (subLength == 1 && sub[0] == '*') {
            sub = nullptr;
            subLength = 0;
        }
        uint8_t id = poiTypeFor(classText, classLength, sub, subLength);
        if (id != 0) {
            copyText(type, typeSize, poiType(id).label);
        } else {
            const char* source = (sub != nullptr && subLength > 0 && strcmp(sub, "None") != 0) ? sub : classText;
            char text[64];
            humanise(source, text, sizeof(text));
            copyText(type, typeSize, text);
        }
        return ResultGroup::Poi;
    }
    if (strncmp(label, "place/", 6) == 0) {
        static const char* const NAMES[][2] = {
            { "city", "City" }, { "town", "Town" }, { "village", "Village" }, { "hamlet", "Hamlet" }, { "suburb", "Suburb" },
            { "quarter", "Quarter" }, { "neighbourhood", "Neighbourhood" }, { "island", "Island" },
            { "isolated_dwelling", "Dwelling" }, { "locality", "Locality" },
        };
        const char* classText = label + 6;
        for (const auto& entry : NAMES) {
            if (strcmp(classText, entry[0]) == 0) {
                copyText(type, typeSize, entry[1]);
                return ResultGroup::Place;
            }
        }
        char text[64];
        humanise(classText, text, sizeof(text));
        copyText(type, typeSize, text);
        return ResultGroup::Place;
    }
    if (strncmp(label, "peak/", 5) == 0) {
        copyText(type, typeSize, strcmp(label + 5, "cliff") == 0 ? "Cliff" : "Peak");
    } else if (strncmp(label, "water/", 6) == 0) {
        copyText(type, typeSize, strcmp(label + 6, "lake") == 0 ? "Lake" : "Water");
    } else if (strncmp(label, "park/", 5) == 0) {
        copyText(type, typeSize, "Park");
    } else if (strncmp(label, "aerodrome/", 10) == 0) {
        copyText(type, typeSize, "Airport");
    } else {
        char text[64];
        humanise(label, text, sizeof(text));
        copyText(type, typeSize, text);
    }
    return ResultGroup::Natural;
}

/** Whether the folded text of a name equals one of the keys (also its transliteration in the region's profile). */
bool isExactName(const char* name, const char* const* keys, int keyCount, Translit profile) {
    char latin[KEY_BYTES * 2];
    char original[KEY_BYTES * 2];
    char translit[KEY_BYTES * 2];
    foldLatin(name, latin, sizeof(latin));
    trimBlanks(latin);
    bool nonLatin = hasNonLatin(name);
    if (nonLatin) {
        foldOriginal(name, original, sizeof(original));
        transliterate(name, profile, translit, sizeof(translit));
        trimBlanks(original);
        trimBlanks(translit);
    }
    for (int i = 0; i < keyCount; i++) {
        if (strcmp(latin, keys[i]) == 0 || (nonLatin && (strcmp(original, keys[i]) == 0 || strcmp(translit, keys[i]) == 0))) {
            return true;
        }
    }
    return false;
}

/** "Štefánikova 4": the number text of an address record as the street shows it. */
void numberText(const SearchAddress& a, bool slashSecond, SearchIndexReader& reader, char* out, size_t size) {
    if (a.isText) {
        if (!reader.overflowText(a.textOffset, out, size)) {
            out[0] = 0;
        }
        return;
    }
    char letter[3] = { 0, 0, 0 };
    if (a.suffix != 0) {
        letter[0] = static_cast<char>(a.suffix >= 'a' && a.suffix <= 'z' ? a.suffix - 'a' + 'A' : a.suffix);
    }
    if ((a.flags & HOUSE_RANGE) != 0 && a.aux != 0) {
        snprintf(out, size, "%u-%u", a.number, a.aux);
    } else if ((a.flags & HOUSE_SLASH) != 0 && a.aux != 0 && !slashSecond) {
        snprintf(out, size, "%u/%u%s", a.number, a.aux, letter);
    } else if ((a.flags & HOUSE_SLASH) != 0 && a.aux == 0 && letter[0] != 0 && !slashSecond) {
        snprintf(out, size, "%u/%s", a.number, letter);
    } else {
        snprintf(out, size, "%u%s", a.number, letter);
    }
}

// ---------------------------------------------------------------------------------------------------- nearest records

struct NearPoint {
    uint64_t distanceSq;
    SearchPointRecord record;
};

uint64_t distanceSquared(int32_t x, int32_t y, SearchOrigin origin) {
    int64_t dx = static_cast<int64_t>(x) - origin.x;
    int64_t dy = static_cast<int64_t>(y) - origin.y;
    return static_cast<uint64_t>(dx * dx + dy * dy);
}

/** Reads a run of point records in chunks and keeps the k nearest to the origin, nearest first, appended to out. */
bool nearestPoints(SearchIndexReader& reader, uint32_t first, uint32_t count, SearchOrigin origin, size_t k, GrowArray<NearPoint>& out) {
    GrowArray<SearchPointRecord> chunk;
    GrowArray<NearPoint> best;
    for (uint32_t done = 0; done < count; done += RECORD_CHUNK) {
        uint32_t n = std::min<uint32_t>(RECORD_CHUNK, count - done);
        if (!reader.pointRecords(first + done, n, chunk)) {
            return false;
        }
        for (const SearchPointRecord& record : chunk) {
            uint64_t d = distanceSquared(record.x, record.y, origin);
            if (best.size() >= k && d >= best[best.size() - 1].distanceSq) {
                continue;
            }
            NearPoint item = { d, record };
            size_t at;
            if (best.size() < k) {
                best.push(item);
                at = best.size() - 1;
            } else {
                at = k - 1;
                best[at] = item;
            }
            while (at > 0 && best[at - 1].distanceSq > best[at].distanceSq) {
                std::swap(best[at - 1], best[at]);
                at--;
            }
        }
    }
    for (const NearPoint& item : best) {
        out.push(item);
    }
    return !out.failed();
}

// ---------------------------------------------------------------------------------------------------- categories

const char* const LABELS_FOOD[] = { "poi/restaurant/*", "poi/fast_food/*", "poi/bar/*", "poi/beer/*", "poi/ice_cream/*", "poi/bakery/*" };
const char* const LABELS_CAFE[] = { "poi/cafe/*", "poi/shop/coffee" };
const char* const LABELS_SHOP[] = { "poi/shop/*", "poi/clothing_store/*", "poi/alcohol_shop/*", "poi/butcher/*", "poi/grocery/greengrocer",
                                    "poi/grocery/deli", "poi/grocery/marketplace", "poi/grocery/department_store", "poi/bicycle/*",
                                    "poi/music/*" };
const char* const LABELS_SUPERMARKET[] = { "poi/grocery/supermarket", "poi/shop/supermarket", "poi/shop/convenience" };
const char* const LABELS_PHARMACY[] = { "poi/pharmacy/*" };
const char* const LABELS_TOILETS[] = { "poi/toilets/*" };
const char* const LABELS_STOP[] = { "poi/bus/*", "poi/railway/tram_stop" };
const char* const LABELS_PARKING[] = { "poi/parking/*" };
const char* const LABELS_FUEL[] = { "poi/fuel/*" };
const char* const LABELS_BANK[] = { "poi/atm/*", "poi/bank/*" };
const char* const LABELS_HOSPITAL[] = { "poi/hospital/*" };
const char* const LABELS_VIEWPOINT[] = { "poi/attraction/viewpoint" };

#define CATEGORY(name, labels) { name, labels, static_cast<uint8_t>(sizeof(labels) / sizeof(labels[0])) }
const SearchCategory CATEGORIES[] = {
    CATEGORY("Food and drink", LABELS_FOOD), CATEGORY("Cafe", LABELS_CAFE),         CATEGORY("Shops", LABELS_SHOP),
    CATEGORY("Supermarket", LABELS_SUPERMARKET), CATEGORY("Pharmacy", LABELS_PHARMACY), CATEGORY("Toilets", LABELS_TOILETS),
    CATEGORY("Bus or tram stop", LABELS_STOP), CATEGORY("Parking", LABELS_PARKING),   CATEGORY("Fuel and charging", LABELS_FUEL),
    CATEGORY("Bank or ATM", LABELS_BANK),     CATEGORY("Hospital", LABELS_HOSPITAL),  CATEGORY("Viewpoint", LABELS_VIEWPOINT),
};
#undef CATEGORY

/** A pattern that ends in "/" and an asterisk covers every subclass of the class; any other pattern must be equal. */
bool labelMatches(const char* pattern, const char* label) {
    size_t length = strlen(pattern);
    if (length >= 2 && pattern[length - 1] == '*' && pattern[length - 2] == '/') {
        return strncmp(label, pattern, length - 1) == 0;
    }
    return strcmp(label, pattern) == 0;
}

} // namespace

/** The typed text prepared for lookups. */
struct SearchEngine::Query {
    char keys[2][KEY_BYTES];
    int keyCount;
    QueryReading readings[3];
    int readingCount;
};

// ---------------------------------------------------------------------------------------------------- registry

SearchEngine::SearchEngine() = default;

SearchEngine::~SearchEngine() {
    removeAll();
}

int SearchEngine::addIndex(SearchFile* file, const char* regionName, Translit profile, bool slashSecond) {
    if (slotCount >= SEARCH_MAX_INDEXES || file == nullptr) {
        return -1;
    }
    Slot* slot = new (std::nothrow) Slot();
    if (slot == nullptr) {
        return -1;
    }
    if (!slot->reader.open(file)) {
        delete slot;
        return -1;
    }
    slot->file = file;
    copyText(slot->region, sizeof(slot->region), regionName != nullptr ? regionName : "");
    slot->profile = profile;
    slot->slashSecond = slashSecond;
    int index = slotCount++;
    slots[index] = slot;
    order[index] = static_cast<uint8_t>(index);
    return index;
}

void SearchEngine::removeAll() {
    for (int i = 0; i < slotCount; i++) {
        delete slots[i];
        slots[i] = nullptr;
    }
    slotCount = 0;
    ranked.release();
    cursor = 0;
}

const char* SearchEngine::regionName(int slot) const {
    return slot >= 0 && slot < slotCount ? slots[slot]->region : "";
}

const SearchIndexReader* SearchEngine::reader(int slot) const {
    return slot >= 0 && slot < slotCount ? &slots[slot]->reader : nullptr;
}

void SearchEngine::reorder(const int* wanted, int count) {
    uint8_t next[SEARCH_MAX_INDEXES];
    bool used[SEARCH_MAX_INDEXES] = {};
    int n = 0;
    for (int i = 0; i < count && n < slotCount; i++) {
        if (wanted[i] >= 0 && wanted[i] < slotCount && !used[wanted[i]]) {
            used[wanted[i]] = true;
            next[n++] = static_cast<uint8_t>(wanted[i]);
        }
    }
    for (int i = 0; i < slotCount; i++) {
        int slot = order[i];
        if (!used[slot]) {
            used[slot] = true;
            next[n++] = static_cast<uint8_t>(slot);
        }
    }
    for (int i = 0; i < slotCount; i++) {
        order[i] = next[i];
    }
}

bool SearchEngine::loadResident(int slot, bool nameGroups, bool keyPages) {
    return slot >= 0 && slot < slotCount && slots[slot]->reader.loadResident(nameGroups, keyPages);
}

void SearchEngine::dropResident() {
    for (int i = 0; i < slotCount; i++) {
        slots[i]->reader.loadResident(false, false);
    }
}

void SearchEngine::setCandidateLimit(uint32_t limit) {
    candidateLimit = std::max<uint32_t>(8, std::min<uint32_t>(limit, 512));
}

int SearchEngine::priorityOf(int slotIndex) const {
    for (int i = 0; i < slotCount; i++) {
        if (order[i] == slotIndex) {
            return i;
        }
    }
    return slotCount;
}

const SearchCategory* SearchEngine::categories(int* count) {
    if (count != nullptr) {
        *count = static_cast<int>(sizeof(CATEGORIES) / sizeof(CATEGORIES[0]));
    }
    return CATEGORIES;
}

// ---------------------------------------------------------------------------------------------------- candidates

void SearchEngine::startSearch(SearchOrigin origin) {
    currentOrigin = origin;
    ranked.clear();
    cursor = 0;
    double n = PI_VALUE * (1.0 - 2.0 * static_cast<double>(origin.y) / WORLD_UNITS);
    double latitude = std::atan(std::sinh(n));
    unitMetres = 40075016.686 * std::cos(latitude) / WORLD_UNITS;
    for (int i = 0; i < slotCount; i++) {
        slots[i]->reader.resetIo();
    }
}

void SearchEngine::setGeo(SearchResult& result, uint64_t& distanceSq) const {
    distanceSq = distanceSquared(result.x, result.y, currentOrigin);
    result.distanceM = static_cast<uint32_t>(std::sqrt(static_cast<double>(distanceSq)) * unitMetres + 0.5);
    double dx = static_cast<double>(result.x) - currentOrigin.x;
    double dy = static_cast<double>(result.y) - currentOrigin.y;
    double degrees = std::atan2(dx, -dy) * 180.0 / PI_VALUE;
    if (degrees < 0) {
        degrees += 360.0;
    }
    result.bearing = static_cast<uint16_t>(static_cast<int>(degrees + 0.5) % 360);
}

namespace {

/** A new candidate for a point: filled by the caller further. Declared here because Candidate is private. */
template <typename T>
void zero(T& value) {
    memset(static_cast<void*>(&value), 0, sizeof(T));
}

} // namespace

void SearchEngine::rankCandidates(size_t cap) {
    const size_t total = ranked.size();
    GrowArray<uint32_t> sorted;
    if (!sorted.resize(total)) {
        return;
    }
    for (size_t i = 0; i < total; i++) {
        sorted[i] = static_cast<uint32_t>(i);
    }
    const Candidate* items = ranked.begin();
    std::sort(sorted.begin(), sorted.end(), [items](uint32_t a, uint32_t b) {
        const Candidate& x = items[a];
        const Candidate& y = items[b];
        if (x.result.group != y.result.group) {
            return x.result.group < y.result.group;
        }
        if (x.rank != y.rank) {
            return x.rank < y.rank;
        }
        if (x.distanceSq != y.distanceSq) {
            return x.distanceSq < y.distanceSq;
        }
        if (x.priority != y.priority) {
            return x.priority < y.priority;
        }
        return a < b;
    });
    GrowArray<Candidate> kept;
    kept.reserve(std::min(total, cap));
    for (size_t s = 0; s < total && kept.size() < cap; s++) {
        const Candidate& c = items[sorted[s]];
        bool duplicate = false;
        for (size_t k = 0; k < kept.size() && !duplicate; k++) {
            Candidate& other = kept[k];
            if (other.result.group != c.result.group || strcmp(other.result.type, c.result.type) != 0) {
                continue;
            }
            bool names = other.nameLazy || c.nameLazy || strcmp(other.result.name, c.result.name) == 0;
            if (!names) {
                continue;
            }
            if (std::abs(other.result.x - c.result.x) <= DUPLICATE_UNITS && std::abs(other.result.y - c.result.y) <= DUPLICATE_UNITS) {
                duplicate = true;
            } else if (c.result.hasBox && other.result.hasBox && other.result.slot != c.result.slot &&
                       c.result.minX <= other.result.maxX && c.result.maxX >= other.result.minX &&
                       c.result.minY <= other.result.maxY && c.result.maxY >= other.result.minY) {
                // the same street cut by a block border: one result with the union of the boxes
                other.result.minX = std::min(other.result.minX, c.result.minX);
                other.result.minY = std::min(other.result.minY, c.result.minY);
                other.result.maxX = std::max(other.result.maxX, c.result.maxX);
                other.result.maxY = std::max(other.result.maxY, c.result.maxY);
                duplicate = true;
            }
        }
        if (!duplicate) {
            kept.push(c);
        }
    }
    ranked.swap(kept);
}

bool SearchEngine::materialise(Candidate& candidate, SearchResult& out) {
    if (candidate.nameLazy) {
        SearchNg group;
        if (candidate.result.slot < slotCount && slots[candidate.result.slot]->reader.ng(candidate.ng, group)) {
            copyText(candidate.result.name, sizeof(candidate.result.name), group.name);
        } else {
            copyText(candidate.result.name, sizeof(candidate.result.name), candidate.result.type);
        }
        candidate.nameLazy = false;
    }
    if (!candidate.isPlace) {
        findPlace(candidate.result);
        candidate.isPlace = true;
    }
    out = candidate.result;
    return true;
}

void SearchEngine::loadPlaces(Slot& slot) {
    slot.placesLoaded = true;
    for (uint32_t pair = 0; pair < slot.reader.info().classCount; pair++) {
        char label[96];
        uint32_t first, count;
        if (!slot.reader.classPair(pair, label, sizeof(label), first, count) || count == 0 || strncmp(label, "place/", 6) != 0) {
            continue;
        }
        static const struct {
            const char* name;
            PlaceClass placeClass;
            PlacePoint::Level level;
        } SETTLEMENTS[] = {
            { "city", PlaceClass::City, PlacePoint::Big },          { "town", PlaceClass::Town, PlacePoint::Big },
            { "village", PlaceClass::Village, PlacePoint::Small }, { "hamlet", PlaceClass::Hamlet, PlacePoint::Small },
            { "suburb", PlaceClass::Suburb, PlacePoint::Suburb },
        };
        // "place/city" or, when the pairs were folded, "place/city/*"
        const char* classText = label + 6;
        const char* slash = strchr(classText, '/');
        const size_t classLength = slash != nullptr ? static_cast<size_t>(slash - classText) : strlen(classText);
        float scale = 0.0f;
        PlacePoint::Level level = PlacePoint::Small;
        for (const auto& settlement : SETTLEMENTS) {
            if (strlen(settlement.name) == classLength && strncmp(classText, settlement.name, classLength) == 0) {
                scale = placeClassScale(settlement.placeClass);
                level = settlement.level;
            }
        }
        if (scale == 0.0f) {
            continue;
        }
        for (uint32_t done = 0; done < count; done += RECORD_CHUNK) {
            GrowArray<SearchPointRecord> records;
            if (!slot.reader.pointRecords(first + done, std::min(RECORD_CHUNK, count - done), records)) {
                break;
            }
            for (const SearchPointRecord& record : records) {
                if (record.ng != SIDX_UNNAMED) {
                    slot.places.push({ record.x, record.y, record.ng, scale, level });
                }
            }
        }
    }
}

SearchEngine::BestPlace SearchEngine::bestPlace(int32_t x, int32_t y, PlacePoint::Level from, PlacePoint::Level to, float limit) {
    BestPlace best;
    float bestScore = limit;
    for (int s = 0; s < slotCount; s++) {
        Slot& slot = *slots[s];
        if (!slot.placesLoaded) {
            loadPlaces(slot);
        }
        for (const PlacePoint& place : slot.places) {
            if (place.level < from || place.level > to) {
                continue;
            }
            const double dx = static_cast<double>(place.x) - x;
            const double dy = static_cast<double>(place.y) - y;
            const auto score = static_cast<float>(std::sqrt(dx * dx + dy * dy) * unitMetres / place.scale);
            if (score < bestScore) {
                bestScore = score;
                best.slot = s;
                best.ng = place.ng;
                best.level = place.level;
            }
        }
    }
    return best;
}

void SearchEngine::findPlace(SearchResult& result) {
    result.place[0] = 0;
    const BestPlace settlement = bestPlace(result.x, result.y, PlacePoint::Big, PlacePoint::Small, PLACE_SCORE_LIMIT);
    SearchNg group;
    if (settlement.slot < 0 || !slots[settlement.slot]->reader.ng(settlement.ng, group)) {
        return;
    }
    char text[2 * SIDX_MAX_NAME + 4];
    copyText(text, sizeof(text), group.name);
    if (settlement.level == PlacePoint::Big) {
        // Inside a city or town the suburb tells more: "Ružinov, Bratislava"
        const BestPlace suburb = bestPlace(result.x, result.y, PlacePoint::Suburb, PlacePoint::Suburb, SUBURB_SCORE_LIMIT);
        SearchNg suburbGroup;
        if (suburb.slot >= 0 && slots[suburb.slot]->reader.ng(suburb.ng, suburbGroup) && strcmp(suburbGroup.name, group.name) != 0) {
            snprintf(text, sizeof(text), "%s, %s", suburbGroup.name, group.name);
        }
    }
    // "Holice 238" (a village address) and the village's own street already name the place
    const size_t length = strlen(group.name);
    if (length == 0 || strncmp(result.name, group.name, length) == 0 || strncmp(result.name, text, strlen(text)) == 0) {
        return;
    }
    copyText(result.place, sizeof(result.place), text);
}

int SearchEngine::takeResults(SearchResult* out, int max) {
    int n = 0;
    while (n < max && cursor < ranked.size()) {
        materialise(ranked[cursor], out[n]);
        n++;
        cursor++;
    }
    return n;
}

int SearchEngine::more(SearchResult* out, int max) {
    return takeResults(out, max);
}

// ---------------------------------------------------------------------------------------------------- address search

bool SearchEngine::collectAddresses(const Query& query, SearchOrigin) {
    for (int r = 0; r < query.readingCount; r++) {
        const QueryReading& reading = query.readings[r];
        if (reading.number[0] == 0) {
            continue;
        }
        HouseNumber number;
        if (!parseHouseNumber(reading.number, strlen(reading.number), number)) {
            continue;
        }
        char keys[2][KEY_BYTES];
        int keyCount = makeKeys(reading.street, keys);
        if (characterCount(keys[0]) < MIN_CHARACTERS) {
            continue;
        }
        const size_t before = ranked.size();
        for (int p = 0; p < slotCount; p++) {
            const int slotIndex = order[p];
            Slot& slot = *slots[slotIndex];
            uint16_t wanted = number.number;
            if ((number.flags & HOUSE_SLASH) != 0 && number.aux != 0 && slot.slashSecond) {
                wanted = number.aux;
            }
            int suffix = number.suffix != 0 ? number.suffix : -1;
            for (int k = 0; k < keyCount; k++) {
                GrowArray<SearchAddressHit> hits;
                if (!slot.reader.searchAddress(keys[k], wanted, suffix, 20, hits)) {
                    continue;
                }
                for (const SearchAddressHit& hit : hits) {
                    Candidate c;
                    zero(c);
                    char text[40];
                    numberText(hit.address, slot.slashSecond, slot.reader, text, sizeof(text));
                    snprintf(c.result.name, sizeof(c.result.name), "%s %s", hit.street, text);
                    copyText(c.result.type, sizeof(c.result.type), "Address");
                    c.result.x = hit.address.x;
                    c.result.y = hit.address.y;
                    c.result.minX = c.result.maxX = c.result.x;
                    c.result.minY = c.result.maxY = c.result.y;
                    c.result.group = ResultGroup::Address;
                    c.result.slot = static_cast<uint8_t>(slotIndex);
                    c.result.lowConfidence = (hit.address.flags & ADDRESS_LOW) != 0;
                    c.result.villageEstimated = (hit.address.flags & ADDRESS_VILLAGE) != 0;
                    c.rank = (suffix < 0 && hit.address.suffix != 0) ? 1 : 0;
                    c.priority = static_cast<uint8_t>(p);
                    setGeo(c.result, c.distanceSq);
                    ranked.push(c);
                }
            }
        }
        if (ranked.size() > before) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------- name search

void SearchEngine::collectNames(const char* const* keys, int keyCount, SearchOrigin origin) {
    GrowArray<uint32_t> found;
    GrowArray<uint32_t> buffer;
    buffer.resize(candidateLimit);
    for (int p = 0; p < slotCount; p++) {
        const int slotIndex = order[p];
        Slot& slot = *slots[slotIndex];
        found.clear();
        for (int k = 0; k < keyCount; k++) {
            uint32_t n = slot.reader.prefix(keys[k], candidateLimit, buffer.begin());
            for (uint32_t i = 0; i < n; i++) {
                bool seen = false;
                for (uint32_t f : found) {
                    seen = seen || f == buffer[i];
                }
                if (!seen) {
                    found.push(buffer[i]);
                }
            }
        }
        for (uint32_t ngOffset : found) {
            SearchNg group;
            if (!slot.reader.ng(ngOffset, group)) {
                continue;
            }
            const bool exact = isExactName(group.name, keys, keyCount, slot.profile);
            if (group.kind == SearchKind::Category) {
                if (!exact) {
                    continue;
                }
                GrowArray<NearPoint> nearest;
                if (!nearestPoints(slot.reader, group.first, group.count, origin, CATEGORY_POINTS, nearest)) {
                    continue;
                }
                for (const NearPoint& item : nearest) {
                    Candidate c;
                    zero(c);
                    char label[96];
                    uint32_t first, count;
                    slot.reader.classPair(item.record.pair, label, sizeof(label), first, count);
                    c.isPlace = describeLabel(label, c.result.type, sizeof(c.result.type)) == ResultGroup::Place;
                    c.result.x = item.record.x;
                    c.result.y = item.record.y;
                    c.result.minX = c.result.maxX = c.result.x;
                    c.result.minY = c.result.maxY = c.result.y;
                    c.result.group = ResultGroup::Category;
                    c.result.slot = static_cast<uint8_t>(slotIndex);
                    c.priority = static_cast<uint8_t>(p);
                    if (item.record.ng == SIDX_UNNAMED) {
                        copyText(c.result.name, sizeof(c.result.name), c.result.type);
                    } else {
                        c.nameLazy = true;
                        c.ng = item.record.ng;
                    }
                    setGeo(c.result, c.distanceSq);
                    ranked.push(c);
                }
            } else if (group.kind == SearchKind::Point) {
                GrowArray<NearPoint> nearest;
                if (!nearestPoints(slot.reader, group.first, group.count, origin, NAME_POINTS, nearest)) {
                    continue;
                }
                for (const NearPoint& item : nearest) {
                    Candidate c;
                    zero(c);
                    char label[96];
                    uint32_t first, count;
                    slot.reader.classPair(item.record.pair, label, sizeof(label), first, count);
                    ResultGroup kind = describeLabel(label, c.result.type, sizeof(c.result.type));
                    c.isPlace = kind == ResultGroup::Place;
                    copyText(c.result.name, sizeof(c.result.name), group.name);
                    c.result.x = item.record.x;
                    c.result.y = item.record.y;
                    c.result.minX = c.result.maxX = c.result.x;
                    c.result.minY = c.result.maxY = c.result.y;
                    c.result.group = exact ? ResultGroup::ExactName : kind;
                    c.result.slot = static_cast<uint8_t>(slotIndex);
                    c.priority = static_cast<uint8_t>(p);
                    setGeo(c.result, c.distanceSq);
                    ranked.push(c);
                }
            } else {
                GrowArray<SearchStreetRecord> records;
                if (!slot.reader.streetRecords(group.first, std::min<uint32_t>(group.count, 512), records)) {
                    continue;
                }
                GrowArray<Candidate> streets;
                for (const SearchStreetRecord& record : records) {
                    Candidate c;
                    zero(c);
                    copyText(c.result.name, sizeof(c.result.name), group.name);
                    copyText(c.result.type, sizeof(c.result.type), "Street");
                    c.result.x = record.x;
                    c.result.y = record.y;
                    c.result.minX = record.minX;
                    c.result.minY = record.minY;
                    c.result.maxX = record.maxX;
                    c.result.maxY = record.maxY;
                    c.result.hasBox = true;
                    c.result.group = exact ? ResultGroup::ExactName : ResultGroup::Street;
                    c.result.slot = static_cast<uint8_t>(slotIndex);
                    c.priority = static_cast<uint8_t>(p);
                    setGeo(c.result, c.distanceSq);
                    streets.push(c);
                }
                std::sort(streets.begin(), streets.end(), [](const Candidate& a, const Candidate& b) { return a.distanceSq < b.distanceSq; });
                for (size_t i = 0; i < streets.size() && i < STREET_CLUSTERS; i++) {
                    ranked.push(streets[i]);
                }
            }
        }
        if (ranked.size() > PRUNE_AT) {
            rankCandidates(MAX_RANKED);
        }
    }
}

bool SearchEngine::prefixExists(const char* key) {
    uint32_t ref;
    for (int p = 0; p < slotCount; p++) {
        if (slots[order[p]]->reader.prefix(key, 1, &ref) > 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------- search

void SearchEngine::fillStats(SearchStats* stats, uint64_t startUs, bool short_) {
    if (stats == nullptr) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    for (int i = 0; i < slotCount; i++) {
        SearchIoStats io = slots[i]->reader.io();
        stats->readCalls += io.calls;
        stats->readBlocks += io.blocks;
        stats->readRuns += io.runs;
    }
    stats->total = static_cast<uint32_t>(ranked.size());
    for (const Candidate& c : ranked) {
        stats->groupCount[static_cast<int>(c.result.group)]++;
    }
    stats->indexesSearched = static_cast<uint32_t>(slotCount);
    stats->tooShort = short_;
    if (clock != nullptr) {
        stats->elapsedUs = static_cast<uint32_t>(clock() - startUs);
    }
}

int SearchEngine::search(const char* typed, SearchOrigin origin, SearchResult* out, int max, SearchStats* stats) {
    const uint64_t startUs = clock != nullptr ? clock() : 0;
    startSearch(origin);
    Query query;
    memset(&query, 0, sizeof(query));
    query.keyCount = makeKeys(typed != nullptr ? typed : "", query.keys);
    query.readingCount = parseQuery(typed != nullptr ? typed : "", query.readings);
    const bool shortText = characterCount(query.keys[0]) < MIN_CHARACTERS;
    bool backedOff = false;
    char usedKey[KEY_BYTES];
    copyText(usedKey, sizeof(usedKey), query.keys[0]);

    if (!shortText && slotCount > 0) {
        if (!collectAddresses(query, origin)) {
            const char* keys[2] = { query.keys[0], query.keys[1] };
            collectNames(keys, query.keyCount, origin);
        }
        if (ranked.size() == 0) {
            // the longest prefix that still has results: the existence of a prefix only goes one way, so bisect
            const char* primary = query.keys[0];
            int lo = MIN_CHARACTERS, hi = characterCount(primary) - 1, best = 0;
            char prefix[KEY_BYTES];
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                size_t length = bytesOfCharacters(primary, mid);
                memcpy(prefix, primary, length);
                prefix[length] = 0;
                trimBlanks(prefix);
                if (characterCount(prefix) >= MIN_CHARACTERS && prefixExists(prefix)) {
                    best = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            if (best > 0) {
                size_t length = bytesOfCharacters(primary, best);
                memcpy(prefix, primary, length);
                prefix[length] = 0;
                trimBlanks(prefix);
                const char* keys[1] = { prefix };
                collectNames(keys, 1, origin);
                backedOff = ranked.size() > 0;
                copyText(usedKey, sizeof(usedKey), prefix);
            }
        }
        rankCandidates(MAX_RANKED);
    }
    fillStats(stats, startUs, shortText);
    if (stats != nullptr) {
        stats->backedOff = backedOff;
        copyText(stats->key, sizeof(stats->key), usedKey);
        if (backedOff) {
            snprintf(stats->header, sizeof(stats->header), "Showing results for '%s'", usedKey);
        }
    }
    return takeResults(out, max);
}

// ---------------------------------------------------------------------------------------------------- nearby

int SearchEngine::nearby(int category, SearchOrigin origin, SearchResult* out, int max, SearchStats* stats) {
    const uint64_t startUs = clock != nullptr ? clock() : 0;
    startSearch(origin);
    const int categoryTotal = static_cast<int>(sizeof(CATEGORIES) / sizeof(CATEGORIES[0]));
    if (category >= 0 && category < categoryTotal && max > 0) {
        const SearchCategory& wanted = CATEGORIES[category];
        const size_t k = static_cast<size_t>(std::min(max, 64));
        for (int p = 0; p < slotCount; p++) {
            const int slotIndex = order[p];
            Slot& slot = *slots[slotIndex];
            for (uint32_t pair = 0; pair < slot.reader.info().classCount; pair++) {
                char label[96];
                uint32_t first, count;
                if (!slot.reader.classPair(pair, label, sizeof(label), first, count) || count == 0) {
                    continue;
                }
                bool match = false;
                for (int l = 0; l < wanted.labelCount && !match; l++) {
                    match = labelMatches(wanted.labels[l], label);
                }
                if (!match) {
                    continue;
                }
                GrowArray<NearPoint> nearest;
                if (!nearestPoints(slot.reader, first, count, origin, k, nearest)) {
                    continue;
                }
                for (const NearPoint& item : nearest) {
                    Candidate c;
                    zero(c);
                    c.isPlace = describeLabel(label, c.result.type, sizeof(c.result.type)) == ResultGroup::Place;
                    c.result.x = item.record.x;
                    c.result.y = item.record.y;
                    c.result.minX = c.result.maxX = c.result.x;
                    c.result.minY = c.result.maxY = c.result.y;
                    c.result.group = ResultGroup::Nearby;
                    c.result.slot = static_cast<uint8_t>(slotIndex);
                    c.priority = static_cast<uint8_t>(p);
                    if (item.record.ng == SIDX_UNNAMED) {
                        copyText(c.result.name, sizeof(c.result.name), c.result.type);
                    } else {
                        c.nameLazy = true;
                        c.ng = item.record.ng;
                    }
                    setGeo(c.result, c.distanceSq);
                    ranked.push(c);
                }
            }
            if (ranked.size() > PRUNE_AT) {
                rankCandidates(MAX_RANKED);
            }
        }
        rankCandidates(std::max<size_t>(k, 64));
    }
    // the reads of the names are part of the measurement: the stats come after taking the results
    int n = takeResults(out, max);
    if (stats != nullptr) {
        fillStats(stats, startUs, false);
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------- recent searches

void searchTypeLine(const char* type, const char* place, char* out, size_t size) {
    if (place != nullptr && place[0] != 0) {
        char text[SEARCH_LINE_BYTES + SEARCH_PLACE_BYTES];
        snprintf(text, sizeof(text), "%s \xC2\xB7 %s", type, place);
        copyText(out, size, text);
    } else {
        copyText(out, size, type);
    }
}

void RecentSearches::add(const SearchResult& result) {
    Entry entry = {};
    copyText(entry.name, sizeof(entry.name), result.name);
    searchTypeLine(result.type, result.place, entry.type, sizeof(entry.type));
    entry.x = result.x;
    entry.y = result.y;
    int at = used;
    for (int i = 0; i < used; i++) {
        if (strcmp(entries[i].name, entry.name) == 0 && entries[i].x == entry.x && entries[i].y == entry.y) {
            at = i;
            break;
        }
    }
    if (at == used) {
        if (used < CAPACITY) {
            used++;
        }
        at = used - 1;
    }
    for (int i = at; i > 0; i--) {
        entries[i] = entries[i - 1];
    }
    entries[0] = entry;
}

bool RecentSearches::save(WriteBytes write, void* context) const {
    char text[CAPACITY * (SEARCH_NAME_BYTES + SEARCH_LINE_BYTES + 32) + 32];
    size_t length = static_cast<size_t>(snprintf(text, sizeof(text), "recent 1\n"));
    for (int i = 0; i < used; i++) {
        char name[SEARCH_NAME_BYTES];
        char type[SEARCH_LINE_BYTES];
        copyText(name, sizeof(name), entries[i].name);
        copyText(type, sizeof(type), entries[i].type);
        for (char* s : { name, type }) {
            for (; *s != 0; s++) {
                if (*s == '\t' || *s == '\n' || *s == '\r') {
                    *s = ' ';
                }
            }
        }
        int n = snprintf(text + length, sizeof(text) - length, "%ld\t%ld\t%s\t%s\n", static_cast<long>(entries[i].x), static_cast<long>(entries[i].y), type, name);
        if (n < 0 || length + static_cast<size_t>(n) >= sizeof(text)) {
            return false;
        }
        length += static_cast<size_t>(n);
    }
    return write(context, text, length);
}

bool RecentSearches::load(ReadBytes read, void* context) {
    used = 0;
    char text[CAPACITY * (SEARCH_NAME_BYTES + SEARCH_LINE_BYTES + 32) + 32];
    size_t length = 0;
    while (length + 1 < sizeof(text)) {
        size_t got = read(context, text + length, sizeof(text) - 1 - length);
        if (got == 0) {
            break;
        }
        length += got;
    }
    text[length] = 0;
    char* line = text;
    char* end = strchr(line, '\n');
    if (end == nullptr || strncmp(line, "recent 1", 8) != 0) {
        return false;
    }
    line = end + 1;
    while (*line != 0 && used < CAPACITY) {
        end = strchr(line, '\n');
        if (end != nullptr) {
            *end = 0;
        }
        char* fields[4];
        int n = 0;
        char* at = line;
        fields[n++] = at;
        while (n < 4 && (at = strchr(at, '\t')) != nullptr) {
            *at++ = 0;
            fields[n++] = at;
        }
        if (n == 4) {
            Entry entry = {};
            entry.x = atoi(fields[0]);
            entry.y = atoi(fields[1]);
            copyText(entry.type, sizeof(entry.type), fields[2]);
            copyText(entry.name, sizeof(entry.name), fields[3]);
            entries[used++] = entry;
        }
        if (end == nullptr) {
            break;
        }
        line = end + 1;
    }
    return true;
}

} // namespace maps
