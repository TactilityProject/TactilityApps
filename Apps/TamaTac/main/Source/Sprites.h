/**
 * @file Sprites.h
 * @brief Sprite types, enums, and icon data for TamaTac pet
 *
 * Pet sprites are 24x24 RGB565 with animation support (in SpriteData.h).
 * UI icons remain 8x8 monochrome (defined here).
 */
#pragma once

#include <cstdint>

// ── Sprite Dimensions ────────────────────────────────────────────────────────

constexpr int SPRITE_WIDTH = 24;
constexpr int SPRITE_HEIGHT = 24;
constexpr int SPRITE_PIXELS = SPRITE_WIDTH * SPRITE_HEIGHT;  // 576

constexpr int ICON_WIDTH = 8;
constexpr int ICON_HEIGHT = 8;

// Transparent color key (magenta in RGB565)
constexpr uint16_t SPRITE_TRANSPARENT = 0xF81F;

// ── Pet Sprite IDs ───────────────────────────────────────────────────────────

enum SpriteId {
    SPRITE_EGG_IDLE,
    SPRITE_BABY_IDLE,
    SPRITE_TEEN_IDLE,
    SPRITE_ADULT_IDLE,
    SPRITE_ELDER_IDLE,
    SPRITE_GHOST,
    SPRITE_SICK,
    SPRITE_HAPPY,
    SPRITE_SAD,
    SPRITE_EATING,
    SPRITE_PLAYING,
    SPRITE_SLEEPING,

    PET_SPRITE_COUNT  // Number of animated pet sprites
};

// ── Icon IDs ─────────────────────────────────────────────────────────────────

enum IconId {
    ICON_POOP,
    ICON_HUNGER,
    ICON_HAPPINESS,
    ICON_HEALTH,
    ICON_ENERGY,
    ICON_FEED,
    ICON_PLAY,
    ICON_MEDICINE,
    ICON_SLEEP,

    ICON_COUNT
};

// ── Animation Structures ─────────────────────────────────────────────────────

struct AnimFrame {
    const uint16_t* data;  // Pointer to RGB565 pixel array (576 uint16_t)
};

struct AnimatedSprite {
    const AnimFrame* frames;
    uint8_t frameCount;
    uint16_t frameDelayMs;
    bool loop;
};

// ── Icon Structure (8x8 monochrome) ─────────────────────────────────────────

struct Icon {
    const uint8_t* data;
    int width;
    int height;
};

// ── Animated Sprite Lookup ───────────────────────────────────────────────────

extern const AnimatedSprite animatedSprites[PET_SPRITE_COUNT];

inline const AnimatedSprite& getAnimSprite(SpriteId id) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= PET_SPRITE_COUNT) idx = 0;
    return animatedSprites[idx];
}

// ── Icon Data (8x8 monochrome, defined inline) ──────────────────────────────

constexpr uint8_t icon_poop[] = {
    0x00, 0x18, 0x3C, 0x7E, 0x7E, 0xFF, 0xFF, 0x00,
};

constexpr uint8_t icon_hunger[] = {
    0x18, 0x08, 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x3C,
};

constexpr uint8_t icon_happiness[] = {
    0x00, 0x66, 0x66, 0x00, 0x81, 0x42, 0x3C, 0x00,
};

constexpr uint8_t icon_health[] = {
    0x00, 0x66, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00,
};

constexpr uint8_t icon_energy[] = {
    0x18, 0x1C, 0x0E, 0xFF, 0x70, 0x38, 0x18, 0x00,
};

constexpr uint8_t icon_feed[] = {
    0x54, 0x54, 0x54, 0x7C, 0x10, 0x10, 0x10, 0x10,
};

constexpr uint8_t icon_play[] = {
    0x3C, 0x7E, 0xDB, 0xFF, 0xDB, 0x7E, 0x3C, 0x00,
};

constexpr uint8_t icon_medicine[] = {
    0x18, 0x18, 0x7E, 0xFF, 0x7E, 0x18, 0x18, 0x00,
};

constexpr uint8_t icon_sleep[] = {
    0x1C, 0x38, 0x70, 0x60, 0x70, 0x38, 0x1C, 0x00,
};

inline const Icon icons[ICON_COUNT] = {
    {icon_poop,      ICON_WIDTH, ICON_HEIGHT},
    {icon_hunger,    ICON_WIDTH, ICON_HEIGHT},
    {icon_happiness, ICON_WIDTH, ICON_HEIGHT},
    {icon_health,    ICON_WIDTH, ICON_HEIGHT},
    {icon_energy,    ICON_WIDTH, ICON_HEIGHT},
    {icon_feed,      ICON_WIDTH, ICON_HEIGHT},
    {icon_play,      ICON_WIDTH, ICON_HEIGHT},
    {icon_medicine,  ICON_WIDTH, ICON_HEIGHT},
    {icon_sleep,     ICON_WIDTH, ICON_HEIGHT},
};

inline const Icon& getIcon(IconId id) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= ICON_COUNT) idx = 0;
    return icons[idx];
}
