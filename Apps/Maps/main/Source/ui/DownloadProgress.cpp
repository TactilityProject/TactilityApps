// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadProgress.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr int PADDING = 8;
constexpr int BUTTON_COUNT = 3;

int characterLength(unsigned char leading) {
    if (leading < 0x80) {
        return 1;
    }
    if ((leading & 0xE0) == 0xC0) {
        return 2;
    }
    if ((leading & 0xF0) == 0xE0) {
        return 3;
    }
    if ((leading & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

/** Copies at most maxBytes of source, stopping on a UTF-8 character boundary, and terminates dest. */
size_t copyTruncatedUtf8(char* dest, const char* source, size_t maxBytes) {
    const auto* cursor = reinterpret_cast<const unsigned char*>(source);
    size_t written = 0;
    while (*cursor != '\0') {
        int length = characterLength(*cursor);
        bool complete = true;
        for (int i = 1; i < length; i++) {
            if (cursor[i] == '\0' || (cursor[i] & 0xC0) != 0x80) {
                complete = false;
                break;
            }
        }
        if (!complete) {
            length = 1;
        }
        if (written + static_cast<size_t>(length) > maxBytes) {
            break;
        }
        memcpy(dest + written, cursor, static_cast<size_t>(length));
        written += static_cast<size_t>(length);
        cursor += length;
    }
    dest[written] = '\0';
    return written;
}

} // namespace

void formatProgressPill(TransferKind kind, const char* title, int64_t done, int64_t total, char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return;
    }
    if (kind == TransferKind::Browse) {
        snprintf(out, size, "Fetching detail %lld/%lld", static_cast<long long>(done), static_cast<long long>(total));
        return;
    }

    const char* verb = kind == TransferKind::Update ? "Updating " : "Downloading ";
    const int64_t percent = total > 0 ? (done * 100) / total : 0;
    char suffix[64];
    snprintf(suffix, sizeof(suffix), " %lld%% \xC2\xB7 %lld/%lld tiles", static_cast<long long>(percent),
        static_cast<long long>(done), static_cast<long long>(total));

    const size_t available = size - 1;
    const size_t prefixLength = strlen(verb);
    const size_t suffixLength = strlen(suffix);
    size_t titleRoom = 0;
    if (available >= prefixLength + suffixLength) {
        titleRoom = available - prefixLength - suffixLength;
    }

    if (prefixLength > available) {
        snprintf(out, size, "%s", verb);
        return;
    }
    memcpy(out, verb, prefixLength);
    size_t used = prefixLength;
    used += copyTruncatedUtf8(out + used, title != nullptr ? title : "", titleRoom);
    snprintf(out + used, size - used, "%s", suffix);
}

void TransferRate::reset() {
    hasSample = false;
    hasRate = false;
    lastTimeMs = 0;
    lastBytes = 0;
    rate = 0.0;
}

void TransferRate::add(uint32_t nowMs, uint64_t bytes) {
    if (!hasSample || bytes < lastBytes) {
        hasSample = true;
        hasRate = false;
        lastTimeMs = nowMs;
        lastBytes = bytes;
        rate = 0.0;
        return;
    }
    const uint32_t elapsed = nowMs - lastTimeMs;
    if (elapsed < 1000) {
        return;
    }
    const double current = static_cast<double>(bytes - lastBytes) * 1000.0 / static_cast<double>(elapsed);
    rate = hasRate ? 0.7 * rate + 0.3 * current : current;
    hasRate = true;
    lastTimeMs = nowMs;
    lastBytes = bytes;
}

double TransferRate::bytesPerSecond() const {
    return hasRate ? rate : 0.0;
}

int64_t TransferRate::secondsLeft(uint64_t bytes, uint64_t totalBytes) const {
    if (!hasRate || rate <= 0.0) {
        return -1;
    }
    if (totalBytes <= bytes) {
        return 0;
    }
    return static_cast<int64_t>(static_cast<double>(totalBytes - bytes) / rate);
}

void formatBytes(uint64_t bytes, char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return;
    }
    if (bytes >= 1000000) {
        snprintf(out, size, "%.1f MB", static_cast<double>(bytes) / 1000000.0);
    } else {
        snprintf(out, size, "%llu KB", static_cast<unsigned long long>(bytes / 1000));
    }
}

void formatDuration(int64_t seconds, char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds < 60) {
        snprintf(out, size, "%lld s", static_cast<long long>(seconds));
    } else if (seconds < 3600) {
        snprintf(out, size, "%lld min %lld s", static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60));
    } else {
        snprintf(out, size, "%lld h %02lld min", static_cast<long long>(seconds / 3600),
            static_cast<long long>((seconds % 3600) / 60));
    }
}

DownloadCard::Button DownloadCard::buttonAt(int index) const {
    static constexpr Button BUTTONS[BUTTON_COUNT] = { Button::PauseResume, Button::Cancel, Button::Close };
    if (index < 0 || index >= BUTTON_COUNT) {
        return Button::None;
    }
    return BUTTONS[index];
}

void DownloadCard::set(const char* titleText, const char* const* detailLines, int count, bool isPaused, bool onlyClose) {
    snprintf(title, sizeof(title), "%s", titleText != nullptr ? titleText : "");
    paused = isPaused;
    closeOnly = onlyClose;
    if (closeOnly) {
        focus = 2;
    }
    lineCount = std::clamp(count, 0, MAX_LINES);
    for (int i = 0; i < lineCount; i++) {
        snprintf(lines[i], LINE_LENGTH, "%s", detailLines != nullptr && detailLines[i] != nullptr ? detailLines[i] : "");
    }
}

void DownloadCard::layout(int width, int height, Font& font) {
    const int cardWidth = std::min(width - 16, 300);
    const int line = font.lineHeight();
    const int buttonHeight = line + 12;
    const int cardHeight = PADDING + (1 + lineCount) * line + PADDING + buttonHeight + PADDING;
    frame = { (width - cardWidth) / 2, (height - cardHeight) / 2, cardWidth, cardHeight };
    const int buttonWidth = (cardWidth - (BUTTON_COUNT + 1) * PADDING) / BUTTON_COUNT;
    const int buttonY = frame.y + cardHeight - PADDING - buttonHeight;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        buttons[i] = { frame.x + PADDING + i * (buttonWidth + PADDING), buttonY, buttonWidth, buttonHeight };
    }
}

void DownloadCard::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    canvas.fillRect(0, 0, canvas.width, canvas.height, theme.scrim, 110);
    canvas.fillRoundRect(frame.x, frame.y, frame.width, frame.height, 8, theme.panel);
    const int left = frame.x + PADDING;
    const int textWidth = frame.width - 2 * PADDING;
    int y = frame.y + PADDING;
    drawClippedText(canvas, font, left, y, title, textWidth, theme.text);
    y += font.lineHeight();
    for (int i = 0; i < lineCount; i++) {
        drawClippedText(canvas, font, left, y, lines[i], textWidth, theme.muted);
        y += font.lineHeight();
    }
    if (!closeOnly) {
        drawButton(canvas, font, theme, buttons[0], paused ? "Resume" : "Pause", focus == 0);
        drawButton(canvas, font, theme, buttons[1], "Cancel", focus == 1, true);
    }
    drawButton(canvas, font, theme, buttons[2], "Close", focus == 2);
}

DownloadCard::Button DownloadCard::handleKey(uint32_t key) {
    switch (key) {
        case keys::ENTER:
        case keys::YES:
            return buttonAt(focus);
        case keys::TAB:
        case keys::RIGHT:
        case keys::DOWN:
        case keys::FOCUS_NEXT:
        case 'd':
        case 'D':
        case 's':
        case 'S':
            if (!closeOnly) {
                focus = (focus + 1) % BUTTON_COUNT;
            }
            return Button::None;
        case keys::LEFT:
        case keys::UP:
        case keys::FOCUS_PREVIOUS:
        case 'a':
        case 'A':
        case 'w':
        case 'W':
            if (!closeOnly) {
                focus = (focus + BUTTON_COUNT - 1) % BUTTON_COUNT;
            }
            return Button::None;
        case keys::ESCAPE:
        case keys::BACKSPACE:
        case keys::NO:
            return Button::Close;
        default:
            return Button::None;
    }
}

DownloadCard::Button DownloadCard::handleTap(int x, int y) {
    for (int i = closeOnly ? BUTTON_COUNT - 1 : 0; i < BUTTON_COUNT; i++) {
        if (buttons[i].contains(x, y)) {
            return buttonAt(i);
        }
    }
    if (!frame.contains(x, y)) {
        return Button::Close;
    }
    return Button::None;
}

} // namespace maps
