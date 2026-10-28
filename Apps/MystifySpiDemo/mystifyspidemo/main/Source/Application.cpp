// SPDX-License-Identifier: Apache-2.0
#include "Application.h"

#include <ili9341panel.h>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <app/event.h>
#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/module.h>

#include <Tactility/kernel/Kernel.h>

#include <gfx/Gfx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

constexpr auto* TAG = "MystifyDemo";

namespace {

// Tab5's panel is native portrait; direct rendering does not rotate it for us. A landscape
// logical frame keeps the mirrored scene upright. Use 270 if it appears upside down.
constexpr int PRIMARY_LANDSCAPE_ROTATION = 90;
// The mirrored scene is rendered at this logical size and scaled to the panel by the PPA. A
// smaller source is much cheaper to draw; 16:9 matches the Tab5 panel.
constexpr int PRIMARY_MIRROR_WIDTH = 640;
constexpr int PRIMARY_MIRROR_HEIGHT = 360;

constexpr int TARGET_FPS = 30;
constexpr int64_t FRAME_BUDGET_US = 1'000'000 / TARGET_FPS;

constexpr int WALKER_COUNT = 2;
constexpr int VERTEX_COUNT = 4;
constexpr int TRAIL_LENGTH = 6;
// Normalised: both the 320x240 panel and the primary frame use the same simulation.
constexpr float PLAY_MARGIN = 0.02f;

uint16_t fade565(uint16_t rgb565, int brightness /* 0..255 */) {
    const int r = ((rgb565 >> 11) & 0x1F) * brightness / 255;
    const int g = ((rgb565 >> 5) & 0x3F) * brightness / 255;
    const int b = (rgb565 & 0x1F) * brightness / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

float randomFloat(float minimum, float maximum) {
    const uint32_t step = esp_random() % 1000;
    return minimum + (maximum - minimum) * (static_cast<float>(step) / 999.0f);
}

struct Walker {
    // Normalised positions in [0, 1]; velocities in screen-heights per second.
    float x[VERTEX_COUNT];
    float y[VERTEX_COUNT];
    float velocityX[VERTEX_COUNT];
    float velocityY[VERTEX_COUNT];
    uint16_t color = 0xFFFF;
    // history[0] is the current frame, history[TRAIL_LENGTH - 1] the oldest echo.
    float historyX[TRAIL_LENGTH][VERTEX_COUNT];
    float historyY[TRAIL_LENGTH][VERTEX_COUNT];
};

void resetWalker(Walker& walker, uint16_t color) {
    walker.color = color;
    for (int vertex = 0; vertex < VERTEX_COUNT; vertex++) {
        walker.x[vertex] = randomFloat(PLAY_MARGIN, 1.0f - PLAY_MARGIN);
        walker.y[vertex] = randomFloat(PLAY_MARGIN, 1.0f - PLAY_MARGIN);
        walker.velocityX[vertex] = randomFloat(0.08f, 0.22f) * ((esp_random() & 1) != 0 ? 1.0f : -1.0f);
        walker.velocityY[vertex] = randomFloat(0.08f, 0.22f) * ((esp_random() & 1) != 0 ? 1.0f : -1.0f);
    }
    for (int age = 0; age < TRAIL_LENGTH; age++) {
        std::memcpy(walker.historyX[age], walker.x, sizeof(walker.x));
        std::memcpy(walker.historyY[age], walker.y, sizeof(walker.y));
    }
}

void moveWalker(Walker& walker, float deltaSeconds) {
    for (int vertex = 0; vertex < VERTEX_COUNT; vertex++) {
        walker.x[vertex] += walker.velocityX[vertex] * deltaSeconds;
        walker.y[vertex] += walker.velocityY[vertex] * deltaSeconds;

        if (walker.x[vertex] < PLAY_MARGIN) {
            walker.x[vertex] = PLAY_MARGIN;
            walker.velocityX[vertex] = std::abs(walker.velocityX[vertex]);
        } else if (walker.x[vertex] > 1.0f - PLAY_MARGIN) {
            walker.x[vertex] = 1.0f - PLAY_MARGIN;
            walker.velocityX[vertex] = -std::abs(walker.velocityX[vertex]);
        }

        if (walker.y[vertex] < PLAY_MARGIN) {
            walker.y[vertex] = PLAY_MARGIN;
            walker.velocityY[vertex] = std::abs(walker.velocityY[vertex]);
        } else if (walker.y[vertex] > 1.0f - PLAY_MARGIN) {
            walker.y[vertex] = 1.0f - PLAY_MARGIN;
            walker.velocityY[vertex] = -std::abs(walker.velocityY[vertex]);
        }
    }

    for (int age = TRAIL_LENGTH - 1; age > 0; age--) {
        std::memcpy(walker.historyX[age], walker.historyX[age - 1], sizeof(walker.x));
        std::memcpy(walker.historyY[age], walker.historyY[age - 1], sizeof(walker.y));
    }
    std::memcpy(walker.historyX[0], walker.x, sizeof(walker.x));
    std::memcpy(walker.historyY[0], walker.y, sizeof(walker.y));
}

/** The whole surface is the play area: each display fills its own aspect ratio. */
void drawScene(gfx::Surface& surface, const Walker* walkers, int fps) {
    surface.clear(0x0000);

    const float width = static_cast<float>(surface.width());
    const float height = static_cast<float>(surface.height());

    for (int index = 0; index < WALKER_COUNT; index++) {
        const Walker& walker = walkers[index];
        // Draw the oldest echo first so the newest polygon lands on top.
        for (int age = TRAIL_LENGTH - 1; age >= 0; age--) {
            const int brightness = 40 + (TRAIL_LENGTH - 1 - age) * 215 / (TRAIL_LENGTH - 1);
            const uint16_t color = fade565(walker.color, brightness);

            gfx::Point points[VERTEX_COUNT];
            for (int vertex = 0; vertex < VERTEX_COUNT; vertex++) {
                points[vertex] = gfx::Point {
                    static_cast<int>(walker.historyX[age][vertex] * width + 0.5f),
                    static_cast<int>(walker.historyY[age][vertex] * height + 0.5f),
                };
            }
            surface.drawPolygon(points, VERTEX_COUNT, color);
        }
    }

    const uint8_t textScale = static_cast<uint8_t>(
        std::min(4, std::max(1, std::min(surface.width(), surface.height()) / 150))
    );
    const int margin = 4 * textScale;

    char info[48];
    std::snprintf(info, sizeof(info), "%dx%d  %d fps", surface.width(), surface.height(), fps);
    surface.drawText(margin, margin, info, gfx::FONT_5X7, 0x9CF3, 0x0000, textScale);
    surface.drawText(
        margin,
        surface.height() - 11 * textScale,
        "any input exits",
        gfx::FONT_5X7,
        0x632C,
        0x0000,
        textScale
    );
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

/**
 * A DisplaySink variant that prefers PSRAM for the frame buffer.
 *
 * `gfx::DisplaySink` requires DMA-capable memory, and ESP-IDF doesn't tag PSRAM as
 * `MALLOC_CAP_DMA` on ESP32-P4 even though the SPI DMA engine can read from it (with
 * `SPI_TRANS_DMA_USE_PSRAM`, which the ILI9341 driver sets). Requiring internal RAM for a
 * 320x240 frame (153.6 KB) would waste most of it, so this sink asks for PSRAM first and lets
 * Tactility fall back to any suitable memory when there is none.
 */
class ExternalFirstDisplaySink : public gfx::DisplaySink {
public:
    explicit ExternalFirstDisplaySink(struct Device* display) : gfx::DisplaySink(display) {}

    bool frame_buffer_policy(MemoryPolicy& out) const override {
        out = MemoryPolicy { 0, MEMORY_CAPABILITY_EXTERNAL, 64 };
        return true;
    }
};

/**
 * The module's API. It is looked up while the app runs instead of linked, and holding the module
 * reference keeps the panel module from being stopped (e.g. from Kernel settings) mid-demo.
 */
struct PanelApi {
    decltype(&ili9341panel_get_device) getDevice = nullptr;
    Module* module = nullptr;
};

bool acquirePanelApi(PanelApi& api) {
    Module* module = nullptr;

    uintptr_t address = 0;
    Module* owner = nullptr;
    if (!module_resolve_symbol_global_get("ili9341panel_get_device", &address, &owner)) {
        return false;
    }
    api.getDevice = reinterpret_cast<decltype(api.getDevice)>(address);
    module = owner;

    api.module = module;
    return true;
}

void releasePanelApi(PanelApi& api) {
    if (api.module != nullptr) {
        module_put(api.module);
        api.module = nullptr;
    }
}

} // namespace

const char* runApplication(struct Device* display, AnyInput& input) {
    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);
    struct AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &event_group) == ERROR_NONE);

    PanelApi api;
    if (!acquirePanelApi(api)) {
        check(app_event_unsubscribe(&subscription) == ERROR_NONE);
        task_event_group_destruct(&event_group);
        return "The ILI9341 panel module is not available. Install or enable the package.";
    }

    struct Device* lcd = nullptr;
    if (api.getDevice(&lcd) != ERROR_NONE || lcd == nullptr) {
        releasePanelApi(api);
        check(app_event_unsubscribe(&subscription) == ERROR_NONE);
        task_event_group_destruct(&event_group);
        return "The ILI9341 panel is not active. Check the wiring and the panel settings.";
    }

    const int lcd_width = display_get_resolution_x(lcd);
    const int lcd_height = display_get_resolution_y(lcd);

    ExternalFirstDisplaySink sink(lcd);
    gfx::SinkTarget lcd_target(sink, lcd_width, lcd_height, display_get_color_format(lcd));
    if (!lcd_target.valid()) {
        device_put(lcd);
        releasePanelApi(api);
        check(app_event_unsubscribe(&subscription) == ERROR_NONE);
        task_event_group_destruct(&event_group);
        return "The Gfx sink could not allocate a frame for the panel.";
    }

    // The primary display shows the same scene. LVGL is stopped by the app entry point, so this
    // owns the panel until the app exits.
    gfx::Target primary(display);
    if (primary.valid()) {
        const gfx::Info& info = primary.info();
        if (info.width < info.height) {
            const gfx::Transform transform {
                PRIMARY_MIRROR_WIDTH,
                PRIMARY_MIRROR_HEIGHT,
                PRIMARY_LANDSCAPE_ROTATION,
            };
            if (!primary.set_transform(transform)) {
                ESP_LOGW(TAG, "Primary rotation unavailable; keeping native orientation");
            }
        }
    } else {
        ESP_LOGW(TAG, "Primary display can't be used for direct rendering; showing the SPI panel only");
    }

    Walker walkers[WALKER_COUNT];
    resetWalker(walkers[0], 0x07FF); // cyan
    resetWalker(walkers[1], 0xF81F); // magenta

    int frames_this_second = 0;
    int fps = 0;
    int64_t measurement_start_us = esp_timer_get_time();

    auto render_scene = [&](gfx::Surface& surface) {
        drawScene(surface, walkers, fps);
    };

    // Unmistakable write-path check before the animation: red, green, blue, white, black. Both
    // screens show the same sequence.
    ESP_LOGI(TAG, "Self-test: red, green, blue, white, black");
    constexpr uint16_t SELF_TEST_COLORS[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000 };
    for (const uint16_t color : SELF_TEST_COLORS) {
        lcd_target.render([color](gfx::Surface& surface) {
            surface.clear(color);
        });
        if (primary.valid()) {
            primary.render([color](gfx::Surface& surface) {
                surface.clear(color);
            });
        }
        tt::kernel::delayTicks(tt::kernel::millisToTicks(150));
    }

    ESP_LOGI(
        TAG,
        "Running Mystify at up to %d fps on the panel%s",
        TARGET_FPS,
        primary.valid() ? " and the primary display" : ""
    );

    int64_t last_frame_us = esp_timer_get_time();

    while (!takeExit(subscription, input)) {
        const int64_t frame_start_us = esp_timer_get_time();
        float deltaSeconds = static_cast<float>(frame_start_us - last_frame_us) / 1'000'000.0f;
        last_frame_us = frame_start_us;
        deltaSeconds = std::min(deltaSeconds, 0.15f);

        for (auto& walker : walkers) {
            moveWalker(walker, deltaSeconds);
        }

        lcd_target.render(render_scene);
        if (primary.valid()) {
            primary.render(render_scene);
        }

        frames_this_second++;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - measurement_start_us >= 1'000'000) {
            fps = frames_this_second;
            const gfx::Stats& stats = lcd_target.stats();
            ESP_LOGI(
                TAG,
                "%d fps, %d dirty px, present %d us, total %d us",
                fps,
                stats.dirtyPixels,
                stats.presentUs,
                stats.totalUs
            );
            frames_this_second = 0;
            measurement_start_us = now_us;
        }

        const int64_t remaining_us = FRAME_BUDGET_US - (now_us - frame_start_us);
        if (remaining_us > 0) {
            tt::kernel::delayTicks(tt::kernel::millisToTicks(static_cast<uint32_t>(remaining_us / 1000)));
        }
    }

    // Leave both panels black: their images otherwise stay on screen after the app exits.
    lcd_target.render([](gfx::Surface& surface) { surface.clear(0x0000); });
    if (primary.valid()) {
        primary.render([](gfx::Surface& surface) { surface.clear(0x0000); });
    }

    device_put(lcd);
    releasePanelApi(api);

    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&event_group);
    return nullptr;
}
