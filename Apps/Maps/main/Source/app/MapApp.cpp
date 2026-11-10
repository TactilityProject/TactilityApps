// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapView.h"

#include "../platform/Platform.h"

#include <app/paths.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/module.h>
#include <tactility/paths.h>
#include <lvgl/module.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <new>

namespace maps {

namespace {

/**
 * One line per run in sessions.txt: uptime in seconds, then free PSRAM and internal RAM in KB at the start and at the
 * exit. Memory that another run or app kept shows as a low start value. The file keeps its last lines only.
 */
void logSession(const char* root, uint32_t startPsram, uint32_t startInternal) {
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/sessions.txt", root);
    char kept[1536] = "";
    size_t used = 0;
    if (FILE* file = fopen(path, "r")) {
        used = fread(kept, 1, sizeof(kept) - 1, file);
        fclose(file);
    }
    kept[used] = '\0';
    // Old lines go once the file passes about 1 KB
    const char* from = kept;
    while (strlen(from) > 1024) {
        const char* next = strchr(from, '\n');
        if (next == nullptr) {
            break;
        }
        from = next + 1;
    }
    char line[96];
    snprintf(line, sizeof(line), "uptime %u s  start %u/%u KB  exit %u/%u KB (PSRAM/internal)\n", static_cast<unsigned>(nowMs() / 1000),
        static_cast<unsigned>(startPsram / 1024), static_cast<unsigned>(startInternal / 1024),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    if (FILE* file = fopen(path, "w")) {
        fputs(from, file);
        fputs(line, file);
        fclose(file);
    }
}


void runFullscreen(Device* display, const char* root, const char* dataPath) {
    const bool lvglActive = module_is_started(&lvgl_module);
    if (lvglActive) {
        const uint32_t start = nowMs();
        module_stop(&lvgl_module);
        logInfo("LVGL stopped in %u ms", nowMs() - start);
    }

    // Above the render lanes and the I/O task (4): the loop sleeps between frames, so it only takes the CPU to
    // answer input and draw. At the same priority frames took 150-270 ms while tiles were loading.
    const UBaseType_t appPriority = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, appPriority + 1);

    // The view is large, it lives in external RAM where the device has it
    void* memory = largeAlloc(sizeof(MapView));
    if (memory != nullptr) {
        auto* view = new (memory) MapView();
        if (view->begin(display, root, dataPath)) {
            view->run();
        }
        view->end();
        view->~MapView();
        largeFree(memory);
    } else {
        logError("Out of memory");
    }
    vTaskPrioritySet(nullptr, appPriority);

    // LVGL comes back on every exit path, also after errors
    if (lvglActive && !module_is_started(&lvgl_module)) {
        const uint32_t start = nowMs();
        module_start(&lvgl_module);
        logInfo("LVGL started in %u ms", nowMs() - start);
    }
}

bool pathExists(const char* path) {
    struct stat info {};
    return stat(path, &info) == 0;
}

/**
 * Tactility moved app data from <data>/user/app/<id> to <data>/appdata/<id> (Tactility main 2ae9194c). The OS moves the
 * whole user/app folder once, but only when appdata/ does not exist yet. If the Maps folder was left behind, move its
 * entries here: same card, so a rename each. An entry whose name already exists at the destination stays where it is
 * and is reported; nothing is ever deleted except the old folder itself once it is empty.
 */
void migrateLegacyData(const char* dataPath, const char* root) {
    char legacy[PATH_MAX_LENGTH];
    if (snprintf(legacy, sizeof(legacy), "%s/user/app/%s", dataPath, APP_ID) >= static_cast<int>(sizeof(legacy)) ||
        !pathExists(legacy) || strcmp(legacy, root) == 0) {
        return;
    }
    if (!pathExists(root)) {
        if (rename(legacy, root) == 0) {
            logInfo("Data moved from %s to %s", legacy, root);
        } else {
            logError("Data not moved from %s to %s, it stays there", legacy, root);
        }
        return;
    }
    DIR* folder = opendir(legacy);
    if (folder == nullptr) {
        return;
    }
    int moved = 0;
    int kept = 0;
    char names[16][64];
    int nameCount = 0;
    // Names first, then the renames: renaming while reading a FAT folder can skip entries
    for (dirent* entry = readdir(folder); entry != nullptr; entry = readdir(folder)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (nameCount < 16) {
            snprintf(names[nameCount++], sizeof(names[0]), "%s", entry->d_name);
        } else {
            kept++;
        }
    }
    closedir(folder);
    for (int i = 0; i < nameCount; i++) {
        char from[PATH_MAX_LENGTH];
        char to[PATH_MAX_LENGTH];
        snprintf(from, sizeof(from), "%s/%s", legacy, names[i]);
        snprintf(to, sizeof(to), "%s/%s", root, names[i]);
        if (pathExists(to)) {
            logError("Not moved, %s exists already: %s", to, from);
            kept++;
        } else if (rename(from, to) == 0) {
            logInfo("Moved %s to %s", from, to);
            moved++;
        } else {
            logError("Could not move %s", from);
            kept++;
        }
    }
    if (kept == 0 && rmdir(legacy) == 0) {
        logInfo("Removed the empty old folder %s", legacy);
    }
    logInfo("Old data folder: %d entries moved, %d left in %s", moved, kept, legacy);
}

} // namespace

} // namespace maps

extern "C" int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    maps::logMemory("start");
    char root[maps::PATH_MAX_LENGTH];
    if (app_paths_get_user_data_directory(maps::APP_ID, root, sizeof(root)) != ERROR_NONE) {
        maps::logError("No user data directory");
        return 0;
    }
    char dataPath[maps::PATH_MAX_LENGTH] = "";
    paths_get_data_path(dataPath, sizeof(dataPath));
    maps::migrateLegacyData(dataPath, root);
    mkdir(root, 0777);
    char mapsFolder[maps::PATH_MAX_LENGTH];
    snprintf(mapsFolder, sizeof(mapsFolder), "%s/maps", root);
    mkdir(mapsFolder, 0777);

    Device* display = nullptr;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display) != ERROR_NONE) {
        maps::logError("No display");
        return 0;
    }
    const auto startPsram = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    const auto startInternal = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    maps::runFullscreen(display, root, dataPath);
    device_put(display);
    maps::logMemory("exit");
    maps::logSession(root, startPsram, startInternal);
    return 0;
}
