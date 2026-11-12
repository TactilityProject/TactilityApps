// SPDX-License-Identifier: GPL-3.0-or-later
#include "TouchKeyboard.h"

#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr int KEY_HEIGHT = 30;
constexpr int GAP = 2;
constexpr int ROWS = 4;

/** The accented letters, lower case, ten to a row; the last five share the bottom row with the controls. */
constexpr uint32_t ACCENTS[] = {
    0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xFD, 0xE4, 0xEB, 0xEF, 0xF6,
    0xFC, 0x151, 0x171, 0xE7, 0x10D, 0x107, 0x161, 0x17E, 0x159, 0x10F,
    0x165, 0x148, 0x13E, 0x13A, 0x142, 0x144, 0x15B, 0x17A, 0x17C, 0x105,
    0x119, 0x11F, 0x15F, 0x131, 0xDF,
};

/** The upper case of a lower case letter, other code points stay as they are. */
uint32_t upperCase(uint32_t code) {
    if (code >= 'a' && code <= 'z') {
        return code - 32;
    }
    if (code == 0xDF) {
        return 0x1E9E;
    }
    if (code >= 0xE0 && code <= 0xFE && code != 0xF7) {
        return code - 0x20;
    }
    if (code == 0x131) {
        return 'I';
    }
    // In Latin Extended-A the letters used here are one above their capital
    if (code >= 0x101 && code < 0x180) {
        return code - 1;
    }
    return code;
}

/** Writes a code point as UTF-8 into out (at least 5 bytes). */
void encode(uint32_t code, char* out) {
    int n = 0;
    if (code < 0x80) {
        out[n++] = static_cast<char>(code);
    } else if (code < 0x800) {
        out[n++] = static_cast<char>(0xC0 | (code >> 6));
        out[n++] = static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out[n++] = static_cast<char>(0xE0 | (code >> 12));
        out[n++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out[n++] = static_cast<char>(0x80 | (code & 0x3F));
    }
    out[n] = '\0';
}

} // namespace

int TouchKeyboard::heightFor(int /*width*/) {
    return GAP + ROWS * KEY_HEIGHT + (ROWS - 1) * GAP;
}

void TouchKeyboard::open() {
    visible = true;
    page = Page::Letters;
    shift = Shift::Off;
    build();
}

void TouchKeyboard::layout(int width, int height, Font& /*font*/) {
    screenWidth = width;
    const int keyboardHeight = heightFor(width);
    area = { 0, height - keyboardHeight, width, keyboardHeight };
    build();
}

void TouchKeyboard::beginRow(int row, int offsetUnits) {
    rowY = area.y + GAP + row * (KEY_HEIGHT + GAP);
    rowCursor = offsetUnits;
}

void TouchKeyboard::add(Kind kind, uint32_t code, const char* label, int units) {
    if (keyCount >= MAX_KEYS) {
        return;
    }
    // Keys are cut from the row by the same rounding at both edges, so neighbours never overlap
    const int span = screenWidth - GAP;
    const int left = GAP + rowCursor * span / ROW_UNITS;
    const int right = GAP + (rowCursor + units) * span / ROW_UNITS - GAP;
    Key& key = keys_[keyCount++];
    key.rect = { left, rowY, right - left, KEY_HEIGHT };
    key.kind = kind;
    key.code = code;
    snprintf(key.label, sizeof(key.label), "%s", label);
    rowCursor += units;
}

void TouchKeyboard::addCharacter(uint32_t code, int units) {
    char label[8];
    encode(code, label);
    add(Kind::Character, code, label, units);
}

void TouchKeyboard::build() {
    keyCount = 0;
    if (screenWidth <= 0) {
        return;
    }
    switch (page) {
        case Page::Letters: {
            const char* rows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
            beginRow(0, 0);
            for (const char* c = rows[0]; *c != '\0'; c++) {
                addCharacter(static_cast<uint32_t>(*c), 4);
            }
            beginRow(1, 2);
            for (const char* c = rows[1]; *c != '\0'; c++) {
                addCharacter(static_cast<uint32_t>(*c), 4);
            }
            beginRow(2, 0);
            add(Kind::Shift, 0, "Shift", 6);
            for (const char* c = rows[2]; *c != '\0'; c++) {
                addCharacter(static_cast<uint32_t>(*c), 4);
            }
            add(Kind::Delete, 0, "Del", 6);
            beginRow(3, 0);
            add(Kind::ToSymbols, 0, "123", 6);
            addCharacter(',', 4);
            add(Kind::Space, ' ', "Space", 20);
            addCharacter('.', 4);
            add(Kind::Enter, 0, "OK", 6);
            break;
        }
        case Page::Symbols: {
            beginRow(0, 0);
            for (const char* c = "1234567890"; *c != '\0'; c++) {
                addCharacter(static_cast<uint32_t>(*c), 4);
            }
            beginRow(1, 2);
            for (const char* c = "-/:;()&@\""; *c != '\0'; c++) {
                addCharacter(static_cast<uint32_t>(*c), 4);
            }
            beginRow(2, 0);
            add(Kind::ToAccents, 0, "\xC3\x81\xC3\x89", 8);
            for (const char* c = "#'!?"; *c != '\0'; c++) {
                addCharacter(static_cast<uint32_t>(*c), 6);
            }
            add(Kind::Delete, 0, "Del", 8);
            beginRow(3, 0);
            add(Kind::ToLetters, 0, "ABC", 6);
            addCharacter(',', 4);
            add(Kind::Space, ' ', "Space", 20);
            addCharacter('.', 4);
            add(Kind::Enter, 0, "OK", 6);
            break;
        }
        case Page::Accents: {
            int index = 0;
            for (int row = 0; row < 3; row++) {
                beginRow(row, 0);
                for (int i = 0; i < 10; i++) {
                    addCharacter(ACCENTS[index++], 4);
                }
            }
            beginRow(3, 0);
            add(Kind::ToLetters, 0, "ABC", 6);
            add(Kind::Shift, 0, "Shift", 6);
            for (; index < static_cast<int>(sizeof(ACCENTS) / sizeof(ACCENTS[0])); index++) {
                addCharacter(ACCENTS[index], 4);
            }
            add(Kind::Delete, 0, "Del", 4);
            add(Kind::Enter, 0, "OK", 4);
            break;
        }
    }
}

void TouchKeyboard::setPage(Page next) {
    page = next;
    shift = Shift::Off;
    build();
}

bool TouchKeyboard::contains(int x, int y) const {
    return visible && area.contains(x, y);
}

void TouchKeyboard::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    if (!visible) {
        return;
    }
    canvas.fillRect(area.x, area.y, area.width, area.height, theme.panel);
    canvas.fillRect(area.x, area.y, area.width, 1, theme.border);
    for (int i = 0; i < keyCount; i++) {
        const Key& key = keys_[i];
        if (key.kind == Kind::Character && shifted()) {
            char label[8];
            encode(upperCase(key.code), label);
            drawButton(canvas, font, theme, key.rect, label, false);
        } else {
            drawButton(canvas, font, theme, key.rect, key.label, key.kind == Kind::Shift && shift == Shift::Lock);
        }
    }
}

uint32_t TouchKeyboard::tap(int x, int y) {
    if (!contains(x, y)) {
        return 0;
    }
    for (int i = 0; i < keyCount; i++) {
        const Key& key = keys_[i];
        if (!key.rect.contains(x, y)) {
            continue;
        }
        switch (key.kind) {
            case Kind::Character: {
                const uint32_t code = shifted() ? upperCase(key.code) : key.code;
                if (shift == Shift::Once) {
                    shift = Shift::Off;
                }
                return code;
            }
            case Kind::Shift:
                shift = shift == Shift::Off ? Shift::Once : (shift == Shift::Once ? Shift::Lock : Shift::Off);
                return 0;
            case Kind::Delete:
                return keys::BACKSPACE;
            case Kind::Enter:
                return keys::ENTER;
            case Kind::Space:
                return ' ';
            case Kind::ToLetters:
                setPage(Page::Letters);
                return 0;
            case Kind::ToSymbols:
                setPage(Page::Symbols);
                return 0;
            case Kind::ToAccents:
                setPage(Page::Accents);
                return 0;
        }
    }
    return 0;
}

bool TouchKeyboard::keyRect(const char* label, Rect& rect) const {
    for (int i = 0; i < keyCount; i++) {
        if (strcmp(keys_[i].label, label) == 0) {
            rect = keys_[i].rect;
            return true;
        }
    }
    return false;
}

bool TouchKeyboard::keyCentre(const char* label, int& x, int& y) const {
    Rect rect {};
    if (!keyRect(label, rect)) {
        return false;
    }
    x = rect.x + rect.width / 2;
    y = rect.y + rect.height / 2;
    return true;
}

} // namespace maps
