#include "EpubReader.h"

#include <lvgl_window_manager/dialog.h>
#include <app/event.h>
#include <app/scheduler.h>

#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

#include <esp_heap_caps.h>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = app_instance_id;

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    ctx.eventGroup_ = &event_group;
    check(task_event_group_claim_bit(&event_group, &ctx.dialogBit_) == ERROR_NONE);
    check(task_event_group_claim_bit(&event_group, &ctx.openFileBit_) == ERROR_NONE);
    check(task_event_group_claim_bit(&event_group, &ctx.psramAlertBit_) == ERROR_NONE);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    const bool hasPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    if (hasPsram) {
        epubReaderInit(&ctx);
    }

    WindowId window = window_manager_create_ext(app_instance_id, epubReaderCreateWidgets, epubReaderDestroyWidgets, &ctx);

    bool should_close = false;
    if (!hasPsram) {
        static const char* const buttons[] = { "OK" };
        const error_t result = window_manager_alert_dialog(app_instance_id, "PSRAM Required",
            "ePub Reader requires a device with PSRAM and cannot run on this hardware.", buttons, 1,
            [](int32_t, void* userData) {
                auto* context = static_cast<Context*>(userData);
                task_event_group_signal(context->eventGroup_, context->psramAlertBit_);
            }, &ctx);
        should_close = result != ERROR_NONE;
    } else if (argc > 0 && argv[0][0] != '\0') {
        // Started to open a specific file
        lvgl_lock();
        openBook(&ctx, argv[0]);
        showView(&ctx);
        lvgl_unlock();
    }

    while (!should_close) {
        uint32_t flags = 0;
        task_event_group_wait_any(&event_group, &flags, portMAX_DELAY);

        if ((flags & ctx.psramAlertBit_) != 0) {
            // This app cannot run without PSRAM
            should_close = true;
        }
        if ((flags & ctx.dialogBit_) != 0) {
            handleDialogResult(&ctx);
        }
        if ((flags & ctx.openFileBit_) != 0) {
            startFileSelection(&ctx);
        }

        struct AppEvent event;
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            switch (event.type) {
                case APP_EVENT_CLOSE:
                    should_close = true;
                    break;

                case APP_EVENT_RESULT:
                    handleFileResult(&ctx, event.result.launch_id, event.result.result);
                    break;

                default:
                    break;
            }
            if (should_close) break;
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_release_bit(&event_group, ctx.dialogBit_);
    task_event_group_release_bit(&event_group, ctx.openFileBit_);
    task_event_group_release_bit(&event_group, ctx.psramAlertBit_);
    task_event_group_destruct(&event_group);
    epubReaderTeardown(&ctx);

    return 0;
}

}
