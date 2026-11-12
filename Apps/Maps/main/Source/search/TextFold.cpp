// SPDX-License-Identifier: GPL-3.0-or-later
// The study's text folding (host/search/fold.py) for search keys and queries. The character tables are made from
// fold.py by tools/make_fold_table.py (FoldTable.h).
#include "TextFold.h"

#include "FoldTable.h"

#include <cstring>

namespace maps {

namespace {

/** Code points of a text, at most this many are folded (names are short; longer texts are cut). */
constexpr size_t MAX_POINTS = 256;

template <size_t N>
const char* lookup(const FoldEntry (&table)[N], uint32_t codePoint) {
    size_t low = 0;
    size_t high = N;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (table[middle].codePoint < codePoint) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < N && table[low].codePoint == codePoint ? table[low].text : nullptr;
}

bool isApostrophe(uint32_t c) {
    for (uint32_t a : FOLD_APOSTROPHES) {
        if (a == c) {
            return true;
        }
    }
    return false;
}

bool isMark(uint32_t c) {
    return (c >= 0xFE00 && c <= 0xFE0F) || c == 0x200D || (c >= 0x300 && c <= 0x36F) || (c >= 0x483 && c <= 0x489) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
        (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F);
}

/** Not a letter or digit for Python (isalnum) outside ASCII: FoldTable's ranges, emoji and symbols above U+FFFF. */
bool isNonAsciiSeparator(uint32_t c) {
    if (c >= 0x10000) {
        return c >= 0x1F000 && c <= 0x1FAFF;
    }
    size_t low = 0;
    size_t high = sizeof(FOLD_SEPARATORS) / sizeof(FOLD_SEPARATORS[0]);
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (FOLD_SEPARATORS[middle][1] < c) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < sizeof(FOLD_SEPARATORS) / sizeof(FOLD_SEPARATORS[0]) && FOLD_SEPARATORS[low][0] <= c;
}

bool isAsciiAlnum(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isLetter(uint32_t c) {
    if (c < 0x80) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }
    return !isNonAsciiSeparator(c) && !isMark(c);
}

/** Lowercase for Greek and Cyrillic (and ASCII); other scripts unchanged. */
uint32_t lowerGreekCyrillic(uint32_t c) {
    if (c >= 'A' && c <= 'Z') {
        return c + 32;
    }
    if ((c >= 0x391 && c <= 0x3A9) && c != 0x3A2) {
        return c + 0x20;
    }
    switch (c) {
        case 0x386:
            return 0x3AC;
        case 0x388:
            return 0x3AD;
        case 0x389:
            return 0x3AE;
        case 0x38A:
            return 0x3AF;
        case 0x38C:
            return 0x3CC;
        case 0x38E:
            return 0x3CD;
        case 0x38F:
            return 0x3CE;
        default:
            break;
    }
    if (c >= 0x410 && c <= 0x42F) {
        return c + 0x20;
    }
    if (c >= 0x400 && c <= 0x40F) {
        return c + 0x50;
    }
    if (((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) && (c & 1) == 0) {
        return c + 1;
    }
    if (c >= 0x4C1 && c <= 0x4CE && (c & 1) == 1) {
        return c + 1;
    }
    return c;
}

/** The text as code points. */
size_t decodeAll(const char* text, uint32_t* points, size_t capacity) {
    const size_t length = strlen(text);
    size_t at = 0;
    size_t count = 0;
    while (at < length && count < capacity) {
        uint32_t c;
        const size_t used = utf8Decode(text + at, length - at, c);
        if (used == 0) {
            break;
        }
        at += used;
        points[count++] = c;
    }
    return count;
}

/** Writes text pieces with single spaces between words and none at the ends. */
class SpacedWriter {
public:
    SpacedWriter(char* out, size_t size) : out(out), size(size) {
        if (size > 0) {
            out[0] = '\0';
        }
    }
    void space() { pendingSpace = used > 0; }
    void text(const char* piece) {
        for (const char* p = piece; *p != '\0'; p++) {
            if (*p == ' ') {
                space();
            } else {
                byte(*p);
            }
        }
    }
    void point(uint32_t c) {
        if (c == ' ') {
            space();
            return;
        }
        char buffer[5];
        const size_t n = utf8Append(c, buffer, sizeof(buffer), 0);
        // A code point is written whole or not at all
        if (used + (pendingSpace ? 1 : 0) + n + 1 > size) {
            full = true;
            return;
        }
        for (size_t i = 0; i < n; i++) {
            byte(buffer[i]);
        }
    }
    size_t finish() {
        if (size > 0) {
            out[used] = '\0';
        }
        return used;
    }

private:
    void byte(char c) {
        if (full) {
            return;
        }
        if (pendingSpace) {
            if (used + 2 > size) {
                full = true;
                return;
            }
            out[used++] = ' ';
            pendingSpace = false;
        }
        if (used + 1 >= size) {
            full = true;
            return;
        }
        out[used++] = c;
    }
    char* out;
    size_t size;
    size_t used = 0;
    bool pendingSpace = false;
    bool full = false;
};

void foldPointLatin(uint32_t c, SpacedWriter& writer) {
    if (isApostrophe(c) || isMark(c)) {
        return;
    }
    if (c >= 0xAC00 && c <= 0xD7A3) {
        // Hangul syllables decompose into their jamo (NFD), as in fold.py
        const uint32_t index = c - 0xAC00;
        writer.point(0x1100 + index / (21 * 28));
        writer.point(0x1161 + (index % (21 * 28)) / 28);
        if (index % 28 != 0) {
            writer.point(0x11A7 + index % 28);
        }
        return;
    }
    const char* folded = lookup(FOLD_LATIN, c);
    if (folded != nullptr) {
        writer.text(folded);
    } else if (c < 0x80) {
        if (isAsciiAlnum(c)) {
            writer.point(c);
        } else {
            writer.space();
        }
    } else if (isNonAsciiSeparator(c)) {
        writer.space();
    } else {
        writer.point(lowerGreekCyrillic(c));
    }
}

} // namespace

size_t utf8Decode(const char* text, size_t length, uint32_t& codePoint) {
    if (length == 0) {
        codePoint = 0;
        return 0;
    }
    const auto* u = reinterpret_cast<const uint8_t*>(text);
    size_t need;
    uint32_t c;
    if (u[0] < 0x80) {
        codePoint = u[0];
        return 1;
    } else if ((u[0] & 0xE0) == 0xC0) {
        need = 2;
        c = u[0] & 0x1F;
    } else if ((u[0] & 0xF0) == 0xE0) {
        need = 3;
        c = u[0] & 0x0F;
    } else if ((u[0] & 0xF8) == 0xF0) {
        need = 4;
        c = u[0] & 0x07;
    } else {
        codePoint = 0xFFFD;
        return 1;
    }
    if (need > length) {
        codePoint = 0xFFFD;
        return 1;
    }
    for (size_t i = 1; i < need; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            codePoint = 0xFFFD;
            return 1;
        }
        c = (c << 6) | (u[i] & 0x3F);
    }
    codePoint = c;
    return need;
}

size_t utf8Append(uint32_t c, char* out, size_t size, size_t used) {
    char bytes[4];
    size_t n;
    if (c < 0x80) {
        bytes[0] = static_cast<char>(c);
        n = 1;
    } else if (c < 0x800) {
        bytes[0] = static_cast<char>(0xC0 | (c >> 6));
        bytes[1] = static_cast<char>(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        bytes[0] = static_cast<char>(0xE0 | (c >> 12));
        bytes[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        bytes[2] = static_cast<char>(0x80 | (c & 0x3F));
        n = 3;
    } else {
        bytes[0] = static_cast<char>(0xF0 | (c >> 18));
        bytes[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        bytes[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        bytes[3] = static_cast<char>(0x80 | (c & 0x3F));
        n = 4;
    }
    if (used + n + 1 > size) {
        return used;
    }
    memcpy(out + used, bytes, n);
    used += n;
    out[used] = '\0';
    return used;
}

size_t foldLatin(const char* text, char* out, size_t size) {
    uint32_t points[MAX_POINTS];
    const size_t count = decodeAll(text, points, MAX_POINTS);
    SpacedWriter writer(out, size);
    for (size_t i = 0; i < count; i++) {
        // Python's lower(): a capital sigma ending a word is a final sigma
        if (points[i] == 0x3A3) {
            // Unicode's rule: a cased letter before it and none after it, skipping case-ignorable characters (". : ' ·")
            auto ignorable = [](uint32_t c) { return c == '.' || c == ':' || c == '\'' || c == 0x2019 || c == 0xB7 || isMark(c); };
            size_t before = i;
            while (before > 0 && ignorable(points[before - 1])) {
                before--;
            }
            size_t after = i + 1;
            while (after < count && ignorable(points[after])) {
                after++;
            }
            const bool letterBefore = before > 0 && isLetter(points[before - 1]) && !(points[before - 1] >= '0' && points[before - 1] <= '9');
            const bool letterAfter = after < count && isLetter(points[after]) && !(points[after] >= '0' && points[after] <= '9');
            if (letterBefore && !letterAfter) {
                writer.point(0x3C2);
                continue;
            }
        }
        foldPointLatin(points[i], writer);
    }
    return writer.finish();
}

size_t foldOriginal(const char* text, char* out, size_t size) {
    uint32_t points[MAX_POINTS];
    const size_t count = decodeAll(text, points, MAX_POINTS);
    SpacedWriter writer(out, size);
    for (size_t i = 0; i < count; i++) {
        const uint32_t c = points[i];
        if (isApostrophe(c) || isMark(c)) {
            continue;
        }
        if (c >= 0x370 && c <= 0x52F) {
            const char* folded = lookup(FOLD_ORIGINAL, c);
            if (folded != nullptr) {
                writer.text(folded);
            } else if (isNonAsciiSeparator(c)) {
                writer.space();
            } else {
                writer.point(lowerGreekCyrillic(c));
            }
        } else if (c < 0x80) {
            if (isAsciiAlnum(c)) {
                writer.point(lowerGreekCyrillic(c));
            } else {
                writer.space();
            }
        } else if (isNonAsciiSeparator(c)) {
            writer.space();
        } else {
            const char* lower = lookup(FOLD_LOWER, c);
            if (lower != nullptr) {
                writer.text(lower);
            } else {
                writer.point(c);
            }
        }
    }
    return writer.finish();
}

bool hasNonLatin(const char* text) {
    const size_t length = strlen(text);
    size_t at = 0;
    while (at < length) {
        uint32_t c;
        const size_t used = utf8Decode(text + at, length - at, c);
        if (used == 0) {
            break;
        }
        if (c >= 0x370 && c <= 0x52F) {
            return true;
        }
        at += used;
    }
    return false;
}

size_t transliterate(const char* text, Translit profile, char* out, size_t size) {
    uint32_t points[MAX_POINTS];
    size_t count = decodeAll(text, points, MAX_POINTS);
    // Lowercase; Greek without tonos and with final sigma as sigma (FOLD_ORIGINAL does both for Greek)
    for (size_t i = 0; i < count; i++) {
        uint32_t c = lowerGreekCyrillic(points[i]);
        if (c >= 0x370 && c <= 0x3FF) {
            const char* plain = lookup(FOLD_ORIGINAL, c);
            if (plain != nullptr) {
                utf8Decode(plain, strlen(plain), c);
            }
        }
        points[i] = c;
    }
    // Into Latin, then fold_latin over the result (as fold.py does)
    char latin[4 * MAX_POINTS + 1];
    size_t used = 0;
    latin[0] = '\0';
    auto appendText = [&](const char* piece) {
        for (const char* p = piece; *p != '\0' && used + 1 < sizeof(latin); p++) {
            latin[used++] = *p;
        }
        latin[used] = '\0';
    };
    auto wordStart = [&](size_t i) { return i == 0 || !isLetter(points[i - 1]); };
    auto isOneOf = [](uint32_t c, const char* utf8) {
        size_t at = 0;
        const size_t length = strlen(utf8);
        while (at < length) {
            uint32_t d;
            at += utf8Decode(utf8 + at, length - at, d);
            if (d == c) {
                return true;
            }
        }
        return false;
    };
    const FoldEntry* table = profile == Translit::Ukrainian ? TRANSLIT_UK : (profile == Translit::Serbian ? TRANSLIT_SR : TRANSLIT_RU);
    const size_t tableSize = profile == Translit::Ukrainian ? sizeof(TRANSLIT_UK) / sizeof(FoldEntry)
                                                            : (profile == Translit::Serbian ? sizeof(TRANSLIT_SR) / sizeof(FoldEntry)
                                                                                            : sizeof(TRANSLIT_RU) / sizeof(FoldEntry));
    auto tableText = [&](uint32_t c) -> const char* {
        size_t low = 0;
        size_t high = tableSize;
        while (low < high) {
            const size_t middle = (low + high) / 2;
            if (table[middle].codePoint < c) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        return low < tableSize && table[low].codePoint == c ? table[low].text : nullptr;
    };
    for (size_t i = 0; i < count;) {
        const uint32_t c = points[i];
        const uint32_t next = i + 1 < count ? points[i + 1] : 0;
        if ((c >= 0x3B1 && c <= 0x3C9) || c == 0x3C2) {
            // Greek digraphs (ELOT 743)
            if (c == 0x3BF && next == 0x3C5) {  // ου
                appendText("ou");
                i += 2;
                continue;
            }
            if ((c == 0x3B1 || c == 0x3B5) && next == 0x3C5) {  // αυ, ευ
                const uint32_t after = i + 2 < count ? points[i + 2] : 0;
                appendText(c == 0x3B1 ? "a" : "e");
                appendText(after == 0 || isOneOf(after, "\xCE\xB8\xCE\xBA\xCE\xBE\xCF\x80\xCF\x83\xCF\x84\xCF\x86\xCF\x87\xCF\x88") ? "f" : "v");
                i += 2;
                continue;
            }
            if (c == 0x3BC && next == 0x3C0) {  // μπ
                appendText(wordStart(i) ? "b" : "mp");
                i += 2;
                continue;
            }
            if (c == 0x3BD && next == 0x3C4) {  // ντ
                appendText(wordStart(i) ? "d" : "nt");
                i += 2;
                continue;
            }
            if (c == 0x3B3 && next == 0x3B3) {  // γγ
                appendText("ng");
                i += 2;
                continue;
            }
            if (c == 0x3B3 && next == 0x3BA) {  // γκ
                appendText(wordStart(i) ? "g" : "nk");
                i += 2;
                continue;
            }
            const char* latinText = tableText(c);
            if (latinText != nullptr) {
                appendText(latinText);
            } else {
                char one[5];
                utf8Append(c, one, sizeof(one), 0);
                appendText(one);
            }
            i++;
            continue;
        }
        if (c >= 0x400 && c <= 0x52F) {
            if (c == 0x457 && profile == Translit::Ukrainian && wordStart(i)) {  // ї at a word start
                appendText("yi");
            } else {
                const char* latinText = tableText(c);
                appendText(latinText != nullptr ? latinText : "");
            }
            i++;
            continue;
        }
        char one[5];
        utf8Append(c, one, sizeof(one), 0);
        appendText(one);
        i++;
    }
    return foldLatin(latin, out, size);
}

} // namespace maps
