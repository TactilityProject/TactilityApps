/**
 * @file SnakeUi.h
 * @brief LVGL widget interface for the Snake game
 */
#ifndef SNAKE_UI_H
#define SNAKE_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>
#include "SnakeHelpers.h"
#include "lvgl.h"

/***********************
 * FUNCTION PROTOTYPES
 **********************/

/**
 * @brief Create a new Snake game widget
 * @param parent Parent LVGL object
 * @param cell_size Size of each cell in pixels (SNAKE_CELL_SMALL, MEDIUM, or LARGE)
 * @param wall_collision If true, hitting walls = game over; if false, snake wraps around
 * @return Pointer to the created LVGL object, or NULL on failure
 */
lv_obj_t* snake_create(lv_obj_t* parent, uint16_t cell_size, bool wall_collision);

/**
 * @brief Reset the game to start a new game
 * @param obj Snake game LVGL object
 */
void snake_set_new_game(lv_obj_t* obj);

/**
 * @brief Get the current score
 * @param obj Snake game LVGL object
 * @return Current score
 */
uint16_t snake_get_score(lv_obj_t* obj);

/**
 * @brief Get the current snake length
 * @param obj Snake game LVGL object
 * @return Current length
 */
uint16_t snake_get_length(lv_obj_t* obj);

/**
 * @brief Check if game is over
 * @param obj Snake game LVGL object
 * @return true if game is over
 */
bool snake_get_game_over(lv_obj_t* obj);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SNAKE_UI_H*/
