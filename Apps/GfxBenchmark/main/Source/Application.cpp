#include "Application.h"

#include <esp_log.h>

#include <app/event.h>
#include <tactility/check.h>

#include <Tactility/kernel/Kernel.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

constexpr auto TAG = "GfxBenchmark";

/** How long each phase stays on screen after it has been measured. Not part of the timing. */
constexpr uint32_t PHASE_PREVIEW_MS = 250;

namespace {

constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t RED = 0xF800;
constexpr uint16_t GREEN = 0x07E0;
constexpr uint16_t BLUE = 0x001F;
constexpr uint16_t YELLOW = 0xFFE0;
constexpr uint16_t CYAN = 0x07FF;
constexpr uint16_t MAGENTA = 0xF81F;
constexpr uint16_t ORANGE = 0xFD20;
constexpr uint16_t GREY = 0x8410;
constexpr uint16_t DARK_STRIPE = 0x0841;

constexpr int PHASE_COUNT = 17;

static const char* const PHASE_NAMES[PHASE_COUNT] = {
    "Screen fill",
    "Text",
    "Pixels",
    "Lines",
    "H/V lines",
    "Rectangles (filled)",
    "Rectangles (outline)",
    "Triangles (filled)",
    "Triangles (outline)",
    "Circles (filled)",
    "Circles (outline)",
    "Arcs (filled)",
    "Arcs (outline)",
    "Round rects (filled)",
    "Round rects (outline)",
    "Sprites",
    "Polygons",
};

// Short enough for the results table on a 128 px wide display.
static const char* const PHASE_SHORT_NAMES[PHASE_COUNT] = {
    "Fill",
    "Text",
    "Pixels",
    "Lines",
    "H/V",
    "RectF",
    "Rect",
    "TriF",
    "Tri",
    "CircF",
    "Circ",
    "ArcF",
    "Arc",
    "RoundF",
    "Round",
    "Sprite",
    "Poly",
};

struct Context {
    int width;
    int height;
    int minDimension;
    int step;
    gfx::RenderMode mode;
    gfx::Canvas* sprite = nullptr;
};

struct PhaseResult {
    const char* name;
    const char* shortName;
    int drawUs;
    int presentUs;
    int totalUs;
    int dirtyPixels;
    int dirtyRects;
};

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

int clampInt(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

const char* modeName(gfx::RenderMode mode) {
    switch (mode) {
        case gfx::RenderMode::Flip:
            return "flip";
        case gfx::RenderMode::FullFrame:
            return "full";
        case gfx::RenderMode::Banded:
            return "banded";
        case gfx::RenderMode::Transform:
            return "transform";
        case gfx::RenderMode::None:
        default:
            return "none";
    }
}

int titleScaleFor(const Context& context) {
    if (context.width >= 600 && context.height >= 300) {
        return 3;
    }
    if (context.width >= 220 && context.height >= 140) {
        return 2;
    }
    return 1;
}

int tableScaleFor(const Context& context) {
    if (context.height >= 800 && context.width >= 500) {
        return 3;
    }
    if (context.height >= 420 && context.width >= 320) {
        return 2;
    }
    return 1;
}

bool takeExitRequest(struct AppEventSubscription& subscription, AnyInput& input) {
    bool shouldExit = false;
    struct AppEvent event {};
    while (app_event_poll(&subscription, &event) == ERROR_NONE) {
        if (event.type == APP_EVENT_CLOSE) {
            shouldExit = true;
        }
    }
    if (input.activity()) {
        shouldExit = true;
    }
    return shouldExit;
}

void waitForExit(struct AppEventSubscription& subscription, AnyInput& input) {
    while (!takeExitRequest(subscription, input)) {
        tt::kernel::delayTicks(tt::kernel::millisToTicks(50));
    }
}

// ---------------------------------------------------------------------------
// Phase drawings
//
// The phases mirror the M5UnitTest LCD GFX test (PDQ): each one fills the screen with a different
// primitive. Sizes are adapted to the display so a 135x240 panel and a 720x1280 panel both finish
// in a reasonable time, while still doing enough work to be meaningful.
// ---------------------------------------------------------------------------

void phaseFillScreen(gfx::Surface& surface, const Context&) {
    surface.clear(WHITE);
    surface.clear(RED);
    surface.clear(GREEN);
    surface.clear(BLUE);
    surface.clear(BLACK);
}

void phaseText(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);

    const int margin = clampInt(context.minDimension / 60, 2, 8);
    const int titleScale = titleScaleFor(context);
    const char* titleText = context.width >= 120 ? "Gfx Benchmark" : "Gfx";

    gfx::TextStyle title;
    title.font = &gfx::FONT_8X8;
    title.foreground = MAGENTA;
    title.align = gfx::TextAlign::Center;
    title.scale = static_cast<uint8_t>(titleScale);
    surface.drawText(context.width / 2, margin, titleText, title);

    int y = margin + gfx::lineHeight(gfx::FONT_8X8, static_cast<uint8_t>(titleScale)) + margin;

    static const char* const lines[] = {
        "Hello World!",
        "RED GREEN BLUE",
        "1234.56",
        "0xDEADBEEF",
        "The quick brown fox",
        "jumps over the lazy dog",
        "Pack my box with five",
        "dozen liquor jugs.",
    };
    static const uint16_t colors[] = {
        WHITE, RED, YELLOW, WHITE, WHITE, GREEN, CYAN, ORANGE,
    };

    const int bodyScale = (context.height >= 480 && context.width >= 320) ? 2 : 1;
    const int bodyLineHeight = gfx::lineHeight(gfx::FONT_5X7, static_cast<uint8_t>(bodyScale));
    for (int i = 0; i < static_cast<int>(sizeof(lines) / sizeof(lines[0])); i++) {
        if (y + bodyLineHeight > context.height - margin) {
            break;
        }
        surface.drawText(margin, y, lines[i], gfx::FONT_5X7, colors[i], BLACK, static_cast<uint8_t>(bodyScale));
        y += bodyLineHeight;
    }

    const int largeScale = context.width >= 480 ? 2 : 1;
    const int largeLineHeight = gfx::lineHeight(gfx::FONT_8X8, static_cast<uint8_t>(largeScale));
    if (y + largeLineHeight <= context.height - margin) {
        surface.drawText(margin, y, "AaBbCc 0123", gfx::FONT_8X8, CYAN, BLACK, static_cast<uint8_t>(largeScale));
        y += largeLineHeight;
    }

    const int terminusLineHeight = gfx::lineHeight(gfx::FONT_8X16);
    if (y + terminusLineHeight <= context.height - margin) {
        surface.drawText(margin, y, "Terminus 8x16", gfx::FONT_8X16, GREEN, BLACK, 1);
        y += terminusLineHeight;
    }

    if (context.height - margin - y >= bodyLineHeight * 2 && context.width >= 120) {
        gfx::TextStyle wrap;
        wrap.font = &gfx::FONT_5X7_PROPORTIONAL;
        wrap.foreground = WHITE;
        wrap.scale = static_cast<uint8_t>(bodyScale);
        wrap.wrap = gfx::TextWrap::Word;
        surface.drawText(
            gfx::Rect { margin, y, context.width - margin, context.height - margin },
            "The quick brown fox jumps over the lazy dog. Wrapped text exercises the layout path.",
            wrap
        );
    }
}

void phasePixels(gfx::Surface& surface, const Context&) {
    surface.clear(BLACK);

    const int width = surface.width();
    const int yEnd = surface.originY() + surface.height();
    for (int y = surface.originY(); y < yEnd; y++) {
        for (int x = 0; x < width; x++) {
            const uint16_t color = static_cast<uint16_t>(
                ((x & 0x1F) << 11) | ((y & 0x3F) << 5) | ((x ^ y) & 0x1F)
            );
            surface.pixel(x, y, color);
        }
    }
}

void phaseLines(gfx::Surface& surface, const Context& context) {
    auto sweep = [&](int x0, int y0, int x1, int y1, uint16_t color) {
        for (int x = 0; x < context.width; x += context.step) {
            surface.line(x0, y0, x, y1, color);
        }
        for (int y = 0; y < context.height; y += context.step) {
            surface.line(x0, y0, x1, y, color);
        }
    };

    surface.clear(BLACK);
    sweep(0, 0, context.width - 1, context.height - 1, BLUE);
    surface.clear(BLACK);
    sweep(context.width - 1, 0, 0, context.height - 1, GREEN);
    surface.clear(BLACK);
    sweep(0, context.height - 1, context.width - 1, 0, RED);
    surface.clear(BLACK);
    sweep(context.width - 1, context.height - 1, 0, 0, YELLOW);
}

void phaseFastLines(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    for (int y = 0; y < context.height; y += context.step) {
        surface.hline(0, y, context.width, RED);
    }
    for (int x = 0; x < context.width; x += context.step) {
        surface.vline(x, 0, context.height, BLUE);
    }
}

void phaseFilledRects(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int cx = context.width / 2;
    const int cy = context.height / 2;
    for (int i = context.minDimension; i > 0; i -= context.step) {
        const int half = i / 2;
        const uint8_t shade = static_cast<uint8_t>(std::min(i, 255));
        surface.fillRect(cx - half, cy - half, i, i, rgb565(shade, shade, 0));
    }
}

void phaseRects(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int cx = context.width / 2;
    const int cy = context.height / 2;
    for (int i = 2; i < context.minDimension; i += context.step) {
        const int half = i / 2;
        surface.drawRect(cx - half, cy - half, i, i, GREEN);
    }
}

void phaseFilledTriangles(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int cx = context.width / 2 - 1;
    const int cy = context.height / 2 - 1;
    const int limit = std::min(cx, cy);
    for (int i = limit - 1; i > 4; i -= context.step) {
        const uint8_t intensity = static_cast<uint8_t>(std::min(i * 2, 255));
        surface.fillTriangle(cx, cy - i, cx - i, cy + i, cx + i, cy + i, rgb565(0, intensity, intensity));
    }
}

void phaseTriangles(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int cx = context.width / 2 - 1;
    const int cy = context.height / 2 - 1;
    const int limit = std::min(cx, cy);
    for (int i = context.step; i < limit; i += context.step) {
        surface.drawTriangle(cx, cy - i, cx - i, cy + i, cx + i, cy + i, rgb565(0, 0, static_cast<uint8_t>(std::min(i * 4, 255))));
    }
}

void phaseFilledCircles(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int radius = clampInt(context.minDimension / 14, 4, 32);
    const int spacing = radius * 2 + 2;
    for (int y = radius; y < context.height; y += spacing) {
        for (int x = radius; x < context.width; x += spacing) {
            surface.fillCircle(x, y, radius, rgb565(
                static_cast<uint8_t>(x * 255 / context.width),
                static_cast<uint8_t>(y * 255 / context.height),
                128
            ));
        }
    }
}

void phaseCircles(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int radius = clampInt(context.minDimension / 14, 4, 32);
    const int spacing = radius * 2 + 2;
    for (int y = radius; y < context.height; y += spacing) {
        for (int x = radius; x < context.width; x += spacing) {
            surface.drawCircle(x, y, radius, rgb565(
                static_cast<uint8_t>(255 - x * 255 / context.width),
                static_cast<uint8_t>(255 - y * 255 / context.height),
                255
            ));
        }
    }
}

void phaseFilledArcs(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int cx = context.width / 2;
    const int cy = context.height / 2;
    const int maxRadius = std::min(cx, cy) - 1;
    if (maxRadius <= 2) {
        return;
    }

    const int arcStep = clampInt(maxRadius / 16, 4, 24);
    for (int r = arcStep; r < maxRadius; r += arcStep) {
        const float end = 360.0f * static_cast<float>(r) / static_cast<float>(maxRadius);
        const uint8_t red = static_cast<uint8_t>(std::min(r * 255 / maxRadius, 255));
        surface.fillArc(cx, cy, r, r / 2, 0.0f, end, rgb565(red, 128, static_cast<uint8_t>(255 - red)));
    }
}

void phaseArcs(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int cx = context.width / 2;
    const int cy = context.height / 2;
    const int maxRadius = std::min(cx, cy) - 1;
    if (maxRadius <= 2) {
        return;
    }

    const int arcStep = clampInt(maxRadius / 16, 4, 24);
    for (int r = arcStep; r < maxRadius; r += arcStep) {
        const float end = 360.0f * static_cast<float>(r) / static_cast<float>(maxRadius);
        const uint8_t blue = static_cast<uint8_t>(std::min(r * 255 / maxRadius, 255));
        surface.drawArc(cx, cy, r, r / 2, 0.0f, end, rgb565(255, 255 - blue / 2, blue));
    }
}

void phaseFilledRoundRects(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int cx = context.width / 2;
    const int cy = context.height / 2;
    for (int i = context.minDimension - 1; i > 20; i -= context.step) {
        const int half = i / 2;
        const int radius = std::max(2, i / 8);
        const uint8_t intensity = static_cast<uint8_t>(std::min(i * 2, 255));
        surface.fillRoundRect(cx - half, cy - half, i, i, radius, rgb565(0, intensity, 128));
    }
}

void phaseRoundRects(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const int cx = context.width / 2;
    const int cy = context.height / 2;
    for (int i = 20; i < context.minDimension; i += context.step) {
        const int half = i / 2;
        const int radius = std::max(2, i / 8);
        surface.drawRoundRect(cx - half, cy - half, i, i, radius, rgb565(255, 0, static_cast<uint8_t>(std::min(i * 2, 255))));
    }
}

void phaseSprites(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);

    if (context.sprite == nullptr || !context.sprite->valid()) {
        return;
    }

    constexpr int spriteSize = 48;
    const int spacing = spriteSize + 8;
    for (int y = 0; y < context.height; y += spacing) {
        for (int x = 0; x < context.width; x += spacing) {
            surface.blitTransparent(*context.sprite->buffer(), 0, 0, spriteSize, spriteSize, x, y, MAGENTA);
        }
    }

    static uint8_t alpha[32 * 32];
    static bool alphaReady = false;
    if (!alphaReady) {
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                const int dx = x - 16;
                const int dy = y - 16;
                const int distanceSquared = dx * dx + dy * dy;
                alpha[y * 32 + x] = distanceSquared >= 16 * 16
                    ? 0
                    : static_cast<uint8_t>((16 * 16 - distanceSquared) * 255 / (16 * 16));
            }
        }
        alphaReady = true;
    }

    surface.drawAlphaMask(alpha, 32, 32, 32, context.width / 2 - 16, context.height / 2 - 16, WHITE);
}

void phasePolygons(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);

    const int cx = context.width / 2;
    const int cy = context.height / 2;
    const int radius = std::max(8, context.minDimension / 3);
    constexpr float PI = 3.14159265f;

    gfx::Point star[10];
    for (int i = 0; i < 10; i++) {
        const float angle = -PI / 2.0f + static_cast<float>(i) * PI / 5.0f;
        const float r = (i % 2 == 0) ? static_cast<float>(radius) : static_cast<float>(radius) / 2.0f;
        star[i] = {
            cx + static_cast<int>(std::cos(angle) * r),
            cy + static_cast<int>(std::sin(angle) * r),
        };
    }
    surface.fillPolygon(star, 10, rgb565(255, 128, 0));
    surface.drawPolygon(star, 10, WHITE);

    const int inner = std::max(4, radius / 2);
    gfx::Point pentagon[5];
    for (int i = 0; i < 5; i++) {
        const float angle = -PI / 2.0f + static_cast<float>(i) * 2.0f * PI / 5.0f;
        pentagon[i] = {
            cx + static_cast<int>(std::cos(angle) * static_cast<float>(inner)),
            cy + static_cast<int>(std::sin(angle) * static_cast<float>(inner)),
        };
    }
    surface.drawPolygon(pentagon, 5, CYAN);
}

using PhaseDrawer = void (*)(gfx::Surface&, const Context&);

static const PhaseDrawer PHASE_DRAWERS[PHASE_COUNT] = {
    phaseFillScreen,
    phaseText,
    phasePixels,
    phaseLines,
    phaseFastLines,
    phaseFilledRects,
    phaseRects,
    phaseFilledTriangles,
    phaseTriangles,
    phaseFilledCircles,
    phaseCircles,
    phaseFilledArcs,
    phaseArcs,
    phaseFilledRoundRects,
    phaseRoundRects,
    phaseSprites,
    phasePolygons,
};

static_assert(sizeof(PHASE_DRAWERS) / sizeof(PHASE_DRAWERS[0]) == PHASE_COUNT, "phase table size mismatch");

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

void drawSplash(gfx::Surface& surface, const Context& context) {
    surface.gradient({ 0, 0, context.width, context.height }, rgb565(0, 0, 48), BLACK);
    const int halo = clampInt(context.minDimension / 3, 8, 160);
    surface.fillEllipse(context.width / 2, context.height / 2, halo, halo / 2, rgb565(0, 0, 24));

    const bool narrow = context.width < 120;
    const int margin = clampInt(context.minDimension / 40, 2, 8);
    const int titleScale = titleScaleFor(context);

    gfx::TextStyle title;
    title.font = &gfx::FONT_8X8;
    title.foreground = MAGENTA;
    title.align = gfx::TextAlign::Center;
    title.verticalAlign = gfx::TextVerticalAlign::Middle;
    title.scale = static_cast<uint8_t>(titleScale);
    surface.drawText(context.width / 2, context.height / 2 - gfx::lineHeight(gfx::FONT_8X8, static_cast<uint8_t>(titleScale)), narrow ? "Gfx" : "Gfx Benchmark", title);

    gfx::TextStyle subtitle;
    subtitle.font = &gfx::FONT_5X7;
    subtitle.foreground = WHITE;
    subtitle.align = gfx::TextAlign::Center;
    subtitle.verticalAlign = gfx::TextVerticalAlign::Middle;
    surface.drawText(context.width / 2, context.height / 2 + gfx::lineHeight(gfx::FONT_5X7), narrow ? "PDQ test" : "PDQ-style primitive test", subtitle);

    gfx::TextStyle hint;
    hint.font = &gfx::FONT_5X7;
    hint.foreground = GREY;
    hint.align = gfx::TextAlign::Center;
    hint.verticalAlign = gfx::TextVerticalAlign::Bottom;
    surface.drawText(context.width / 2, context.height - margin, narrow ? "Any key" : "Any input cancels", hint);
}

void drawSlowMessage(gfx::Surface& surface, const Context& context) {
    surface.clear(BLACK);
    const bool narrow = context.width < 120;
    const int margin = clampInt(context.minDimension / 40, 2, 8);

    gfx::TextStyle title;
    title.font = &gfx::FONT_8X8;
    title.foreground = ORANGE;
    title.align = gfx::TextAlign::Center;
    title.verticalAlign = gfx::TextVerticalAlign::Middle;
    title.scale = static_cast<uint8_t>(titleScaleFor(context));
    surface.drawText(context.width / 2, context.height / 2 - gfx::lineHeight(gfx::FONT_8X8, title.scale), "Skipped", title);

    gfx::TextStyle subtitle;
    subtitle.font = &gfx::FONT_5X7;
    subtitle.foreground = WHITE;
    subtitle.align = gfx::TextAlign::Center;
    subtitle.verticalAlign = gfx::TextVerticalAlign::Middle;
    surface.drawText(context.width / 2, context.height / 2 + gfx::lineHeight(gfx::FONT_5X7), narrow ? "Slow panel" : "Slow-refresh display", subtitle);

    gfx::TextStyle hint;
    hint.font = &gfx::FONT_5X7;
    hint.foreground = GREY;
    hint.align = gfx::TextAlign::Center;
    hint.verticalAlign = gfx::TextVerticalAlign::Bottom;
    surface.drawText(context.width / 2, context.height - margin, narrow ? "Any key" : "Any input to exit", hint);
}

void drawSmallResults(gfx::Surface& surface, const Context& context, const PhaseResult* results, unsigned long totalDrawUs, unsigned long totalPresentUs) {
    const int lineHeight = gfx::lineHeight(gfx::FONT_5X7);
    const int margin = 1;
    int y = margin;

    int slowest = 0;
    for (int i = 1; i < PHASE_COUNT; i++) {
        if (results[i].drawUs > results[slowest].drawUs) {
            slowest = i;
        }
    }

    surface.drawText(margin, y, "Gfx", gfx::FONT_5X7, MAGENTA);
    y += lineHeight;

    char line[32];
    std::snprintf(line, sizeof(line), "T:%lu", totalDrawUs);
    surface.drawText(margin, y, line, gfx::FONT_5X7, WHITE);
    y += lineHeight;

    std::snprintf(line, sizeof(line), "P:%lu", totalPresentUs);
    surface.drawText(margin, y, line, gfx::FONT_5X7, CYAN);
    y += lineHeight;

    surface.drawText(margin, y, "Slowest", gfx::FONT_5X7, CYAN);
    y += lineHeight;
    surface.drawText(margin, y, results[slowest].shortName, gfx::FONT_5X7, YELLOW);
    y += lineHeight;

    std::snprintf(line, sizeof(line), "%d us", results[slowest].drawUs);
    surface.drawText(margin, y, line, gfx::FONT_5X7, YELLOW);
    y += lineHeight;

    if (y + lineHeight <= context.height) {
        surface.drawText(margin, y, "Any key", gfx::FONT_5X7, GREY);
    }
}

void drawResults(gfx::Surface& surface, const Context& context, const PhaseResult* results, unsigned long totalDrawUs, unsigned long totalPresentUs) {
    surface.gradient({ 0, 0, context.width, context.height }, rgb565(0, 16, 48), BLACK);

    if (context.width < 120) {
        drawSmallResults(surface, context, results, totalDrawUs, totalPresentUs);
        return;
    }

    const int margin = clampInt(context.minDimension / 60, 2, 8);
    const int titleScale = titleScaleFor(context);
    const int tableScale = tableScaleFor(context);
    const int titleHeight = gfx::lineHeight(gfx::FONT_8X8, static_cast<uint8_t>(titleScale));
    const int lineHeight = gfx::lineHeight(gfx::FONT_5X7, static_cast<uint8_t>(tableScale));

    gfx::TextStyle title;
    title.font = &gfx::FONT_8X8;
    title.foreground = MAGENTA;
    title.align = gfx::TextAlign::Center;
    title.scale = static_cast<uint8_t>(titleScale);
    surface.drawText(context.width / 2, margin, "Gfx Benchmark", title);

    int y = margin + titleHeight + margin;

    char subtitle[48];
    std::snprintf(subtitle, sizeof(subtitle), "%dx%d %s", context.width, context.height, modeName(context.mode));
    gfx::TextStyle subtitleStyle;
    subtitleStyle.font = &gfx::FONT_5X7;
    subtitleStyle.foreground = CYAN;
    subtitleStyle.align = gfx::TextAlign::Center;
    surface.drawText(context.width / 2, y, subtitle, subtitleStyle);
    y += lineHeight;

    // Short landscape panels (Cardputer, T-Embed) don't have room for one row per phase plus the
    // full footer. Split the table into two columns there, which keeps all 15 phases and the
    // draw/present/hint footer visible.
    const bool twoColumn = context.height < 200 && context.height >= 110 && context.width >= 180;
    const int footerLines = context.height >= 100 ? 3 : 1;
    // The header line is drawn after this calculation, so reserve it as well as the footer.
    const int available = context.height - margin - footerLines * lineHeight - y - lineHeight;
    const int rowsAvailable = std::max(0, available / lineHeight);
    const int capacity = twoColumn ? rowsAvailable * 2 : rowsAvailable;
    const bool compact = capacity < PHASE_COUNT;
    const int visibleCount = std::min(capacity, PHASE_COUNT);
    const int rowsPerColumn = twoColumn ? (visibleCount + 1) / 2 : visibleCount;

    int order[PHASE_COUNT];
    for (int i = 0; i < PHASE_COUNT; i++) {
        order[i] = i;
    }
    if (compact) {
        std::sort(order, order + PHASE_COUNT, [results](int a, int b) {
            return results[a].drawUs > results[b].drawUs;
        });
    }

    const int split = context.width / 2;
    const int leftTimeRight = twoColumn ? split - margin : context.width - margin;

    gfx::TextStyle header;
    header.font = &gfx::FONT_5X7;
    header.foreground = WHITE;
    header.scale = static_cast<uint8_t>(tableScale);
    surface.drawText(margin, y, compact ? "Slowest" : "Phase", header);
    header.align = gfx::TextAlign::Right;
    surface.drawText(leftTimeRight, y, "us", header);

    if (twoColumn) {
        header.align = gfx::TextAlign::Left;
        surface.drawText(split + margin, y, compact ? "Slowest" : "Phase", header);
        header.align = gfx::TextAlign::Right;
        surface.drawText(context.width - margin, y, "us", header);
    }

    y += lineHeight;
    surface.hline(margin, y - 1, context.width - 2 * margin, GREY);

    for (int row = 0; row < rowsPerColumn; row++) {
        if (row % 2 == 0) {
            surface.fillRect(margin, y, context.width - 2 * margin, lineHeight, DARK_STRIPE);
        }

        auto drawCell = [&](int index, int nameX, int timeRight) {
            if (index >= visibleCount) {
                return;
            }
            const PhaseResult& result = results[order[index]];

            gfx::TextStyle cell;
            cell.font = &gfx::FONT_5X7;
            cell.foreground = WHITE;
            cell.scale = static_cast<uint8_t>(tableScale);
            surface.drawText(nameX, y, result.shortName, cell);

            char time[16];
            std::snprintf(time, sizeof(time), "%d", result.drawUs);
            cell.foreground = YELLOW;
            cell.align = gfx::TextAlign::Right;
            surface.drawText(timeRight, y, time, cell);
        };

        drawCell(row, margin, leftTimeRight);
        if (twoColumn) {
            drawCell(row + rowsPerColumn, split + margin, context.width - margin);
        }

        y += lineHeight;
    }

    const int footerTop = context.height - margin - footerLines * lineHeight;
    char drawLine[48];
    char presentLine[48];
    std::snprintf(drawLine, sizeof(drawLine), "Draw %lu us", totalDrawUs);
    std::snprintf(presentLine, sizeof(presentLine), "Present %lu us", totalPresentUs);

    gfx::TextStyle footer;
    footer.font = &gfx::FONT_5X7;
    footer.foreground = GREEN;
    footer.scale = static_cast<uint8_t>(tableScale);
    footer.verticalAlign = gfx::TextVerticalAlign::Bottom;
    surface.drawText(margin, footerTop + lineHeight, drawLine, footer);

    if (footerLines > 1) {
        footer.foreground = CYAN;
        surface.drawText(margin, footerTop + 2 * lineHeight, presentLine, footer);

        gfx::TextStyle hint;
        hint.font = &gfx::FONT_5X7;
        hint.foreground = GREY;
        hint.scale = static_cast<uint8_t>(tableScale);
        hint.align = gfx::TextAlign::Right;
        hint.verticalAlign = gfx::TextVerticalAlign::Bottom;
        surface.drawText(context.width - margin, context.height - margin, "Any input to exit", hint);
    }
}

} // namespace

void runApplication(gfx::Target* target, AnyInput* input) {
    const gfx::Info& info = target->info();

    Context context {
        info.frameWidth > 0 ? info.frameWidth : info.width,
        info.frameHeight > 0 ? info.frameHeight : info.height,
        0,
        0,
        info.mode,
    };
    context.minDimension = std::min(context.width, context.height);
    context.step = clampInt(context.minDimension / 24, 6, 32);

    struct TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    struct AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);

    if (info.slowRefresh) {
        ESP_LOGW(TAG, "Slow-refresh display: skipping the benchmark");
        target->render([&](gfx::Surface& surface) {
            drawSlowMessage(surface, context);
        }, gfx::Prepare::Overwrite);
        target->refresh();
        waitForExit(subscription, *input);
    } else {
        ESP_LOGI(TAG, "Benchmarking %dx%d, mode %s, step %d", context.width, context.height, modeName(context.mode), context.step);

        gfx::Canvas sprite(48, 48, DISPLAY_COLOR_FORMAT_RGB565);
        if (sprite.valid()) {
            gfx::Surface& canvas = sprite.surface();
            canvas.clear(MAGENTA);
            canvas.fillRoundRect(4, 4, 40, 40, 8, BLUE);
            canvas.fillCircle(24, 24, 14, CYAN);
            canvas.drawText(10, 20, "GX", gfx::FONT_8X8, WHITE);
            context.sprite = &sprite;
        } else {
            ESP_LOGW(TAG, "Could not allocate the sprite canvas");
        }

        target->render([&](gfx::Surface& surface) {
            drawSplash(surface, context);
        }, gfx::Prepare::Overwrite);
        tt::kernel::delayTicks(tt::kernel::millisToTicks(300));

        PhaseResult results[PHASE_COUNT] {};
        bool aborted = false;
        unsigned long totalDrawUs = 0;
        unsigned long totalPresentUs = 0;

        for (int i = 0; i < PHASE_COUNT; i++) {
            if (takeExitRequest(subscription, *input)) {
                aborted = true;
                break;
            }

            target->render([&](gfx::Surface& surface) {
                PHASE_DRAWERS[i](surface, context);
            }, gfx::Prepare::Overwrite);

            const gfx::Stats& stats = target->stats();
            results[i] = PhaseResult {
                PHASE_NAMES[i],
                PHASE_SHORT_NAMES[i],
                stats.drawUs,
                stats.presentUs,
                stats.totalUs,
                stats.dirtyPixels,
                stats.dirtyRects,
            };
            totalDrawUs += static_cast<unsigned long>(stats.drawUs);
            totalPresentUs += static_cast<unsigned long>(stats.presentUs);

            ESP_LOGI(
                TAG,
                "%2d/%d %-20s draw %7d us  present %7d us  total %7d us  dirty %7d px",
                i + 1,
                PHASE_COUNT,
                PHASE_NAMES[i],
                stats.drawUs,
                stats.presentUs,
                stats.totalUs,
                stats.dirtyPixels
            );

            tt::kernel::delayTicks(tt::kernel::millisToTicks(PHASE_PREVIEW_MS));
        }

        if (!aborted) {
            ESP_LOGI(TAG, "Total draw %lu us, present %lu us", totalDrawUs, totalPresentUs);
            target->render([&](gfx::Surface& surface) {
                drawResults(surface, context, results, totalDrawUs, totalPresentUs);
            }, gfx::Prepare::Overwrite);
            waitForExit(subscription, *input);
        }
    }

    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
}


