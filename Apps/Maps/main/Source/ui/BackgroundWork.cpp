// SPDX-License-Identifier: GPL-3.0-or-later
#include "BackgroundWork.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr int PADDING = 8;

const char* workName(WorkKind kind) {
    switch (kind) {
        case WorkKind::PrepareTiles: return "Preparing tiles";
        case WorkKind::UpgradeTiles: return "Upgrading tiles";
        case WorkKind::IdleUpdate: return "Updating maps";
        case WorkKind::Fonts: return "Making label fonts";
        case WorkKind::Catalog: return "Loading the region list";
        case WorkKind::CacheCleanup: return "Cleaning the cache";
        case WorkKind::GpxImport: return "Importing GPX";
        case WorkKind::MoveBrowsed: return "Moving browsed tiles";
        case WorkKind::AddressIndex: return "Building search index";
        case WorkKind::SplitTiles: return "Splitting tiles";
        default: return "";
    }
}

} // namespace

void BackgroundWork::start(WorkKind kind, int32_t total) {
    if (kind >= WorkKind::Count) return;
    auto& entry = entries[static_cast<size_t>(kind)];
    entry.done.store(0, std::memory_order_relaxed);
    entry.total.store(total, std::memory_order_relaxed);
    entry.running.store(true, std::memory_order_relaxed);
    gen.fetch_add(1, std::memory_order_relaxed);
}

void BackgroundWork::progress(WorkKind kind, int32_t done, int32_t total) {
    if (kind >= WorkKind::Count) return;
    auto& entry = entries[static_cast<size_t>(kind)];
    entry.done.store(done, std::memory_order_relaxed);
    if (total != -1) {
        entry.total.store(total, std::memory_order_relaxed);
    }
}

void BackgroundWork::finish(WorkKind kind) {
    if (kind >= WorkKind::Count) return;
    auto& entry = entries[static_cast<size_t>(kind)];
    if (entry.running.exchange(false, std::memory_order_relaxed)) {
        gen.fetch_add(1, std::memory_order_relaxed);
    }
}

bool BackgroundWork::isRunning(WorkKind kind) const {
    if (kind >= WorkKind::Count) return false;
    return entries[static_cast<size_t>(kind)].running.load(std::memory_order_relaxed);
}

bool BackgroundWork::any() const {
    for (size_t i = 0; i < static_cast<size_t>(WorkKind::Count); i++) {
        if (entries[i].running.load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

uint32_t BackgroundWork::generation() const {
    return gen.load(std::memory_order_relaxed);
}

void BackgroundWork::describe(WorkKind kind, char* out, size_t size) const {
    if (kind >= WorkKind::Count || out == nullptr || size == 0) return;
    const auto& entry = entries[static_cast<size_t>(kind)];
    const int32_t total = entry.total.load(std::memory_order_relaxed);
    if (total > 0) {
        snprintf(out, size, "%s %d/%d", workName(kind),
            static_cast<int>(entry.done.load(std::memory_order_relaxed)), static_cast<int>(total));
    } else {
        snprintf(out, size, "%s", workName(kind));
    }
}

int BackgroundWork::lines(char (*out)[48], int maxLines) const {
    int count = 0;
    for (size_t i = 0; i < static_cast<size_t>(WorkKind::Count) && count < maxLines; i++) {
        if (entries[i].running.load(std::memory_order_relaxed)) {
            describe(static_cast<WorkKind>(i), out[count], 48);
            count++;
        }
    }
    return count;
}

void drawWorkDot(Canvas& canvas, int x, int y, uint16_t color) {
    canvas.fillCircle(static_cast<float>(x), static_cast<float>(y), 3.0f, color, 255);
}

Rect workDotTarget(int x, int y) {
    return { x - 12, y - 12, 24, 24 };
}

void WorkCard::open() {
    visible = true;
}

void WorkCard::close() {
    visible = false;
}

void WorkCard::set(const BackgroundWork& work) {
    lineCount = work.lines(lines, MAX_LINES);
    if (lineCount == 0) {
        strncpy(lines[0], "Nothing is running", LINE_LENGTH);
        lines[0][LINE_LENGTH - 1] = '\0';
        lineCount = 1;
    }
}

void WorkCard::layout(int width, int height, Font& font) {
    const int cardWidth = std::min(width - 16, 300);
    const int line = font.lineHeight();
    const int buttonHeight = line + 12;
    const int cardHeight = PADDING + (1 + lineCount) * line + PADDING + buttonHeight + PADDING;
    frame = { (width - cardWidth) / 2, (height - cardHeight) / 2, cardWidth, cardHeight };
    closeBtn = { frame.x + PADDING, frame.y + cardHeight - PADDING - buttonHeight, cardWidth - 2 * PADDING, buttonHeight };
}

void WorkCard::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    canvas.fillRect(0, 0, canvas.width, canvas.height, theme.scrim, 110);
    canvas.fillRoundRect(frame.x, frame.y, frame.width, frame.height, 8, theme.panel);
    const int left = frame.x + PADDING;
    const int textWidth = frame.width - 2 * PADDING;
    int y = frame.y + PADDING;
    drawClippedText(canvas, font, left, y, "Working in the background", textWidth, theme.text);
    y += font.lineHeight();
    for (int i = 0; i < lineCount; i++) {
        drawClippedText(canvas, font, left, y, lines[i], textWidth, theme.muted);
        y += font.lineHeight();
    }
    drawButton(canvas, font, theme, closeBtn, "Close", true);
}

bool WorkCard::hitClose(int x, int y) const {
    return closeBtn.contains(x, y);
}

Rect WorkCard::closeButton() const {
    return closeBtn;
}

} // namespace maps
