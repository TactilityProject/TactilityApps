/**
 * @file SnakeLogic.c
 * @brief Pure game logic for the Snake game
 */
#include "SnakeLogic.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Initialize the snake body as a linked list
 */
snake_segment_t* snake_init_body(uint16_t length, int16_t start_x, int16_t start_y) {
    if (length == 0) {
        return NULL;
    }

    // Create head segment
    snake_segment_t* head = (snake_segment_t*)lv_malloc(sizeof(snake_segment_t));
    if (!head) {
        return NULL;
    }

    head->x = start_x;
    head->y = start_y;
    head->obj = NULL;
    head->prior = NULL;
    head->next = NULL;

    // Create remaining segments (body extends to the left of head)
    snake_segment_t* current = head;
    for (uint16_t i = 1; i < length; i++) {
        snake_segment_t* segment = (snake_segment_t*)lv_malloc(sizeof(snake_segment_t));
        if (!segment) {
            // Clean up already allocated segments
            snake_free_body(head);
            return NULL;
        }

        segment->x = start_x - i;  // Body extends left from head
        segment->y = start_y;
        segment->obj = NULL;
        segment->prior = current;
        segment->next = NULL;

        current->next = segment;
        current = segment;
    }

    return head;
}

/**
 * @brief Free all memory used by the snake body
 */
void snake_free_body(snake_segment_t* head) {
    snake_segment_t* current = head;
    while (current != NULL) {
        snake_segment_t* next = current->next;
        // Note: LVGL objects must be deleted separately by the UI layer
        lv_free(current);
        current = next;
    }
}

/**
 * @brief Add a new segment to the tail of the snake
 */
bool snake_grow(snake_segment_t* head) {
    if (!head) {
        return false;
    }

    // Find the tail
    snake_segment_t* tail = head;
    while (tail->next != NULL) {
        tail = tail->next;
    }

    // Create new segment at tail position (will be updated on next move)
    snake_segment_t* new_segment = (snake_segment_t*)lv_malloc(sizeof(snake_segment_t));
    if (!new_segment) {
        return false;
    }

    new_segment->x = tail->x;
    new_segment->y = tail->y;
    new_segment->obj = NULL;
    new_segment->prior = tail;
    new_segment->next = NULL;

    tail->next = new_segment;

    return true;
}

/**
 * @brief Move the snake one step in the current direction
 */
bool snake_move(snake_game_t* game) {
    if (!game || !game->head || game->game_over) {
        return false;
    }

    // Apply buffered direction
    game->direction = game->next_direction;

    // Calculate new head position
    int16_t new_x = game->head->x;
    int16_t new_y = game->head->y;

    switch (game->direction) {
        case SNAKE_DIR_UP:
            new_y--;
            break;
        case SNAKE_DIR_DOWN:
            new_y++;
            break;
        case SNAKE_DIR_LEFT:
            new_x--;
            break;
        case SNAKE_DIR_RIGHT:
            new_x++;
            break;
    }

    // Handle wall collision or wrap-around
    if (game->wall_collision_enabled) {
        // Wall collision mode - hitting walls = game over
        if (new_x < 0 || new_x >= game->grid_width ||
            new_y < 0 || new_y >= game->grid_height) {
            game->game_over = true;
            return false;
        }
    } else {
        // Wrap around walls
        if (new_x < 0) new_x = game->grid_width - 1;
        else if (new_x >= game->grid_width) new_x = 0;
        if (new_y < 0) new_y = game->grid_height - 1;
        else if (new_y >= game->grid_height) new_y = 0;
    }

    // Check for self collision BEFORE moving (so tail hasn't vacated yet)
    // Skip the tail segment since it will move out of the way
    snake_segment_t* segment = game->head->next;
    while (segment != NULL && segment->next != NULL) {  // Stop before tail
        if (segment->x == new_x && segment->y == new_y) {
            game->game_over = true;
            return false;
        }
        segment = segment->next;
    }
    // Also check the tail - it will move, so new head CAN go there
    // (This is intentional - allows snake to "chase its tail")

    // Move body segments (from tail to head, each takes position of previous)
    snake_segment_t* tail = game->head;
    while (tail->next != NULL) {
        tail = tail->next;
    }

    // Move from tail towards head
    while (tail->prior != NULL) {
        tail->x = tail->prior->x;
        tail->y = tail->prior->y;
        tail = tail->prior;
    }

    // Move head to new position
    game->head->x = new_x;
    game->head->y = new_y;

    return true;
}

/**
 * @brief Set the snake's next direction (with 180° reversal prevention)
 */
bool snake_set_direction(snake_game_t* game, snake_direction_t dir) {
    if (!game) {
        return false;
    }

    // Prevent 180° reversal - check against BUFFERED direction to handle rapid inputs
    snake_direction_t current = game->next_direction;

    if ((current == SNAKE_DIR_UP && dir == SNAKE_DIR_DOWN) ||
        (current == SNAKE_DIR_DOWN && dir == SNAKE_DIR_UP) ||
        (current == SNAKE_DIR_LEFT && dir == SNAKE_DIR_RIGHT) ||
        (current == SNAKE_DIR_RIGHT && dir == SNAKE_DIR_LEFT)) {
        return false;
    }

    game->next_direction = dir;
    return true;
}

/**
 * @brief Check if a position is occupied by the snake body
 */
static bool is_position_on_snake(snake_segment_t* head, int16_t x, int16_t y) {
    snake_segment_t* current = head;
    while (current != NULL) {
        if (current->x == x && current->y == y) {
            return true;
        }
        current = current->next;
    }
    return false;
}

/**
 * @brief Spawn food at a random location not occupied by snake
 * @return true if food was placed, false if grid is full (win condition)
 */
bool snake_spawn_food(snake_game_t* game) {
    if (!game || game->grid_width == 0 || game->grid_height == 0) {
        return false;
    }

    int16_t x, y;
    const int32_t grid_size = (int32_t)game->grid_width * game->grid_height;
    uint16_t snake_len = snake_count_segments(game->head);

    // Use random sampling for sparse grids, deterministic search for dense grids
    if (snake_len < (grid_size * 3 / 4)) {
        // Random sampling - efficient for sparse grids
        int attempts = 0;
        do {
            x = rand() % game->grid_width;
            y = rand() % game->grid_height;
            attempts++;
        } while (is_position_on_snake(game->head, x, y) && attempts < grid_size);
        
        if (!is_position_on_snake(game->head, x, y)) {
            game->food_x = x;
            game->food_y = y;
            return true;
        }
    }

    // Deterministic search - guaranteed to find free cell if one exists
    int16_t start_y = rand() % game->grid_height;
    int16_t start_x = rand() % game->grid_width;
    for (int16_t i = 0; i < game->grid_height; i++) {
        int16_t gy = (start_y + i) % game->grid_height;
        for (int16_t j = 0; j < game->grid_width; j++) {
            int16_t gx = (start_x + j) % game->grid_width;
            if (!is_position_on_snake(game->head, gx, gy)) {
                game->food_x = gx;
                game->food_y = gy;
                return true;
            }
        }
    }

    // Grid is truly full - win condition
    return false;
}

/**
 * @brief Check if snake head collides with walls (unused - wrap-around enabled)
 */
bool snake_check_wall_collision(snake_game_t* game) {
    if (!game || !game->head) {
        return false;
    }

    int16_t x = game->head->x;
    int16_t y = game->head->y;

    return (x < 0 || x >= game->grid_width || y < 0 || y >= game->grid_height);
}

/**
 * @brief Check if snake head collides with its own body
 */
bool snake_check_self_collision(snake_game_t* game) {
    if (!game || !game->head) {
        return false;
    }

    int16_t head_x = game->head->x;
    int16_t head_y = game->head->y;

    // Check collision with body segments (skip head itself)
    snake_segment_t* current = game->head->next;
    while (current != NULL) {
        if (current->x == head_x && current->y == head_y) {
            return true;
        }
        current = current->next;
    }

    return false;
}

/**
 * @brief Check if snake head collides with food
 */
bool snake_check_food_collision(snake_game_t* game) {
    if (!game || !game->head) {
        return false;
    }

    return (game->head->x == game->food_x && game->head->y == game->food_y);
}

/**
 * @brief Count the number of segments in the snake
 */
uint16_t snake_count_segments(snake_segment_t* head) {
    uint16_t length = 0;
    snake_segment_t* current = head;

    while (current != NULL) {
        length++;
        current = current->next;
    }

    return length;
}
