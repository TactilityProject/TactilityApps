#include "TamaTac.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

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

    struct AppEventSubscription sub {};
    sub.app_instance_id = app_instance_id;
    app_event_subscribe(&sub);

    WindowId window = window_manager_create(app_instance_id, tamaTacCreateWidgets, ctx.get());
    ctx->window = window;

    bool should_close = false;
    while (!should_close) {
        struct AppEvent event;
        if (app_event_await(&sub, &event, portMAX_DELAY) != ERROR_NONE) {
            break;
        }
        switch (event.type) {
            case APP_EVENT_CLOSE:
                app_manager_finish(app_instance_id);
                should_close = true;
                break;

            case APP_EVENT_RESULT: {
                uint32_t launch_id = event.result.launch_id;
                if (launch_id == ctx->resetDialogId && ctx->resetDialogId != 0) {
                    ctx->resetDialogId = 0;
                    int32_t buttonIndex = event.result.result;

                    if (buttonIndex == 0) {
                        // User picked "Reset" (first button)
                        if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

                        ctx->petLogic.reset();
                        ctx->petLogic.saveState();
                        ctx->lastKnownStage = LifeStage::Egg;
                        ctx->pendingResetUI = true;  // Defer UI update to MainView's anim timer

                        if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);
                    }
                    app_manager_stop(launch_id);
                }
                break;
            }

            default:
                break;
        }
    }

    window_manager_remove(window);
    app_event_unsubscribe(&sub);
    tamaTacTeardown(ctx.get());

    return 0;
}

}
