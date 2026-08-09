#include "SerialConsole.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <memory>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Heap-allocated: ConsoleViewState spawns two background threads (uartThread/viewThread)
    // whose lambdas capture this Context's address, and those threads keep running across a
    // window burial/resurface cycle (only the widget tree gets destroyed then, see
    // ConsoleView.h's consoleViewRebuildWidgets()) - so it can't be a stack frame tied to this
    // function's own lifetime in the way a simpler app's Context could be.
    auto ctx = std::make_unique<Context>();
    ctx->appInstanceId = app_instance_id;

    struct AppEventSubscription sub {};
    sub.app_instance_id = app_instance_id;
    app_event_subscribe(&sub);

    WindowId window = window_manager_create(app_instance_id, serialConsoleCreateWidgets, ctx.get());
    ctx->window = window;

    bool should_close = false;
    while (!should_close) {
        struct AppEvent event;
        if (app_event_await(&sub, &event, portMAX_DELAY) != ERROR_NONE) {
            break;
        }
        if (event.type == APP_EVENT_CLOSE) {
            app_manager_finish(app_instance_id);
            should_close = true;
        }
    }

    window_manager_remove(window);
    app_event_unsubscribe(&sub);
    serialConsoleTeardown(ctx.get());

    return 0;
}

}
