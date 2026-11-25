#pragma once

#include "DoomRenderer.h"

#include <tactility/device.h>

/**
 * Sets up the renderer and input, locates a WAD on the SD card,
 * and starts the doomgeneric engine. Runs the doomgeneric_Tick() loop until
 * doomMain_RequestStop() is called.
 *
 * Must run on a worker task, not the LVGL/UI task.
 */
void doomMain_Run(struct Device* display, const DoomScreen& screen);

/** @return why the last doomMain_Run() stopped early, or an empty string if it didn't */
const char* doomMain_GetError();

/** Signals the doom loop (running on another task) to exit. */
extern "C" void doomMain_RequestStop();
