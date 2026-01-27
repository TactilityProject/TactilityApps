#pragma once

#include "PixelBuffer.h"
#include "drivers/DisplayDriver.h"
#include <cmath>
#include <cstdlib>
#include <esp_random.h>

/**
 * Mystify Screensaver Demo
 *
 * Classic Windows-style mystify screensaver with bouncing polygons and trailing edges.
 * Adapted to work with DisplayDriver and PixelBuffer abstractions.
 *
 * Usage:
 *   MystifyDemo mystify;
 *   mystify.init(display);
 *
 *   while (!shouldExit) {
 *       mystify.update();
 *   }
 */
class MystifyDemo {
public:
    static constexpr int NUM_POLYGONS = 2;
    static constexpr int NUM_VERTICES = 4;
    static constexpr int TRAIL_LENGTH = 8;
    static constexpr int COLOR_CHANGE_INTERVAL = 200;  // Frames between color changes
    static constexpr int STRIP_HEIGHT = 16;  // Draw in strips to avoid SPI buffer overflow

    MystifyDemo() = default;
    ~MystifyDemo() { deinit(); }

    // Non-copyable, non-movable (owns PixelBuffer)
    MystifyDemo(const MystifyDemo&) = delete;
    MystifyDemo& operator=(const MystifyDemo&) = delete;
    MystifyDemo(MystifyDemo&&) = delete;
    MystifyDemo& operator=(MystifyDemo&&) = delete;

    bool init(DisplayDriver* display) {
        if (!display) {
            return false;
        }

        display_ = display;
        width_ = display->getWidth();
        height_ = display->getHeight();

        if (width_ <= 0 || height_ <= 0) {
            return false;
        }

        // Seed random generator with hardware entropy
        srand(static_cast<unsigned>(esp_random()));

        // Allocate full-screen framebuffer
        void* mem = malloc(sizeof(PixelBuffer));
        if (!mem) {
            return false;
        }
        framebuffer_ = new(mem) PixelBuffer(width_, height_, display->getColorFormat());

        initPolygons();
        return true;
    }

    void deinit() {
        if (framebuffer_) {
            framebuffer_->~PixelBuffer();
            free(framebuffer_);
            framebuffer_ = nullptr;
        }
        display_ = nullptr;
    }

    void update() {
        if (!framebuffer_ || !display_) return;

        // Clear framebuffer to black
        framebuffer_->clear();

        // Update and draw each polygon
        for (int p = 0; p < NUM_POLYGONS; p++) {
            updatePolygon(polygons_[p]);
            drawPolygon(polygons_[p]);
        }

        // Send framebuffer to display in strips (full screen is too large for single SPI transaction)
        display_->lock();
        for (int y = 0; y < height_; y += STRIP_HEIGHT) {
            int stripEnd = (y + STRIP_HEIGHT > height_) ? height_ : y + STRIP_HEIGHT;
            display_->drawBitmap(0, y, width_, stripEnd, framebuffer_->getDataAtRow(y));
        }
        display_->unlock();
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
        // History: [trail_index][vertex_index] = {x, y}
        int16_t historyX[TRAIL_LENGTH][NUM_VERTICES];
        int16_t historyY[TRAIL_LENGTH][NUM_VERTICES];
        uint8_t colorIndex;
        int historyHead = 0;
        bool historyFull = false;
        int colorChangeCounter = 0;
    };

    // Vibrant colors as RGB888 for format-agnostic rendering
    struct Color {
        uint8_t r, g, b;
    };

    static constexpr Color COLOR_POOL[] = {
        {255, 0, 255},    // Magenta
        {0, 255, 255},    // Cyan
        {255, 255, 0},    // Yellow
        {255, 128, 0},    // Orange
        {0, 255, 128},    // Spring green
        {128, 0, 255},    // Purple
        {255, 64, 128},   // Hot pink
        {128, 255, 0},    // Lime
    };
    static constexpr int COLOR_POOL_SIZE = sizeof(COLOR_POOL) / sizeof(COLOR_POOL[0]);

    DisplayDriver* display_ = nullptr;
    PixelBuffer* framebuffer_ = nullptr;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    Polygon polygons_[NUM_POLYGONS];

    static float randomFloat(float min, float max) {
        return min + (max - min) * (static_cast<float>(rand()) / static_cast<float>(RAND_MAX));
    }

    void initPolygons() {
        for (int p = 0; p < NUM_POLYGONS; p++) {
            Polygon& polygon = polygons_[p];

            // Pick random color from pool
            polygon.colorIndex = rand() % COLOR_POOL_SIZE;
            polygon.historyHead = 0;
            polygon.historyFull = false;
            // Stagger color changes so polygons don't change simultaneously
            polygon.colorChangeCounter = rand() % COLOR_CHANGE_INTERVAL;

            // Initialize vertices with random positions and velocities
            for (int v = 0; v < NUM_VERTICES; v++) {
                Vertex& vertex = polygon.vertices[v];
                vertex.x = static_cast<float>(rand() % width_);
                vertex.y = static_cast<float>(rand() % height_);

                // Speed range for smooth movement
                vertex.dx = randomFloat(0.8f, 2.0f);
                vertex.dy = randomFloat(0.8f, 2.0f);
                if (rand() % 2) vertex.dx = -vertex.dx;
                if (rand() % 2) vertex.dy = -vertex.dy;

                // Ensure dx != dy for more interesting movement patterns
                if (std::fabs(vertex.dx - vertex.dy) < 0.3f) {
                    vertex.dy += (vertex.dy > 0 ? 0.5f : -0.5f);
                }
            }

            // Initialize history with current positions
            for (int t = 0; t < TRAIL_LENGTH; t++) {
                for (int v = 0; v < NUM_VERTICES; v++) {
                    polygon.historyX[t][v] = static_cast<int16_t>(polygon.vertices[v].x);
                    polygon.historyY[t][v] = static_cast<int16_t>(polygon.vertices[v].y);
                }
            }
        }
    }

    void updatePolygon(Polygon& polygon) {
        constexpr float minSpeed = 0.5f;
        constexpr float maxSpeed = 2.5f;

        // Periodic color change
        polygon.colorChangeCounter++;
        if (polygon.colorChangeCounter >= COLOR_CHANGE_INTERVAL) {
            polygon.colorChangeCounter = 0;
            // Pick a different color
            uint8_t newColor;
            do {
                newColor = rand() % COLOR_POOL_SIZE;
            } while (newColor == polygon.colorIndex && COLOR_POOL_SIZE > 1);
            polygon.colorIndex = newColor;
        }

        // Move vertices
        for (int v = 0; v < NUM_VERTICES; v++) {
            Vertex& vertex = polygon.vertices[v];
            vertex.x += vertex.dx;
            vertex.y += vertex.dy;

            // Bounce off edges with slight angle variation for organic movement
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

            // Clamp speeds to prevent runaway acceleration or stalling
            auto clampSpeed = [minSpeed, maxSpeed](float& speed) {
                float sign = (speed >= 0) ? 1.0f : -1.0f;
                float absSpeed = std::fabs(speed);
                if (absSpeed < minSpeed) absSpeed = minSpeed;
                if (absSpeed > maxSpeed) absSpeed = maxSpeed;
                speed = sign * absSpeed;
            };
            clampSpeed(vertex.dx);
            clampSpeed(vertex.dy);
        }

        // Advance history ring buffer
        polygon.historyHead = (polygon.historyHead + 1) % TRAIL_LENGTH;
        if (polygon.historyHead == 0) {
            polygon.historyFull = true;
        }

        // Store current positions
        for (int v = 0; v < NUM_VERTICES; v++) {
            polygon.historyX[polygon.historyHead][v] = static_cast<int16_t>(polygon.vertices[v].x);
            polygon.historyY[polygon.historyHead][v] = static_cast<int16_t>(polygon.vertices[v].y);
        }
    }

    void drawPolygon(const Polygon& polygon) {
        const Color& baseColor = COLOR_POOL[polygon.colorIndex];

        // Draw trail from oldest to newest (so newest is on top)
        for (int t = TRAIL_LENGTH - 1; t >= 0; t--) {
            int histIndex = polygon.historyHead - t;
            if (histIndex < 0) histIndex += TRAIL_LENGTH;

            // Skip if we don't have enough history yet
            if (!polygon.historyFull && histIndex > polygon.historyHead) {
                continue;
            }

            // Calculate brightness for this trail frame (older = dimmer)
            int brightness = 255 - (t * 230 / TRAIL_LENGTH);
            if (brightness < 25) brightness = 25;

            // Scale color by brightness
            uint8_t r = (baseColor.r * brightness) / 255;
            uint8_t g = (baseColor.g * brightness) / 255;
            uint8_t b = (baseColor.b * brightness) / 255;

            // Draw edges connecting vertices
            for (int e = 0; e < NUM_VERTICES; e++) {
                int nextVertex = (e + 1) % NUM_VERTICES;

                int x0 = polygon.historyX[histIndex][e];
                int y0 = polygon.historyY[histIndex][e];
                int x1 = polygon.historyX[histIndex][nextVertex];
                int y1 = polygon.historyY[histIndex][nextVertex];

                drawLine(x0, y0, x1, y1, r, g, b);
            }
        }
    }

    // Bresenham's line algorithm
    void drawLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
        int dx = std::abs(x1 - x0);
        int dy = std::abs(y1 - y0);
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = dx - dy;

        while (true) {
            setPixel(x0, y0, r, g, b);

            if (x0 == x1 && y0 == y1) break;

            int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x >= 0 && x < width_ && y >= 0 && y < height_) {
            framebuffer_->setPixel(x, y, r, g, b);
        }
    }
};
