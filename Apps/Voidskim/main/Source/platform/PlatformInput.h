#pragma once

#include "../core/Input.h"
#include "../core/KeyBindings.h"
#include "DisplayOutput.h"

#include <tactility/device.h>

/**
 * Reads the keyboards, trackballs and touch screen into an InputState.
 *
 * Keyboards: through the game's KeyBindings (see there).
 * Trackball: roll to steer (left/right) and accelerate/brake (up/down), click to boost/confirm.
 */
class PlatformInput {
public:
    void init(const DisplayOutput* output);
    void shutdown();
    /** @param nowMs a millisecond clock */
    void poll(hd::InputState& state, uint32_t nowMs);
    /** The key bindings to map keys with. Read on every key press, so changes apply at once. */
    void setBindings(const hd::KeyBindings* keyBindings) { bindings = keyBindings; }

private:
    static constexpr int MAX_KEYBOARDS = 4;
    static constexpr int MAX_TRACKBALLS = 2;
    static constexpr int ACTION_COUNT = hd::ACTION_COUNT;
    static constexpr int MAX_HELD_KEYS = 16;

    const DisplayOutput* output = nullptr;
    const hd::KeyBindings* bindings = nullptr;
    Device* keyboards[MAX_KEYBOARDS] = {};
    int keyboardCount = 0;
    Device* trackballs[MAX_TRACKBALLS] = {};
    int trackballCount = 0;
    Device* touch = nullptr;

    // Keys currently down, with the actions they had when pressed. Per action, when it was last
    // pressed: some keyboards only send a quick press/release pair (or a stream of them while
    // held), so an action stays held for a short while after each press.
    uint32_t heldKeys[MAX_HELD_KEYS] = {};
    uint32_t heldActions[MAX_HELD_KEYS] = {};
    int heldKeyCount = 0;
    uint32_t lastPress[ACTION_COUNT] = {};
    uint32_t trackballUntil[ACTION_COUNT] = {};

    void handleKey(uint32_t key, bool pressed, uint32_t nowMs, hd::InputState& state);
};
