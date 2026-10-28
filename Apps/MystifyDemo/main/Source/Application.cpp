#include "Application.h"
#include "MystifyDemo.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <app/event.h>
#include <tactility/check.h>

#include <Tactility/kernel/Kernel.h>

constexpr auto TAG = "Application";

// The frame budget is measured from the start of a frame, so drawing time counts towards it
// instead of being added to it. It's only a cap: a slower panel just runs slower.
constexpr int TARGET_FPS = 60;

// Prints fps and per-phase render timings once a second. Useful when bringing up a new panel.
constexpr bool LOG_STATS = false;

void runApplication(gfx::Target* target, AnyInput* input) {
    MystifyDemo mystify;
    if (!mystify.init(target)) {
        ESP_LOGE(TAG, "Failed to initialize MystifyDemo");
        return;
    }

    const gfx::Info& info = target->info();
    ESP_LOGI(TAG, "Starting Mystify demo (%dx%d) - any input to exit", info.width, info.height);

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);
    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    const int64_t frameBudgetUs = 1000000 / TARGET_FPS;

    bool should_close = false;

    int64_t window_start_us = esp_timer_get_time();
    int window_frames = 0;
    int64_t sum_copy_us = 0;
    int64_t sum_draw_us = 0;
    int64_t sum_sync_us = 0;
    int64_t sum_present_us = 0;
    int64_t sum_total_us = 0;

    do {
        const int64_t frame_start_us = esp_timer_get_time();

        mystify.update();

        if (LOG_STATS) {
            const gfx::Stats& stats = target->stats();
            sum_copy_us += stats.copyUs;
            sum_draw_us += stats.drawUs;
            sum_sync_us += stats.syncUs;
            sum_present_us += stats.presentUs;
            sum_total_us += stats.totalUs;
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

        const int64_t frame_us = esp_timer_get_time() - frame_start_us;
        if (frame_us < frameBudgetUs) {
            const int64_t remaining_ms = (frameBudgetUs - frame_us + 999) / 1000;
            tt::kernel::delayTicks(tt::kernel::millisToTicks(static_cast<uint32_t>(remaining_ms)));
        }

        if (LOG_STATS) {
            window_frames++;
            const int64_t elapsed_us = esp_timer_get_time() - window_start_us;
            if (elapsed_us >= 1000000) {
                const gfx::Stats& stats = target->stats();
                const int fps = static_cast<int>((static_cast<int64_t>(window_frames) * 1000000) / elapsed_us);
                const int frames = window_frames;
                ESP_LOGI(TAG, "%d fps, %d rects, %d px | copy %d draw %d sync %d present %d us (total %d)",
                    fps,
                    stats.dirtyRects,
                    stats.dirtyPixels,
                    static_cast<int>(sum_copy_us / frames),
                    static_cast<int>(sum_draw_us / frames),
                    static_cast<int>(sum_sync_us / frames),
                    static_cast<int>(sum_present_us / frames),
                    static_cast<int>(sum_total_us / frames)
                );
                window_start_us = esp_timer_get_time();
                window_frames = 0;
                sum_copy_us = 0;
                sum_draw_us = 0;
                sum_sync_us = 0;
                sum_present_us = 0;
                sum_total_us = 0;
            }
        }
    } while (!input->activity());

    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    ESP_LOGI(TAG, "Mystify demo ended");
}
