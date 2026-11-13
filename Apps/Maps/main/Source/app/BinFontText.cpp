// SPDX-License-Identifier: GPL-3.0-or-later
#include "BinFontText.h"

#include "../platform/Platform.h"
#include "../search/TextFold.h"

#include <binfont/binfont.h>
#include <binfont/render.h>
#include <graphics/pixel_buffer.h>
#include <lvgl.h>
#include <lvgl/fonts.h>

#include <algorithm>
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
    // 2, 3 or 4 bytes; a broken sequence is skipped and shown as '?'
    const int extra = (first & 0xE0) == 0xC0 ? 1 : (first & 0xF0) == 0xE0 ? 2 : (first & 0xF8) == 0xF0 ? 3 : 0;
    uint32_t codepoint = first & (0x3Fu >> extra);
    int taken = 0;
    while (taken < extra && (static_cast<uint8_t>(*text) & 0xC0) == 0x80) {
        codepoint = (codepoint << 6) | (static_cast<uint8_t>(*text++) & 0x3Fu);
        taken++;
    }
    if (extra == 0 || taken < extra) {
        while ((static_cast<uint8_t>(*text) & 0xC0) == 0x80) {
            text++;
        }
        return '?';
    }
    return codepoint;
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

namespace {

/** Upper case Latin letters beyond Latin-1 (Latin Extended-A and -B pairs, Latin Extended Additional). */
bool isUpperLatin(uint32_t c) {
    if (c >= 0xC0 && c <= 0xDE) {
        return c != 0xD7;
    }
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177) || (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0x218 && c <= 0x21B)) {
        return (c & 1) == 0;
    }
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
        return (c & 1) == 1;
    }
    return c == 0x178 || c == 0x181 || c == 0x186 || c == 0x189 || c == 0x18A || c == 0x191 || c == 0x198 || c == 0x1A0 || c == 0x1AF;
}

} // namespace

const char* BinFontText::withFallback(const char* text) {
    // Only rewritten when a glyph is missing
    bool missing = false;
    for (const char* p = text; *p != '\0' && !missing;) {
        BinFontGlyph glyph;
        missing = !binfont_get_glyph(font, nextCodepoint(p), &glyph);
    }
    if (!missing) {
        return text;
    }
    size_t used = 0;
    const char* p = text;
    while (*p != '\0' && used + 8 < sizeof(fallbackText)) {
        const char* start = p;
        const uint32_t codepoint = nextCodepoint(p);
        BinFontGlyph glyph;
        if (binfont_get_glyph(font, codepoint, &glyph)) {
            while (start < p && used + 1 < sizeof(fallbackText)) {
                fallbackText[used++] = *start++;
            }
            continue;
        }
        // The search's fold: diacritics off, lower case ("Š" -> "s"), then the case back
        char one[8] = {};
        memcpy(one, start, std::min<size_t>(static_cast<size_t>(p - start), sizeof(one) - 1));
        char folded[16];
        foldLatin(one, folded, sizeof(folded));
        for (size_t i = 0; folded[i] != '\0' && used + 1 < sizeof(fallbackText); i++) {
            const char c = folded[i];
            fallbackText[used++] = isUpperLatin(codepoint) && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        }
    }
    fallbackText[used] = '\0';
    return fallbackText;
}

int BinFontText::textWidth(const char* text) {
    if (font == nullptr) {
        return 0;
    }
    text = withFallback(text);
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
    binfont_draw_text_rgb565(target, x, y + ascentPixels, font, withFallback(text), color, PIXEL_BUFFER_CONVERSION_AVERAGE);
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
