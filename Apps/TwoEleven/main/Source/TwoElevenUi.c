#include "TwoElevenUi.h"
#include "TwoElevenLogic.h"
#include "TwoElevenHelpers.h"
#include <stdlib.h>
#include <string.h>
#include <tt_lvgl_keyboard.h>

static void game_play_event(lv_event_t * e);
static void btnm_event_cb(lv_event_t * e);
static void focus_event(lv_event_t* e);

/**
 * @brief Free all resources for the 2048 game object
 */
static void delete_event(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    twoeleven_t * game_2048 = (twoeleven_t *)lv_obj_get_user_data(obj);
    if (game_2048) {
        // Reset group editing mode if we enabled it
        if (tt_lvgl_hardware_keyboard_is_available()) {
            lv_group_t* group = lv_group_get_default();
            if (group) {
                lv_group_set_editing(group, false);
            }
        }
        for (uint16_t index = 0; index < game_2048->map_count; index++) {
            if (game_2048->btnm_map[index]) {
                lv_free(game_2048->btnm_map[index]);
                game_2048->btnm_map[index] = NULL;
            }
        }
        lv_free(game_2048->btnm_map);
        // Free matrix
        for (uint16_t i = 0; i < game_2048->matrix_size; i++) {
            lv_free(game_2048->matrix[i]);
        }
        lv_free(game_2048->matrix);
        lv_free(game_2048);
        lv_obj_set_user_data(obj, NULL);
    }
}

/**
 * @brief Handle focus/defocus to manage edit mode for keyboard input
 */
static void focus_event(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_group_t* group = lv_group_get_default();

    if (!group) return;

    if (code == LV_EVENT_FOCUSED) {
        // Enable edit mode so arrow keys control the game
        lv_group_set_editing(group, true);
    } else if (code == LV_EVENT_DEFOCUSED) {
        // Restore normal focus navigation
        lv_group_set_editing(group, false);
    }
}

/**
 * @brief Create a new 2048 game object
 */
lv_obj_t * twoeleven_create(lv_obj_t * parent, uint16_t matrix_size)
{
    lv_obj_t * obj = lv_obj_create(parent);
    if (!obj) return NULL;
    twoeleven_t * game_2048 = (twoeleven_t *)lv_malloc(sizeof(twoeleven_t));
    if (!game_2048) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_set_user_data(obj, game_2048);

    game_2048->score = 0;
    game_2048->game_over = false;
    // Limit matrix size to 3x3 to 6x6, default to 4x4 if out of range
    if (matrix_size < 3 || matrix_size > 6) {
        matrix_size = 4;
    }
    game_2048->matrix_size = matrix_size;
    game_2048->map_count = game_2048->matrix_size * game_2048->matrix_size + game_2048->matrix_size;

    // Allocate matrix
    game_2048->matrix = lv_malloc(game_2048->matrix_size * sizeof(uint16_t*));
    if (!game_2048->matrix) {
        lv_free(game_2048);
        lv_obj_delete(obj);
        return NULL;
    }
    for (uint16_t i = 0; i < game_2048->matrix_size; i++) {
        game_2048->matrix[i] = lv_malloc(game_2048->matrix_size * sizeof(uint16_t));
        if (!game_2048->matrix[i]) {
            for (uint16_t j = 0; j < i; j++) lv_free(game_2048->matrix[j]);
            lv_free(game_2048->matrix);
            lv_free(game_2048);
            lv_obj_delete(obj);
            return NULL;
        }
    }

    // Allocate button map
    game_2048->btnm_map = lv_malloc(game_2048->map_count * sizeof(char*));
    for (uint16_t index = 0; index < game_2048->map_count; index++) {
        if (((index + 1) % (game_2048->matrix_size + 1)) == 0) {
            game_2048->btnm_map[index] = (char *)lv_malloc(2);
            if (!game_2048->btnm_map[index]) continue;
            if ((index + 1) == game_2048->map_count)
                strcpy(game_2048->btnm_map[index], "");
            else
                strcpy(game_2048->btnm_map[index], "\n");
        } else {
            game_2048->btnm_map[index] = (char *)lv_malloc(16);
            if (!game_2048->btnm_map[index]) continue;
            strcpy(game_2048->btnm_map[index], " ");
        }
    }

    init_matrix_num(game_2048->matrix_size, game_2048->matrix);
    update_btnm_map(game_2048->matrix_size, game_2048->btnm_map, (const uint16_t **)game_2048->matrix);

    // Style initialization (unchanged)
    lv_obj_set_style_outline_color(obj, lv_theme_get_color_primary(obj), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(obj, lv_display_dpx(lv_obj_get_disp(obj), 2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(obj, lv_display_dpx(lv_obj_get_disp(obj), 2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(obj, LV_OPA_50, LV_STATE_FOCUS_KEY);

    // Create button matrix
    game_2048->btnm = lv_btnmatrix_create(obj);
    lv_obj_set_size(game_2048->btnm, LV_PCT(100), LV_PCT(100));
    lv_obj_center(game_2048->btnm);
    lv_obj_set_style_pad_all(game_2048->btnm, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(game_2048->btnm, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(game_2048->btnm, ELEVENTWO_BG_COLOR, LV_PART_MAIN);
    lv_group_remove_obj(game_2048->btnm);
    lv_obj_add_flag(game_2048->btnm, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_remove_flag(game_2048->btnm, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_btnmatrix_set_map(game_2048->btnm, (const char **)game_2048->btnm_map);
    lv_btnmatrix_set_btn_ctrl_all(game_2048->btnm, LV_BTNMATRIX_CTRL_DISABLED);

    lv_obj_add_event_cb(game_2048->btnm, game_play_event, LV_EVENT_GESTURE, obj);
    lv_obj_add_event_cb(game_2048->btnm, game_play_event, LV_EVENT_KEY, obj);
    lv_obj_add_event_cb(game_2048->btnm, btnm_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_event_cb(obj, delete_event, LV_EVENT_DELETE, NULL);

    if (tt_lvgl_hardware_keyboard_is_available()) {
        lv_group_t* group = lv_group_get_default();
        if (group) {
            lv_group_add_obj(group, game_2048->btnm);
            // Register focus handlers to manage edit mode lifecycle
            lv_obj_add_event_cb(game_2048->btnm, focus_event, LV_EVENT_FOCUSED, NULL);
            lv_obj_add_event_cb(game_2048->btnm, focus_event, LV_EVENT_DEFOCUSED, NULL);
            // Focus the container (will trigger FOCUSED event and enable edit mode)
            lv_group_focus_obj(game_2048->btnm);
        }
    }

    return obj;
}

/**
 * @brief Start a new game (reset state)
 */
void twoeleven_set_new_game(lv_obj_t * obj)
{
    twoeleven_t * game_2048 = (twoeleven_t *)lv_obj_get_user_data(obj);
    if (!game_2048) return;
    game_2048->score = 0;
    game_2048->game_over = false;
    game_2048->map_count = game_2048->matrix_size * game_2048->matrix_size + game_2048->matrix_size;
    init_matrix_num(game_2048->matrix_size, game_2048->matrix);
    update_btnm_map(game_2048->matrix_size, game_2048->btnm_map, (const uint16_t **)game_2048->matrix);
    lv_btnmatrix_set_map(game_2048->btnm, (const char **)game_2048->btnm_map);
    lv_obj_invalidate(game_2048->btnm);
    lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
}

/**
 * @brief Event callback for game play (gesture/key)
 */
static void game_play_event(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_user_data(e);
    twoeleven_t * game_2048 = (twoeleven_t *)lv_obj_get_user_data(obj);
    bool success = false;
    if (!game_2048) return;

    if (code == LV_EVENT_GESTURE) {
        game_2048->game_over = game_over(game_2048->matrix_size, (const uint16_t **)game_2048->matrix);
        if (!game_2048->game_over) {
            lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
            switch (dir) {
                case LV_DIR_TOP:
                    success = move_right(&(game_2048->score), game_2048->matrix_size, game_2048->matrix);
                    break;
                case LV_DIR_BOTTOM:
                    success = move_left(&(game_2048->score), game_2048->matrix_size, game_2048->matrix);
                    break;
                case LV_DIR_LEFT:
                    success = move_up(&(game_2048->score), game_2048->matrix_size, game_2048->matrix);
                    break;
                case LV_DIR_RIGHT:
                    success = move_down(&(game_2048->score), game_2048->matrix_size, game_2048->matrix);
                    break;
                default:
                    break;
            }
        } else {
            lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
            return;
        }
    } else if (code == LV_EVENT_KEY) {
        game_2048->game_over = game_over(game_2048->matrix_size, (const uint16_t **)game_2048->matrix);
        if (!game_2048->game_over) {
            uint32_t key = lv_event_get_key(e);
            switch (key) {
                // Arrow keys, WASD, and punctuation keys for cardputer
                case LV_KEY_UP:
                case 'w':
                case 'W':
                case ';':
                    success = move_right(&(game_2048->score), game_2048->matrix_size, game_2048->matrix);
                    break;
                case LV_KEY_DOWN:
                case 's':
                case 'S':
                case '.':
                    success = move_left(&(game_2048->score), game_2048->matrix_size, game_2048->matrix);
                    break;
                case LV_KEY_LEFT:
                case 'a':
                case 'A':
                case ',':
                    success = move_up(&(game_2048->score), game_2048->matrix_size, game_2048->matrix);
                    break;
                case LV_KEY_RIGHT:
                case 'd':
                case 'D':
                case '/':
                    success = move_down(&(game_2048->score), game_2048->matrix_size, game_2048->matrix);
                    break;
                default:
                    break;
            }
        } else {
            lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
            return;
        }
    }

    if (success) {
        add_random(game_2048->matrix_size, game_2048->matrix);
        update_btnm_map(game_2048->matrix_size, game_2048->btnm_map, (const uint16_t **)game_2048->matrix);
        lv_btnmatrix_set_map(game_2048->btnm, (const char **)game_2048->btnm_map);
        lv_obj_invalidate(game_2048->btnm);
        lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

/**
 * @brief Event callback for button matrix drawing (coloring tiles)
 */
static void btnm_event_cb(lv_event_t * e)
{
    lv_obj_t * btnm = lv_event_get_target_obj(e);
    lv_obj_t * parent = lv_obj_get_parent(btnm);
    twoeleven_t * game_2048 = (twoeleven_t *)lv_obj_get_user_data(parent);
    if (!game_2048) return;

    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc = lv_draw_task_get_draw_dsc(draw_task);

    if (base_dsc->part == LV_PART_ITEMS) {
        lv_draw_label_dsc_t * label_draw_dsc = lv_draw_task_get_label_dsc(draw_task);
        lv_draw_fill_dsc_t * fill_draw_dsc = lv_draw_task_get_fill_dsc(draw_task);

        if ((int32_t)base_dsc->id1 >= 0) {
            uint16_t x = (uint16_t)((base_dsc->id1) / game_2048->matrix_size);
            uint16_t y = (uint16_t)((base_dsc->id1) % game_2048->matrix_size);
            uint16_t num = (uint16_t)(1 << (game_2048->matrix[x][y]));

            if (fill_draw_dsc) {
                fill_draw_dsc->radius = 3;
                fill_draw_dsc->color = get_num_color(num);
            }
            if (label_draw_dsc) {
                label_draw_dsc->color = (num < 8) ? ELEVENTWO_TEXT_BLACK_COLOR : ELEVENTWO_TEXT_WHITE_COLOR;
            }
        } else {
            if (fill_draw_dsc) fill_draw_dsc->radius = 5;
        }
    }
}
