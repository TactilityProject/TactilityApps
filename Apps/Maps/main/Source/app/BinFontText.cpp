// SPDX-License-Identifier: GPL-3.0-or-later
#include "BinFontText.h"

#include "../platform/Platform.h"
#include "../render/Raster.h"

#include <binfont/binfont.h>

#include <cstdio>
#include <cstring>

namespace maps {

namespace {

struct FontSearch {
    const char* prefix;
    char name[96];
};

bool findFont(const char* name, bool isDirectory, void* context) {
    auto* search = static_cast<FontSearch*>(context);
    if (!isDirectory && strncmp(name, search->prefix, strlen(search->prefix)) == 0) {
        snprintf(search->name, sizeof(search->name), "%s", name);
        return false;
    }
    return true;
}

/** Decodes one UTF-8 character and advances the pointer. */
uint32_t nextCodepoint(const char*& text) {
    const auto first = static_cast<uint8_t>(*text++);
    if (first < 0x80) {
        return first;
    }
    if ((first & 0xE0) == 0xC0 && *text != '\0') {
        return ((first & 0x1Fu) << 6) | (static_cast<uint8_t>(*text++) & 0x3Fu);
    }
    // Longer sequences are skipped and shown as '?'
    while ((static_cast<uint8_t>(*text) & 0xC0) == 0x80) {
        text++;
    }
    return '?';
}

} // namespace

bool BinFontText::open(const char* folder, const char* const* prefixes, int prefixCount) {
    for (int i = 0; i < prefixCount; i++) {
        FontSearch search { prefixes[i], "" };
        listDirectory(folder, findFont, &search);
        if (search.name[0] == '\0') {
            continue;
        }
        char path[260];
        snprintf(path, sizeof(path), "%s/%s", folder, search.name);
        if (binfont_open_file(path, &font) == ERROR_NONE) {
            BinFontMetrics metrics;
            binfont_get_metrics(font, &metrics);
            height = metrics.line_height;
            ascent = metrics.ascent;
            logInfo("Font %s, line height %d", search.name, height);
            return true;
        }
        font = nullptr;
    }
    return false;
}

void BinFontText::close() {
    for (auto& entry : glyphs) {
        largeFree(entry.alpha);
        entry = {};
    }
    largeFree(copyright.alpha);
    copyright = {};
    if (font != nullptr) {
        binfont_close(font);
        font = nullptr;
    }
}

void BinFontText::preload() {
    for (uint32_t codepoint = 32; codepoint < 127; codepoint++) {
        glyph(codepoint);
    }
    glyph(0xA9);
}

BinFontText::Glyph* BinFontText::glyph(uint32_t codepoint) {
    Glyph* entry;
    if (codepoint >= 32 && codepoint < 128) {
        entry = &glyphs[codepoint - 32];
    } else if (codepoint == 0xA9) {
        entry = &copyright;
    } else {
        entry = &glyphs['?' - 32];
        codepoint = '?';
    }
    if (entry->loaded || font == nullptr) {
        return entry;
    }
    entry->loaded = true;
    BinFontGlyph info;
    if (!binfont_get_glyph(font, codepoint, &info)) {
        return entry;
    }
    entry->x = info.x;
    entry->y = info.y;
    entry->width = info.width;
    entry->height = info.height;
    entry->advance = static_cast<uint16_t>((info.advance_x16 + 8) >> 4);
    if (info.width > 0 && info.height > 0) {
        entry->alpha = static_cast<uint8_t*>(largeAlloc(static_cast<size_t>(info.width) * info.height));
        if (entry->alpha != nullptr && binfont_get_glyph_bitmap(font, &info, entry->alpha, info.width) != ERROR_NONE) {
            largeFree(entry->alpha);
            entry->alpha = nullptr;
        }
    }
    return entry;
}

int BinFontText::textWidth(const char* text) {
    int width = 0;
    while (*text != '\0') {
        width += glyph(nextCodepoint(text))->advance;
    }
    return width;
}

void BinFontText::drawText(Canvas& canvas, int x, int y, const char* text, uint16_t color) {
    if (font == nullptr) {
        return;
    }
    const int baseline = y + ascent;
    while (*text != '\0') {
        const Glyph* entry = glyph(nextCodepoint(text));
        if (entry->alpha != nullptr) {
            const int left = x + entry->x;
            const int top = baseline - (entry->y + entry->height);
            for (int row = 0; row < entry->height; row++) {
                for (int column = 0; column < entry->width; column++) {
                    const uint8_t alpha = entry->alpha[row * entry->width + column];
                    if (alpha != 0) {
                        canvas.blendPixel(left + column, top + row, color, (alpha + 4u) >> 3);
                    }
                }
            }
        }
        x += entry->advance;
    }
}

} // namespace maps
