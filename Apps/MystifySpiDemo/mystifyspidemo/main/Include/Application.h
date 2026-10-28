#pragma once

#include <input/AnyInput.h>

struct Device;

/**
 * Runs the SPI display demo until the user provides input or closes the app.
 *
 * @param display the primary display device, used to mirror the scene while LVGL is stopped
 * @return nullptr on success, or a message to show in an alert dialog when the display could not
 *         be brought up.
 */
const char* runApplication(struct Device* display, AnyInput& input);
