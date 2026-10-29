// SPDX-License-Identifier: GPL-3.0-or-later
#include "TileCodec.h"

namespace maps {

namespace {

constexpr uint16_t RUN_FLAG = 0x8000;
constexpr size_t MAX_TOKEN = 0x7FFF;
constexpr size_t MIN_RUN = 3;

} // namespace

size_t encodeTile(const uint16_t* pixels, size_t count, uint16_t* out, size_t outCapacity) {
    size_t used = 0;
    size_t position = 0;
    while (position < count) {
        size_t run = 1;
        while (position + run < count && run < MAX_TOKEN && pixels[position + run] == pixels[position]) {
            run++;
        }
        if (run >= MIN_RUN) {
            if (used + 2 > outCapacity) {
                return 0;
            }
            out[used++] = static_cast<uint16_t>(RUN_FLAG | run);
            out[used++] = pixels[position];
            position += run;
            continue;
        }
        // Literal pixels up to the next run of MIN_RUN equal pixels
        size_t literal = 0;
        while (position + literal < count && literal < MAX_TOKEN) {
            const size_t next = position + literal;
            if (next + MIN_RUN <= count && pixels[next] == pixels[next + 1] && pixels[next] == pixels[next + 2]) {
                break;
            }
            literal++;
        }
        if (used + 1 + literal > outCapacity) {
            return 0;
        }
        out[used++] = static_cast<uint16_t>(literal);
        for (size_t i = 0; i < literal; i++) {
            out[used++] = pixels[position + i];
        }
        position += literal;
    }
    return used;
}

bool decodeTile(const uint16_t* data, size_t dataCount, uint16_t* pixels, size_t count) {
    size_t read = 0;
    size_t written = 0;
    while (read < dataCount && written < count) {
        const uint16_t token = data[read++];
        const size_t length = token & MAX_TOKEN;
        if (length == 0 || written + length > count) {
            return false;
        }
        if (token & RUN_FLAG) {
            if (read >= dataCount) {
                return false;
            }
            const uint16_t pixel = data[read++];
            for (size_t i = 0; i < length; i++) {
                pixels[written++] = pixel;
            }
        } else {
            if (read + length > dataCount) {
                return false;
            }
            for (size_t i = 0; i < length; i++) {
                pixels[written++] = data[read++];
            }
        }
    }
    return written == count && read == dataCount;
}

} // namespace maps
