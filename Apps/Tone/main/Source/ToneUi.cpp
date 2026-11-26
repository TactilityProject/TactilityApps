// SPDX-License-Identifier: GPL-3.0
#include "Tone.h"
#include "ToneSynth.h"

#include <app/event.h>

#include <lvgl/fonts.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/chip.h>
#include <lvgl/widgets/sliderbox.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl/icons/shared.h>
#include <lvgl_window_manager/dialog.h>

#include <tactility/device.h>
#include <tactility/drivers/audio_codec.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/error.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int32_t REFRESH_PERIOD_MS = 200;
constexpr int32_t SLIDER_STEPS = 1000;
constexpr int32_t MAX_PERIOD_SLIDER_S = 30;

constexpr uint32_t PRESETS[TONE_PRESET_COUNT] = { 50, 100, 220, 440, 880, 1000, 5000, 10000 };
// One string, as tables of pointers need relocation
constexpr char PRESET_NAMES[TONE_PRESET_COUNT][4] = { "50", "100", "220", "440", "880", "1k", "5k", "10k" };
constexpr char WAVEFORM_NAMES[TONE_WAVEFORM_COUNT][7] = { "Sine", "Square", "Saw", "Tri", "Noise" };
constexpr char NOTE_NAMES[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

enum Action : int32_t { ACTION_NONE, ACTION_MENU, ACTION_PRESET, ACTION_BACK };
enum MenuItem : uint8_t { MENU_PRESETS, MENU_SWEEP, MENU_FREQUENCY };

void rebuild(Context* ctx);
void syncControls(Context* ctx);
void showFrequencyDialog(Context* ctx);

Context* contextOf(lv_event_t* event) {
    return static_cast<Context*>(lv_event_get_user_data(event));
}

uint32_t indexOf(lv_event_t* event) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
}

void setIndex(lv_obj_t* obj, uint32_t index) {
    lv_obj_set_user_data(obj, reinterpret_cast<void*>(static_cast<uintptr_t>(index)));
}

void requestAction(Context* ctx, Action action, int32_t value = 0) {
    ctx->action = static_cast<uint32_t>(action) | (static_cast<uint32_t>(value) << 8);
    task_event_group_signal(ctx->eventGroup, ctx->actionBit);
}

/* ── Frequencies ──────────────────────────────────────────────────── */

/** The sliders are logarithmic: every octave gets the same room */
int32_t sliderOfFrequency(uint32_t hz) {
    const float ratio = static_cast<float>(tone_synth::MAX_FREQUENCY_HZ) / tone_synth::MIN_FREQUENCY_HZ;
    const float position = logf(static_cast<float>(hz) / tone_synth::MIN_FREQUENCY_HZ) / logf(ratio);
    return static_cast<int32_t>(position * SLIDER_STEPS + 0.5f);
}

uint32_t frequencyOfSlider(int32_t value) {
    const float ratio = static_cast<float>(tone_synth::MAX_FREQUENCY_HZ) / tone_synth::MIN_FREQUENCY_HZ;
    const float hz = tone_synth::MIN_FREQUENCY_HZ * powf(ratio, static_cast<float>(value) / SLIDER_STEPS);
    return tone_synth::clamp_frequency(static_cast<uint32_t>(hz + 0.5f));
}

/** E.g. "A4" or "C#5 -12" with the offset in cents */
void formatNote(uint32_t hz, char* buffer, size_t size) {
    const float semitones = 69.0f + 12.0f * log2f(static_cast<float>(hz) / 440.0f);
    const int note = static_cast<int>(semitones + 0.5f);
    const float offset = (semitones - note) * 100.0f;
    const int cents = static_cast<int>(offset >= 0 ? offset + 0.5f : offset - 0.5f);
    if (cents == 0) {
        snprintf(buffer, size, "%s%d", NOTE_NAMES[note % 12], note / 12 - 1);
    } else {
        snprintf(buffer, size, "%s%d %+d", NOTE_NAMES[note % 12], note / 12 - 1, cents);
    }
}

void setFixedFrequency(Context* ctx, uint32_t hz) {
    ctx->playback.sweep.store(false);
    ctx->playback.fixedHz.store(tone_synth::clamp_frequency(hz));
    syncControls(ctx);
}

/* ── Showing the state ────────────────────────────────────────────── */

void updateReadout(Context* ctx) {
    if (ctx->frequencyLabel == nullptr) return;

    const bool playing = ctx->playback.playing.load();
    const bool sweep = ctx->playback.sweep.load();
    const uint32_t currentHz = ctx->playback.currentHz.load();
    const uint32_t fixedHz = ctx->playback.fixedHz.load();
    const char* state = playing ? "playing" : "ready";

    if (sweep && !(playing && currentHz > 0)) {
        lv_label_set_text_fmt(ctx->frequencyLabel, "%" PRIu32 "-%" PRIu32 " Hz",
            ctx->playback.sweepMinHz.load(), ctx->playback.sweepMaxHz.load());
    } else {
        lv_label_set_text_fmt(ctx->frequencyLabel, "%" PRIu32 " Hz", sweep ? currentHz : fixedHz);
    }

    if (sweep) {
        lv_label_set_text_fmt(ctx->statusLabel, "sweep - %s", state);
    } else {
        char note[16];
        formatNote(fixedHz, note, sizeof(note));
        lv_label_set_text_fmt(ctx->statusLabel, "%s - %s", note, state);
    }
}

void updatePlayButton(Context* ctx) {
    if (ctx->playButton == nullptr) return;
    const bool playing = ctx->playback.playing.load();
    lv_label_set_text(ctx->playLabel, playing ? LV_SYMBOL_STOP " Stop" : LV_SYMBOL_PLAY " Play");
}

void drawPreview(Context* ctx) {
    if (ctx->previewLine == nullptr) return;
    lv_obj_t* box = lv_obj_get_parent(ctx->previewLine);
    const int32_t width = lv_obj_get_content_width(box);
    const int32_t height = lv_obj_get_content_height(box);
    if (width <= 0 || height <= 0) return;

    const auto waveform = static_cast<ToneWaveform>(ctx->playback.waveform.load());
    uint32_t noise = 0x12345678u;
    for (uint32_t i = 0; i < TONE_PREVIEW_POINTS; i++) {
        // Four periods
        const float phase = static_cast<float>((i * 4) % (TONE_PREVIEW_POINTS - 1)) / (TONE_PREVIEW_POINTS - 1);
        float value = 0;
        switch (waveform) {
            case ToneWaveform::Sine: value = sinf(phase * 6.2831853f); break;
            case ToneWaveform::Square: value = phase < 0.5f ? 1.0f : -1.0f; break;
            case ToneWaveform::Saw: value = 2.0f * phase - 1.0f; break;
            case ToneWaveform::Triangle: value = phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase; break;
            case ToneWaveform::Noise:
                noise = noise * 1103515245u + 12345u;
                value = static_cast<float>((noise >> 16) & 0x7fff) / 16384.0f - 1.0f;
                break;
        }
        ctx->previewPoints[i].x = static_cast<lv_value_precise_t>(i * (width - 1) / (TONE_PREVIEW_POINTS - 1));
        ctx->previewPoints[i].y = static_cast<lv_value_precise_t>((height - 1) / 2.0f - value * (height - 3) / 2.0f);
    }
    lv_line_set_points(ctx->previewLine, ctx->previewPoints, TONE_PREVIEW_POINTS);
}

void setChecked(lv_obj_t* obj, bool checked) {
    if (obj != nullptr) lv_obj_set_state(obj, LV_STATE_CHECKED, checked);
}

/** Updates all controls to the settings */
void syncControls(Context* ctx) {
    const bool sweep = ctx->playback.sweep.load();
    const uint32_t hz = ctx->playback.fixedHz.load();
    const uint32_t waveform = ctx->playback.waveform.load();

    if (ctx->frequencySlider != nullptr) {
        lv_slider_set_value(ctx->frequencySlider, sliderOfFrequency(hz), LV_ANIM_OFF);
    }
    for (uint32_t i = 0; i < TONE_WAVEFORM_COUNT; i++) {
        setChecked(ctx->waveformChips[i], i == waveform);
    }
    for (uint32_t i = 0; i < TONE_PRESET_COUNT; i++) {
        setChecked(ctx->presetChips[i], !sweep && PRESETS[i] == hz);
    }
    setChecked(ctx->fixedChip, !sweep);
    setChecked(ctx->sweepChip, sweep);

    const uint32_t from = ctx->playback.sweepMinHz.load();
    const uint32_t to = ctx->playback.sweepMaxHz.load();
    const uint32_t periodMs = ctx->playback.sweepPeriodMs.load();
    if (ctx->sweepLabels[0] != nullptr) {
        lv_slider_set_value(ctx->sweepSliders[0], sliderOfFrequency(from), LV_ANIM_OFF);
        lv_slider_set_value(ctx->sweepSliders[1], sliderOfFrequency(to), LV_ANIM_OFF);
        lv_slider_set_value(ctx->sweepSliders[2], std::clamp<int32_t>(periodMs / 1000, 1, MAX_PERIOD_SLIDER_S), LV_ANIM_OFF);
        lv_label_set_text_fmt(ctx->sweepLabels[0], "%" PRIu32 " Hz", from);
        lv_label_set_text_fmt(ctx->sweepLabels[1], "%" PRIu32 " Hz", to);
        // Integer-only formatting, as app code can't use %f
        if (periodMs % 1000 == 0) {
            lv_label_set_text_fmt(ctx->sweepLabels[2], "%" PRIu32 " s", periodMs / 1000);
        } else {
            lv_label_set_text_fmt(ctx->sweepLabels[2], "%" PRIu32 ".%03" PRIu32 " s", periodMs / 1000, periodMs % 1000);
        }
    }

    drawPreview(ctx);
    updateReadout(ctx);
    updatePlayButton(ctx);
}

/* ── Typed values ─────────────────────────────────────────────────── */

/** Parses a number with up to three decimals into thousandths, e.g. "4.5" into 4500 */
bool parseThousandths(const char* text, uint32_t* out) {
    uint32_t whole = 0;
    uint32_t fraction = 0;
    uint32_t decimals = 0;
    bool point = false;
    bool any = false;
    for (const char* c = text; *c != '\0'; c++) {
        if (*c == ' ') continue;
        if (*c == '.' || *c == ',') {
            if (point) return false;
            point = true;
            continue;
        }
        if (*c < '0' || *c > '9') return false;
        any = true;
        if (point) {
            if (decimals == 3) return false;
            fraction = fraction * 10 + (*c - '0');
            decimals++;
        } else {
            whole = whole * 10 + (*c - '0');
            if (whole > 100000) return false;
        }
    }
    if (!any) return false;
    while (decimals++ < 3) fraction *= 10;
    *out = whole * 1000 + fraction;
    return true;
}

void onFrequencyEntered(bool confirmed, const char* text, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    uint32_t value = 0;
    if (!confirmed || !parseThousandths(text, &value) || value == 0) return;
    lvgl_lock();
    setFixedFrequency(ctx, value / 1000);
    lvgl_unlock();
}

void onSweepValueEntered(bool confirmed, const char* text, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    uint32_t value = 0;
    if (!confirmed || !parseThousandths(text, &value) || value == 0) return;
    lvgl_lock();
    if (ctx->editedSweepValue == SweepValue::Period) {
        ctx->playback.sweepPeriodMs.store(std::clamp<uint32_t>(value, 250, 600000));
    } else {
        auto& target = ctx->editedSweepValue == SweepValue::From ? ctx->playback.sweepMinHz : ctx->playback.sweepMaxHz;
        target.store(tone_synth::clamp_frequency(value / 1000));
    }
    syncControls(ctx);
    lvgl_unlock();
}

void showFrequencyDialog(Context* ctx) {
    char current[16];
    snprintf(current, sizeof(current), "%" PRIu32, ctx->playback.fixedHz.load());
    window_manager_input_dialog(ctx->appInstanceId, "Frequency", "20 to 20000 Hz", current, "OK", "Cancel", onFrequencyEntered, ctx);
}

/* ── Widget events ────────────────────────────────────────────────── */

void onExitPressed(lv_event_t* event) {
    app_event_emit_close(contextOf(event)->appInstanceId);
}

void onBackPressed(lv_event_t* event) {
    requestAction(contextOf(event), ACTION_BACK);
}

void onPlayPressed(lv_event_t* event) {
    auto* ctx = contextOf(event);

    // Serialize against main()'s close path: once `closing` is set, a late play press
    // must not start a task on a context that is about to be torn down.
    if (ctx->playbackGate == nullptr || xSemaphoreTake(ctx->playbackGate, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!ctx->closing) {
        if (ctx->playback.playing.load()) {
            tone_playback_stop(&ctx->playback);
            updatePlayButton(ctx);
            updateReadout(ctx);
        } else {
            const error_t result = tone_playback_start(&ctx->playback);
            updatePlayButton(ctx);
            updateReadout(ctx);
            if (result != ERROR_NONE) {
                lv_label_set_text_fmt(ctx->statusLabel, "Playback failed (%s)", error_to_string(result));
            }
        }
    }
    xSemaphoreGive(ctx->playbackGate);
}

/** Fine steps for low frequencies, larger ones further up */
void step(Context* ctx, int direction) {
    const uint32_t hz = ctx->playback.fixedHz.load();
    const uint32_t reference = direction > 0 ? hz : hz - 1;
    const uint32_t size = reference < 100 ? 1 : (reference < 1000 ? 10 : 100);
    setFixedFrequency(ctx, direction > 0 ? hz + size : (hz > size ? hz - size : 0));
}

void onLower(lv_event_t* event) {
    step(contextOf(event), -1);
}

void onHigher(lv_event_t* event) {
    step(contextOf(event), 1);
}

void onReadoutPressed(lv_event_t* event) {
    showFrequencyDialog(contextOf(event));
}

void onFrequencySlider(lv_event_t* event) {
    auto* ctx = contextOf(event);
    ctx->playback.sweep.store(false);
    ctx->playback.fixedHz.store(frequencyOfSlider(lv_slider_get_value(ctx->frequencySlider)));
    // Not syncControls(): it would move the slider that is being dragged
    for (uint32_t i = 0; i < TONE_PRESET_COUNT; i++) {
        setChecked(ctx->presetChips[i], PRESETS[i] == ctx->playback.fixedHz.load());
    }
    setChecked(ctx->fixedChip, true);
    setChecked(ctx->sweepChip, false);
    updateReadout(ctx);
}

void onWaveformPressed(lv_event_t* event) {
    auto* ctx = contextOf(event);
    ctx->playback.waveform.store(indexOf(event));
    syncControls(ctx);
}

void onPresetPressed(lv_event_t* event) {
    setFixedFrequency(contextOf(event), PRESETS[indexOf(event)]);
}

void onSweepModePressed(lv_event_t* event) {
    auto* ctx = contextOf(event);
    ctx->playback.sweep.store(indexOf(event) == 1);
    syncControls(ctx);
}

void onSweepSlider(lv_event_t* event) {
    auto* ctx = contextOf(event);
    const uint32_t index = indexOf(event);
    const int32_t value = lv_slider_get_value(ctx->sweepSliders[index]);
    if (index == static_cast<uint32_t>(SweepValue::Period)) {
        ctx->playback.sweepPeriodMs.store(static_cast<uint32_t>(value) * 1000);
        lv_label_set_text_fmt(ctx->sweepLabels[index], "%" PRId32 " s", value);
    } else {
        const uint32_t hz = frequencyOfSlider(value);
        (index == 0 ? ctx->playback.sweepMinHz : ctx->playback.sweepMaxHz).store(hz);
        lv_label_set_text_fmt(ctx->sweepLabels[index], "%" PRIu32 " Hz", hz);
    }
    updateReadout(ctx);
}

void onSweepValuePressed(lv_event_t* event) {
    auto* ctx = contextOf(event);
    ctx->editedSweepValue = static_cast<SweepValue>(indexOf(event));
    const bool period = ctx->editedSweepValue == SweepValue::Period;
    window_manager_input_dialog(ctx->appInstanceId, period ? "Sweep period" : "Sweep frequency",
        period ? "0.25 to 600 seconds" : "20 to 20000 Hz", "", "OK", "Cancel", onSweepValueEntered, ctx);
}

void onVolumeChanged(lv_event_t* event) {
    auto* ctx = contextOf(event);
    auto* box = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const int32_t percent = lvgl_sliderbox_get_value(box);
    ctx->volumePercent = static_cast<uint8_t>(percent);
    if (ctx->playback.streamDevice != nullptr) {
        audio_stream_set_volume(ctx->playback.streamDevice, AUDIO_CODEC_DIR_OUTPUT, static_cast<float>(percent));
    }
}

void onMenuChoice(int32_t item, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    if (item >= 0) requestAction(ctx, ACTION_MENU, item);
}

void onMenuPressed(lv_event_t* event) {
    auto* ctx = contextOf(event);
    const char* items[3];
    uint32_t count = 0;
    auto add = [&](MenuItem item, const char* label) {
        ctx->menuActions[count] = item;
        items[count] = label;
        count++;
    };
    if (!ctx->presetsShown) add(MENU_PRESETS, "Presets");
    if (ctx->compact) add(MENU_SWEEP, "Sweep");
    add(MENU_FREQUENCY, "Enter frequency");
    window_manager_selection_dialog(ctx->appInstanceId, "Tone", items, count, onMenuChoice, ctx);
}

void onPresetChoice(int32_t item, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    if (item >= 0) requestAction(ctx, ACTION_PRESET, item);
}

void onRefreshTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    updatePlayButton(ctx);
    updateReadout(ctx);
}

void onPreviewResized(lv_event_t* event) {
    drawPreview(contextOf(event));
}

/* ── Building ─────────────────────────────────────────────────────── */

lv_obj_t* createBox(lv_obj_t* parent, lv_flex_flow_t flow, int32_t gap) {
    auto* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(box, gap, 0);
    lv_obj_set_scrollable(box, false);
    return box;
}

lv_obj_t* createCard(lv_obj_t* parent, int32_t gap) {
    auto* card = lvgl_card_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, gap, 0);
    lv_obj_set_scrollable(card, false);
    return card;
}

lv_obj_t* createLabel(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t* createChip(Context* ctx, lv_obj_t* parent, const char* text, uint32_t index, lv_event_cb_t callback) {
    auto* chip = lvgl_chip_create(parent);
    lv_obj_center(createLabel(chip, text));
    setIndex(chip, index);
    lv_obj_add_event_cb(chip, callback, LV_EVENT_SHORT_CLICKED, ctx);
    return chip;
}

/**
 * For a row of chips with flex grow: the flex layout doesn't subtract their margins from the space
 * that it divides, so the row's padding takes over from the margins.
 */
void fitGrowingChips(lv_obj_t* row, lv_obj_t* const* chips, uint32_t count, int32_t gap) {
    lv_obj_update_layout(row);
    // Where the first chip starts is its margin
    const int32_t margin = lv_obj_get_x(chips[0]);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_set_style_margin_hor(chips[i], 0, 0);
    }
    lv_obj_set_style_pad_hor(row, margin, 0);
    lv_obj_set_style_pad_column(row, gap + 2 * margin, 0);
}

lv_obj_t* createStepButton(Context* ctx, lv_obj_t* parent, const char* text, lv_event_cb_t callback) {
    auto* button = lv_button_create(parent);
    const int32_t size = lvgl_get_text_font_height(FONT_SIZE_DEFAULT) * 2;
    lv_obj_set_size(button, size, size);
    lv_obj_center(createLabel(button, text));
    lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, ctx);
    lv_obj_add_event_cb(button, callback, LV_EVENT_LONG_PRESSED_REPEAT, ctx);
    return button;
}

void createReadout(Context* ctx, lv_obj_t* parent, int32_t gap) {
    auto* row = createBox(parent, LV_FLEX_FLOW_ROW, gap);
    createStepButton(ctx, row, LV_SYMBOL_MINUS, onLower);

    // Tapping the frequency types an exact value
    auto* texts = createBox(row, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(texts, 0);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_clickable(texts, true);
    lv_obj_add_event_cb(texts, onReadoutPressed, LV_EVENT_SHORT_CLICKED, ctx);
    ctx->frequencyLabel = createLabel(texts, "");
    lv_obj_set_style_text_font(ctx->frequencyLabel, lvgl_get_text_font(FONT_SIZE_LARGE), 0);
    ctx->statusLabel = createLabel(texts, "");
    lv_obj_set_style_text_font(ctx->statusLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(ctx->statusLabel, LV_OPA_70, 0);

    createStepButton(ctx, row, LV_SYMBOL_PLUS, onHigher);
}

lv_obj_t* createSlider(Context* ctx, lv_obj_t* parent, int32_t min, int32_t max, lv_event_cb_t callback) {
    auto* slider = lv_slider_create(parent);
    lv_slider_set_range(slider, min, max);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, ctx);
    return slider;
}

void createFrequencySlider(Context* ctx, lv_obj_t* parent) {
    // The padding keeps the knob inside the screen
    auto* box = createBox(parent, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_pad_hor(box, 10, 0);
    lv_obj_set_style_pad_ver(box, 6, 0);
    ctx->frequencySlider = createSlider(ctx, box, 0, SLIDER_STEPS, onFrequencySlider);
    lv_obj_set_flex_grow(ctx->frequencySlider, 1);
}

/** @return the container, which is hidden when there's no room for it */
lv_obj_t* createPreview(Context* ctx, lv_obj_t* parent) {
    auto* card = lvgl_card_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    // Takes the height that is left
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_min_height(card, lvgl_get_text_font_height(FONT_SIZE_DEFAULT) * 2, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_set_scrollable(card, false);
    ctx->previewLine = lv_line_create(card);
    lv_obj_set_style_line_width(ctx->previewLine, 2, 0);
    lv_obj_set_style_line_color(ctx->previewLine, lv_theme_get_color_primary(card), 0);
    lv_obj_add_event_cb(card, onPreviewResized, LV_EVENT_SIZE_CHANGED, ctx);
    return card;
}

bool isWrapped(Context* ctx, lv_obj_t* row) {
    lv_obj_update_layout(row);
    return lv_obj_get_y(ctx->waveformChips[TONE_WAVEFORM_COUNT - 1]) > lv_obj_get_y(ctx->waveformChips[0]);
}

/** The chips are as wide as their text. When they don't fit on one line, they get smaller, and then wrap. */
void createWaveforms(Context* ctx, lv_obj_t* parent, int32_t gap) {
    // No flex grow: it would shrink the chips' share below their text, and push the last ones off screen
    auto* row = createBox(parent, LV_FLEX_FLOW_ROW_WRAP, gap);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    for (uint32_t i = 0; i < TONE_WAVEFORM_COUNT; i++) {
        ctx->waveformChips[i] = createChip(ctx, row, WAVEFORM_NAMES[i], i, onWaveformPressed);
    }
    if (isWrapped(ctx, row)) {
        lv_obj_set_style_pad_column(row, 2, 0);
        for (auto* chip : ctx->waveformChips) {
            lv_obj_set_style_pad_left(chip, 4, 0);
            lv_obj_set_style_pad_right(chip, 4, 0);
            lv_obj_set_style_text_font(chip, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
        }
    }
}

lv_obj_t* createPresets(Context* ctx, lv_obj_t* parent, int32_t gap) {
    auto* card = createCard(parent, gap);
    createLabel(card, "Presets (Hz)");
    for (uint32_t i = 0; i < TONE_PRESET_COUNT; i += 4) {
        auto* row = createBox(card, LV_FLEX_FLOW_ROW, gap);
        for (uint32_t j = i; j < i + 4; j++) {
            ctx->presetChips[j] = createChip(ctx, row, PRESET_NAMES[j], j, onPresetPressed);
            lv_obj_set_flex_grow(ctx->presetChips[j], 1);
        }
        fitGrowingChips(row, &ctx->presetChips[i], 4, gap);
    }
    return card;
}

void createSweep(Context* ctx, lv_obj_t* parent, int32_t gap) {
    auto* card = createCard(parent, gap);
    auto* modes = createBox(card, LV_FLEX_FLOW_ROW, gap);
    ctx->fixedChip = createChip(ctx, modes, "Fixed", 0, onSweepModePressed);
    lv_obj_set_flex_grow(ctx->fixedChip, 1);
    ctx->sweepChip = createChip(ctx, modes, "Sweep", 1, onSweepModePressed);
    lv_obj_set_flex_grow(ctx->sweepChip, 1);
    lv_obj_t* modeChips[] = { ctx->fixedChip, ctx->sweepChip };
    fitGrowingChips(modes, modeChips, 2, gap);

    for (uint32_t i = 0; i < 3; i++) {
        auto* row = createBox(card, LV_FLEX_FLOW_ROW, gap + 8);
        lv_obj_set_style_pad_right(row, 2, 0);
        createLabel(row, i == 0 ? "From" : (i == 1 ? "To" : "Period"));
        const bool period = i == static_cast<uint32_t>(SweepValue::Period);
        ctx->sweepSliders[i] = createSlider(ctx, row, period ? 1 : 0, period ? MAX_PERIOD_SLIDER_S : SLIDER_STEPS, onSweepSlider);
        setIndex(ctx->sweepSliders[i], i);
        lv_obj_set_flex_grow(ctx->sweepSliders[i], 1);
        // Tapping the value types an exact one
        auto* value = lv_button_create(row);
        setIndex(value, i);
        lv_obj_add_event_cb(value, onSweepValuePressed, LV_EVENT_SHORT_CLICKED, ctx);
        ctx->sweepLabels[i] = createLabel(value, "");
    }
}

void createVolume(Context* ctx, lv_obj_t* parent, int32_t gap) {
    auto* row = createBox(parent, LV_FLEX_FLOW_ROW, gap + 4);
    createLabel(row, "Volume");
    auto* box = lvgl_sliderbox_create(row, 0, 100, 5, ctx->volumePercent);
    lv_obj_set_width(box, 0);
    lv_obj_set_flex_grow(box, 1);
    lvgl_sliderbox_add_value_changed_cb(box, onVolumeChanged, ctx);
}

void createPlayButton(Context* ctx, lv_obj_t* parent, bool large) {
    ctx->playButton = lv_button_create(parent);
    lv_obj_set_width(ctx->playButton, LV_PCT(100));
    if (large) lv_obj_set_height(ctx->playButton, lvgl_get_text_font_height(FONT_SIZE_DEFAULT) * 3);
    lv_obj_add_event_cb(ctx->playButton, onPlayPressed, LV_EVENT_SHORT_CLICKED, ctx);
    ctx->playLabel = createLabel(ctx->playButton, "");
    lv_obj_center(ctx->playLabel);
}

lv_obj_t* createColumn(lv_obj_t* parent, int32_t gap) {
    auto* column = createBox(parent, LV_FLEX_FLOW_COLUMN, gap);
    lv_obj_set_height(column, LV_PCT(100));
    lv_obj_set_style_pad_all(column, gap, 0);
    lv_obj_set_scrollable(column, true);
    return column;
}

bool overflows(lv_obj_t* column) {
    lv_obj_update_layout(column);
    return lv_obj_get_scroll_bottom(column) > 0;
}

void clearWidgetPointers(Context* ctx) {
    ctx->frequencyLabel = nullptr;
    ctx->statusLabel = nullptr;
    ctx->frequencySlider = nullptr;
    ctx->playButton = nullptr;
    ctx->playLabel = nullptr;
    for (auto*& chip : ctx->waveformChips) chip = nullptr;
    for (auto*& chip : ctx->presetChips) chip = nullptr;
    ctx->fixedChip = nullptr;
    ctx->sweepChip = nullptr;
    for (auto*& slider : ctx->sweepSliders) slider = nullptr;
    for (auto*& label : ctx->sweepLabels) label = nullptr;
    ctx->previewLine = nullptr;
}

/**
 * Large landscape screens have two columns, large portrait screens one with everything.
 * Smaller screens have the sweep settings on their own page. When a page still doesn't fit,
 * the presets are removed first, then the wave preview.
 */
void rebuild(Context* ctx) {
    lv_obj_t* root = ctx->root;
    if (root == nullptr) return;
    clearWidgetPointers(ctx);
    lv_obj_clean(root);

    auto* toolbar = lvgl_toolbar_create(root, "Tone");

    auto* body = createBox(root, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_update_layout(root);

    const int32_t width = lv_obj_get_content_width(body);
    const int32_t height = lv_obj_get_content_height(body);
    const int32_t gap = height < 260 ? 4 : 8;
    const bool wide = width >= 600 && height >= 300 && width > height;
    const bool tall = !wide && height >= 560;
    ctx->compact = !wide && !tall;
    if (!ctx->compact) ctx->sweepPage = false;

    if (ctx->sweepPage) {
        lvgl_toolbar_set_title(toolbar, "Sweep");
        lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_LEFT, onBackPressed, ctx);
        auto* column = createColumn(body, gap);
        createSweep(ctx, column, gap);
        createPlayButton(ctx, column, false);
        syncControls(ctx);
        return;
    }
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onExitPressed, ctx);

    lv_obj_t* column = createColumn(body, gap);
    lv_obj_t* side = nullptr;
    lv_obj_t* presets = nullptr;
    lv_obj_t* preview = nullptr;

    createReadout(ctx, column, gap);
    createFrequencySlider(ctx, column);
    if (wide) {
        // Left: what is watched and touched while listening. Right: the settings.
        lv_obj_set_width(column, 0);
        lv_obj_set_flex_grow(column, 1);
        preview = createPreview(ctx, column);
        createWaveforms(ctx, column, gap);
        createPlayButton(ctx, column, true);

        side = createColumn(body, gap);
        lv_obj_set_width(side, LV_PCT(45));
        presets = createPresets(ctx, side, gap);
        createSweep(ctx, side, gap);
        createVolume(ctx, side, gap);
    } else if (tall) {
        preview = createPreview(ctx, column);
        createWaveforms(ctx, column, gap);
        presets = createPresets(ctx, column, gap);
        createSweep(ctx, column, gap);
        createVolume(ctx, column, gap);
        createPlayButton(ctx, column, true);
    } else {
        createWaveforms(ctx, column, gap);
        presets = createPresets(ctx, column, gap);
        preview = createPreview(ctx, column);
        createVolume(ctx, column, gap);
        createPlayButton(ctx, column, false);
    }

    // Presets go first, then the preview
    ctx->presetsShown = true;
    if (overflows(side != nullptr ? side : column)) {
        lv_obj_set_hidden(presets, true);
        ctx->presetsShown = false;
    }
    if (overflows(column)) {
        lv_obj_set_hidden(presets, true);
        ctx->presetsShown = false;
        if (overflows(column)) {
            lv_obj_set_hidden(preview, true);
        }
    }

    // The menu has what isn't on this page
    if (ctx->compact || !ctx->presetsShown) {
        auto* menu = lvgl_toolbar_add_text_button_action(toolbar, LVGL_ICON_SHARED_MORE_VERT, onMenuPressed, ctx);
        lv_obj_set_style_text_font(menu, lvgl_get_shared_icon_default_font(), 0);
    }

    lv_obj_update_layout(root);
    syncControls(ctx);
}

} // namespace

void toneRunAction(Context* ctx) {
    const uint32_t pending = ctx->action.exchange(ACTION_NONE);
    const int32_t action = static_cast<int32_t>(pending & 0xFF);
    const int32_t value = static_cast<int32_t>(pending >> 8);

    lvgl_lock();
    if (action == ACTION_BACK) {
        ctx->sweepPage = false;
        rebuild(ctx);
    } else if (action == ACTION_PRESET && value < static_cast<int32_t>(TONE_PRESET_COUNT)) {
        setFixedFrequency(ctx, PRESETS[value]);
    } else if (action == ACTION_MENU && ctx->root != nullptr && value < 3) {
        switch (ctx->menuActions[value]) {
            case MENU_PRESETS: {
                const char* items[TONE_PRESET_COUNT];
                for (uint32_t i = 0; i < TONE_PRESET_COUNT; i++) items[i] = PRESET_NAMES[i];
                window_manager_selection_dialog(ctx->appInstanceId, "Presets (Hz)", items, TONE_PRESET_COUNT, onPresetChoice, ctx);
                break;
            }
            case MENU_SWEEP:
                ctx->sweepPage = true;
                rebuild(ctx);
                break;
            case MENU_FREQUENCY:
                showFrequencyDialog(ctx);
                break;
        }
    }
    lvgl_unlock();
}

void toneCreateWidgets(lv_obj_t* root, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

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

    ctx->root = root;
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 0, 0);
    lv_obj_set_scrollable(root, false);
    rebuild(ctx);

    ctx->refreshTimer = lv_timer_create(onRefreshTimer, REFRESH_PERIOD_MS, ctx);
}

void toneDestroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    if (ctx->refreshTimer != nullptr) {
        lv_timer_delete(ctx->refreshTimer);
        ctx->refreshTimer = nullptr;
    }
    ctx->root = nullptr;
    clearWidgetPointers(ctx);
}
