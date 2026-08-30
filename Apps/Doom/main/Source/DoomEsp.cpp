#include "DoomEsp.h"

#include "DoomAudio.h"

#include "doomgeneric.h"
#include "doomkeys.h"

// From m_config.c. Declared here rather than including m_config.h, which has no
// extern "C" guard of its own.
extern "C" void M_SaveDefaults(void);

// From z_zone.c. Frees the zone allocation made by Z_Init(), without which every run leaks its
// zone size (default 6 MiB) permanently.
extern "C" void Z_Shutdown(void);

// From w_wad.c. Closes the WAD file(s) opened by W_AddFile(), without which every run leaks a
// FILE*/fd, exhausting the SD/FATFS descriptor pool after a handful of runs. Must run before
// Z_Shutdown(): it frees the lump hash table via Z_Free(), which needs the zone still alive.
extern "C" void W_Shutdown(void);

// From s_sound.c. Tears down the sound and music subsystems (I_ShutdownSound/I_ShutdownMusic),
// which for the OPL music module frees the currently registered song's tracks and destroys the
// emulated chip instance. Normally reached only via I_Quit()'s I_AtExit list (menu "Quit Game"),
// never on touch-to-exit. Safe to call directly here since the game loop has already stopped.
extern "C" void S_Shutdown(void);

#include <cerrno>

#include <app/paths.h>

#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/keyboard.h>

#include <driver/ppa.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

constexpr auto* TAG = "DoomEsp";

/** Must match manifest.properties' app.id */
static constexpr const char* APP_ID = "tactility.doom";

// m_misc.c / m_config.c reference this as the save/config directory on ESP32
// (no real definition is built here, since i_*sound.c / the original main/ are excluded).
//
// Filled in at startup from the app's own user data directory, which always
// exists and is writable - the old hard-coded "/sdcard/doom" was the same
// folder the WADs live in, so on a device with no such folder (i.e. anyone
// running the bundled shareware WAD) every config and savegame write silently
// failed.
//
// 128 bytes because the user data path is long: everything Tactility owns now
// lives under a "tactility" folder, making this
// "/sdcard/tactility/user/app/tactility.doom/" (46 chars, ~59 once a
// savegame filename is appended), or "/data/..." on a device with no SD card.
// The previous char[32] could not hold it.
//
// Note this is NOT the WAD directory - that is a separate local (wadDir) at
// <volume>/doom. The empty initialiser is deliberate: it is always replaced by
// setupSaveDir() before the engine starts, and leaving it empty means a failure
// to resolve the user data path degrades to M_SetConfigDir's own "no config
// dir" handling rather than silently writing into the WAD folder, which is what
// the old "/sdcard/doom" default did.
extern "C" char doomEsp_savedir[128] = "";

// Doom's native render resolution (set via DOOMGENERIC_RESX/RESY)
constexpr int DOOM_W = DOOMGENERIC_RESX;
constexpr int DOOM_H = DOOMGENERIC_RESY;

// Written from doomEsp_RequestStop() (LVGL/touch task, or the doom task via
// I_Quit) and read by the game loop on the other core. volatile would prevent
// the compiler caching it but gives no ordering guarantees across cores.
static std::atomic<bool> running {false};

static uint16_t* doomRb565 = nullptr;
static uint16_t* scaledFrameBuffer = nullptr;
static uint16_t* hwFrameBuffers[2] = { nullptr, nullptr }; // panel-owned double buffer, when supported
static int backBufferIndex = 1; // PPA writes here; cur_fb_index starts at 0, so back buffer starts at 1
static bool usingHwFrameBuffer = false;
static ppa_client_handle_t ppaClient = nullptr;
static Device* g_display = nullptr;
static uint32_t scaledW = 0;
static uint32_t scaledH = 0;
// Keyboard input via TactilityKernel's keyboard device API
struct DoomKeyEvent {
    int pressed;
    unsigned char key;
};

static constexpr int MAX_KEYBOARDS = 4;
static Device* kbDevices[MAX_KEYBOARDS] = {};
static int kbCount = 0;
static QueueHandle_t doomKeyQueue = nullptr;

static void queueDoomKey(unsigned char key, bool pressed) {
    if (key == 0 || doomKeyQueue == nullptr) {
        return;
    }
    DoomKeyEvent ev = { pressed ? 1 : 0, key };
    xQueueSend(doomKeyQueue, &ev, 0);
}

static void mapAndQueue(uint32_t rawKey, bool pressed) {
    // ASCII printable (0x20-0x7E) passes through directly
    if (rawKey >= 0x20 && rawKey <= 0x7E) {
        unsigned char key = static_cast<unsigned char>(rawKey);
        queueDoomKey(key, pressed);

        // Space / Backspace also trigger USE
        if (rawKey == ' ') {
            queueDoomKey(KEY_USE, pressed);
        }
        // 'f' also triggers Fire
        if (rawKey == 'f') {
            queueDoomKey(KEY_FIRE, pressed);
        }
        // WASD → arrow keys
        if (rawKey == 'w') {
            queueDoomKey(KEY_UPARROW, pressed);
        }
        if (rawKey == 'a') {
            queueDoomKey(KEY_LEFTARROW, pressed);
        }
        if (rawKey == 's') {
            queueDoomKey(KEY_DOWNARROW, pressed);
        }
        if (rawKey == 'd') {
            queueDoomKey(KEY_RIGHTARROW, pressed);
        }
        return;
    }

    // Non-ASCII: map common special key codes to doomkeys.h
    switch (rawKey) {
        case CODEPOINT_ARROW_LEFT: queueDoomKey(KEY_LEFTARROW, pressed); break;
        case CODEPOINT_ARROW_UP: queueDoomKey(KEY_UPARROW, pressed); break;
        case CODEPOINT_ARROW_RIGHT: queueDoomKey(KEY_RIGHTARROW, pressed); break;
        case CODEPOINT_ARROW_DOWN: queueDoomKey(KEY_DOWNARROW, pressed); break;
        case CODEPOINT_ESCAPE: queueDoomKey(KEY_ESCAPE, pressed); break;
        case CODEPOINT_ENTER: queueDoomKey(KEY_ENTER, pressed); break;
        case CODEPOINT_TAB: queueDoomKey(KEY_TAB, pressed); break;
        case CODEPOINT_DELETE: queueDoomKey(KEY_BACKSPACE, pressed);
                   queueDoomKey(KEY_USE, pressed); break;
        default:
            break;
    }
}

// --- Optional on-screen FPS counter (set to 0 to disable) ---
#define DOOM_SHOW_FPS 1

#if DOOM_SHOW_FPS
// Minimal 5x7 font, digits 0-9.
static const uint8_t fps_font[10][7] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}, // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
};

// Draw the FPS value onto the RGB565 framebuffer, after the PPA blit.
//
// The PPA rotates the game image by 90 degrees into the panel's native
// (portrait) buffer, but this overlay is drawn afterwards and so has to apply
// the same rotation itself - otherwise the digits appear on their side.
//
// Everything below is therefore authored in landscape coordinates, the same
// orientation the player sees, and mapped through plot() on the way out.
// Landscape is scaledH wide by scaledW tall, i.e. the panel dimensions
// transposed.
static inline void fps_plot(uint16_t* fb, int lx, int ly, uint16_t colour) {
    // Rotate landscape (lx, ly) into the portrait panel buffer. This is the
    // inverse of the PPA's 90-degree rotation, so the overlay ends up in the
    // same orientation as the game image beneath it.
    int py = (int) scaledH - 1 - lx;
    int px = ly;
    if (px >= 0 && px < (int) scaledW && py >= 0 && py < (int) scaledH) {
        fb[py * scaledW + px] = colour;
    }
}

static void draw_fps_overlay(uint16_t* fb, int fps) {
    const int scale = 3;
    char buf[6];
    int n = snprintf(buf, sizeof(buf), "%d", fps > 9999 ? 9999 : fps);
    int cell = (5 + 1) * scale; // 5px glyph + 1px gap
    int bw = n * cell + 6, bh = 7 * scale + 6;

    // Landscape extents, and a top-right origin within them.
    const int landscapeW = (int) scaledH;
    const int ox = landscapeW - bw - 10 + 3;
    const int oy = 10;

    for (int y = oy - 3; y < oy - 3 + bh; y++) // dark backing box for legibility
        for (int x = ox - 3; x < ox - 3 + bw; x++)
            fps_plot(fb, x, y, 0x0000);

    for (int i = 0; i < n; i++) { // yellow digits
        const uint8_t* g = fps_font[buf[i] - '0'];
        for (int ry = 0; ry < 7; ry++)
            for (int rx = 0; rx < 5; rx++)
                if (g[ry] & (1 << (4 - rx)))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            fps_plot(fb, ox + i * cell + rx * scale + sx,
                                     oy + ry * scale + sy, 0xFFE0);
    }
}
#endif

extern "C" void doom_draw_frame(const uint32_t* buffer) {
    memcpy(doomRb565, buffer, DOOM_W * DOOM_H * 2);
    esp_cache_msync(doomRb565, DOOM_W * DOOM_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    uint16_t* outputBuffer = usingHwFrameBuffer ? hwFrameBuffers[backBufferIndex] : scaledFrameBuffer;

    ppa_srm_oper_config_t srmConfig = {
        .in = {
            .buffer = doomRb565,
            .pic_w = DOOM_W,
            .pic_h = DOOM_H,
            .block_w = DOOM_W,
            .block_h = DOOM_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = outputBuffer,
            .buffer_size = (uint32_t)(scaledW * scaledH * 2),
            .pic_w = scaledW,
            .pic_h = scaledH,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = (float)scaledH / (float)DOOM_W,
        .scale_y = (float)scaledW / (float)DOOM_H,
    };

    ppa_do_scale_rotate_mirror(ppaClient, &srmConfig);

#if DOOM_SHOW_FPS
    static uint32_t fps_frames = 0;
    static uint64_t fps_last_us = 0;
    static int fps_value = 0;
    fps_frames++;
    uint64_t now_us = esp_timer_get_time();
    if (fps_last_us == 0)
        fps_last_us = now_us;
    if (now_us - fps_last_us >= 500000) { // refresh twice a second
        fps_value = (int)(fps_frames * 1000000ULL / (now_us - fps_last_us));
        fps_frames = 0;
        fps_last_us = now_us;
    }
    draw_fps_overlay(outputBuffer, fps_value);
#endif

    esp_cache_msync(outputBuffer, scaledW * scaledH * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    display_draw_bitmap(g_display, 0, 0, scaledW, scaledH, outputBuffer);

    if (usingHwFrameBuffer) {
        backBufferIndex = 1 - backBufferIndex;
    }
}

extern "C" int doom_get_key(int* pressed, unsigned char* key) {
    DoomKeyEvent ev;
    if (doomKeyQueue != nullptr && xQueueReceive(doomKeyQueue, &ev, 0) == pdTRUE) {
        *pressed = ev.pressed;
        *key = ev.key;
        return 1;
    }
    return 0;
}

extern "C" void doomEsp_RequestStop() {
    running.store(false);
}

// Returns the volume root, e.g. "/sdcard" or "/data", by taking the first
// component of the app's user data path (".../tactility/user/app/<id>"). In
// practice this is always "/sdcard" on P4 hardware, which needs an SD card
// anyway, but "/data" is handled for devices without one.
static std::string getSdRoot() {
    char path[128] = {0};
    app_paths_get_user_data_directory(APP_ID, path, sizeof(path));
    std::string s(path);
    size_t pos = s.find('/', 1);
    return (pos != std::string::npos) ? s.substr(0, pos) : "/sdcard";
}

// Creates a directory and every missing parent along the way.
static void makeDirs(const std::string& path) {
    std::string partial;
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            partial = path.substr(0, i);
            mkdir(partial.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

// Points DOOM's config/savegame/temp directory at the app's own user data
// folder and makes sure it exists.
//
// This is deliberately NOT the WAD folder. The two were the same before, which
// meant that on a device without an sdRoot/doom directory - i.e. anyone playing
// the bundled shareware WAD - saving a game or the sound settings wrote into a
// folder that did not exist, and silently did nothing.
static void setupSaveDir() {
    char dir[sizeof(doomEsp_savedir)] = {0};

    if (app_paths_get_user_data_directory(APP_ID, dir, sizeof(dir)) != ERROR_NONE || dir[0] == '\0') {
        // Leaves doomEsp_savedir empty, which m_config.c treats as "no config
        // directory" - config and savegames are then not written at all,
        // rather than being written somewhere wrong.
        ESP_LOGW(TAG, "Could not resolve user data path - config and savegames will not persist");
        return;
    }

    std::string path(dir);

    // This string MUST end in a separator. m_config.c builds the config file
    // paths with M_StringJoin(configdir, "default.cfg", NULL) - no separator of
    // its own - so without the trailing slash the config would be written to
    // ".../tactility.doomdefault.cfg". (M_TempFile in m_misc.c does insert
    // a separator, so it sees a harmless doubled slash instead.)
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    // Create the directory using the un-suffixed form, then re-add the slash.
    makeDirs(path);

    path += '/';

    snprintf(doomEsp_savedir, sizeof(doomEsp_savedir), "%s", path.c_str());
    ESP_LOGI(TAG, "Save/config directory: %s", doomEsp_savedir);
}

// Creates sdRoot/doom/ if absent, so there is an obvious place to drop full
// WADs even on a device that has only ever run the bundled shareware one.
// Empty file 'ADD_WADS_HERE' created to let the user know... to add wads here!
static void ensureWadDir(const std::string& wadDir) {
    makeDirs(wadDir);
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "%s/%s", wadDir.c_str(), "ADD_WADS_HERE");
    FILE* f = fopen(filepath, "w");
    if (f != nullptr) {
        fclose(f);
    } else {
        ESP_LOGW(TAG, "Could not create %s", filepath);
    }
}

// Releases everything doomEsp_Run() acquired and resets the shared state to
// safe inactive values.
//
// Both exits from doomEsp_Run() come through here, so a failed startup leaves
// exactly the same state as a clean shutdown. Clearing the pointers matters
// beyond tidiness: doom_draw_frame() dereferences doomRb565, ppaClient and
// g_display without checking, so leaving stale values behind would turn a
// second run - or a late frame callback - into a use-after-free.
static void releaseResources() {
    for (int i = 0; i < kbCount; i++) {
        if (kbDevices[i] != nullptr) {
            device_put(kbDevices[i]);
            kbDevices[i] = nullptr;
        }
    }
    kbCount = 0;
    if (doomKeyQueue != nullptr) {
        vQueueDelete(doomKeyQueue);
        doomKeyQueue = nullptr;
    }

    if (ppaClient != nullptr) {
        ppa_unregister_client(ppaClient);
        ppaClient = nullptr;
    }

    if (doomRb565 != nullptr) {
        heap_caps_free(doomRb565);
        doomRb565 = nullptr;
    }

    if (scaledFrameBuffer != nullptr) {
        heap_caps_free(scaledFrameBuffer);
        scaledFrameBuffer = nullptr;
    }

    // Panel-owned, not ours to free - just stop referencing them.
    hwFrameBuffers[0] = nullptr;
    hwFrameBuffers[1] = nullptr;
    usingHwFrameBuffer = false;
    backBufferIndex = 1;

    g_display = nullptr;
}

// Searches sdRoot/doom/ for a known IWAD/PWAD.
static bool findWad(const std::string& dir, const char* name, std::string& outPath) {
    std::string candidate = dir + "/" + name;

    bool found = false;
    FILE* f = fopen(candidate.c_str(), "rb");
    if (f != nullptr) {
        fclose(f);
        found = true;
    } else {
        ESP_LOGW(TAG, "fopen failed: %s (errno %d)", candidate.c_str(), errno);
    }

    if (found) {
        outPath = candidate;
    }
    return found;
}

void doomEsp_Run(Device* display) {
    g_display = display;

    scaledW = display_get_resolution_x(display);
    scaledH = display_get_resolution_y(display);

    // 1. Allocate the PPA input buffer (internal RAM preferred for DMA alignment, PSRAM fallback)
    doomRb565 = (uint16_t*)heap_caps_aligned_alloc(64, DOOM_W * DOOM_H * 2, MALLOC_CAP_INTERNAL);
    if (doomRb565 == nullptr) {
        ESP_LOGW(TAG, "Internal RAM alloc failed, using PSRAM for doomRb565");
        doomRb565 = (uint16_t*)heap_caps_aligned_alloc(64, DOOM_W * DOOM_H * 2, MALLOC_CAP_SPIRAM);
    }
    assert(doomRb565 != nullptr);

    // 2. Get the PPA output buffer(s) (scaled/rotated, matches the display panel size).
    // Prefer writing PPA's output directly into one of the panel's double-buffered
    // hardware frame buffers (num_fbs=2): PPA writes into the back buffer while the
    // DSI controller scans out the front buffer, then drawBitmap() flips them at the
    // next frame boundary - no tearing. Fall back to a separate PSRAM buffer +
    // drawBitmap if double buffering is unsupported.
    if (display_get_frame_buffer_count(display) == 2) {
        ESP_LOGI(TAG, "Using direct hardware frame buffers for PPA output");
        void* fbPtr0 = nullptr;
        void* fbPtr1 = nullptr;
        display_get_frame_buffer(display, 0, &fbPtr0);
        display_get_frame_buffer(display, 1, &fbPtr1);
        hwFrameBuffers[0] = (uint16_t*)fbPtr0;
        hwFrameBuffers[1] = (uint16_t*)fbPtr1;
        backBufferIndex = 1;
        usingHwFrameBuffer = true;

        // Clear both buffers to black once up front, so any row/column PPA never
        // writes to (a tiny edge gap, independent of scaling) shows as black
        // instead of stale/garbage content.
        memset(hwFrameBuffers[0], 0, (size_t)scaledW * scaledH * 2);
        memset(hwFrameBuffers[1], 0, (size_t)scaledW * scaledH * 2);
        esp_cache_msync(hwFrameBuffers[0], (size_t)scaledW * scaledH * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_cache_msync(hwFrameBuffers[1], (size_t)scaledW * scaledH * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    } else {
        scaledFrameBuffer = (uint16_t*)heap_caps_aligned_alloc(64, (size_t)scaledW * scaledH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        assert(scaledFrameBuffer != nullptr);
        usingHwFrameBuffer = false;
    }

    // 3. Register the PPA client
    ppa_client_config_t ppaConfig = { .oper_type = PPA_OPERATION_SRM };
    ESP_ERROR_CHECK(ppa_register_client(&ppaConfig, &ppaClient));

    // 3b. Discover all keyboard devices (best-effort - Doom runs fine without them)
    doomKeyQueue = xQueueCreate(32, sizeof(DoomKeyEvent));
    kbCount = 0;
    device_for_each_of_type(&KEYBOARD_TYPE, nullptr, [](struct Device* device, void*) -> bool {
        if (kbCount < MAX_KEYBOARDS) {
            device_get(device);
            kbDevices[kbCount++] = device;
        }
        return kbCount < MAX_KEYBOARDS;
    });
    if (kbCount == 0) {
        ESP_LOGW(TAG, "No keyboard devices found");
    } else {
        ESP_LOGI(TAG, "Found %d keyboard device(s)", kbCount);
    }

    // 4. Locate a WAD on the SD card
    std::string sdRoot = getSdRoot();
    std::string wadDir = sdRoot + "/doom";
    LOG_I(TAG, "WAD DIR: %s", wadDir.c_str());

    // Config, savegames and temp files live in the app's own data folder, not
    // alongside the WADs - see setupSaveDir().
    setupSaveDir();

    // Make sure the WAD folder exists even when we end up falling back to the
    // bundled shareware WAD, so it is discoverable as the place to put full ones.
    ensureWadDir(wadDir);

    std::string iwadPath;
    std::string pwadPath;
    bool haveIwad = findWad(wadDir, "doom2.wad", iwadPath)
        || findWad(wadDir, "doom.wad", iwadPath)
        || findWad(wadDir, "doom1.wad", iwadPath);
    bool havePwad = findWad(wadDir, "chiquito.wad", pwadPath);

    // No full/retail IWAD on the SD card - fall back to the bundled shareware WAD (assets/doom1.wad).
    if (!haveIwad) {
        char assetPath[192] = {0};
        if (app_paths_get_assets_path(APP_ID, "doom1.wad", assetPath, sizeof(assetPath)) == ERROR_NONE) {
            iwadPath = assetPath;
            haveIwad = true;
            ESP_LOGI(TAG, "No IWAD on SD card - using bundled shareware WAD");
        }
    }

    if (!haveIwad) {
        ESP_LOGE(TAG, "No IWAD found in %s (expected doom2.wad, doom.wad or doom1.wad) and no bundled fallback", wadDir.c_str());
        releaseResources();
        return;
    }

    ESP_LOGI(TAG, "Starting DOOM. IWAD: %s, PWAD: %s", iwadPath.c_str(), havePwad ? pwadPath.c_str() : "None");

    // 5. Start the audio mixer (best-effort - Doom runs fine without it)
    doomAudio_Init();

    // 6. Boot doomgeneric
    //
    // doomgeneric_Create() stores argv in the global myargv and the engine
    // parses it lazily afterwards (M_CheckParmWithArgs for -iwad, -config and
    // friends), so the array has to outlive this function - a block-scoped
    // local would dangle for the whole session. iwadPath/pwadPath are function
    // locals that stay alive for as long as the game loop runs below, which is
    // the entire lifetime of the pointers held here.
    running.store(true);
    static char* argv[7];
    int argc;
    if (havePwad) {
        argv[0] = (char*)"doom";
        argv[1] = (char*)"-iwad";
        argv[2] = (char*)iwadPath.c_str();
        argv[3] = (char*)"-file";
        argv[4] = (char*)pwadPath.c_str();
        argv[5] = (char*)"-gfxmode";
        argv[6] = (char*)"rgb565";
        argc = 7;
    } else {
        argv[0] = (char*)"doom";
        argv[1] = (char*)"-iwad";
        argv[2] = (char*)iwadPath.c_str();
        argv[3] = (char*)"-gfxmode";
        argv[4] = (char*)"rgb565";
        argc = 5;
    }
    doomgeneric_Create(argc, argv);

    while (running.load()) {
        for (int i = 0; i < kbCount; i++) {
            if (kbDevices[i] != nullptr && device_is_ready(kbDevices[i])) {
                KeyboardKeyData data;
                while (keyboard_read_key(kbDevices[i], &data) == ERROR_NONE) {
                    mapAndQueue(data.key, data.pressed);
                    if (!data.continue_reading) {
                        break;
                    }
                }
            }
        }
        doomgeneric_Tick();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Persist the config (sound volumes, controls, etc). This is the only place
    // it is saved: d_main.c no longer registers M_SaveDefaults as an I_AtExit
    // handler, because those run from inside I_Quit() while the game loop is
    // still live, and only on the menu's "Quit Game" - touch-to-exit never
    // reached them at all.
    // This runs on the doom task after the render loop has stopped, never on the LVGL task.
    if (doomEsp_savedir[0] != '\0') {
        M_SaveDefaults();
    }

    // Must run before doomAudio_Shutdown()/W_Shutdown()/Z_Shutdown(): it
    // releases a WAD lump (W_ReleaseLumpName) and frees music state that
    // points into zone memory, all of which need the WAD/zone still alive.
    S_Shutdown();

    doomAudio_Shutdown();
    doomgeneric_Shutdown();
    W_Shutdown();
    Z_Shutdown();
    releaseResources();
}
