#include "TwoElevenLogic.h"
#include "TwoElevenHelpers.h"
#include <stdlib.h>
#include <time.h>
#include <string.h>

/**
 * @brief Initialize the matrix with two random tiles
 */
void init_matrix_num(uint16_t matrix_size, uint16_t **matrix)
{
    for (uint8_t x = 0; x < matrix_size; x++) {
        for (uint8_t y = 0; y < matrix_size; y++) {
            matrix[x][y] = 0;
        }
    }
    // Add two random tiles
    add_random(matrix_size, matrix);
    add_random(matrix_size, matrix);
}

/**
 * @brief Add a random tile (2 or 4) to the matrix
 */
void add_random(uint16_t matrix_size, uint16_t **matrix)
{
    static bool initialized = false;
    if (!initialized) {
        srand((unsigned int)time(NULL));
        initialized = true;
    }
    uint16_t empty[matrix_size * matrix_size][2];
    uint16_t len = 0;
    for (uint8_t x = 0; x < matrix_size; x++) {
        for (uint8_t y = 0; y < matrix_size; y++) {
            if (matrix[x][y] == 0) {
                empty[len][0] = x;
                empty[len][1] = y;
                len++;
            }
        }
    }
    if (len > 0) {
        uint16_t r = rand() % len;
        uint8_t x = empty[r][0];
        uint8_t y = empty[r][1];
        uint16_t n = (rand() % 10 == 0) ? 2 : 1; // 10% chance for 4, else 2
        matrix[x][y] = n;
    }
}

/**
 * @brief Move up (or left/right/down via rotation)
 */
bool move_up(uint16_t * score, uint16_t matrix_size, uint16_t **matrix) {
    bool success = false;
    for (uint8_t x = 0; x < matrix_size; x++) {
        success |= slide_array(score, matrix_size, matrix[x]);
    }
    return success;
}

bool move_down(uint16_t * score, uint16_t matrix_size, uint16_t **matrix) {
    rotate_matrix(matrix_size, matrix);
    rotate_matrix(matrix_size, matrix);
    bool success = move_up(score, matrix_size, matrix);
    rotate_matrix(matrix_size, matrix);
    rotate_matrix(matrix_size, matrix);
    return success;
}

bool move_left(uint16_t * score, uint16_t matrix_size, uint16_t **matrix) {
    rotate_matrix(matrix_size, matrix);
    bool success = move_up(score, matrix_size, matrix);
    rotate_matrix(matrix_size, matrix);
    rotate_matrix(matrix_size, matrix);
    rotate_matrix(matrix_size, matrix);
    return success;
}

bool move_right(uint16_t * score, uint16_t matrix_size, uint16_t **matrix) {
    rotate_matrix(matrix_size, matrix);
    rotate_matrix(matrix_size, matrix);
    rotate_matrix(matrix_size, matrix);
    bool success = move_up(score, matrix_size, matrix);
    rotate_matrix(matrix_size, matrix);
    return success;
}

/**
 * @brief Check if the game is over (no moves left)
 *        Does not mutate the original matrix
 */
bool game_over(uint16_t matrix_size, const uint16_t **matrix) {
    if (count_empty(matrix_size, matrix) > 0) return false;
    if (find_pair_down(matrix_size, matrix)) return false;
    // Check for possible moves in rotated matrices
    uint16_t **temp = lv_malloc(matrix_size * sizeof(uint16_t*));
    for (uint16_t i = 0; i < matrix_size; i++) {
        temp[i] = lv_malloc(matrix_size * sizeof(uint16_t));
        memcpy(temp[i], matrix[i], matrix_size * sizeof(uint16_t));
    }
    rotate_matrix(matrix_size, temp);
    if (find_pair_down(matrix_size, (const uint16_t **)temp)) {
        for (uint16_t i = 0; i < matrix_size; i++) lv_free(temp[i]);
        lv_free(temp);
        return false;
    }
    rotate_matrix(matrix_size, temp);
    if (find_pair_down(matrix_size, (const uint16_t **)temp)) {
        for (uint16_t i = 0; i < matrix_size; i++) lv_free(temp[i]);
        lv_free(temp);
        return false;
    }
    rotate_matrix(matrix_size, temp);
    if (find_pair_down(matrix_size, (const uint16_t **)temp)) {
        for (uint16_t i = 0; i < matrix_size; i++) lv_free(temp[i]);
        lv_free(temp);
        return false;
    }
    for (uint16_t i = 0; i < matrix_size; i++) lv_free(temp[i]);
    lv_free(temp);
    return true;
}

/**
 * @brief Get the current score
 */
uint32_t twoeleven_get_score(lv_obj_t * obj)
{
    const twoeleven_t * game_2048 = (const twoeleven_t *)lv_obj_get_user_data(obj);
    if (!game_2048) return 0;
    return game_2048->score;
}

/**
 * @brief Get the game over status
 */
bool twoeleven_get_status(lv_obj_t * obj)
{
    const twoeleven_t * game_2048 = (const twoeleven_t *)lv_obj_get_user_data(obj);
    if (!game_2048) return true;
    return game_2048->game_over;
}

/**
 * @brief Get the best tile value
 */
uint16_t twoeleven_get_best_tile(lv_obj_t * obj)
{
    const twoeleven_t * game_2048 = (const twoeleven_t *)lv_obj_get_user_data(obj);
    if (!game_2048) return 0;
    uint16_t best_tile = 0;
    for (uint8_t x = 0; x < game_2048->matrix_size; x++) {
        for (uint8_t y = 0; y < game_2048->matrix_size; y++) {
            if (best_tile < game_2048->matrix[x][y]) {
                best_tile = game_2048->matrix[x][y];
            }
        }
    }
    return (uint16_t)(1 << best_tile);
}
