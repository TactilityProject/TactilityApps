// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

/** A house number as stored in the index: number, second part, a one-letter suffix and flags. */
struct HouseNumber {
    uint16_t number;
    /** The part after "/" ("1394/12": 12), or the end of a range ("3-5": 5); 0 for none. */
    uint16_t aux;
    /** Lowercase ASCII letter, 0 for none. */
    char suffix;
    /** HOUSE_SLASH, HOUSE_RANGE. */
    uint8_t flags;
};

constexpr uint8_t HOUSE_SLASH = 1;
constexpr uint8_t HOUSE_RANGE = 2;

/** parseHouseNumber(): parses a house number string; false when it doesn't fit any form (stored as text then). */
bool parseHouseNumber(const char* text, size_t length, HouseNumber& out);

/** One reading of a query: the street (or name) text and the house number text ("" for none). */
struct QueryReading {
    char street[96];
    char number[24];
};

/** parseQuery(): the readings of a query, most likely first (at most 3). Returns how many. */
int parseQuery(const char* query, QueryReading out[3]);

} // namespace maps
