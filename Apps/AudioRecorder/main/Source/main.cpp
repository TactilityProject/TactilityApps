#include "AudioRecorder.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

#include <unistd.h>

#include <mutex>

namespace {

// Called whenever this window's widgets are torn down while buried (e.g. the
// AlertDialog delete-confirmation opening on top). The poll timer is stopped here
// and the list rows' heap strings are freed while the widgets still exist - session
// state (recordings, selection, the audio task) must survive burial since
// audiorecorderCreateWidgets() reruns and rebuilds the widgets from scratch when this
// window resurfaces.
void audiorecorderDestroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    if (ctx->pollTimer != nullptr) {
        lv_timer_del(ctx->pollTimer);
        ctx->pollTimer = nullptr;
    }
    // Runs while the widgets still exist, before the window manager deletes them
    recorderFreeRowData(ctx);
}

} // namespace

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = app_instance_id;

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create_ext(app_instance_id, audiorecorderCreateWidgets, audiorecorderDestroyWidgets, &ctx);
    ctx.window = window;

    bool should_close = false;
    while (!should_close) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        struct AppEvent event;
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            switch (event.type) {
                case APP_EVENT_CLOSE:
                    should_close = true;
                    break;

                case APP_EVENT_RESULT: {
                    uint32_t launch_id = event.result.launch_id;
                    if (launch_id == ctx.pendingDeleteDialogId && ctx.pendingDeleteDialogId != 0) {
                        ctx.pendingDeleteDialogId = 0;
                        int32_t buttonIndex = event.result.result;

                        if (buttonIndex == 0 && !ctx.pendingDeletePath.empty()) {
                            // User picked "Delete" (first button). Only touch
                            // ctx.recordings here (no lv_obj) - this runs on the
                            // app's own task, not the LVGL thread. The actual
                            // list rebuild happens on the poll timer instead.
                            {
                                std::lock_guard lg(ctx.audio.pathMutex);
                                if (ctx.audio.activeFilepath == ctx.pendingDeletePath) {
                                    ctx.audio.stopRequested.store(true);
                                    ctx.audio.pauseRequested.store(false);
                                }
                            }
                            unlink(ctx.pendingDeletePath.c_str());
                            {
                                std::lock_guard lg(ctx.recordingsMutex);
                                if (ctx.selectedPath == ctx.pendingDeletePath) {
                                    ctx.selectedPath.clear();
                                }
                            }
                            recorderScanDirectory(&ctx);
                            ctx.listDirty = true;
                        }
                        ctx.pendingDeletePath.clear();
                        app_manager_stop(launch_id);
                    }
                    break;
                }

                default:
                    break;
            }
            if (should_close) break;
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);
    audiorecorderTeardown(&ctx);

    return 0;
}

}
