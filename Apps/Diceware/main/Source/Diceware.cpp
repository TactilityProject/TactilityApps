#include "Diceware.h"

#include <app/paths.h>
#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/dialog.h>

#include <esp_log.h>
#include <esp_random.h>

#include <cstdio>

static constexpr auto* TAG = "Diceware";

/** Must match manifest.properties' app.id */
static constexpr auto* APP_ID = "tactility.diceware";

/** The EFF large word list has one word for every roll of five dice */
static constexpr uint32_t WORD_LIST_SIZE = 7776;

static std::string readWordAtLine(const uint32_t lineIndex) {
    char path[256];
    if (app_paths_get_assets_path(APP_ID, "eff_large_wordlist.txt", path, sizeof(path)) != ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to get assets path");
        return "";
    }

    std::string word;
    FILE* file = fopen(path, "r");
    if (file != nullptr) {
        char c;
        uint32_t line = 0;
        while (line < lineIndex && fread(&c, 1, 1, file)) {
            if (c == '\n') line++;
        }
        while (fread(&c, 1, 1, file) && c != '\n') {
            if (c != '\r') word += c;
        }
        fclose(file);
    } else {
        ESP_LOGE(TAG, "Failed to open %s", path);
    }
    return word;
}

/** Roughly how long it takes to find the phrase, searching half of all of them at a trillion per second */
static const char* crackTime(const uint32_t wordCount, char* buffer, size_t size) {
    double years = 0.5 / 1e12 / 3.15e7;
    for (uint32_t i = 0; i < wordCount; i++) {
        years *= WORD_LIST_SIZE;
    }
    if (years < 1.0 / 365) {
        snprintf(buffer, size, "under a day");
    } else if (years < 1) {
        snprintf(buffer, size, "%d days", static_cast<int>(years * 365));
    } else if (years < 1e3) {
        snprintf(buffer, size, "%d years", static_cast<int>(years));
    } else if (years < 1e6) {
        snprintf(buffer, size, "%d thousand years", static_cast<int>(years / 1e3));
    } else {
        snprintf(buffer, size, "over a million years");
    }
    return buffer;
}

/** "5 words", or just "5" when the footer has no room for that. Requires the LVGL lock. */
static void updateCountLabel(Context* ctx) {
    auto* footer = lv_obj_get_parent(ctx->countLabel);
    lv_label_set_text_fmt(ctx->countLabel, "%u words", static_cast<unsigned>(ctx->wordCount));
    lv_obj_update_layout(footer);

    // Roll is the last button, and gets the space that is left
    auto* roll = lv_obj_get_child(footer, -1);
    // The text with some room on both sides. The padding getters aren't exported to apps.
    const int32_t needed = lv_obj_get_width(lv_obj_get_child(roll, 0)) + LV_DPX(16);
    if (lv_obj_get_width(roll) < needed) {
        lv_label_set_text_fmt(ctx->countLabel, "%u", static_cast<unsigned>(ctx->wordCount));
    }
}

/** Requires the LVGL lock */
static void updateWidgets(Context* ctx) {
    if (ctx->phraseLabel == nullptr) return;

    std::string phrase;
    for (const auto& word : ctx->words) {
        if (!phrase.empty()) phrase += ' ';
        phrase += word;
    }
    if (ctx->failed) {
        phrase = "Can't read the word list";
    } else if (ctx->busy) {
        phrase = "Rolling...";
    }
    lv_label_set_text(ctx->phraseLabel, phrase.c_str());

    updateCountLabel(ctx);

    const lv_color_t on = lv_theme_get_color_primary(ctx->phraseLabel);
    for (uint32_t i = 0; i < METER_SEGMENTS; i++) {
        const bool lit = i <= ctx->wordCount - MIN_WORDS;
        lv_obj_set_style_bg_color(ctx->meter[i], on, 0);
        lv_obj_set_style_bg_opa(ctx->meter[i], lit ? LV_OPA_COVER : LV_OPA_20, 0);
    }

    // log2(7776) = 12.925 bits per word
    char time[32];
    lv_label_set_text_fmt(ctx->strengthLabel, "%u bits - %s to crack at 1 trillion guesses/s",
        static_cast<unsigned>(ctx->wordCount * 12925 / 1000), crackTime(ctx->wordCount, time, sizeof(time)));
}

static void jobMain(Context* ctx) {
    lvgl_lock();
    const size_t missing = ctx->wordCount - ctx->words.size();
    lvgl_unlock();

    std::vector<std::string> picked;
    for (size_t i = 0; i < missing; i++) {
        auto word = readWordAtLine(esp_random() % WORD_LIST_SIZE);
        if (word.empty()) break;
        picked.push_back(word);
    }

    lvgl_lock();
    ctx->failed = picked.size() != missing;
    for (auto& word : picked) {
        ctx->words.push_back(word);
    }
    ctx->busy = false;
    updateWidgets(ctx);
    lvgl_unlock();
}

static void cleanupJob(Context* ctx) {
    if (ctx->jobThread != nullptr) {
        ctx->jobThread->join();
        ctx->jobThread = nullptr;
    }
}

/** Picks words until there are wordCount of them. Requires the LVGL lock, and no job that is busy. */
static void startJob(Context* ctx) {
    // The previous job has finished, as it's not busy
    cleanupJob(ctx);

    ctx->busy = true;
    ctx->failed = false;
    updateWidgets(ctx);
    ctx->jobThread = std::make_unique<tt::Thread>("Diceware", 4096, [ctx] {
        jobMain(ctx);
        return 0;
    });
    ctx->jobThread->start();
}

/** Keeps the words there are: more words are added to them, fewer are taken off the end */
static void changeWordCount(Context* ctx, int change) {
    const uint32_t count = ctx->wordCount + change;
    if (ctx->busy || count < MIN_WORDS || count > MAX_WORDS) return;

    ctx->wordCount = count;
    if (ctx->words.size() > count) {
        ctx->words.resize(count);
        updateWidgets(ctx);
    } else {
        startJob(ctx);
    }
}

static void onFewerClicked(lv_event_t* e) {
    changeWordCount(static_cast<Context*>(lv_event_get_user_data(e)), -1);
}

static void onMoreClicked(lv_event_t* e) {
    changeWordCount(static_cast<Context*>(lv_event_get_user_data(e)), 1);
}

static void onRollClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx->busy) return;
    ctx->words.clear();
    startJob(ctx);
}

static void onHelpClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    static const char* const buttons[] = { "OK" };
    window_manager_alert_dialog(ctx->appInstanceId, "Diceware Info",
        "The hardware random number generator can use the Wi-Fi radio to improve randomness. There's no need to connect to a Wi-Fi network for this to work.",
        buttons, 1, nullptr, nullptr);
}

static lv_obj_t* createRow(lv_obj_t* parent) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_scrollable(row, false);
    return row;
}

static lv_obj_t* createButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, Context* ctx) {
    auto* button = lv_button_create(parent);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, ctx);
    return button;
}

void dicewareCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_scrollable(parent, false);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 4, 0);

    auto* toolbar = lvgl_toolbar_create(parent, "Diceware");
    lvgl_toolbar_add_text_button_action(toolbar, "?", onHelpClicked, ctx);

    // The phrase takes the space that is left, and scrolls when it doesn't fit
    auto* phrase_area = lv_obj_create(parent);
    lv_obj_remove_style_all(phrase_area);
    lv_obj_set_width(phrase_area, LV_PCT(100));
    lv_obj_set_flex_grow(phrase_area, 1);
    lv_obj_set_flex_flow(phrase_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(phrase_area, 8, 0);
    // Centred while it fits, at the top when it scrolls
    lv_obj_set_flex_align(phrase_area, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const bool small = lv_display_get_vertical_resolution(nullptr) < 200;
    ctx->phraseLabel = lv_label_create(phrase_area);
    lv_obj_set_width(ctx->phraseLabel, LV_PCT(100));
    lv_label_set_long_mode(ctx->phraseLabel, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ctx->phraseLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ctx->phraseLabel, lvgl_get_text_font(small ? FONT_SIZE_DEFAULT : FONT_SIZE_LARGE), 0);

    auto* meter = createRow(parent);
    lv_obj_set_style_pad_column(meter, 2, 0);
    for (auto& segment : ctx->meter) {
        segment = lv_obj_create(meter);
        lv_obj_remove_style_all(segment);
        lv_obj_set_height(segment, 5);
        lv_obj_set_flex_grow(segment, 1);
        lv_obj_set_style_radius(segment, 2, 0);
    }

    ctx->strengthLabel = lv_label_create(parent);
    lv_obj_set_width(ctx->strengthLabel, LV_PCT(100));
    lv_label_set_long_mode(ctx->strengthLabel, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ctx->strengthLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ctx->strengthLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(ctx->strengthLabel, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(ctx->strengthLabel, 8, 0);

    auto* footer = createRow(parent);
    lv_obj_set_style_pad_column(footer, 6, 0);
    lv_obj_set_style_pad_bottom(footer, 6, 0);
    createButton(footer, LV_SYMBOL_MINUS, onFewerClicked, ctx);
    ctx->countLabel = lv_label_create(footer);
    createButton(footer, LV_SYMBOL_PLUS, onMoreClicked, ctx);
    auto* roll = createButton(footer, "Roll", onRollClicked, ctx);
    lv_obj_set_flex_grow(roll, 1);

    // A phrase is shown right away, and kept when the window is rebuilt
    if (!ctx->busy && ctx->words.empty() && !ctx->failed) {
        startJob(ctx);
    } else {
        updateWidgets(ctx);
    }
}

void dicewareDestroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->phraseLabel = nullptr;
    ctx->strengthLabel = nullptr;
    ctx->countLabel = nullptr;
    for (auto& segment : ctx->meter) {
        segment = nullptr;
    }
}

void dicewareTeardown(Context* ctx) {
    cleanupJob(ctx);
}
