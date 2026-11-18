#include "MusicPlayer.h"

#include <app/event.h>

#include <lvgl/fonts.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/text_button.h>
#include <lvgl/widgets/toolbar.h>

#include <tactility/device.h>
#include <tactility/drivers/audio_codec.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/log.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace {

constexpr uint32_t UI_REFRESH_PERIOD_MS = 250;

struct OutputScan {
    std::vector<std::string> names;
};

void onClosePressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    app_event_emit_close(context->appInstanceId);
}

void setStatus(MusicPlayerContext* context, const std::string& status) {
    std::lock_guard lock(context->statusMutex);
    context->statusText = status;
}

bool scanOutputDevice(Device* device, void* userData) {
    auto* outputs = static_cast<OutputScan*>(userData);
    if (!device_is_ready(device) || device->name == nullptr) {
        return true;
    }

    bool outputSupported = false;
    if (audio_stream_is_supported(device, AUDIO_CODEC_DIR_OUTPUT, &outputSupported) == ERROR_NONE &&
        outputSupported) {
        outputs->names.emplace_back(device->name);
    }
    return true;
}

void refreshOutputDevices(MusicPlayerContext* context) {
    OutputScan scan;
    device_for_each_of_type(&AUDIO_STREAM_TYPE, &scan, scanOutputDevice);
    std::sort(scan.names.begin(), scan.names.end());

    std::lock_guard lock(context->outputMutex);
    context->outputNames = std::move(scan.names);
    if (std::find(context->outputNames.begin(), context->outputNames.end(), context->selectedOutputName) ==
        context->outputNames.end()) {
        context->selectedOutputName = context->outputNames.empty() ? "" : context->outputNames.front();
    }
}

void setVolumeSliderFromDevice(MusicPlayerContext* context) {
    if (context->volumeSlider == nullptr) {
        return;
    }

    std::string outputName;
    {
        std::lock_guard lock(context->outputMutex);
        outputName = context->selectedOutputName;
    }
    if (outputName.empty()) {
        return;
    }

    Device* device = nullptr;
    if (device_get_by_name(outputName.c_str(), &device) != ERROR_NONE || device == nullptr) {
        setStatus(context, "Audio output is unavailable");
        return;
    }
    float volume = 0.0f;
    const error_t error = audio_stream_get_volume(device, AUDIO_CODEC_DIR_OUTPUT, &volume);
    device_put(device);
    if (error != ERROR_NONE) {
        setStatus(context, "Volume control unavailable");
        return;
    }

    context->volumePercent = static_cast<uint8_t>(std::max(0.0f, std::min(volume, 100.0f)));
    context->updatingVolumeSlider = true;
    lv_slider_set_value(context->volumeSlider, context->volumePercent, LV_ANIM_OFF);
    context->updatingVolumeSlider = false;
    if (context->volumeLabel != nullptr) {
        lv_label_set_text_fmt(context->volumeLabel, "%u%%", context->volumePercent);
    }
}

void onOutputChanged(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const uint32_t selected = lv_dropdown_get_selected(context->outputDropdown);
    {
        std::lock_guard lock(context->outputMutex);
        if (selected < context->outputNames.size()) {
            context->selectedOutputName = context->outputNames[selected];
        } else {
            context->selectedOutputName.clear();
        }
    }
    setVolumeSliderFromDevice(context);
}

void onVolumeChanged(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    if (context->updatingVolumeSlider) {
        return;
    }
    context->volumePercent = static_cast<uint8_t>(lv_slider_get_value(context->volumeSlider));
    if (context->volumeLabel != nullptr) {
        lv_label_set_text_fmt(context->volumeLabel, "%u%%", context->volumePercent);
    }

    std::string outputName;
    {
        std::lock_guard lock(context->outputMutex);
        outputName = context->selectedOutputName;
    }
    if (outputName.empty()) {
        setStatus(context, "Select an audio output");
        return;
    }

    Device* device = nullptr;
    if (device_get_by_name(outputName.c_str(), &device) != ERROR_NONE || device == nullptr) {
        setStatus(context, "Audio output is unavailable");
        return;
    }
    const error_t error = audio_stream_set_volume(device, AUDIO_CODEC_DIR_OUTPUT, context->volumePercent);
    device_put(device);
    if (error != ERROR_NONE) {
        LOG_W(MUSIC_PLAYER_TAG, "Could not set output volume: %d", error);
        setStatus(context, "Volume control unavailable");
    }
}

bool copyTrack(MusicPlayerContext* context, int32_t index, MusicTrack* track) {
    std::lock_guard lock(context->tracksMutex);
    if (index < 0 || static_cast<size_t>(index) >= context->tracks.size()) {
        return false;
    }
    *track = context->tracks[static_cast<size_t>(index)];
    return true;
}

int32_t getSelectedTrackIndex(MusicPlayerContext* context) {
    std::lock_guard lock(context->tracksMutex);
    return context->selectedTrackIndex;
}

size_t getTrackCount(MusicPlayerContext* context) {
    std::lock_guard lock(context->tracksMutex);
    return context->tracks.size();
}

void formatTime(uint32_t seconds, char* output, size_t outputSize) {
    snprintf(output, outputSize, "%u:%02u", static_cast<unsigned>(seconds / 60),
             static_cast<unsigned>(seconds % 60));
}

void updateTrackInfo(MusicPlayerContext* context) {
    if (context->trackTitleLabel == nullptr) {
        return;
    }
    MusicTrack track;
    const int32_t index = getSelectedTrackIndex(context);
    if (!copyTrack(context, index, &track)) {
        lv_label_set_text(context->trackTitleLabel, "No track selected");
        lv_label_set_text(context->trackArtistLabel, "");
        lv_label_set_text(context->trackAlbumLabel, "");
        return;
    }
    lv_label_set_text(context->trackTitleLabel, track.title.c_str());
    lv_label_set_text(context->trackArtistLabel, track.artist.empty() ? "Unknown artist" : track.artist.c_str());
    lv_label_set_text(context->trackAlbumLabel, track.album.empty() ? " " : track.album.c_str());
}

void onTrackClicked(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    auto* row = lv_event_get_target_obj(event);
    auto* path = static_cast<std::string*>(lv_obj_get_user_data(row));
    if (path == nullptr) {
        return;
    }

    int32_t index = -1;
    {
        std::lock_guard lock(context->tracksMutex);
        for (size_t i = 0; i < context->tracks.size(); i++) {
            if (&context->tracks[i].path == path) {
                index = static_cast<int32_t>(i);
                context->selectedTrackIndex = index;
                break;
            }
        }
    }
    if (index >= 0) {
        const uint32_t childCount = lv_obj_get_child_count(context->trackList);
        for (uint32_t i = 0; i < childCount; i++) {
            lv_obj_remove_state(lv_obj_get_child(context->trackList, static_cast<int32_t>(i)), LV_STATE_CHECKED);
        }
        lv_obj_add_state(row, LV_STATE_CHECKED);
        updateTrackInfo(context);
        musicPlayerRequestStart(context, index);
    }
}

void renderTrackList(MusicPlayerContext* context) {
    if (context->trackList == nullptr) {
        return;
    }
    lv_obj_clean(context->trackList);

    std::lock_guard lock(context->tracksMutex);
    if (context->trackCountLabel != nullptr) {
        lv_label_set_text_fmt(context->trackCountLabel, "%u", static_cast<unsigned>(context->tracks.size()));
    }
    if (context->tracks.empty()) {
        auto* emptyLabel = lv_label_create(context->trackList);
        lv_label_set_text(emptyLabel, "No MP3 files in this folder");
        return;
    }

    for (size_t i = 0; i < context->tracks.size(); i++) {
        const MusicTrack& track = context->tracks[i];
        auto* row = lv_button_create(context->trackList);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_user_data(row, &context->tracks[i].path);
        lv_obj_add_event_cb(row, onTrackClicked, LV_EVENT_CLICKED, context);

        auto* label = lv_label_create(row);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        if (track.artist.empty()) {
            lv_label_set_text(label, track.title.c_str());
        } else {
            lv_label_set_text_fmt(label, "%s - %s", track.title.c_str(), track.artist.c_str());
        }
        if (static_cast<int32_t>(i) == context->selectedTrackIndex) {
            lv_obj_add_state(row, LV_STATE_CHECKED);
        }
    }
}

void onScanPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const char* text = lv_textarea_get_text(context->folderInput);
    musicPlayerRequestStop(context);
    musicPlayerScanFolder(context, text == nullptr ? "" : text);
    renderTrackList(context);
    updateTrackInfo(context);
}

void selectAndPlay(MusicPlayerContext* context, int32_t index) {
    const size_t count = getTrackCount(context);
    if (count == 0) {
        return;
    }
    index = (index + static_cast<int32_t>(count)) % static_cast<int32_t>(count);
    {
        std::lock_guard lock(context->tracksMutex);
        context->selectedTrackIndex = index;
    }
    updateTrackInfo(context);
    renderTrackList(context);
    musicPlayerRequestStart(context, index);
}

void onPlayPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const MusicPlaybackState state = context->playbackState.load();
    if (state == MusicPlaybackState::Playing || state == MusicPlaybackState::Opening) {
        context->pauseRequested.store(true);
    } else if (state == MusicPlaybackState::Paused) {
        context->pauseRequested.store(false);
    } else {
        musicPlayerRequestStart(context, getSelectedTrackIndex(context));
    }
}

void onStopPressed(lv_event_t* event) {
    musicPlayerRequestStop(static_cast<MusicPlayerContext*>(lv_event_get_user_data(event)));
}

void onPreviousPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const int32_t selected = getSelectedTrackIndex(context);
    if (selected >= 0) {
        selectAndPlay(context, selected - 1);
    }
}

void onNextPressed(lv_event_t* event) {
    auto* context = static_cast<MusicPlayerContext*>(lv_event_get_user_data(event));
    const int32_t selected = getSelectedTrackIndex(context);
    if (selected >= 0) {
        selectAndPlay(context, selected + 1);
    }
}

lv_obj_t* createCard(lv_obj_t* parent) {
    auto* card = lvgl_card_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    return card;
}

lv_obj_t* createRow(lv_obj_t* parent) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, LV_DPX(8), LV_STATE_DEFAULT);
    lv_obj_set_scrollable(row, false);
    return row;
}

void setButtonText(lv_obj_t* button, const char* text) {
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
}

void createWidgetsImpl(lv_obj_t* parent, MusicPlayerContext* context) {
    const lv_coord_t padding = lvgl_get_ui_density() == LVGL_UI_DENSITY_COMPACT ? 6 : 10;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(parent, false);
    auto* toolbar = lvgl_toolbar_create(parent, "Music Player");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onClosePressed, context);

    auto* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    auto* folderCard = createCard(content);
    auto* folderTitle = lv_label_create(folderCard);
    lv_label_set_text(folderTitle, "SD card folder");
    auto* folderRow = createRow(folderCard);
    context->folderInput = lv_textarea_create(folderRow);
    lv_textarea_set_one_line(context->folderInput, true);
    lv_textarea_set_text(context->folderInput, context->folderPath.c_str());
    lv_obj_set_flex_grow(context->folderInput, 1);
    auto* scanButton = lvgl_text_button_create(folderRow);
    setButtonText(scanButton, "Scan");
    lv_obj_add_event_cb(scanButton, onScanPressed, LV_EVENT_CLICKED, context);

    auto* outputCard = createCard(content);
    auto* outputTitle = lv_label_create(outputCard);
    lv_label_set_text(outputTitle, "Audio output");
    context->outputDropdown = lv_dropdown_create(outputCard);
    lv_obj_set_width(context->outputDropdown, LV_PCT(100));

    std::string outputOptions;
    uint32_t selectedOutputIndex = 0;
    {
        std::lock_guard lock(context->outputMutex);
        for (size_t i = 0; i < context->outputNames.size(); i++) {
            if (i > 0) {
                outputOptions.push_back('\n');
            }
            outputOptions += context->outputNames[i];
            if (context->outputNames[i] == context->selectedOutputName) {
                selectedOutputIndex = static_cast<uint32_t>(i);
            }
        }
    }
    lv_dropdown_set_options(context->outputDropdown, outputOptions.empty() ? "No output device" : outputOptions.c_str());
    lv_dropdown_set_selected(context->outputDropdown, selectedOutputIndex);
    lv_obj_set_state(context->outputDropdown, LV_STATE_DISABLED, outputOptions.empty());
    lv_obj_add_event_cb(context->outputDropdown, onOutputChanged, LV_EVENT_VALUE_CHANGED, context);

    auto* volumeRow = createRow(outputCard);
    auto* volumeTitle = lv_label_create(volumeRow);
    lv_label_set_text(volumeTitle, "Volume");
    context->volumeSlider = lv_slider_create(volumeRow);
    lv_slider_set_range(context->volumeSlider, 0, 100);
    lv_slider_set_value(context->volumeSlider, context->volumePercent, LV_ANIM_OFF);
    lv_obj_set_flex_grow(context->volumeSlider, 1);
    lv_obj_add_event_cb(context->volumeSlider, onVolumeChanged, LV_EVENT_VALUE_CHANGED, context);
    context->volumeLabel = lv_label_create(volumeRow);
    lv_obj_set_width(context->volumeLabel, LV_SIZE_CONTENT);
    lv_label_set_text_fmt(context->volumeLabel, "%u%%", context->volumePercent);

    auto* playerCard = createCard(content);
    context->trackTitleLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->trackTitleLabel, LV_PCT(100));
    lv_obj_set_style_text_font(context->trackTitleLabel, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);
    lv_label_set_long_mode(context->trackTitleLabel, LV_LABEL_LONG_MODE_DOTS);
    context->trackArtistLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->trackArtistLabel, LV_PCT(100));
    context->trackAlbumLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->trackAlbumLabel, LV_PCT(100));

    context->progressBar = lv_bar_create(playerCard);
    lv_obj_set_width(context->progressBar, LV_PCT(100));
    lv_bar_set_range(context->progressBar, 0, 100);
    lv_bar_set_value(context->progressBar, 0, LV_ANIM_OFF);
    context->progressLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->progressLabel, LV_PCT(100));
    lv_label_set_text(context->progressLabel, "0:00 / --:--");
    context->playbackStatusLabel = lv_label_create(playerCard);
    lv_obj_set_width(context->playbackStatusLabel, LV_PCT(100));

    auto* controls = createRow(playerCard);
    auto* previousButton = lv_button_create(controls);
    setButtonText(previousButton, LV_SYMBOL_PREV);
    lv_obj_add_event_cb(previousButton, onPreviousPressed, LV_EVENT_CLICKED, context);
    auto* playButton = lv_button_create(controls);
    context->playButtonLabel = lv_label_create(playButton);
    lv_label_set_text(context->playButtonLabel, LV_SYMBOL_PLAY);
    lv_obj_center(context->playButtonLabel);
    lv_obj_add_event_cb(playButton, onPlayPressed, LV_EVENT_CLICKED, context);
    auto* stopButton = lv_button_create(controls);
    setButtonText(stopButton, LV_SYMBOL_STOP);
    lv_obj_add_event_cb(stopButton, onStopPressed, LV_EVENT_CLICKED, context);
    auto* nextButton = lv_button_create(controls);
    setButtonText(nextButton, LV_SYMBOL_NEXT);
    lv_obj_add_event_cb(nextButton, onNextPressed, LV_EVENT_CLICKED, context);

    auto* listCard = createCard(content);
    auto* listHeader = createRow(listCard);
    auto* listTitle = lv_label_create(listHeader);
    lv_label_set_text(listTitle, "Tracks");
    context->trackCountLabel = lv_label_create(listHeader);
    lv_obj_set_flex_grow(context->trackCountLabel, 1);
    lv_obj_set_style_text_align(context->trackCountLabel, LV_TEXT_ALIGN_RIGHT, LV_STATE_DEFAULT);
    context->trackList = lv_obj_create(listCard);
    lv_obj_remove_style_all(context->trackList);
    lv_obj_set_width(context->trackList, LV_PCT(100));
    lv_obj_set_height(context->trackList, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(context->trackList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(context->trackList, LV_DPX(4), LV_STATE_DEFAULT);
    lv_obj_set_scrollable(context->trackList, false);

    updateTrackInfo(context);
    renderTrackList(context);
    setVolumeSliderFromDevice(context);
}

void onRefreshTimer(lv_timer_t* timer) {
    auto* context = static_cast<MusicPlayerContext*>(lv_timer_get_user_data(timer));
    if (context == nullptr) {
        return;
    }

    const MusicPlaybackState state = context->playbackState.load();
    const int32_t currentTrack = context->currentTrackIndex.load();
    if (state != MusicPlaybackState::Idle && currentTrack >= 0 && currentTrack != getSelectedTrackIndex(context)) {
        {
            std::lock_guard lock(context->tracksMutex);
            if (static_cast<size_t>(currentTrack) < context->tracks.size()) {
                context->selectedTrackIndex = currentTrack;
            }
        }
        updateTrackInfo(context);
        renderTrackList(context);
    }

    if (context->playButtonLabel != nullptr) {
        if (state == MusicPlaybackState::Playing || state == MusicPlaybackState::Opening) {
            lv_label_set_text(context->playButtonLabel, LV_SYMBOL_PAUSE);
        } else {
            lv_label_set_text(context->playButtonLabel, LV_SYMBOL_PLAY);
        }
    }

    const uint32_t current = context->currentTimeSeconds.load();
    const uint32_t total = context->totalTimeSeconds.load();
    if (context->progressLabel != nullptr) {
        char currentText[16];
        char totalText[16];
        char text[48];
        formatTime(current, currentText, sizeof(currentText));
        if (total == 0) {
            snprintf(totalText, sizeof(totalText), "--:--");
        } else {
            formatTime(total, totalText, sizeof(totalText));
        }
        snprintf(text, sizeof(text), "%s / %s", currentText, totalText);
        lv_label_set_text(context->progressLabel, text);
    }
    if (context->progressBar != nullptr) {
        const int32_t percent = total == 0 ? 0 : std::min<int32_t>(100, static_cast<int32_t>(current * 100 / total));
        lv_bar_set_value(context->progressBar, percent, LV_ANIM_OFF);
    }

    if (context->playbackStatusLabel != nullptr) {
        std::string status;
        {
            std::lock_guard lock(context->statusMutex);
            status = context->statusText;
        }
        if (state == MusicPlaybackState::Paused) {
            status = "Paused";
        } else if (state == MusicPlaybackState::Playing) {
            status = "Playing";
        } else if (state == MusicPlaybackState::Opening) {
            status = "Opening track";
        }
        lv_label_set_text(context->playbackStatusLabel, status.c_str());
    }
    if (context->outputDropdown != nullptr) {
        const bool active = state == MusicPlaybackState::Opening || state == MusicPlaybackState::Playing ||
                            state == MusicPlaybackState::Paused;
        bool noOutput = false;
        {
            std::lock_guard lock(context->outputMutex);
            noOutput = context->selectedOutputName.empty();
        }
        lv_obj_set_state(context->outputDropdown, LV_STATE_DISABLED, active || noOutput);
    }
}

void clearWidgetPointers(MusicPlayerContext* context) {
    context->folderInput = nullptr;
    context->outputDropdown = nullptr;
    context->volumeSlider = nullptr;
    context->volumeLabel = nullptr;
    context->trackTitleLabel = nullptr;
    context->trackArtistLabel = nullptr;
    context->trackAlbumLabel = nullptr;
    context->playbackStatusLabel = nullptr;
    context->progressBar = nullptr;
    context->progressLabel = nullptr;
    context->playButtonLabel = nullptr;
    context->trackList = nullptr;
    context->trackCountLabel = nullptr;
}

} // namespace

void musicPlayerCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* context = static_cast<MusicPlayerContext*>(userData);
    if (context->refreshTimer != nullptr) {
        lv_timer_delete(context->refreshTimer);
        context->refreshTimer = nullptr;
    }
    clearWidgetPointers(context);
    refreshOutputDevices(context);
    createWidgetsImpl(parent, context);
    context->refreshTimer = lv_timer_create(onRefreshTimer, UI_REFRESH_PERIOD_MS, context);
    onRefreshTimer(context->refreshTimer);
}

void musicPlayerDestroyWidgets(void* userData) {
    auto* context = static_cast<MusicPlayerContext*>(userData);
    if (context->refreshTimer != nullptr) {
        lv_timer_delete(context->refreshTimer);
        context->refreshTimer = nullptr;
    }
    clearWidgetPointers(context);
}
