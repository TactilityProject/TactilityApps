#include "DoomRenderer.h"

#include "DoomPlatform.h"
#include "TouchOverlay.h"

#include "doomgeneric.h"

#include <tactility/drivers/display.h>
#include <tactility/log.h>
#include <tactility/memory.h>

#if DOOM_USE_PPA
#include <driver/ppa.h>
#include <esp_cache.h>
#endif

#include <cstring>

constexpr auto* TAG = "DoomRenderer";

// DOOM's screen: 320 pixels wide. The game is 200 high; the control panel canvas is up to 200.
constexpr int SRC_W = DOOMGENERIC_RESX;
constexpr int GAME_H = DOOMGENERIC_RESY;

// From i_video.c: the engine's current palette
struct DoomColor {
    uint32_t b : 8;
    uint32_t g : 8;
    uint32_t r : 8;
    uint32_t a : 8;
};
extern "C" DoomColor colors[256];

// region Layout

// A source image (game or control panel) and where it ends up.
//
// "User" coordinates are the display as the user sees it (LVGL's logical orientation). "Draw"
// coordinates are what display_draw_bitmap() takes: the user's view rotated by drawRotation.
struct Layer {
    bool enabled;
    int srcHeight;
    // Destination rectangle in user coordinates
    int ux, uy, uw, uh;
    // The same rectangle in draw coordinates
    int dx, dy, dw, dh;
    // User-local x/y -> source x/y (nearest neighbour)
    uint16_t* xMap;
    uint16_t* yMap;
#if DOOM_USE_PPA
    uint16_t* rgb565; // PPA input: the source converted through the palette
#else
    uint16_t* buffer; // RGB16 mode: the scaled, rotated layer in the panel's colour format
#endif
};

enum { LAYER_GAME, LAYER_PANEL, LAYER_COUNT };

static Device* display = nullptr;
static DoomScreen screen = {};
static int userW = 0;
static int userH = 0;
static Layer layers[LAYER_COUNT] = {};
static int panelHeight = 0;
static float layoutScale = 1.0f;

static float quantise(float scale, float step) {
    if (step <= 0.0f) {
        return scale;
    }
    // Hardware scale factors are quantised; round down so the image never overflows
    float q = (float)(int)(scale / step) * step;
    return q < step ? step : q;
}

static void placeLayer(Layer& layer, int srcHeight, int ux, int uy, int uw, int uh) {
    layer.enabled = true;
    layer.srcHeight = srcHeight;
    layer.ux = ux;
    layer.uy = uy;
    layer.uw = uw;
    layer.uh = uh;
    // User -> draw rotation (counter-clockwise quarter turns):
    // 1: (x, y) -> (y, userW - 1 - x), 2: -> (userW - 1 - x, userH - 1 - y), 3: -> (userH - 1 - y, x)
    switch (screen.drawRotation) {
        case 1: layer.dx = uy; layer.dy = userW - ux - uw; layer.dw = uh; layer.dh = uw; break;
        case 2: layer.dx = userW - ux - uw; layer.dy = userH - uy - uh; layer.dw = uw; layer.dh = uh; break;
        case 3: layer.dx = userH - uy - uh; layer.dy = ux; layer.dw = uh; layer.dh = uw; break;
        default: layer.dx = ux; layer.dy = uy; layer.dw = uw; layer.dh = uh; break;
    }
}

static void computeLayout(bool allowPanel, float scaleStep) {
    const bool sideways = (screen.drawRotation & 1) != 0;
    userW = (int)(sideways ? screen.drawHeight : screen.drawWidth);
    userH = (int)(sideways ? screen.drawWidth : screen.drawHeight);
    panelHeight = 0;

    // Portrait with room to spare: game full width at the top, control panel below it
    if (allowPanel && userH > userW) {
        const float scale = quantise((float)userW / SRC_W, scaleStep);
        const int gw = (int)(SRC_W * scale);
        const int gh = (int)(GAME_H * scale);
        const int available = userH - gh;
        int canvasHeight = (int)(available / scale);
        if (canvasHeight > GAME_H) {
            canvasHeight = GAME_H;
        }
        if (canvasHeight >= TOUCH_OVERLAY_MIN_PANEL_HEIGHT) {
            const int ph = (int)(canvasHeight * scale);
            const int x = (userW - gw) / 2;
            layoutScale = scale;
            panelHeight = canvasHeight;
            placeLayer(layers[LAYER_GAME], GAME_H, x, 0, gw, gh);
            placeLayer(layers[LAYER_PANEL], canvasHeight, x, gh + (available - ph) / 2, gw, ph);
            LOG_I(TAG, "User view %dx%d (rotation %d): game %dx%d at the top, control panel %dx%d below",
                userW, userH, screen.drawRotation, gw, gh, gw, ph);
            return;
        }
    }

    // Otherwise the game is scaled to fit and centred
    float scale = (float)userW / SRC_W;
    if ((float)userH / GAME_H < scale) {
        scale = (float)userH / GAME_H;
    }
    scale = quantise(scale, scaleStep);
    int gw = (int)(SRC_W * scale);
    int gh = (int)(GAME_H * scale);
    if (gw > userW) gw = userW;
    if (gh > userH) gh = userH;
    layoutScale = scale;
    placeLayer(layers[LAYER_GAME], GAME_H, (userW - gw) / 2, (userH - gh) / 2, gw, gh);
    layers[LAYER_PANEL].enabled = false;
    LOG_I(TAG, "User view %dx%d (rotation %d): game %dx%d", userW, userH, screen.drawRotation, gw, gh);
}

static bool allocateMaps(Layer& layer) {
    layer.xMap = (uint16_t*)memory_alloc(layer.uw * sizeof(uint16_t));
    layer.yMap = (uint16_t*)memory_alloc(layer.uh * sizeof(uint16_t));
    if (layer.xMap == nullptr || layer.yMap == nullptr) {
        return false;
    }
    for (int x = 0; x < layer.uw; x++) {
        layer.xMap[x] = (uint16_t)(x * SRC_W / layer.uw);
    }
    for (int y = 0; y < layer.uh; y++) {
        layer.yMap[y] = (uint16_t)(y * layer.srcHeight / layer.uh);
    }
    return true;
}

// endregion

// region Software rendering

#if !DOOM_USE_PPA

static bool swapBytes = false;
static bool swapRedBlue = false;

// Output pixel format of the software path
enum class PixelMode {
    Rgb16, // RGB565/BGR565, optionally byte-swapped. Each layer is drawn as its own rectangle.
    Gray8, // 8 bpp luminance. Dithered to black/white, full screen drawn.
    Mono, // 1 bpp, rows packed MSB-first, bit 1 = white. Dithered, full screen drawn.
};
static PixelMode pixelMode = PixelMode::Rgb16;
// Full-screen buffer for Gray8/Mono. Everything outside the layers stays black (zero).
static uint8_t* screenBuffer = nullptr;
static size_t screenStride = 0;
static uint16_t* clearBuffer = nullptr; // zeroed rows used to paint the screen black once

// Calls write(drawX, drawY, paletteIndex) for every pixel of a layer's draw rectangle, row by row.
// Inverse of placeLayer(): draw-local (rx, ry) shows user-local
// 0: (rx, ry), 1: (uw - 1 - ry, rx), 2: (uw - 1 - rx, uh - 1 - ry), 3: (ry, uh - 1 - rx)
template<typename Writer>
static inline void forEachPixel(const Layer& layer, const uint8_t* src, Writer write) {
    const uint16_t* xMap = layer.xMap;
    const uint16_t* yMap = layer.yMap;
    for (int ry = 0; ry < layer.dh; ry++) {
        switch (screen.drawRotation) {
            case 1: {
                const uint8_t* column = src + xMap[layer.uw - 1 - ry];
                for (int rx = 0; rx < layer.dw; rx++) {
                    write(rx, ry, column[yMap[rx] * SRC_W]);
                }
                break;
            }
            case 2: {
                const uint8_t* row = src + yMap[layer.uh - 1 - ry] * SRC_W;
                for (int rx = 0; rx < layer.dw; rx++) {
                    write(rx, ry, row[xMap[layer.uw - 1 - rx]]);
                }
                break;
            }
            case 3: {
                const uint8_t* column = src + xMap[ry];
                for (int rx = 0; rx < layer.dw; rx++) {
                    write(rx, ry, column[yMap[layer.uh - 1 - rx] * SRC_W]);
                }
                break;
            }
            default: {
                const uint8_t* row = src + yMap[ry] * SRC_W;
                for (int rx = 0; rx < layer.dw; rx++) {
                    write(rx, ry, row[xMap[rx]]);
                }
                break;
            }
        }
    }
}

static inline uint16_t toPanelColour(uint16_t c) {
    if (swapRedBlue) {
        c = (uint16_t)(((c & 0x001F) << 11) | (c & 0x07E0) | ((c & 0xF800) >> 11));
    }
    if (swapBytes) {
        c = (uint16_t)((c << 8) | (c >> 8));
    }
    return c;
}

// 8x8 Bayer matrix for ordered dithering. Ordered (rather than error diffusion) dithering is
// stable from frame to frame, so moving scenes don't shimmer, and costs one compare per pixel.
static const uint8_t BAYER8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

static uint8_t isqrt16(uint32_t v) {
    uint32_t r = 0;
    for (uint32_t bit = 1u << 14; bit != 0; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return (uint8_t)r;
}

static void drawDithered(const uint8_t* sources[LAYER_COUNT]) {
    // Palette luminance, brightened: DOOM is dark, and thresholded to 1 bit the shadows would
    // otherwise all collapse to black. Averaging linear and sqrt is roughly a 0.7 gamma.
    uint8_t luma[256];
    for (int i = 0; i < 256; i++) {
        const DoomColor& c = colors[i];
        const uint32_t l = (77u * c.r + 150u * c.g + 29u * c.b) >> 8;
        luma[i] = (uint8_t)((l + isqrt16(l * 255u)) / 2);
    }

    for (int i = 0; i < LAYER_COUNT; i++) {
        const Layer& layer = layers[i];
        if (!layer.enabled || sources[i] == nullptr) {
            continue;
        }
        forEachPixel(layer, sources[i], [&](int rx, int ry, uint8_t index) {
            const int x = layer.dx + rx;
            const int y = layer.dy + ry;
            const bool white = luma[index] > (uint8_t)(BAYER8[y & 7][x & 7] * 4 + 2);
            uint8_t* row = screenBuffer + (size_t)y * screenStride;
            if (pixelMode == PixelMode::Mono) {
                const uint8_t mask = (uint8_t)(0x80 >> (x & 7));
                row[x >> 3] = white ? (row[x >> 3] | mask) : (row[x >> 3] & (uint8_t)~mask);
            } else {
                row[x] = white ? 0xFF : 0x00;
            }
        });
    }

    display_draw_bitmap(display, 0, 0, (int32_t)screen.drawWidth, (int32_t)screen.drawHeight, screenBuffer);
}

static void* allocPreferInternal(size_t size) {
    const MemoryPolicy policy = { .required = 0, .desired = MEMORY_CAPABILITY_INTERNAL | MEMORY_CAPABILITY_DMA, .alignment = 0 };
    return memory_alloc_with_policy(size, &policy);
}

static bool selectPixelMode() {
    pixelMode = PixelMode::Rgb16;
    switch (display_get_color_format(display)) {
        case DISPLAY_COLOR_FORMAT_MONOCHROME: pixelMode = PixelMode::Mono; return true;
        case DISPLAY_COLOR_FORMAT_GRAYSCALE8: pixelMode = PixelMode::Gray8; return true;
        case DISPLAY_COLOR_FORMAT_RGB565: swapBytes = false; swapRedBlue = false; return true;
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED: swapBytes = true; swapRedBlue = false; return true;
        case DISPLAY_COLOR_FORMAT_BGR565: swapBytes = false; swapRedBlue = true; return true;
        case DISPLAY_COLOR_FORMAT_BGR565_SWAPPED: swapBytes = true; swapRedBlue = true; return true;
        default:
            LOG_E(TAG, "Unsupported display colour format");
            return false;
    }
}

static bool initSoftware() {
    if (pixelMode != PixelMode::Rgb16) {
        // Monochrome panels take whole frames (bytes hold 8 pixels across, and the LVGL bridge
        // also always draws them in full), so the whole screen is buffered and redrawn
        screenStride = pixelMode == PixelMode::Mono ? (screen.drawWidth + 7) / 8 : screen.drawWidth;
        screenBuffer = (uint8_t*)memory_calloc(screenStride * screen.drawHeight, 1);
        if (screenBuffer == nullptr) {
            LOG_E(TAG, "Failed to allocate the frame buffer");
            return false;
        }
        LOG_I(TAG, "%s display: dithering to black and white", pixelMode == PixelMode::Mono ? "Monochrome" : "Grayscale");
        return true;
    }

    for (auto& layer : layers) {
        if (layer.enabled) {
            layer.buffer = (uint16_t*)allocPreferInternal((size_t)layer.dw * layer.dh * 2);
            if (layer.buffer == nullptr) {
                LOG_E(TAG, "Failed to allocate the frame buffer");
                return false;
            }
        }
    }

    // Paint the whole screen black once: only the layers are redrawn per frame
    constexpr int CLEAR_ROWS = 8;
    clearBuffer = (uint16_t*)allocPreferInternal((size_t)screen.drawWidth * CLEAR_ROWS * 2);
    if (clearBuffer != nullptr) {
        memset(clearBuffer, 0, (size_t)screen.drawWidth * CLEAR_ROWS * 2);
        for (int y = 0; y < (int)screen.drawHeight; y += CLEAR_ROWS) {
            const int yEnd = y + CLEAR_ROWS > (int)screen.drawHeight ? (int)screen.drawHeight : y + CLEAR_ROWS;
            display_draw_bitmap(display, 0, y, (int32_t)screen.drawWidth, yEnd, clearBuffer);
        }
    }
    return true;
}

static void drawSoftware(const uint8_t* sources[LAYER_COUNT]) {
    if (pixelMode != PixelMode::Rgb16) {
        drawDithered(sources);
        return;
    }

    // Palette in the panel's own format (byte order, RGB/BGR), so the per-pixel work is a single
    // table lookup. Rebuilt every frame: 256 entries is negligible next to the pixel count, and it
    // picks up palette changes (damage/pickup flashes, gamma) without extra hooks.
    uint16_t lut[256];
    for (int i = 0; i < 256; i++) {
        const DoomColor& c = colors[i];
        lut[i] = toPanelColour((uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3)));
    }

    for (int i = 0; i < LAYER_COUNT; i++) {
        const Layer& layer = layers[i];
        if (!layer.enabled || sources[i] == nullptr) {
            continue;
        }
        uint16_t* out = layer.buffer;
        const int stride = layer.dw;
        forEachPixel(layer, sources[i], [&](int rx, int ry, uint8_t index) {
            out[ry * stride + rx] = lut[index];
        });
        display_draw_bitmap(display, layer.dx, layer.dy, layer.dx + layer.dw, layer.dy + layer.dh, layer.buffer);
    }
}

static void shutdownSoftware() {
    for (auto& layer : layers) {
        memory_free(layer.buffer);
        layer.buffer = nullptr;
    }
    memory_free(screenBuffer);
    screenBuffer = nullptr;
    memory_free(clearBuffer);
    clearBuffer = nullptr;
    pixelMode = PixelMode::Rgb16;
}

#endif

// endregion

// region PPA rendering

#if DOOM_USE_PPA

static uint16_t* outputBuffer = nullptr;
static uint16_t* hwFrameBuffers[2] = { nullptr, nullptr }; // panel-owned double buffer, when supported
static int backBufferIndex = 1; // PPA writes here; cur_fb_index starts at 0, so back buffer starts at 1
static bool usingHwFrameBuffer = false;
static ppa_client_handle_t ppaClient = nullptr;

static bool initPpa() {
    // Per-layer PPA input: the source converted to RGB565 (internal RAM preferred for DMA, PSRAM fallback)
    const MemoryPolicy inputPolicy = { .required = 0, .desired = MEMORY_CAPABILITY_INTERNAL, .alignment = 64 };
    for (auto& layer : layers) {
        if (layer.enabled) {
            layer.rgb565 = (uint16_t*)memory_alloc_with_policy((size_t)SRC_W * layer.srcHeight * 2, &inputPolicy);
            if (layer.rgb565 == nullptr) {
                LOG_E(TAG, "Failed to allocate the frame buffer");
                return false;
            }
        }
    }

    // PPA output: prefer the panel's own double-buffered frame buffers (num_fbs=2): the PPA writes
    // into the back buffer while the DSI controller scans out the front one, then draw_bitmap()
    // flips them at the next frame boundary - no tearing. Otherwise a separate PSRAM buffer.
    //
    // Either way the buffers are cleared to black once up front: the PPA only writes the layer
    // blocks, so everything around them stays black.
    const size_t fbSize = (size_t)screen.drawWidth * screen.drawHeight * 2;
    if (display_get_frame_buffer_count(display) == 2) {
        LOG_I(TAG, "Using direct hardware frame buffers for PPA output");
        void* fb0 = nullptr;
        void* fb1 = nullptr;
        display_get_frame_buffer(display, 0, &fb0);
        display_get_frame_buffer(display, 1, &fb1);
        hwFrameBuffers[0] = (uint16_t*)fb0;
        hwFrameBuffers[1] = (uint16_t*)fb1;
        backBufferIndex = 1;
        usingHwFrameBuffer = true;
        for (auto* fb : hwFrameBuffers) {
            memset(fb, 0, fbSize);
            esp_cache_msync(fb, fbSize, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
    } else {
        const MemoryPolicy outputPolicy = { .required = MEMORY_CAPABILITY_EXTERNAL, .desired = 0, .alignment = 64 };
        outputBuffer = (uint16_t*)memory_alloc_with_policy(fbSize, &outputPolicy);
        if (outputBuffer == nullptr) {
            LOG_E(TAG, "Failed to allocate the output buffer");
            return false;
        }
        memset(outputBuffer, 0, fbSize);
        usingHwFrameBuffer = false;
    }

    ppa_client_config_t ppaConfig = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        .flags = { .allow_pd = 0 },
    };
    if (ppa_register_client(&ppaConfig, &ppaClient) != ESP_OK) {
        LOG_E(TAG, "Failed to register PPA client");
        return false;
    }
    return true;
}

static void drawPpa(const uint8_t* sources[LAYER_COUNT]) {
    uint16_t lut[256];
    for (int i = 0; i < 256; i++) {
        const DoomColor& c = colors[i];
        lut[i] = (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
    }

    uint16_t* target = usingHwFrameBuffer ? hwFrameBuffers[backBufferIndex] : outputBuffer;
    const size_t fbSize = (size_t)screen.drawWidth * screen.drawHeight * 2;

    for (int i = 0; i < LAYER_COUNT; i++) {
        Layer& layer = layers[i];
        if (!layer.enabled || sources[i] == nullptr) {
            continue;
        }
        const int pixels = SRC_W * layer.srcHeight;
        for (int p = 0; p < pixels; p++) {
            layer.rgb565[p] = lut[sources[i][p]];
        }
        esp_cache_msync(layer.rgb565, (size_t)pixels * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        ppa_srm_oper_config_t srm = {
            .in = {
                .buffer = layer.rgb565,
                .pic_w = (uint32_t)SRC_W,
                .pic_h = (uint32_t)layer.srcHeight,
                .block_w = (uint32_t)SRC_W,
                .block_h = (uint32_t)layer.srcHeight,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
            },
            .out = {
                .buffer = target,
                .buffer_size = (uint32_t)fbSize,
                .pic_w = screen.drawWidth,
                .pic_h = screen.drawHeight,
                .block_offset_x = (uint32_t)layer.dx,
                .block_offset_y = (uint32_t)layer.dy,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
            },
            // Same counter-clockwise convention as LVGL's rotation
            .rotation_angle = (ppa_srm_rotation_angle_t)screen.drawRotation,
            .scale_x = layoutScale,
            .scale_y = layoutScale,
            .mirror_x = false,
            .mirror_y = false,
            .rgb_swap = false,
            .byte_swap = false,
            .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
            .alpha_fix_val = 0,
            .mode = PPA_TRANS_MODE_BLOCKING,
            .user_data = nullptr,
        };
        ppa_do_scale_rotate_mirror(ppaClient, &srm);
    }

    esp_cache_msync(target, fbSize, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    display_draw_bitmap(display, 0, 0, (int32_t)screen.drawWidth, (int32_t)screen.drawHeight, target);

    if (usingHwFrameBuffer) {
        backBufferIndex = 1 - backBufferIndex;
    }
}

static void shutdownPpa() {
    if (ppaClient != nullptr) {
        ppa_unregister_client(ppaClient);
        ppaClient = nullptr;
    }
    for (auto& layer : layers) {
        memory_free(layer.rgb565);
        layer.rgb565 = nullptr;
    }
    memory_free(outputBuffer);
    outputBuffer = nullptr;
    // Panel-owned, not ours to free
    hwFrameBuffers[0] = nullptr;
    hwFrameBuffers[1] = nullptr;
    usingHwFrameBuffer = false;
    backBufferIndex = 1;
}

#endif

// endregion

bool doomRenderer_Init(Device* displayDevice, const DoomScreen& doomScreen, bool allowPanel) {
    display = displayDevice;
    screen = doomScreen;
    screen.drawRotation &= 3;
    screen.touchRotation &= 3;

#if DOOM_USE_PPA
    // PPA scale factors have 1/16 precision
    computeLayout(allowPanel, 1.0f / 16.0f);
#else
    if (!selectPixelMode()) {
        return false;
    }
    computeLayout(allowPanel, 0.0f);
#endif

    for (auto& layer : layers) {
        if (layer.enabled && !allocateMaps(layer)) {
            LOG_E(TAG, "Failed to allocate the scaling tables");
            return false;
        }
    }

#if DOOM_USE_PPA
    return initPpa();
#else
    return initSoftware();
#endif
}

void doomRenderer_Shutdown() {
#if DOOM_USE_PPA
    shutdownPpa();
#else
    shutdownSoftware();
#endif
    for (auto& layer : layers) {
        memory_free(layer.xMap);
        memory_free(layer.yMap);
        layer = {};
    }
    panelHeight = 0;
    display = nullptr;
}

int doomRenderer_PanelHeight() {
    return panelHeight;
}

void doomRenderer_Draw(const uint8_t* game, const uint8_t* panel) {
    const uint8_t* sources[LAYER_COUNT] = { game, panel };
#if DOOM_USE_PPA
    drawPpa(sources);
#else
    drawSoftware(sources);
#endif
}

DoomTouchTarget doomRenderer_MapTouch(int touchX, int touchY, int* outX, int* outY) {
    // Raw touch -> user view. The touch space is the user's view rotated by touchRotation;
    // inverse of the mapping in placeLayer().
    const bool sideways = (screen.touchRotation & 1) != 0;
    const int tw = (int)(sideways ? screen.touchHeight : screen.touchWidth); // user-view size in touch units
    const int th = (int)(sideways ? screen.touchWidth : screen.touchHeight);
    int ux, uy;
    switch (screen.touchRotation) {
        case 1: ux = tw - 1 - touchY; uy = touchX; break;
        case 2: ux = tw - 1 - touchX; uy = th - 1 - touchY; break;
        case 3: ux = touchY; uy = th - 1 - touchX; break;
        default: ux = touchX; uy = touchY; break;
    }
    // Touch panels can report a different resolution than the display
    if (tw > 0 && th > 0 && (tw != userW || th != userH)) {
        ux = ux * userW / tw;
        uy = uy * userH / th;
    }

    for (int i = 0; i < LAYER_COUNT; i++) {
        const Layer& layer = layers[i];
        if (!layer.enabled) {
            continue;
        }
        const int lx = ux - layer.ux;
        const int ly = uy - layer.uy;
        if (lx >= 0 && ly >= 0 && lx < layer.uw && ly < layer.uh) {
            *outX = lx * SRC_W / layer.uw;
            *outY = ly * layer.srcHeight / layer.uh;
            return i == LAYER_GAME ? DoomTouchTarget::Game : DoomTouchTarget::Panel;
        }
    }
    return DoomTouchTarget::None;
}
