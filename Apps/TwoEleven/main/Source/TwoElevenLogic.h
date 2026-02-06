#ifndef TWOELEVEN_LOGIC_H
#define TWOELEVEN_LOGIC_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "TwoElevenHelpers.h"

/***********************
 * FUNCTION PROTOTYPES
 **********************/
/**
 * @brief Initialize the matrix with two random tiles
 */
void init_matrix_num(uint16_t matrix_size, uint16_t **matrix);

/**
 * @brief Add a random tile (2 or 4) to the matrix
 */
void add_random(uint16_t matrix_size, uint16_t **matrix);

/**
 * @brief Move up
 */
bool move_up(uint16_t * score, uint16_t matrix_size, uint16_t **matrix);

/**
 * @brief Move down
 */
bool move_down(uint16_t * score, uint16_t matrix_size, uint16_t **matrix);

/**
 * @brief Move left
 */
bool move_left(uint16_t * score, uint16_t matrix_size, uint16_t **matrix);

/**
 * @brief Move right
 */
bool move_right(uint16_t * score, uint16_t matrix_size, uint16_t **matrix);

/**
 * @brief Check if the game is over (no moves left)
 *        Does not mutate the original matrix
 */
bool game_over(uint16_t matrix_size, const uint16_t **matrix);

/**
 * @brief Get the current score
 */
uint32_t twoeleven_get_score(lv_obj_t * obj);

/**
 * @brief Get the game over status
 */
bool twoeleven_get_status(lv_obj_t * obj);

/**
 * @brief Get the best tile value
 */
uint16_t twoeleven_get_best_tile(lv_obj_t * obj);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*TWOELEVEN_LOGIC_H*/