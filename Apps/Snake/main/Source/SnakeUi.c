/**
 * @file SnakeUi.c
 * @brief LVGL widget implementation for the Snake game
 */
#include "SnakeUi.h"
#include "SnakeLogic.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tt_lvgl_keyboard.h>

// Forward declarations
static void game_play_event(lv_event_t* e);
static void snake_timer_cb(lv_timer_t* timer);
static void delete_event(lv_event_t* e);
static void focus_event(lv_event_t* e);
static void snake_draw(snake_game_t* game);
static void snake_create_segment_objects(snake_game_t* game);
static void snake_delete_segment_objects(snake_game_t* game);

// Static flag to ensure srand is only called once
static bool srand_initialized = false;

/**
 * @brief Free all resources for the snake game object
 */
static void delete_event(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    snake_game_t* game = (snake_game_t*)lv_obj_get_user_data(obj);

    if (game) {
        // Stop timer first
        if (game->timer) {
            lv_timer_delete(game->timer);
            game->timer = NULL;
        }

        // Restore edit mode to false before cleanup
        if (tt_lvgl_hardware_keyboard_is_available()) {
            lv_group_t* group = lv_group_get_default();
            if (group) {
                lv_group_set_editing(group, false);
            }
        }

        // Delete LVGL objects for snake segments
        snake_delete_segment_objects(game);

        // Free snake body linked list
        snake_free_body(game->head);
        game->head = NULL;

        // Food object is a child of container, will be deleted automatically
        // Container is a child of obj, will be deleted automatically

        lv_free(game);
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
 * @brief Delete LVGL objects for all snake segments
 */
static void snake_delete_segment_objects(snake_game_t* game) {
    if (!game || !game->head) return;

    snake_segment_t* segment = game->head;
    while (segment != NULL) {
        if (segment->obj) {
            lv_obj_delete(segment->obj);
            segment->obj = NULL;
        }
        segment = segment->next;
    }
}

/**
 * @brief Create LVGL objects for all snake segments
 */
static void snake_create_segment_objects(snake_game_t* game) {
    if (!game || !game->head || !game->container) return;

    snake_segment_t* segment = game->head;
    bool is_head = true;

    while (segment != NULL) {
        segment->obj = lv_obj_create(game->container);
        lv_obj_set_size(segment->obj, game->cell_size - 2, game->cell_size - 2);
        lv_obj_set_style_radius(segment->obj, SNAKE_CELL_RADIUS, LV_PART_MAIN);
        lv_obj_set_style_border_width(segment->obj, 0, LV_PART_MAIN);

        if (is_head) {
            lv_obj_set_style_bg_color(segment->obj, SNAKE_HEAD_COLOR, LV_PART_MAIN);
            is_head = false;
        } else {
            lv_obj_set_style_bg_color(segment->obj, SNAKE_BODY_COLOR, LV_PART_MAIN);
        }

        // Position will be set by snake_draw()
        segment = segment->next;
    }
}

/**
 * @brief Update LVGL object positions based on game state
 */
static void snake_draw(snake_game_t* game) {
    if (!game || !game->container) return;

    // Update snake segment positions
    snake_segment_t* segment = game->head;
    while (segment != NULL) {
        if (segment->obj) {
            lv_coord_t px = segment->x * game->cell_size + 1;
            lv_coord_t py = segment->y * game->cell_size + 1;
            lv_obj_set_pos(segment->obj, px, py);
        }
        segment = segment->next;
    }

    // Update food position
    if (game->food) {
        lv_coord_t fx = game->food_x * game->cell_size + 1;
        lv_coord_t fy = game->food_y * game->cell_size + 1;
        lv_obj_set_pos(game->food, fx, fy);
    }
}

/**
 * @brief Timer callback - moves snake and updates display
 */
static void snake_timer_cb(lv_timer_t* timer) {
    snake_game_t* game = (snake_game_t*)lv_timer_get_user_data(timer);
    if (!game || game->game_over || game->paused) return;

    // Move snake
    bool move_ok = snake_move(game);

    if (!move_ok) {
        // Collision occurred, game over
        game->game_over = true;
        lv_obj_send_event(game->widget, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }

    // Check food collision
    if (snake_check_food_collision(game)) {
        // Grow snake and update score
        if (!snake_grow(game->head)) {
            // Memory allocation failed - treat as game over
            game->game_over = true;
            lv_obj_send_event(game->widget, LV_EVENT_VALUE_CHANGED, NULL);
            return;
        }
        game->score++;
        game->length++;

        // Create LVGL object for new segment
        snake_segment_t* tail = game->head;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        if (tail && !tail->obj && game->container) {
            tail->obj = lv_obj_create(game->container);
            if (!tail->obj) {
                game->game_over = true;
                lv_obj_send_event(game->widget, LV_EVENT_VALUE_CHANGED, NULL);
                return;
            }
            lv_obj_set_size(tail->obj, game->cell_size - 2, game->cell_size - 2);
            lv_obj_set_style_radius(tail->obj, SNAKE_CELL_RADIUS, LV_PART_MAIN);
            lv_obj_set_style_border_width(tail->obj, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_color(tail->obj, SNAKE_BODY_COLOR, LV_PART_MAIN);
        }

        // Spawn new food - if fails, grid is full (win!)
        if (!snake_spawn_food(game)) {
            // Hide food since there's no valid position
            if (game->food) {
                lv_obj_add_flag(game->food, LV_OBJ_FLAG_HIDDEN);
            }
            // Player won - end the game
            game->game_over = true;
            lv_obj_send_event(game->widget, LV_EVENT_VALUE_CHANGED, NULL);
            return;
        }

        // Increase speed (decrease timer period) as snake grows
        uint32_t foods_eaten = game->length - SNAKE_INITIAL_LENGTH;
        uint32_t speed_reduction = foods_eaten * SNAKE_SPEED_DECREASE_MS;
        uint32_t new_period = SNAKE_GAME_SPEED_MS;
        if (speed_reduction < (SNAKE_GAME_SPEED_MS - SNAKE_MIN_SPEED_MS)) {
            new_period = SNAKE_GAME_SPEED_MS - speed_reduction;
        } else {
            new_period = SNAKE_MIN_SPEED_MS;
        }
        lv_timer_set_period(game->timer, new_period);

        // Notify score change
        lv_obj_send_event(game->widget, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // Update display
    snake_draw(game);
}

/**
 * @brief Event callback for game play (gesture/key)
 */
static void game_play_event(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_user_data(e);
    snake_game_t* game = (snake_game_t*)lv_obj_get_user_data(obj);

    if (!game || game->game_over) return;

    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        switch (dir) {
            case LV_DIR_TOP:
                snake_set_direction(game, SNAKE_DIR_UP);
                break;
            case LV_DIR_BOTTOM:
                snake_set_direction(game, SNAKE_DIR_DOWN);
                break;
            case LV_DIR_LEFT:
                snake_set_direction(game, SNAKE_DIR_LEFT);
                break;
            case LV_DIR_RIGHT:
                snake_set_direction(game, SNAKE_DIR_RIGHT);
                break;
            default:
                break;
        }
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        // Arrow keys, WASD, and punctuation keys for cardputer
        switch (key) {
            case LV_KEY_UP:
            case 'w':
            case 'W':
            case ';':
                snake_set_direction(game, SNAKE_DIR_UP);
                break;
            case LV_KEY_DOWN:
            case 's':
            case 'S':
            case '.':
                snake_set_direction(game, SNAKE_DIR_DOWN);
                break;
            case LV_KEY_LEFT:
            case 'a':
            case 'A':
            case ',':
                snake_set_direction(game, SNAKE_DIR_LEFT);
                break;
            case LV_KEY_RIGHT:
            case 'd':
            case 'D':
            case '/':
                snake_set_direction(game, SNAKE_DIR_RIGHT);
                break;
            default:
                break;
        }
    }
}

/**
 * @brief Create a new Snake game widget
 */
lv_obj_t* snake_create(lv_obj_t* parent, uint16_t cell_size, bool wall_collision) {
    // Create main object
    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj) return NULL;

    // Allocate game state
    snake_game_t* game = (snake_game_t*)lv_malloc(sizeof(snake_game_t));
    if (!game) {
        lv_obj_delete(obj);
        return NULL;
    }
    memset(game, 0, sizeof(snake_game_t));
    lv_obj_set_user_data(obj, game);

    // Store widget reference for events
    game->widget = obj;

    // Initialize game state
    game->cell_size = cell_size;
    game->score = 0;
    game->length = SNAKE_INITIAL_LENGTH;
    game->direction = SNAKE_DIR_RIGHT;
    game->next_direction = SNAKE_DIR_RIGHT;
    game->game_over = false;
    game->paused = false;
    game->wall_collision_enabled = wall_collision;

    // Set up main object
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);

    // Create game container (the actual playing field)
    game->container = lv_obj_create(obj);
    lv_obj_remove_flag(game->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(game->container, SNAKE_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(game->container, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(game->container, SNAKE_GRID_COLOR, LV_PART_MAIN);
    lv_obj_set_style_pad_all(game->container, 0, LV_PART_MAIN);
    lv_group_remove_obj(game->container);
    lv_obj_remove_flag(game->container, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Calculate grid dimensions based on available space (non-square)
    lv_coord_t available_w = lv_obj_get_content_width(parent) - 6;
    lv_coord_t available_h = lv_obj_get_content_height(parent) - 6;

    if (available_w < 50) available_w = 50;
    if (available_h < 50) available_h = 50;

    // Calculate how many cells fit in each dimension
    game->grid_width = available_w / cell_size;
    game->grid_height = available_h / cell_size;

    // Ensure minimum grid size (3x3 minimum for playable game)
    if (game->grid_width < 3) game->grid_width = 3;
    if (game->grid_height < 3) game->grid_height = 3;

    // Calculate actual field size
    lv_coord_t field_w = game->cell_size * game->grid_width;
    lv_coord_t field_h = game->cell_size * game->grid_height;

    lv_obj_set_size(game->container, field_w, field_h);
    lv_obj_center(game->container);

    // Initialize snake body at center
    int16_t start_x = game->grid_width / 2;
    int16_t start_y = game->grid_height / 2;
    game->head = snake_init_body(SNAKE_INITIAL_LENGTH, start_x, start_y);
    if (!game->head) {
        lv_obj_set_user_data(obj, NULL);
        lv_free(game);
        lv_obj_delete(obj);
        return NULL;
    }

    // Create LVGL objects for snake segments
    snake_create_segment_objects(game);

    // Create food object
    game->food = lv_obj_create(game->container);
    lv_obj_set_size(game->food, game->cell_size - 2, game->cell_size - 2);
    lv_obj_set_style_radius(game->food, game->cell_size / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(game->food, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(game->food, SNAKE_FOOD_COLOR, LV_PART_MAIN);

    // Initialize random seed only once
    if (!srand_initialized) {
        srand((unsigned int)time(NULL));
        srand_initialized = true;
    }

    // Spawn initial food
    snake_spawn_food(game);

    // Initial draw
    snake_draw(game);

    // Add event callbacks for touch gestures and keyboard
    lv_obj_add_event_cb(game->container, game_play_event, LV_EVENT_GESTURE, obj);
    lv_obj_add_event_cb(game->container, game_play_event, LV_EVENT_KEY, obj);
    lv_obj_add_event_cb(obj, delete_event, LV_EVENT_DELETE, NULL);

    // Set up keyboard focus if available
    if (tt_lvgl_hardware_keyboard_is_available()) {
        lv_group_t* group = lv_group_get_default();
        if (group) {
            lv_group_add_obj(group, game->container);
            // Register focus handlers to manage edit mode lifecycle
            lv_obj_add_event_cb(game->container, focus_event, LV_EVENT_FOCUSED, NULL);
            lv_obj_add_event_cb(game->container, focus_event, LV_EVENT_DEFOCUSED, NULL);
            // Focus the container (will trigger FOCUSED event and enable edit mode)
            lv_group_focus_obj(game->container);
        }
    }

    // Start game timer
    game->timer = lv_timer_create(snake_timer_cb, SNAKE_GAME_SPEED_MS, game);

    return obj;
}

/**
 * @brief Reset the game to start a new game
 */
void snake_set_new_game(lv_obj_t* obj) {
    snake_game_t* game = (snake_game_t*)lv_obj_get_user_data(obj);
    if (!game) return;

    // Stop timer during reset
    if (game->timer) {
        lv_timer_pause(game->timer);
    }

    // Delete old snake segment objects
    snake_delete_segment_objects(game);

    // Free old snake body
    snake_free_body(game->head);
    game->head = NULL;

    // Reset game state
    game->score = 0;
    game->length = SNAKE_INITIAL_LENGTH;
    game->direction = SNAKE_DIR_RIGHT;
    game->next_direction = SNAKE_DIR_RIGHT;
    game->game_over = false;
    game->paused = false;

    // Create new snake body at center
    int16_t start_x = game->grid_width / 2;
    int16_t start_y = game->grid_height / 2;
    game->head = snake_init_body(SNAKE_INITIAL_LENGTH, start_x, start_y);

    if (!game->head) {
        // Memory allocation failed - keep game in over state
        game->game_over = true;
        lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }

    // Create new segment objects
    snake_create_segment_objects(game);

    // Spawn new food and unhide it
    if (snake_spawn_food(game)) {
        if (game->food) {
            lv_obj_remove_flag(game->food, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Draw
    snake_draw(game);

    // Resume timer
    if (game->timer) {
        lv_timer_resume(game->timer);
    }

    // Notify change
    lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
}

/**
 * @brief Get the current score
 */
uint16_t snake_get_score(lv_obj_t* obj) {
    snake_game_t* game = (snake_game_t*)lv_obj_get_user_data(obj);
    if (!game) return 0;
    return game->score;
}

/**
 * @brief Get the current snake length
 */
uint16_t snake_get_length(lv_obj_t* obj) {
    snake_game_t* game = (snake_game_t*)lv_obj_get_user_data(obj);
    if (!game) return 0;
    return game->length;
}

/**
 * @brief Check if game is over
 */
bool snake_get_game_over(lv_obj_t* obj) {
    snake_game_t* game = (snake_game_t*)lv_obj_get_user_data(obj);
    if (!game) return true;
    return game->game_over;
}
