#include "KeyBindings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hd {

namespace {

// Saved names: stable, unlike the display names
const char* const SAVE_NAMES[CONTROL_COUNT] = { "steer_left", "steer_right", "accelerate", "brake", "boost", "pause" };
const char* const DISPLAY_NAMES[CONTROL_COUNT] = { "STEER LEFT", "STEER RIGHT", "ACCELERATE", "BRAKE", "BOOST", "PAUSE" };
// The race action, and the menu action it doubles as
const uint32_t RACE_ACTIONS[CONTROL_COUNT] = {
    ActionSteerLeft | ActionLeft,
    ActionSteerRight | ActionRight,
    ActionAccelerate | ActionUp,
    ActionBrake | ActionDown,
    ActionBoost | ActionConfirm,
    ActionPause | ActionBack,
};

} // namespace

void KeyBindings::setDefaults() {
    const uint32_t defaults[CONTROL_COUNT][SLOTS] = {
        { KeyLeft, 'a' },
        { KeyRight, 'd' },
        { KeyUp, 'w' },
        { KeyDown, 's' },
        { KeySpace, 'b' },
        { KeyEscape, 'p' },
    };
    memcpy(keys, defaults, sizeof(keys));
}

void KeyBindings::bind(Control control, int slot, uint32_t key) {
    key = normaliseKey(key);
    if (key != KeyNone) {
        for (auto& controlKeys : keys) {
            for (auto& k : controlKeys) {
                if (k == key) k = KeyNone;
            }
        }
    }
    keys[(int)control][slot] = key;
}

uint32_t KeyBindings::actionsFor(uint32_t key) const {
    key = normaliseKey(key);
    if (key == KeyNone) {
        return 0;
    }
    uint32_t actions = 0;
    for (int c = 0; c < CONTROL_COUNT; c++) {
        for (uint32_t k : keys[c]) {
            if (k == key) actions |= RACE_ACTIONS[c];
        }
    }
    if (actions != 0) {
        return actions; // a bound key only does what it's bound to, also in the menus
    }
    switch (key) {
        case KeyLeft: actions |= ActionLeft; break;
        case KeyRight: actions |= ActionRight; break;
        case KeyUp: actions |= ActionUp; break;
        case KeyDown: actions |= ActionDown; break;
        case KeyEnter: case KeySpace: actions |= ActionConfirm; break;
        case KeyEscape: case KeyBackspace: case KeyDelete: actions |= ActionBack; break;
        default: break;
    }
    return actions;
}

std::string KeyBindings::serialise() const {
    std::string out;
    char line[64];
    for (int c = 0; c < CONTROL_COUNT; c++) {
        snprintf(line, sizeof(line), "bind %s %u %u\n", SAVE_NAMES[c], (unsigned)keys[c][0], (unsigned)keys[c][1]);
        out += line;
    }
    return out;
}

void KeyBindings::parse(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        char name[32] = {};
        unsigned long first = 0, second = 0;
        if (sscanf(line.c_str(), "bind %31s %lu %lu", name, &first, &second) != 3) {
            continue;
        }
        for (int c = 0; c < CONTROL_COUNT; c++) {
            if (strcmp(name, SAVE_NAMES[c]) == 0) {
                keys[c][0] = normaliseKey((uint32_t)first);
                keys[c][1] = normaliseKey((uint32_t)second);
            }
        }
    }
}

const char* KeyBindings::controlName(Control control) {
    return DISPLAY_NAMES[(int)control];
}

void KeyBindings::keyName(uint32_t key, char* out, size_t size) {
    switch (key) {
        case KeyNone: snprintf(out, size, "---"); return;
        case KeyLeft: snprintf(out, size, "LEFT"); return;
        case KeyRight: snprintf(out, size, "RIGHT"); return;
        case KeyUp: snprintf(out, size, "UP"); return;
        case KeyDown: snprintf(out, size, "DOWN"); return;
        case KeyEnter: snprintf(out, size, "ENTER"); return;
        case KeyEscape: snprintf(out, size, "ESC"); return;
        case KeyBackspace: snprintf(out, size, "BKSP"); return;
        case KeyDelete: snprintf(out, size, "DEL"); return;
        case KeyTab: snprintf(out, size, "TAB"); return;
        case KeySpace: snprintf(out, size, "SPACE"); return;
        default: break;
    }
    if (key >= 'a' && key <= 'z') {
        snprintf(out, size, "%c", (char)(key - 32));
    } else if (key > ' ' && key < 0x60) {
        snprintf(out, size, "%c", (char)key); // the font has ASCII 32..95
    } else {
        snprintf(out, size, "#%X", (unsigned)key);
    }
}

} // namespace hd
