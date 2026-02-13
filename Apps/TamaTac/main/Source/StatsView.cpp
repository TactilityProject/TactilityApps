/**
 * @file StatsView.cpp
 * @brief Stats detail view implementation
 */

#include "StatsView.h"
#include "TamaTac.h"
#include <cstdio>

lv_obj_t* StatsView::createStatRow(lv_obj_t* parentContainer, const char* labelText, lv_color_t color, bool isXLarge) {
    lv_obj_t* row = lv_obj_create(parentContainer);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_right(row, isXLarge ? 20 : 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, labelText);
    lv_obj_set_style_text_color(label, lv_palette_lighten(LV_PALETTE_GREY, 5), 0);

    lv_obj_t* value = lv_label_create(row);
    lv_label_set_text(value, "--");
    lv_obj_set_style_text_color(value, color, 0);

    return value;
}

void StatsView::onStart(lv_obj_t* parentWidget, TamaTac* appInstance) {
    parent = parentWidget;
    app = appInstance;

    // Detect screen size for responsive layout
    // Use display resolution for reliable sizing (parent may not be laid out yet on first load)
    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

    // Scaled dimensions
    int padAll = isSmall ? 6 : (isXLarge ? 24 : 12);
    int padRowVal = isSmall ? 4 : (isXLarge ? 16 : 8);

    // Main content wrapper
    mainWrapper = lv_obj_create(parent);
    lv_obj_set_size(mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(mainWrapper, padAll, 0);
    lv_obj_set_style_pad_row(mainWrapper, padRowVal, 0);
    lv_obj_set_style_bg_opa(mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mainWrapper, 0, 0);
    lv_obj_set_flex_flow(mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Title row (stage/age on left, status on right)
    lv_obj_t* titleRow = lv_obj_create(mainWrapper);
    lv_obj_set_width(titleRow, LV_PCT(100));
    lv_obj_set_height(titleRow, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(titleRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(titleRow, 0, 0);
    lv_obj_set_style_pad_all(titleRow, 0, 0);
    lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    titleLabel = lv_label_create(titleRow);
    lv_label_set_text(titleLabel, "Egg | 0d 0h");
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xFFFFFF), 0);

    statusLabel = lv_label_create(titleRow);
    lv_label_set_text(statusLabel, "");
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(0xFFFFFF), 0);

    // Personality row
    personalityValue = createStatRow(mainWrapper, "Personality", lv_palette_main(LV_PALETTE_PURPLE), isXLarge);

    // Create stat rows with colored values
    hungerValue = createStatRow(mainWrapper, "Hunger", lv_color_hex(0xFF9900), isXLarge);
    happyValue = createStatRow(mainWrapper, "Happy", lv_color_hex(0xFFCC00), isXLarge);
    healthValue = createStatRow(mainWrapper, "Health", lv_color_hex(0x00FF00), isXLarge);
    energyValue = createStatRow(mainWrapper, "Energy", lv_color_hex(0x00CCFF), isXLarge);
    cleanValue = createStatRow(mainWrapper, "Clean", lv_color_hex(0xFFFFFF), isXLarge);
}

void StatsView::onStop() {
    mainWrapper = nullptr;
    titleLabel = nullptr;
    statusLabel = nullptr;
    hungerValue = nullptr;
    happyValue = nullptr;
    healthValue = nullptr;
    energyValue = nullptr;
    cleanValue = nullptr;
    personalityValue = nullptr;
    parent = nullptr;
    app = nullptr;
}

void StatsView::updateStats(PetLogic* petLogic) {
    if (petLogic == nullptr || titleLabel == nullptr) return;
    if (hungerValue == nullptr || happyValue == nullptr ||
        healthValue == nullptr || energyValue == nullptr ||
        cleanValue == nullptr || statusLabel == nullptr) return;

    const PetStats& stats = petLogic->getStats();

    // Update title with stage and age
    int hours = stats.ageHours;
    int days = hours / 24;
    hours = hours % 24;

    // Update title (stage and age)
    char titleText[64];
    snprintf(titleText, sizeof(titleText), "%s | %dd %dh", lifeStageToString(stats.stage), days, hours);
    lv_label_set_text(titleLabel, titleText);

    // Update status label (right-aligned)
    const char* status = "";
    if (stats.isDead) status = "[DEAD]";
    else if (stats.isSick) status = "[SICK]";
    else if (stats.isAsleep) status = "[SLEEPING]";
    lv_label_set_text(statusLabel, status);

    // Update personality
    if (personalityValue) {
        lv_label_set_text(personalityValue, personalityToString(stats.personality));
    }

    // Update individual stat values
    char valueText[16];

    snprintf(valueText, sizeof(valueText), "%d%%", stats.hunger);
    lv_label_set_text(hungerValue, valueText);

    snprintf(valueText, sizeof(valueText), "%d%%", stats.happiness);
    lv_label_set_text(happyValue, valueText);

    snprintf(valueText, sizeof(valueText), "%d%%", stats.health);
    lv_label_set_text(healthValue, valueText);

    snprintf(valueText, sizeof(valueText), "%d%%", stats.energy);
    lv_label_set_text(energyValue, valueText);

    snprintf(valueText, sizeof(valueText), "%d%%", stats.cleanliness);
    lv_label_set_text(cleanValue, valueText);
}
