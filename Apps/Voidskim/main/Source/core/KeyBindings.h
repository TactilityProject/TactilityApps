#pragma once

#include "Input.h"

#include <cstddef>
#include <string>

namespace hd {

/** The race controls a player can bind keys to. */
enum class Control : uint8_t { SteerLeft, SteerRight, Accelerate, Brake, Boost, Pause, Count };
constexpr int CONTROL_COUNT = (int)Control::Count;

/**
 * Which keys drive which race control: two keys each. A key is bound to at most one control.
 *
 * Every bound key also navigates the menus (steer = left/right, accelerate/brake = up/down,
 * boost = confirm, pause = back), so the menus work on keyboards without arrow keys. Arrow keys,
 * Enter, Space, Escape, Backspace and Delete navigate the menus too, unless they're bound.
 */
class KeyBindings {
public:
    static constexpr int SLOTS = 2;

    KeyBindings() { setDefaults(); }

    void setDefaults();
    uint32_t key(Control control, int slot) const { return keys[(int)control][slot]; }
    /** Binds a key, taking it away from any other control or slot that had it. */
    void bind(Control control, int slot, uint32_t key);

    /** @return the Action bits a (normalised) key maps to */
    uint32_t actionsFor(uint32_t key) const;

    /** "steer_left 8592 97" lines, one per control */
    std::string serialise() const;
    /** Reads the lines serialise() writes, ignoring anything it doesn't know. */
    void parse(const std::string& text);

    static const char* controlName(Control control);
    /** A short name for a key, in characters the game's font has ("LEFT", "SPACE", "W") */
    static void keyName(uint32_t key, char* out, size_t size);

private:
    uint32_t keys[CONTROL_COUNT][SLOTS] = {};
};

} // namespace hd
