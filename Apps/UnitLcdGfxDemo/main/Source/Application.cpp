#include "Application.h"
#include "GroveLookup.h"

#include <UnitLcd.h>
#include <UnitLcdSink.h>
#include <UnitPaHub.h>

#include <esp_log.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
#include <app/event.h>
#include <tactility/check.h>

#include <Tactility/kernel/Kernel.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <optional>

constexpr auto TAG = "UnitLcdGfxDemo";

// Tab5's panel is native portrait; direct rendering does not rotate it for us. Turn it into a
// landscape logical frame with the PPA. If the status appears upside down, use 270 instead.
constexpr int PRIMARY_LANDSCAPE_ROTATION = 90;

namespace {

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// The M5 LCD Unit is 240x135 in landscape. Its frame buffer lives in internal RAM, and on
// no-PSRAM boards (e.g. Cardputer) a full 240x135 frame (64.8 KB) may not be available as a single
// contiguous block. The demo therefore only drives the animated play area through the sink and
// draws the surrounding chrome directly on the unit with its own hardware primitives. The
// preferred play area is 228x72 (about 32 KB); smaller heights are tried when memory is tighter.
constexpr int LCD_PLAY_LEFT = 6;
constexpr int LCD_PLAY_TOP = 40;
constexpr int LCD_PLAY_WIDTH = 228;
constexpr int LCD_PLAY_HEIGHT = 72;
constexpr int LCD_PLAY_HEIGHT_FALLBACKS[] = { 56, 40, 24 };
constexpr int LCD_BALL_RADIUS = 7;

constexpr uint16_t LCD_BACKGROUND = rgb565(6, 10, 20);
constexpr uint16_t LCD_CHROME = rgb565(40, 60, 90);
constexpr uint16_t LCD_TITLE_COLOR = 0xF81F;
constexpr uint16_t LCD_SUBTITLE_COLOR = 0x07FF;
constexpr uint16_t LCD_BALL_COLOR = 0xF800;

int clampInt(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

int titleScaleFor(const gfx::Surface& surface) {
    return clampInt(std::min(surface.width(), surface.height()) / 140, 1, 5);
}

int bodyScaleFor(const gfx::Surface& surface) {
    return clampInt(std::min(surface.width(), surface.height()) / 200, 1, 3);
}

void preparePrimary(gfx::Target& primary) {
    const gfx::Info& info = primary.info();
    if (info.width < info.height) {
        const gfx::Transform transform {
            info.height,
            info.width,
            PRIMARY_LANDSCAPE_ROTATION,
        };
        if (!primary.set_transform(transform)) {
            ESP_LOGW(TAG, "Primary rotation unavailable; keeping native orientation");
        }
    }
}

bool takeExit(struct AppEventSubscription& subscription, AnyInput& input) {
    bool shouldExit = false;
    struct AppEvent event {};
    while (app_event_poll(&subscription, &event) == ERROR_NONE) {
        if (event.type == APP_EVENT_CLOSE) {
            shouldExit = true;
        }
    }
    return shouldExit || input.activity();
}

void waitForExit(struct AppEventSubscription& subscription, AnyInput& input) {
    while (!takeExit(subscription, input)) {
        tt::kernel::delayTicks(tt::kernel::millisToTicks(50));
    }
}

void showPrimaryMessage(gfx::Target& primary, const char* title, const char* line1, const char* line2) {
    primary.render([&](gfx::Surface& surface) {
        surface.clear(0x0000);

        const int titleScale = titleScaleFor(surface);
        const int bodyScale = bodyScaleFor(surface);
        const int titleHeight = gfx::lineHeight(gfx::FONT_8X16, static_cast<uint8_t>(titleScale));
        const int bodyHeight = gfx::lineHeight(gfx::FONT_5X7, static_cast<uint8_t>(bodyScale));
        const int gap = bodyHeight;
        const int blockHeight = titleHeight + gap + bodyHeight * 2;
        const int startY = std::max(4, (surface.height() - blockHeight) / 2);

        gfx::TextStyle titleStyle;
        titleStyle.font = &gfx::FONT_8X16;
        titleStyle.foreground = 0xFFFF;
        titleStyle.align = gfx::TextAlign::Center;
        titleStyle.scale = static_cast<uint8_t>(titleScale);
        surface.drawText(surface.width() / 2, startY, title, titleStyle);

        gfx::TextStyle body;
        body.font = &gfx::FONT_5X7;
        body.foreground = 0x07FF;
        body.align = gfx::TextAlign::Center;
        body.scale = static_cast<uint8_t>(bodyScale);
        surface.drawText(surface.width() / 2, startY + titleHeight + gap, line1, body);
        surface.drawText(surface.width() / 2, startY + titleHeight + gap + bodyHeight, line2, body);

        gfx::TextStyle hint;
        hint.font = &gfx::FONT_5X7;
        hint.foreground = 0x8410;
        hint.align = gfx::TextAlign::Center;
        hint.verticalAlign = gfx::TextVerticalAlign::Bottom;
        hint.scale = static_cast<uint8_t>(bodyScale);
        surface.drawText(surface.width() / 2, surface.height() - 6, "Any input to exit", hint);
    }, gfx::Prepare::Overwrite);

    if (primary.info().slowRefresh) {
        primary.refresh();
    }
}

void drawPrimaryStatus(gfx::Target& primary, int lcdWidth, int lcdHeight, bool viaHub, int channel) {
    primary.render([&](gfx::Surface& surface) {
        surface.clear(0x0000);

        const int titleScale = titleScaleFor(surface);
        const int bodyScale = bodyScaleFor(surface);
        const int titleHeight = gfx::lineHeight(gfx::FONT_8X16, static_cast<uint8_t>(titleScale));
        const int bodyHeight = gfx::lineHeight(gfx::FONT_5X7, static_cast<uint8_t>(bodyScale));
        const int gap = bodyHeight;
        const int blockHeight = titleHeight + gap + bodyHeight * 3;
        const int startY = std::max(4, (surface.height() - blockHeight) / 2);

        gfx::TextStyle title;
        title.font = &gfx::FONT_8X16;
        title.foreground = 0xF81F;
        title.align = gfx::TextAlign::Center;
        title.scale = static_cast<uint8_t>(titleScale);
        surface.drawText(surface.width() / 2, startY, "UnitLCD Gfx Sink", title);

        char line[48];
        gfx::TextStyle body;
        body.font = &gfx::FONT_5X7;
        body.foreground = 0xFFFF;
        body.align = gfx::TextAlign::Center;
        body.scale = static_cast<uint8_t>(bodyScale);

        int y = startY + titleHeight + gap;
        std::snprintf(line, sizeof(line), "LCD %dx%d", lcdWidth, lcdHeight);
        surface.drawText(surface.width() / 2, y, line, body);

        y += bodyHeight;
        if (viaHub) {
            std::snprintf(line, sizeof(line), "via PaHub channel %d", channel);
        } else {
            std::snprintf(line, sizeof(line), "direct I2C");
        }
        body.foreground = 0x07FF;
        surface.drawText(surface.width() / 2, y, line, body);

        y += bodyHeight;
        body.foreground = 0x07E0;
        surface.drawText(surface.width() / 2, y, "Watch the LCD unit", body);

        gfx::TextStyle hint;
        hint.font = &gfx::FONT_5X7;
        hint.foreground = 0x8410;
        hint.align = gfx::TextAlign::Center;
        hint.verticalAlign = gfx::TextVerticalAlign::Bottom;
        hint.scale = static_cast<uint8_t>(bodyScale);
        surface.drawText(surface.width() / 2, surface.height() - 6, "Any input to exit", hint);
    }, gfx::Prepare::Overwrite);

    if (primary.info().slowRefresh) {
        primary.refresh();
    }
}

// Everything outside the play area is static, so it is drawn once with the unit's own primitives
// instead of going through the sink's frame buffer.
void drawLcdChrome(UnitLcd& lcd, int playHeight) {
    const int footerY = LCD_PLAY_TOP + playHeight + 8;

    lcd.fillScreen(LCD_BACKGROUND);
    lcd.drawText(6, 4, "UnitLCD Gfx Sink", LCD_TITLE_COLOR, LCD_BACKGROUND, 2);
    lcd.drawText(6, 23, "SinkTarget + UnitLcdSink", LCD_SUBTITLE_COLOR, LCD_BACKGROUND);
    lcd.fillRect(6, 34, static_cast<uint8_t>(lcd.width() - 7), 34, LCD_CHROME);
    lcd.drawRect(
        static_cast<uint8_t>(LCD_PLAY_LEFT - 1),
        static_cast<uint8_t>(LCD_PLAY_TOP - 1),
        static_cast<uint8_t>(LCD_PLAY_WIDTH + 2),
        static_cast<uint8_t>(playHeight + 2),
        LCD_CHROME
    );
    lcd.drawText(6, static_cast<uint8_t>(footerY), "Any input to exit", 0x8410, LCD_BACKGROUND);
}

void drawLcdFailure(UnitLcd& lcd) {
    lcd.fillScreen(LCD_BACKGROUND);
    lcd.drawText(6, 6, "Sink init failed", 0xFFFF, LCD_BACKGROUND, 2);
    lcd.drawText(6, 28, "Not enough contiguous RAM", LCD_SUBTITLE_COLOR, LCD_BACKGROUND);
    lcd.drawText(6, 38, "for the play area frame", LCD_SUBTITLE_COLOR, LCD_BACKGROUND);
    lcd.drawText(6, 120, "Any input to exit", 0x8410, LCD_BACKGROUND);
}

} // namespace

void runApplication(Device* display, AnyInput* input) {
    struct TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    struct AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);

    Device* i2c = findGroveI2cDevice();
    if (i2c == nullptr) {
        gfx::Target primary(display);
        if (primary.valid()) {
            preparePrimary(primary);
            showPrimaryMessage(primary, "UnitLCD Gfx Sink", "No Grove I2C port found", "Set a Grove port to I2C");
        } else {
            ESP_LOGE(TAG, "Primary display can't be used for the status screen");
        }
        waitForExit(subscription, *input);
        check(app_event_unsubscribe(&subscription) == ERROR_NONE);
        task_event_group_destruct(&eventGroup);
        return;
    }

    UnitLcd lcd;
    UnitPaHub hub;
    uint8_t channel = UnitPaHub::NO_CHANNEL;
    bool usingHub = false;
    bool found = false;

    if (hub.begin(i2c)) {
        for (uint8_t candidate = 0; candidate < UnitPaHub::NUM_CHANNELS && !found; candidate++) {
            if (!hub.select(candidate)) {
                continue;
            }
            if (lcd.begin(i2c)) {
                channel = candidate;
                usingHub = true;
                found = true;
            }
        }
        if (!found) {
            hub.deselect();
        }
    }

    if (!found && lcd.begin(i2c)) {
        usingHub = false;
        found = true;
    }

    if (!found) {
        gfx::Target primary(display);
        if (primary.valid()) {
            preparePrimary(primary);
            showPrimaryMessage(primary, "UnitLCD Gfx Sink", "LCD unit not found", "Check the Grove cable and address");
        } else {
            ESP_LOGE(TAG, "Primary display can't be used for the status screen");
        }
        waitForExit(subscription, *input);
        check(app_event_unsubscribe(&subscription) == ERROR_NONE);
        task_event_group_destruct(&eventGroup);
        return;
    }

    lcd.setBrightness(180);
    lcd.setRotation(1); // landscape, 240x135

    // Allocate the play-area frame first: on no-PSRAM boards the primary Target may not be able to
    // coexist with it, and a partial frame fits where a full one does not. If even the preferred
    // height doesn't fit in one contiguous block, try smaller ones.
#ifdef ESP_PLATFORM
    ESP_LOGI(
        TAG,
        "Largest free blocks: internal %u bytes, any %u bytes",
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))
    );
#endif

    UnitLcdSink sink(lcd, LCD_PLAY_LEFT, LCD_PLAY_TOP);
    std::optional<gfx::SinkTarget> lcdTarget;
    lcdTarget.emplace(sink, LCD_PLAY_WIDTH, LCD_PLAY_HEIGHT, DISPLAY_COLOR_FORMAT_RGB565);
    for (std::size_t i = 0; !lcdTarget->valid() && i < std::size(LCD_PLAY_HEIGHT_FALLBACKS); i++) {
        lcdTarget.reset();
        lcdTarget.emplace(sink, LCD_PLAY_WIDTH, LCD_PLAY_HEIGHT_FALLBACKS[i], DISPLAY_COLOR_FORMAT_RGB565);
    }

    if (!lcdTarget->valid()) {
        drawLcdFailure(lcd);
        tt::kernel::delayTicks(tt::kernel::millisToTicks(80));
        lcd.setBrightness(0);
        if (usingHub) {
            hub.deselect();
        }
        ESP_LOGE(TAG, "LCD sink unavailable; see the gfx/UnitLcdSink log for the reason");
        waitForExit(subscription, *input);
        check(app_event_unsubscribe(&subscription) == ERROR_NONE);
        task_event_group_destruct(&eventGroup);
        return;
    }

    const int playHeight = lcdTarget->info().height;
    const int footerY = LCD_PLAY_TOP + playHeight + 8;

    // The unit's framebuffer survives app restarts, so erase the previous run before drawing.
    drawLcdChrome(lcd, playHeight);

    ESP_LOGI(
        TAG,
        "UnitLCD ready: %dx%d (%s), play area %dx%d at (%d, %d)",
        lcd.width(),
        lcd.height(),
        usingHub ? "via PaHub" : "direct",
        LCD_PLAY_WIDTH,
        playHeight,
        LCD_PLAY_LEFT,
        LCD_PLAY_TOP
    );
    if (playHeight != LCD_PLAY_HEIGHT) {
        ESP_LOGW(
            TAG,
            "Reduced the play area to %dx%d to fit the free internal RAM",
            LCD_PLAY_WIDTH,
            playHeight
        );
    }

    gfx::Target primary(display);
    if (primary.valid()) {
        preparePrimary(primary);
        drawPrimaryStatus(primary, lcd.width(), lcd.height(), usingHub, channel);
    } else {
        ESP_LOGW(TAG, "Not enough memory for the primary status target; only the LCD is active");
    }

    // All coordinates below are local to the sink's play area.
    const int radius = LCD_BALL_RADIUS;
    const int ballMinX = radius + 2;
    const int ballMaxX = LCD_PLAY_WIDTH - radius - 2;
    const int ballMinY = radius + 2;
    const int ballMaxY = playHeight - radius - 2;

    int ballX = LCD_PLAY_WIDTH / 2;
    int ballY = playHeight / 2;
    int velocityX = 3;
    int velocityY = 2;
    int previousX = ballX;
    int previousY = ballY;
    unsigned long frames = 0;
    bool clearPlayArea = true;

    auto drawLcd = [&](gfx::Surface& surface) {
        if (clearPlayArea) {
            surface.clear(0x0000);
            clearPlayArea = false;
        } else {
            surface.fillRect(
                previousX - radius,
                previousY - radius,
                radius * 2 + 1,
                radius * 2 + 1,
                0x0000
            );
        }
        surface.fillCircle(ballX, ballY, radius, LCD_BALL_COLOR);
    };

    while (true) {
        if (usingHub && !hub.select(channel)) {
            ESP_LOGW(TAG, "PaHub channel %d became unavailable", channel);
            break;
        }

        lcdTarget->render(drawLcd);

        // The frame counter is outside the play area, so draw it straight onto the unit.
        char status[16];
        const int length = std::min(
            std::snprintf(status, sizeof(status), "f:%lu", frames),
            static_cast<int>(sizeof(status)) - 1
        );
        lcd.drawText(
            static_cast<uint8_t>(lcd.width() - 6 - length * 6),
            static_cast<uint8_t>(footerY),
            status,
            0x07E0,
            LCD_BACKGROUND
        );

        frames++;
        previousX = ballX;
        previousY = ballY;
        ballX += velocityX;
        ballY += velocityY;
        if (ballX <= ballMinX) {
            ballX = ballMinX;
            velocityX = std::abs(velocityX);
        } else if (ballX >= ballMaxX) {
            ballX = ballMaxX;
            velocityX = -std::abs(velocityX);
        }
        if (ballY <= ballMinY) {
            ballY = ballMinY;
            velocityY = std::abs(velocityY);
        } else if (ballY >= ballMaxY) {
            ballY = ballMaxY;
            velocityY = -std::abs(velocityY);
        }

        if (takeExit(subscription, *input)) {
            break;
        }
        tt::kernel::delayTicks(tt::kernel::millisToTicks(40));
    }

    ESP_LOGI(
        TAG,
        "Stopped after %lu frames; last frame had %d dirty rect(s), %d pixel(s)",
        frames,
        lcdTarget->stats().dirtyRects,
        lcdTarget->stats().dirtyPixels
    );

    lcd.fillScreen(0x0000);
    tt::kernel::delayTicks(tt::kernel::millisToTicks(80));
    lcd.setBrightness(0);
    if (usingHub) {
        hub.deselect();
    }

    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
}
