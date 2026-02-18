/**
 * @file Breakout.h
 * @brief Breakout arcade game for Tactility
 */
#pragma once

#include <tt_app.h>
#include <lvgl.h>
#include <TactilityCpp/App.h>

class SfxEngine;

enum class GameState { Ready, Playing, Paused, GameOver };
enum class BrickType : uint8_t { Normal, Silver, Gold };
enum class PowerUpType : uint8_t { Laser, Extend, Catch, Slow, BreakOut, Split, ExtraLife };

static constexpr int TICK_MS = 25;
static constexpr int MAX_COLS = 12;
static constexpr int MAX_ROWS = 5;
static constexpr int MAX_BRICKS = MAX_COLS * MAX_ROWS;
static constexpr int INITIAL_LIVES = 10;
static constexpr int MAX_CAPSULES = 4;
static constexpr int MAX_BALLS = 3;
static constexpr int MAX_LASERS = 2;

// Per-color scoring (Purple=50 through Yellow=120)
static constexpr int COLOR_SCORES[] = { 50, 60, 70, 80, 90, 100, 110, 120 };

struct Capsule {
    float x, y;
    PowerUpType type;
    bool active;
};

struct BallState {
    float x, y, vx, vy;
    bool active;
    lv_obj_t* obj;
};

struct Laser {
    float x, y;
    bool active;
    lv_obj_t* obj;
};

class Breakout final : public App {

private:
    // UI pointers (nulled in onHide)
    lv_obj_t* gameArea = nullptr;
    lv_obj_t* paddle = nullptr;
    lv_obj_t* bricks[MAX_BRICKS] = {};
    lv_obj_t* scoreLabel = nullptr;
    lv_obj_t* livesLabel = nullptr;
    lv_obj_t* messageLabel = nullptr;
    lv_timer_t* gameTimer = nullptr;
    lv_obj_t* soundBtnIcon = nullptr;

    // Capsule UI
    lv_obj_t* capsuleObjs[MAX_CAPSULES] = {};
    lv_obj_t* capsuleLabels[MAX_CAPSULES] = {};

    // Laser UI
    Laser lasers[MAX_LASERS] = {};

    // BreakOut exit
    lv_obj_t* exitIndicator = nullptr;

    // Sfx
    SfxEngine* sfxEngine = nullptr;

    // Game state (persists across hide/show)
    GameState state = GameState::Ready;
    int score = 0;
    int lives = INITIAL_LIVES;
    int level = 1;
    int bricksRemaining = 0;
    int destroyedCount = 0;
    bool brickAlive[MAX_BRICKS] = {};
    BrickType brickType[MAX_BRICKS] = {};
    int brickHits[MAX_BRICKS] = {};
    int brickColorIndex[MAX_BRICKS] = {};
    bool needsInit = true;

    // Multi-ball
    BallState balls[MAX_BALLS] = {};
    int activeBallCount = 1;

    // Capsules
    Capsule capsules[MAX_CAPSULES] = {};
    int capsuleW = 0, capsuleH = 0;
    float capsuleFallSpeed = 0;

    // Power-up state
    bool catchActive = false;
    int catchBallIndex = -1;
    float catchOffsetX = 0;
    int catchAutoReleaseTicks = 0;

    float originalBallSpeed = 0;
    int slowRecoveryTicks = 0;

    int originalPaddleW = 0;
    bool extendActive = false;

    bool laserActive = false;
    int laserCooldown = 0;
    int laserW = 0, laserH = 0;
    float laserSpeed = 0;

    bool exitOpen = false;

    float baseBallSpeed = 0;

    // Paddle
    float paddleX = 0;

    // Layout dimensions (calculated in onShow)
    int areaW = 0, areaH = 0;
    int cols = 0, rows = 0;
    int brickW = 0, brickH = 0;
    int brickGap = 0;
    int brickOffsetX = 0, brickOffsetY = 0;
    int ballSize = 0;
    int paddleW = 0, paddleH = 0;
    int paddleYPos = 0;
    float ballSpeed = 0;
    float paddleSpeed = 0;

    // Static callbacks
    static void onTick(lv_timer_t* timer);
    static void onPressed(lv_event_t* e);
    static void onClicked(lv_event_t* e);
    static void onKey(lv_event_t* e);
    static void onFocused(lv_event_t* e);
    static void onPauseClicked(lv_event_t* e);
    static void onSoundToggled(lv_event_t* e);

    // Game logic
    void startGame();
    void nextLevel();
    void resetBall();
    void launchBall();
    void update();
    void checkLaserBrickCollisions();
    void loseLife();
    void winLevel();
    void createBricks();
    void setupLevelPattern();
    void refreshBricks();
    void updateScoreDisplay();
    void updateMessage();
    void togglePause();
    void updateSoundIcon();

    // Capsule system
    void spawnCapsule(float x, float y);
    void updateCapsules();
    void activatePowerUp(PowerUpType type);
    void clearPowerUps();
    void createCapsuleObjs();

    // Multi-ball
    void updateBalls();
    void splitBalls();

    // Laser
    void updateLasers();
    void fireLaser();
    void createLaserObjs();

    // BreakOut exit
    void openExit();
    void closeExit();

    // Brick helpers
    void hitBrick(int idx);
    int scoreBrick(int idx);

public:
    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
};
