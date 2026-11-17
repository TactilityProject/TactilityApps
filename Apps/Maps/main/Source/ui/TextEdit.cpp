// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextEdit.h"

#include <cstring>

namespace maps {

bool utf8Append(char* buffer, size_t size, uint32_t codepoint) {
    char bytes[4];
    size_t count = 0;
    if (codepoint < 0x80) {
        bytes[count++] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        bytes[count++] = static_cast<char>(0xC0 | (codepoint >> 6));
        bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        bytes[count++] = static_cast<char>(0xE0 | (codepoint >> 12));
        bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        bytes[count++] = static_cast<char>(0xF0 | (codepoint >> 18));
        bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        return false;
    }
    const size_t length = strlen(buffer);
    // The terminator needs its byte too.
    if (length + count >= size) {
        return false;
    }
    memcpy(buffer + length, bytes, count);
    buffer[length + count] = '\0';
    return true;
}

bool utf8EraseLast(char* buffer) {
    size_t length = strlen(buffer);
    if (length == 0) {
        return false;
    }
    // Step back over continuation bytes to the lead byte, so a whole character goes.
    while (length > 0 && (static_cast<unsigned char>(buffer[length - 1]) & 0xC0) == 0x80) {
        length--;
    }
    if (length > 0) {
        length--;
    }
    buffer[length] = '\0';
    return true;
}

} // namespace maps
