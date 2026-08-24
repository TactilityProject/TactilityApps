#include "EpubReader.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = app_instance_id;
    if (argc > 0) {
        ctx.launchFilePath_ = argv[0];
    }

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(app_instance_id, epubReaderCreateWidgets, &ctx);

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

                    if (launch_id == ctx.psramAlertId_) {
                        // PSRAM alert closed - this app cannot run without PSRAM, close self.
                        ctx.psramAlertId_ = 0;
                        app_manager_stop(launch_id);
                        should_close = true;
                    } else if (launch_id == ctx.tocDialogId_) {
                        ctx.tocDialogId_ = 0;
                        int32_t selection = event.result.result;
                        if (selection >= 0 && ctx.epub_) {
                            const auto& toc = ctx.epub_->getToc();
                            const auto& spine = ctx.epub_->getSpine();
                            if (selection < (int32_t)toc.size()) {
                                for (size_t i = 0; i < spine.size(); ++i) {
                                    if (spine[i].href == toc[(size_t)selection].href) {
                                        lvgl_lock();
                                        loadChapter(&ctx, (int)i, 0);
                                        lvgl_unlock();
                                        break;
                                    }
                                }
                            }
                        }
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
    epubReaderTeardown(&ctx);

    return 0;
}

}
