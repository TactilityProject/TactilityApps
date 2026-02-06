/**
 * @file SnakeHelpers.h
 * @brief Data structures and constants for the Snake game
 */
#ifndef SNAKE_HELPERS_H
#define SNAKE_HELPERS_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/*********************
 *      DEFINES
 *********************/

// Cell sizes in pixels (larger = easier, smaller = harder)
#define SNAKE_CELL_LARGE     16  // Easy - bigger cells, fewer cells fit
#define SNAKE_CELL_MEDIUM    12  // Medium
#define SNAKE_CELL_SMALL     8   // Hard - smaller cells, more cells fit

// Game timing
#define SNAKE_GAME_SPEED_MS      300  // Initial timer interval in milliseconds
#define SNAKE_MIN_SPEED_MS       60   // Minimum (fastest) timer interval
#define SNAKE_SPEED_DECREASE_MS  5    // Speed increase per food eaten (ms reduction)

// Visual settings
#define SNAKE_INITIAL_LENGTH 3
#define SNAKE_CELL_RADIUS    2

// Colors
#define SNAKE_HEAD_COLOR     lv_color_hex(0x4CAF50)  // Green
#define SNAKE_BODY_COLOR     lv_color_hex(0x81C784)  // Light green
#define SNAKE_FOOD_COLOR     lv_color_hex(0xF44336)  // Red
#define SNAKE_BG_COLOR       lv_color_hex(0x212121)  // Dark background
#define SNAKE_GRID_COLOR     lv_color_hex(0x424242)  // Grid lines

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief Snake movement direction
 */
typedef enum {
    SNAKE_DIR_UP = 0,
    SNAKE_DIR_DOWN,
    SNAKE_DIR_LEFT,
    SNAKE_DIR_RIGHT
} snake_direction_t;

/**
 * @brief Snake body segment (doubly-linked list node)
 */
typedef struct snake_segment {
    int16_t x;                      // Grid x position
    int16_t y;                      // Grid y position
    lv_obj_t* obj;                  // LVGL object for this segment
    struct snake_segment* prior;    // Previous segment (towards tail)
    struct snake_segment* next;     // Next segment (towards head)
} snake_segment_t;

/**
 * @brief Complete game state
 */
typedef struct {
    // Snake data
    snake_segment_t* head;          // Snake head (linked list)

    // UI elements
    lv_obj_t* widget;               // Parent widget (for events)
    lv_obj_t* container;            // Main game container
    lv_obj_t* food;                 // Food object
    lv_timer_t* timer;              // Game timer

    // Game state
    snake_direction_t direction;     // Current movement direction
    snake_direction_t next_direction;// Buffered direction (prevents 180° reversal)

    // Grid settings (supports non-square)
    uint16_t grid_width;            // Grid width in cells
    uint16_t grid_height;           // Grid height in cells
    uint16_t cell_size;             // Pixel size per cell

    // Score tracking
    uint16_t score;
    uint16_t length;

    // Food position
    int16_t food_x;
    int16_t food_y;

    // State flags
    bool game_over;
    bool paused;
    bool wall_collision_enabled;            // If true, hitting walls = game over
} snake_game_t;

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SNAKE_HELPERS_H*/
