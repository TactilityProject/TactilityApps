#include "AudioRecorder.h"

#include <lvgl_window_manager/dialog.h>
#include <app/event.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

#include <unistd.h>

#include <mutex>

namespace {

// Called whenever this window's widgets are torn down while buried (e.g. the
// delete-confirmation dialog opening on top). The poll timer is stopped here
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

    ctx.eventGroup = &event_group;
    check(task_event_group_claim_bit(&event_group, &ctx.deleteBit) == ERROR_NONE);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create_ext(app_instance_id, audiorecorderCreateWidgets, audiorecorderDestroyWidgets, &ctx);
    ctx.window = window;

    bool should_close = false;
    while (!should_close) {
        uint32_t flags = 0;
        task_event_group_wait_any(&event_group, &flags, portMAX_DELAY);

        if ((flags & ctx.deleteBit) != 0) {
            std::string path;
            {
                std::lock_guard lg(ctx.deleteMutex);
                path.swap(ctx.pendingDeletePath);
            }
            if (ctx.deleteConfirmed && !path.empty()) {
                // Only touch ctx.recordings here (no lv_obj) - this runs on the app's
                // own task, not the LVGL thread. The list rebuild happens on the poll timer.
                {
                    std::lock_guard lg(ctx.audio.pathMutex);
                    if (ctx.audio.activeFilepath == path) {
                        ctx.audio.stopRequested.store(true);
                        ctx.audio.pauseRequested.store(false);
                    }
                }
                unlink(path.c_str());
                {
                    std::lock_guard lg(ctx.recordingsMutex);
                    if (ctx.selectedPath == path) {
                        ctx.selectedPath.clear();
                    }
                }
                recorderScanDirectory(&ctx);
                ctx.listDirty = true;
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
    task_event_group_release_bit(&event_group, ctx.deleteBit);
    task_event_group_destruct(&event_group);
    audiorecorderTeardown(&ctx);

    return 0;
}

}
