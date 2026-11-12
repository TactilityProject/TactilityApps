// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchSheet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace maps {

namespace {

constexpr int PADDING = 8;
constexpr int ROW_PADDING = 10;
constexpr const char* PLACEHOLDER = "Search places, streets, addresses";
constexpr const char* CATEGORY_ENTRY = "Nearby by category";

void copyText(char* target, size_t size, const char* source) {
    snprintf(target, size, "%s", source != nullptr ? source : "");
}

} // namespace

void SearchSheet::store(Slot& slot, const Row& row) {
    copyText(slot.name, sizeof(slot.name), row.name);
    copyText(slot.type, sizeof(slot.type), row.type);
    copyText(slot.note, sizeof(slot.note), row.note);
    copyText(slot.distance, sizeof(slot.distance), row.distance);
    slot.bearing = row.bearing;
}

void SearchSheet::open() {
    value[0] = '\0';
    headerText[0] = '\0';
    resultCount = 0;
    moreRows = false;
    categoryView = false;
    resultView = false;
    selection = 0;
    first = 0;
    dragPixels = 0;
    askedAt = -1;
    focusField = true;
    highlighted = typing;
    visible = true;
    relayout();
}

void SearchSheet::setTypingKeyboard(bool enabled) {
    typing = enabled;
    highlighted = enabled;
    relayout();
}

void SearchSheet::setText(const char* text) {
    copyText(value, sizeof(value), text);
    categoryView = false;
    resultView = false;
    selection = 0;
    first = 0;
}

void SearchSheet::setHeader(const char* text) {
    copyText(headerText, sizeof(headerText), text);
    relayout();
}

void SearchSheet::setRows(const Row* rows, int count, bool more) {
    const int kept = std::clamp(count, 0, MAX_ROWS);
    for (int i = 0; i < kept; i++) {
        store(results[i], rows[i]);
    }
    resultCount = kept;
    moreRows = more && count <= MAX_ROWS;
    selection = 0;
    first = 0;
    dragPixels = 0;
    askedAt = -1;
    highlighted = typing;
    keepVisible();
}

void SearchSheet::appendRows(const Row* rows, int count, bool more) {
    const int room = MAX_ROWS - resultCount;
    const int kept = std::clamp(count, 0, room);
    for (int i = 0; i < kept; i++) {
        store(results[resultCount + i], rows[i]);
    }
    resultCount += kept;
    moreRows = more && resultCount < MAX_ROWS;
    keepVisible();
}

void SearchSheet::setRecent(const Row* rows, int count) {
    recentCount = std::clamp(count, 0, MAX_RECENT);
    for (int i = 0; i < recentCount; i++) {
        store(recent[i], rows[i]);
    }
    keepVisible();
}

void SearchSheet::setCategories(const char* const* names, int count) {
    categoryCount = std::clamp(count, 0, MAX_CATEGORIES);
    for (int i = 0; i < categoryCount; i++) {
        copyText(categories[i], sizeof(categories[i]), names[i]);
    }
    keepVisible();
}

void SearchSheet::showResults() {
    categoryView = false;
    resultView = true;
    selection = 0;
    first = 0;
    highlighted = highlighted || typing;
    keepVisible();
}

void SearchSheet::showCategories() {
    resultView = false;
    categoryView = true;
    selection = 0;
    first = 0;
    highlighted = highlighted || typing;
    keepVisible();
}

// ---------------------------------------------------------------------------------------------------------------

int SearchSheet::entryCount() const {
    if (categoryView) {
        return categoryCount;
    }
    if (value[0] != '\0' || resultView) {
        return resultCount;
    }
    return (categoryCount > 0 ? 1 : 0) + recentCount;
}

SearchSheet::Source SearchSheet::sourceOf(int entry, int& index) const {
    if (categoryView) {
        index = entry;
        return Source::Category;
    }
    if (value[0] != '\0' || resultView) {
        index = entry;
        return Source::Result;
    }
    const int offset = categoryCount > 0 ? 1 : 0;
    if (entry < offset) {
        index = -1;
        return Source::Browse;
    }
    index = entry - offset;
    return Source::Recent;
}

const SearchSheet::Slot* SearchSheet::slotOf(int entry) const {
    int index;
    switch (sourceOf(entry, index)) {
        case Source::Result:
            return index >= 0 && index < resultCount ? &results[index] : nullptr;
        case Source::Recent:
            return index >= 0 && index < recentCount ? &recent[index] : nullptr;
        default:
            return nullptr;
    }
}

int SearchSheet::entryHeight(int entry) const {
    const Slot* slot = slotOf(entry);
    if (slot == nullptr || compact) {
        return lineHeight + ROW_PADDING;
    }
    return 2 * lineHeight + 6 + (slot->note[0] != '\0' ? lineHeight : 0);
}

int SearchSheet::rowHeight() const {
    return std::max(8, entryHeight(first));
}

int SearchSheet::fitting(int firstEntry) const {
    const int count = entryCount();
    int used = 0;
    int rows = 0;
    for (int e = firstEntry; e < count; e++) {
        const int h = entryHeight(e);
        if (used + h > listBottom - listTop) {
            break;
        }
        used += h;
        rows++;
    }
    return std::max(rows, 1);
}

int SearchSheet::maxFirst() const {
    int used = 0;
    int f = entryCount();
    while (f > 0 && used + entryHeight(f - 1) <= listBottom - listTop) {
        used += entryHeight(f - 1);
        f--;
    }
    return std::min(f, std::max(0, entryCount() - 1));
}

void SearchSheet::keepVisible() {
    const int count = entryCount();
    selection = std::clamp(selection, 0, std::max(0, count - 1));
    first = std::clamp(first, 0, maxFirst());
    if (selection < first) {
        first = selection;
    }
    while (first < selection && selection >= first + fitting(first)) {
        first++;
    }
}

void SearchSheet::relayout() {
    barHeight = lineHeight + 12;
    const int headerHeight = headerText[0] != '\0' ? lineHeight + 2 : 2;
    listTop = barHeight + headerHeight;
    const int hintHeight = typing && !compact ? lineHeight + 4 : 0;
    listBottom = std::max(listTop + 1, frame.height - hintHeight);
    keepVisible();
}

void SearchSheet::layout(int width, int height, Font& font, bool compactLayout) {
    compact = compactLayout;
    lineHeight = font.lineHeight();
    frame = { 0, 0, width, height };
    barHeight = lineHeight + 12;
    backButton = { 0, 0, lineHeight + 16, barHeight };
    field = { backButton.width, 3, width - backButton.width - 6, lineHeight + 8 };
    relayout();
}

bool SearchSheet::wantsMore() const {
    if (!moreRows || categoryView || value[0] == '\0' || resultCount == 0 || askedAt == resultCount) {
        return false;
    }
    return selection >= resultCount - 3 || first + fitting(first) >= resultCount - 1;
}

SearchSheet::Event SearchSheet::wantMoreIfNeeded(Event event) {
    if (event.type == EventType::None && wantsMore()) {
        askedAt = resultCount;
        return { EventType::WantMore, -1 };
    }
    return event;
}

// ---------------------------------------------------------------------------------------------------------------

void SearchSheet::drawArrow(Canvas& canvas, float cx, float cy, float degrees, uint16_t color) const {
    const float radians = degrees * 3.14159265f / 180.0f;
    const float s = sinf(radians);
    const float c = cosf(radians);
    // A triangle pointing up, turned clockwise
    const float base[3][2] = { { 0.0f, -6.0f }, { 4.5f, 5.0f }, { -4.5f, 5.0f } };
    float px[3];
    float py[3];
    for (int i = 0; i < 3; i++) {
        px[i] = cx + base[i][0] * c - base[i][1] * s;
        py[i] = cy + base[i][0] * s + base[i][1] * c;
    }
    const int left = static_cast<int>(floorf(cx)) - 8;
    const int top = static_cast<int>(floorf(cy)) - 8;
    for (int y = top; y <= top + 16; y++) {
        for (int x = left; x <= left + 16; x++) {
            const float tx = static_cast<float>(x) + 0.5f;
            const float ty = static_cast<float>(y) + 0.5f;
            bool negative = false;
            bool positive = false;
            for (int i = 0; i < 3; i++) {
                const int j = (i + 1) % 3;
                const float cross = (px[j] - px[i]) * (ty - py[i]) - (py[j] - py[i]) * (tx - px[i]);
                negative = negative || cross < 0.0f;
                positive = positive || cross > 0.0f;
            }
            if (!(negative && positive)) {
                canvas.fillRect(x, y, 1, 1, color);
            }
        }
    }
}

void SearchSheet::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    // The map stays visible but dimmed behind the sheet, as behind the menus
    canvas.fillRect(0, 0, canvas.width, canvas.height, theme.scrim, 90);
    canvas.fillRect(frame.x, frame.y, frame.width, frame.height, theme.panel, theme.panelOpacity);

    // < at the top left, drawn as lines
    const auto backX = static_cast<float>(backButton.x + backButton.width / 2);
    const auto backY = static_cast<float>(field.y + field.height / 2);
    canvas.line(backX + 3.0f, backY - 6.0f, backX - 3.0f, backY, 2.0f, theme.text);
    canvas.line(backX - 3.0f, backY, backX + 3.0f, backY + 6.0f, 2.0f, theme.text);

    // The text field: the end of a long text stays in view; with the placeholder when empty
    canvas.fillRoundRect(field.x, field.y, field.width, field.height, 4, focusField ? theme.accent : theme.border);
    canvas.fillRoundRect(field.x + 1, field.y + 1, field.width - 2, field.height - 2, 3, theme.panel);
    if (value[0] == '\0') {
        drawClippedText(canvas, font, field.x + 4, field.y + 4, categoryView ? CATEGORY_ENTRY : PLACEHOLDER, field.width - 8, theme.muted);
        if (focusField) {
            canvas.fillRect(field.x + 4, field.y + 3, 2, field.height - 6, theme.accent, 255);
        }
    } else {
        const char* shown = value;
        while (*shown != '\0' && font.textWidth(shown) > field.width - 14) {
            shown++;
            while ((static_cast<unsigned char>(*shown) & 0xC0) == 0x80) {
                shown++;
            }
        }
        font.drawText(canvas, field.x + 4, field.y + 4, shown, theme.text);
        if (focusField) {
            canvas.fillRect(field.x + 4 + font.textWidth(shown) + 1, field.y + 3, 2, field.height - 6, theme.accent, 255);
        }
    }

    // The note line
    if (headerText[0] != '\0') {
        drawClippedText(canvas, font, PADDING, barHeight + 1, headerText, frame.width - 2 * PADDING, theme.muted);
    }
    canvas.fillRect(0, listTop - 1, frame.width, 1, theme.border);

    // The rows that fit completely
    const int count = entryCount();
    const int left = 4;
    const int width = frame.width - 8;
    int y = listTop;
    int shownRows = 0;
    for (int e = first; e < count; e++) {
        const int h = entryHeight(e);
        if (y + h > listBottom) {
            break;
        }
        shownRows++;
        int index;
        const Source source = sourceOf(e, index);
        const Slot* slot = slotOf(e);
        const bool isHighlighted = highlighted && e == selection;
        if (isHighlighted) {
            canvas.fillRoundRect(left, y, width, h - 2, 5, theme.accent);
        }
        const uint16_t color = isHighlighted ? theme.accentText : theme.text;
        const uint16_t quiet = isHighlighted ? theme.accentText : theme.muted;
        const int textLeft = left + PADDING;
        const int textRight = left + width - PADDING;
        if (slot == nullptr) {
            const char* label = source == Source::Browse ? CATEGORY_ENTRY : categories[index];
            const int textY = y + (h - 2 - lineHeight) / 2;
            const int room = source == Source::Browse ? textRight - textLeft - 14 : textRight - textLeft;
            drawClippedText(canvas, font, textLeft, textY, label, room, color);
            if (source == Source::Browse) {
                const auto cx = static_cast<float>(textRight - 4);
                const auto cy = static_cast<float>(y + (h - 2) / 2);
                canvas.line(cx - 3.0f, cy - 6.0f, cx + 3.0f, cy, 2.0f, color);
                canvas.line(cx + 3.0f, cy, cx - 3.0f, cy + 6.0f, 2.0f, color);
            }
        } else {
            const bool hasDistance = slot->distance[0] != '\0';
            const int distanceWidth = hasDistance ? font.textWidth(slot->distance) : 0;
            const int distanceX = textRight - distanceWidth;
            // The distance, with the arrow to its left
            const int arrowCentreX = distanceX - 10;
            const int rightLimit = hasDistance ? arrowCentreX - 8 : textRight;
            if (compact) {
                const int textY = y + (h - 2 - lineHeight) / 2;
                if (hasDistance) {
                    font.drawText(canvas, distanceX, textY, slot->distance, color);
                    drawArrow(canvas, static_cast<float>(arrowCentreX), static_cast<float>(textY + lineHeight / 2), slot->bearing, color);
                }
                const int typeWidth = slot->type[0] != '\0' ? font.textWidth(slot->type) : 0;
                const int reserved = typeWidth > 0 ? 8 + std::min(typeWidth, 56) : 0;
                const int nameRoom = rightLimit - textLeft - reserved;
                drawClippedText(canvas, font, textLeft, textY, slot->name, nameRoom, color);
                const int nameEnd = textLeft + std::min(font.textWidth(slot->name), nameRoom);
                const int typeRoom = rightLimit - nameEnd - 8;
                if (typeWidth > 0 && typeRoom >= 24) {
                    drawClippedText(canvas, font, nameEnd + 8, textY, slot->type, typeRoom, quiet);
                }
            } else {
                const int line1 = y + 3;
                const int line2 = line1 + lineHeight;
                drawClippedText(canvas, font, textLeft, line1, slot->name, textRight - textLeft, color);
                drawClippedText(canvas, font, textLeft, line2, slot->type, rightLimit - textLeft, quiet);
                if (hasDistance) {
                    font.drawText(canvas, distanceX, line2, slot->distance, quiet);
                    drawArrow(canvas, static_cast<float>(arrowCentreX), static_cast<float>(line2 + lineHeight / 2), slot->bearing, quiet);
                }
                if (slot->note[0] != '\0') {
                    drawClippedText(canvas, font, textLeft, line2 + lineHeight, slot->note, textRight - textLeft, quiet);
                }
            }
        }
        y += h;
    }

    // A thin bar shows where the visible rows are in a longer list
    if (shownRows > 0 && shownRows < count) {
        const int trackHeight = y - listTop;
        const int barLength = std::max(8, trackHeight * shownRows / count);
        const int barTop = listTop + (trackHeight - barLength) * first / std::max(1, maxFirst());
        canvas.fillRect(frame.width - 3, barTop, 2, barLength, theme.muted);
    }

    if (typing && !compact) {
        const int hintY = frame.height - lineHeight - 2;
        canvas.fillRect(0, hintY - 2, frame.width, 1, theme.border);
        drawClippedText(canvas, font, PADDING, hintY, "Roll: choose  Enter: open  Esc: close", frame.width - 2 * PADDING, theme.muted);
    }
}

// ---------------------------------------------------------------------------------------------------------------

void SearchSheet::append(uint32_t codepoint) {
    char bytes[4];
    size_t count = 0;
    if (codepoint < 0x80) {
        bytes[count++] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        bytes[count++] = static_cast<char>(0xC0 | (codepoint >> 6));
        bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        bytes[count++] = static_cast<char>(0xE0 | (codepoint >> 12));
        bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    const size_t length = strlen(value);
    if (length + count < sizeof(value)) {
        memcpy(value + length, bytes, count);
        value[length + count] = '\0';
    }
}

bool SearchSheet::erase() {
    size_t length = strlen(value);
    if (length == 0) {
        return false;
    }
    while (length > 0 && (static_cast<unsigned char>(value[length - 1]) & 0xC0) == 0x80) {
        length--;
    }
    if (length > 0) {
        length--;
    }
    value[length] = '\0';
    return true;
}

SearchSheet::Event SearchSheet::activate(int entry) {
    int index;
    switch (sourceOf(entry, index)) {
        case Source::Browse:
            showCategories();
            return {};
        case Source::Category:
            return { EventType::OpenCategory, index };
        case Source::Recent:
            return index < recentCount ? Event { EventType::OpenRecent, index } : Event {};
        case Source::Result:
            return index < resultCount ? Event { EventType::OpenRow, index } : Event {};
    }
    return {};
}

SearchSheet::Event SearchSheet::openSelected() {
    if (entryCount() == 0) {
        return {};
    }
    // Without a highlight (touch) the first visible row is the one Enter means
    if (!highlighted) {
        selection = first;
    }
    return activate(selection);
}

SearchSheet::Event SearchSheet::goBack() {
    if (resultView) {
        // A nearby list goes back to the categories
        resultView = false;
        categoryView = true;
        selection = 0;
        first = 0;
        return {};
    }
    if (categoryView) {
        categoryView = false;
        selection = 0;
        first = 0;
        return {};
    }
    return { EventType::Close, -1 };
}

SearchSheet::Event SearchSheet::move(int rows) {
    if (entryCount() == 0 || rows == 0) {
        if (rows < 0) {
            focusField = true;
        }
        return {};
    }
    if (!highlighted) {
        // The first step shows the highlight where the list is, it does not move yet
        highlighted = true;
        if (selection < first || selection >= first + fitting(first)) {
            selection = first;
        }
        return wantMoreIfNeeded({});
    }
    if (rows < 0 && selection == 0) {
        focusField = true;
        return {};
    }
    focusField = false;
    selection = std::clamp(selection + rows, 0, entryCount() - 1);
    keepVisible();
    return wantMoreIfNeeded({});
}

SearchSheet::Event SearchSheet::scrollBy(int rows) {
    first = std::clamp(first + rows, 0, maxFirst());
    return wantMoreIfNeeded({});
}

SearchSheet::Event SearchSheet::handleDrag(int dy) {
    dragPixels -= dy;
    Event result;
    const int height = rowHeight();
    while (std::abs(dragPixels) >= height) {
        const int step = dragPixels > 0 ? 1 : -1;
        dragPixels -= step * height;
        const Event event = scrollBy(step);
        if (event.type != EventType::None) {
            result = event;
        }
    }
    return result;
}

SearchSheet::Event SearchSheet::handleKey(uint32_t key) {
    switch (key) {
        case keys::ENTER:
        case keys::YES:
            return openSelected();
        case keys::ESCAPE:
        case keys::NO:
            return goBack();
        case keys::UP:
        case keys::FOCUS_PREVIOUS:
            return move(-1);
        case keys::DOWN:
        case keys::FOCUS_NEXT:
            return move(1);
        case keys::TAB:
            focusField = !focusField;
            if (!focusField) {
                highlighted = true;
            }
            return {};
        case keys::BACKSPACE:
        case keys::DELETE:
            if (categoryView || resultView || value[0] == '\0') {
                return goBack();
            }
            erase();
            selection = 0;
            first = 0;
            highlighted = typing;
            focusField = true;
            return { EventType::TextChanged, -1 };
        default:
            break;
    }
    if ((key >= 0x20 && key < 0x7F) || (key >= 0xA0 && key < 0x2000)) {
        categoryView = false;
        resultView = false;
        const size_t before = strlen(value);
        append(key);
        focusField = true;
        if (strlen(value) == before) {
            return {};
        }
        selection = 0;
        first = 0;
        highlighted = typing;
        return { EventType::TextChanged, -1 };
    }
    return {};
}

int SearchSheet::rowAt(int x, int y) const {
    if (x < 0 || x >= frame.width || y < listTop || y >= listBottom) {
        return -1;
    }
    int top = listTop;
    const int count = entryCount();
    for (int e = first; e < count; e++) {
        const int h = entryHeight(e);
        if (top + h > listBottom) {
            break;
        }
        if (y < top + h) {
            return e;
        }
        top += h;
    }
    return -1;
}

bool SearchSheet::entryRect(int entry, Rect& rect) const {
    int top = listTop;
    const int count = entryCount();
    for (int e = first; e < count; e++) {
        const int h = entryHeight(e);
        if (top + h > listBottom) {
            break;
        }
        if (e == entry) {
            rect = { 0, top, frame.width, h };
            return true;
        }
        top += h;
    }
    return false;
}

void SearchSheet::hover(int entry) {
    if (entry >= 0 && entry < entryCount()) {
        selection = entry;
        highlighted = true;
        focusField = false;
    }
}

SearchSheet::Event SearchSheet::handleTap(int x, int y) {
    if (backButton.contains(x, y)) {
        return goBack();
    }
    if (field.contains(x, y)) {
        focusField = true;
        return { EventType::FocusField, -1 };
    }
    const int entry = rowAt(x, y);
    if (entry >= 0) {
        selection = entry;
        return activate(entry);
    }
    return {};
}

} // namespace maps
