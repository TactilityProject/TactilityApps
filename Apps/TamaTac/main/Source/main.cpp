#include "TamaTac.h"

#include <lvgl_window_manager/dialog.h>
#include <app/event.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Heap-allocated: tamaTacInit() hands ctx's address to a raw FreeRTOS timer
    // (xTimerCreate's pvTimerID) whose callback runs on the FreeRTOS timer service task, fully
    // independent of both the LVGL thread and this window's burial state - it keeps firing for
    // the whole app session, not just while a window exists.
    auto ctx = std::make_unique<Context>();
    ctx->appInstanceId = app_instance_id;

    tamaTacInit(ctx.get());

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    ctx->eventGroup = &event_group;
    check(task_event_group_claim_bit(&event_group, &ctx->resetBit) == ERROR_NONE);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(app_instance_id, tamaTacCreateWidgets, ctx.get());
    ctx->window = window;

    bool should_close = false;
    while (!should_close) {
        uint32_t flags = 0;
        task_event_group_wait_any(&event_group, &flags, portMAX_DELAY);

        if ((flags & ctx->resetBit) != 0) {
            if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

            ctx->petLogic.reset();
            ctx->petLogic.saveState();
            ctx->lastKnownStage = LifeStage::Egg;
            ctx->lastPetTime = 0;  // Clear tap cooldown for the new pet
            // Achievements + cemetery intentionally survive: they
            // record the player's history, not this pet's life.
            ctx->pendingResetUI = true;  // Defer UI update to MainView's anim timer

            if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);
        }

        struct AppEvent event;
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            switch (event.type) {
                case APP_EVENT_CLOSE:
                    should_close = true;
                    break;

                default:
                    break;
            }
            if (should_close) break;
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_release_bit(&event_group, ctx->resetBit);
    task_event_group_destruct(&event_group);
    tamaTacTeardown(ctx.get());

    return 0;
}

}
