// SPDX-License-Identifier: GPL-3.0
#include "Tone.h"
#include "ToneSynth.h"

#include <app/event.h>

#include <lvgl/devices/keyboard.h>
#include <lvgl/fonts.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/chip.h>
#include <lvgl/widgets/sliderbox.h>
#include <lvgl/widgets/toolbar.h>

#include <tactility/device.h>
#include <tactility/drivers/audio_codec.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/error.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int32_t REFRESH_PERIOD_MS = 200;

constexpr uint32_t DEFAULT_FREQUENCY_HZ = 440;
constexpr uint32_t DEFAULT_SWEEP_MIN_HZ = 200;
constexpr uint32_t DEFAULT_SWEEP_MAX_HZ = 1000;
constexpr uint32_t DEFAULT_SWEEP_PERIOD_MS = 4000;

struct FrequencyPreset {
    const char* label;
    uint32_t frequency; // 0 marks the "Sweep" option
};

constexpr FrequencyPreset FREQUENCY_PRESETS[] = {
    { "50", 50 },
    { "100", 100 },
    { "220", 220 },
    { "440", 440 },
    { "880", 880 },
    { "1K", 1000 },
    { "5K", 5000 },
    { "10K", 10000 },
    { "Sweep", 0 },
};
static_assert(
    sizeof(FREQUENCY_PRESETS) / sizeof(FREQUENCY_PRESETS[0]) == TONE_FREQUENCY_PRESET_COUNT,
    "preset list must match TONE_FREQUENCY_PRESET_COUNT");
constexpr const char* TONE_WAVEFORM_LABELS[TONE_WAVEFORM_COUNT] = {
    "Sine", "Square", "Saw", "Triangle", "Noise",
};
constexpr const char* TONE_CHANNEL_LABELS[TONE_CHANNEL_COUNT] = {
    "Mono", "Stereo",
};

const char* waveformName(ToneWaveform waveform) {
    const uint32_t index = static_cast<uint32_t>(waveform);
    return index < TONE_WAVEFORM_COUNT ? TONE_WAVEFORM_LABELS[index] : "?";
}

void onBackPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

void wireTextarea(lv_obj_t* textarea) {
#ifndef ESP_PLATFORM
    auto* keyboard = lvgl_software_keyboard_get_last();
    if (keyboard != nullptr && keyboard->object != nullptr) {
        lvgl_keyboard_add_textarea(keyboard, textarea);
    }
#endif
}

lv_obj_t* createNumberField(lv_obj_t* parent, uint16_t maxLength) {
    auto* textarea = lv_textarea_create(parent);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_accepted_chars(textarea, "0123456789");
    lv_textarea_set_max_length(textarea, maxLength);
    wireTextarea(textarea);
    return textarea;
}

uint32_t displayedFrequency(const Context* ctx) {
    const uint32_t hz = ctx->playback.fixedHz.load();
    if (hz == 0) {
        return DEFAULT_FREQUENCY_HZ;
    }
    return tone_synth::clamp_frequency(hz);
}

void setTextareaNumber(lv_obj_t* textarea, uint32_t value) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%" PRIu32, value);
    lv_textarea_set_text(textarea, buffer);
}

// Returns true when the field holds a complete positive integer
bool parsePositiveInteger(lv_obj_t* textarea, uint32_t* out) {
    const char* text = lv_textarea_get_text(textarea);
    if (text == nullptr || *text == '\0') {
        return false;
    }
    uint32_t value = 0;
    for (const char* c = text; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(*c - '0');
        if (value > 100000) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    *out = value;
    return true;
}

void resolveTextareaNumber(lv_obj_t* textarea, uint32_t fallback, std::atomic<uint32_t>* target) {
    uint32_t value = 0;
    target->store(parsePositiveInteger(textarea, &value) ? tone_synth::clamp_frequency(value) : fallback);
}

void refreshChipSelection(lv_obj_t** buttons, uint32_t count, int32_t selectedIndex) {
    for (uint32_t i = 0; i < count; i++) {
        if (buttons[i] != nullptr) {
            lv_obj_set_state(buttons[i], LV_STATE_CHECKED, static_cast<int32_t>(i) == selectedIndex);
        }
    }
}

void syncPresetChips(Context* ctx) {
    if (ctx->playback.sweep.load()) {
        refreshChipSelection(ctx->presetButtons, TONE_FREQUENCY_PRESET_COUNT,
                             static_cast<int32_t>(TONE_FREQUENCY_PRESET_COUNT) - 1);
        return;
    }
    const uint32_t hz = ctx->playback.fixedHz.load();
    int32_t match = -1;
    for (uint32_t i = 0; i < TONE_FREQUENCY_PRESET_COUNT - 1; i++) {
        if (FREQUENCY_PRESETS[i].frequency == hz) {
            match = static_cast<int32_t>(i);
            break;
        }
    }
    refreshChipSelection(ctx->presetButtons, TONE_FREQUENCY_PRESET_COUNT, match);
}

void updateFrequencyInput(Context* ctx) {
    ctx->updatingFrequencyInput = true;
    if (ctx->playback.sweep.load()) {
        lv_textarea_set_text(ctx->frequencyInput, "");
    } else {
        const uint32_t hz = ctx->playback.fixedHz.load();
        if (hz == 0) {
            lv_textarea_set_text(ctx->frequencyInput, "");
        } else {
            setTextareaNumber(ctx->frequencyInput, hz);
        }
    }
    ctx->updatingFrequencyInput = false;
}

void updateSweepCard(Context* ctx) {
    if (ctx->sweepCard != nullptr) {
        if (ctx->playback.sweep.load()) {
            lv_obj_remove_flag(ctx->sweepCard, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ctx->sweepCard, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void updateChannelAvailability(Context* ctx) {
    const bool playing = ctx->playback.playing.load();
    for (auto* button : ctx->channelButtons) {
        if (button != nullptr) {
            lv_obj_set_state(button, LV_STATE_DISABLED, playing);
        }
    }
}

void updatePlayButton(Context* ctx) {
    if (ctx->playButtonLabel == nullptr) {
        return;
    }
    lv_label_set_text(ctx->playButtonLabel, ctx->playback.playing.load() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void updateReadout(Context* ctx) {
    if (ctx->readoutFrequencyLabel == nullptr || ctx->readoutStatusLabel == nullptr) {
        return;
    }

    const bool playing = ctx->playback.playing.load();
    const bool sweep = ctx->playback.sweep.load();
    const auto waveform = static_cast<ToneWaveform>(ctx->playback.waveform.load());
    const uint32_t currentHz = ctx->playback.currentHz.load();

    if (playing && currentHz > 0) {
        lv_label_set_text_fmt(ctx->readoutFrequencyLabel, "%" PRIu32 " Hz", currentHz);
    } else if (sweep) {
        lv_label_set_text_fmt(ctx->readoutFrequencyLabel, "Sweep %" PRIu32 "-%" PRIu32 " Hz",
                              ctx->playback.sweepMinHz.load(), ctx->playback.sweepMaxHz.load());
    } else {
        lv_label_set_text_fmt(ctx->readoutFrequencyLabel, "%" PRIu32 " Hz", displayedFrequency(ctx));
    }

    const char* channel = ctx->selectedChannel == 0 ? "mono" : "stereo";
    if (playing) {
        lv_label_set_text_fmt(ctx->readoutStatusLabel, "Playing %s - %s", waveformName(waveform), channel);
    } else {
        lv_label_set_text_fmt(ctx->readoutStatusLabel, "Ready - %s - %s", waveformName(waveform), channel);
    }
}

void onPlayPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));

    // Serialize against main()'s close path: once `closing` is set, a late play press
    // must not start a task on a context that is about to be torn down.
    if (ctx->playbackGate == nullptr || xSemaphoreTake(ctx->playbackGate, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (ctx->closing) {
        xSemaphoreGive(ctx->playbackGate);
        return;
    }

    if (ctx->playback.playing.load()) {
        tone_playback_stop(&ctx->playback);
        updatePlayButton(ctx);
        updateChannelAvailability(ctx);
        updateReadout(ctx);
        xSemaphoreGive(ctx->playbackGate);
        return;
    }

    const error_t result = tone_playback_start(&ctx->playback);
    if (result != ERROR_NONE) {
        if (ctx->readoutStatusLabel != nullptr) {
            lv_label_set_text_fmt(ctx->readoutStatusLabel, "Playback failed (%s)", error_to_string(result));
        }
        xSemaphoreGive(ctx->playbackGate);
        return;
    }
    updatePlayButton(ctx);
    updateChannelAvailability(ctx);
    updateReadout(ctx);
    xSemaphoreGive(ctx->playbackGate);
}

void onPresetPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(event));

    for (uint32_t i = 0; i < TONE_FREQUENCY_PRESET_COUNT; i++) {
        if (ctx->presetButtons[i] != chip) {
            continue;
        }
        if (FREQUENCY_PRESETS[i].frequency == 0) {
            ctx->playback.sweep.store(true);
        } else {
            ctx->playback.sweep.store(false);
            ctx->playback.fixedHz.store(FREQUENCY_PRESETS[i].frequency);
        }
        break;
    }

    syncPresetChips(ctx);
    updateFrequencyInput(ctx);
    updateSweepCard(ctx);
    updateReadout(ctx);
}

void onFrequencyInputChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (ctx->updatingFrequencyInput) {
        return;
    }

    uint32_t frequency = 0;
    if (parsePositiveInteger(ctx->frequencyInput, &frequency)) {
        ctx->playback.sweep.store(false);
        ctx->playback.fixedHz.store(tone_synth::clamp_frequency(frequency));
    }

    syncPresetChips(ctx);
    updateSweepCard(ctx);
    updateReadout(ctx);
}

void onWaveformPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(event));

    for (uint32_t i = 0; i < TONE_WAVEFORM_COUNT; i++) {
        if (ctx->waveformButtons[i] == chip) {
            ctx->playback.waveform.store(i);
            break;
        }
    }
    refreshChipSelection(ctx->waveformButtons, TONE_WAVEFORM_COUNT,
                         static_cast<int32_t>(ctx->playback.waveform.load()));
    updateReadout(ctx);
}

void onChannelPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (ctx->playback.playing.load()) {
        return; // channels are fixed at stream-open time
    }
    auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(event));

    for (uint32_t i = 0; i < TONE_CHANNEL_COUNT; i++) {
        if (ctx->channelButtons[i] == chip) {
            ctx->selectedChannel = static_cast<uint8_t>(i);
            ctx->playback.channels = i == 0 ? 1 : 2;
            break;
        }
    }
    refreshChipSelection(ctx->channelButtons, TONE_CHANNEL_COUNT, ctx->selectedChannel);
    updateReadout(ctx);
}

// Parses a seconds value with up to three decimal places (e.g. "4.5") into milliseconds.
bool parsePeriodMs(const char* text, uint32_t* outMs) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    uint32_t intPart = 0;
    uint32_t fracMs = 0;
    uint32_t fracDigits = 0;
    bool pointSeen = false;
    for (const char* c = text; *c != '\0'; c++) {
        if (*c == '.') {
            if (pointSeen) {
                return false;
            }
            pointSeen = true;
            continue;
        }
        if (*c < '0' || *c > '9') {
            return false;
        }
        if (pointSeen) {
            if (fracDigits == 3) {
                return false;
            }
            fracMs = fracMs * 10 + static_cast<uint32_t>(*c - '0');
            fracDigits++;
        } else {
            intPart = intPart * 10 + static_cast<uint32_t>(*c - '0');
            if (intPart > 600) {
                return false;
            }
        }
    }
    while (fracDigits < 3) {
        fracMs *= 10;
        fracDigits++;
    }
    if (intPart == 0 && fracMs == 0) {
        return false;
    }
    *outMs = std::max<uint32_t>(250u, std::min<uint32_t>(intPart * 1000 + fracMs, 600000u));
    return true;
}

void onSweepMinChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    resolveTextareaNumber(ctx->sweepMinInput, DEFAULT_SWEEP_MIN_HZ, &ctx->playback.sweepMinHz);
    if (ctx->playback.sweep.load()) {
        updateReadout(ctx);
    }
}

void onSweepMaxChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    resolveTextareaNumber(ctx->sweepMaxInput, DEFAULT_SWEEP_MAX_HZ, &ctx->playback.sweepMaxHz);
    if (ctx->playback.sweep.load()) {
        updateReadout(ctx);
    }
}

void onSweepPeriodChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    uint32_t ms = 0;
    if (parsePeriodMs(lv_textarea_get_text(ctx->sweepPeriodInput), &ms)) {
        ctx->playback.sweepPeriodMs.store(ms);
    } else {
        ctx->playback.sweepPeriodMs.store(DEFAULT_SWEEP_PERIOD_MS);
    }
}

void onVolumeChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* box = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const int32_t percent = lvgl_sliderbox_get_value(box);
    ctx->volumePercent = static_cast<uint8_t>(percent);
    if (ctx->playback.streamDevice != nullptr) {
        audio_stream_set_volume(ctx->playback.streamDevice, AUDIO_CODEC_DIR_OUTPUT, static_cast<float>(percent));
    }
}

void onRefreshTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    if (ctx == nullptr) {
        return;
    }
    updatePlayButton(ctx);
    updateChannelAvailability(ctx);
    updateReadout(ctx);
}

lv_obj_t* createCard(lv_obj_t* parent) {
    auto* card = lvgl_card_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t* createRow(lv_obj_t* parent, lv_coord_t gap) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, gap, LV_STATE_DEFAULT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t* createChip(lv_obj_t* parent, const char* text, lv_coord_t minWidth) {
    auto* chip = lvgl_chip_create(parent);
    if (minWidth > 0) {
        lv_obj_set_style_min_width(chip, minWidth, LV_STATE_DEFAULT);
    }
    auto* label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return chip;
}

void toneCreateWidgetsImpl(lv_obj_t* parent, Context* ctx) {
    const bool compact = lvgl_get_ui_density() == LVGL_UI_DENSITY_COMPACT;
    const lv_coord_t pad = compact ? 8 : 12;
    const lv_coord_t gap = compact ? 4 : 8;
    const lv_coord_t chipMinWidth = compact ? 48 : 56;
    const lv_coord_t fontHeight = lvgl_get_text_font_height(FONT_SIZE_DEFAULT);
    const lv_coord_t fieldHeight = static_cast<lv_coord_t>(fontHeight * 1.8f);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    auto* toolbar = lvgl_toolbar_create(parent, "Tone");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);
    ctx->playButton = lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_PLAY, onPlayPressed, ctx);
    ctx->playButtonLabel = lv_obj_get_child(ctx->playButton, 0);

    auto* mainWrapper = lv_obj_create(parent);
    lv_obj_set_flex_flow(mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(mainWrapper, LV_PCT(100));
    lv_obj_set_flex_grow(mainWrapper, 1);
    lv_obj_set_style_pad_all(mainWrapper, pad, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(mainWrapper, gap, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(mainWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(mainWrapper, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(mainWrapper, LV_DIR_VER);

    // Readout
    auto* readoutCard = createCard(mainWrapper);
    ctx->readoutFrequencyLabel = lv_label_create(readoutCard);
    lv_obj_set_width(ctx->readoutFrequencyLabel, LV_PCT(100));
    lv_obj_set_style_text_align(ctx->readoutFrequencyLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ctx->readoutFrequencyLabel, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);
    ctx->readoutStatusLabel = lv_label_create(readoutCard);
    lv_obj_set_width(ctx->readoutStatusLabel, LV_PCT(100));
    lv_obj_set_style_text_align(ctx->readoutStatusLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ctx->readoutStatusLabel, lv_palette_darken(LV_PALETTE_GREY, 3), LV_STATE_DEFAULT);

    // Frequency presets + free entry
    auto* frequencyCard = createCard(mainWrapper);
    auto* presetRow = createRow(frequencyCard, gap);
    for (uint32_t i = 0; i < TONE_FREQUENCY_PRESET_COUNT; i++) {
        auto* chip = createChip(presetRow, FREQUENCY_PRESETS[i].label, chipMinWidth);
        lv_obj_add_event_cb(chip, onPresetPressed, LV_EVENT_SHORT_CLICKED, ctx);
        ctx->presetButtons[i] = chip;
    }

    ctx->frequencyInput = createNumberField(frequencyCard, 5);
    lv_textarea_set_placeholder_text(ctx->frequencyInput, "Frequency in Hz");
    lv_textarea_set_align(ctx->frequencyInput, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(ctx->frequencyInput, LV_PCT(100), fieldHeight);
    lv_obj_add_event_cb(ctx->frequencyInput, onFrequencyInputChanged, LV_EVENT_VALUE_CHANGED, ctx);

    // Waveform
    auto* waveformCard = createCard(mainWrapper);
    auto* waveformRow = createRow(waveformCard, gap);
    for (uint32_t i = 0; i < TONE_WAVEFORM_COUNT; i++) {
        auto* chip = createChip(waveformRow, TONE_WAVEFORM_LABELS[i], chipMinWidth);
        lv_obj_add_event_cb(chip, onWaveformPressed, LV_EVENT_SHORT_CLICKED, ctx);
        ctx->waveformButtons[i] = chip;
    }

    // Sweep settings (shown only while sweep mode is active)
    ctx->sweepCard = createCard(mainWrapper);
    auto* sweepTitle = lv_label_create(ctx->sweepCard);
    lv_label_set_text(sweepTitle, "Sweep");
    lv_obj_set_style_text_font(sweepTitle, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);

    ctx->sweepMinInput = createNumberField(ctx->sweepCard, 5);
    lv_textarea_set_placeholder_text(ctx->sweepMinInput, "Min Hz");
    lv_textarea_set_align(ctx->sweepMinInput, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(ctx->sweepMinInput, LV_PCT(100), fieldHeight);
    lv_obj_add_event_cb(ctx->sweepMinInput, onSweepMinChanged, LV_EVENT_VALUE_CHANGED, ctx);

    ctx->sweepMaxInput = createNumberField(ctx->sweepCard, 5);
    lv_textarea_set_placeholder_text(ctx->sweepMaxInput, "Max Hz");
    lv_textarea_set_align(ctx->sweepMaxInput, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(ctx->sweepMaxInput, LV_PCT(100), fieldHeight);
    lv_obj_add_event_cb(ctx->sweepMaxInput, onSweepMaxChanged, LV_EVENT_VALUE_CHANGED, ctx);

    ctx->sweepPeriodInput = createNumberField(ctx->sweepCard, 6);
    lv_textarea_set_placeholder_text(ctx->sweepPeriodInput, "Period (s)");
    lv_textarea_set_accepted_chars(ctx->sweepPeriodInput, "0123456789.");
    lv_textarea_set_align(ctx->sweepPeriodInput, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(ctx->sweepPeriodInput, LV_PCT(100), fieldHeight);
    lv_obj_add_event_cb(ctx->sweepPeriodInput, onSweepPeriodChanged, LV_EVENT_VALUE_CHANGED, ctx);

    // Output (channels + volume)
    auto* outputCard = createCard(mainWrapper);
    auto* channelRow = createRow(outputCard, gap);
    for (uint32_t i = 0; i < TONE_CHANNEL_COUNT; i++) {
        auto* chip = createChip(channelRow, TONE_CHANNEL_LABELS[i], chipMinWidth);
        lv_obj_add_event_cb(chip, onChannelPressed, LV_EVENT_SHORT_CLICKED, ctx);
        ctx->channelButtons[i] = chip;
    }

    auto* volumeRow = createRow(outputCard, gap);
    auto* volumeLabel = lv_label_create(volumeRow);
    lv_label_set_text(volumeLabel, "Volume");
    ctx->volumeBox = lvgl_sliderbox_create(volumeRow, 0, 100, 5, ctx->volumePercent);
    lv_obj_set_width(ctx->volumeBox, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->volumeBox, 1);
    lvgl_sliderbox_add_value_changed_cb(ctx->volumeBox, onVolumeChanged, ctx);

    // Seed the sweep fields from the current settings.
    setTextareaNumber(ctx->sweepMinInput, ctx->playback.sweepMinHz.load());
    setTextareaNumber(ctx->sweepMaxInput, ctx->playback.sweepMaxHz.load());
    {
        char periodBuffer[16];
        const uint32_t periodMs = ctx->playback.sweepPeriodMs.load();
        // Integer-only: { "4.000" for 4000, "0.250" for 250 }. Avoids double math
        // (%f / double division), which the Xtensa firmware cannot resolve for app code.
        snprintf(periodBuffer, sizeof(periodBuffer), "%" PRIu32 ".%03" PRIu32,
                 periodMs / 1000, periodMs % 1000);
        lv_textarea_set_text(ctx->sweepPeriodInput, periodBuffer);
    }

    refreshChipSelection(ctx->channelButtons, TONE_CHANNEL_COUNT, ctx->selectedChannel);
    ctx->playback.channels = ctx->selectedChannel == 0 ? 1 : 2;

    syncPresetChips(ctx);
    refreshChipSelection(ctx->waveformButtons, TONE_WAVEFORM_COUNT,
                         static_cast<int32_t>(ctx->playback.waveform.load()));
    updateFrequencyInput(ctx);
    updateSweepCard(ctx);
    updateReadout(ctx);
    updatePlayButton(ctx);
    updateChannelAvailability(ctx);

    ctx->refreshTimer = lv_timer_create(onRefreshTimer, REFRESH_PERIOD_MS, ctx);
}

} // namespace

void toneCreateWidgets(lv_obj_t* root, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    if (ctx->refreshTimer != nullptr) {
        lv_timer_delete(ctx->refreshTimer);
        ctx->refreshTimer = nullptr;
    }

    // The audio-stream device may appear at any point (e.g. a USB codec) so re-query on
    // every window build rather than caching a one-time lookup.
    Device* streamDevice = ctx->playback.streamDevice;
    if (streamDevice == nullptr) {
        if (device_get_first_by_type(&AUDIO_STREAM_TYPE, &streamDevice) == ERROR_NONE && streamDevice != nullptr) {
            ctx->playback.streamDevice = streamDevice;
        }
    }
    if (ctx->playback.streamDevice != nullptr) {
        float initialVolume = 100.0f;
        if (audio_stream_get_volume(ctx->playback.streamDevice, AUDIO_CODEC_DIR_OUTPUT, &initialVolume) == ERROR_NONE) {
            ctx->volumePercent = static_cast<uint8_t>(std::max(0.0f, std::min(initialVolume, 100.0f)));
        }
    }

    toneCreateWidgetsImpl(root, ctx);
}

void toneDestroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    if (ctx->refreshTimer != nullptr) {
        lv_timer_delete(ctx->refreshTimer);
        ctx->refreshTimer = nullptr;
    }

    ctx->playButton = nullptr;
    ctx->playButtonLabel = nullptr;
    ctx->readoutFrequencyLabel = nullptr;
    ctx->readoutStatusLabel = nullptr;
    ctx->frequencyInput = nullptr;
    for (auto*& button : ctx->presetButtons) {
        button = nullptr;
    }
    for (auto*& button : ctx->waveformButtons) {
        button = nullptr;
    }
    for (auto*& button : ctx->channelButtons) {
        button = nullptr;
    }
    ctx->sweepCard = nullptr;
    ctx->sweepMinInput = nullptr;
    ctx->sweepMaxInput = nullptr;
    ctx->sweepPeriodInput = nullptr;
    ctx->volumeBox = nullptr;
}