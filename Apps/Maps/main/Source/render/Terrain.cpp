// SPDX-License-Identifier: GPL-3.0-or-later
#include "Terrain.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace maps {

namespace {

constexpr uint8_t PNG_SIGNATURE[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
/** No real chunk of a terrain tile is near this: a larger length is a damaged file. */
constexpr uint32_t MAX_CHUNK = 1u << 24;

uint32_t readU32Big(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) | (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

/** The PNG Paeth predictor. */
uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return static_cast<uint8_t>(a);
    }
    return static_cast<uint8_t>(pb <= pc ? b : c);
}

} // namespace

bool PngDataInput::readExact(uint8_t* buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        const int got = file.read(buffer + done, size - done);
        if (got <= 0) {
            return false;
        }
        done += static_cast<size_t>(got);
    }
    return true;
}

bool PngDataInput::open() {
    uint8_t header[8 + 8 + 13 + 4];
    if (!readExact(header, sizeof(header)) || memcmp(header, PNG_SIGNATURE, 8) != 0 || readU32Big(header + 8) != 13 ||
        memcmp(header + 12, "IHDR", 4) != 0) {
        return false;
    }
    const uint8_t* ihdr = header + 16;
    imageWidth = static_cast<int>(readU32Big(ihdr));
    imageHeight = static_cast<int>(readU32Big(ihdr + 4));
    const uint8_t depth = ihdr[8];
    const uint8_t colour = ihdr[9];
    const uint8_t interlace = ihdr[12];
    bytesPerPixel = colour == 2 ? 3 : (colour == 6 ? 4 : 0);
    left = 0;
    ended = false;
    return depth == 8 && bytesPerPixel != 0 && ihdr[10] == 0 && ihdr[11] == 0 && interlace == 0 && imageWidth > 0 && imageWidth <= 1024 &&
        imageHeight > 0 && imageHeight <= 1024;
}

bool PngDataInput::nextData() {
    while (!ended) {
        uint8_t chunk[8];
        if (!readExact(chunk, sizeof(chunk))) {
            ended = true;
            return false;
        }
        const uint32_t length = readU32Big(chunk);
        if (length > MAX_CHUNK) {
            ended = true;
            return false;
        }
        if (memcmp(chunk + 4, "IDAT", 4) == 0) {
            left = length;
            if (length > 0) {
                return true;
            }
            uint8_t crc[4];
            if (!readExact(crc, 4)) {
                ended = true;
                return false;
            }
            continue;
        }
        if (memcmp(chunk + 4, "IEND", 4) == 0) {
            ended = true;
            return false;
        }
        // Another chunk (gAMA, tEXt...): skipped with its CRC
        uint8_t skip[256];
        uint32_t rest = length + 4;
        while (rest > 0) {
            const uint32_t step = rest < sizeof(skip) ? rest : static_cast<uint32_t>(sizeof(skip));
            if (!readExact(skip, step)) {
                ended = true;
                return false;
            }
            rest -= step;
        }
    }
    return false;
}

int PngDataInput::read(uint8_t* buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        if (left == 0) {
            if (!nextData()) {
                break;
            }
        }
        const size_t step = size - done < left ? size - done : left;
        const int got = file.read(buffer + done, step);
        if (got <= 0) {
            ended = true;
            break;
        }
        done += static_cast<size_t>(got);
        left -= static_cast<uint32_t>(got);
        if (left == 0) {
            uint8_t crc[4];
            if (!readExact(crc, 4)) {
                ended = true;
                break;
            }
        }
    }
    return static_cast<int>(done);
}

bool PngDataInput::rewind() {
    return file.rewind() && open();
}

int terrainLightStep(float bearingDegrees) {
    float turned = std::fmod(bearingDegrees, 360.0f);
    if (turned < 0.0f) {
        turned += 360.0f;
    }
    return static_cast<int>(std::floor(turned / 45.0f + 0.5f)) % 8;
}

float terrainLightAzimuth(int step) {
    return 315.0f + 45.0f * static_cast<float>(((step % 8) + 8) % 8);
}

void hillshade(const uint16_t* grid, int width, int height, int stride, float metresPerCell, float exaggeration, uint8_t* shade, int shadeStride,
    float lightAzimuth) {
    // Light from lightAzimuth, 45 degrees above the horizon: unit vector (x east, y south, z up); 315 gives (-0.5, -0.5)
    const float radians = lightAzimuth * 0.017453292f;
    const float lightX = 0.70710678f * std::sin(radians);
    const float lightY = -0.70710678f * std::cos(radians);
    const float lightZ = 0.70710678f;
    // Heights are half metres: the slope per cell in metres per metre
    const float scale = exaggeration * 0.5f / (2.0f * metresPerCell);
    for (int y = 0; y < height; y++) {
        const int up = y > 0 ? y - 1 : y;
        const int down = y + 1 < height ? y + 1 : y;
        for (int x = 0; x < width; x++) {
            const int left = x > 0 ? x - 1 : x;
            const int right = x + 1 < width ? x + 1 : x;
            const float dzdx = (static_cast<float>(grid[static_cast<size_t>(y) * stride + right]) - static_cast<float>(grid[static_cast<size_t>(y) * stride + left])) *
                scale * (2.0f / static_cast<float>(right - left > 0 ? right - left : 1));
            const float dzdy = (static_cast<float>(grid[static_cast<size_t>(down) * stride + x]) - static_cast<float>(grid[static_cast<size_t>(up) * stride + x])) *
                scale * (2.0f / static_cast<float>(down - up > 0 ? down - up : 1));
            // The surface normal (-dz/dx, -dz/dy, 1), normalised, against the light
            const float length = std::sqrt(dzdx * dzdx + dzdy * dzdy + 1.0f);
            const float lit = (-dzdx * lightX - dzdy * lightY + lightZ) / length;
            // Flat ground (lit = lightZ) and slopes towards the light get no shade
            const float dark = (lightZ - lit) / lightZ;
            shade[static_cast<size_t>(y) * shadeStride + x] = static_cast<uint8_t>(dark <= 0.0f ? 0 : (dark >= 1.0f ? 255 : static_cast<int>(dark * 255.0f + 0.5f)));
        }
    }
}

bool decodeTerrarium(ByteInput& file, InflateReader& inflater, uint8_t* rowBuffer, uint16_t* grid, int stride) {
    PngDataInput data(file);
    if (!data.open() || data.width() != TERRAIN_TILE_SIZE || data.height() != TERRAIN_TILE_SIZE || !inflater.begin(&data) ||
        inflater.format() != InflateReader::Format::Zlib) {
        return false;
    }
    const int bpp = data.pixelBytes();
    const int rowBytes = TERRAIN_TILE_SIZE * bpp;
    uint8_t* previous = rowBuffer;
    uint8_t* current = rowBuffer + 1024;
    memset(previous, 0, static_cast<size_t>(rowBytes));
    for (int y = 0; y < TERRAIN_TILE_SIZE; y++) {
        const int filter = inflater.get();
        if (filter < 0 || filter > 4 || inflater.read(current, static_cast<size_t>(rowBytes)) != static_cast<size_t>(rowBytes)) {
            return false;
        }
        for (int i = 0; i < rowBytes; i++) {
            const int a = i >= bpp ? current[i - bpp] : 0;
            const int b = previous[i];
            const int c = i >= bpp ? previous[i - bpp] : 0;
            switch (filter) {
                case 1:
                    current[i] = static_cast<uint8_t>(current[i] + a);
                    break;
                case 2:
                    current[i] = static_cast<uint8_t>(current[i] + b);
                    break;
                case 3:
                    current[i] = static_cast<uint8_t>(current[i] + ((a + b) >> 1));
                    break;
                case 4:
                    current[i] = static_cast<uint8_t>(current[i] + paeth(a, b, c));
                    break;
                default:
                    break;
            }
        }
        uint16_t* out = grid + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < TERRAIN_TILE_SIZE; x++) {
            const uint8_t* pixel = current + x * bpp;
            const float metres = static_cast<float>(pixel[0]) * 256.0f + static_cast<float>(pixel[1]) + static_cast<float>(pixel[2]) / 256.0f - 32768.0f;
            out[x] = terrainCode(metres);
        }
        uint8_t* swap = previous;
        previous = current;
        current = swap;
    }
    return !inflater.failed();
}

} // namespace maps
