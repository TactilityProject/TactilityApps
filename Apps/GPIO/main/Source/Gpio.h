#pragma once

#include <Tactility/RecursiveMutex.h>
#include <tactility/concurrent/timer.h>

#include <tactility/drivers/gpio_controller.h>

#include <lvgl.h>
#include <memory>
#include <vector>

struct Context {
    uint32_t appInstanceId;
    // Set by main() right after window_manager_create() returns - the timer callback needs it
    // to check whether this window is still topmost before touching any widget (see
    // gpioOnTimer()'s comment).
    uint32_t window = 0;

    // The board's first active GPIO controller. Null if none is found (see gpioInit()).
    struct Device* gpioController = nullptr;
    uint32_t pinCount = 0;

    // unique_ptr<T[]>, same reason as pinStates below.
    std::unique_ptr<lv_obj_t*[]> pinWidgets;
    // unique_ptr<uint8_t[]>, not std::vector: this app links with -nostdlib (elf_loader.cmake), so
    // libstdc++ itself is never linked in - any allocator-guard helper symbol a vector's
    // destructor path needs (hit with both vector<bool> and vector<uint8_t> on esp32p4) is
    // unresolvable. new[]/delete[] need no such external symbols. Size is ctx->pinCount.
    std::unique_ptr<uint8_t[]> pinStates;
    // Constructed once in gpioInit(); needs ctx's address for its callback closure, so it can't
    // be a plain default member initializer (Context doesn't exist yet at that point in main()).
    // Plain C timer (tactility/concurrent/timer.h), not tt::Timer: tt::Timer stores its callback as
    // std::function<void()>, and this app links with -nostdlib (elf_loader.cmake) - libstdc++.a
    // itself is never linked in, and the toolchain's libstdc++.a is not built with -fPIC anyway, so
    // it cannot be linked into this -fPIC -shared ELF at all. timer_callback_t is a plain function
    // pointer + void* context, needing no libstdc++ support. Freed explicitly in gpioTeardown().
    struct Timer* timer = nullptr;
    tt::RecursiveMutex mutex;
};

/** Sets up state that must exist for the whole app instance lifetime. Call once, right after
 *  constructing the Context and before window_manager_create(). */
void gpioInit(Context* ctx);

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void gpioCreateWidgets(lv_obj_t* parent, void* userData);

/** Stops the periodic refresh timer and releases widget-tracking state. Call once, after the
 *  window has been torn down. */
void gpioTeardown(Context* ctx);
