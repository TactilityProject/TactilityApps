#pragma once

#include <app/instance.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/drivers/audio_stream.h>

constexpr auto* TAG = "AudioRecorder";
constexpr auto* APP_ID = "tactility.audiorecorder";

/** Longest recording the service makes, in seconds */
constexpr uint32_t RECORDER_MAX_SECONDS = 300;
/** PCM bytes that the service writes per second (16 kHz, mono, 16-bit), for showing a file's duration */
constexpr uint32_t RECORDER_BYTES_PER_SECOND = 16000 * 1 * (16 / 8);
/** Size of the WAV header in front of the PCM data */
constexpr uint32_t RECORDER_WAV_HEADER_BYTES = 44;

enum class RecorderState : uint8_t {
    Idle,
    Recording,
    Playing,
    Paused,
};

// UI thread -> audio thread request. Only used to kick off a new activity;
// pause/resume/stop of an in-progress activity go through the atomics below
// instead, since the task is blocked inside doRecord/doPlayback and won't
// see a new `pending` value until that call returns.
enum class RecorderRequest : uint8_t {
    None,
    StartRecording,
    StartPlayback,
};

struct RecordingItem {
    std::string filename;
    std::string fullpath;
    size_t sizeBytes = 0;
    time_t mtime = 0;
};

// Shared state between the LVGL thread and the audio task. The audio task
// never touches lv_obj; it publishes state the poll timer reads.
struct RecorderAudioState {
    std::atomic<RecorderRequest> pending{RecorderRequest::None};
    std::atomic<bool> shutdown{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> pauseRequested{false};
    std::atomic<RecorderState> state{RecorderState::Idle};

    std::atomic<uint32_t> currentTimeSec{0};
    std::atomic<uint32_t> totalTimeSec{0};

    std::mutex pathMutex;
    std::string recordDir;             // set once at init; directory new recordings are created in
    std::string requestedPlaybackPath; // set by UI before posting StartPlayback
    std::string activeFilepath;        // path currently recording/playing (published by task)

    // Set by the audio task when a recording finishes, so the poll timer can
    // rescan the directory without the task touching lv_obj directly.
    std::atomic<bool> recordingFinished{false};

    TaskHandle_t task = nullptr;
};

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    std::string appDataDir;
    RecorderAudioState audio;

    // recordings is written by recorderScanDirectory() and read by recorderRenderList().
    // Both the LVGL thread (poll timer, button callbacks) and this app's own task
    // (main.cpp's APP_EVENT_RESULT handler, after a delete confirmation) call
    // recorderScanDirectory(), so it's guarded by recordingsMutex; listDirty tells the
    // poll timer a rescan happened off-thread and recorderRenderList() (lv_obj access,
    // LVGL-thread-only) needs to run.
    std::mutex recordingsMutex;
    std::vector<RecordingItem> recordings;
    std::atomic<bool> listDirty{false};
    bool backendStarted = false;

    // Instance id of the currently-shown "delete" AlertDialog, or 0 if none is pending.
    uint32_t pendingDeleteDialogId = 0;
    std::string pendingDeletePath;

    // Fullpath of the recording selected in the list (row tap), or empty if none.
    // The player card acts on this; the toolbar delete button deletes it.
    std::string selectedPath;

    lv_obj_t* toolbar = nullptr;
    lv_obj_t* main_container = nullptr;
    lv_obj_t* refresh_btn = nullptr;
    lv_obj_t* delete_btn = nullptr;

    /* Recording Card */
    lv_obj_t* rec_card = nullptr;
    lv_obj_t* rec_status_label = nullptr;
    lv_obj_t* rec_limit_label = nullptr;
    lv_obj_t* rec_time_label = nullptr;
    lv_obj_t* rec_action_btn = nullptr;
    lv_obj_t* rec_action_label = nullptr;

    /* Inline player, created under the selected list item */
    lv_obj_t* player_panel = nullptr;
    lv_obj_t* play_bar = nullptr;
    lv_obj_t* play_time_label = nullptr;
    lv_obj_t* play_toggle_btn = nullptr;
    lv_obj_t* play_toggle_label = nullptr;
    lv_obj_t* play_stop_btn = nullptr;

    /* List Card */
    lv_obj_t* list_card = nullptr;
    lv_obj_t* list_title_label = nullptr;
    lv_obj_t* list_count_label = nullptr;
    lv_obj_t* list_container = nullptr;
    lv_obj_t* empty_container = nullptr;

    lv_timer_t* pollTimer = nullptr;
    RecorderState lastRenderedState = RecorderState::Idle;
    uint32_t lastRenderedSecond = ~0u;
    uint32_t lastRenderedTotal = ~0u;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void audiorecorderCreateWidgets(lv_obj_t* parent, void* userData);

/** Frees the per-row strings while their widgets still exist (called before the window's widgets are deleted). */
void recorderFreeRowData(Context* ctx);

/** Tears down widgets and stops the poll timer. Call once the window is torn down. */
void audiorecorderTeardown(Context* ctx);

// ---- RecorderService.cpp ---------------------------------------------------

/** Entry point for the dedicated audio task (record OR play, one at a time). */
void recorderTaskEntry(void* param);

/** Scans the app's recordings directory, newest first. */
void recorderScanDirectory(Context* ctx);

// ---- AudioRecorder.cpp -----------------------------------------------------

void recorderInitAppData(Context* ctx);
void recorderEnsureBackendStarted(Context* ctx);
void recorderRefreshFromState(Context* ctx);
void recorderRenderList(Context* ctx);
void recorderRequestDelete(Context* ctx, const std::string& fullpath);
void recorderSelectRecording(Context* ctx, const std::string& fullpath);
