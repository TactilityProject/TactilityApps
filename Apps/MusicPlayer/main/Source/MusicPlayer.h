#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <app/instance.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>

constexpr auto* MUSIC_PLAYER_TAG = "MusicPlayer";

struct MusicTrack {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    uint32_t durationSeconds = 0;
};

enum class MusicPlaybackState : uint8_t {
    Idle,
    Opening,
    Playing,
    Paused,
};

struct MusicPlayerContext {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    std::mutex tracksMutex;
    std::vector<MusicTrack> tracks;
    int32_t selectedTrackIndex = -1;

    std::mutex outputMutex;
    std::vector<std::string> outputNames;
    std::string selectedOutputName;

    std::mutex controlMutex;
    bool startPending = false;
    int32_t requestedTrackIndex = -1;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> pauseRequested{false};
    std::atomic<bool> shutdown{false};
    std::atomic<MusicPlaybackState> playbackState{MusicPlaybackState::Idle};
    std::atomic<int32_t> currentTrackIndex{-1};
    std::atomic<uint32_t> currentTimeSeconds{0};
    std::atomic<uint32_t> totalTimeSeconds{0};
    std::atomic<TaskHandle_t> task{nullptr};

    std::mutex statusMutex;
    std::string statusText = "Ready";

    std::string folderPath = "/sdcard/Music";
    uint8_t volumePercent = 80;
    bool updatingVolumeSlider = false;

    lv_obj_t* folderInput = nullptr;
    lv_obj_t* outputDropdown = nullptr;
    lv_obj_t* volumeSlider = nullptr;
    lv_obj_t* volumeLabel = nullptr;
    lv_obj_t* trackTitleLabel = nullptr;
    lv_obj_t* trackArtistLabel = nullptr;
    lv_obj_t* trackAlbumLabel = nullptr;
    lv_obj_t* playbackStatusLabel = nullptr;
    lv_obj_t* progressBar = nullptr;
    lv_obj_t* progressLabel = nullptr;
    lv_obj_t* playButtonLabel = nullptr;
    lv_obj_t* trackList = nullptr;
    lv_obj_t* trackCountLabel = nullptr;
    lv_timer_t* refreshTimer = nullptr;
};

void musicPlayerCreateWidgets(lv_obj_t* parent, void* userData);
void musicPlayerDestroyWidgets(void* userData);
void musicPlayerTaskEntry(void* param);
bool musicPlayerStartTask(MusicPlayerContext* context);
void musicPlayerScanFolder(MusicPlayerContext* context, const std::string& path);
void musicPlayerRequestStart(MusicPlayerContext* context, int32_t trackIndex);
void musicPlayerRequestStop(MusicPlayerContext* context);
