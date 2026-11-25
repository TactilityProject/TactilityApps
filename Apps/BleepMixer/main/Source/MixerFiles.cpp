#include "Mixer.h"

#include <lvgl_window_manager/dialog.h>

#include <tactility/log.h>

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

namespace {

constexpr auto* TAG = "BleepMixer";
constexpr auto* EXTENSION = ".bleep";

void makeDirectories(const std::string& path) {
    std::string partial;
    for (size_t i = 0; i < path.size(); i++) {
        partial.push_back(path[i]);
        if ((path[i] == '/' && i > 0) || i + 1 == path.size()) {
            mkdir(partial.c_str(), 0755); // fails harmlessly when it exists
        }
    }
}

/** @return the names (without extension) of the .bleep files in a directory, sorted */
std::vector<std::string> listSongs(const std::string& directory) {
    std::vector<std::string> names;
    if (directory.empty()) return names;
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) return names;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        const size_t ext = name.size() > 6 ? name.size() - 6 : std::string::npos;
        if (ext != std::string::npos && name.compare(ext, 6, EXTENSION) == 0) {
            names.push_back(name.substr(0, ext));
        }
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

void showMessage(Context& ctx, const char* title, const std::string& message) {
    static const char* const ok[] = { "OK" };
    window_manager_alert_dialog(ctx.appInstanceId, title, message.c_str(), ok, 1, nullptr, nullptr);
}

/** Writes the song as name.bleep. Call without the LVGL lock. */
void saveAs(Context& ctx, const std::string& name) {
    lvgl_lock();
    const std::string text = bleep::formatSong(ctx.song, "Made with BleepMixer");
    lvgl_unlock();
    makeDirectories(ctx.songsDirectory);
    const std::string path = ctx.songsDirectory + "/" + name + EXTENSION;
    if (!writeTextFile(path, text)) {
        showMessage(ctx, "Not saved", "Could not write " + path);
        return;
    }
    LOG_I(TAG, "Saved %s", path.c_str());
    lvgl_lock();
    ctx.name = name;
    ctx.dirty = false;
    mixerSongChanged(ctx);
    lvgl_unlock();
}

void onOpenChosen(int32_t index, void* userData) {
    auto& ctx = *static_cast<Context*>(userData);
    if (index < 0 || index >= (int32_t)ctx.openPaths.size()) {
        return;
    }
    bleep::Song song;
    std::string name;
    if (index == 0) {
        song = newSong();
    } else {
        std::string text;
        std::string error;
        const std::string& path = ctx.openPaths[index];
        if (!readTextFile(path, text) || !bleep::parseSong(text, song, error)) {
            showMessage(ctx, "Can't open", path + (error.empty() ? "" : "\n" + error));
            return;
        }
        // Examples open as new songs, saved under the user's songs
        const size_t slash = path.find_last_of('/');
        name = path.substr(slash + 1, path.size() - slash - 1 - 6);
    }
    lvgl_lock();
    ctx.player.stop();
    ctx.song = std::move(song);
    ctx.name = name;
    ctx.dirty = false;
    ctx.bar = 0;
    ctx.row = 0;
    ctx.scrollRow = 0;
    mixerSongChanged(ctx);
    lvgl_unlock();
}

void onSaveAsEntered(bool confirmed, const char* text, void* userData) {
    auto& ctx = *static_cast<Context*>(userData);
    if (!confirmed) return;
    const std::string name = sanitiseName(text);
    if (name.empty()) {
        showMessage(ctx, "Not saved", "Use letters, digits, '-' or '_' for the name.");
        return;
    }
    saveAs(ctx, name);
}

} // namespace

bool readTextFile(const std::string& path, std::string& out) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    out.clear();
    char buffer[512];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, n);
    }
    fclose(file);
    return true;
}

bool writeTextFile(const std::string& path, const std::string& text) {
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
    return fclose(file) == 0 && ok;
}

bleep::Song newSong() {
    bleep::Song song;
    song.bpm = 120;
    song.bars.resize(4);
    for (auto& bar : song.bars) {
        for (int row = 0; row < bleep::ROWS_PER_BAR; row++) {
            uint8_t drum = bleep::CellRest;
            if (row % 8 == 0) drum = bleep::CellKick;
            else if (row % 8 == 4) drum = bleep::CellSnare;
            else if (row % 2 == 0) drum = bleep::CellHat;
            bar.cells[bleep::Drums][row] = drum;
        }
    }
    return song;
}

std::string sanitiseName(const char* text) {
    std::string name;
    for (const char* c = text; c != nullptr && *c != '\0' && name.size() < 40; c++) {
        const char ch = *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
            name.push_back(ch);
        } else if (ch == ' ' && !name.empty() && name.back() != '-') {
            name.push_back('-');
        }
    }
    while (!name.empty() && name.back() == '-') name.pop_back();
    return name;
}

void askSaveAs(Context& ctx) {
    window_manager_input_dialog(ctx.appInstanceId, "Save as", "Name of the song", ctx.name.c_str(), "Save", "Cancel",
                                onSaveAsEntered, &ctx);
}

void runRequest(Context& ctx, Request request) {
    switch (request) {
        case Request::Save: {
            lvgl_lock();
            const std::string name = ctx.name;
            lvgl_unlock();
            if (name.empty()) {
                askSaveAs(ctx);
            } else {
                saveAs(ctx, name);
            }
            break;
        }
        case Request::ListForOpen: {
            ctx.openPaths.clear();
            ctx.openLabels.clear();
            ctx.openPaths.emplace_back();
            ctx.openLabels.emplace_back("New song");
            for (const auto& name : listSongs(ctx.songsDirectory)) {
                ctx.openPaths.push_back(ctx.songsDirectory + "/" + name + EXTENSION);
                ctx.openLabels.push_back(name);
            }
            for (const auto& name : listSongs(ctx.examplesDirectory)) {
                ctx.openPaths.push_back(ctx.examplesDirectory + "/" + name + EXTENSION);
                ctx.openLabels.push_back(name + " (example)");
            }
            std::vector<const char*> items;
            for (const auto& label : ctx.openLabels) items.push_back(label.c_str());
            window_manager_selection_dialog(ctx.appInstanceId, "Open", items.data(), (uint32_t)items.size(), onOpenChosen, &ctx);
            break;
        }
        case Request::None:
            break;
    }
}
