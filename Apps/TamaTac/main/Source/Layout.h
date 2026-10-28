/**
 * @file Layout.h
 * @brief Shared responsive-layout helper for TamaTac views.
 *
 * Single source of truth for size detection. Uses display resolution
 * (parent may not be laid out yet on first load) plus orientation flags
 * so portrait-narrow screens (e.g. 135x240) can stack vertically while
 * landscape-short screens (e.g. 240x135) stay compact.
 *
 * Size classes (kept compatible with the legacy tiers):
 *   Small  - (W < 280 || H < 180): Cardputer 240x135, Sticks3 135x240, 170x320, 172x320
 *   Medium - default: 320x240, 240x320, 320x480, 480x222, 480x480, square 240x240
 *   Large  - (W >= 400 && H >= 300, not XLarge): 480x480, split views
 *   XLarge - (W >= 600): 800x480 RGB panels and similar
 *   XXLarge - (max(W,H) >= 1000): Tab5 720x1280, 1024x600 RGB, large EPD
 *
 * Ultra-small (80x160 tdongle, 128x64 OLED) is explicitly unsupported.
 */

#pragma once

#include <lvgl.h>

enum class TamaSizeClass {
    Small,
    Medium,
    Large,
    XLarge,
    XXLarge
};

struct TamaViewport {
    lv_coord_t width = 0;
    lv_coord_t height = 0;
    TamaSizeClass sizeClass = TamaSizeClass::Medium;
    bool isPortrait = false; // height > width
    bool isNarrow = false;   // width < 300 (e.g. 135-wide portrait)
    bool isShort = false;    // height < 250 (e.g. 135-high landscape)
};

inline TamaViewport tamaGetViewport() {
    TamaViewport vp;
    vp.width = lv_display_get_horizontal_resolution(nullptr);
    vp.height = lv_display_get_vertical_resolution(nullptr);

    if (vp.width < 280 || vp.height < 180) {
        vp.sizeClass = TamaSizeClass::Small;
    } else if (vp.width >= 1000 || vp.height >= 1000) {
        vp.sizeClass = TamaSizeClass::XXLarge;
    } else if (vp.width >= 600) {
        vp.sizeClass = TamaSizeClass::XLarge;
    } else if (vp.width >= 400 && vp.height >= 300) {
        vp.sizeClass = TamaSizeClass::Large;
    } else {
        vp.sizeClass = TamaSizeClass::Medium;
    }

    vp.isPortrait = vp.height > vp.width;
    vp.isNarrow = vp.width < 300;
    vp.isShort = vp.height < 250;
    return vp;
}

inline bool tamaIsSmall(const TamaViewport& vp) {
    return vp.sizeClass == TamaSizeClass::Small;
}

// "At least XLarge" - keeps pre-XXL call sites behaving on big screens.
inline bool tamaIsXLarge(const TamaViewport& vp) {
    return vp.sizeClass == TamaSizeClass::XLarge ||
        vp.sizeClass == TamaSizeClass::XXLarge;
}

inline bool tamaIsXXLarge(const TamaViewport& vp) {
    return vp.sizeClass == TamaSizeClass::XXLarge;
}

// Standard paddings shared by views (matches legacy values)
inline int tamaPadAll(const TamaViewport& vp) {
    switch (vp.sizeClass) {
        case TamaSizeClass::Small: return 4;
        case TamaSizeClass::XLarge:
        case TamaSizeClass::XXLarge: return 16;
        default: return 8;
    }
}

inline int tamaPadRow(const TamaViewport& vp) {
    switch (vp.sizeClass) {
        case TamaSizeClass::Small: return 4;
        case TamaSizeClass::XLarge:
        case TamaSizeClass::XXLarge: return 16;
        default: return 8;
    }
}

inline int tamaGap(const TamaViewport& vp) {
    switch (vp.sizeClass) {
        case TamaSizeClass::Small: return 4;
        case TamaSizeClass::XLarge:
        case TamaSizeClass::XXLarge: return 12;
        default: return 6;
    }
}

// Max content width for the capped tiers (XLarge/XXLarge) so bars/buttons
// don't stretch absurdly on wide screens. 0 = percent mode (tiers below XL
// follow the parent). Recompute callers need this LIVE (rotation changes
// the display resolution) - never cache it across orientation changes.
inline int tamaContentCap(const TamaViewport& vp) {
    if (vp.sizeClass == TamaSizeClass::XXLarge) {
        int capped = vp.width - 32;
        if (capped > 800) capped = 800;
        if (capped < 200) capped = 200;
        return capped;
    }
    if (vp.sizeClass == TamaSizeClass::XLarge) {
        int capped = vp.width - 32;
        if (capped > 720) capped = 720;
        if (capped < 200) capped = 200;
        return capped;
    }
    return 0;
}
