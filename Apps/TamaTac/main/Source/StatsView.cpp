/**
 * @file StatsView.cpp
 * @brief Stats detail view implementation
 */

#include "StatsView.h"
#include "TamaTac.h"
#include <cstdio>

namespace {

lv_obj_t* createStatRow(lv_obj_t* parentContainer, const char* labelText, lv_color_t color, bool isXLarge) {
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

} // namespace

void statsViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx) {
    StatsViewState* state = &ctx->statsView;

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
    state->mainWrapper = lv_obj_create(parentWidget);
    lv_obj_set_size(state->mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(state->mainWrapper, padAll, 0);
    lv_obj_set_style_pad_row(state->mainWrapper, padRowVal, 0);
    lv_obj_set_style_bg_opa(state->mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->mainWrapper, 0, 0);
    lv_obj_set_flex_flow(state->mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(state->mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(state->mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Title row (stage/age on left, status on right)
    lv_obj_t* titleRow = lv_obj_create(state->mainWrapper);
    lv_obj_set_width(titleRow, LV_PCT(100));
    lv_obj_set_height(titleRow, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(titleRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(titleRow, 0, 0);
    lv_obj_set_style_pad_all(titleRow, 0, 0);
    lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    state->titleLabel = lv_label_create(titleRow);
    lv_label_set_text(state->titleLabel, "Egg | 0d 0h");
    lv_obj_set_style_text_color(state->titleLabel, lv_color_hex(0xFFFFFF), 0);

    state->statusLabel = lv_label_create(titleRow);
    lv_label_set_text(state->statusLabel, "");
    lv_obj_set_style_text_color(state->statusLabel, lv_color_hex(0xFFFFFF), 0);

    // Personality row
    state->personalityValue = createStatRow(state->mainWrapper, "Personality", lv_palette_main(LV_PALETTE_PURPLE), isXLarge);

    // Create stat rows with colored values
    state->hungerValue = createStatRow(state->mainWrapper, "Hunger", lv_color_hex(0xFF9900), isXLarge);
    state->happyValue = createStatRow(state->mainWrapper, "Happy", lv_color_hex(0xFFCC00), isXLarge);
    state->healthValue = createStatRow(state->mainWrapper, "Health", lv_color_hex(0x00FF00), isXLarge);
    state->energyValue = createStatRow(state->mainWrapper, "Energy", lv_color_hex(0x00CCFF), isXLarge);
    state->cleanValue = createStatRow(state->mainWrapper, "Clean", lv_color_hex(0xFFFFFF), isXLarge);
}

void statsViewStop(Context* ctx) {
    StatsViewState* state = &ctx->statsView;
    state->mainWrapper = nullptr;
    state->titleLabel = nullptr;
    state->statusLabel = nullptr;
    state->hungerValue = nullptr;
    state->happyValue = nullptr;
    state->healthValue = nullptr;
    state->energyValue = nullptr;
    state->cleanValue = nullptr;
    state->personalityValue = nullptr;
}

void statsViewUpdateStats(Context* ctx) {
    StatsViewState* state = &ctx->statsView;
    if (state->titleLabel == nullptr) return;
    if (state->hungerValue == nullptr || state->happyValue == nullptr ||
        state->healthValue == nullptr || state->energyValue == nullptr ||
        state->cleanValue == nullptr || state->statusLabel == nullptr) return;

    const PetStats& stats = ctx->petLogic.getStats();

    // Update title with stage and age
    int hours = stats.ageHours;
    int days = hours / 24;
    hours = hours % 24;

    char titleText[64];
    snprintf(titleText, sizeof(titleText), "%s | %dd %dh", lifeStageToString(stats.stage), days, hours);
    lv_label_set_text(state->titleLabel, titleText);

    // Update status label (right-aligned)
    const char* status = "";
    if (stats.isDead) status = "[DEAD]";
    else if (stats.isSick) status = "[SICK]";
    else if (stats.isAsleep) status = "[SLEEPING]";
    lv_label_set_text(state->statusLabel, status);

    // Update personality
    lv_label_set_text(state->personalityValue, personalityToString(stats.personality));

    // Update individual stat values
    char valueText[16];

    snprintf(valueText, sizeof(valueText), "%d%%", stats.hunger);
    lv_label_set_text(state->hungerValue, valueText);

    snprintf(valueText, sizeof(valueText), "%d%%", stats.happiness);
    lv_label_set_text(state->happyValue, valueText);

    snprintf(valueText, sizeof(valueText), "%d%%", stats.health);
    lv_label_set_text(state->healthValue, valueText);

    snprintf(valueText, sizeof(valueText), "%d%%", stats.energy);
    lv_label_set_text(state->energyValue, valueText);

    snprintf(valueText, sizeof(valueText), "%d%%", stats.cleanliness);
    lv_label_set_text(state->cleanValue, valueText);
}
