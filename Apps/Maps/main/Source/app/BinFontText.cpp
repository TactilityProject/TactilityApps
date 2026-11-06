// SPDX-License-Identifier: GPL-3.0-or-later
#include "BinFontText.h"

#include "../platform/Platform.h"

#include <binfont/binfont.h>
#include <binfont/render.h>
#include <graphics/pixel_buffer.h>
#include <lvgl.h>
#include <lvgl/fonts.h>

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
            owned = true;
            BinFontMetrics metrics;
            binfont_get_metrics(font, &metrics);
            height = metrics.line_height;
            ascentPixels = metrics.ascent;
            logInfo("Font %s, line height %d", search.name, height);
            return true;
        }
        font = nullptr;
    }
    return false;
}

bool BinFontText::openFile(const char* path) {
    close();
    if (binfont_open_file(path, &font) != ERROR_NONE) {
        font = nullptr;
        return false;
    }
    owned = true;
    BinFontMetrics metrics;
    binfont_get_metrics(font, &metrics);
    height = metrics.line_height;
    ascentPixels = metrics.ascent;
    logInfo("Font file %s, line height %d", path, height);
    return true;
}

bool BinFontText::borrow(int lvglFontSize) {
    const lv_font_t* lvFont = lvgl_get_text_font(static_cast<LvglFontSize>(lvglFontSize));
    // Tactility's fonts come from lvgl_binfont_create(), which keeps the BinFont in dsc. Without a registered font
    // LVGL's built-in one comes back, whose dsc is something else.
    if (lvFont == nullptr || lvFont->dsc == nullptr || lvFont->get_glyph_dsc == lv_font_get_glyph_dsc_fmt_txt) {
        return false;
    }
    font = static_cast<BinFont*>(const_cast<void*>(lvFont->dsc));
    owned = false;
    BinFontMetrics metrics;
    binfont_get_metrics(font, &metrics);
    height = metrics.line_height;
    ascentPixels = metrics.ascent;
    logInfo("Font from the system, size %d, line height %d", lvglFontSize, height);
    return true;
}

void BinFontText::close() {
    if (target != nullptr) {
        pixel_buffer_free(target);
        target = nullptr;
        targetPixels = nullptr;
    }
    if (font != nullptr && owned) {
        binfont_close(font);
    }
    font = nullptr;
    owned = false;
}

int BinFontText::textWidth(const char* text) {
    if (font == nullptr) {
        return 0;
    }
    // The same advances and kerning binfont_draw_text_rgb565() applies
    int32_t widthX16 = 0;
    uint32_t previous = 0;
    bool hasPrevious = false;
    while (*text != '\0') {
        BinFontGlyph glyph;
        if (!binfont_get_glyph(font, nextCodepoint(text), &glyph)) {
            continue;
        }
        if (hasPrevious) {
            widthX16 += binfont_get_kerning_x16(font, previous, glyph.glyph_id);
        }
        widthX16 += static_cast<int32_t>(glyph.advance_x16);
        previous = glyph.glyph_id;
        hasPrevious = true;
    }
    return (widthX16 + 8) >> 4;
}

void BinFontText::drawText(Canvas& canvas, int x, int y, const char* text, uint16_t color) {
    if (font == nullptr) {
        return;
    }
    if (target == nullptr || targetPixels != canvas.pixels) {
        if (target != nullptr) {
            pixel_buffer_free(target);
        }
        target = pixel_buffer_wrap(canvas.pixels, DISPLAY_COLOR_FORMAT_RGB565, canvas.width, canvas.height, 0);
        targetPixels = target != nullptr ? canvas.pixels : nullptr;
        if (target == nullptr) {
            return;
        }
    }
    binfont_draw_text_rgb565(target, x, y + ascentPixels, font, text, color, PIXEL_BUFFER_CONVERSION_AVERAGE);
}

bool BinFontText::glyph(uint32_t codepoint, LabelGlyph& out, bool withMask) {
    BinFontGlyph found;
    if (font == nullptr || !binfont_get_glyph(font, codepoint, &found)) {
        return false;
    }
    out.id = found.glyph_id;
    out.advanceX16 = static_cast<int>(found.advance_x16);
    out.left = found.x;
    // binfont gives the box's bottom above the baseline, labels want its top below it
    out.top = -(found.y + found.height);
    out.width = found.width;
    out.height = found.height;
    out.alpha = nullptr;
    if (withMask && found.width > 0 && found.height > 0 && found.width * found.height <= MASK_BYTES &&
        binfont_get_glyph_bitmap(font, &found, mask, found.width) == ERROR_NONE) {
        out.alpha = mask;
    }
    return true;
}

int BinFontText::kerningX16(uint32_t leftId, uint32_t rightId) {
    return font != nullptr ? binfont_get_kerning_x16(font, leftId, rightId) : 0;
}

} // namespace maps
