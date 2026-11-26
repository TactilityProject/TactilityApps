#include "Snake.h"

#include <lvgl_window_manager/dialog.h>
#include <app/event.h>
#include <app/scheduler.h>

#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

extern "C" {

int main(int argc, char* argv[]) {
    constexpr TickType_t LVGL_LOCK_TIMEOUT = 500;

    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = app_instance_id;

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    ctx.eventGroup = &event_group;
    check(task_event_group_claim_bit(&event_group, &ctx.dialogBit) == ERROR_NONE);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(app_instance_id, snakeCreateWidgets, &ctx);
    ctx.window = window;

    // First launch: nothing active yet. Later transitions follow from the dialog results below.
    snakeShowSelectionDialog(&ctx);

    bool should_close = false;
    while (!should_close) {
        uint32_t flags = 0;
        task_event_group_wait_any(&event_group, &flags, portMAX_DELAY);

        if ((flags & ctx.dialogBit) != 0) {
            const GameDialog dialog = ctx.dialog.exchange(GameDialog::None);
            const int32_t result = ctx.dialogResult;
            if (dialog == GameDialog::Selection) {
                if (result == SNAKE_SELECTION_HOW_TO_PLAY) {
                    snakeShowHelpDialog(&ctx);
                } else if (result > SNAKE_SELECTION_HOW_TO_PLAY) {
                    if (lvgl_try_lock(LVGL_LOCK_TIMEOUT)) {
                        snakeStartGame(&ctx, result);
                        lvgl_unlock();
                    }
                } else {
                    // Closed without selecting - close self.
                    should_close = true;
                }
            } else if (dialog == GameDialog::Help) {
                snakeShowSelectionDialog(&ctx);
            } else if (dialog == GameDialog::GameOver) {
                // Game has ended - clear it and return to the selection dialog.
                if (lvgl_try_lock(LVGL_LOCK_TIMEOUT)) {
                    snakeClearGame(&ctx);
                    lvgl_unlock();
                }
                snakeShowSelectionDialog(&ctx);
            }
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
    task_event_group_release_bit(&event_group, ctx.dialogBit);
    task_event_group_destruct(&event_group);
    snakeTeardown(&ctx);

    return 0;
}

}
