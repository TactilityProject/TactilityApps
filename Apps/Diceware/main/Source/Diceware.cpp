#include "Diceware.h"

#include <tt_app_alertdialog.h>
#include <tactility/filesystem/file_mutex.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/toolbar.h>

#include <esp_random.h>
#include <esp_log.h>

#include <Tactility/kernel/Kernel.h>

constexpr char* TAG = "Diceware";

static void skipNewlines(FILE* file, const int count) {
    char c;
    int count_in_file = 0;
    while (count_in_file < count && fread(&c, 1, 1, file)) {
        if (c == '\n') {
            count_in_file++;
        }
    }
}

static std::string readWord(FILE* file) {
    char c;
    std::string result;
    // Read word until newline
    while (fread(&c, 1, 1, file) && c != '\n') { result += c; }
    return result;
}

static std::string readWordAtLine(const AppHandle handle, const int lineIndex) {
    char path[256];
    size_t size = 256;
    tt_app_get_assets_child_path(handle, "eff_large_wordlist.txt", path, &size);
    if (size == 0) {
        ESP_LOGE(TAG, "Failed to get assets path");
        return "";
    }

    struct FileMutex mutex;
    file_mutex_get(&mutex, path);
    std::string word;
    file_mutex_lock(&mutex);
    FILE* file = fopen(path, "r");
    if (file != nullptr) {
        skipNewlines(file, lineIndex);
        word = readWord(file);
        fclose(file);
    } else { ESP_LOGE(TAG, "Failed to open %s", path); }
    file_mutex_unlock(&mutex);
    return word;
}

int32_t Diceware::jobMain() {
    std::string result;
    for (int i = 0; i < wordCount; i++) {
        constexpr int line_count = 7776;
        const auto line_index = esp_random() % line_count;
        auto word = readWordAtLine(handle, line_index);
        result += word;
        result += " ";
    }

    onFinishJob(result);

    return 0;
}

void Diceware::cleanupJob() {
    if (jobThread != nullptr) {
        jobThread->join();
        jobThread = nullptr;
    }
}

void Diceware::startJob(uint32_t jobWordCount) {
    cleanupJob();

    wordCount = jobWordCount;
    jobThread = std::make_unique<tt::Thread>("Diceware", 4096, [this] {
        return jobMain();
    });
    jobThread->start();
}

void Diceware::onFinishJob(std::string result) {
    lvgl_lock();
    lv_label_set_text(resultLabel, result.c_str());
    lvgl_unlock();
}

void Diceware::onClickGenerate(lv_event_t* e) {
    auto* application = static_cast<Diceware*>(lv_event_get_user_data(e));
    auto* spinbox = application->spinbox;

    lv_label_set_text(application->resultLabel, "Generating...");

    const auto word_count = lv_spinbox_get_value(spinbox);
    application->startJob(word_count);
}

void Diceware::onSpinboxDecrement(lv_event_t* e) {
    auto* spinbox = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_spinbox_decrement(spinbox);
}

void Diceware::onSpinboxIncrement(lv_event_t* e) {
    auto* spinbox = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_spinbox_increment(spinbox);
}

void Diceware::onHelpClicked(lv_event_t* e) {
    const char* buttons[] = { "OK" };
    tt_app_alertdialog_start("Diceware Info", "The hardware random number generator can use the Wi-Fi radio to improve randomness. There's no need to connect to a Wi-Fi network for this to work.", buttons, 1);
}

void Diceware::onShow(AppHandle appHandle, lv_obj_t* parent) {
    handle = appHandle;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    auto* toolbar = lvgl_toolbar_create(parent, "Diceware");
    lvgl_toolbar_add_text_button_action(toolbar, "?", onHelpClicked, nullptr);

    auto* wrapper = lv_obj_create(parent);
    lv_obj_set_style_border_width(wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_width(wrapper, LV_PCT(100));
    lv_obj_set_flex_grow(wrapper, 1);

    auto* top_row_wrapper = lv_obj_create(wrapper);
    lv_obj_set_style_border_width(top_row_wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_size(top_row_wrapper, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(top_row_wrapper, 0, LV_STATE_DEFAULT);

    auto* generate_button = lv_button_create(top_row_wrapper);
    lv_obj_align(generate_button, LV_ALIGN_LEFT_MID, 0, 0);
    auto* generate_label = lv_label_create(generate_button);
    lv_label_set_text(generate_label, "Generate");
    lv_obj_add_event_cb(generate_button, onClickGenerate, LV_EVENT_SHORT_CLICKED, this);

    spinbox = lv_spinbox_create(top_row_wrapper);
    lv_spinbox_set_range(spinbox, 4, 12);
    lv_spinbox_set_value(spinbox, 5);
    lv_spinbox_set_step(spinbox, 1);
    lv_spinbox_set_digit_format(spinbox, 1, 0);
    lv_obj_set_width(spinbox, 30);

    auto* spinbox_dec_button = lv_button_create(top_row_wrapper);
    lv_obj_set_style_bg_image_src(spinbox_dec_button, LV_SYMBOL_MINUS, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(spinbox_dec_button, onSpinboxDecrement, LV_EVENT_SHORT_CLICKED, spinbox);
    lv_obj_set_style_pad_all(spinbox_dec_button, 16, LV_STATE_DEFAULT);

    auto* spinbox_inc_button = lv_button_create(top_row_wrapper);
    // lv_obj_align_to(spinbox_inc_button, spinbox, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
    lv_obj_set_style_bg_image_src(spinbox_inc_button, LV_SYMBOL_PLUS, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(spinbox_inc_button, onSpinboxIncrement, LV_EVENT_SHORT_CLICKED, spinbox);
    lv_obj_set_style_pad_all(spinbox_inc_button, 16, LV_STATE_DEFAULT);

    // Align spinbox widgets
    lv_obj_align(spinbox_inc_button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_align_to(spinbox, spinbox_inc_button, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    lv_obj_align_to(spinbox_dec_button, spinbox, LV_ALIGN_OUT_LEFT_MID, -5, 0);

    auto* result_wrapper = lv_obj_create(wrapper);
    lv_obj_set_flex_grow(result_wrapper, 1);
    lv_obj_set_width(result_wrapper, LV_PCT(100));
    lv_obj_set_style_pad_all(result_wrapper, 0, LV_STATE_DEFAULT);

    resultLabel = lv_label_create(result_wrapper);
    // See https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/random.html
    lv_label_set_text(resultLabel, "Press Generate button\nWi-Fi improves randomness.\nSee info button.");
    lv_label_set_long_mode(resultLabel, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_size(resultLabel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_align(resultLabel, LV_ALIGN_CENTER);
    lv_obj_set_style_text_align(resultLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
}

void Diceware::onHide(AppHandle context) {
    cleanupJob();
}
