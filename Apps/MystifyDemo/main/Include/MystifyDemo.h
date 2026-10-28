#pragma once

#include <gfx/Gfx.h>

#include <cmath>
#include <cstdlib>

#include <esp_random.h>

/**
 * Mystify screensaver demo.
 *
 * Draws two bouncing polygons with trailing edges. Rendering goes through gfx::Target, which picks
 * the cheapest way onto the panel (frame buffer flip, dirty regions, or bands), so the same drawing
 * code runs on MIPI, RGB, SPI and e-paper panels regardless of how much RAM they have.
 *
 * The trails are erased by redrawing the snapshots that dropped out of them in black, instead of
 * clearing each polygon's bounding box. The box is large (four independently bouncing vertices span
 * much of the screen) while the dropped snapshots are few, and on a hardware frame buffer every
 * touched cache line costs a fill and a writeback. Erasing the ones from the previous two frames
 * also repairs the back buffer, which still holds the frame before last, so the target doesn't have
 * to copy anything forward - hence Prepare::Overwrite below.
 */
class MystifyDemo {
public:
    static constexpr int NUM_POLYGONS = 2;
    static constexpr int NUM_VERTICES = 4;
    static constexpr int TRAIL_LENGTH = 8;
    static constexpr int COLOR_CHANGE_INTERVAL = 200;  // Frames between colour changes
    // Two, because a double-buffered target displays the frame before last when this one is drawn
    static constexpr int ERASE_AGES = 2;
    // One snapshot per trail entry, plus enough to still reproduce the trails of ERASE_AGES frames
    // ago: a ring of only TRAIL_LENGTH slots loses the oldest snapshot the moment it advances.
    static constexpr int HISTORY_LENGTH = TRAIL_LENGTH + ERASE_AGES;

    MystifyDemo() = default;
    ~MystifyDemo() = default;

    MystifyDemo(const MystifyDemo&) = delete;
    MystifyDemo& operator=(const MystifyDemo&) = delete;
    MystifyDemo(MystifyDemo&&) = delete;
    MystifyDemo& operator=(MystifyDemo&&) = delete;

    /** @param target must outlive this object */
    bool init(gfx::Target* target) {
        if (target == nullptr || !target->valid()) {
            return false;
        }

        target_ = target;
        width_ = target->info().width;
        height_ = target->info().height;
        if (width_ <= 0 || height_ <= 0) {
            return false;
        }

        srand(static_cast<unsigned>(esp_random()));

        initPolygons();
        return true;
    }

    void update() {
        if (target_ == nullptr) {
            return;
        }

        for (auto& polygon : polygons_) {
            advance(polygon);
        }

        target_->render([this](gfx::Surface& surface) {
            draw(surface);
        }, gfx::Prepare::Overwrite);
    }

private:
    // Smooth sub-pixel movement with floats
    struct Vertex {
        float x = 0;
        float y = 0;
        float dx = 0;
        float dy = 0;
    };

    struct Polygon {
        Vertex vertices[NUM_VERTICES];
        // History: [snapshot_index][vertex_index] = {x, y}
        int16_t historyX[HISTORY_LENGTH][NUM_VERTICES];
        int16_t historyY[HISTORY_LENGTH][NUM_VERTICES];
        uint8_t colorIndex = 0;
        int historyHead = 0;
        int colorChangeCounter = 0;
    };

    // Vibrant colours as RGB888, converted per pixel for format-agnostic rendering
    struct Color {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    static constexpr Color COLOR_POOL[] = {
        { 255, 0, 255 },    // Magenta
        { 0, 255, 255 },    // Cyan
        { 255, 255, 0 },    // Yellow
        { 255, 128, 0 },    // Orange
        { 0, 255, 128 },    // Spring green
        { 128, 0, 255 },    // Purple
        { 255, 64, 128 },   // Hot pink
        { 128, 255, 0 },    // Lime
    };
    static constexpr int COLOR_POOL_SIZE = sizeof(COLOR_POOL) / sizeof(COLOR_POOL[0]);

    gfx::Target* target_ = nullptr;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    Polygon polygons_[NUM_POLYGONS];

    static float randomFloat(float min, float max) {
        return min + (max - min) * (static_cast<float>(rand()) / static_cast<float>(RAND_MAX));
    }

    static uint16_t toRgb565(uint8_t r, uint8_t g, uint8_t b) {
        return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

    void initPolygons() {
        for (auto& polygon : polygons_) {
            polygon.colorIndex = static_cast<uint8_t>(rand() % COLOR_POOL_SIZE);
            polygon.historyHead = 0;
            // Stagger colour changes so both polygons don't change at the same time
            polygon.colorChangeCounter = rand() % COLOR_CHANGE_INTERVAL;

            for (auto& vertex : polygon.vertices) {
                vertex.x = static_cast<float>(rand() % width_);
                vertex.y = static_cast<float>(rand() % height_);

                vertex.dx = randomFloat(0.8f, 2.0f);
                vertex.dy = randomFloat(0.8f, 2.0f);
                if (rand() % 2) vertex.dx = -vertex.dx;
                if (rand() % 2) vertex.dy = -vertex.dy;

                // Ensure dx != dy for more interesting movement patterns
                if (std::fabs(vertex.dx - vertex.dy) < 0.3f) {
                    vertex.dy += (vertex.dy > 0 ? 0.5f : -0.5f);
                }
            }

            for (int t = 0; t < HISTORY_LENGTH; t++) {
                for (int v = 0; v < NUM_VERTICES; v++) {
                    polygon.historyX[t][v] = static_cast<int16_t>(polygon.vertices[v].x);
                    polygon.historyY[t][v] = static_cast<int16_t>(polygon.vertices[v].y);
                }
            }
        }
    }

    /** Moves the polygon and records its new position. Drawing is a separate step. */
    void advance(Polygon& polygon) {
        constexpr float minSpeed = 0.5f;
        constexpr float maxSpeed = 2.5f;

        polygon.colorChangeCounter++;
        if (polygon.colorChangeCounter >= COLOR_CHANGE_INTERVAL) {
            polygon.colorChangeCounter = 0;
            uint8_t newColor;
            do {
                newColor = static_cast<uint8_t>(rand() % COLOR_POOL_SIZE);
            } while (newColor == polygon.colorIndex && COLOR_POOL_SIZE > 1);
            polygon.colorIndex = newColor;
        }

        for (auto& vertex : polygon.vertices) {
            vertex.x += vertex.dx;
            vertex.y += vertex.dy;

            // Bounce off the edges with a slight angle variation for organic movement
            if (vertex.x <= 0) {
                vertex.x = 0;
                vertex.dx = std::fabs(vertex.dx);
                vertex.dy *= (1.0f + randomFloat(-0.1f, 0.1f));
            } else if (vertex.x >= width_ - 1) {
                vertex.x = static_cast<float>(width_ - 1);
                vertex.dx = -std::fabs(vertex.dx);
                vertex.dy *= (1.0f + randomFloat(-0.1f, 0.1f));
            }

            if (vertex.y <= 0) {
                vertex.y = 0;
                vertex.dy = std::fabs(vertex.dy);
                vertex.dx *= (1.0f + randomFloat(-0.1f, 0.1f));
            } else if (vertex.y >= height_ - 1) {
                vertex.y = static_cast<float>(height_ - 1);
                vertex.dy = -std::fabs(vertex.dy);
                vertex.dx *= (1.0f + randomFloat(-0.1f, 0.1f));
            }

            auto clampSpeed = [](float& speed) {
                const float sign = (speed >= 0) ? 1.0f : -1.0f;
                float absolute = std::fabs(speed);
                if (absolute < minSpeed) absolute = minSpeed;
                if (absolute > maxSpeed) absolute = maxSpeed;
                speed = sign * absolute;
            };
            clampSpeed(vertex.dx);
            clampSpeed(vertex.dy);
        }

        // Advance the history ring buffer
        polygon.historyHead = (polygon.historyHead + 1) % HISTORY_LENGTH;

        for (int v = 0; v < NUM_VERTICES; v++) {
            polygon.historyX[polygon.historyHead][v] = static_cast<int16_t>(polygon.vertices[v].x);
            polygon.historyY[polygon.historyHead][v] = static_cast<int16_t>(polygon.vertices[v].y);
        }
    }

    /** Draws the polygon's single quad for one snapshot @a offset frames before the newest. */
    void drawSnapshot(gfx::Surface& surface, const Polygon& polygon, int offset, uint16_t color) const {
        int index = polygon.historyHead - offset;
        while (index < 0) {
            index += HISTORY_LENGTH;
        }

        for (int e = 0; e < NUM_VERTICES; e++) {
            const int nextVertex = (e + 1) % NUM_VERTICES;
            surface.line(
                polygon.historyX[index][e],
                polygon.historyY[index][e],
                polygon.historyX[index][nextVertex],
                polygon.historyY[index][nextVertex],
                color
            );
        }
    }

    /** Draws the current trail, oldest snapshot first so the newest ends up on top. */
    void drawTrail(gfx::Surface& surface, const Polygon& polygon) const {
        const Color& baseColor = COLOR_POOL[polygon.colorIndex];

        for (int t = TRAIL_LENGTH - 1; t >= 0; t--) {
            // Older trail frames are dimmer
            int brightness = 255 - (t * 230 / TRAIL_LENGTH);
            if (brightness < 25) brightness = 25;

            const uint16_t color = toRgb565(
                static_cast<uint8_t>(baseColor.r * brightness / 255),
                static_cast<uint8_t>(baseColor.g * brightness / 255),
                static_cast<uint8_t>(baseColor.b * brightness / 255)
            );

            drawSnapshot(surface, polygon, t, color);
        }
    }

    /** Draws one frame. Safe to call more than once per update (banded mode does). */
    void draw(gfx::Surface& surface) const {
        // Only the snapshots that dropped out of the trail since the frame in the back buffer was
        // drawn need erasing: the ones still in the trail are redrawn anyway, with the brightness
        // of their new position. Erasing one polygon after another has been drawn would wipe that
        // one's trail, so every erase happens before any draw.
        for (const auto& polygon : polygons_) {
            for (int offset = TRAIL_LENGTH; offset < TRAIL_LENGTH + ERASE_AGES; offset++) {
                drawSnapshot(surface, polygon, offset, 0x0000);
            }
        }

        for (const auto& polygon : polygons_) {
            drawTrail(surface, polygon);
        }
    }
};
