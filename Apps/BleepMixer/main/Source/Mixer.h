#pragma once

#include <Bleep/AudioOutput.h>
#include <Bleep/Player.h>
#include <Bleep/Song.h>

#include <app/instance.h>
#include <lvgl/lvgl.h>

#include <atomic>
#include <string>
#include <vector>

constexpr auto* APP_ID = "tactility.bleepmixer";

/** Work for the app's main task: file access stays off the LVGL task. */
enum class Request : int { None, Save, ListForOpen };

struct Context {
    AppInstanceId appInstanceId = 0;
    std::string songsDirectory; // the user's songs (user data)/songs
    std::string examplesDirectory; // (assets)/examples

    // The song being edited. Changed on the LVGL task only (or with the LVGL lock held).
    bleep::Song song;
    std::string name; // file name without .bleep; empty for a new song
    bool dirty = false;

    bleep::Player player;
    bleep::AudioOutput audio;
    bool audioAvailable = false;

    // Editor state (LVGL task)
    int bar = 0;
    int row = 0;
    int channel = 0;
    int octave = 4;
    bool loopBar = false;
    uint8_t muted = 0;
    bool keypadVisible = true;
    int scrollRow = 0; // first row shown in the grid
    int shownPlayPosition = -1;

    std::atomic<Request> request { Request::None };
    // Open dialog: paths and labels of the songs found, item 0 is "New song"
    std::vector<std::string> openPaths;
    std::vector<std::string> openLabels;

    // Widgets, null while the window isn't shown
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* playLabel = nullptr;
    lv_obj_t* barLabel = nullptr;
    lv_obj_t* bpmLabel = nullptr;
    lv_obj_t* loopButton = nullptr;
    lv_obj_t* grid = nullptr;
    lv_obj_t* keypad = nullptr;
    lv_timer_t* timer = nullptr;
    int dragStartY = 0;
    int dragStartScroll = 0;
    bool dragged = false;
};

// MixerUi.cpp
void mixerCreateWidgets(lv_obj_t* root, void* userData);
void mixerDestroyWidgets(void* userData);
/** Refreshes the widgets after the song was replaced. Call with the LVGL lock held. */
void mixerSongChanged(Context& ctx);

// MixerFiles.cpp
bool readTextFile(const std::string& path, std::string& out);
bool writeTextFile(const std::string& path, const std::string& text);
/** A new song: one bar with a simple beat */
bleep::Song newSong();
/** Runs a request on the main task. */
void runRequest(Context& ctx, Request request);
/** Saves under a new name, asking for it. Call from any task. */
void askSaveAs(Context& ctx);
/** Turns user input into a safe file name (may return empty) */
std::string sanitiseName(const char* text);
