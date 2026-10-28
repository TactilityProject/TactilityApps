#include "Application.h"

#include <esp_log.h>

#include <app/event.h>
#include <tactility/check.h>

#include <Tactility/kernel/Kernel.h>

#include <algorithm>
#include <cstdint>

constexpr auto TAG = "Application";

static uint16_t toRgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/** Three colour bands across the top half. */
static void drawBandsArea(gfx::Surface& surface, int y, int height) {
    const int width = surface.width();
    const int stripe = width / 3;
    if (stripe <= 0) {
        return;
    }

    // On a grayscale panel, equal-luminance colours (pure red/green/blue all average to 85)
    // collapse into one flat block. Use explicit dark/mid/light greys there so the three bands stay
    // visually distinct; colour displays still get real RGB.
    const bool grayscale = surface.format() == DISPLAY_COLOR_FORMAT_GRAYSCALE8;
    const uint16_t first = grayscale ? toRgb565(64, 64, 64) : toRgb565(255, 0, 0);
    const uint16_t second = grayscale ? toRgb565(160, 160, 160) : toRgb565(0, 255, 0);
    const uint16_t third = grayscale ? toRgb565(230, 230, 230) : toRgb565(0, 0, 255);

    surface.fillRect(0, y, stripe, height, first);
    surface.fillRect(stripe, y, stripe, height, second);
    surface.fillRect(stripe * 2, y, width - stripe * 2, height, third);
}

/** Red/green/blue gradients across the bottom half. */
static void drawFadingArea(gfx::Surface& surface, int y, int height) {
    const int width = surface.width();
    const int stroke = std::max(width / 3, 1);

    for (int x = 0; x < width; x++) {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        if (x < stroke) {
            r = static_cast<uint8_t>(x * 255 / stroke);
        } else if (x < stroke * 2) {
            g = static_cast<uint8_t>((x - stroke) * 255 / stroke);
        } else {
            b = static_cast<uint8_t>((x - stroke * 2) * 255 / stroke);
        }
        surface.fillRect(x, y, 1, height, toRgb565(r, g, b));
    }
}

static void drawPattern(gfx::Surface& surface, int display_height) {
    // Clearing the surface also makes the callback overwrite every pixel it covers, so the target
    // doesn't have to copy the previous frame forward (Prepare::Overwrite).
    surface.clear();

    const int half = display_height / 2;
    drawBandsArea(surface, 0, half);
    drawFadingArea(surface, half, display_height - half);
}

void runApplication(gfx::Target* target, AnyInput* input) {
    const gfx::Info& info = target->info();
    const bool slow_refresh = info.slowRefresh;

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);
    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    bool should_close = false;
    do {
        target->render([height = info.height](gfx::Surface& surface) {
            drawPattern(surface, height);
        }, gfx::Prepare::Overwrite);

        if (slow_refresh) {
            // Fast partial updates never fully resolve grayscale on this kind of panel - they
            // flatten it towards black and white. One quality pass does.
            target->refresh();
        }

        struct AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                should_close = true;
            }
        }
        if (should_close) {
            break;
        }

        if (slow_refresh) {
            // The content is static, and repeating it continuously would leave a slow panel
            // flickering through partial and quality refreshes. Draw once and wait for input.
            while (true) {
                struct AppEvent pending {};
                while (app_event_poll(&sub, &pending) == ERROR_NONE) {
                    if (pending.type == APP_EVENT_CLOSE) {
                        should_close = true;
                    }
                }
                if (should_close || input->activity()) {
                    break;
                }
                tt::kernel::delayTicks(tt::kernel::millisToTicks(50));
            }
            break;
        }

        // Give other tasks space to breathe: SPI displays would otherwise time out SPI SD access.
        tt::kernel::delayTicks(1);
    } while (!input->activity());

    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);
}
