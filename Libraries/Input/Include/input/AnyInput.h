// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include <tactility/device.h>

/**
 * "Any input" detection, mirroring the boot screen: a touch press, a key press, or a mouse or
 * trackball button press all count.
 *
 * A touch that is already in progress when this object is constructed is deliberately ignored, so
 * the tap that launched the app doesn't immediately close it. Call activity() from a loop, or wait()
 * to block until something happens.
 */
class AnyInput {
public:
    /** Finds the input devices; takes a reference on each of them until destruction. */
    AnyInput();

    ~AnyInput();

    AnyInput(const AnyInput&) = delete;
    AnyInput& operator=(const AnyInput&) = delete;

    /** True when at least one input device (touch, keyboard, mouse, trackball) is available. */
    bool available() const;

    /** Non-blocking: true when a fresh press happened since the previous call. */
    bool activity();

    /**
     * Blocks until activity() returns true.
     * @param timeout_ms how long to wait, or UINT32_MAX to wait forever
     * @return true when input happened, false on timeout
     */
    bool wait(uint32_t timeout_ms = UINT32_MAX);

private:
    static constexpr int MAX_KEYBOARDS = 4;

    struct Device* touch_ = nullptr;
    struct Device* trackball_ = nullptr;
    struct Device* mouse_ = nullptr;
    struct Device* keyboards_[MAX_KEYBOARDS] {};
    int keyboard_count_ = 0;

    // Held state starts "down" so a touch/button held while the app starts doesn't count
    bool touch_down_ = true;
    bool trackball_down_ = true;
    bool mouse_down_ = true;
};
