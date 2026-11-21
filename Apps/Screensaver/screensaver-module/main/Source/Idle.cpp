// SPDX-License-Identifier: GPL-3.0-only
#include "Idle.h"

#include "BouncingBallsScreensaver.h"
#include "MatrixRainScreensaver.h"
#include "MystifyScreensaver.h"
#include "Screensaver.h"
#include "StackChanScreensaver.h"

#include <tactility/device.h>
#include <tactility/drivers/backlight.h>
#include <tactility/drivers/display.h>

#include <lvgl.h>
#include <lvgl/lvgl.h>

#include <cstdlib>
#include <ctime>
#include <memory>

namespace {

constexpr uint32_t TICK_INTERVAL_MS = 50;
// After this long on the screensaver the animation stops and the backlight goes off
constexpr uint32_t AUTO_OFF_MS = 5 * 60 * 1000;

struct Idle {
    lv_timer_t* timer = nullptr;
    lv_obj_t* overlay = nullptr;
    std::unique_ptr<screensaver::Screensaver> animation;
    ScreensaverSettings settings {};
    uint32_t startTick = 0;
    bool stopRequested = false;
    bool backlightOff = false;
    uint8_t savedBrightness = 0;
};

// Only used with the LVGL lock held
Idle* idle = nullptr;

template <typename Function>
bool withBacklight(Function function) {
    Device* display = nullptr;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display) != ERROR_NONE) {
        return false;
    }
    Device* backlight = nullptr;
    bool result = false;
    if (display_get_backlight(display, &backlight) == ERROR_NONE) {
        result = function(backlight);
        device_put(backlight);
    }
    device_put(display);
    return result;
}

void turnBacklightOff() {
    withBacklight([](Device* backlight) {
        if (backlight_get_brightness(backlight, &idle->savedBrightness) != ERROR_NONE) {
            return false;
        }
        idle->backlightOff = backlight_set_brightness(backlight, 0) == ERROR_NONE;
        return true;
    });
}

void restoreBacklight() {
    if (idle->backlightOff) {
        withBacklight([](Device* backlight) { return backlight_set_brightness(backlight, idle->savedBrightness) == ERROR_NONE; });
        idle->backlightOff = false;
    }
}

void onOverlayClicked(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    idle->stopRequested = true;
    lv_display_trigger_activity(nullptr);
}

std::unique_ptr<screensaver::Screensaver> createAnimation(uint8_t type) {
    switch (type) {
        case SCREENSAVER_TYPE_BOUNCING_BALLS: return std::make_unique<screensaver::BouncingBallsScreensaver>();
        case SCREENSAVER_TYPE_MYSTIFY: return std::make_unique<screensaver::MystifyScreensaver>();
        case SCREENSAVER_TYPE_MATRIX_RAIN: return std::make_unique<screensaver::MatrixRainScreensaver>();
        case SCREENSAVER_TYPE_STACKCHAN: return std::make_unique<screensaver::StackChanScreensaver>();
        default: return nullptr;
    }
}

void startScreensaver() {
    if (idle->overlay != nullptr) {
        return;
    }
    idle->stopRequested = false;
    idle->startTick = lv_tick_get();

    const lv_coord_t width = lv_display_get_horizontal_resolution(nullptr);
    const lv_coord_t height = lv_display_get_vertical_resolution(nullptr);

    idle->overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(idle->overlay);
    lv_obj_set_size(idle->overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(idle->overlay, 0, 0);
    lv_obj_set_style_bg_color(idle->overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(idle->overlay, LV_OPA_COVER, 0);
    lv_obj_set_clickable(idle->overlay, true);
    lv_obj_add_event_cb(idle->overlay, onOverlayClicked, LV_EVENT_CLICKED, nullptr);

    idle->animation = createAnimation(idle->settings.type);
    if (idle->animation) {
        idle->animation->start(idle->overlay, width, height);
    } else {
        // A black screen has nothing to show, so the backlight goes off right away
        turnBacklightOff();
    }
}

void stopScreensaver() {
    if (idle->overlay == nullptr) {
        return;
    }
    if (idle->animation) {
        idle->animation->stop();
        idle->animation.reset();
    }
    lv_obj_delete(idle->overlay);
    idle->overlay = nullptr;
    idle->stopRequested = false;
    restoreBacklight();
}

void onTick(lv_timer_t*) {
    if (lv_display_get_default() == nullptr) {
        return;
    }

    if (idle->overlay == nullptr) {
        if (idle->settings.enabled && idle->settings.timeout_ms > 0 && lv_display_get_inactive_time(nullptr) >= idle->settings.timeout_ms) {
            startScreensaver();
        }
        return;
    }

    // Input after the start means that the user is back, which also holds for a preview
    const uint32_t sinceStart = lv_tick_elaps(idle->startTick);
    if (idle->stopRequested || lv_display_get_inactive_time(nullptr) < sinceStart) {
        stopScreensaver();
        return;
    }

    if (!idle->backlightOff) {
        if (sinceStart >= AUTO_OFF_MS) {
            if (idle->animation) {
                idle->animation->stop();
                idle->animation.reset();
            }
            turnBacklightOff();
        } else if (idle->animation) {
            idle->animation->update(lv_display_get_horizontal_resolution(nullptr), lv_display_get_vertical_resolution(nullptr));
        }
    }
}

}

bool idleStart(const ScreensaverSettings& settings) {
    if (!lvgl_is_running()) {
        return false;
    }
    srand(static_cast<unsigned int>(time(nullptr)));
    lvgl_lock();
    idle = new Idle();
    idle->settings = settings;
    idle->timer = lv_timer_create(onTick, TICK_INTERVAL_MS, nullptr);
    lvgl_unlock();
    return true;
}

void idleStop() {
    lvgl_lock();
    if (idle != nullptr) {
        stopScreensaver();
        lv_timer_delete(idle->timer);
        delete idle;
        idle = nullptr;
    }
    lvgl_unlock();
}

void idleApplySettings(const ScreensaverSettings& settings) {
    lvgl_lock();
    if (idle != nullptr) {
        idle->settings = settings;
    }
    lvgl_unlock();
}

void idlePreview() {
    lvgl_lock();
    if (idle != nullptr) {
        startScreensaver();
    }
    lvgl_unlock();
}

ScreensaverSettings idleGetSettings() {
    lvgl_lock();
    const ScreensaverSettings settings = idle != nullptr ? idle->settings : ScreensaverSettings {};
    lvgl_unlock();
    return settings;
}
