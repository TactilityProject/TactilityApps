// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchCommon.h"

#include "QueryParse.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr uint32_t NONE = UINT32_MAX;
constexpr double PI_D = 3.14159265358979323846;

size_t characterCount(const char* s, size_t bytes) {
    size_t n = 0;
    for (size_t i = 0; i < bytes; i++) {
        if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

} // namespace

double metresPerUnitAtTileY(double tileY) {
    const double n = PI_D * (1.0 - 2.0 * tileY / 16384.0);
    const double lat = std::atan(std::sinh(n));
    return 40075016.686 * std::cos(lat) / (16384.0 * 4096.0);
}

// ------------------------------------------------------------------------------------------------ HashIndex

void HashIndex::clear() {
    full = false;
    values.clear();
    hashes.clear();
    used = 0;
}

void HashIndex::release() {
    full = false;
    values.release();
    hashes.release();
    used = 0;
}

void HashIndex::put(uint64_t hash, uint32_t value) {
    const size_t mask = values.size() - 1;
    size_t i = static_cast<size_t>(hash) & mask;
    while (values[i] != NONE) {
        i = (i + 1) & mask;
    }
    values[i] = value;
    hashes[i] = hash;
}

void HashIndex::insert(uint64_t hash, uint32_t value) {
    if (full) {
        return;
    }
    if ((used + 1) * 2 > values.size()) {
        // Both new arrays first: without memory for both the old table stays as it was (find() keeps working on it)
        // and the index is full, which failed() reports
        const size_t capacity = values.size() < 1024 ? 1024 : values.size() * 2;
        GrowArray<uint32_t> grownValues;
        GrowArray<uint64_t> grownHashes;
        if (!grownValues.resize(capacity) || !grownHashes.resize(capacity)) {
            full = true;
            return;
        }
        for (size_t i = 0; i < capacity; i++) {
            grownValues[i] = NONE;
        }
        grownValues.swap(values);
        grownHashes.swap(hashes);
        // The old table is in the grown* arrays now
        for (size_t i = 0; i < grownValues.size(); i++) {
            if (grownValues[i] != NONE) {
                put(grownHashes[i], grownValues[i]);
            }
        }
    }
    put(hash, value);
    used++;
}

// ------------------------------------------------------------------------------------------------ KeyMaker

void KeyMaker::sortKeyOf(const char* name, Translit profile, char* out, size_t outSize) {
    if (!hasNonLatin(name)) {
        foldLatin(name, out, outSize);
    } else {
        transliterate(name, profile, out, outSize);
    }
}

bool KeyMaker::has(const char* key) const {
    for (size_t i = 0; i < offsets.size(); i++) {
        if (strcmp(&pool[offsets[i]], key) == 0) {
            return true;
        }
    }
    return false;
}

void KeyMaker::add(const char* key) {
    offsets.push(static_cast<uint32_t>(pool.size()));
    for (const char* c = key;; c++) {
        pool.push(*c);
        if (*c == '\0') {
            break;
        }
    }
}

bool KeyMaker::make(const char* name, const char* latin, const char* en, bool useEn, Translit profile) {
    pool.clear();
    offsets.clear();
    // The sort key first in the pool (offset 0), not one of the keys
    sortKeyOf(name, profile, scratch[4], BUFFER);
    for (const char* c = scratch[4];; c++) {
        pool.push(*c);
        if (*c == '\0') {
            break;
        }
    }
    // fold.search_keys
    int n = 0;
    if (!hasNonLatin(name)) {
        foldLatin(name, scratch[n++], BUFFER);
        if (latin != nullptr && !hasNonLatin(latin)) {
            foldLatin(latin, scratch[n++], BUFFER);
        }
    } else {
        transliterate(name, profile, scratch[n++], BUFFER);
        foldOriginal(name, scratch[n++], BUFFER);
        if (latin != nullptr) {
            foldLatin(latin, scratch[n++], BUFFER);
        }
    }
    if (useEn && en != nullptr && !hasNonLatin(en)) {
        foldLatin(en, scratch[n++], BUFFER);
    }
    for (int i = 0; i < n; i++) {
        if (scratch[i][0] != '\0' && !has(scratch[i])) {
            add(scratch[i]);
        }
    }
    // fold.suffix_keys of each base key: the text from each later word of at least 3 characters
    const size_t baseCount = offsets.size();
    for (size_t b = 0; b < baseCount; b++) {
        // The pool may move while keys are added: work on a copy
        strncpy(scratch[4], &pool[offsets[b]], BUFFER - 1);
        scratch[4][BUFFER - 1] = '\0';
        const char* word = scratch[4];
        bool first = true;
        while (true) {
            const char* space = strchr(word, ' ');
            const size_t length = space != nullptr ? static_cast<size_t>(space - word) : strlen(word);
            if (!first && characterCount(word, length) >= 3 && !has(word)) {
                add(word);
            }
            first = false;
            if (space == nullptr) {
                break;
            }
            word = space + 1;
        }
    }
    return !pool.failed() && !offsets.failed();
}

// ------------------------------------------------------------------------------------------------ number helpers

HouseNumberStyle houseNumberStyle(const char* text) {
    HouseNumberStyle k = {};
    k.parsed = parseHouseNumber(text, strlen(text), k.number);
    if (k.parsed) {
        k.plain = k.number.aux == 0 && k.number.suffix == 0 && k.number.flags == 0;
        k.slash = (k.number.flags & HOUSE_SLASH) != 0 && k.number.aux != 0;
        k.secondSmaller = k.slash && k.number.aux < k.number.number;
    }
    return k;
}

int32_t houseVoteNumber(const char* text, bool slashSecond) {
    const HouseNumberStyle kind = houseNumberStyle(text);
    if (kind.parsed) {
        if (kind.slash && slashSecond) {
            return static_cast<int32_t>(kind.number.aux);
        }
        return static_cast<int32_t>(kind.number.number);
    }
    int32_t value = 0;
    int digits = 0;
    while (text[digits] >= '0' && text[digits] <= '9' && digits < 9) {
        value = value * 10 + (text[digits] - '0');
        digits++;
    }
    return digits > 0 ? value : -1;
}

} // namespace maps
