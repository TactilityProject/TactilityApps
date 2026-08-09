#include "Snake.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = app_instance_id;

    struct AppEventSubscription sub {};
    sub.app_instance_id = app_instance_id;
    app_event_subscribe(&sub);

    WindowId window = window_manager_create(app_instance_id, snakeCreateWidgets, &ctx);
    ctx.window = window;

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

                // Don't manipulate LVGL objects here - they may be invalid (this window may
                // still be buried, or mid-rebuild). Just store state for snakeCreateWidgets to
                // handle once it runs again.
                if (launch_id == ctx.selectionDialogId && ctx.selectionDialogId != 0) {
                    ctx.selectionDialogId = 0;
                    int32_t selection = event.result.result;

                    if (selection == SNAKE_SELECTION_HOW_TO_PLAY) {
                        ctx.showHelpOnShow = true;
                    } else if (selection >= SNAKE_SELECTION_EASY && selection <= SNAKE_SELECTION_HELL) {
                        ctx.pendingSelection = selection;
                    } else {
                        // Closed without selecting
                        ctx.shouldExit = true;
                    }
                    app_manager_stop(launch_id);

                } else if (launch_id == ctx.helpDialogId && ctx.helpDialogId != 0) {
                    ctx.helpDialogId = 0;
                    // Return to selection dialog
                    ctx.pendingSelection = -1;
                    app_manager_stop(launch_id);

                } else if (launch_id == ctx.gameOverDialogId && ctx.gameOverDialogId != 0) {
                    ctx.gameOverDialogId = 0;
                    // Game has genuinely ended - return to selection dialog rather than letting
                    // snakeCreateWidgets' resurface handling start a fresh game at the same
                    // difficulty (that path is only for burial by something else entirely).
                    ctx.pendingSelection = -1;
                    ctx.currentDifficulty = -1;
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
    snakeTeardown(&ctx);

    return 0;
}

}
