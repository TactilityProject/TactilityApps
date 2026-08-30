#pragma once

/**
 * Starts the 16-channel software mixer and opens an output audio stream.
 * Safe to call even if no audio stream device is present - sound will
 * simply remain silent and Sound_StartSound() etc. become no-ops.
 *
 * Must be called before doomgeneric_Create() so DG_sound_module is ready
 * to receive Sound_Init().
 */
void doomAudio_Init();

/** Stops the mixer task and closes the output stream. */
void doomAudio_Shutdown();
