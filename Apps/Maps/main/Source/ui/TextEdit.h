// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * Appends one code point to the NUL-terminated UTF-8 text in buffer (size bytes in all, terminator included). Encodes
 * one to four bytes. Returns false and leaves the text unchanged when the character does not fit or is not a valid
 * code point.
 */
bool utf8Append(char* buffer, size_t size, uint32_t codepoint);

/** Removes the last whole code point (all its bytes) from the NUL-terminated UTF-8 text. Returns false when empty. */
bool utf8EraseLast(char* buffer);

} // namespace maps
