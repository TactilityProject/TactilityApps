#include "TwoEleven.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>

extern "C" {

int main(int argc, char* argv[]) {
    constexpr TickType_t LVGL_LOCK_TIMEOUT = 500;

    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = app_instance_id;

    struct AppEventSubscription sub {};
    sub.app_instance_id = app_instance_id;
    app_event_subscribe(&sub);

    WindowId window = window_manager_create(app_instance_id, twoElevenCreateWidgets, &ctx);
    ctx.window = window;

    // First launch: nothing active yet - open the selection dialog. Every later transition is
    // likewise driven from here (APP_EVENT_RESULT below), never from twoElevenCreateWidgets -
    // see TwoEleven.h's comment on why.
    twoElevenShowSelectionDialog(&ctx);

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

                if (launch_id == ctx.selectionDialogId && ctx.selectionDialogId != 0) {
                    ctx.selectionDialogId = 0;
                    int32_t selection = event.result.result;
                    app_manager_stop(launch_id);

                    if (selection == TWOELEVEN_SELECTION_HOW_TO_PLAY) {
                        twoElevenShowHelpDialog(&ctx);
                    } else if (selection >= TWOELEVEN_SELECTION_3X3 && selection <= TWOELEVEN_SELECTION_6X6) {
                        if (lvgl_try_lock(LVGL_LOCK_TIMEOUT)) {
                            twoElevenStartGame(&ctx, selection);
                            lvgl_unlock();
                        }
                    } else {
                        // Closed without selecting - close self.
                        app_manager_finish(app_instance_id);
                        should_close = true;
                    }

                } else if (launch_id == ctx.helpDialogId && ctx.helpDialogId != 0) {
                    ctx.helpDialogId = 0;
                    app_manager_stop(launch_id);
                    // Return to selection dialog
                    twoElevenShowSelectionDialog(&ctx);

                } else if (launch_id == ctx.gameOverDialogId && ctx.gameOverDialogId != 0) {
                    ctx.gameOverDialogId = 0;
                    app_manager_stop(launch_id);
                    // Game has genuinely ended - clear it and return to the selection dialog.
                    if (lvgl_try_lock(LVGL_LOCK_TIMEOUT)) {
                        twoElevenClearGame(&ctx);
                        lvgl_unlock();
                    }
                    twoElevenShowSelectionDialog(&ctx);
                }
                break;
            }

            default:
                break;
        }
    }

    window_manager_remove(window);
    app_event_unsubscribe(&sub);
    twoElevenTeardown(&ctx);

    return 0;
}

}
