#include "DisplayOutput.h"

#include <tactility/drivers/display.h>
#include <tactility/log.h>
#include <tactility/memory.h>

#ifdef ESP_PLATFORM
#include <soc/soc_caps.h>
#endif

#if defined(SOC_PPA_SUPPORTED) && SOC_PPA_SUPPORTED
#define HD_USE_PPA 1
#include <driver/ppa.h>
#include <esp_cache.h>
#else
#define HD_USE_PPA 0
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

constexpr auto* TAG = "VoidskimDisplay";

// Monochrome: UI pixels brighter than this are white
constexpr uint32_t UI_THRESHOLD = 96;

// Internal RAM to leave free for the system after placing the frame buffer there
constexpr size_t INTERNAL_RESERVE_BYTES = 96 * 1024;

// The largest frame the game renders. Software 3D costs per pixel, so a big panel gets a smaller
// frame, scaled up. Chips with a PPA have the CPU (and the hardware scaler) for more.
// Budgeted by pixel count rather than width and height, so very wide (or tall) screens get a
// frame that's still tall (or wide) enough for the menus at the same rendering cost.
#if HD_USE_PPA
constexpr float MAX_FRAME_PIXELS = 640.0f * 360.0f;
#else
constexpr float MAX_FRAME_PIXELS = 400.0f * 240.0f;
#endif

namespace {

enum class PixelMode { Rgb16, Gray8, Mono };

PixelMode pixelMode = PixelMode::Rgb16;
bool swapBytes = false;
bool swapRedBlue = false;
// Software RGB16: the frame is converted and sent a band of rows at a time, from a small buffer
// in internal DMA-capable RAM. A whole-frame buffer would end up in PSRAM, and SPI panel drivers
// then need a DMA bounce buffer of the same size, which doesn't fit in internal RAM.
uint16_t* bandBuffer = nullptr;
int bandRows = 0;
constexpr size_t BAND_BYTES = 16 * 1024;
uint8_t* screenBuffer = nullptr; // Gray8/Mono: the whole screen
size_t screenStride = 0;

#if HD_USE_PPA
uint16_t* outputBuffer = nullptr;
uint16_t* hwFrameBuffers[2] = { nullptr, nullptr };
int backBufferIndex = 1;
bool usingHwFrameBuffer = false;
ppa_client_handle_t ppaClient = nullptr;
#endif

const uint8_t BAYER8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

inline uint16_t toPanel(uint16_t c) {
    if (swapRedBlue) c = (uint16_t)(((c & 0x001F) << 11) | (c & 0x07E0) | ((c & 0xF800) >> 11));
    if (swapBytes) c = (uint16_t)((c << 8) | (c >> 8));
    return c;
}

inline uint8_t luma565(uint16_t c) {
    const uint32_t r = ((c >> 11) & 31) << 3;
    const uint32_t g = ((c >> 5) & 63) << 2;
    const uint32_t b = (c & 31) << 3;
    return (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
}

} // namespace

bool DisplayOutput::init(Device* displayDevice, const ScreenInfo& info) {
    display = displayDevice;
    screen = info;
    screen.drawRotation &= 3;
    screen.touchRotation &= 3;
    const bool sideways = (screen.drawRotation & 1) != 0;
    userWidth = (int)(sideways ? screen.drawHeight : screen.drawWidth);
    userHeight = (int)(sideways ? screen.drawWidth : screen.drawHeight);

    // Frame size: the display's aspect ratio, within the pixel budget
    scale = std::fmax(1.0f, std::sqrt((float)userWidth * (float)userHeight / MAX_FRAME_PIXELS));
#if HD_USE_PPA
    scale = std::ceil(scale * 16.0f) / 16.0f; // PPA scale factors have 1/16 precision
#endif
    frameWidth = (int)((float)userWidth / scale) & ~1;
    frameHeight = (int)((float)userHeight / scale) & ~1;
    uw = std::min(userWidth, (int)((float)frameWidth * scale));
    uh = std::min(userHeight, (int)((float)frameHeight * scale));
    ux = (userWidth - uw) / 2;
    uy = (userHeight - uh) / 2;
    switch (screen.drawRotation) {
        case 1: dx = uy; dy = userWidth - ux - uw; dw = uh; dh = uw; break;
        case 2: dx = userWidth - ux - uw; dy = userHeight - uy - uh; dw = uw; dh = uh; break;
        case 3: dx = userHeight - uy - uh; dy = ux; dw = uh; dh = uw; break;
        default: dx = ux; dy = uy; dw = uw; dh = uh; break;
    }
    LOG_I(TAG, "Display %dx%d (rotation %d): frame %dx%d, scale %.2f", userWidth, userHeight, screen.drawRotation,
        frameWidth, frameHeight, (double)scale);

    // Internal RAM renders fastest, but only when it fits with room to spare: Wi-Fi, Bluetooth and
    // the drivers need internal RAM too (running out crashes them). Otherwise PSRAM.
    // 64-byte aligned, as it's also the PPA's input.
    const size_t frameBytes = (size_t)frameWidth * frameHeight * 2;
    bool frameInternal = false;
    if (memory_heap_largest_block() >= frameBytes + INTERNAL_RESERVE_BYTES && memory_heap_free() >= frameBytes + INTERNAL_RESERVE_BYTES) {
        const MemoryPolicy internalPolicy = { .required = MEMORY_CAPABILITY_INTERNAL, .desired = 0, .alignment = 64 };
        frame_ = (uint16_t*)memory_alloc_with_policy(frameBytes, &internalPolicy);
        frameInternal = frame_ != nullptr;
    }
    if (frame_ == nullptr) {
        const MemoryPolicy externalPolicy = { .required = 0, .desired = MEMORY_CAPABILITY_EXTERNAL, .alignment = 64 };
        frame_ = (uint16_t*)memory_alloc_with_policy(frameBytes, &externalPolicy);
    }
    LOG_I(TAG, "Frame buffer (%u bytes) in %s RAM", (unsigned)frameBytes, frameInternal ? "internal" : "external");
    xMap = (uint16_t*)memory_alloc(uw * sizeof(uint16_t));
    yMap = (uint16_t*)memory_alloc(uh * sizeof(uint16_t));
    if (frame_ == nullptr || xMap == nullptr || yMap == nullptr) {
        LOG_E(TAG, "Out of memory for the frame");
        return false;
    }
    memset(frame_, 0, (size_t)frameWidth * frameHeight * 2);
    for (int x = 0; x < uw; x++) xMap[x] = (uint16_t)(x * frameWidth / uw);
    for (int y = 0; y < uh; y++) yMap[y] = (uint16_t)(y * frameHeight / uh);
    return initOutput();
}

bool DisplayOutput::initOutput() {
    const size_t fbSize = (size_t)screen.drawWidth * screen.drawHeight * 2;
#if HD_USE_PPA
    if (display_get_frame_buffer_count(display) == 2) {
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
            LOG_E(TAG, "Out of memory for the output buffer");
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
#else
    pixelMode = PixelMode::Rgb16;
    switch (display_get_color_format(display)) {
        case DISPLAY_COLOR_FORMAT_MONOCHROME: pixelMode = PixelMode::Mono; break;
        case DISPLAY_COLOR_FORMAT_GRAYSCALE8: pixelMode = PixelMode::Gray8; break;
        case DISPLAY_COLOR_FORMAT_RGB565: swapBytes = false; swapRedBlue = false; break;
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED: swapBytes = true; swapRedBlue = false; break;
        case DISPLAY_COLOR_FORMAT_BGR565: swapBytes = false; swapRedBlue = true; break;
        case DISPLAY_COLOR_FORMAT_BGR565_SWAPPED: swapBytes = true; swapRedBlue = true; break;
        default:
            LOG_E(TAG, "Unsupported display colour format");
            return false;
    }
    if (pixelMode != PixelMode::Rgb16) {
        screenStride = pixelMode == PixelMode::Mono ? (screen.drawWidth + 7) / 8 : screen.drawWidth;
        screenBuffer = (uint8_t*)memory_calloc(screenStride * screen.drawHeight, 1);
        return screenBuffer != nullptr;
    }
    // Wide enough for a full screen row too, for the initial clear
    const int bandWidth = std::max(dw, (int)screen.drawWidth);
    const MemoryPolicy bandPolicy = { .required = MEMORY_CAPABILITY_INTERNAL | MEMORY_CAPABILITY_DMA, .desired = 0, .alignment = 4 };
    for (size_t bytes = BAND_BYTES; bytes >= 1024 && bandBuffer == nullptr; bytes /= 2) {
        bandRows = std::max(1, (int)(bytes / ((size_t)bandWidth * 2)));
        bandBuffer = (uint16_t*)memory_alloc_with_policy((size_t)bandWidth * bandRows * 2, &bandPolicy);
    }
    if (bandBuffer == nullptr) {
        LOG_E(TAG, "Out of internal memory for the output buffer");
        return false;
    }
    // Paint the screen black once: only the frame rectangle is drawn after this
    memset(bandBuffer, 0, (size_t)bandWidth * bandRows * 2);
    for (int y = 0; y < (int)screen.drawHeight; y += bandRows) {
        const int yEnd = std::min((int)screen.drawHeight, y + bandRows);
        display_draw_bitmap(display, 0, y, (int32_t)screen.drawWidth, yEnd, bandBuffer);
    }
    (void)fbSize;
    return true;
#endif
}

void DisplayOutput::shutdown() {
#if HD_USE_PPA
    if (ppaClient != nullptr) {
        ppa_unregister_client(ppaClient);
        ppaClient = nullptr;
    }
    memory_free(outputBuffer);
    outputBuffer = nullptr;
    hwFrameBuffers[0] = hwFrameBuffers[1] = nullptr;
    usingHwFrameBuffer = false;
#endif
    memory_free(bandBuffer);
    bandBuffer = nullptr;
    memory_free(screenBuffer);
    screenBuffer = nullptr;
    memory_free(frame_);
    frame_ = nullptr;
    memory_free(xMap);
    xMap = nullptr;
    memory_free(yMap);
    yMap = nullptr;
    display = nullptr;
}

void DisplayOutput::present(const uint8_t* uiMask) {
#if HD_USE_PPA
    esp_cache_msync(frame_, (size_t)frameWidth * frameHeight * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    uint16_t* target = usingHwFrameBuffer ? hwFrameBuffers[backBufferIndex] : outputBuffer;
    const size_t fbSize = (size_t)screen.drawWidth * screen.drawHeight * 2;
    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = frame_,
            .pic_w = (uint32_t)frameWidth,
            .pic_h = (uint32_t)frameHeight,
            .block_w = (uint32_t)frameWidth,
            .block_h = (uint32_t)frameHeight,
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
            .block_offset_x = (uint32_t)dx,
            .block_offset_y = (uint32_t)dy,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = (ppa_srm_rotation_angle_t)screen.drawRotation, // same convention as LVGL
        .scale_x = scale,
        .scale_y = scale,
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
    esp_cache_msync(target, fbSize, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    display_draw_bitmap(display, 0, 0, (int32_t)screen.drawWidth, (int32_t)screen.drawHeight, target);
    if (usingHwFrameBuffer) {
        backBufferIndex = 1 - backBufferIndex;
    }
#else
    presentSoftware(uiMask);
#endif
}

void DisplayOutput::presentSoftware(const uint8_t* uiMask) {
    // Calls write(rx, ry, pixel) for each pixel of the draw rectangle. Draw-local (rx, ry) shows
    // user-local 0: (rx, ry), 1: (uw - 1 - ry, rx), 2: (uw - 1 - rx, uh - 1 - ry), 3: (ry, uh - 1 - rx)
    auto forEachPixel = [&](int rowStart, int rowEnd, auto write) {
        const uint16_t* src = frame_;
        const int stride = frameWidth;
        for (int ry = rowStart; ry < rowEnd; ry++) {
            switch (screen.drawRotation) {
                case 1: {
                    const uint16_t* column = src + xMap[uw - 1 - ry];
                    for (int rx = 0; rx < dw; rx++) write(rx, ry, &column[yMap[rx] * stride]);
                    break;
                }
                case 2: {
                    const uint16_t* row = src + yMap[uh - 1 - ry] * stride;
                    for (int rx = 0; rx < dw; rx++) write(rx, ry, &row[xMap[uw - 1 - rx]]);
                    break;
                }
                case 3: {
                    const uint16_t* column = src + xMap[ry];
                    for (int rx = 0; rx < dw; rx++) write(rx, ry, &column[yMap[uh - 1 - rx] * stride]);
                    break;
                }
                default: {
                    const uint16_t* row = src + yMap[ry] * stride;
                    for (int rx = 0; rx < dw; rx++) write(rx, ry, &row[xMap[rx]]);
                    break;
                }
            }
        }
    };

    if (pixelMode == PixelMode::Rgb16) {
        uint16_t* out = bandBuffer;
        const int stride = dw;
        for (int bandStart = 0; bandStart < dh; bandStart += bandRows) {
            const int bandEnd = std::min(dh, bandStart + bandRows);
            forEachPixel(bandStart, bandEnd, [&](int rx, int ry, const uint16_t* p) { out[(ry - bandStart) * stride + rx] = toPanel(*p); });
            // Synchronous: the driver is done with the buffer when this returns
            display_draw_bitmap(display, dx, dy + bandStart, dx + dw, dy + bandEnd, bandBuffer);
        }
        return;
    }

    // Monochrome/greyscale: the 3D world gets an ordered dither, brightened a little (roughly a 0.7
    // gamma). UI pixels (marked in the game's mask) are plain black or white, so text stays crisp.
    forEachPixel(0, dh, [&](int rx, int ry, const uint16_t* p) {
        const int x = dx + rx;
        const int y = dy + ry;
        const uint32_t l = luma565(*p);
        bool white;
        if (uiMask != nullptr && uiMask[p - frame_]) {
            white = l > UI_THRESHOLD;
        } else {
            const uint32_t bright = (l + (uint32_t)std::sqrt((float)(l * 255u))) / 2;
            white = bright > (uint32_t)(BAYER8[y & 7][x & 7] * 4 + 2);
        }
        uint8_t* row = screenBuffer + (size_t)y * screenStride;
        if (pixelMode == PixelMode::Mono) {
            const uint8_t mask = (uint8_t)(0x80 >> (x & 7));
            row[x >> 3] = white ? (row[x >> 3] | mask) : (row[x >> 3] & (uint8_t)~mask);
        } else {
            row[x] = white ? 0xFF : 0x00;
        }
    });
    display_draw_bitmap(display, 0, 0, (int32_t)screen.drawWidth, (int32_t)screen.drawHeight, screenBuffer);
}

bool DisplayOutput::mapTouch(int touchX, int touchY, int& outX, int& outY) const {
    // Raw touch -> user view (inverse of the touch rotation)
    const bool sideways = (screen.touchRotation & 1) != 0;
    const int tw = (int)(sideways ? screen.touchHeight : screen.touchWidth);
    const int th = (int)(sideways ? screen.touchWidth : screen.touchHeight);
    int x, y;
    switch (screen.touchRotation) {
        case 1: x = tw - 1 - touchY; y = touchX; break;
        case 2: x = tw - 1 - touchX; y = th - 1 - touchY; break;
        case 3: x = touchY; y = th - 1 - touchX; break;
        default: x = touchX; y = touchY; break;
    }
    if (tw > 0 && th > 0 && (tw != userWidth || th != userHeight)) {
        x = x * userWidth / tw;
        y = y * userHeight / th;
    }
    // User view -> frame
    x -= ux;
    y -= uy;
    if (x < 0 || y < 0 || x >= uw || y >= uh) {
        return false;
    }
    outX = x * frameWidth / uw;
    outY = y * frameHeight / uh;
    return true;
}

bool DisplayOutput::isMonochrome() const {
    return pixelMode != PixelMode::Rgb16;
}
