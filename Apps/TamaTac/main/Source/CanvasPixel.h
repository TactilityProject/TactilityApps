#pragma once

#include <lvgl.h>
#include <cstdint>

// Backing store for one pixel of a LV_COLOR_FORMAT_NATIVE canvas.
#ifdef ESP_PLATFORM
using CanvasPixel = lv_color_t; // Large enough for the RGB565 of the devices
#else
using CanvasPixel = uint32_t; // The simulator renders up to 32 bits per pixel
#endif
