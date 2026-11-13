// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

/** Transliteration profiles of the study (fold.py PROFILES): Russian, Ukrainian, Serbian; Greek is the same in all. */
enum class Translit : uint8_t { Russian, Ukrainian, Serbian };

/** Decodes one UTF-8 code point at text[0..length); returns the bytes used (0 at the end, 1 for a bad byte, U+FFFD). */
size_t utf8Decode(const char* text, size_t length, uint32_t& codePoint);
/** Appends a code point as UTF-8 at out[used] within size (room for the final 0 kept); returns the new used. */
size_t utf8Append(uint32_t codePoint, char* out, size_t size, size_t used);

/**
 * The search keys' text form (fold.py fold_latin): lowercase, Latin diacritics removed (Štefánikova -> stefanikova,
 * Łódź -> lodz, Đường -> duong), apostrophes dropped, punctuation to one space. Greek and Cyrillic are only lowercased.
 * Returns the length written to out (0-terminated).
 */
size_t foldLatin(const char* text, char* out, size_t size);
/** fold_orig: Greek and Cyrillic kept in their script (Київ -> київ), tonos removed, final sigma, ё -> е, ґ -> г. */
size_t foldOriginal(const char* text, char* out, size_t size);
/** Whether the text has a Greek or Cyrillic letter (U+0370..U+052F). */
bool hasNonLatin(const char* text);
/** Greek and Cyrillic to Latin with the profile's table, then foldLatin (Київ -> kyiv, Αθήνα -> athina). */
size_t transliterate(const char* text, Translit profile, char* out, size_t size);

} // namespace maps
