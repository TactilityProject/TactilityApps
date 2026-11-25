#include "Game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace hd {

namespace {

constexpr float MENU_REPEAT_DELAY = 0.35f;
constexpr float MENU_REPEAT_INTERVAL = 0.12f;
constexpr int MAX_STEPS_PER_FRAME = 5;
constexpr int MAX_OPPONENTS = 5;
// Steering easing, in full-lock units per second (see updateRace)
constexpr float STEER_TURN_IN_RATE = 5.0f;
constexpr float STEER_RELEASE_RATE = 8.0f;
const char* const AI_NAMES[] = { "NOVA", "VEGA", "ORION", "LYRA", "ZED" };

const uint16_t WHITE = rgb565(0xFFFFFF);
const uint16_t GREY = rgb565(0x9AA0B4);
const uint16_t ACCENT = rgb565(0xFFC94A);
const uint16_t RED = rgb565(0xE63946);
const uint16_t CYAN = rgb565(0x4CC9F0);
const uint16_t PANEL = rgb565(0x0A0E1A);

bool readFile(const std::string& path, std::string& out) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    out.clear();
    char buffer[512];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, n);
    }
    fclose(file);
    return true;
}

void listTracks(const std::string& directory, std::vector<std::string>& out) {
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) {
        return;
    }
    std::vector<std::string> names;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".trk") == 0) {
            names.push_back(name);
        }
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        out.push_back(directory + "/" + name);
    }
}

void formatTime(float seconds, char* out, size_t size) {
    if (seconds < 0.0f) seconds = 0.0f;
    const int total = (int)(seconds * 100.0f);
    snprintf(out, size, "%d:%02d.%02d", total / 6000, (total / 100) % 60, total % 100);
}

const char* placeSuffix(int place) {
    if (place % 100 >= 11 && place % 100 <= 13) return "TH";
    switch (place % 10) {
        case 1: return "ST";
        case 2: return "ND";
        case 3: return "RD";
        default: return "TH";
    }
}

} // namespace

Game::Game() = default;
Game::~Game() = default;

int Game::textScale() const {
    return std::max(1, canvas.height / 170);
}

bool Game::init(uint16_t* framebuffer, int width, int height, const GameConfig& gameConfig) {
    canvas = { framebuffer, width, height, nullptr };
    config = gameConfig;
    if (config.monochrome) {
        uiMaskStorage.assign((size_t)width * height, 0);
        canvas.uiMask = uiMaskStorage.data();
    }
    world.init(framebuffer, width, height);
    if (!loadContent()) {
        return false;
    }
    loadSettings();
    loadRecords();
    previewDirty = true;
    return true;
}

bool Game::loadContent() {
    std::string text;
    std::string error;
    if (!readFile(config.assetsDirectory + "/vehicles.txt", text) || !parseVehicles(text, vehicles, error)) {
        vehicles = defaultVehicles();
    }

    std::vector<std::string> paths;
    listTracks(config.assetsDirectory + "/levels", paths);
    if (!config.userDirectory.empty()) {
        listTracks(config.userDirectory + "/levels", paths);
    }
    tracks.clear();
    for (const auto& path : paths) {
        Track probe;
        if (readFile(path, text) && probe.load(text, error)) {
            tracks.push_back({ path, probe.name() });
        } else {
            printf("Voidskim: skipping %s: %s\n", path.c_str(), error.c_str());
        }
    }
    if (tracks.empty()) {
        errorMessage = "No tracks found in " + config.assetsDirectory + "/levels";
        return false;
    }
    trackSongs.clear();
    trackSongs.resize(tracks.size());
    trackSongTried.assign(tracks.size(), false);
    menuSongLoaded = loadSong(config.assetsDirectory + "/music/menu.bleep", menuSong);
    resultsSongLoaded = loadSong(config.assetsDirectory + "/music/results.bleep", resultsSong);
    return true;
}

bool Game::loadSong(const std::string& path, Song& song) {
    std::string text;
    std::string error;
    if (!readFile(path, text)) {
        return false;
    }
    if (!parseSong(text, song, error)) {
        printf("Voidskim: %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    return true;
}

const Song* Game::trackSong(int index) {
    if (index < 0 || index >= (int)tracks.size()) {
        return nullptr;
    }
    if (!trackSongTried[index]) {
        trackSongTried[index] = true;
        // The track's music: the .bleep file next to it, with the same name
        std::string path = tracks[index].path;
        path.replace(path.size() - 4, 4, ".bleep");
        auto song = std::make_unique<Song>();
        if (loadSong(path, *song)) {
            trackSongs[index] = std::move(song);
        }
    }
    return trackSongs[index].get();
}

bool Game::loadTrack(int index) {
    // Tracks can have difficulty-specific walls, so they're loaded per difficulty
    if (index == loadedTrack && difficulty == loadedDifficulty) {
        return true;
    }
    static const char* const DIFFICULTY_KEYS[] = { "easy", "normal", "hard" };
    std::string error;
    if (!readFile(tracks[index].path, loadedTrackText) || !track.load(loadedTrackText, error, DIFFICULTY_KEYS[(int)difficulty])) {
        errorMessage = "Failed to load " + tracks[index].path + ": " + error;
        loadedTrack = -1;
        return false;
    }
    loadedTrack = index;
    loadedDifficulty = difficulty;
    return true;
}

void Game::buildSetup(bool preview) {
    setup = RaceSetup();
    setup.track = selectedTrack;
    setup.trackChecksum = textChecksum(loadedTrackText);
    setup.laps = track.laps();
    setup.difficulty = difficulty;
    setup.seed = preview ? 1u : (uint32_t)(time * 1000.0f) + 1u;
    Random random(setup.seed);
    // The opponents start ahead; the player starts at the back of the grid
    for (int i = 0; i < opponents; i++) {
        RacerSlot slot;
        slot.kind = SlotKind::AI;
        slot.name = AI_NAMES[i % MAX_OPPONENTS];
        slot.vehicle = (int)(random.next() % vehicles.size());
        setup.slots.push_back(slot);
    }
    RacerSlot player;
    player.kind = SlotKind::Local;
    player.name = "YOU";
    player.vehicle = selectedVehicle;
    setup.slots.push_back(player);
}

void Game::startRace(bool preview) {
    if (!loadTrack(selectedTrack)) {
        return;
    }
    buildSetup(preview);
    race.setup(&track, &vehicles, setup.laps);
    controllers.clear();
    localController = nullptr;
    for (size_t i = 0; i < setup.slots.size(); i++) {
        const RacerSlot& slot = setup.slots[i];
        const int id = race.addRacer(slot.name, slot.vehicle, slot.kind != SlotKind::AI);
        if (slot.kind == SlotKind::Local) {
            auto controller = std::make_unique<LocalController>();
            localController = controller.get();
            localRacer = id;
            controllers.push_back(std::move(controller));
        } else {
            // Remote slots would get a net::RemoteController here (see net/NetSession.h)
            controllers.push_back(std::make_unique<AIController>(setup.difficulty, setup.seed * 31u + (uint32_t)i));
        }
    }
    inputs.assign(controllers.size(), RacerInput());
    steering = 0.0f;
    autopilot = std::make_unique<AIController>(Difficulty::Easy, setup.seed ^ 0x5A5Au);
    race.start();
    world.build(race);
    accumulator = 0.0f;
    resultsDelay = 0.0f;
    lastLapAnnounced = 0;
    lapTimes.clear();
    newRecordTotal = false;
    newRecordLap = false;
    message.clear();
    watch = SoundWatch();
    previewDirty = false;
}

// region Input

void Game::updateInputEdges(const InputState& input, float frameTime) {
    const uint32_t held = input.held;
    pressed = held & ~previousHeld;
    // Menu navigation repeats while held
    const uint32_t repeatable = ActionUp | ActionDown | ActionLeft | ActionRight;
    if ((held & repeatable) != 0 && (pressed & repeatable) == 0) {
        repeatTimer -= frameTime;
        if (repeatTimer <= 0.0f) {
            pressed |= held & repeatable;
            repeatTimer = MENU_REPEAT_INTERVAL;
        }
    } else {
        repeatTimer = MENU_REPEAT_DELAY;
    }
    previousHeld = held;

    // The touch controls show on touch and hide when a keyboard is used
    if (input.keyPressed) {
        touchControlsVisible = false;
    }
    if (input.touchCount > 0 && input.touchAvailable) {
        touchControlsVisible = true;
    }
}

void Game::layoutTouchButtons(Rect& pause, int& leftX, int& rightX, int& padY, int& padRadius,
                              int& accelX, int& accelY, int& accelRadius, int& brakeX, int& brakeY,
                              int& boostX, int& boostY, int& smallRadius) const {
    const int unit = std::min(canvas.width, canvas.height);
    const int margin = unit / 30 + 2;
    padRadius = unit * 12 / 100;
    accelRadius = unit * 14 / 100;
    smallRadius = unit * 10 / 100;
    padY = canvas.height - margin - padRadius;
    leftX = margin + padRadius;
    rightX = leftX + padRadius * 2 + margin;
    accelX = canvas.width - margin - accelRadius;
    if (canvas.height > canvas.width) {
        // Portrait: no room beside the steering pad, so stack them in one column, brake at the bottom
        brakeX = accelX;
        brakeY = canvas.height - margin - smallRadius;
        accelY = brakeY - smallRadius - margin - accelRadius;
        boostX = accelX;
        boostY = accelY - accelRadius - margin - smallRadius;
    } else {
        accelY = canvas.height - margin - accelRadius;
        brakeX = accelX - accelRadius - margin - smallRadius;
        brakeY = canvas.height - margin - smallRadius;
        boostX = accelX;
        boostY = accelY - accelRadius - margin - smallRadius;
    }
    const int pauseSize = unit / 9;
    pause = { canvas.width / 2 - pauseSize / 2, margin, pauseSize, pauseSize };
}

uint32_t Game::touchButtonsAt(const InputState& input) const {
    Rect pause;
    int leftX, rightX, padY, padRadius, accelX, accelY, accelRadius, brakeX, brakeY, boostX, boostY, smallRadius;
    layoutTouchButtons(pause, leftX, rightX, padY, padRadius, accelX, accelY, accelRadius, brakeX, brakeY, boostX, boostY, smallRadius);
    auto inCircle = [](int x, int y, int cx, int cy, int r) {
        const int reach = r * 115 / 100; // fingers are less precise than the outlines
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= reach * reach;
    };
    uint32_t buttons = 0;
    for (int i = 0; i < input.touchCount; i++) {
        const int x = input.touches[i].x;
        const int y = input.touches[i].y;
        if (inCircle(x, y, leftX, padY, padRadius)) buttons |= TouchLeft;
        else if (inCircle(x, y, rightX, padY, padRadius)) buttons |= TouchRight;
        else if (inCircle(x, y, accelX, accelY, accelRadius)) buttons |= TouchAccelerate;
        else if (inCircle(x, y, brakeX, brakeY, smallRadius)) buttons |= TouchBrake;
        else if (inCircle(x, y, boostX, boostY, smallRadius)) buttons |= TouchBoost;
        else if (pause.contains(x, y)) buttons |= TouchPause;
    }
    return buttons;
}

// endregion

void Game::update(const InputState& input, float frameTime) {
    frameTime = std::min(frameTime, 0.1f);
    time += frameTime;
    updateInputEdges(input, frameTime);
    switch (screen) {
        case Screen::Menu: updateMenu(input); break;
        case Screen::Settings: updateSettings(input); break;
        case Screen::Controls: updateControls(input, frameTime); break;
        case Screen::Race: updateRace(input, frameTime); break;
        case Screen::Paused: updatePaused(input); break;
        case Screen::Results: updateResults(input, frameTime); break;
    }
    previousTouchCount = input.touchCount;
}

void Game::updateMenu(const InputState& input) {
    auto change = [&](int row, int delta) {
        switch (row) {
            case RowTrack:
                selectedTrack = (selectedTrack + delta + (int)tracks.size()) % (int)tracks.size();
                previewDirty = true;
                break;
            case RowCraft:
                selectedVehicle = (selectedVehicle + delta + (int)vehicles.size()) % (int)vehicles.size();
                previewDirty = true;
                break;
            case RowOpponents:
                opponents = std::clamp(opponents + delta, 0, MAX_OPPONENTS);
                previewDirty = true;
                break;
            case RowDifficulty:
                difficulty = (Difficulty)(((int)difficulty + delta + 3) % 3);
                previewDirty = true; // the track's walls can depend on it
                break;
            default:
                break;
        }
        sfx(Sfx::MenuMove);
    };
    auto activate = [&](int row) {
        sfx(row == RowQuit ? Sfx::MenuBack : Sfx::MenuSelect);
        if (row == RowStart) {
            startRace(false);
            if (loadedTrack == selectedTrack) {
                screen = Screen::Race;
            }
        } else if (row == RowSettings) {
            screen = Screen::Settings;
            settingsRow = SettingsAudio;
        } else if (row == RowQuit) {
            quit = true;
        }
    };

    if (sound != nullptr) {
        sound->setMusic(menuSongLoaded ? &menuSong : nullptr);
        sound->setEngine(0.0f, false, false, false);
    }
    if (pressed & (ActionUp | ActionDown)) sfx(Sfx::MenuMove);
    if (pressed & ActionUp) menuRow = (menuRow + MenuRowCount - 1) % MenuRowCount;
    if (pressed & ActionDown) menuRow = (menuRow + 1) % MenuRowCount;
    if (pressed & ActionLeft) change(menuRow, -1);
    if (pressed & ActionRight) change(menuRow, 1);
    if (pressed & ActionConfirm) {
        if (menuRow < RowStart) change(menuRow, 1); else activate(menuRow);
    }
    if (pressed & ActionBack) {
        menuRow = RowQuit;
        sfx(Sfx::MenuBack);
    }

    // Touch: tap a row. On a setting, the left half goes back and the right half forward.
    if (input.touchCount > 0 && previousTouchCount == 0) {
        const int x = input.touches[0].x;
        const int y = input.touches[0].y;
        for (int row = 0; row < MenuRowCount; row++) {
            if (menuRowRects[row].contains(x, y)) {
                menuRow = row;
                if (row < RowStart) {
                    change(row, x < menuRowRects[row].x + menuRowRects[row].w / 2 ? -1 : 1);
                } else {
                    activate(row);
                }
            }
        }
    }

    if (previewDirty && screen == Screen::Menu) {
        startRace(true);
    }
}

void Game::updateRace(const InputState& input, float frameTime) {
    previousTouchHeld = touchHeld;
    touchHeld = touchControlsVisible ? touchButtonsAt(input) : 0;
    const uint32_t touchPressed = touchHeld & ~previousTouchHeld;

    if ((pressed & (ActionPause | ActionBack)) || (touchPressed & TouchPause)) {
        screen = Screen::Paused;
        pauseRow = PauseResume;
        return;
    }

    // The local player's input
    if (localController != nullptr) {
        const uint32_t held = input.held;
        RacerInput local;
        const bool left = (held & ActionSteerLeft) || (touchHeld & TouchLeft);
        const bool right = (held & ActionSteerRight) || (touchHeld & TouchRight);
        // Keys, trackball and touch are all on/off: ease the steering in and out so the craft
        // turns (and banks) smoothly. Returning to centre or reversing is quicker than turning in.
        const float target = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
        const bool easingOut = target == 0.0f || target * steering < 0.0f;
        steering = approach(steering, target, (easingOut ? STEER_RELEASE_RATE : STEER_TURN_IN_RATE) * frameTime);
        local.steer = steering;
        local.throttle = (held & ActionAccelerate) || (touchHeld & TouchAccelerate);
        local.brake = (held & ActionBrake) || (touchHeld & TouchBrake);
        local.boost = (held & ActionBoost) || (touchHeld & TouchBoost);
        // Boost implies going forwards
        local.throttle = local.throttle || local.boost;
        localController->set(local);
    }

    accumulator += frameTime;
    int steps = 0;
    while (accumulator >= RaceSim::TICK && steps < MAX_STEPS_PER_FRAME) {
        for (size_t i = 0; i < controllers.size(); i++) {
            inputs[i] = controllers[i]->control(race, (int)i);
        }
        // Once finished, the player's craft drives itself for the cool-down lap
        const Racer& me = race.racer(localRacer);
        if (me.finished && autopilot) {
            inputs[localRacer] = autopilot->control(race, localRacer);
        }
        race.step(inputs);
        accumulator -= RaceSim::TICK;
        steps++;
    }
    if (steps == MAX_STEPS_PER_FRAME) {
        accumulator = 0.0f; // too slow to keep up: slow the game down rather than spiral
    }

    const Racer& me = race.racer(localRacer);
    // Lap times, as each lap completes
    // (not the cool-down laps the autopilot drives after the finish)
    while ((int)lapTimes.size() < std::min(me.lapsCompleted, race.laps())) {
        lapTimes.push_back(me.lastLapTime);
        if (timeTrial() && !me.finished) {
            // How this lap went, against the best lap so far (this race or the record)
            float best = records[recordKey()].lap;
            for (size_t i = 0; i + 1 < lapTimes.size(); i++) {
                if (best <= 0.0f || lapTimes[i] < best) best = lapTimes[i];
            }
            char lapText[16];
            formatTime(me.lastLapTime, lapText, sizeof(lapText));
            char text[48];
            if (best > 0.0f) {
                const float delta = me.lastLapTime - best;
                snprintf(text, sizeof(text), "%s  %c%d.%02d", lapText, delta < 0.0f ? '-' : '+',
                         (int)std::fabs(delta), (int)(std::fabs(delta) * 100.0f) % 100);
            } else {
                snprintf(text, sizeof(text), "LAP %s", lapText);
            }
            message = text;
            messageTime = 2.5f;
        }
    }
    if (me.lapsCompleted == race.laps() - 1 && lastLapAnnounced < me.lapsCompleted && race.laps() > 1) {
        lastLapAnnounced = me.lapsCompleted;
        if (!timeTrial()) {
            message = "FINAL LAP";
            messageTime = 2.0f;
        }
    }
    messageTime = std::max(0.0f, messageTime - frameTime);

    world.update(race, localRacer, time, frameTime);
    updateRaceSounds(true);

    // After the player finishes the others race on (up to FINISH_GRACE): any key or tap skips that
    const bool anyInput = input.keyPressed || pressed != 0 || (input.touchCount > 0 && previousTouchCount == 0);
    if (me.finished && race.phase() != RacePhase::Finished && anyInput) {
        race.finishRemaining();
        resultsDelay = 1.5f;
    }
    if (race.phase() == RacePhase::Finished) {
        resultsDelay += frameTime;
        if (resultsDelay > 1.5f) {
            if (timeTrial()) finishTimeTrial();
            screen = Screen::Results;
            resultsTime = 0.0f;
        }
    }
}

void Game::updatePaused(const InputState& input) {
    auto activate = [&](int row) {
        switch (row) {
            case PauseResume: screen = Screen::Race; sfx(Sfx::MenuSelect); break;
            case PauseRestart: startRace(false); screen = Screen::Race; break;
            case PauseQuit: screen = Screen::Menu; previewDirty = true; break;
        }
    };
    updateRaceSounds(false); // engine quiet while paused
    if (pressed & (ActionUp | ActionDown)) sfx(Sfx::MenuMove);
    if (pressed & ActionUp) pauseRow = (pauseRow + PauseRowCount - 1) % PauseRowCount;
    if (pressed & ActionDown) pauseRow = (pauseRow + 1) % PauseRowCount;
    if (pressed & ActionConfirm) activate(pauseRow);
    else if (pressed & ActionBack) activate(PauseResume);
    if (input.touchCount > 0 && previousTouchCount == 0) {
        for (int row = 0; row < PauseRowCount; row++) {
            if (pauseRowRects[row].contains(input.touches[0].x, input.touches[0].y)) {
                activate(row);
            }
        }
    }
    // Releasing the pause touch shouldn't count as a new press when resuming
    touchHeld = 0;
    previousTouchHeld = TouchPause;
}

void Game::updateRaceSounds(bool active) {
    if (sound == nullptr) {
        return;
    }
    const Racer& me = race.racer(localRacer);
    const VehicleSpec& spec = race.vehicle(me);
    sound->setMusic(trackSong(loadedTrack));
    sound->setEngine(me.speed() / spec.topSpeed, me.input.throttle, me.boosting, active && me.mode == RacerMode::Driving);
    if (!active) {
        return;
    }

    // Countdown: a beep each second, a higher one at the start
    const float t = race.raceTime();
    if (t < 0.0f && std::ceil(t) != std::ceil(watch.raceTime)) {
        sfx(Sfx::CountdownBeep);
    }
    if (t >= 0.0f && watch.raceTime < 0.0f) {
        sfx(Sfx::CountdownGo);
    }
    // State changes of the player's craft
    if (me.boosting && !watch.boosting) sfx(Sfx::Boost);
    if (me.wallHitTime < watch.wallHitTime && me.wallHitTime < 0.05f) sfx(Sfx::WallHit);
    if (me.mode == RacerMode::Driving && watch.mode == RacerMode::Driving) {
        if (me.airborne && !watch.airborne && me.verticalSpeed > 2.0f) sfx(Sfx::Launch);
        if (!me.airborne && watch.airborne) sfx(Sfx::Land);
    }
    if (me.mode == RacerMode::Falling && watch.mode == RacerMode::Driving) sfx(Sfx::Fall);
    if (me.mode == RacerMode::Driving && watch.mode == RacerMode::Respawning) sfx(Sfx::Respawn);
    if (me.finished && !watch.finished) {
        sfx(Sfx::Finish);
    } else if (me.lapsCompleted > watch.lapsCompleted && me.lapsCompleted >= 1 && !me.finished) {
        sfx(me.lapsCompleted == race.laps() - 1 ? Sfx::FinalLap : Sfx::Lap);
    }
    watch.airborne = me.airborne;
    watch.boosting = me.boosting;
    watch.finished = me.finished;
    watch.mode = me.mode;
    watch.lapsCompleted = me.lapsCompleted;
    watch.wallHitTime = me.wallHitTime;
    watch.raceTime = t;
}

void Game::updateResults(const InputState& input, float frameTime) {
    if (sound != nullptr) {
        sound->setMusic(resultsSongLoaded ? &resultsSong : nullptr);
        sound->setEngine(0.0f, false, false, false);
    }
    // Any key or tap continues, after a moment so a key held from the race doesn't skip it
    resultsTime += frameTime;
    const bool tapped = input.touchCount > 0 && previousTouchCount == 0;
    if (resultsTime > 0.4f && (tapped || pressed != 0 || input.keyPressed)) {
        sfx(Sfx::MenuSelect);
        screen = Screen::Menu;
        previewDirty = true;
    }
}

// region Settings

namespace {

const char* const AUDIO_NAMES[] = { "MUSIC + SFX", "SFX ONLY", "OFF" };
constexpr float CAPTURE_TIMEOUT = 6.0f;

} // namespace

void Game::loadSettings() {
    if (config.userDirectory.empty()) {
        return;
    }
    std::string text;
    if (!readFile(config.userDirectory + "/settings.txt", text)) {
        return;
    }
    int mode = 0;
    const size_t at = text.find("audio ");
    if (at != std::string::npos && sscanf(text.c_str() + at, "audio %d", &mode) == 1 && mode >= 0 && mode < 3) {
        audioMode = (AudioMode)mode;
    }
    bindings.parse(text);
}

void Game::saveSettings() {
    settingsChanged = false;
    if (config.userDirectory.empty()) {
        return;
    }
    mkdir(config.userDirectory.c_str(), 0777); // may already exist
    const std::string path = config.userDirectory + "/settings.txt";
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        errorMessage = "Could not save " + path;
        return;
    }
    fprintf(file, "# Voidskim settings\naudio %d\n", (int)audioMode);
    const std::string keys = bindings.serialise();
    fwrite(keys.data(), 1, keys.size(), file);
    fclose(file);
}

std::string Game::recordKey() const {
    if (loadedTrack < 0 || loadedTrack >= (int)tracks.size()) return std::string();
    // The track file's name: stable, and without spaces
    const std::string& path = tracks[loadedTrack].path;
    const size_t slash = path.find_last_of('/');
    return path.substr(slash == std::string::npos ? 0 : slash + 1);
}

void Game::loadRecords() {
    records.clear();
    std::string text;
    if (config.userDirectory.empty() || !readFile(config.userDirectory + "/records.txt", text)) return;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        char name[96] = {};
        int totalMs = 0, lapMs = 0;
        if (sscanf(line.c_str(), "record %95s %d %d", name, &totalMs, &lapMs) == 3) {
            records[name] = { totalMs / 1000.0f, lapMs / 1000.0f };
        }
    }
}

void Game::saveRecords() {
    if (config.userDirectory.empty()) return;
    mkdir(config.userDirectory.c_str(), 0777); // may already exist
    FILE* file = fopen((config.userDirectory + "/records.txt").c_str(), "wb");
    if (file == nullptr) return;
    fprintf(file, "# Voidskim time trial records: track file, best race and best lap in milliseconds\n");
    for (const auto& [name, record] : records) {
        if (record.total > 0.0f || record.lap > 0.0f) {
            fprintf(file, "record %s %d %d\n", name.c_str(), (int)(record.total * 1000.0f + 0.5f), (int)(record.lap * 1000.0f + 0.5f));
        }
    }
    fclose(file);
}

void Game::finishTimeTrial() {
    const Racer& me = race.racer(localRacer);
    if (!me.finished || me.finishTime >= 1e8f) return;
    Record& record = records[recordKey()];
    newRecordTotal = record.total <= 0.0f || me.finishTime < record.total;
    newRecordLap = me.bestLapTime > 0.0f && (record.lap <= 0.0f || me.bestLapTime < record.lap);
    if (newRecordTotal) record.total = me.finishTime;
    if (newRecordLap) record.lap = me.bestLapTime;
    if (newRecordTotal || newRecordLap) saveRecords();
}

void Game::updateSettings(const InputState& input) {
    auto changeAudio = [&](int delta) {
        audioMode = (AudioMode)(((int)audioMode + delta + 3) % 3);
        if (sound != nullptr) sound->setMode(audioMode);
        settingsChanged = true;
        sfx(Sfx::MenuMove);
    };
    auto leave = [&]() {
        if (settingsChanged) saveSettings();
        screen = Screen::Menu;
        sfx(Sfx::MenuBack);
    };
    auto activate = [&](int row) {
        switch (row) {
            case SettingsAudio: changeAudio(1); break;
            case SettingsControls:
                screen = Screen::Controls;
                controlsRow = 0;
                controlsSlot = 0;
                capturing = false;
                sfx(Sfx::MenuSelect);
                break;
            case SettingsBack: leave(); break;
        }
    };
    if (pressed & (ActionUp | ActionDown)) sfx(Sfx::MenuMove);
    if (pressed & ActionUp) settingsRow = (settingsRow + SettingsRowCount - 1) % SettingsRowCount;
    if (pressed & ActionDown) settingsRow = (settingsRow + 1) % SettingsRowCount;
    if (settingsRow == SettingsAudio && (pressed & ActionLeft)) changeAudio(-1);
    if (settingsRow == SettingsAudio && (pressed & ActionRight)) changeAudio(1);
    if (pressed & ActionConfirm) {
        activate(settingsRow);
    } else if (pressed & ActionBack) {
        leave();
    }
    if (input.touchCount > 0 && previousTouchCount == 0) {
        const int x = input.touches[0].x;
        const int y = input.touches[0].y;
        for (int row = 0; row < SettingsRowCount; row++) {
            if (settingsRowRects[row].contains(x, y)) {
                settingsRow = row;
                if (row == SettingsAudio) {
                    changeAudio(x < settingsRowRects[row].x + settingsRowRects[row].w / 2 ? -1 : 1);
                } else {
                    activate(row);
                }
            }
        }
    }
}

void Game::updateControls(const InputState& input, float frameTime) {
    const bool tapped = input.touchCount > 0 && previousTouchCount == 0;
    if (capturing) {
        // The next key pressed is the new binding. A tap, or waiting, cancels.
        captureTime += frameTime;
        if (input.keysPressedCount > 0) {
            bindings.bind((Control)controlsRow, controlsSlot, input.keysPressed[0]);
            settingsChanged = true;
            capturing = false;
            sfx(Sfx::MenuSelect);
        } else if (tapped || captureTime > CAPTURE_TIMEOUT) {
            capturing = false;
            sfx(Sfx::MenuBack);
        }
        return;
    }
    auto startCapture = [&]() {
        capturing = true;
        captureTime = 0.0f;
        sfx(Sfx::MenuSelect);
    };
    auto activate = [&](int row) {
        if (row < CONTROL_COUNT) {
            startCapture();
        } else if (row == ControlsDefaults) {
            bindings.setDefaults();
            settingsChanged = true;
            sfx(Sfx::MenuSelect);
        } else {
            screen = Screen::Settings;
            sfx(Sfx::MenuBack);
        }
    };
    if (pressed & (ActionUp | ActionDown | ActionLeft | ActionRight)) sfx(Sfx::MenuMove);
    if (pressed & ActionUp) controlsRow = (controlsRow + ControlsRowCount - 1) % ControlsRowCount;
    if (pressed & ActionDown) controlsRow = (controlsRow + 1) % ControlsRowCount;
    if (pressed & ActionLeft) controlsSlot = 0;
    if (pressed & ActionRight) controlsSlot = KeyBindings::SLOTS - 1;
    if (pressed & ActionConfirm) {
        activate(controlsRow);
    } else if (pressed & ActionBack) {
        activate(ControlsBack);
    }
    if (tapped) {
        const int x = input.touches[0].x;
        const int y = input.touches[0].y;
        for (int row = 0; row < ControlsRowCount; row++) {
            if (!controlsRowRects[row].contains(x, y)) continue;
            controlsRow = row;
            if (row < CONTROL_COUNT) {
                for (int slot = 0; slot < KeyBindings::SLOTS; slot++) {
                    if (slotRects[row][slot].contains(x, y)) {
                        controlsSlot = slot;
                        startCapture();
                    }
                }
            } else {
                activate(row);
            }
        }
    }
}

// endregion

// region Rendering

void Game::render() {
    if (canvas.uiMask != nullptr) {
        std::fill(uiMaskStorage.begin(), uiMaskStorage.end(), 0);
    }
    switch (screen) {
        case Screen::Menu:
            world.updateShowcase(race, time);
            world.render();
            drawMenu();
            break;
        case Screen::Settings:
            world.updateShowcase(race, time);
            world.render();
            drawSettings();
            break;
        case Screen::Controls:
            world.updateShowcase(race, time);
            world.render();
            drawControls();
            break;
        case Screen::Race:
            world.render();
            drawHud();
            if (touchControlsVisible) drawTouchControls();
            break;
        case Screen::Paused:
            world.render();
            drawHud();
            drawPause();
            break;
        case Screen::Results:
            world.render();
            drawResults();
            break;
    }
}

void Game::drawMenu() {
    const int s = textScale();
    const int w = canvas.width;
    const int h = canvas.height;

    // Layout: title, settings, craft stats below. When that doesn't fit (short or very wide
    // screens), the stats move beside the settings if there's room, then the title and rows
    // shrink, and as a last resort the stats are left out.
    const int top = std::max(2, h / 14);
    const int gap = 4 * s;
    const int statsRowH = 9 * s;
    const int statsH = 4 * statsRowH + 4 * s;
    const int statsW = 150 * s;
    int titleScale = s * 3;
    int rowH = 13 * s;
    bool statsBeside = false;
    bool showStats = true;
    auto neededHeight = [&]() {
        const int panelH = rowH * MenuRowCount + 8 * s;
        const int body = !showStats ? panelH : (statsBeside ? std::max(panelH, statsH) : panelH + gap + statsH);
        return top + 7 * titleScale + 6 * s + body + 2;
    };
    if (neededHeight() > h && w >= 220 * s + statsW + 3 * gap) {
        statsBeside = true;
    }
    while (neededHeight() > h && titleScale > s) {
        titleScale -= s;
    }
    if (neededHeight() > h) {
        rowH = 10 * s;
    }
    if (neededHeight() > h) {
        showStats = false;
    }

    drawTextCentered(canvas, w / 2, top, "VOIDSKIM", titleScale, ACCENT);

    const int panelW = statsBeside ? std::min(300 * s, w - statsW - 3 * gap) : std::min(w - 8, 300 * s);
    const int panelH = rowH * MenuRowCount + 8 * s;
    const int groupW = statsBeside ? panelW + gap + statsW : panelW;
    const int panelX = (w - groupW) / 2;
    const int panelY = top + 7 * titleScale + 6 * s;
    const int panelCentre = panelX + panelW / 2;
    blendRect(canvas, panelX, panelY, panelW, panelH, PANEL, 190);

    char value[48];
    for (int row = 0; row < MenuRowCount; row++) {
        const int y = panelY + 4 * s + row * rowH;
        const bool selected = row == menuRow;
        menuRowRects[row] = { panelX, y, panelW, rowH };
        if (selected) {
            blendRect(canvas, panelX + 2, y, panelW - 4, rowH - 2, ACCENT, 60);
            if (config.monochrome) {
                // A faint highlight disappears in black and white: outline the row instead
                drawRect(canvas, panelX + 2, y, panelW - 4, rowH - 2, WHITE);
            }
        }
        const uint16_t colour = selected ? WHITE : GREY;
        const int textY = y + (rowH - 7 * s) / 2;
        const char* label = nullptr;
        value[0] = '\0';
        switch (row) {
            case RowTrack: label = "TRACK"; snprintf(value, sizeof(value), "%s", tracks[selectedTrack].name.c_str()); break;
            case RowCraft: label = "CRAFT"; snprintf(value, sizeof(value), "%s", vehicles[selectedVehicle].name.c_str()); break;
            case RowOpponents:
                label = "OPPONENTS";
                if (opponents == 0) snprintf(value, sizeof(value), "NONE: TIME TRIAL");
                else snprintf(value, sizeof(value), "%d", opponents);
                break;
            case RowDifficulty: label = "DIFFICULTY"; snprintf(value, sizeof(value), "%s", difficultyName(difficulty)); break;
            case RowStart: drawTextCentered(canvas, panelCentre, textY, "START RACE", s, selected ? ACCENT : colour); break;
            case RowSettings: drawTextCentered(canvas, panelCentre, textY, "SETTINGS", s, colour); break;
            case RowQuit: drawTextCentered(canvas, panelCentre, textY, "QUIT", s, colour); break;
        }
        if (label != nullptr) {
            drawText(canvas, panelX + 6 * s, textY, label, s, colour);
            const int valueRight = panelX + panelW - 6 * s;
            const int valueWidth = textWidth(value, s);
            drawText(canvas, valueRight - valueWidth - 10 * s, textY, value, s, selected ? ACCENT : colour);
            fillArrow(canvas, valueRight - valueWidth - 15 * s, textY + 3 * s, 4 * s, 3, colour);
            fillArrow(canvas, valueRight - 2 * s, textY + 3 * s, 4 * s, 1, colour);
        }
    }

    // Craft stats, compared to the best of each
    const VehicleSpec& v = vehicles[selectedVehicle];
    float maxSpeed = 1, maxAccel = 1, maxHandling = 1, maxBoost = 1;
    for (const auto& o : vehicles) {
        maxSpeed = std::max(maxSpeed, o.topSpeed);
        maxAccel = std::max(maxAccel, o.acceleration);
        maxHandling = std::max(maxHandling, o.turnRate * o.grip);
        maxBoost = std::max(maxBoost, o.boostSpeed * o.boostTime);
    }
    const float stats[4] = { v.topSpeed / maxSpeed, v.acceleration / maxAccel, v.turnRate * v.grip / maxHandling,
                             v.boostSpeed * v.boostTime / maxBoost };
    const char* names[4] = { "SPEED", "ACCEL", "GRIP", "BOOST" };
    if (showStats) {
        const int statsX = statsBeside ? panelX + panelW + gap : panelX;
        const int statsY = statsBeside ? panelY : panelY + panelH + gap;
        const int width = statsBeside ? statsW : panelW;
        blendRect(canvas, statsX, statsY, width, statsH, PANEL, 170);
        for (int i = 0; i < 4; i++) {
            const int y = statsY + 3 * s + i * statsRowH;
            drawText(canvas, statsX + 6 * s, y, names[i], s, GREY);
            const int barX = statsX + 50 * s;
            const int barW = width - 56 * s;
            fillRect(canvas, barX, y + s, barW, 5 * s, rgb565(0x2A2F45));
            fillRect(canvas, barX, y + s, (int)(barW * stats[i]), 5 * s, rgb565(v.colour));
        }
    }
}

int Game::drawListPanel(const char* title, int rows, int rowH, int panelW) {
    const int s = textScale();
    const int w = canvas.width;
    const int h = canvas.height;
    blendRect(canvas, 0, 0, w, h, 0x0000, 110);
    const int titleScale = h >= 7 * 3 * s + 14 * s + rows * rowH + 10 * s ? s * 3 : s * 2;
    const int contentH = 7 * titleScale + 8 * s + rows * rowH + 8 * s;
    const int top = std::max(2, (h - contentH) / 2);
    drawTextCentered(canvas, w / 2, top, title, titleScale, ACCENT);
    const int panelY = top + 7 * titleScale + 8 * s;
    blendRect(canvas, (w - panelW) / 2, panelY, panelW, rows * rowH + 8 * s, PANEL, 190);
    return panelY + 4 * s;
}

void Game::drawSettings() {
    const int s = textScale();
    const int w = canvas.width;
    const int rowH = 13 * s;
    const int panelW = std::min(w - 8, 260 * s);
    const int panelX = (w - panelW) / 2;
    const int y0 = drawListPanel("SETTINGS", SettingsRowCount, rowH, panelW);
    for (int row = 0; row < SettingsRowCount; row++) {
        const int y = y0 + row * rowH;
        const bool selected = row == settingsRow;
        settingsRowRects[row] = { panelX, y, panelW, rowH };
        if (selected) {
            blendRect(canvas, panelX + 2, y, panelW - 4, rowH - 2, ACCENT, 60);
            if (config.monochrome) drawRect(canvas, panelX + 2, y, panelW - 4, rowH - 2, WHITE);
        }
        const uint16_t colour = selected ? WHITE : GREY;
        const int textY = y + (rowH - 7 * s) / 2;
        if (row == SettingsAudio) {
            drawText(canvas, panelX + 6 * s, textY, "AUDIO", s, colour);
            const char* value = AUDIO_NAMES[(int)audioMode];
            const int valueRight = panelX + panelW - 6 * s;
            const int valueWidth = textWidth(value, s);
            drawText(canvas, valueRight - valueWidth - 10 * s, textY, value, s, selected ? ACCENT : colour);
            fillArrow(canvas, valueRight - valueWidth - 15 * s, textY + 3 * s, 4 * s, 3, colour);
            fillArrow(canvas, valueRight - 2 * s, textY + 3 * s, 4 * s, 1, colour);
        } else {
            drawTextCentered(canvas, w / 2, textY, row == SettingsControls ? "CONTROLS" : "BACK", s, colour);
        }
    }
}

void Game::drawControls() {
    const int s = textScale();
    const int w = canvas.width;
    const int h = canvas.height;
    const int rowH = std::max(9 * s, std::min(12 * s, (h - 7 * 2 * s - 40 * s) / ControlsRowCount));
    const int panelW = std::min(w - 8, 280 * s);
    const int panelX = (w - panelW) / 2;
    const int y0 = drawListPanel("CONTROLS", ControlsRowCount, rowH, panelW);
    const int slotW = std::min(52 * s, (panelW - 12 * s) / 4);
    char name[16];
    for (int row = 0; row < ControlsRowCount; row++) {
        const int y = y0 + row * rowH;
        const bool selected = row == controlsRow;
        controlsRowRects[row] = { panelX, y, panelW, rowH };
        const uint16_t colour = selected ? WHITE : GREY;
        const int textY = y + (rowH - 7 * s) / 2;
        if (row >= CONTROL_COUNT) {
            if (selected) {
                blendRect(canvas, panelX + 2, y, panelW - 4, rowH - 2, ACCENT, 60);
                if (config.monochrome) drawRect(canvas, panelX + 2, y, panelW - 4, rowH - 2, WHITE);
            }
            drawTextCentered(canvas, w / 2, textY, row == ControlsDefaults ? "DEFAULTS" : "BACK", s, colour);
            continue;
        }
        drawText(canvas, panelX + 6 * s, textY, KeyBindings::controlName((Control)row), s, colour);
        for (int slot = 0; slot < KeyBindings::SLOTS; slot++) {
            const int x = panelX + panelW - 4 * s - (KeyBindings::SLOTS - slot) * (slotW + 2 * s);
            slotRects[row][slot] = { x, y, slotW, rowH - 2 };
            const bool active = selected && slot == controlsSlot;
            if (active) {
                blendRect(canvas, x, y, slotW, rowH - 2, ACCENT, capturing ? 140 : 70);
                drawRect(canvas, x, y, slotW, rowH - 2, capturing || config.monochrome ? WHITE : ACCENT);
            } else {
                blendRect(canvas, x, y, slotW, rowH - 2, 0x0000, 90);
            }
            if (active && capturing) {
                if (((int)(time * 4.0f)) % 2 == 0) drawTextCentered(canvas, x + slotW / 2, textY, "?", s, WHITE);
                continue;
            }
            KeyBindings::keyName(bindings.key((Control)row, slot), name, sizeof(name));
            drawTextCentered(canvas, x + slotW / 2, textY, name, s, active ? WHITE : colour);
        }
    }
    const char* hint = capturing ? "PRESS A KEY  (WAIT OR TAP TO CANCEL)" : "SELECT A KEY TO CHANGE IT";
    const int hintScale = textWidth(hint, s) <= w - 4 ? s : std::max(1, s / 2);
    drawTextCentered(canvas, w / 2, std::min(h - 9 * hintScale, y0 + ControlsRowCount * rowH + 8 * s), hint, hintScale, capturing ? ACCENT : GREY);
}

void Game::drawHud() {
    const int s = textScale();
    const int w = canvas.width;
    const int h = canvas.height;
    const Racer& me = race.racer(localRacer);
    char text[48];

    // Black and white: solid backings, as small text gets lost in the dithered world
    if (config.monochrome) {
        const int barW = std::max(40, w / 5);
        fillRect(canvas, 0, 0, std::max(barW, textWidth("LAP 9/9", s)) + 8 * s, 30 * s, 0x0000);
        fillRect(canvas, w - textWidth("88TH", s * 2) - 8 * s, 0, textWidth("88TH", s * 2) + 8 * s, 29 * s, 0x0000);
        fillRect(canvas, w / 2 - 20 * s, h - 27 * s, 40 * s, 27 * s, 0x0000);
    }

    // Lap and time, top left
    const int lap = std::clamp(me.lapsCompleted + 1, 1, race.laps());
    snprintf(text, sizeof(text), "LAP %d/%d", lap, race.laps());
    drawText(canvas, 4 * s, 4 * s, text, s, WHITE);
    char timeText[16];
    formatTime(me.finished && me.finishTime < 1e8f ? me.finishTime : race.raceTime(), timeText, sizeof(timeText));
    drawText(canvas, 4 * s, 14 * s, timeText, s, GREY);

    // Boost tank
    const int barW = std::max(40, w / 5);
    fillRect(canvas, 4 * s, 24 * s, barW, 4 * s, rgb565(0x1B1F33));
    const uint16_t boostColour = me.boosting ? WHITE : (me.boostEnergy > 0.3f ? CYAN : RED);
    fillRect(canvas, 4 * s, 24 * s, (int)(barW * me.boostEnergy), 4 * s, boostColour);
    drawRect(canvas, 4 * s - 1, 24 * s - 1, barW + 2, 4 * s + 2, GREY);

    if (timeTrial()) {
        // Time trial, top right: this lap's time, and the best lap to beat
        const float lapTime = me.finished ? me.lastLapTime : std::max(0.0f, race.raceTime() - me.lapStartTime);
        formatTime(lapTime, text, sizeof(text));
        drawTextRight(canvas, w - 4 * s, 4 * s, text, s * 2, ACCENT);
        float best = records[recordKey()].lap;
        for (float t : lapTimes) {
            if (best <= 0.0f || t < best) best = t;
        }
        char bestText[16];
        formatTime(best, bestText, sizeof(bestText));
        snprintf(text, sizeof(text), "BEST %s", best > 0.0f ? bestText : "-:--.--");
        drawTextRight(canvas, w - 4 * s, 20 * s, text, s, GREY);
    } else {
        // Place, top right
        snprintf(text, sizeof(text), "%d%s", me.place, placeSuffix(me.place));
        drawTextRight(canvas, w - 4 * s, 4 * s, text, s * 2, ACCENT);
        snprintf(text, sizeof(text), "OF %d", race.racerCount());
        drawTextRight(canvas, w - 4 * s, 20 * s, text, s, GREY);
    }

    // Minimap below the place
    const int mapSize = std::min(w, h) / 4;
    drawMinimap(w - mapSize - 4 * s, 30 * s, mapSize);

    // Speed, bottom centre
    snprintf(text, sizeof(text), "%d", (int)(me.speed() * 3.6f));
    drawTextCentered(canvas, w / 2, h - 25 * s, text, s * 2, WHITE);
    drawTextCentered(canvas, w / 2, h - 9 * s, "KM/H", s, GREY);

    // Messages, centre
    const float t = race.raceTime();
    const int centreY = h / 3;
    if (t < 0.0f) {
        snprintf(text, sizeof(text), "%d", (int)std::ceil(-t));
        drawTextCentered(canvas, w / 2, centreY, text, s * 5, ACCENT);
    } else if (t < 1.0f) {
        drawTextCentered(canvas, w / 2, centreY, "GO!", s * 5, rgb565(0x80ED99));
    } else if (me.finished) {
        if (timeTrial()) {
            snprintf(text, sizeof(text), "FINISHED %s", timeText);
        } else {
            snprintf(text, sizeof(text), "FINISHED %d%s", me.place, placeSuffix(me.place));
        }
        drawTextCentered(canvas, w / 2, centreY, text, s * 2, ACCENT);
    } else if (me.mode == RacerMode::Falling || me.mode == RacerMode::Respawning) {
        drawTextCentered(canvas, w / 2, centreY, "FELL OFF!", s * 2, RED);
    } else if (me.wrongWayTime > 1.0f) {
        if (((int)(time * 4.0f)) % 2 == 0) drawTextCentered(canvas, w / 2, centreY, "WRONG WAY", s * 2, RED);
    } else if (messageTime > 0.0f) {
        drawTextCentered(canvas, w / 2, centreY, message.c_str(), s * 2, ACCENT);
    }
}

void Game::drawMinimap(int x, int y, int size) {
    Vec2 lo;
    Vec2 hi;
    track.bounds(lo, hi);
    const float span = std::max(hi.x - lo.x, hi.z - lo.z);
    if (span < 1.0f) return;
    const float scale = (float)(size - 4) / span;
    const float offsetX = (size - (hi.x - lo.x) * scale) * 0.5f;
    const float offsetZ = (size - (hi.z - lo.z) * scale) * 0.5f;
    auto map = [&](const Vec2& p, int& px, int& py) {
        px = x + (int)(offsetX + (p.x - lo.x) * scale);
        py = y + size - (int)(offsetZ + (p.z - lo.z) * scale); // +z is up on the map
    };
    blendRect(canvas, x, y, size, size, PANEL, 140);
    const int n = track.sampleCount();
    const int step = std::max(1, n / 120);
    for (int i = 0; i < n; i += step) {
        int x0, y0, x1, y1;
        map(track.sample(i).position, x0, y0);
        map(track.sample(i + step).position, x1, y1);
        drawLine(canvas, x0, y0, x1, y1, track.sample(i).gap ? RED : GREY);
    }
    for (const auto& racer : race.racers()) {
        if (racer.mode == RacerMode::Respawning) continue;
        int px, py;
        map(racer.position, px, py);
        const bool me = racer.id == localRacer;
        fillRect(canvas, px - (me ? 2 : 1), py - (me ? 2 : 1), me ? 5 : 3, me ? 5 : 3, me ? WHITE : rgb565(vehicles[racer.vehicle].colour));
    }
}

void Game::drawTouchControls() {
    Rect pause;
    int leftX, rightX, padY, padRadius, accelX, accelY, accelRadius, brakeX, brakeY, boostX, boostY, smallRadius;
    layoutTouchButtons(pause, leftX, rightX, padY, padRadius, accelX, accelY, accelRadius, brakeX, brakeY, boostX, boostY, smallRadius);
    const int s = textScale();
    auto button = [&](int cx, int cy, int r, bool down) {
        blendCircle(canvas, cx, cy, r, down ? RED : 0x0000, down ? 150 : 90);
        drawCircle(canvas, cx, cy, r, WHITE);
    };
    // Labels centred on their real height. Arrows are shifted towards their tip by a sixth of
    // their size: that puts the triangle's centroid, where it looks centred, on the circle's centre.
    auto label = [&](int cx, int cy, const char* text, int scale, uint16_t colour) {
        drawTextCentered(canvas, cx, cy - textHeight(scale) / 2, text, scale, colour);
    };
    const int arrowSize = padRadius;
    button(leftX, padY, padRadius, touchHeld & TouchLeft);
    fillArrow(canvas, leftX - arrowSize / 6, padY, arrowSize, 3, WHITE);
    button(rightX, padY, padRadius, touchHeld & TouchRight);
    fillArrow(canvas, rightX + arrowSize / 6, padY, arrowSize, 1, WHITE);
    button(accelX, accelY, accelRadius, touchHeld & TouchAccelerate);
    label(accelX, accelY, "GO", std::max(2, s * 2), WHITE);
    button(brakeX, brakeY, smallRadius, touchHeld & TouchBrake);
    label(brakeX, brakeY, "BRK", s, WHITE);
    button(boostX, boostY, smallRadius, touchHeld & TouchBoost);
    label(boostX, boostY, "BST", s, CYAN);
    // Pause
    blendRect(canvas, pause.x, pause.y, pause.w, pause.h, 0x0000, 90);
    drawRect(canvas, pause.x, pause.y, pause.w, pause.h, WHITE);
    const int barW = std::max(2, pause.w / 6);
    fillRect(canvas, pause.x + pause.w / 2 - barW - barW / 2, pause.y + pause.h / 4, barW, pause.h / 2, WHITE);
    fillRect(canvas, pause.x + pause.w / 2 + barW / 2, pause.y + pause.h / 4, barW, pause.h / 2, WHITE);
}

void Game::drawPause() {
    const int s = textScale();
    const int w = canvas.width;
    const int h = canvas.height;
    blendRect(canvas, 0, 0, w, h, 0x0000, 120);
    drawTextCentered(canvas, w / 2, h / 5, "PAUSED", s * 3, ACCENT);
    const char* labels[PauseRowCount] = { "RESUME", "RESTART", "QUIT TO MENU" };
    const int rowH = 14 * s;
    const int panelW = std::min(w - 8, 160 * s);
    const int x = (w - panelW) / 2;
    const int y0 = h / 5 + 30 * s;
    for (int row = 0; row < PauseRowCount; row++) {
        const int y = y0 + row * rowH;
        pauseRowRects[row] = { x, y, panelW, rowH };
        blendRect(canvas, x, y, panelW, rowH - 2, row == pauseRow ? ACCENT : PANEL, row == pauseRow ? 90 : 170);
        if (config.monochrome && row == pauseRow) {
            drawRect(canvas, x, y, panelW, rowH - 2, WHITE);
        }
        drawTextCentered(canvas, w / 2, y + (rowH - 2 - 7 * s) / 2, labels[row], s, row == pauseRow ? WHITE : GREY);
    }
}

void Game::drawResults() {
    const int s = textScale();
    const int w = canvas.width;
    const int h = canvas.height;
    blendRect(canvas, 0, 0, w, h, PANEL, 170);
    char text[48];
    if (timeTrial()) {
        // Every lap, the total and the records
        const int titleScale = h >= 200 * s ? s * 3 : s * 2;
        drawTextCentered(canvas, w / 2, 8 * s, "TIME TRIAL", titleScale, ACCENT);
        const int rowH = 10 * s;
        const int x = std::max(4, w / 2 - 90 * s);
        const int right = std::min(w - 4, w / 2 + 90 * s);
        int y = 8 * s + 7 * titleScale + 6 * s;
        size_t bestLap = 0;
        for (size_t i = 1; i < lapTimes.size(); i++) {
            if (lapTimes[i] < lapTimes[bestLap]) bestLap = i;
        }
        for (size_t i = 0; i < lapTimes.size(); i++) {
            const uint16_t colour = i == bestLap ? ACCENT : WHITE;
            snprintf(text, sizeof(text), "LAP %d", (int)i + 1);
            drawText(canvas, x, y, text, s, colour);
            formatTime(lapTimes[i], text, sizeof(text));
            drawTextRight(canvas, right, y, text, s, colour);
            y += rowH;
        }
        const Racer& me = race.racer(localRacer);
        drawText(canvas, x, y + 2 * s, "TOTAL", s, WHITE);
        formatTime(me.finishTime < 1e8f ? me.finishTime : 0.0f, text, sizeof(text));
        drawTextRight(canvas, right, y + 2 * s, text, s, WHITE);
        y += rowH + 6 * s;
        const Record& record = records[recordKey()];
        char recordText[16];
        formatTime(record.total, recordText, sizeof(recordText));
        snprintf(text, sizeof(text), newRecordTotal ? "NEW RECORD %s" : "RECORD %s", recordText);
        drawTextCentered(canvas, w / 2, y, text, s, newRecordTotal ? rgb565(0x80ED99) : GREY);
        formatTime(record.lap, recordText, sizeof(recordText));
        snprintf(text, sizeof(text), newRecordLap ? "NEW BEST LAP %s" : "BEST LAP %s", recordText);
        drawTextCentered(canvas, w / 2, y + rowH, text, s, newRecordLap ? rgb565(0x80ED99) : GREY);
        if (((int)(time * 2.0f)) % 2 == 0) {
            drawTextCentered(canvas, w / 2, h - 14 * s, touchControlsVisible ? "TAP TO CONTINUE" : "PRESS ANY KEY", s, WHITE);
        }
        return;
    }
    drawTextCentered(canvas, w / 2, 8 * s, "RESULTS", s * 3, ACCENT);
    std::vector<const Racer*> order;
    for (const auto& racer : race.racers()) order.push_back(&racer);
    std::sort(order.begin(), order.end(), [](const Racer* a, const Racer* b) { return a->place < b->place; });
    const int rowH = 11 * s;
    const int x = std::max(4, w / 2 - 120 * s);
    const int right = std::min(w - 4, w / 2 + 120 * s);
    int y = 8 * s + 7 * s * 3 + 8 * s;
    for (const Racer* racer : order) {
        const bool me = racer->id == localRacer;
        const uint16_t colour = me ? ACCENT : WHITE;
        snprintf(text, sizeof(text), "%d%s", racer->place, placeSuffix(racer->place));
        drawText(canvas, x, y, text, s, colour);
        drawText(canvas, x + 30 * s, y, racer->name.c_str(), s, colour);
        if (racer->finishTime < 1e8f) {
            formatTime(racer->finishTime, text, sizeof(text));
        } else {
            snprintf(text, sizeof(text), "--:--.--");
        }
        drawTextRight(canvas, right, y, text, s, colour);
        y += rowH;
    }
    const Racer& me = race.racer(localRacer);
    if (me.bestLapTime > 0.0f) {
        char lapText[16];
        formatTime(me.bestLapTime, lapText, sizeof(lapText));
        snprintf(text, sizeof(text), "BEST LAP %s", lapText);
        drawTextCentered(canvas, w / 2, y + 4 * s, text, s, GREY);
    }
    if (((int)(time * 2.0f)) % 2 == 0) {
        drawTextCentered(canvas, w / 2, h - 14 * s, touchControlsVisible ? "TAP TO CONTINUE" : "PRESS ANY KEY", s, WHITE);
    }
}

// endregion

} // namespace hd
