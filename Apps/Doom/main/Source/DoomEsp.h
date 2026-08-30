#pragma once

#include <tactility/device.h>

/**
 * Initializes the PPA scale/rotate pipeline, locates a WAD on the SD card,
 * and starts the doomgeneric engine. Runs the doomgeneric_Tick() loop until
 * doomEsp_RequestStop() is called.
 *
 * Must run on a worker task, not the LVGL/UI task.
 */
void doomEsp_Run(struct Device* display);

/** Signals the doom loop (running on another task) to exit. */
extern "C" void doomEsp_RequestStop();
