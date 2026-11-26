#ifndef TWOELEVEN_HELPERS_H
#define TWOELEVEN_HELPERS_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lvgl.h"

/*********************
 *      DEFINES
 *********************/

#define ELEVENTWO_BG_COLOR             lv_color_hex(0xb3a397)
#define ELEVENTWO_TEXT_COLOR           lv_color_hex(0x3b342f)
#define ELEVENTWO_NUMBER_EMPTY_COLOR   lv_color_hex(0xc7b9ac)
#define ELEVENTWO_NUMBER_2_COLOR       lv_color_hex(0xeee4da)
#define ELEVENTWO_NUMBER_4_COLOR       lv_color_hex(0xede0c8)
#define ELEVENTWO_NUMBER_8_COLOR       lv_color_hex(0xf2b179)
#define ELEVENTWO_NUMBER_16_COLOR      lv_color_hex(0xf59563)
#define ELEVENTWO_NUMBER_32_COLOR      lv_color_hex(0xf67c5f)
#define ELEVENTWO_NUMBER_64_COLOR      lv_color_hex(0xf75f3b)
#define ELEVENTWO_NUMBER_128_COLOR     lv_color_hex(0xedcf72)
#define ELEVENTWO_NUMBER_256_COLOR     lv_color_hex(0xedcc61)
#define ELEVENTWO_NUMBER_512_COLOR     lv_color_hex(0xedc850)
#define ELEVENTWO_NUMBER_1024_COLOR    lv_color_hex(0xedc53f)
#define ELEVENTWO_NUMBER_2048_COLOR    lv_color_hex(0xedc22e)

/**********************
 *      TYPEDEFS
 **********************/
/**
 * @brief Struct for 2048 game state
 */
struct twoeleven_t {
    lv_obj_t * btnm;
    uint16_t score;
    uint16_t map_count;
    uint16_t matrix_size;
    uint16_t **matrix;
    char ** btnm_map;
    bool game_over;
};

typedef struct twoeleven_t twoeleven_t;

/***********************
 * FUNCTION PROTOTYPES
 **********************/
/**
 * @brief Get the color for a given tile value
 */
lv_color_t get_num_color(uint16_t num);

/**
 * @brief Count empty cells in the matrix
 */
uint8_t count_empty(uint16_t matrix_size, const uint16_t **matrix);

/**
 * @brief Find if any adjacent pairs exist (for move possibility)
 */
bool find_pair_down(uint16_t matrix_size, const uint16_t **matrix);

/**
 * @brief Rotate the matrix 90 degrees clockwise (in-place)
 *        Used for move logic
 */
void rotate_matrix(uint16_t matrix_size, uint16_t **matrix);

/**
 * @brief Slide and merge a single row/column
 */
bool slide_array(uint16_t * score, uint16_t matrix_size, uint16_t *array);

/**
 * @brief Update the button map with current matrix values
 */
void update_btnm_map(uint16_t matrix_size, char ** btnm_map, const uint16_t **matrix);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*TWOELEVEN_HELPERS_H*/