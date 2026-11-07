// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tabs.h"

#include <lvgl/theme.h>

#include <cstring>

namespace gpsviewer {

Theme makeTheme(bool night) {
    Theme theme {};
    theme.night = night;
    const bool mono = lvgl_theme_is_mono();
    if (night && mono) {
        // A 1-bit display has no dark reds: white on black, shapes carry the hierarchy
        theme.background = lv_color_black();
        theme.paintBackground = true;
        theme.border = lv_color_white();
        theme.accent = lv_color_white();
        theme.success = lv_color_white();
        theme.warning = lv_color_white();
        theme.text = lv_color_white();
        theme.dim = lv_color_white();
        theme.grid = lv_color_white();
        theme.mono = true;
        return theme;
    }
    if (night) {
        // Tactical red: one hue only, brightness carries the hierarchy
        theme.background = lv_color_hex(0x000000);
        theme.paintBackground = true;
        theme.border = lv_color_hex(0x380C0C);
        theme.accent = lv_color_hex(0xFF2A2A);
        theme.success = lv_color_hex(0xFF2A2A);
        theme.warning = lv_color_hex(0xB81F1F);
        theme.text = lv_color_hex(0xFF2A2A);
        theme.dim = lv_color_hex(0x7A1E1E);
        theme.grid = lv_color_hex(0x4A1212);
        return theme;
    }
    // The theme's own tones (Material 3 neutrals of the Tactility theme), for what the app draws itself
    LvglThemeSettings settings;
    lvgl_theme_get_settings(&settings);
    const bool dark = settings.is_dark;
    theme.background = dark ? lv_color_hex(0x21222B) : lv_color_hex(0xF3F3F3);
    if (settings.surface_override) {
        theme.background = settings.color_surface;
    }
    theme.paintBackground = false;
    theme.text = dark ? lv_color_hex(0xE2E2E2) : lv_color_hex(0x1C1C1C);
    theme.dim = dark ? lv_color_hex(0x919191) : lv_color_hex(0x5E5E5E);
    theme.border = dark ? lv_color_hex(0x3A3B44) : lv_color_hex(0xD6D6D6);
    theme.grid = dark ? lv_color_hex(0x4A4B55) : lv_color_hex(0xC6C6C6);
    theme.accent = mono ? theme.text : settings.color_primary;
    theme.success = mono ? theme.text : lv_palette_main(LV_PALETTE_GREEN);
    theme.warning = mono ? theme.dim : lv_palette_main(LV_PALETTE_AMBER);
    theme.mono = mono;
    return theme;
}

lv_color_t snrColor(int snr, const Theme& theme) {
    if (snr >= 35) {
        return theme.success;
    }
    if (snr >= 25) {
        return theme.warning;
    }
    return theme.dim;
}

lv_color_t systemColor(GnssSystem system, const Theme& theme) {
    if (theme.mono) {
        return theme.text;
    }
    if (theme.night) {
        switch (system) {
            case GnssSystem::Gps:
            case GnssSystem::Glonass:
            case GnssSystem::Galileo:
            case GnssSystem::Beidou:
            case GnssSystem::Qzss:
                return theme.text;
            default:
                return theme.warning;
        }
    }
    switch (system) {
        case GnssSystem::Gps: return lv_color_hex(0x3B82F6);
        case GnssSystem::Glonass: return lv_color_hex(0xF59E0B);
        case GnssSystem::Galileo: return lv_color_hex(0x06B6D4);
        case GnssSystem::Beidou: return lv_color_hex(0xA855F7);
        case GnssSystem::Qzss: return lv_color_hex(0xEC4899);
        case GnssSystem::Sbas: return lv_color_hex(0xA3E635);
        case GnssSystem::Navic: return lv_color_hex(0xFB7185);
        default: return lv_color_hex(0x8E9BAE);
    }
}

void setLabelText(lv_obj_t* label, const char* text) {
    if (label == nullptr) {
        return;
    }
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

} // namespace gpsviewer
