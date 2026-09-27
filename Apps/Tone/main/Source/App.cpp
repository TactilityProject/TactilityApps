// SPDX-License-Identifier: GPL-3.0

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/audio_codec.h>
#include <tactility/drivers/audio_stream.h>

#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/widgets/toolbar.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>

#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdint>

namespace {

constexpr auto* TAG = "Tone";

constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint8_t BITS_PER_SAMPLE = 16;
constexpr uint8_t CHANNELS = 2;

constexpr size_t CHUNK_SAMPLES = 512;
constexpr int32_t AMPLITUDE = 8000; // ~25% of full scale

constexpr uint32_t SWEEP_FREQUENCY = 0;
constexpr uint32_t SWEEP_START_HZ = 200;
constexpr uint32_t SWEEP_END_HZ = 1000;
constexpr uint32_t SWEEP_PERIOD_SAMPLES = 2u * SAMPLE_RATE;

constexpr double PI = 3.14159265358979323846;

constexpr uint32_t PLAYBACK_TASK_STACK_BYTES = 4096;
constexpr uint32_t PLAYBACK_TASK_PRIORITY = 9;

constexpr lv_coord_t MAX_CONTENT_WIDTH = 560;

struct FrequencyPreset {
    const char* label;
    uint32_t frequency;
};

constexpr FrequencyPreset FREQUENCY_PRESETS[] = {
    { "440 Hz", 440u },
    { "622 Hz", 622u },
    { "880 Hz", 880u },
    { "Sweep", SWEEP_FREQUENCY },
};

constexpr uint32_t PRESET_COUNT = sizeof(FREQUENCY_PRESETS) / sizeof(FREQUENCY_PRESETS[0]);

struct Context {
    uint32_t appInstanceId;

    Device* streamDevice = nullptr;

    // Read/written from the LVGL thread and the playback task.
    std::atomic<bool> playing { false };
    std::atomic<uint32_t> frequency { SWEEP_FREQUENCY };

    TaskHandle_t playbackTask = nullptr;
    AudioStreamHandle streamHandle = nullptr;

    lv_obj_t* presetButtons[PRESET_COUNT] = {};
    uint32_t selectedPreset = PRESET_COUNT - 1; // Sweep is selected by default

    lv_obj_t* playButton = nullptr;
    lv_obj_t* playButtonLabel = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* volumeValueLabel = nullptr;
};

void updateStatusLabel(Context* ctx) {
    if (ctx->statusLabel == nullptr) {
        return;
    }
    if (ctx->playing.load()) {
        if (ctx->frequency.load() == SWEEP_FREQUENCY) {
            lv_label_set_text(ctx->statusLabel, "Playing sweep 200-1000 Hz");
        } else {
            lv_label_set_text_fmt(ctx->statusLabel, "Playing %" PRIu32 " Hz", static_cast<uint32_t>(ctx->frequency.load()));
        }
    } else {
        if (ctx->frequency.load() == SWEEP_FREQUENCY) {
            lv_label_set_text(ctx->statusLabel, "Sweep 200-1000 Hz. Press play.");
        } else {
            lv_label_set_text_fmt(ctx->statusLabel, "%" PRIu32 " Hz. Press play.", static_cast<uint32_t>(ctx->frequency.load()));
        }
    }
}

void onBackPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

void onFrequencyPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(event));
    uint32_t frequency = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button)));

    for (uint32_t i = 0; i < PRESET_COUNT; i++) {
        if (ctx->presetButtons[i] == button) {
            ctx->selectedPreset = i;
            lv_obj_add_state(button, LV_STATE_CHECKED);
        } else if (ctx->presetButtons[i] != nullptr) {
            lv_obj_clear_state(ctx->presetButtons[i], LV_STATE_CHECKED);
        }
    }

    ctx->frequency.store(frequency);
    updateStatusLabel(ctx);
}

void updatePlayButton(Context* ctx) {
    if (ctx->playButton == nullptr || ctx->playButtonLabel == nullptr) {
        return;
    }
    if (ctx->playing.load()) {
        lv_label_set_text(ctx->playButtonLabel, LV_SYMBOL_PAUSE " Stop");
        lv_obj_set_style_bg_color(ctx->playButton, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
        lv_obj_set_style_bg_color(ctx->playButton, lv_palette_darken(LV_PALETTE_RED, 2), LV_STATE_PRESSED);
    } else {
        lv_label_set_text(ctx->playButtonLabel, LV_SYMBOL_PLAY " Play");
        lv_obj_set_style_bg_color(ctx->playButton, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(ctx->playButton, lv_palette_darken(LV_PALETTE_BLUE, 2), LV_STATE_PRESSED);
    }
}

void playbackTask(void* argument) {
    auto* ctx = static_cast<Context*>(argument);

    if (ctx->streamHandle == nullptr) {
        ctx->playing.store(false);
        ctx->playbackTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int16_t chunk[CHUNK_SAMPLES * CHANNELS];
    uint32_t sweepSample = 0;
    double phase = 0.0;

    while (ctx->playing.load()) {
        uint32_t frequency = ctx->frequency.load();
        for (size_t i = 0; i < CHUNK_SAMPLES; i++) {
            double currentFrequency;
            if (frequency == SWEEP_FREQUENCY) {
                double progress = static_cast<double>(sweepSample % SWEEP_PERIOD_SAMPLES) / SWEEP_PERIOD_SAMPLES;
                currentFrequency = SWEEP_START_HZ + (SWEEP_END_HZ - SWEEP_START_HZ) * progress;
                sweepSample++;
            } else {
                currentFrequency = frequency;
            }

            int16_t sample = static_cast<int16_t>(AMPLITUDE * std::sin(phase));
            for (size_t c = 0; c < CHANNELS; c++) {
                chunk[i * CHANNELS + c] = sample;
            }
            phase += 2.0 * PI / SAMPLE_RATE * currentFrequency;
        }

        size_t bytesWritten = 0;
        // Block instead of timing out
        error_t result = audio_stream_write(ctx->streamHandle, chunk, sizeof(chunk), &bytesWritten, portMAX_DELAY);
        if (result != ERROR_NONE) {
            ESP_LOGE(TAG, "Write failed: %s", error_to_string(result));
            ctx->playing.store(false);
            break;
        }
    }

    audio_stream_close(ctx->streamHandle);
    ctx->streamHandle = nullptr;
    ctx->playbackTask = nullptr;

    lvgl_lock();
    updateStatusLabel(ctx);
    updatePlayButton(ctx);
    lvgl_unlock();

    vTaskDelete(nullptr);
}

void onPlayPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));

    if (ctx->playing.load()) {
        ctx->playing.store(false);
        lv_label_set_text(ctx->statusLabel, "Stopping...");
        updatePlayButton(ctx);
        return;
    }

    Device* streamDevice = nullptr;
    if (device_get_first_by_type(&AUDIO_STREAM_TYPE, &streamDevice) != ERROR_NONE || streamDevice == nullptr) {
        lv_label_set_text(ctx->statusLabel, "No audio stream device");
        return;
    }
    ctx->streamDevice = streamDevice;

    const AudioStreamConfig config = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = BITS_PER_SAMPLE,
        .channels = CHANNELS,
    };

    AudioStreamHandle handle = nullptr;
    if (audio_stream_open_output(streamDevice, &config, &handle) != ERROR_NONE) {
        lv_label_set_text(ctx->statusLabel, "Failed to open output stream");
        return;
    }

    ctx->streamHandle = handle;
    ctx->playing.store(true);
    BaseType_t taskResult = xTaskCreate(playbackTask, "tone-playback", PLAYBACK_TASK_STACK_BYTES, ctx, PLAYBACK_TASK_PRIORITY, &ctx->playbackTask);
    if (taskResult != pdPASS) {
        ctx->playing.store(false);
        ctx->streamHandle = nullptr;
        audio_stream_close(handle);
        lv_label_set_text(ctx->statusLabel, "Failed to start playback task");
        updatePlayButton(ctx);
        return;
    }

    updateStatusLabel(ctx);
    updatePlayButton(ctx);
}

void onVolumeSlider(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    lv_obj_t* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
    int32_t percent = lv_slider_get_value(slider);
    if (ctx->volumeValueLabel != nullptr) {
        lv_label_set_text_fmt(ctx->volumeValueLabel, "%" PRId32, percent);
    }
    if (ctx->streamDevice != nullptr) {
        audio_stream_set_volume(ctx->streamDevice, AUDIO_CODEC_DIR_OUTPUT, static_cast<float>(percent));
    }
}

lv_obj_t* createFrequencyButton(lv_obj_t* parent, const FrequencyPreset& preset, Context* ctx) {
    auto* button = lv_button_create(parent);
    auto* buttonLabel = lv_label_create(button);
    lv_label_set_text(buttonLabel, preset.label);
    lv_obj_center(buttonLabel);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<uintptr_t>(preset.frequency)));
    lv_obj_add_event_cb(button, onFrequencyPressed, LV_EVENT_SHORT_CLICKED, ctx);

    lv_obj_set_style_bg_color(button, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, lv_palette_darken(LV_PALETTE_BLUE, 2), LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_palette_lighten(LV_PALETTE_GREY, 3), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(button, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_FOCUS_KEY);

    return button;
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    float initialVolume = 100.0f;
    Device* volumeDevice = nullptr;
    if (device_get_first_by_type(&AUDIO_STREAM_TYPE, &volumeDevice) == ERROR_NONE && volumeDevice != nullptr) {
        audio_stream_get_volume(volumeDevice, AUDIO_CODEC_DIR_OUTPUT, &initialVolume);
        ctx->streamDevice = volumeDevice;
    }

    const lv_coord_t pad = 12;
    const lv_coord_t gap = 10;
    const uint32_t textHeight = lvgl_get_text_font_height(FONT_SIZE_DEFAULT);
    const lv_coord_t controlHeight = static_cast<lv_coord_t>(textHeight * 2.0f);
    const lv_coord_t playHeight = static_cast<lv_coord_t>(textHeight * 2.4f);
    const lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    const lv_coord_t contentWidth = screenWidth < MAX_CONTENT_WIDTH ? screenWidth : MAX_CONTENT_WIDTH;

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* toolbar = lvgl_toolbar_create(parent, "Tone");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);

    // Content area fills everything below the toolbar; its flex alignment centers
    // the control block as a group on both axes.
    auto* content = lv_obj_create(parent);
    lv_obj_set_width(content, contentWidth);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(content, pad, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(content, gap, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    auto* presetRow = lv_obj_create(content);
    lv_obj_set_width(presetRow, LV_PCT(100));
    lv_obj_set_height(presetRow, controlHeight);
    lv_obj_set_flex_flow(presetRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(presetRow, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(presetRow, gap, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(presetRow, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(presetRow, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_remove_flag(presetRow, LV_OBJ_FLAG_SCROLLABLE);

    for (uint32_t i = 0; i < PRESET_COUNT; i++) {
        auto* button = createFrequencyButton(presetRow, FREQUENCY_PRESETS[i], ctx);
        lv_obj_set_height(button, controlHeight);
        lv_obj_set_style_pad_hor(button, 0, LV_STATE_DEFAULT);
        lv_obj_set_flex_grow(button, 1);
        ctx->presetButtons[i] = button;
    }
    lv_obj_add_state(ctx->presetButtons[ctx->selectedPreset], LV_STATE_CHECKED);

    // Natural-width play button, centered by the content flex.
    ctx->playButton = lv_button_create(content);
    lv_obj_set_width(ctx->playButton, LV_SIZE_CONTENT);
    lv_obj_set_height(ctx->playButton, playHeight);
    lv_obj_set_style_pad_hor(ctx->playButton, 24, LV_STATE_DEFAULT);
    ctx->playButtonLabel = lv_label_create(ctx->playButton);
    lv_label_set_text(ctx->playButtonLabel, LV_SYMBOL_PLAY " Play");
    lv_obj_center(ctx->playButtonLabel);
    lv_obj_set_style_text_color(ctx->playButtonLabel, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ctx->playButton, onPlayPressed, LV_EVENT_SHORT_CLICKED, ctx);
    updatePlayButton(ctx);

    auto* volumeRow = lv_obj_create(content);
    lv_obj_set_width(volumeRow, LV_PCT(100));
    lv_obj_set_height(volumeRow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(volumeRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volumeRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(volumeRow, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(volumeRow, gap, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(volumeRow, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(volumeRow, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_remove_flag(volumeRow, LV_OBJ_FLAG_SCROLLABLE);

    auto* volumeIcon = lv_label_create(volumeRow);
    lv_label_set_text(volumeIcon, LV_SYMBOL_VOLUME_MAX);

    // Thin-track slider: the object height IS the track thickness; knob paddles
    // inflate the knob above/below it (see lv_slider.c position_knob()).
    auto* volumeSlider = lv_slider_create(volumeRow);
    lv_slider_set_range(volumeSlider, 0, 100);
    lv_slider_set_value(volumeSlider, static_cast<int32_t>(initialVolume), LV_ANIM_OFF);
    lv_obj_set_flex_grow(volumeSlider, 1);
    lv_obj_set_height(volumeSlider, 6);
    lv_obj_set_style_pad_ver(volumeSlider, 5, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(volumeSlider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volumeSlider, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(volumeSlider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volumeSlider, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(volumeSlider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(volumeSlider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_border_color(volumeSlider, lv_palette_main(LV_PALETTE_BLUE), LV_PART_KNOB);
    lv_obj_set_style_border_width(volumeSlider, 2, LV_PART_KNOB);
    lv_obj_set_style_radius(volumeSlider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(volumeSlider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(volumeSlider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(volumeSlider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(volumeSlider, onVolumeSlider, LV_EVENT_VALUE_CHANGED, ctx);

    ctx->volumeValueLabel = lv_label_create(volumeRow);
    lv_label_set_text_fmt(ctx->volumeValueLabel, "%" PRId32, static_cast<int32_t>(initialVolume));
    lv_obj_set_width(ctx->volumeValueLabel, 24);
    lv_obj_set_style_text_align(ctx->volumeValueLabel, LV_TEXT_ALIGN_RIGHT, LV_STATE_DEFAULT);

    ctx->statusLabel = lv_label_create(content);
    lv_obj_set_width(ctx->statusLabel, LV_PCT(100));
    lv_obj_set_style_text_align(ctx->statusLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ctx->statusLabel, lv_palette_darken(LV_PALETTE_GREY, 3), LV_STATE_DEFAULT);
    updateStatusLabel(ctx);
}

} // namespace

extern "C" {

int main(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    Context ctx {};
    ctx.appInstanceId = appInstanceId;

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &eventGroup) == ERROR_NONE);

    WindowId window = window_manager_create(appInstanceId, createWidgets, &ctx);

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&eventGroup, nullptr, portMAX_DELAY);

        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                // Stop playback so the stream closes before our task dies.
                ctx.playing.store(false);
                while (ctx.playbackTask != nullptr) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                shouldClose = true;
            }
            if (shouldClose) break;
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);

    return 0;
}

} // extern "C"