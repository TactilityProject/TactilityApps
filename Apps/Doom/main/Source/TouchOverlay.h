#pragma once

// On-screen touch controls.
//
// Drawn into an 8-bit palettised canvas 320 pixels wide (DOOM's own screen width), in one of two
// layouts:
// - Overlay: drawn over the 320x200 game frame. The buttons stay above the status bar (the bottom
//   32 rows), so the health/ammo display is never covered.
// - Panel: a separate canvas shown below the game (portrait screens), with larger controls.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of on-screen buttons. A pressed-button set is a bitmask of (1 << index). */
#define TOUCH_OVERLAY_BUTTON_COUNT 9

/** The smallest panel canvas height that fits the panel layout. */
#define TOUCH_OVERLAY_MIN_PANEL_HEIGHT 170

/**
 * Selects the layout. Must be called before the other functions.
 * @param[in] panel 0 for the overlay layout, 1 for the panel layout
 * @param[in] height the canvas height: 200 for the overlay, 170-200 for the panel
 */
void touchOverlay_SetLayout(int panel, int height);

/** Draws the buttons into the canvas (320 x height). In panel mode the canvas is cleared first. */
void touchOverlay_Draw(uint8_t* frame, uint32_t pressedMask);

/** Draws text in DOOM's HUD font into a 320-wide canvas, right-aligned at rightX. */
void touchOverlay_DrawText(uint8_t* frame, int rightX, int y, const char* text);

/**
 * @return the mask of buttons pressed by a touch at the given canvas coordinate, or 0.
 * Usually one button; the D-pad's diagonal slices press two directions at once.
 */
uint32_t touchOverlay_HitTest(int x, int y);

/**
 * @return the DOOM key a button sends when pressed right now. Depends on the game state: in menus
 * Fire confirms and Use goes back, and the weapon button picks the next owned weapon.
 */
unsigned char touchOverlay_KeyForButton(int button);

#ifdef __cplusplus
}
#endif
