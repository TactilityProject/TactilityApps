// SPDX-License-Identifier: GPL-3.0-or-later
#include "Debug.h"

#include "../platform/Platform.h"

#include <cstdlib>
#include <cstring>

namespace maps {

namespace {

void writeLittleEndian(uint8_t* out, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; i++) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

} // namespace

bool saveBmp(const char* path, const uint16_t* pixels, int width, int height) {
    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    const int rowBytes = (width * 3 + 3) & ~3;
    uint8_t header[54] = { 'B', 'M' };
    writeLittleEndian(header + 2, 54 + rowBytes * height, 4);
    writeLittleEndian(header + 10, 54, 4);
    writeLittleEndian(header + 14, 40, 4);
    writeLittleEndian(header + 18, width, 4);
    writeLittleEndian(header + 22, height, 4);
    writeLittleEndian(header + 26, 1, 2);
    writeLittleEndian(header + 28, 24, 2);
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    auto* row = static_cast<uint8_t*>(largeAlloc(rowBytes));
    if (row == nullptr) {
        fclose(file);
        return false;
    }
    memset(row, 0, rowBytes);
    for (int y = height - 1; y >= 0 && ok; y--) {
        for (int x = 0; x < width; x++) {
            const uint16_t c = pixels[y * width + x];
            row[x * 3 + 0] = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
            row[x * 3 + 1] = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
            row[x * 3 + 2] = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
        }
        ok = fwrite(row, 1, rowBytes, file) == static_cast<size_t>(rowBytes);
    }
    largeFree(row);
    fclose(file);
    return ok;
}

DebugScript::~DebugScript() {
    largeFree(text);
}

bool DebugScript::open(const char* root) {
    char path[160];
    snprintf(path, sizeof(path), "%s/debug.txt", root);
    FILE* file = fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    constexpr size_t MAX_SCRIPT = 16 * 1024;
    text = static_cast<char*>(largeAlloc(MAX_SCRIPT));
    if (text != nullptr) {
        const size_t length = fread(text, 1, MAX_SCRIPT - 1, file);
        text[length] = '\0';
        position = 0;
        logInfo("Playing %s", path);
    }
    fclose(file);
    return text != nullptr;
}

DebugScript::Step DebugScript::next(uint32_t now) {
    Step step {};
    step.action = Action::None;
    if (text == nullptr || static_cast<int32_t>(now - resumeAt) < 0) {
        return step;
    }
    char line[96];
    while (text[position] != '\0') {
        size_t length = 0;
        while (text[position] != '\0' && text[position] != '\n') {
            if (length + 1 < sizeof(line)) {
                line[length++] = text[position];
            }
            position++;
        }
        line[length] = '\0';
        if (text[position] == '\n') {
            position++;
        }
        char command[16] = "";
        char argument[48] = "";
        if (sscanf(line, "%15s %47[^\r\n]", command, argument) < 1 || command[0] == '#') {
            continue;
        }
        logInfo("debug: %s %s", command, argument);
        if (strcmp(command, "wait") == 0) {
            resumeAt = now + static_cast<uint32_t>(atoi(argument));
            return step;
        }
        if (strcmp(command, "key") == 0) {
            step.action = Action::Key;
            if (strcmp(argument, "esc") == 0) {
                step.key = 0x1B;
            } else if (strcmp(argument, "enter") == 0) {
                step.key = 10;
            } else if (strcmp(argument, "bs") == 0) {
                step.key = '\b';
            } else {
                step.key = static_cast<uint8_t>(argument[0]);
            }
            return step;
        }
        if (strcmp(command, "shot") == 0) {
            step.action = Action::Shot;
            snprintf(step.name, sizeof(step.name), "%s", argument);
            return step;
        }
        if (strcmp(command, "goto") == 0 && sscanf(argument, "%lf %lf %d", &step.lat, &step.lon, &step.zoom) == 3) {
            step.action = Action::Goto;
            return step;
        }
        if (strcmp(command, "idle") == 0) {
            step.action = Action::WaitIdle;
            return step;
        }
        if (strcmp(command, "exit") == 0) {
            step.action = Action::Exit;
            return step;
        }
    }
    largeFree(text);
    text = nullptr;
    return step;
}

} // namespace maps
