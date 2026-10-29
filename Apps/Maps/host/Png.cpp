// SPDX-License-Identifier: GPL-3.0-or-later
#include "Png.h"

#include <cstdio>
#include <miniz.h>
#include <vector>

bool writePngRgb565(const char* path, const uint16_t* pixels, int width, int height) {
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int i = 0; i < width * height; i++) {
        const uint16_t c = pixels[i];
        rgb[i * 3 + 0] = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
        rgb[i * 3 + 1] = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
        rgb[i * 3 + 2] = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
    }
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), width, height, 3, &size);
    if (png == nullptr) {
        return false;
    }
    FILE* file = fopen(path, "wb");
    const bool written = file != nullptr && fwrite(png, 1, size, file) == size;
    if (file != nullptr) {
        fclose(file);
    }
    mz_free(png);
    return written;
}
