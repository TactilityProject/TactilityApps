#pragma once

#include "Controller.h"
#include "Input.h"
#include "KeyBindings.h"
#include "RaceSetup.h"
#include "Sound.h"
#include "../render/Draw2D.h"
#include "../render/WorldRenderer.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hd {

struct GameConfig {
    std::string assetsDirectory; // contains levels/*.trk and vehicles.txt
    std::string userDirectory; // optional: levels/*.trk here are added to the list
    /** The display is black and white: keep a UI mask (see Canvas::uiMask) and adapt highlights */
    bool monochrome = false;
};

/**
 * The whole game, independent of the platform: menus, races, HUD and touch controls.
 * The platform calls update() and render() every frame and shows the framebuffer.
 */
class Game {
public:
    Game();
    ~Game();

    /** @param framebuffer RGB565, width x height, owned by the caller */
    bool init(uint16_t* framebuffer, int width, int height, const GameConfig& config);

    /** Where sound goes. Optional: without it the game is silent. Must outlive the game. */
    void setSound(SoundSystem* soundSystem) {
        sound = soundSystem;
        if (sound != nullptr) sound->setMode(audioMode);
    }

    /** The player's key bindings, for the platform to map keys with. Lives as long as the game. */
    const KeyBindings& keyBindings() const { return bindings; }

    /** @param frameTime seconds since the last update */
    void update(const InputState& input, float frameTime);
    void render();

    bool quitRequested() const { return quit; }
    /** The UI mask for this frame (Canvas::uiMask), or nullptr when not monochrome. */
    const uint8_t* uiMask() const { return canvas.uiMask; }

    /** Why init() failed, or the last loading problem. */
    const std::string& error() const { return errorMessage; }

private:
    enum class Screen { Menu, Settings, Controls, Race, Paused, Results };
    enum MenuRow { RowTrack, RowCraft, RowOpponents, RowDifficulty, RowStart, RowSettings, RowQuit, MenuRowCount };
    enum SettingsRow { SettingsAudio, SettingsQuality, SettingsFps, SettingsTouchLayout, SettingsControls, SettingsBack, SettingsRowCount };
    // The Controls screen: one row per control, then these
    enum ControlsRow { ControlsDefaults = CONTROL_COUNT, ControlsBack, ControlsRowCount };
    enum PauseRow { PauseResume, PauseRestart, PauseSettings, PauseQuit, PauseRowCount };

    // Touch buttons during a race, as bits for touchHeld
    enum TouchButton : uint32_t {
        TouchLeft = 1u << 0,
        TouchRight = 1u << 1,
        TouchAccelerate = 1u << 2,
        TouchBrake = 1u << 3,
        TouchBoost = 1u << 4,
        TouchPause = 1u << 5,
    };

    struct TrackEntry {
        std::string path;
        std::string name;
    };

    struct Rect {
        int x = 0, y = 0, w = 0, h = 0;
        bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    };

    Canvas canvas;
    std::vector<uint8_t> uiMaskStorage;
    GameConfig config;
    std::string errorMessage;
    bool quit = false;
    float time = 0.0f;
    float accumulator = 0.0f;

    // Content
    std::vector<TrackEntry> tracks;
    // Songs are kept for the whole game: the audio thread may still be playing one
    Song menuSong;
    Song resultsSong;
    bool menuSongLoaded = false;
    bool resultsSongLoaded = false;
    std::vector<std::unique_ptr<Song>> trackSongs; // per track, loaded on first use (nullptr: none)
    std::vector<bool> trackSongTried;
    std::vector<VehicleSpec> vehicles;
    Track track;
    int loadedTrack = -1;
    Difficulty loadedDifficulty = Difficulty::Normal;
    std::string loadedTrackText;

    // Menu selections
    Screen screen = Screen::Menu;
    int menuRow = RowStart;
    int pauseRow = PauseResume;
    int selectedTrack = 0;
    int selectedVehicle = 0;
    int opponents = 3;
    Difficulty difficulty = Difficulty::Normal;
    Rect menuRowRects[MenuRowCount];
    Rect pauseRowRects[PauseRowCount];
    int settingsRow = SettingsAudio;
    Rect settingsRowRects[SettingsRowCount];
    int controlsRow = 0;
    int controlsSlot = 0; // which of the control's keys is selected
    bool capturing = false; // waiting for the key to bind
    float captureTime = 0.0f;
    Rect controlsRowRects[ControlsRowCount];
    Rect slotRects[CONTROL_COUNT][KeyBindings::SLOTS];
    KeyBindings bindings;
    bool settingsChanged = false; // to save when leaving the settings
    bool settingsFromPause = false; // opened from the pause menu: return there, over the frozen race
    bool showFps = false;
    bool steerOnRight = false; // touch controls mirrored: the steering buttons right, the others left
    int quality = 0; // 3D resolution: 0 low (default), 1 medium, 2 high (full); the HUD and menus stay sharp
    void applyQuality();
    // Frames per second, counted over half a second
    float fpsTime = 0.0f;
    int fpsFrames = 0;
    int fps = 0;
    bool previewDirty = true;

    // Input edges and repeat
    uint32_t previousHeld = 0;
    uint32_t pressed = 0;
    float repeatTimer = 0.0f;
    int previousTouchCount = 0;
    bool touchControlsVisible = false;
    uint32_t touchHeld = 0;
    uint32_t previousTouchHeld = 0;

    // Race
    RaceSetup setup;
    RaceSim race;
    std::vector<std::unique_ptr<Controller>> controllers;
    LocalController* localController = nullptr;
    std::unique_ptr<AIController> autopilot; // drives the player after they finish
    int localRacer = 0;
    float steering = 0.0f; // the player's eased steering, -1..1
    std::vector<RacerInput> inputs;
    WorldRenderer world;
    float resultsDelay = 0.0f;
    float resultsTime = 0.0f; // seconds on the results screen
    int lastLapAnnounced = 0;

    // Time trial (no opponents): lap times and the records, per track file
    struct Record {
        float total = 0.0f; // 0: none yet
        float lap = 0.0f;
    };
    std::map<std::string, Record> records;
    std::vector<float> lapTimes; // this race, the player's
    bool newRecordTotal = false;
    bool newRecordLap = false;
    bool timeTrial() const { return race.racerCount() == 1; }
    std::string recordKey() const;
    void loadRecords();
    void saveRecords();
    void finishTimeTrial();
    float messageTime = 0.0f;

    // Sound
    SoundSystem* sound = nullptr;
    AudioMode audioMode = AudioMode::All;
    // The local racer last frame, to turn state changes into sound effects
    struct SoundWatch {
        bool airborne = false;
        bool boosting = false;
        bool finished = false;
        RacerMode mode = RacerMode::Driving;
        int lapsCompleted = -1;
        float wallHitTime = 99.0f;
        float raceTime = -RaceSim::COUNTDOWN - 0.01f; // so the first beep comes with the "3"
    } watch;

    void sfx(Sfx effect) { if (sound != nullptr) sound->play(effect); }
    void updateRaceSounds(bool active);
    std::string message;

    bool loadContent();
    bool loadSong(const std::string& path, Song& song);
    const Song* trackSong(int index);
    bool loadTrack(int index);
    void buildSetup(bool preview);
    void startRace(bool preview);

    void updateInputEdges(const InputState& input, float frameTime);
    void updateMenu(const InputState& input);
    void updateSettings(const InputState& input);
    void updateControls(const InputState& input, float frameTime);
    void loadSettings();
    void saveSettings();
    void updateRace(const InputState& input, float frameTime);
    void updatePaused(const InputState& input);
    void updateResults(const InputState& input, float frameTime);
    /** @return the TouchButton bits held */
    uint32_t touchButtonsAt(const InputState& input) const;
    struct TouchLayout {
        Rect pause;
        int leftX = 0, rightX = 0, steerY = 0; // the steering buttons, as big as GO (accelRadius)
        int accelX = 0, accelY = 0, accelRadius = 0;
        int brakeX = 0, brakeY = 0, boostX = 0, boostY = 0, smallRadius = 0;
    };
    TouchLayout layoutTouchControls() const;

    void drawMenu();
    void drawSettings();
    void drawFps();
    void drawControls();
    /** A centred panel of rows on a dimmed screen: @return the y of the first row */
    int drawListPanel(const char* title, int rows, int rowH, int panelW);
    void drawHud();
    void drawMinimap(int x, int y, int size);
    void drawTouchControls();
    void drawPause();
    void drawResults();
    int textScale() const;
};

} // namespace hd
