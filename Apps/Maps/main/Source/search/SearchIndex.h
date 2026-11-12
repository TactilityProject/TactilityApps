// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../route/GrowArray.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * The search index file (.sidx), format version 1. It is the C++ port of host/search/sidx.py (M6 study, section 3).
 * All numbers are little endian.
 *
 *   header (128 bytes) | class table | fence table | key pages | name groups | point records | street records |
 *   address records | overflow strings
 *
 *  - header: "SIDX", u16 version (1), u16 point size (4, or 8 for a wide region), i32 origin x, i32 origin y,
 *    u32 class count, fence count, key page count, point count, street record count, u32 address count, u32 0, then
 *    eight (u32 offset, u32 size) pairs for the sections above, in that order. The rest is zero.
 *  - point "PT": (u16 x, u16 y) relative to the origin, one unit = one z14 tile unit (~0.4 m at 48 N). When the
 *    region is 65536 units or wider, a PT is (i32 x, i32 y) and the point size is 8 ("wide mode").
 *  - class table, per class pair: u8 length, label ("poi/cafe/cafe", "place/town"), u32 first point record, u32 count.
 *  - fence table: u8 length, key bytes, for the first key of every key page.
 *  - key page: 4096 bytes of entries { u8 key length (1..40), key bytes, u24 name group offset }, sorted by key bytes,
 *    a zero length ends the page, no entry straddles a page. A key is cut at 40 bytes on a UTF-8 character boundary.
 *  - name group ("NG"): u8 name length, name (UTF-8, at most 255 bytes, cut on a character boundary), u8 kind
 *    (0 points, 1 streets, 2 category), u32 first record, u16 count (records in the point or street table).
 *  - point record: PT, u8 class pair, u24 NG offset (0xFFFFFF for an unnamed point). Sorted by (pair, named first,
 *    NG, tile), so a whole class pair is one run.
 *  - street record: PT representative point, PT box minimum, PT box maximum, u32 first address, u16 address count.
 *  - address record: u16 number, u16 aux, u8 suffix, u8 flags, PT. Sorted per street record by (number, aux, suffix).
 *    A house number that does not fit the number grammar (parseHouseNumber) is stored as text in the overflow strings:
 *    number = 0xFFFF, flags = HOUSE_TEXT, and the text's offset in the overflow section is 24 bits: the low 16 bits
 *    in aux, the high 8 bits in the suffix byte (version 1 of sidx.py wrapped at 64 KB here). Offsets are limited to
 *    16 MB, the builder fails when exceeded.
 *  - overflow strings: UTF-8 text, each ended by a zero byte.
 *
 * Counts keep the field widths above (u16 for an NG's records, a street's clusters and a cluster's addresses); the
 * writer fails with an error text instead of capping a count.
 */
constexpr uint32_t SIDX_VERSION = 1;
constexpr uint32_t SIDX_HEADER_SIZE = 128;
constexpr uint32_t SIDX_PAGE_SIZE = 4096;
constexpr uint32_t SIDX_MAX_KEY = 40;
constexpr uint32_t SIDX_MAX_NAME = 255;
constexpr uint32_t SIDX_UNNAMED = 0xFFFFFF;
constexpr uint32_t SIDX_MAX_OFFSET24 = 0xFFFFFF;
constexpr uint32_t SIDX_SECTION_COUNT = 8;

/** The flag of an address record whose number is text in the overflow strings (HOUSE_SLASH = 1, HOUSE_RANGE = 2). */
constexpr uint8_t HOUSE_TEXT = 4;
/**
 * Flags the caller sets through SearchAddressInput::flags (added to version 1, which no device has released yet):
 * ADDRESS_LOW marks an address whose street is 50 to 100 m from the house (low confidence). ADDRESS_VILLAGE marks an
 * address stored under a settlement: the street record of such a place is an ordinary one named like the place, with
 * one cluster whose representative point and box are the place's position (a place may share the name group of a real
 * street of the same name), and every address under it carries ADDRESS_VILLAGE.
 */
constexpr uint8_t ADDRESS_LOW = 8;
constexpr uint8_t ADDRESS_VILLAGE = 16;

enum class SidxSection : uint8_t { Class, Fence, Keys, Ng, Points, Streets, Addresses, Overflow };
enum class SearchKind : uint8_t { Point = 0, Street = 1, Category = 2 };

/** Cuts a key to at most SIDX_MAX_KEY bytes without splitting a UTF-8 character. Returns the new length. */
uint32_t truncateKey(const char* key, size_t length);
/** The same for a name group's name (SIDX_MAX_NAME). */
uint32_t truncateName(const char* name, size_t length);
/** Compares byte strings like Python bytes: content, then the shorter first (the key order). */
int compareSearchBytes(const uint8_t* a, uint32_t aLength, const uint8_t* b, uint32_t bLength);
/** The key of a category name made of ASCII: lowercase, other characters a single blank (the writer's rule). */
uint32_t foldCategoryName(const char* name, char* out, size_t capacity);
/**
 * A house number as an address record stores it (number, aux, suffix, flags; "x/y" swapped when slashSecond).
 * False when it must be stored as text in the overflow strings.
 */
bool indexHouseNumber(const char* text, bool slashSecond, uint16_t& number, uint16_t& aux, uint8_t& suffix, uint8_t& flags);

// ------------------------------------------------------------------------------------------------ encoders

/** The parts of the header. */
struct SearchHeader {
    uint16_t pointBytes;
    int32_t originX;
    int32_t originY;
    uint32_t classCount;
    uint32_t fenceCount;
    uint32_t pageCount;
    uint32_t pointCount;
    uint32_t streetCount;
    uint32_t addressCount;
    uint32_t sectionOffset[SIDX_SECTION_COUNT];
    uint32_t sectionSize[SIDX_SECTION_COUNT];
};

/** The sizes of the records for a point size. */
inline uint32_t pointRecordBytes(uint32_t pointBytes) { return pointBytes + 4; }
inline uint32_t streetRecordBytes(uint32_t pointBytes) { return 3 * pointBytes + 6; }
inline uint32_t addressRecordBytes(uint32_t pointBytes) { return 6 + pointBytes; }

/** Writes the 128-byte header. */
void encodeHeader(const SearchHeader& header, uint8_t out[SIDX_HEADER_SIZE]);
/** Writes a PT (x, y relative to the origin); returns the point size. */
uint32_t encodePt(uint8_t* out, uint32_t pointBytes, int32_t relX, int32_t relY);
/**
 * Appends one key entry to a key page being filled (page is SIDX_PAGE_SIZE bytes, used = bytes so far, starting at
 * 0). Returns false and changes nothing when the entry and the ending zero byte do not fit. The key must be cut
 * (truncateKey) and not empty.
 */
bool encodeKeyPage(uint8_t* page, uint32_t& used, const uint8_t* key, uint32_t keyLength, uint32_t ngOffset);
/** One name group; returns its size (1 + name length + 1 + 4 + 2). The name must be cut (truncateName). */
uint32_t encodeNameGroup(uint8_t* out, const char* name, uint32_t nameLength, SearchKind kind, uint32_t first, uint16_t count);
/** One point record; returns its size. */
uint32_t encodePoint(uint8_t* out, uint32_t pointBytes, int32_t relX, int32_t relY, uint8_t pair, uint32_t ngOffset);
/** One street record; returns its size. */
uint32_t encodeStreet(uint8_t* out, uint32_t pointBytes, const int32_t rep[2], const int32_t boxMin[2], const int32_t boxMax[2],
                      uint32_t firstAddress, uint16_t addressCount);
/** One address record; returns its size. */
uint32_t encodeAddress(uint8_t* out, uint32_t pointBytes, uint16_t number, uint16_t aux, uint8_t suffix, uint8_t flags, int32_t relX,
                       int32_t relY);
/** An address record for text: the offset (below 2^24) goes into aux and the suffix byte. */
uint32_t encodeTextAddress(uint8_t* out, uint32_t pointBytes, uint32_t textOffset, int32_t relX, int32_t relY);

// ------------------------------------------------------------------------------------------------ writer

/** Where the writer puts the file's bytes, in order. The host tool writes a file, the device writes through its I/O task. */
class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual bool append(const void* data, uint32_t size) = 0;
};

/** One address of a street cluster. */
struct SearchAddressInput {
    /** The house number as in the data ("12/A", "9 к3"). */
    const char* number;
    int32_t x;
    int32_t y;
    /** ADDRESS_LOW, ADDRESS_VILLAGE or 0; stored in the record's flags next to the number's own flags. */
    uint8_t flags;
};

/** One place of a street name: the representative vertex, the box and the house numbers assigned to it. */
struct SearchClusterInput {
    int32_t repX, repY;
    int32_t minX, minY;
    int32_t maxX, maxY;
    const SearchAddressInput* addresses;
    uint32_t addressCount;
};

/** A street name with its clusters. Keys are made by the caller. */
struct SearchStreetInput {
    const char* name;
    /** The name's sort key (folded, transliterated for Greek and Cyrillic). Name groups are ordered by it. */
    const char* sortKey;
    /** All search keys of the name including word-suffix keys (not cut, empty ones are skipped). */
    const char* const* keys;
    uint32_t keyCount;
    const SearchClusterInput* clusters;
    uint32_t clusterCount;
};

/** A point: a named or unnamed place, POI or other feature. */
struct SearchPointInput {
    /** The class pair label as sidx.py makes it: "poi/<class>/<subclass>", "place/<class>", "<kind>/<class>". */
    const char* label;
    /** The display name, nullptr for an unnamed point. */
    const char* name;
    /** For a named point: sort key and keys as for streets. Points of the same name and label share the first one's. */
    const char* sortKey;
    const char* const* keys;
    uint32_t keyCount;
    /** Position in global z14 units. */
    int32_t x;
    int32_t y;
};

struct SearchInput {
    const SearchPointInput* points;
    uint32_t pointCount;
    const SearchStreetInput* streets;
    uint32_t streetCount;
    /** "conscription/street" numbers: the second part is the street number and sorts first (sidx.py cfg slash_second). */
    bool slashSecond;
    /**
     * Folded keys for category names that are not plain ASCII (class names come from OSM tags, almost always ASCII):
     * categoryNames[i] is the display name ("pet shop") and categoryKeys[i] its key. Names not in the table are folded
     * as ASCII (lowercase, other characters become blanks). May be null.
     */
    const char* const* categoryNames;
    const char* const* categoryKeys;
    uint32_t categoryOverrides;
};

/** What the build made, for reports and tests. */
struct SearchBuildStats {
    uint32_t totalBytes;
    SearchHeader header;
    uint32_t keyCount;
    uint32_t ngCount;
    uint32_t namedPoints;
    uint32_t unnamedPoints;
    uint32_t overflowStrings;
    /** Class pairs folded into "<class>/(all)" because there were more than 255. */
    uint32_t collapsedPairs;
    /** Category runs without a key because the pairs are not contiguous. */
    uint32_t categoryNotContiguous;
};

/**
 * Builds the whole index in memory and gives it to a sink. For the host tool and tests: the device later streams
 * the pieces with the encode functions above. Fails (false, error()) when a count or offset does not fit its field.
 */
class SearchIndexWriter {
public:
    bool build(const SearchInput& input, ByteSink& sink, SearchBuildStats* stats = nullptr);
    const char* error() const { return errorText; }

private:
    char errorText[200] = {};
};

// ------------------------------------------------------------------------------------------------ reader

/** Where the reader gets bytes: a file, or memory. Returns false when the range cannot be read. */
class SearchFile {
public:
    virtual ~SearchFile() = default;
    virtual bool read(uint32_t offset, void* out, uint32_t size) = 0;
};

/** A name group. */
struct SearchNg {
    char name[SIDX_MAX_NAME + 1];
    SearchKind kind;
    uint32_t first;
    uint16_t count;
    uint32_t offset;
};

struct SearchPointRecord {
    int32_t x, y;
    uint8_t pair;
    uint32_t ng;
};

struct SearchStreetRecord {
    int32_t x, y;
    int32_t minX, minY;
    int32_t maxX, maxY;
    uint32_t firstAddress;
    uint16_t addressCount;
};

struct SearchAddress {
    uint16_t number;
    uint16_t aux;
    /** Lowercase letter or 0. */
    char suffix;
    uint8_t flags;
    int32_t x, y;
    /** Text number: true when flags has HOUSE_TEXT; textOffset is the offset in the overflow section. */
    bool isText;
    uint32_t textOffset;
};

struct SearchAddressHit {
    char street[96];
    SearchAddress address;
};

/** Reads made by the queries: calls, distinct 4 KB blocks of the file and runs of consecutive blocks. */
struct SearchIoStats {
    uint32_t calls;
    uint32_t blocks;
    uint32_t runs;
};

/**
 * Reads an index through a SearchFile. The header, class table and fence table are kept in RAM; the name group area
 * and the key pages can be made resident too (loadResident). Keys passed to the queries are already folded by the
 * caller (TextFold), at most 40 bytes are used.
 */
class SearchIndexReader {
public:
    /** Reads the header, class table and fence table. */
    bool open(SearchFile* file);
    /** Loads the name group area and/or the key pages into memory. Reads inside them cost no file I/O afterwards. */
    bool loadResident(bool nameGroups, bool keyPages);

    void resetIo();
    SearchIoStats io() const;
    /** Bytes read by open(): header + class table + fence table. */
    uint32_t openBytes() const { return SIDX_HEADER_SIZE + header.sectionSize[0] + header.sectionSize[1]; }

    const SearchHeader& info() const { return header; }
    /** A class pair: label (cut to size), first point record and count. */
    bool classPair(uint32_t index, char* label, size_t labelSize, uint32_t& first, uint32_t& count) const;

    /** Distinct NG offsets of the keys that start with the folded key, in key order, at most limit. Returns the count. */
    uint32_t prefix(const char* key, uint32_t limit, uint32_t* ngOffsets);
    bool ng(uint32_t offset, SearchNg& out);
    bool pointRecords(uint32_t first, uint32_t count, GrowArray<SearchPointRecord>& out);
    bool streetRecords(uint32_t first, uint32_t count, GrowArray<SearchStreetRecord>& out);
    bool addressAt(uint32_t index, SearchAddress& out);
    /** Binary search in a street's addresses for a number; suffix -1 = any, 0 = none, else a letter. */
    bool addressFind(uint32_t first, uint32_t count, uint16_t number, int suffix, GrowArray<SearchAddress>& out);
    /** The text of an overflow number (cut to size). */
    bool overflowText(uint32_t offset, char* out, size_t outSize);

    /** Name search: the NGs for a prefix; reads the first records of the first `show` results like sidx.Index. */
    bool searchName(const char* key, uint32_t limit, uint32_t show, GrowArray<SearchNg>& out);
    /** Address search: the street prefix key and a number; stops after a street when 8 addresses are found. */
    bool searchAddress(const char* streetKey, uint16_t number, int suffix, uint32_t limit, GrowArray<SearchAddressHit>& out);
    /** The whole run of a category NG. */
    bool category(const SearchNg& group, GrowArray<SearchPointRecord>& out);

private:
    /** Reads from a section (clamped to its end); returns the bytes read, 0 on failure. */
    uint32_t readSection(SidxSection section, uint32_t offset, void* out, uint32_t size);
    bool readPage(uint32_t page, uint8_t* out);
    void decodePt(const uint8_t* bytes, int32_t& x, int32_t& y) const;
    void decodeAddress(const uint8_t* bytes, SearchAddress& out) const;
    void touch(uint32_t offset, uint32_t size);

    SearchFile* file = nullptr;
    SearchHeader header = {};
    uint32_t pointBytes = 4;
    GrowArray<uint8_t> classBlob;
    GrowArray<uint32_t> classOffsets;
    GrowArray<uint8_t> fenceBlob;
    GrowArray<uint32_t> fenceOffsets;
    GrowArray<uint8_t> residentNg;
    GrowArray<uint8_t> residentKeys;
    bool ngResident = false;
    bool keysResident = false;
    GrowArray<uint8_t> blockSeen;
    uint32_t callCount = 0;
    uint8_t pageBuffer[SIDX_PAGE_SIZE] = {};
};

} // namespace maps
