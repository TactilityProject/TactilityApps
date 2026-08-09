#pragma once

#include <lvgl.h>

struct Context;

// Accent colors matching M5Stack product image palette
constexpr lv_color_t COLOR_I2C  = LV_COLOR_MAKE(0x1A, 0x6E, 0xC8); // M5 blue
constexpr lv_color_t COLOR_GPIO = LV_COLOR_MAKE(0xC0, 0x20, 0x20); // red
constexpr lv_color_t COLOR_UART = LV_COLOR_MAKE(0x20, 0x90, 0x50); // green

// Creates a standard toolbar with a Back button that stops the active test view (if any) and
// returns to the list.
lv_obj_t* testViewCreateToolbar(lv_obj_t* parent, Context* app, const char* title);

// Creates a colored identity banner strip below the toolbar.
// ifaceBadge: short string e.g. "I2C", "GPIO", "UART"
void testViewCreateBanner(lv_obj_t* parent, const char* unitName,
                          const char* ifaceBadge, lv_color_t accentColor);
