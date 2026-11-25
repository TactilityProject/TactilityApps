#pragma once

#include <cstdint>

namespace hd {

/**
 * Game actions, as bits in InputState::held.
 *
 * Menu navigation (the first six) comes from fixed keys (arrows, Enter, Escape...) and from the
 * keys bound to the race controls. The race controls come from the player's key bindings.
 */
enum Action : uint32_t {
    // Menus
    ActionLeft = 1u << 0,
    ActionRight = 1u << 1,
    ActionUp = 1u << 2,
    ActionDown = 1u << 3,
    ActionConfirm = 1u << 4,
    ActionBack = 1u << 5,
    // Racing
    ActionSteerLeft = 1u << 6,
    ActionSteerRight = 1u << 7,
    ActionAccelerate = 1u << 8,
    ActionBrake = 1u << 9,
    ActionBoost = 1u << 10,
    ActionPause = 1u << 11,
};
constexpr int ACTION_COUNT = 12;

/**
 * Key codes: Unicode code points, with letters in lower case. Special keys use the code points
 * Tactility uses for them, so platform key codes pass through unchanged.
 */
enum Key : uint32_t {
    KeyNone = 0,
    KeyBackspace = '\b',
    KeyTab = '\t',
    KeyEnter = 10,
    KeyEscape = 0x1B,
    KeySpace = ' ',
    KeyDelete = 0x7F,
    KeyLeft = 0x2190,
    KeyUp = 0x2191,
    KeyRight = 0x2192,
    KeyDown = 0x2193,
};

/** Upper case letters to lower case, so bindings don't depend on Shift or Caps Lock. */
inline uint32_t normaliseKey(uint32_t key) {
    return (key >= 'A' && key <= 'Z') ? key + 32 : key;
}

struct TouchPoint {
    int x = 0; // in framebuffer pixels
    int y = 0;
};

/**
 * What the platform's input devices are doing right now. Keyboards and trackballs map to actions
 * (the platform's job, using the game's KeyBindings); touches are passed as points, since what
 * they mean depends on the screen.
 */
struct InputState {
    uint32_t held = 0; // Action bits
    static constexpr int MAX_TOUCHES = 5;
    TouchPoint touches[MAX_TOUCHES];
    int touchCount = 0;
    bool touchAvailable = false; // the device has a touch screen
    bool keyPressed = false; // a key was pressed since the last update (hides the touch controls)
    // The keys pressed since the last update (normalised), for binding a key to an action
    static constexpr int MAX_KEYS_PRESSED = 4;
    uint32_t keysPressed[MAX_KEYS_PRESSED] = {};
    int keysPressedCount = 0;
};

} // namespace hd
