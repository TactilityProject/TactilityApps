/**
 * @file SnakeLogic.h
 * @brief Pure game logic for the Snake game (no UI dependencies)
 */
#ifndef SNAKE_LOGIC_H
#define SNAKE_LOGIC_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "SnakeHelpers.h"

/***********************
 * FUNCTION PROTOTYPES
 **********************/

/**
 * @brief Initialize the snake body as a linked list
 * @param length Initial length of the snake
 * @param start_x Starting x position (grid coordinate)
 * @param start_y Starting y position (grid coordinate)
 * @return Pointer to the head segment, or NULL on failure
 */
snake_segment_t* snake_init_body(uint16_t length, int16_t start_x, int16_t start_y);

/**
 * @brief Free all memory used by the snake body
 * @param head Pointer to the head segment
 */
void snake_free_body(snake_segment_t* head);

/**
 * @brief Add a new segment to the tail of the snake
 * @param head Pointer to the head segment
 * @return true on success, false on allocation failure
 */
bool snake_grow(snake_segment_t* head);

/**
 * @brief Move the snake one step in the current direction
 * @param game Game state containing snake and direction
 * @return true if move was successful, false if collision occurred
 */
bool snake_move(snake_game_t* game);

/**
 * @brief Set the snake's next direction (with 180° reversal prevention)
 * @param game Game state
 * @param dir New direction
 * @return true if direction was set, false if it would cause 180° reversal
 */
bool snake_set_direction(snake_game_t* game, snake_direction_t dir);

/**
 * @brief Spawn food at a random location not occupied by snake
 * @param game Game state
 * @return true if food was placed, false if grid is full (win condition)
 */
bool snake_spawn_food(snake_game_t* game);

/**
 * @brief Check if snake head collides with walls
 * @param game Game state
 * @return true if collision detected
 */
bool snake_check_wall_collision(snake_game_t* game);

/**
 * @brief Check if snake head collides with its own body
 * @param game Game state
 * @return true if collision detected
 */
bool snake_check_self_collision(snake_game_t* game);

/**
 * @brief Check if snake head collides with food
 * @param game Game state
 * @return true if collision detected
 */
bool snake_check_food_collision(snake_game_t* game);

/**
 * @brief Count the number of segments in the snake
 * @param head Pointer to the head segment
 * @return Number of segments
 */
uint16_t snake_count_segments(snake_segment_t* head);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SNAKE_LOGIC_H*/
