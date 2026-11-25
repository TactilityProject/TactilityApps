#pragma once

#include <tactility/device.h>

#include <cstdint>

/**
 * How the user sees the display, captured from LVGL before it's stopped (see main.cpp).
 *
 * Rotations are in quarter turns counter-clockwise (LVGL's and the PPA's convention), from the
 * user's view to the coordinate space in question.
 */
struct DoomScreen {
    /** Rotation from the user's view to display_draw_bitmap() coordinates. */
    int drawRotation;
    /** Size of the display_draw_bitmap() coordinate space. */
    uint32_t drawWidth;
    uint32_t drawHeight;
    /** Rotation from the user's view to the touch driver's raw coordinates. */
    int touchRotation;
    /** Size of the touch driver's raw coordinate space. */
    uint32_t touchWidth;
    uint32_t touchHeight;
};

/** What a touch landed on (see doomRenderer_MapTouch()). */
enum class DoomTouchTarget {
    None,
    Game, // DOOM's 320x200 screen
    Panel, // the control panel canvas (320 x doomRenderer_PanelHeight())
};

/**
 * Sets up the renderer and computes the layout: the 320x200 game image scaled to fit (never
 * stretched) and centred, or, when allowPanel is set and the user's view is portrait with enough
 * room, the game at the top and a control panel canvas below it.
 */
bool doomRenderer_Init(Device* display, const DoomScreen& screen, bool allowPanel);

void doomRenderer_Shutdown();

/** @return the control panel canvas height in DOOM pixels, or 0 when there is no panel */
int doomRenderer_PanelHeight();

/**
 * Draws a frame. Both images are 8-bit, 320 pixels wide, indexed into the engine's current palette.
 * @param[in] game the 320x200 game frame
 * @param[in] panel the control panel canvas, or nullptr when there is no panel
 */
void doomRenderer_Draw(const uint8_t* game, const uint8_t* panel);

/**
 * Maps a raw touch driver coordinate onto the game or the control panel.
 * @param[out] outX x in the target's 320-wide coordinate space
 * @param[out] outY y in the target's coordinate space
 */
DoomTouchTarget doomRenderer_MapTouch(int touchX, int touchY, int* outX, int* outY);
