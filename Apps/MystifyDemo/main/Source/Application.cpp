#include "Application.h"
#include "MystifyDemo.h"
#include "PixelBuffer.h"
#include "esp_log.h"

#include <Tactility/kernel/Kernel.h>

constexpr auto TAG = "Application";
constexpr int MYSTIFY_FRAME_DELAY_MS = 50;  // ~20 FPS for smooth animation

static bool isTouched(TouchDriver* touch) {
    uint16_t x, y, strength;
    uint8_t pointCount = 0;
    return touch->getTouchedPoints(&x, &y, &strength, &pointCount, 1);
}

void runApplication(DisplayDriver* display, TouchDriver* touch) {
    // Run the Mystify screensaver demo
    MystifyDemo mystify;
    if (!mystify.init(display)) {
        ESP_LOGE(TAG, "Failed to initialize MystifyDemo");
        return;
    }

    ESP_LOGI(TAG, "Starting Mystify demo - touch to exit");

    do {
        mystify.update();

        // Frame rate limiter - ~20 FPS for smooth animation
        tt::kernel::delayTicks(tt::kernel::millisToTicks(MYSTIFY_FRAME_DELAY_MS));
    } while (!isTouched(touch));

    ESP_LOGI(TAG, "Mystify demo ended");
}

