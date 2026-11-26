#include "AudioRecorder.h"

#include <lvgl_window_manager/dialog.h>
#include <lvgl_window_manager/window_manager.h>
#include <app/paths.h>
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/list.h>
#include <lvgl/widgets/text_button.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/log.h>

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

void formatTimeMmSs(uint32_t sec, char* buf, size_t bufLen) {
    snprintf(buf, bufLen, "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

void formatDuration(uint32_t sec, char* buf, size_t bufLen) {
    snprintf(buf, bufLen, "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

void formatSize(size_t bytes, char* buf, size_t bufLen) {
    if (bytes < 1024 * 1024) {
        snprintf(buf, bufLen, "%.1f KB", (float)bytes / 1024.0F);
    } else {
        snprintf(buf, bufLen, "%.2f MB", (float)bytes / (1024.0F * 1024.0F));
    }
}

/** Duration of a recording, derived from its file size (44-byte WAV header + 32 kB/s PCM) */
uint32_t getRecordingSeconds(const RecordingItem& item) {
    if (item.sizeBytes <= RECORDER_WAV_HEADER_BYTES) {
        return 0;
    }
    return (uint32_t)((item.sizeBytes - RECORDER_WAV_HEADER_BYTES) / RECORDER_BYTES_PER_SECOND);
}

/** A filename without its extension: the extension is always .wav and only clutters the row */
std::string stripExtension(const std::string& filename) {
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return filename;
    }
    return filename.substr(0, dot);
}

std::string getDisplayName(const RecordingItem& item) {
    return stripExtension(item.filename);
}

/** "1:23 - 2.6 MB - 16 Nov 15:56". ASCII only: the default theme font has no middle dot. */
void formatItemMeta(const RecordingItem& item, char* buf, size_t bufLen) {
    char duration[16];
    char size[16];
    formatDuration(getRecordingSeconds(item), duration, sizeof(duration));
    formatSize(item.sizeBytes, size, sizeof(size));

    char date[32] = "";
    struct tm tmInfo {};
    localtime_r(&item.mtime, &tmInfo);
    if (strftime(date, sizeof(date), "%d %b %H:%M", &tmInfo) == 0) {
        date[0] = '\0';
    }

    snprintf(buf, bufLen, "%s - %s - %s", duration, size, date);
}

void formatRecorderLimit(char* buf, size_t bufLen) {
    char maxTime[16];
    formatDuration(RECORDER_MAX_SECONDS, maxTime, sizeof(maxTime));
    snprintf(buf, bufLen, "up to %s", maxTime);
}

uint32_t getSelectedDurationSeconds(Context* ctx) {
    std::lock_guard lg(ctx->recordingsMutex);
    for (const auto& item : ctx->recordings) {
        if (item.fullpath == ctx->selectedPath) {
            return getRecordingSeconds(item);
        }
    }
    return 0;
}

/** The theme's error color, which a Material theme lightens in dark mode so it stays readable */
lv_color_t getRecordingAccentColor() {
    LvglThemeSettings settings {};
    lvgl_theme_get_settings(&settings);
    return settings.is_dark ? lv_color_mix(settings.color_error, lv_color_white(), 140) : settings.color_error;
}

lv_coord_t getCardGap() {
    return (lv_coord_t)lvgl_theme_lerp(4, 12, lvgl_theme_get_spacing());
}

/** Matches the theme's list-item padding, so an expanded panel lines up with the row above it */
lv_coord_t getListItemPadHor() {
    return (lv_coord_t)lvgl_theme_lerp(LV_DPX(2), LV_DPX(16), lvgl_theme_get_spacing());
}

lv_coord_t getListItemPadVer() {
    return (lv_coord_t)lvgl_theme_lerp(LV_DPX(2), LV_DPX(12), lvgl_theme_get_spacing());
}

/** A transparent row/column wrapper, so the card's own background and padding show through */
lv_obj_t* createWrapper(lv_obj_t* parent, lv_flex_flow_t flow) {
    auto* wrapper = lv_obj_create(parent);
    lv_obj_remove_style_all(wrapper);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrapper, flow);
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(wrapper, getCardGap(), 0);
    lv_obj_set_scrollable(wrapper, false);
    return wrapper;
}

lv_obj_t* createRow(lv_obj_t* parent) {
    return createWrapper(parent, LV_FLEX_FLOW_ROW);
}

lv_obj_t* createCard(lv_obj_t* parent) {
    auto* card = lvgl_card_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    return card;
}

bool hasSelection(Context* ctx) {
    std::lock_guard lg(ctx->recordingsMutex);
    return !ctx->selectedPath.empty();
}

void updateToolbarState(Context* ctx) {
    if (ctx->delete_btn != nullptr) {
        lv_obj_set_state(ctx->delete_btn, LV_STATE_DISABLED, !hasSelection(ctx));
    }
}

void syncStateUi(Context* ctx);

void refreshBtnCb(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    // Rescans without dropping the selection; recorderRenderList() only clears a
    // selection whose file is gone.
    recorderScanDirectory(ctx);
    recorderRenderList(ctx);
}

void recActionBtnCb(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    RecorderState state = ctx->audio.state.load();

    if (state == RecorderState::Recording) {
        ctx->audio.stopRequested.store(true);
    } else {
        if (ctx->appDataDir.empty()) {
            LOG_E(TAG, "Refusing to record: no recordings directory available");
            return;
        }
        if (state == RecorderState::Playing || state == RecorderState::Paused) {
            ctx->audio.pauseRequested.store(false);
            ctx->audio.stopRequested.store(true);
        }
        // pending persists until the audio task's loop reads it, so this is safe to
        // set immediately even while playback is still winding down - no busy-wait
        // needed on the LVGL thread.
        ctx->audio.pending.store(RecorderRequest::StartRecording);
    }
}

void playToggleBtnCb(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    RecorderState state = ctx->audio.state.load();

    if (state == RecorderState::Playing) {
        ctx->audio.pauseRequested.store(true);
    } else if (state == RecorderState::Paused) {
        ctx->audio.pauseRequested.store(false);
    } else {
        std::string path;
        {
            std::lock_guard lg(ctx->audio.pathMutex);
            path = ctx->audio.requestedPlaybackPath;
        }
        if (!path.empty()) {
            ctx->audio.pending.store(RecorderRequest::StartPlayback);
        }
    }
}

void playStopBtnCb(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->audio.stopRequested.store(true);
}

void deleteBtnCb(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    std::string selected;
    {
        std::lock_guard lg(ctx->recordingsMutex);
        selected = ctx->selectedPath;
    }
    if (!selected.empty()) {
        recorderRequestDelete(ctx, selected);
    }
}

void rowClickedCb(lv_event_t* event) {
    auto* row = lv_event_get_target_obj(event);
    auto* pathStr = static_cast<std::string*>(lv_obj_get_user_data(row));
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (pathStr == nullptr) {
        return;
    }

    recorderSelectRecording(ctx, *pathStr);

    // Show the selection/highlight right away instead of waiting for the poll timer's
    // rebuild. The rebuild is still deferred, as cleaning a row from inside its own
    // click event is unsafe (see recorderSelectRecording).
    if (ctx->list_container != nullptr) {
        const uint32_t count = lv_obj_get_child_count(ctx->list_container);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_remove_state(lv_obj_get_child(ctx->list_container, (int32_t)i), LV_STATE_CHECKED);
        }
    }
    lv_obj_add_state(row, LV_STATE_CHECKED);
    updateToolbarState(ctx);
}

void clearWidgetPointers(Context* ctx) {
    ctx->rec_card = nullptr;
    ctx->rec_status_label = nullptr;
    ctx->rec_limit_label = nullptr;
    ctx->rec_time_label = nullptr;
    ctx->rec_action_btn = nullptr;
    ctx->rec_action_label = nullptr;

    ctx->player_panel = nullptr;
    ctx->play_bar = nullptr;
    ctx->play_time_label = nullptr;
    ctx->play_toggle_btn = nullptr;
    ctx->play_toggle_label = nullptr;
    ctx->play_stop_btn = nullptr;

    ctx->list_card = nullptr;
    ctx->list_title_label = nullptr;
    ctx->list_count_label = nullptr;
    ctx->list_container = nullptr;
    ctx->empty_container = nullptr;
}

void clearListRows(Context* ctx) {
    if (ctx->list_container == nullptr) {
        return;
    }
    const uint32_t count = lv_obj_get_child_count(ctx->list_container);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* row = lv_obj_get_child(ctx->list_container, (int32_t)i);
        delete static_cast<std::string*>(lv_obj_get_user_data(row));
        lv_obj_set_user_data(row, nullptr);
    }
}

void buildRecordCard(Context* ctx, lv_obj_t* parent) {
    ctx->rec_card = createCard(parent);

    auto* statusRow = createRow(ctx->rec_card);

    ctx->rec_status_label = lv_label_create(statusRow);
    lv_obj_set_style_text_font(ctx->rec_status_label, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(ctx->rec_status_label, LV_OPA_70, 0);
    lv_obj_set_flex_grow(ctx->rec_status_label, 1);
    lv_label_set_text(ctx->rec_status_label, "Ready to record");

    ctx->rec_limit_label = lv_label_create(statusRow);
    lv_obj_set_style_text_font(ctx->rec_limit_label, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(ctx->rec_limit_label, LV_OPA_70, 0);
    char limit[32];
    formatRecorderLimit(limit, sizeof(limit));
    lv_label_set_text(ctx->rec_limit_label, limit);

    ctx->rec_time_label = lv_label_create(ctx->rec_card);
    lv_obj_set_width(ctx->rec_time_label, LV_PCT(100));
    lv_obj_set_style_text_font(ctx->rec_time_label, lvgl_get_text_font(FONT_SIZE_LARGE), 0);
    lv_obj_set_style_text_align(ctx->rec_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ctx->rec_time_label, "00:00");

    // A filled button: the theme decides its color, shape, padding and pressed/focus states
    ctx->rec_action_btn = lv_button_create(ctx->rec_card);
    lv_obj_set_size(ctx->rec_action_btn, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_add_event_cb(ctx->rec_action_btn, recActionBtnCb, LV_EVENT_CLICKED, ctx);

    ctx->rec_action_label = lv_label_create(ctx->rec_action_btn);
    lv_obj_set_style_text_font(ctx->rec_action_label, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_label_set_text(ctx->rec_action_label, LV_SYMBOL_AUDIO "  Record");
    lv_obj_center(ctx->rec_action_label);
}

/**
 * Adds the expandable player controls directly below a list row, so a recording's
 * controls always appear where the user tapped, also at the bottom of a long list.
 */
void addPlayerPanel(Context* ctx, lv_obj_t* list, const RecordingItem& item) {
    auto* panel = lv_obj_create(list);
    lv_obj_remove_style_all(panel);
    lv_obj_set_clickable(panel, false);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(panel, getListItemPadVer(), 0);
    lv_obj_set_style_pad_left(panel, getListItemPadHor(), 0);
    lv_obj_set_style_pad_right(panel, getListItemPadHor(), 0);
    lv_obj_set_style_pad_gap(panel, getCardGap(), 0);
    lv_obj_set_scrollable(panel, false);

    ctx->player_panel = panel;

    ctx->play_bar = lv_bar_create(panel);
    lv_obj_set_width(ctx->play_bar, LV_PCT(100));
    lv_bar_set_range(ctx->play_bar, 0, 100);
    lv_bar_set_value(ctx->play_bar, 0, LV_ANIM_OFF);

    auto* controls = createRow(panel);

    ctx->play_time_label = lv_label_create(controls);
    lv_obj_set_style_text_font(ctx->play_time_label, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_height(ctx->play_time_label, lv_font_get_line_height(lvgl_get_text_font(FONT_SIZE_SMALL)));
    lv_obj_set_style_text_opa(ctx->play_time_label, LV_OPA_70, 0);
    lv_obj_set_flex_grow(ctx->play_time_label, 1);
    {
        char zero[16];
        char total[16];
        char text[48];
        formatTimeMmSs(0, zero, sizeof(zero));
        formatTimeMmSs(getRecordingSeconds(item), total, sizeof(total));
        snprintf(text, sizeof(text), "%s / %s", zero, total);
        lv_label_set_text(ctx->play_time_label, text);
    }

    ctx->play_toggle_btn = lv_button_create(controls);
    lv_obj_set_size(ctx->play_toggle_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_event_cb(ctx->play_toggle_btn, playToggleBtnCb, LV_EVENT_CLICKED, ctx);

    ctx->play_toggle_label = lv_label_create(ctx->play_toggle_btn);
    lv_obj_set_style_text_font(ctx->play_toggle_label, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_label_set_text(ctx->play_toggle_label, LV_SYMBOL_PLAY "  Play");
    lv_obj_center(ctx->play_toggle_label);

    // A low-emphasis action: the theme styles it without a container
    ctx->play_stop_btn = lvgl_text_button_create(controls);
    lv_obj_set_size(ctx->play_stop_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_event_cb(ctx->play_stop_btn, playStopBtnCb, LV_EVENT_CLICKED, ctx);

    lv_obj_t* stop_label = lv_label_create(ctx->play_stop_btn);
    lv_obj_set_style_text_font(stop_label, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_label_set_text(stop_label, LV_SYMBOL_STOP "  Stop");
    lv_obj_center(stop_label);
}

void buildListCard(Context* ctx, lv_obj_t* parent) {
    ctx->list_card = createCard(parent);

    auto* header = createRow(ctx->list_card);

    ctx->list_title_label = lv_label_create(header);
    lv_obj_set_style_text_font(ctx->list_title_label, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_obj_set_flex_grow(ctx->list_title_label, 1);
    lv_label_set_text(ctx->list_title_label, "Recordings");

    ctx->list_count_label = lv_label_create(header);
    lv_obj_set_style_text_font(ctx->list_count_label, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(ctx->list_count_label, LV_OPA_70, 0);
    lv_label_set_text(ctx->list_count_label, "0");

    ctx->list_container = lvgl_list_create(ctx->list_card);
    lv_obj_set_width(ctx->list_container, LV_PCT(100));
    // The page scrolls instead of the list, so the list grows with its rows
    lv_obj_set_height(ctx->list_container, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(ctx->list_container, false);

    ctx->empty_container = lv_obj_create(ctx->list_card);
    lv_obj_remove_style_all(ctx->empty_container);
    lv_obj_set_width(ctx->empty_container, LV_PCT(100));
    lv_obj_set_height(ctx->empty_container, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->empty_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctx->empty_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ctx->empty_container, getCardGap(), 0);
    lv_obj_set_style_pad_gap(ctx->empty_container, getCardGap(), 0);
    lv_obj_set_scrollable(ctx->empty_container, false);

    lv_obj_t* empty_icon = lv_label_create(ctx->empty_container);
    lv_obj_set_style_text_font(empty_icon, lvgl_get_text_font(FONT_SIZE_LARGE), 0);
    lv_obj_set_style_text_color(empty_icon, lv_theme_get_color_primary(empty_icon), 0);
    lv_label_set_text(empty_icon, LV_SYMBOL_AUDIO);

    lv_obj_t* empty_title = lv_label_create(ctx->empty_container);
    lv_obj_set_width(empty_title, LV_PCT(100));
    lv_obj_set_style_text_font(empty_title, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_obj_set_style_text_align(empty_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(empty_title, "No recordings yet");

    lv_obj_t* empty_hint = lv_label_create(ctx->empty_container);
    lv_obj_set_width(empty_hint, LV_PCT(100));
    lv_obj_set_style_text_font(empty_hint, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(empty_hint, LV_OPA_70, 0);
    lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(empty_hint, "Tap Record to make your first one");

    lv_obj_set_hidden(ctx->empty_container, true);
}

void rebuildContent(Context* ctx) {
    if (ctx->main_container == nullptr) {
        return;
    }

    // The rows own heap strings that lv_obj_clean() wouldn't free
    clearListRows(ctx);
    lv_obj_clean(ctx->main_container);
    clearWidgetPointers(ctx);

    lv_obj_set_flex_flow(ctx->main_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctx->main_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(ctx->main_container, true);

    buildRecordCard(ctx, ctx->main_container);
    buildListCard(ctx, ctx->main_container);

    recorderRenderList(ctx);
    ctx->lastRenderedState = ctx->audio.state.load();
    ctx->lastRenderedSecond = ~0u;
    ctx->lastRenderedTotal = ~0u;
}

/** Scrolls the page so the expanded player is visible */
void scrollPlayerPanelIntoView(Context* ctx) {
    if (ctx->player_panel == nullptr || ctx->list_container == nullptr) {
        return;
    }
    const int32_t panelInCard = lv_obj_get_y(ctx->list_container) + lv_obj_get_y(ctx->player_panel);
    const int32_t panelInPage = lv_obj_get_y(ctx->list_card) + panelInCard;
    const int32_t target = panelInPage + lv_obj_get_height(ctx->player_panel) -
                           lv_obj_get_content_height(ctx->main_container);
    lv_obj_scroll_to_y(ctx->main_container, target, LV_ANIM_ON);
}

/** Updates the inline player's progress bar and time from the current playback values */
void updatePlaybackProgress(Context* ctx, uint32_t curSec, uint32_t totSec) {
    if (ctx->play_time_label != nullptr) {
        char curTime[16];
        char totTime[16];
        char text[48];
        formatTimeMmSs(curSec, curTime, sizeof(curTime));
        formatTimeMmSs(totSec, totTime, sizeof(totTime));
        snprintf(text, sizeof(text), "%s / %s", curTime, totTime);
        lv_label_set_text(ctx->play_time_label, text);
    }
    if (ctx->play_bar != nullptr) {
        int32_t percent = (totSec > 0) ? (int32_t)(curSec * 100 / totSec) : 0;
        percent = std::min<int32_t>(percent, 100);
        lv_bar_set_value(ctx->play_bar, percent, LV_ANIM_OFF);
    }
}

/** Applies everything that only changes with the recorder's state, not with its timer */
void syncStateUi(Context* ctx) {
    const RecorderState state = ctx->audio.state.load();
    const bool recording = state == RecorderState::Recording;
    const bool playing = state == RecorderState::Playing || state == RecorderState::Paused;

    if (ctx->rec_action_btn != nullptr) {
        lv_obj_set_state(ctx->rec_action_btn, LV_STATE_CHECKED, recording);
    }
    if (ctx->rec_action_label != nullptr) {
        lv_label_set_text(ctx->rec_action_label, recording ? LV_SYMBOL_STOP "  Stop and Save" : LV_SYMBOL_AUDIO "  Record");
    }
    if (ctx->rec_status_label != nullptr) {
        lv_label_set_text(ctx->rec_status_label, recording ? "Recording" : "Ready to record");
        if (recording && !lvgl_theme_is_mono()) {
            lv_obj_set_style_text_color(ctx->rec_status_label, getRecordingAccentColor(), 0);
            lv_obj_set_style_text_opa(ctx->rec_status_label, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_text_color(ctx->rec_status_label, lv_obj_get_style_text_color(ctx->rec_card, LV_PART_MAIN), 0);
            lv_obj_set_style_text_opa(ctx->rec_status_label, LV_OPA_70, 0);
        }
    }
    if (ctx->rec_time_label != nullptr) {
        char time[16];
        formatTimeMmSs(recording ? ctx->audio.currentTimeSec.load() : 0, time, sizeof(time));
        lv_label_set_text(ctx->rec_time_label, time);
    }
    if (ctx->play_toggle_label != nullptr) {
        lv_label_set_text(ctx->play_toggle_label, state == RecorderState::Playing ? LV_SYMBOL_PAUSE "  Pause" : LV_SYMBOL_PLAY "  Play");
    }
    if (ctx->play_stop_btn != nullptr) {
        lv_obj_set_state(ctx->play_stop_btn, LV_STATE_DISABLED, !playing);
    }
    if (!playing) {
        if (ctx->play_bar != nullptr) {
            lv_bar_set_value(ctx->play_bar, 0, LV_ANIM_OFF);
        }
        if (ctx->play_time_label != nullptr) {
            char zero[16];
            char total[16];
            char text[48];
            formatTimeMmSs(0, zero, sizeof(zero));
            formatTimeMmSs(getSelectedDurationSeconds(ctx), total, sizeof(total));
            snprintf(text, sizeof(text), "%s / %s", zero, total);
            lv_label_set_text(ctx->play_time_label, text);
        }
    }

    updateToolbarState(ctx);
}

} // namespace

void recorderFreeRowData(Context* ctx) {
    clearListRows(ctx);
    // The widget tree is about to be deleted by the window manager
    ctx->list_container = nullptr;
    ctx->empty_container = nullptr;
}

void recorderSelectRecording(Context* ctx, const std::string& fullpath) {
    {
        std::lock_guard lg(ctx->recordingsMutex);
        ctx->selectedPath = fullpath;
    }
    {
        std::lock_guard lg(ctx->audio.pathMutex);
        ctx->audio.requestedPlaybackPath = fullpath;
    }

    // Picking another file while one is playing stops that playback, so the player
    // always shows what is (or is about to be) heard.
    const RecorderState state = ctx->audio.state.load();
    if (state == RecorderState::Playing || state == RecorderState::Paused) {
        std::string active;
        {
            std::lock_guard lg(ctx->audio.pathMutex);
            active = ctx->audio.activeFilepath;
        }
        if (active != fullpath) {
            ctx->audio.stopRequested.store(true);
            ctx->audio.pauseRequested.store(false);
        }
    }

    // Deferred to the poll timer rather than rebuilding the list here: this runs
    // from rowClickedCb, an LVGL click-event handler, and lv_obj_clean()'ing the
    // row currently dispatching its own click event is unsafe.
    ctx->listDirty.store(true);
}

void recorderRequestDelete(Context* ctx, const std::string& fullpath) {
    if (!ctx->pendingDeletePath.empty()) {
        return; // a delete confirmation is already pending
    }

    const char* slash = strrchr(fullpath.c_str(), '/');
    const char* name = slash ? (slash + 1) : fullpath.c_str();
    const std::string displayName = stripExtension(name);

    char message[192];
    snprintf(message, sizeof(message), "Delete \"%s\"?", displayName.c_str());

    ctx->pendingDeletePath = fullpath;

    static const char* const buttons[] = { "Delete", "Cancel" };
    const error_t err = window_manager_alert_dialog(ctx->appInstanceId, "Delete Recording?", message, buttons, 2,
        [](int32_t button, void* userData) {
            // The file is deleted on the app task
            auto* context = static_cast<Context*>(userData);
            context->deleteConfirmed = button == 0;
            task_event_group_signal(context->eventGroup, context->deleteBit);
        }, ctx);
    if (err != ERROR_NONE) {
        LOG_E(TAG, "Failed to show the delete dialog: %d", err);
        ctx->pendingDeletePath.clear();
    }
}

void recorderInitAppData(Context* ctx) {
    // Only runs once per app instance: audiorecorderCreateWidgets() reruns on every
    // resurface (e.g. after the delete-confirmation dialog closes), but session
    // state - the audio task's request queue, the recordings directory - must survive
    // that, not get reset out from under an in-flight request.
    if (ctx->backendStarted) {
        return;
    }

    ctx->audio.shutdown.store(false);
    ctx->audio.pending.store(RecorderRequest::None);

    char dir[192] = {0};
    if (app_paths_get_user_data_directory(APP_ID, dir, sizeof(dir)) != ERROR_NONE) {
        LOG_E(TAG, "app_paths_get_user_data_directory failed");
        return; // appDataDir stays empty; recActionBtnCb refuses to record without it
    }
    ctx->appDataDir = dir;
    mkdir(ctx->appDataDir.c_str(), 0755);

    std::lock_guard lg(ctx->audio.pathMutex);
    ctx->audio.recordDir = ctx->appDataDir;
}

void recorderEnsureBackendStarted(Context* ctx) {
    if (ctx->backendStarted) {
        return;
    }
    ctx->backendStarted = true;

    TaskHandle_t handle = nullptr;
    BaseType_t ok = xTaskCreate(&recorderTaskEntry, "audio_rec", 8192, &ctx->audio, tskIDLE_PRIORITY + 3, &handle);
    if (ok != pdPASS) {
        LOG_E(TAG, "Failed to create audio task");
    } else {
        ctx->audio.task.store(handle);
    }
}

void recorderRenderList(Context* ctx) {
    if (ctx->list_container == nullptr) {
        return;
    }

    std::vector<RecordingItem> recordings;
    std::string selectedPath;
    {
        std::lock_guard lg(ctx->recordingsMutex);
        recordings = ctx->recordings;
        selectedPath = ctx->selectedPath;
    }

    // Drop a selection whose file no longer exists (e.g. when it was deleted externally)
    if (!selectedPath.empty()) {
        const bool exists = std::any_of(recordings.begin(), recordings.end(), [&selectedPath](const RecordingItem& item) {
            return item.fullpath == selectedPath;
        });
        if (!exists) {
            // Stop playback of a file that's no longer there
            const RecorderState state = ctx->audio.state.load();
            if (state == RecorderState::Playing || state == RecorderState::Paused) {
                std::string active;
                {
                    std::lock_guard lg(ctx->audio.pathMutex);
                    active = ctx->audio.activeFilepath;
                }
                if (active == selectedPath) {
                    ctx->audio.stopRequested.store(true);
                    ctx->audio.pauseRequested.store(false);
                }
            }
            {
                std::lock_guard lg(ctx->recordingsMutex);
                ctx->selectedPath.clear();
            }
            {
                std::lock_guard lg(ctx->audio.pathMutex);
                ctx->audio.requestedPlaybackPath.clear();
            }
            selectedPath.clear();
        }
    }

    // Keep the page where the user left it when only the contents change
    const int32_t pageScrollY = lv_obj_get_scroll_y(ctx->main_container);

    // The old player panel is about to be deleted with the rows
    ctx->player_panel = nullptr;
    ctx->play_bar = nullptr;
    ctx->play_time_label = nullptr;
    ctx->play_toggle_btn = nullptr;
    ctx->play_toggle_label = nullptr;
    ctx->play_stop_btn = nullptr;

    clearListRows(ctx);
    lv_obj_clean(ctx->list_container);

    const bool empty = recordings.empty();
    lv_obj_set_hidden(ctx->list_container, empty);
    lv_obj_set_hidden(ctx->empty_container, !empty);

    if (ctx->list_count_label != nullptr) {
        char count[16];
        snprintf(count, sizeof(count), "%u", (unsigned)recordings.size());
        lv_label_set_text(ctx->list_count_label, count);
    }

    for (const auto& item : recordings) {
        const bool selected = item.fullpath == selectedPath;

        // A full-width row with the theme's list-item padding, dividers and selected state
        lv_obj_t* row = lvgl_list_add_button(ctx->list_container, nullptr, nullptr);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* icon = lv_label_create(row);
        lv_label_set_text(icon, LV_SYMBOL_AUDIO);

        auto* textColumn = lv_obj_create(row);
        lv_obj_remove_style_all(textColumn);
        lv_obj_set_clickable(textColumn, false);
        lv_obj_set_width(textColumn, 0);
        lv_obj_set_height(textColumn, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(textColumn, 1);
        lv_obj_set_flex_flow(textColumn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(textColumn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_gap(textColumn, 0, 0);
        lv_obj_set_scrollable(textColumn, false);

        lv_obj_t* name = lv_label_create(textColumn);
        lv_obj_set_width(name, LV_PCT(100));
        lv_obj_set_style_text_font(name, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
        lv_obj_set_height(name, lv_font_get_line_height(lvgl_get_text_font(FONT_SIZE_DEFAULT)));
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        const std::string displayName = getDisplayName(item);
        lv_label_set_text(name, displayName.c_str());

        lv_obj_t* meta = lv_label_create(textColumn);
        lv_obj_set_width(meta, LV_PCT(100));
        lv_obj_set_style_text_font(meta, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
        lv_obj_set_height(meta, lv_font_get_line_height(lvgl_get_text_font(FONT_SIZE_SMALL)));
        lv_obj_set_style_text_opa(meta, LV_OPA_70, 0);
        lv_label_set_long_mode(meta, LV_LABEL_LONG_MODE_DOTS);
        char metaText[96];
        formatItemMeta(item, metaText, sizeof(metaText));
        lv_label_set_text(meta, metaText);

        auto* pathCopy = new std::string(item.fullpath);
        lv_obj_set_user_data(row, pathCopy);
        lv_obj_add_event_cb(row, rowClickedCb, LV_EVENT_SHORT_CLICKED, ctx);

        if (selected) {
            lv_obj_add_state(row, LV_STATE_CHECKED);
            addPlayerPanel(ctx, ctx->list_container, item);
        }
    }

    lvgl_list_hide_last_divider(ctx->list_container);

    lv_obj_update_layout(ctx->main_container);
    if (ctx->player_panel != nullptr) {
        // Bring the expanded controls into view, also when the tapped row was at the bottom
        scrollPlayerPanelIntoView(ctx);
    } else {
        lv_obj_scroll_to_y(ctx->main_container, pageScrollY, LV_ANIM_OFF);
    }

    syncStateUi(ctx);

    // A rebuilt panel starts at zero: restore the progress of paused/playing playback
    const RecorderState state = ctx->audio.state.load();
    if (state == RecorderState::Playing || state == RecorderState::Paused) {
        std::string activePath;
        {
            std::lock_guard lg(ctx->audio.pathMutex);
            activePath = ctx->audio.activeFilepath;
        }
        if (!selectedPath.empty() && activePath == selectedPath) {
            const uint32_t curSec = ctx->audio.currentTimeSec.load();
            const uint32_t totSec = ctx->audio.totalTimeSec.load();
            updatePlaybackProgress(ctx, curSec, totSec);
            ctx->lastRenderedSecond = curSec;
            ctx->lastRenderedTotal = totSec;
        }
    }
}

void recorderRefreshFromState(Context* ctx) {
    if (ctx->listDirty.exchange(false)) {
        recorderRenderList(ctx);
    }

    RecorderState state = ctx->audio.state.load();
    uint32_t curSec = ctx->audio.currentTimeSec.load();
    uint32_t totSec = ctx->audio.totalTimeSec.load();

    // The audio task publishes the result and its path together, so they stay consistent
    RecordingResult result;
    std::string recorded;
    {
        std::lock_guard lg(ctx->audio.pathMutex);
        result = ctx->audio.recordingResult;
        recorded = ctx->audio.completedRecordingPath;
        ctx->audio.recordingResult = RecordingResult::None;
        ctx->audio.completedRecordingPath.clear();
    }
    if (result != RecordingResult::None) {
        if (result == RecordingResult::Success && !recorded.empty()) {
            {
                std::lock_guard lg(ctx->recordingsMutex);
                ctx->selectedPath = recorded;
            }
            {
                std::lock_guard lg(ctx->audio.pathMutex);
                ctx->audio.requestedPlaybackPath = recorded;
            }
        } else if (result == RecordingResult::Failure) {
            LOG_W(TAG, "Recording failed");
        }
        recorderScanDirectory(ctx);
        recorderRenderList(ctx);
    }

    if (state != ctx->lastRenderedState) {
        syncStateUi(ctx);
        ctx->lastRenderedState = state;
        ctx->lastRenderedSecond = ~0u;
        ctx->lastRenderedTotal = ~0u;
    }

    if (curSec != ctx->lastRenderedSecond || totSec != ctx->lastRenderedTotal) {
        if (state == RecorderState::Recording) {
            if (ctx->rec_time_label != nullptr) {
                char time[16];
                formatTimeMmSs(curSec, time, sizeof(time));
                lv_label_set_text(ctx->rec_time_label, time);
            }
        } else if (state == RecorderState::Playing || state == RecorderState::Paused) {
            updatePlaybackProgress(ctx, curSec, totSec);
        }
        ctx->lastRenderedSecond = curSec;
        ctx->lastRenderedTotal = totSec;
    }
}

namespace {

void pollTimerCb(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    // Skip touching any lv_obj while a child dialog (e.g. the delete confirmation)
    // is on top - our widgets are torn down while buried and window_manager may
    // not have rebuilt them yet (see window_manager.h's WindowState docs).
    if (ctx->window != 0 && window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) {
        return;
    }
    recorderRefreshFromState(ctx);
}

} // namespace

void audiorecorderCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_scrollable(parent, false);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(parent, 0, LV_STATE_DEFAULT);

    ctx->toolbar = lvgl_toolbar_create(parent, "Recorder");
    ctx->delete_btn = lvgl_toolbar_add_text_button_action(ctx->toolbar, LV_SYMBOL_TRASH, deleteBtnCb, ctx);
    ctx->refresh_btn = lvgl_toolbar_add_text_button_action(ctx->toolbar, LV_SYMBOL_REFRESH, refreshBtnCb, ctx);

    // A transparent page: the theme's surface shows through and its container padding
    // and gap keep the cards aligned on every screen size and density.
    ctx->main_container = lv_obj_create(parent);
    lv_obj_set_width(ctx->main_container, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->main_container, 1);
    lv_obj_set_style_bg_opa(ctx->main_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->main_container, 0, 0);
    lv_obj_set_style_radius(ctx->main_container, 0, 0);
    lv_obj_set_scroll_dir(ctx->main_container, LV_DIR_VER);

    recorderInitAppData(ctx);
    recorderEnsureBackendStarted(ctx);
    recorderScanDirectory(ctx);

    rebuildContent(ctx);
    syncStateUi(ctx);

    if (ctx->pollTimer != nullptr) {
        lv_timer_del(ctx->pollTimer);
    }
    ctx->pollTimer = lv_timer_create(&pollTimerCb, 200, ctx);
}

void audiorecorderTeardown(Context* ctx) {
    if (ctx->pollTimer != nullptr) {
        lv_timer_del(ctx->pollTimer);
        ctx->pollTimer = nullptr;
    }

    ctx->audio.stopRequested.store(true);
    ctx->audio.shutdown.store(true);
    while (ctx->audio.task.load() != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ctx->toolbar = nullptr;
    ctx->main_container = nullptr;
    ctx->refresh_btn = nullptr;
    ctx->delete_btn = nullptr;

    clearWidgetPointers(ctx);
}
