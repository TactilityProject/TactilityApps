#include "DoomMain.h"

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
// emulated chip instance. Also on I_Quit()'s I_AtExit list (menu "Quit Game"); called here as
// well so shutdown doesn't depend on that path. Safe since the game loop has already stopped.
extern "C" void S_Shutdown(void);

#include <cerrno>

#include <app/paths.h>

#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/keyboard.h>
#include <tactility/drivers/touch.h>
#include <tactility/drivers/trackball.h>
#include <tactility/log.h>
#include <tactility/memory.h>
#include <tactility/time.h>

#include "DoomRenderer.h"
#include "TouchOverlay.h"


#include <tactility/freertos/freertos.h>
#include <tactility/freertos/queue.h>
#include <tactility/freertos/task.h>

#include <sys/stat.h>

#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <string>

constexpr auto* TAG = "DoomMain";

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
extern "C" { char doomMain_savedir[128] = ""; }

// Doom's native render resolution (set via DOOMGENERIC_RESX/RESY)
constexpr int DOOM_W = DOOMGENERIC_RESX;
constexpr int DOOM_H = DOOMGENERIC_RESY;

// Written from doomMain_RequestStop() (the doom task, via I_Quit) and read by
// the game loop. volatile would prevent
// the compiler caching it but gives no ordering guarantees across cores.
static std::atomic<bool> running {false};

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

// Raw keys currently held down. Some keyboard drivers (e.g. the simulator's SDL keyboard)
// report a held key as "pressed" again on every read, LVGL-style. Doom expects one key-down
// per press, so only state changes are forwarded.
static constexpr int MAX_HELD_KEYS = 16;
static uint32_t heldKeys[MAX_HELD_KEYS] = {};
static int heldKeyCount = 0;

/** @return true when the event changes the key's state and should be forwarded */
static bool updateHeldKey(uint32_t rawKey, bool pressed) {
    for (int i = 0; i < heldKeyCount; i++) {
        if (heldKeys[i] == rawKey) {
            if (pressed) {
                return false; // repeat of a held key
            }
            heldKeys[i] = heldKeys[--heldKeyCount];
            return true;
        }
    }
    if (!pressed) {
        return false; // release of a key that isn't down
    }
    if (heldKeyCount < MAX_HELD_KEYS) {
        heldKeys[heldKeyCount++] = rawKey;
    }
    return true;
}

static void mapAndQueue(uint32_t rawKey, bool pressed) {
    if (rawKey == 0 || !updateHeldKey(rawKey, pressed)) {
        return;
    }

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

// --- Touch controls ---
//
// Available whenever there is a touch screen. Whether a keyboard is attached can't be known up
// front (devices like the Tab5 always register keyboard drivers, attached or not), so the
// controls start visible, hide as soon as a keyboard key is pressed, and come back when the
// screen is touched again.
//
// They're drawn over the game, or on a portrait screen with room to spare, in a control panel
// below it (see DoomRenderer.h). Either way they're drawn and hit-tested in DOOM's 320-wide
// coordinates (TouchOverlay.h); the renderer maps touches back through its scaling and rotation.

static Device* touchDevice = nullptr;
static bool touchControls = false; // a touch screen is available
static bool touchOverlayVisible = false;
static bool touchWaitForRelease = false; // the touch that revealed the controls isn't a button press
static uint32_t touchPressedMask = 0;
static unsigned char touchButtonKeys[TOUCH_OVERLAY_BUTTON_COUNT] = {};

// Copy of the game frame with the overlay (and FPS counter) drawn on top. The engine's own
// buffer is never drawn into, since DOOM only redraws parts of it (e.g. the view border) when
// they change.
static uint8_t* overlayFrame = nullptr;
// The control panel canvas (320 x doomRenderer_PanelHeight()), when the controls have a panel
static uint8_t* panelCanvas = nullptr;

static void pollTouch() {
    if (!touchControls) {
        return;
    }
    constexpr uint8_t MAX_POINTS = 5;
    uint16_t xs[MAX_POINTS] = {};
    uint16_t ys[MAX_POINTS] = {};
    uint16_t strengths[MAX_POINTS] = {};
    uint8_t count = 0;
    uint32_t mask = 0;
    touch_read_data(touchDevice, 0);
    const bool touched = touch_get_touched_points(touchDevice, xs, ys, strengths, &count, MAX_POINTS) && count > 0;
    if (!touchOverlayVisible) {
        if (touched) {
            touchOverlayVisible = true;
            touchWaitForRelease = true;
        }
        return;
    }
    if (touchWaitForRelease) {
        if (touched) {
            return;
        }
        touchWaitForRelease = false;
    }
    if (touched) {
        // The controls live on the panel when there is one, otherwise over the game
        const DoomTouchTarget controls = panelCanvas != nullptr ? DoomTouchTarget::Panel : DoomTouchTarget::Game;
        for (uint8_t i = 0; i < count && i < MAX_POINTS; i++) {
            int x, y;
            if (doomRenderer_MapTouch(xs[i], ys[i], &x, &y) == controls) {
                mask |= touchOverlay_HitTest(x, y);
            }
        }
    }

    const uint32_t changed = mask ^ touchPressedMask;
    for (int i = 0; i < TOUCH_OVERLAY_BUTTON_COUNT; i++) {
        if (!(changed & (1u << i))) {
            continue;
        }
        if (mask & (1u << i)) {
            // The key is picked at press time (it depends on menu state and weapons) and
            // remembered, so the release always matches it.
            touchButtonKeys[i] = touchOverlay_KeyForButton(i);
            queueDoomKey(touchButtonKeys[i], true);
        } else {
            queueDoomKey(touchButtonKeys[i], false);
            touchButtonKeys[i] = 0;
        }
    }
    touchPressedMask = mask;
}

// Hides the overlay, releasing any keys its buttons were holding
static void hideTouchOverlay() {
    for (int i = 0; i < TOUCH_OVERLAY_BUTTON_COUNT; i++) {
        if (touchPressedMask & (1u << i)) {
            queueDoomKey(touchButtonKeys[i], false);
            touchButtonKeys[i] = 0;
        }
    }
    touchPressedMask = 0;
    touchOverlayVisible = false;
    touchWaitForRelease = false;
}

static bool isTrackballChild(Device* device) {
    Device* parent = device_get_parent(device);
    return parent != nullptr && device_get_type(parent) == &TRACKBALL_TYPE;
}

// --- Trackball input ---
//
// Read straight from the trackball device (not its keyboard/mouse child), so it works
// whichever mode the user picked in settings.
// - In menus every roll step is a discrete arrow key press, and the click is Enter.
// - In game rolling holds the arrow key for that direction for a short while, extended by
//   each further step, so continuous rolling walks/turns smoothly. The click is Fire.

// From doomstat.h / m_menu.c. boolean is 4 bytes wide on the C side (see doomtype.h).
extern "C" uint32_t menuactive;

static constexpr int MAX_TRACKBALLS = 2;
static Device* tbDevices[MAX_TRACKBALLS] = {};
static int tbCount = 0;

// How long one roll step keeps a movement key held in game.
static constexpr uint32_t TRACKBALL_HOLD_MS = 150;

struct TrackballAxisHold {
    unsigned char key; // currently held doom key, or 0
    uint32_t releaseAtMs;
};
static TrackballAxisHold tbHoldX = {};
static TrackballAxisHold tbHoldY = {};
static bool tbButtonDown = false;
static unsigned char tbButtonKey = 0;

static void releaseAxisHold(TrackballAxisHold& hold) {
    if (hold.key != 0) {
        queueDoomKey(hold.key, false);
        hold.key = 0;
    }
}

static void stepAxis(TrackballAxisHold& hold, int32_t delta, unsigned char negativeKey, unsigned char positiveKey, uint32_t nowMs) {
    if (delta != 0) {
        const unsigned char key = delta > 0 ? positiveKey : negativeKey;
        if (menuactive) {
            releaseAxisHold(hold);
            int32_t steps = delta > 0 ? delta : -delta;
            if (steps > 4) {
                steps = 4; // a fast flick shouldn't race through the whole menu
            }
            for (int32_t i = 0; i < steps; i++) {
                queueDoomKey(key, true);
                queueDoomKey(key, false);
            }
            return;
        }
        if (hold.key != key) {
            releaseAxisHold(hold);
            queueDoomKey(key, true);
            hold.key = key;
        }
        hold.releaseAtMs = nowMs + TRACKBALL_HOLD_MS;
    } else if (hold.key != 0 && (int32_t)(nowMs - hold.releaseAtMs) >= 0) {
        releaseAxisHold(hold);
    }
}

static void pollTrackballs() {
    if (tbCount == 0) {
        return;
    }
    int32_t dx = 0;
    int32_t dy = 0;
    bool button = false;
    for (int i = 0; i < tbCount; i++) {
        if (!device_is_ready(tbDevices[i])) {
            continue;
        }
        int32_t x = 0, y = 0;
        if (trackball_read_delta(tbDevices[i], &x, &y) == ERROR_NONE) {
            dx += x;
            dy += y;
        }
        bool pressed = false;
        if (trackball_get_button_pressed(tbDevices[i], &pressed) == ERROR_NONE && pressed) {
            button = true;
        }
    }

    const uint32_t nowMs = (uint32_t)get_millis();
    stepAxis(tbHoldX, dx, KEY_LEFTARROW, KEY_RIGHTARROW, nowMs);
    stepAxis(tbHoldY, dy, KEY_UPARROW, KEY_DOWNARROW, nowMs);

    if (button != tbButtonDown) {
        tbButtonDown = button;
        if (button) {
            tbButtonKey = menuactive ? KEY_ENTER : KEY_FIRE;
            queueDoomKey(tbButtonKey, true);
        } else if (tbButtonKey != 0) {
            queueDoomKey(tbButtonKey, false);
            tbButtonKey = 0;
        }
    }
}

// --- Rendering ---

// On-screen FPS counter (set to 0 to disable)
#define DOOM_SHOW_FPS 1

#if DOOM_SHOW_FPS
static int updateFps() {
    static uint32_t frames = 0;
    static uint64_t lastUs = 0;
    static int value = 0;
    frames++;
    const uint64_t nowUs = get_micros_since_boot();
    if (lastUs == 0) {
        lastUs = nowUs;
    }
    if (nowUs - lastUs >= 500000) { // refresh twice a second
        value = (int)(frames * 1000000ULL / (nowUs - lastUs));
        frames = 0;
        lastUs = nowUs;
    }
    return value;
}
#endif

// From i_video.c: the engine's 8-bit palettised frame
extern "C" uint8_t* I_VideoBuffer;

// The game frame to display: the engine's own, or a copy with the overlay and FPS counter on top
static const uint8_t* composeGameFrame() {
    const bool overlay = touchControls && touchOverlayVisible && panelCanvas == nullptr;
    if (!overlay && !DOOM_SHOW_FPS) {
        return I_VideoBuffer;
    }
    memcpy(overlayFrame, I_VideoBuffer, DOOM_W * DOOM_H);
    if (overlay) {
        touchOverlay_Draw(overlayFrame, touchPressedMask);
    }
#if DOOM_SHOW_FPS
    char text[12];
    snprintf(text, sizeof(text), "%d FPS", updateFps());
    // Top right, below the overlay's MAP button when that's showing
    touchOverlay_DrawText(overlayFrame, DOOM_W - 2, overlay ? 22 : 2, text);
#endif
    return overlayFrame;
}

static const uint8_t* composePanel() {
    if (panelCanvas == nullptr) {
        return nullptr;
    }
    if (touchOverlayVisible) {
        touchOverlay_Draw(panelCanvas, touchPressedMask);
    } else {
        memset(panelCanvas, 0, (size_t)DOOM_W * doomRenderer_PanelHeight());
    }
    return panelCanvas;
}

extern "C" void doom_draw_frame(const uint32_t*) {
    doomRenderer_Draw(composeGameFrame(), composePanel());
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

extern "C" void doomMain_RequestStop() {
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
    char dir[sizeof(doomMain_savedir)] = {0};

    if (app_paths_get_user_data_directory(APP_ID, dir, sizeof(dir)) != ERROR_NONE || dir[0] == '\0') {
        // Leaves doomMain_savedir empty, which m_config.c treats as "no config
        // directory" - config and savegames are then not written at all,
        // rather than being written somewhere wrong.
        LOG_W(TAG, "Could not resolve user data path - config and savegames will not persist");
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

    snprintf(doomMain_savedir, sizeof(doomMain_savedir), "%s", path.c_str());
    LOG_I(TAG, "Save/config directory: %s", doomMain_savedir);
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
        LOG_W(TAG, "Could not create %s", filepath);
    }
}

// Releases everything doomMain_Run() acquired and resets the shared state to
// safe inactive values.
//
// Both exits from doomMain_Run() come through here, so a failed startup leaves
// exactly the same state as a clean shutdown. Clearing the pointers matters
// beyond tidiness: leaving stale values behind would turn a second run - or a
// late frame callback - into a use-after-free.
static void releaseResources() {
    for (int i = 0; i < kbCount; i++) {
        if (kbDevices[i] != nullptr) {
            device_put(kbDevices[i]);
            kbDevices[i] = nullptr;
        }
    }
    kbCount = 0;
    heldKeyCount = 0;
    for (int i = 0; i < tbCount; i++) {
        if (tbDevices[i] != nullptr) {
            device_put(tbDevices[i]);
            tbDevices[i] = nullptr;
        }
    }
    tbCount = 0;
    if (touchDevice != nullptr) {
        device_put(touchDevice);
        touchDevice = nullptr;
    }
    if (overlayFrame != nullptr) {
        memory_free(overlayFrame);
        overlayFrame = nullptr;
    }
    touchControls = false;
    touchOverlayVisible = false;
    touchWaitForRelease = false;
    touchPressedMask = 0;
    memset(touchButtonKeys, 0, sizeof(touchButtonKeys));
    tbHoldX = {};
    tbHoldY = {};
    tbButtonDown = false;
    tbButtonKey = 0;
    if (doomKeyQueue != nullptr) {
        vQueueDelete(doomKeyQueue);
        doomKeyQueue = nullptr;
    }

    doomRenderer_Shutdown();
    memory_free(panelCanvas);
    panelCanvas = nullptr;

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
        LOG_W(TAG, "fopen failed: %s (errno %d)", candidate.c_str(), errno);
    }

    if (found) {
        outPath = candidate;
    }
    return found;
}

// --- Errors ---
//
// Why the last run stopped early, shown to the user once LVGL is back (see main.cpp).
static char errorMessage[160] = "";

static void setError(const char* message) {
    snprintf(errorMessage, sizeof(errorMessage), "%s", message);
    LOG_E(TAG, "%s", message);
}

const char* doomMain_GetError() {
    return errorMessage;
}

// The engine's fatal errors (I_Error(), e.g. not enough memory for the zone) jump back here
// instead of calling exit(), which would abort the whole device
static jmp_buf fatalErrorJump;

extern "C" [[noreturn]] void doomMain_FatalError(const char* message) {
    setError(message);
    running.store(false);
    longjmp(fatalErrorJump, 1);
}

// Starts the engine and runs its loop until doomMain_RequestStop().
// @return false when the engine stopped on a fatal error (see doomMain_FatalError())
static bool runEngine(int argc, char** argv) {
    // Separate from doomMain_Run() so that none of its locals live across the setjmp()
    if (setjmp(fatalErrorJump) != 0) {
        return false;
    }

    doomgeneric_Create(argc, argv);

    while (running.load()) {
        for (int i = 0; i < kbCount; i++) {
            if (kbDevices[i] != nullptr && device_is_ready(kbDevices[i])) {
                KeyboardKeyData data;
                while (keyboard_read_key(kbDevices[i], &data) == ERROR_NONE) {
                    if (data.key != 0 && data.pressed && touchOverlayVisible) {
                        hideTouchOverlay();
                    }
                    mapAndQueue(data.key, data.pressed);
                    if (!data.continue_reading) {
                        break;
                    }
                }
            }
        }
        pollTrackballs();
        pollTouch();
        doomgeneric_Tick();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return true;
}

// The engine's zone is the bulk of its memory use; it tries 6 MiB first and settles for 3 MiB
static constexpr size_t MIN_ZONE_BYTES = 3 * 1024 * 1024;

void doomMain_Run(Device* display, const DoomScreen& screen) {
    errorMessage[0] = '\0';

    // The zone is one block, so the largest free block is what counts, not the total
    const size_t external = memory_external_largest_block();
    const size_t internal = memory_heap_largest_block();
    const size_t largest = external > internal ? external : internal;
    // 0 means the platform can't tell (e.g. host builds): leave it to the engine's own check
    if (largest != 0 && largest < MIN_ZONE_BYTES) {
        setError("Not enough free memory. Doom needs at least 3 MB of free PSRAM.");
        return;
    }

    // Touch controls get a panel of their own below the game on portrait screens with room
    // to spare, so the renderer needs to know about the touch screen up front
    device_get_first_active_by_type(&TOUCH_TYPE, &touchDevice);

    if (!doomRenderer_Init(display, screen, touchDevice != nullptr)) {
        setError("Could not set up the display. It may be unsupported, or memory is low.");
        releaseResources();
        return;
    }

    // Working copy of the game frame, for the overlay and FPS counter (see composeGameFrame())
    overlayFrame = static_cast<uint8_t*>(memory_alloc(DOOM_W * DOOM_H));
    if (overlayFrame == nullptr) {
        setError("Not enough free memory.");
        releaseResources();
        return;
    }

    // On-screen touch controls, shown until a keyboard is used (see touchOverlayVisible)
    if (touchDevice != nullptr) {
        const int panelHeight = doomRenderer_PanelHeight();
        if (panelHeight > 0) {
            panelCanvas = static_cast<uint8_t*>(memory_calloc((size_t)DOOM_W * panelHeight, 1));
        }
        if (panelHeight == 0 || panelCanvas != nullptr) {
            touchOverlay_SetLayout(panelCanvas != nullptr ? 1 : 0, panelCanvas != nullptr ? panelHeight : DOOM_H);
            touchControls = true;
            touchOverlayVisible = true;
            LOG_I(TAG, "Touch controls enabled (%s)", panelCanvas != nullptr ? "panel below the game" : "over the game");
        } else {
            LOG_E(TAG, "Failed to allocate the control panel");
        }
    }

    // 3b. Discover all keyboard devices (best-effort - Doom runs fine without them)
    doomKeyQueue = xQueueCreate(32, sizeof(DoomKeyEvent));
    kbCount = 0;
    device_for_each_of_type(&KEYBOARD_TYPE, nullptr, [](struct Device* device, void*) -> bool {
        // A trackball in keyboard mode is read directly (see pollTrackballs()) instead.
        if (isTrackballChild(device)) {
            return true;
        }
        if (kbCount < MAX_KEYBOARDS) {
            device_get(device);
            kbDevices[kbCount++] = device;
        }
        return kbCount < MAX_KEYBOARDS;
    });
    tbCount = 0;
    device_for_each_of_type(&TRACKBALL_TYPE, nullptr, [](struct Device* device, void*) -> bool {
        if (tbCount < MAX_TRACKBALLS) {
            device_get(device);
            tbDevices[tbCount++] = device;
        }
        return tbCount < MAX_TRACKBALLS;
    });
    if (tbCount > 0) {
        LOG_I(TAG, "Found %d trackball device(s)", tbCount);
    }

    if (kbCount == 0) {
        LOG_W(TAG, "No keyboard devices found");
    } else {
        LOG_I(TAG, "Found %d keyboard device(s)", kbCount);
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
            LOG_I(TAG, "No IWAD on SD card - using bundled shareware WAD");
        }
    }

    if (!haveIwad) {
        LOG_E(TAG, "No IWAD found in %s (expected doom2.wad, doom.wad or doom1.wad) and no bundled fallback", wadDir.c_str());
        setError("No WAD file found.");
        releaseResources();
        return;
    }

    LOG_I(TAG, "Starting DOOM. IWAD: %s, PWAD: %s", iwadPath.c_str(), havePwad ? pwadPath.c_str() : "None");

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
    if (!runEngine(argc, argv)) {
        // Back from doomMain_FatalError(). I_Error() has already shut down sound and music (its
        // run-on-error exit handlers). Release the rest; all of these cope with being called
        // on a partially started engine.
        doomAudio_Shutdown();
        doomgeneric_Shutdown();
        W_Shutdown();
        Z_Shutdown();
        releaseResources();
        return;
    }

    // Persist the config (sound volumes, controls, etc). This is the only place
    // it is saved: d_main.c no longer registers M_SaveDefaults as an I_AtExit
    // handler, because those run from inside I_Quit() while the game loop is
    // still live.
    // This runs on the doom task after the render loop has stopped, never on the LVGL task.
    if (doomMain_savedir[0] != '\0') {
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
