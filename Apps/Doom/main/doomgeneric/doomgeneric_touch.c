#include "TouchOverlay.h"

#include "doomdef.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_player.h"
#include "i_video.h"
#include "m_controls.h"
#include "st_stuff.h"
#include "v_video.h"

#include <string.h>

// From m_menu.c
extern boolean messageNeedsInput;
void M_WriteText(int x, int y, char* string);
int M_StringWidth(char* string);

// Palette indices in PLAYPAL's base palette
#define COLOUR_BLACK 0
#define COLOUR_WHITE 4
#define COLOUR_RED 176

enum Button {
    BUTTON_UP,
    BUTTON_DOWN,
    BUTTON_LEFT,
    BUTTON_RIGHT,
    BUTTON_FIRE,
    BUTTON_USE,
    BUTTON_WEAPON,
    BUTTON_MENU,
    BUTTON_MAP,
};

typedef struct {
    short x, y, w, h;
    const char* label;
} ButtonRect;

// Dark grey from PLAYPAL's grey ramp (80 is near white, 111 near black): the button fill in
// panel mode, where there's no game image behind the buttons
#define COLOUR_PANEL_BUTTON 106

// Extra radius accepted outside the drawn D-pad ring when hit-testing
#define DPAD_HIT_MARGIN 6

// The current layout (see touchOverlay_SetLayout()).
//
// D-pad: a donut split like a pie into eight 45-degree slices. The four cardinal slices press one
// direction; the slices between them press both neighbours (e.g. up-right walks forward while
// turning right). The centre hole does nothing, so a thumb can rest there.
// The D-pad entries in buttons[] only hold the arrow label positions; the donut is drawn and
// hit-tested separately.
static ButtonRect buttons[TOUCH_OVERLAY_BUTTON_COUNT];
static int dpadCx, dpadCy, dpadOuter, dpadInner;
static int panelMode = 0;
static int canvasHeight = SCREENHEIGHT;

static void setButton(int button, int x, int y, int w, int h, const char* label) {
    buttons[button].x = (short)x;
    buttons[button].y = (short)y;
    buttons[button].w = (short)w;
    buttons[button].h = (short)h;
    buttons[button].label = label;
}

static void placeDpadLabels(void) {
    // Arrow labels centred in the cardinal slices
    const int r = (dpadOuter + dpadInner) / 2;
    setButton(BUTTON_UP, dpadCx - 8, dpadCy - r - 8, 16, 16, "^");
    setButton(BUTTON_DOWN, dpadCx - 8, dpadCy + r - 8, 16, 16, "V");
    setButton(BUTTON_LEFT, dpadCx - r - 8, dpadCy - 8, 16, 16, "<");
    setButton(BUTTON_RIGHT, dpadCx + r - 8, dpadCy - 8, 16, 16, ">");
}

void touchOverlay_SetLayout(int panel, int height) {
    panelMode = panel;
    canvasHeight = height;
    if (!panel) {
        // Over the game: everything ends at or above ST_Y (168), so the status bar stays visible.
        // D-pad bottom-left, actions bottom-right, menu/map in the top corners.
        dpadCx = 46;
        dpadCy = 126;
        dpadOuter = 40;
        dpadInner = 12;
        setButton(BUTTON_FIRE, 266, 116, 50, 50, "FIRE");
        setButton(BUTTON_USE, 226, 134, 32, 32, "USE");
        setButton(BUTTON_WEAPON, 274, 80, 32, 32, "WPN");
        setButton(BUTTON_MENU, 2, 2, 44, 16, "MENU");
        setButton(BUTTON_MAP, 274, 2, 44, 16, "MAP");
    } else {
        // Panel below the game (portrait): a large D-pad on the left, the action buttons stacked
        // on the right, menu/map in the gap between them
        const int cy = height / 2;
        dpadOuter = height / 2 - 6 < 72 ? height / 2 - 6 : 72;
        dpadInner = dpadOuter * 3 / 10;
        dpadCx = dpadOuter + 8;
        dpadCy = cy;
        setButton(BUTTON_FIRE, 236, cy - 26, 72, 72, "FIRE");
        setButton(BUTTON_WEAPON, 270, cy - 74, 40, 40, "WPN");
        setButton(BUTTON_USE, 198, cy + 28, 40, 40, "USE");
        setButton(BUTTON_MENU, 156, 4, 44, 18, "MENU");
        setButton(BUTTON_MAP, 156, height - 22, 44, 18, "MAP");
    }
    placeDpadLabels();
}

static void fillChecker(uint8_t* frame, const ButtonRect* b, uint8_t colour) {
    for (int y = b->y; y < b->y + b->h; y++) {
        uint8_t* row = frame + y * SCREENWIDTH;
        for (int x = b->x + ((y ^ b->x) & 1); x < b->x + b->w; x += 2) {
            row[x] = colour;
        }
    }
}

static void fillSolid(uint8_t* frame, const ButtonRect* b, uint8_t colour) {
    for (int y = b->y; y < b->y + b->h; y++) {
        memset(frame + y * SCREENWIDTH + b->x, colour, b->w);
    }
}

static void outline(uint8_t* frame, const ButtonRect* b, uint8_t colour) {
    memset(frame + b->y * SCREENWIDTH + b->x, colour, b->w);
    memset(frame + (b->y + b->h - 1) * SCREENWIDTH + b->x, colour, b->w);
    for (int y = b->y; y < b->y + b->h; y++) {
        frame[y * SCREENWIDTH + b->x] = colour;
        frame[y * SCREENWIDTH + b->x + b->w - 1] = colour;
    }
}

static int isDpadButton(int button) {
    return button <= BUTTON_RIGHT;
}

// Everything but MENU and MAP is a circle, inscribed in its (square) rectangle
static int isRoundButton(int button) {
    return button != BUTTON_MENU && button != BUTTON_MAP;
}

// Circle test on pixel centres, in doubled coordinates: 0 outside, 1 inside, 2 on the edge
static int circlePixel(const ButtonRect* b, int x, int y) {
    const int d = b->w; // diameter
    const int ddx = 2 * (x - b->x) + 1 - d;
    const int ddy = 2 * (y - b->y) + 1 - d;
    const int d2 = ddx * ddx + ddy * ddy;
    if (d2 >= d * d) {
        return 0;
    }
    return d2 >= (d - 2) * (d - 2) ? 2 : 1;
}

// Fills a round button (colour >= 0, every other pixel when checker) and draws its white edge
static void drawCircle(uint8_t* frame, const ButtonRect* b, int colour, int checker) {
    for (int y = b->y; y < b->y + b->h; y++) {
        uint8_t* row = frame + y * SCREENWIDTH;
        for (int x = b->x; x < b->x + b->w; x++) {
            const int c = circlePixel(b, x, y);
            if (c == 2) {
                row[x] = COLOUR_WHITE;
            } else if (c == 1 && colour >= 0 && (!checker || ((x ^ y) & 1) == 0)) {
                row[x] = (uint8_t)colour;
            }
        }
    }
}

// tan(22.5 degrees) * 1000: slice boundaries lie halfway between the axes and the diagonals
#define TAN_22_5_X1000 414

// @return the button mask of the D-pad slice at an offset from the D-pad centre
static uint32_t dpadSliceMask(int dx, int dy) {
    const int ax = dx < 0 ? -dx : dx;
    const int ay = dy < 0 ? -dy : dy;
    const uint32_t horizontal = 1u << (dx < 0 ? BUTTON_LEFT : BUTTON_RIGHT);
    const uint32_t vertical = 1u << (dy < 0 ? BUTTON_UP : BUTTON_DOWN);
    if (ay * 1000 < ax * TAN_22_5_X1000) {
        return horizontal;
    }
    if (ax * 1000 < ay * TAN_22_5_X1000) {
        return vertical;
    }
    return horizontal | vertical;
}

// Only the pressed slice is drawn (as feedback); otherwise the D-pad is just its four arrows
static void drawDpad(uint8_t* frame, uint32_t pressedMask) {
    const uint32_t dpadMask = (1u << BUTTON_UP) | (1u << BUTTON_DOWN) | (1u << BUTTON_LEFT) | (1u << BUTTON_RIGHT);
    // Only the slice matching the pressed directions exactly lights up, so a diagonal press
    // highlights the diagonal slice rather than its two neighbours as well
    const uint32_t pressedDirections = pressedMask & dpadMask;
    if (pressedDirections == 0) {
        return;
    }
    const int outer2 = dpadOuter * dpadOuter;
    const int inner2 = dpadInner * dpadInner;
    for (int dy = -dpadOuter; dy < dpadOuter; dy++) {
        uint8_t* row = frame + (dpadCy + dy) * SCREENWIDTH + dpadCx;
        for (int dx = -dpadOuter; dx < dpadOuter; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 < outer2 && d2 >= inner2 && dpadSliceMask(dx, dy) == pressedDirections) {
                row[dx] = COLOUR_RED;
            }
        }
    }
}

void touchOverlay_Draw(uint8_t* frame, uint32_t pressedMask) {
    if (panelMode) {
        memset(frame, COLOUR_BLACK, SCREENWIDTH * canvasHeight);
    }
    V_UseBuffer(frame);
    drawDpad(frame, pressedMask);
    for (int i = 0; i < TOUCH_OVERLAY_BUTTON_COUNT; i++) {
        const ButtonRect* b = &buttons[i];
        const int pressed = (pressedMask & (1u << i)) != 0;
        if (isDpadButton(i)) {
            // Just a small circle around the arrow: the touch area is the whole donut slice
            drawCircle(frame, b, -1, 0);
        } else if (isRoundButton(i)) {
            // Over the game every other pixel is darkened, so the game stays visible through it
            drawCircle(frame, b, pressed ? COLOUR_RED : (panelMode ? COLOUR_PANEL_BUTTON : COLOUR_BLACK), !pressed && !panelMode);
        } else {
            if (pressed) {
                fillSolid(frame, b, COLOUR_RED);
            } else if (panelMode) {
                fillSolid(frame, b, COLOUR_PANEL_BUTTON);
            } else {
                fillChecker(frame, b, COLOUR_BLACK);
            }
            outline(frame, b, COLOUR_WHITE);
        }

        // The HUD font is 7px tall
        char* label = (char*)b->label;
        const int textX = b->x + (b->w - M_StringWidth(label)) / 2;
        const int textY = b->y + (b->h - 7) / 2;
        M_WriteText(textX, textY, label);
    }
    V_RestoreBuffer();
}

void touchOverlay_DrawText(uint8_t* frame, int rightX, int y, const char* text) {
    V_UseBuffer(frame);
    M_WriteText(rightX - M_StringWidth((char*)text), y, (char*)text);
    V_RestoreBuffer();
}

uint32_t touchOverlay_HitTest(int x, int y) {
    // D-pad donut: the centre hole is dead, the ring is split into eight slices
    const int dx = x - dpadCx;
    const int dy = y - dpadCy;
    const int d2 = dx * dx + dy * dy;
    const int hitOuter = dpadOuter + DPAD_HIT_MARGIN;
    if (d2 < hitOuter * hitOuter) {
        return d2 < dpadInner * dpadInner ? 0 : dpadSliceMask(dx, dy);
    }

    // Slightly generous hit areas: fingers are much less precise than the drawn outlines
    const int margin = 3;
    for (int i = BUTTON_RIGHT + 1; i < TOUCH_OVERLAY_BUTTON_COUNT; i++) {
        const ButtonRect* b = &buttons[i];
        if (isRoundButton(i)) {
            const int r = b->w / 2 + margin;
            const int dx2 = x - (b->x + b->w / 2);
            const int dy2 = y - (b->y + b->h / 2);
            if (dx2 * dx2 + dy2 * dy2 < r * r) {
                return 1u << i;
            }
        } else if (x >= b->x - margin && x < b->x + b->w + margin &&
            y >= b->y - margin && y < b->y + b->h + margin) {
            return 1u << i;
        }
    }
    return 0;
}

// Weapon slots as selected by the number keys. Slot 1 holds fist and chainsaw, slot 3 the
// shotgun and super shotgun.
static int weaponSlot(weapontype_t weapon) {
    switch (weapon) {
        case wp_fist:
        case wp_chainsaw: return 1;
        case wp_pistol: return 2;
        case wp_shotgun:
        case wp_supershotgun: return 3;
        case wp_chaingun: return 4;
        case wp_missile: return 5;
        case wp_plasma: return 6;
        case wp_bfg: return 7;
        default: return 1;
    }
}

static boolean ownsSlot(const player_t* player, int slot) {
    switch (slot) {
        case 1: return true; // fist is always available
        case 2: return player->weaponowned[wp_pistol];
        case 3: return player->weaponowned[wp_shotgun] || player->weaponowned[wp_supershotgun];
        case 4: return player->weaponowned[wp_chaingun];
        case 5: return player->weaponowned[wp_missile];
        case 6: return player->weaponowned[wp_plasma];
        case 7: return player->weaponowned[wp_bfg];
        default: return false;
    }
}

static unsigned char nextWeaponKey(void) {
    if (gamestate != GS_LEVEL) {
        return 0;
    }
    const player_t* player = &players[consoleplayer];
    // A pending switch counts as current, so repeated taps keep cycling
    weapontype_t current = player->pendingweapon != wp_nochange ? player->pendingweapon : player->readyweapon;
    const int slot = weaponSlot(current);
    for (int i = 1; i <= 7; i++) {
        const int candidate = (slot - 1 + i) % 7 + 1;
        if (ownsSlot(player, candidate)) {
            return (unsigned char)('0' + candidate);
        }
    }
    return 0;
}

unsigned char touchOverlay_KeyForButton(int button) {
    switch (button) {
        case BUTTON_UP: return KEY_UPARROW;
        case BUTTON_DOWN: return KEY_DOWNARROW;
        case BUTTON_LEFT: return KEY_LEFTARROW;
        case BUTTON_RIGHT: return KEY_RIGHTARROW;
        case BUTTON_FIRE:
            if (messageNeedsInput) return (unsigned char)key_menu_confirm;
            return menuactive ? KEY_ENTER : KEY_FIRE;
        case BUTTON_USE:
            if (messageNeedsInput) return (unsigned char)key_menu_abort;
            return menuactive ? KEY_BACKSPACE : KEY_USE;
        case BUTTON_WEAPON: return menuactive ? 0 : nextWeaponKey();
        case BUTTON_MENU: return KEY_ESCAPE;
        case BUTTON_MAP: return menuactive ? 0 : KEY_TAB;
        default: return 0;
    }
}
