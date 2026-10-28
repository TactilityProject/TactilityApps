#pragma once

#include <app/instance.h>
#include <lvgl.h>
#include <lvgl/devices/pointer.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/device.h>
#include <tactility/drivers/bluetooth.h>
#include <tactility/drivers/usb_midi_device.h>
#include <atomic>
#include <cstdint>
#include <memory>

#include "MidiScales.h"
#include "Sequencer.h"
#include "UiKit.h"

// ---------------------------------------------------------------------------
// Transport selection - which MIDI output carries sendMidi()'s bytes
// ---------------------------------------------------------------------------

enum class MidiTransport {
    BLE = 0,
    USB = 1,
};

// ---------------------------------------------------------------------------
// Mode enum
// ---------------------------------------------------------------------------

enum class MidiMode {
    HOME       = -1,
    KEYS       = 0,
    BEATS      = 1,
    ZEN        = 2,
    DROP       = 3,
    RNG        = 4,
    XY         = 5,
    ARP        = 6,
    GRID       = 7,
    CHORD      = 8,
    LFO        = 9,
    RAGA       = 10,
    DIMENSIONS = 11,
    BABY8      = 12,
    TB3PO      = 13,
    EUCLID     = 14,
    COUNT      = 15,
};

// ---------------------------------------------------------------------------
// Forward declarations for per-view build functions (each in ViewXxx.cpp)
// ---------------------------------------------------------------------------

class MidiController;
void buildKeysView      (MidiController* mc, lv_obj_t* parent);
void buildBeatsView     (MidiController* mc, lv_obj_t* parent);
void buildXYPadView     (MidiController* mc, lv_obj_t* parent);
void buildArpView       (MidiController* mc, lv_obj_t* parent);
void buildGridView      (MidiController* mc, lv_obj_t* parent);
void buildChordView     (MidiController* mc, lv_obj_t* parent);
void buildLfoView       (MidiController* mc, lv_obj_t* parent);
void buildZenView       (MidiController* mc, lv_obj_t* parent);
void buildDropView      (MidiController* mc, lv_obj_t* parent);
void buildRngView       (MidiController* mc, lv_obj_t* parent);
void buildRagaView      (MidiController* mc, lv_obj_t* parent);
void buildDimensionsView(MidiController* mc, lv_obj_t* parent);
void buildBaby8View     (MidiController* mc, lv_obj_t* parent);
void buildTb3poView     (MidiController* mc, lv_obj_t* parent);
void buildEuclidView        (MidiController* mc, lv_obj_t* parent);
void euclidOnEncoderUpdate  (MidiController* mc);  // regen patterns + refresh UI after knob change

// Encoder action entry points - implemented in each View*.cpp
void ragaTogglePlay    (MidiController* mc);
void ragaToggleDrone   (MidiController* mc);
void dimTogglePlay     (MidiController* mc);
void dimReset          (MidiController* mc);
void dimChangeOctave   (MidiController* mc, int delta);
void dimChangeRandom   (MidiController* mc, int delta);
void tb3TogglePlay     (MidiController* mc);
void tb3ChangeScale    (MidiController* mc, int delta);
void tb3Regen          (MidiController* mc);
void tb3Redraw         (MidiController* mc);     // repaint piano roll after key/octave changes
void baby8UpdateStepColors(MidiController* mc);  // refresh step colours after an encoder gate change

// 8Encoder hardware support (EncoderInput.cpp)
void encoderInit  (MidiController* mc);  // detect hardware, start poll timer
void encoderStop  (MidiController* mc);  // stop poll timer, turn LEDs off
void encoderSetLed(MidiController* mc, uint8_t idx, uint32_t rgb);  // set one LED
void encoderSetAllLeds(MidiController* mc, uint32_t rgb);            // set all LEDs same color

// ---------------------------------------------------------------------------
// MidiController
// ---------------------------------------------------------------------------

class MidiController final {
public:
    // window_manager_create()'s WindowCreateWidgetsFn - userData is the MidiController* for this instance.
    static void createWidgets(lv_obj_t* parent, void* userData);

    // Call once, after the window has been torn down.
    void teardown();

    // Send a 3-byte MIDI message via BLE MIDI (no-op if not connected)
    void sendMidi(uint8_t cmd, uint8_t d1, uint8_t d2);

    // Send Note Off for all 128 notes on channel 1
    void stopAllNotes();

    // Switch to a mode (or HOME). Stops timers, sends All-Notes-Off, swaps view.
    void switchToMode(MidiMode mode);

    // BLE state. Written by the app's own task, read by sendMidi() on any task.
    std::atomic<bool> bleRunning_ {false};

    // Called by main() on the app's own task. BLE and USB MIDI start and stop there, as they do
    // flash and NVS work, which the LVGL task can't do: its stack is in external RAM.
    // attachEventGroup() claims the bit that the LVGL task signals transport requests with.
    void attachEventGroup(struct TaskEventGroup* group);
    // Restores the transport chosen in an earlier session (selection only, not its on/off state).
    // Call before detectBleState() so a BLE session that is already running still wins.
    void loadSettings();
    // Picks up a BLE MIDI session that is already running, before the window is created
    void detectBleState();
    // Handles the pending transport request and Bluetooth events. Call when the event group wakes.
    void processEvents();

    // Active mode timers - each view creates its own, stored here so
    // switchToMode() can delete them before swapping the view tree.
    lv_timer_t* arpTimer_     = nullptr;
    lv_timer_t* lfoTimer_     = nullptr;
    lv_timer_t* zenTimer_     = nullptr;
    lv_timer_t* dropTimer_    = nullptr;
    lv_timer_t* rngTimer_     = nullptr;
    lv_timer_t* ragaTimer_    = nullptr;
    lv_timer_t* dimTimer_     = nullptr;
    lv_timer_t* baby8Timer_   = nullptr;
    lv_timer_t* tb3poTimer_   = nullptr;
    lv_timer_t* euclidTimer_  = nullptr;
    lv_timer_t* encoderTimer_ = nullptr;  // 8Encoder/LCD poll (100 ms)

    // M5Stack 8Encoder state (I2C address 0x41, optional hardware)
    struct EncoderState {
        bool    present       = false;   // true if detected at 0x41 on startup
        int32_t counter[8]    = {};      // last read absolute counter per knob
        bool    btnPrev[8]    = {};      // previous button state for edge detection
        bool    btnSeedNext   = false;  // if true, next poll seeds btnPrev without firing actions
        // LED colors per knob (0xRRGGBB, 0 = off)
        uint32_t ledColor[8]  = {};
    } encoder_;

    // Drum sequencer (owned here, accessed by ViewBeats)
    std::unique_ptr<Sequencer> seq_;
    std::atomic<int> seqPlayStep{-1};
    std::atomic<int> seqLastStep{-1};  // previous step for highlight refresh

    // Singleton for static callbacks
    static MidiController* s_instance;

    // Current view root - set by each buildXxxView, deleted in switchToMode
    lv_obj_t* currentViewRoot_ = nullptr;

    // -----------------------------------------------------------------------
    // Per-mode state (inner structs - widget pointers nulled by switchToMode/teardown)
    // -----------------------------------------------------------------------

    struct KeysState {
        int octave = 4, scale = 0, key = 0;
        // One "currently sounding note" per touch slot (see lvgl_pointer_get_slot_index() in
        // ViewKeys.cpp's cbPianoPressing/cbPianoReleased) rather than a single lastNote - lets
        // multiple fingers each independently slide/hold their own key. -1 = no note held in
        // that slot. Explicitly initialized (not just `= {}`, which would zero-init to note 0 -
        // a real playable note, not "empty") both here and again in resetAllState(). The literal
        // count below must track LVGL_POINTER_MAX_SLOTS - static_assert catches drift.
        int lastNote[LVGL_POINTER_MAX_SLOTS] = { -1, -1, -1, -1, -1 };
        static_assert(LVGL_POINTER_MAX_SLOTS == 5, "update the lastNote initializer above to match");
        // Single octave visible at a time (OCT-/OCT+ shifts it), like SoundBench.
        lv_obj_t* pianoArea      = nullptr;
        lv_obj_t* whiteKey[7]    = {};
        lv_obj_t* blackKey[5]    = {};
        lv_obj_t* statusLbl      = nullptr;
        lv_obj_t* scaleLbl       = nullptr;
        lv_obj_t* keyLbl         = nullptr;
        lv_obj_t* octLbl         = nullptr;
    } keys_;

    struct BeatsState {
        int bpm = 120;
        bool playing = false;
        lv_obj_t* stepBtn[Sequencer::NUM_TRACKS][Sequencer::NUM_STEPS] = {};
        lv_obj_t* playBtn  = nullptr;
        lv_obj_t* playLbl  = nullptr;
        lv_obj_t* bpmLbl   = nullptr;
        // per-track note-off scheduling (filled in onStep callback)
        uint64_t noteOffAtUs[Sequencer::NUM_TRACKS] = {};
    } beats_;

    struct XYState {
        int xCC = 74, yCC = 71;
        int xVal = 64, yVal = 64;
        lv_obj_t* canvas       = nullptr;
        lv_obj_t* indicator    = nullptr;  // circle widget tracking touch position
        lv_obj_t* xCCLbl       = nullptr;
        lv_obj_t* yCCLbl       = nullptr;
        lv_obj_t* valLbl       = nullptr;
        // canvas pixel dimensions (set after layout)
        int canvasW = 0, canvasH = 0;
    } xy_;

    struct ArpState {
        int bpm = 120;
        int speedIdx = 1;  // 0=quarter 1=eighth 2=sixteenth 3=32nd
        int pattern = 0;   // 0=UP 1=DOWN 2=UPDN 3=RAND 4=CHANCE
        int chordType = 0; // 0=MAJ 1=MIN 2=7TH
        int octaveRange = 2;
        int pianoOctave = 4;
        int triggeredNote = -1;  // root note being arped (-1=stopped)
        bool isPlaying = false;
        int arpStep = 0;
        int arpDir  = 1;   // +1 or -1 for UPDN
        lv_obj_t* pianoKey[12]   = {};
        lv_obj_t* statusLbl      = nullptr;
        lv_obj_t* noteLbl        = nullptr;
        lv_obj_t* patternLbl     = nullptr;
        lv_obj_t* chordLbl       = nullptr;
        lv_obj_t* speedLbl       = nullptr;
        lv_obj_t* bpmLbl         = nullptr;
        lv_obj_t* octLbl         = nullptr;
        lv_obj_t* pianoOctLbl    = nullptr;
        int lastNote = -1;  // for note-off
    } arp_;

    struct GridState {
        int octave = 3;
        // One held note per touch slot, same pattern as KeysState::lastNote - lets multiple
        // fingers each hold their own grid cell instead of the last touch stealing the others'.
        // -1 = no note held in that slot.
        int pressedNote[LVGL_POINTER_MAX_SLOTS] = { -1, -1, -1, -1, -1 };
        static_assert(LVGL_POINTER_MAX_SLOTS == 5, "update the pressedNote initializer above to match");
        lv_obj_t* cell[5][8] = {};
        lv_obj_t* octLbl     = nullptr;
        lv_obj_t* noteLbl    = nullptr;
    } grid_;

    struct ChordState {
        int octave = 4;
        int scale  = 0;
        int key    = 0;
        bool chordPlaying[8] = {};
        // up to 4 notes per chord
        int chordNotes[8][4] = {};
        lv_obj_t* chordBtn[8] = {};
        lv_obj_t* octLbl     = nullptr;
        lv_obj_t* scaleLbl   = nullptr;
    } chord_;

    struct LfoState {
        float rate    = 1.0f;   // Hz
        int amount    = 64;     // 0-127 scale factor
        int ccTarget  = 1;      // CC number (0-127) when not in pitch mode
        bool pitchMode = false;
        bool running   = false;
        int waveform   = 0;    // 0=SINE 1=TRI 2=SQR 3=SAW
        float phase    = 0.0f; // 0.0 - 1.0
        int lastValue  = 64;
        uint64_t lastUpdateUs = 0;
        int updateCounter = 0; // throttle label updates
        lv_obj_t* canvas      = nullptr;
        lv_obj_t* statusLbl   = nullptr;
        lv_obj_t* rateLbl     = nullptr;
        lv_obj_t* amountLbl   = nullptr;
        lv_obj_t* ccLbl       = nullptr;
        lv_obj_t* waveLbl     = nullptr;
        lv_obj_t* valueLbl    = nullptr;
        lv_obj_t* startBtn    = nullptr;
        lv_obj_t* startLbl    = nullptr;
        int canvasW = 0, canvasH = 0;
    } lfo_;

    struct ZenState {
        struct Ball {
            float x, y, vx, vy;
            int   radius;
            uint32_t color;
            bool active = false;
        };
        static constexpr int MAX_BALLS = 4;
        Ball balls[MAX_BALLS] = {};
        lv_obj_t* ballObj[MAX_BALLS] = {};  // LVGL circle widgets for each ball
        int numBalls = 0;
        int scale = 0, key = 0, octave = 4;
        // Wall flash timers (ms, 0 = no flash)
        uint32_t wallFlashUntilMs[24] = {};  // TOP_SEGS(8)+BOTTOM_SEGS(8)+SIDE_SEGS*2(8) = 24
        // Pending note-offs: note value + time to send (0 = empty slot)
        static constexpr int MAX_PENDING_OFFS = 8;
        uint8_t  pendingOffNote[MAX_PENDING_OFFS] = {};
        uint32_t pendingOffMs  [MAX_PENDING_OFFS] = {};
        lv_obj_t* canvas    = nullptr;
        int canvasW = 0, canvasH = 0;
        lv_obj_t* statusLbl = nullptr;
        lv_obj_t* ballCntLbl = nullptr;
        lv_obj_t* octLbl    = nullptr;
    } zen_;

    struct DropState {
        struct DropBall {
            float x, y, vx, vy;
            bool active = false;
            uint32_t spawnedMs = 0;
        };
        struct Platform {
            float x, y, w;
            float angle;  // radians, tilt
            int note;
        };
        static constexpr int MAX_BALLS     = 8;
        static constexpr int MAX_PLATFORMS = 6;
        DropBall  balls[MAX_BALLS]         = {};
        lv_obj_t* ballObj[MAX_BALLS]       = {};  // LVGL circle widgets
        lv_obj_t* platLbl[MAX_PLATFORMS]   = {};  // LVGL label widgets on canvas
        Platform  platforms[MAX_PLATFORMS] = {};
        int numPlatforms = 0;
        int scale = 0, key = 0, octave = 4;
        bool editMode = false;
        lv_obj_t* canvas    = nullptr;
        int canvasW = 0, canvasH = 0;
        lv_obj_t* statusLbl = nullptr;
        lv_obj_t* modeLbl   = nullptr;  // shows "DROP" or "EDIT"
        lv_obj_t* octLbl    = nullptr;
        lv_obj_t* modeBtn   = nullptr;
    } drop_;

    struct RngState {
        int bpm         = 120;
        int subdivision = 0;   // index into SUBDIV_NAMES: 0=1/4 1=1/8 2=1/16
        int minOctave   = 3;
        int maxOctave   = 6;
        int probability = 50;  // 0-100%
        int scaleIdx    = 0;
        int rootKey     = 0;   // 0-11
        bool playing    = false;
        int  currentNote = -1;
        uint64_t nextNoteUs  = 0;
        uint64_t noteOffUs   = 0;
        lv_obj_t* noteLbl    = nullptr;
        lv_obj_t* playBtn    = nullptr;
        lv_obj_t* playLbl    = nullptr;
        lv_obj_t* bpmLbl     = nullptr;
        lv_obj_t* probLbl    = nullptr;
        lv_obj_t* scaleLbl   = nullptr;
        lv_obj_t* keyLbl     = nullptr;
        lv_obj_t* minOctLbl  = nullptr;
        lv_obj_t* maxOctLbl  = nullptr;
        lv_obj_t* subdivLbl  = nullptr;
    } rng_;

    // -----------------------------------------------------------------------
    // New mode states
    // -----------------------------------------------------------------------

    struct RagaState {
        int raga     = 0;   // 0-7
        int tala     = 0;   // 0-2
        int rootNote = 48;
        int bpm      = 120;
        bool playing  = false;
        bool droneOn  = false;
        // Markov / phrase state
        int8_t phrase[128] = {};
        int phraseLen = 0, phraseIdx = 0;
        int8_t history[2] = {0, 0};  // 2nd-order Markov history
        uint32_t nextNoteMs  = 0;
        uint32_t noteOffMs   = 0;
        bool noteActive      = false;
        uint8_t currentNote  = 0;
        bool droneActive     = false;
        uint8_t droneNote    = 0;
        int talaBeat         = 0;
        uint32_t noteInterval = 150;
        uint32_t noteDuration = 120;
        // UI
        lv_obj_t* ragaLbl    = nullptr;
        lv_obj_t* talaLbl    = nullptr;
        lv_obj_t* rootLbl    = nullptr;
        lv_obj_t* bpmLbl     = nullptr;
        lv_obj_t* statusLbl  = nullptr;
        lv_obj_t* playBtn    = nullptr;
        lv_obj_t* playLbl    = nullptr;
        lv_obj_t* droneBtn   = nullptr;
        lv_obj_t* droneLbl   = nullptr;
        lv_obj_t* ragaBigLbl = nullptr;  // right-side display
        lv_obj_t* talaBigLbl = nullptr;
    } raga_;

    struct DimensionsState {
        int   equation   = 1;      // 1-20
        float a = 50, b = 50, c = 50, d = 50;
        float t  = 0.0f;
        float t2 = 100.0f;
        float gran = 1.0f;        // t step per tick
        float rnd  = 0.0f;
        float xOff = 64.0f, yOff = 64.0f;
        float px = 0, py = 0, pz = 0;
        int   octave  = 0;
        int   bpm     = 120;
        bool  playing = false;
        int   lastNote = -1;
        uint32_t tickMs = 0;       // next tick timestamp
        uint32_t noteOffMs = 0;
        // UI
        lv_obj_t* eqLbl    = nullptr;
        lv_obj_t* aLbl     = nullptr;
        lv_obj_t* bLbl     = nullptr;
        lv_obj_t* cLbl     = nullptr;
        lv_obj_t* dLbl     = nullptr;
        lv_obj_t* outLbl   = nullptr;  // "px:N  py:N  pz:N  t:N"
        lv_obj_t* octLbl   = nullptr;
        lv_obj_t* rndLbl   = nullptr;
        lv_obj_t* bpmLbl   = nullptr;
        lv_obj_t* playLbl  = nullptr;
    } dim_;

    struct Baby8State {
        struct Step { int note = 60; int velocity = 100; bool gate = true; };
        Step steps[8]          = {};
        Step patterns[8][8]    = {};
        int  currentStep  = 0;
        int  selectedStep = 0;
        bool playing      = false;
        int  bpm          = 120;
        int  swing        = 50;    // 50-80
        int  patternIdx   = 0;
        int  lastNote     = -1;
        uint32_t nextStepMs = 0;
        // UI
        lv_obj_t* stepBtn[8]  = {};
        lv_obj_t* noteLbl[8]  = {};
        lv_obj_t* bpmLbl      = nullptr;
        lv_obj_t* swingLbl    = nullptr;
        lv_obj_t* patLbl      = nullptr;
        lv_obj_t* editNoteLbl = nullptr;
        lv_obj_t* editVelLbl  = nullptr;
        lv_obj_t* playLbl     = nullptr;
    } baby8_;

    struct Tb3poState {
        struct Step {
            int  note     = 0;
            bool oct_up   = false, oct_down = false;
            bool accent   = false, slide    = false, gate = false;
        };
        Step     steps[32]    = {};
        uint32_t seed         = 12345;
        int      density      = 8;   // 0-16
        int      scale        = 0;
        int      key          = 0;
        int      octave       = 3;
        int      currentStep  = 0;
        bool     playing      = false;
        int      bpm          = 120;
        int      lastNote     = -1;
        int      lastNoteStep = -1;  // step that owns lastNote (slide bookkeeping)
        uint32_t nextStepMs   = 0;
        uint32_t noteOffMs    = 0;
        // UI
        lv_obj_t* canvas      = nullptr;
        lv_obj_t* densityLbl  = nullptr;
        lv_obj_t* seedLbl     = nullptr;
        lv_obj_t* bpmLbl      = nullptr;
        lv_obj_t* scaleLbl    = nullptr;
        lv_obj_t* keyLbl      = nullptr;
        lv_obj_t* playLbl     = nullptr;
        int canvasW = 0, canvasH = 0;
    } tb3po_;

    struct EuclidState {
        struct Voice {
            int  steps    = 16;
            int  events   = 4;
            int  rotation = 0;
            int  note     = 36;
            bool pattern[64] = {};
        };
        Voice voice[8]       = {};
        uint32_t currentStep = 0;  // free-running master clock (each voice uses modulo its own steps)
        bool  playing        = false;
        int   bpm            = 120;
        bool  triplet        = false;
        int   selectedVoice  = 0;
        uint32_t nextStepMs  = 0;
        // UI
        lv_obj_t* canvas     = nullptr;
        lv_obj_t* bpmLbl     = nullptr;
        lv_obj_t* stepsLbl   = nullptr;
        lv_obj_t* eventsLbl  = nullptr;
        lv_obj_t* rotLbl     = nullptr;
        lv_obj_t* noteLbl    = nullptr;
        lv_obj_t* voiceLbl    = nullptr;
        lv_obj_t* playLbl     = nullptr;
        lv_obj_t* tripLbl      = nullptr;  // TRIP ON / TRIP OFF
        lv_obj_t* voiceTabRow = nullptr;  // tab button row - for encoder voice-select highlight
        int canvasW = 0, canvasH = 0;
    } euclid_;

    // Current mode - read by EncoderInput.cpp for per-mode dispatch
    MidiMode currentMode_ = MidiMode::HOME;

public:
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

private:
    lv_obj_t* toolbar_        = nullptr;
    lv_obj_t* transportBtn_   = nullptr;  // toolbar button: current transport - opens the selector
    lv_obj_t* transportIconLbl_ = nullptr;  // transport icon inside that button
    lv_obj_t* transportOverlay_ = nullptr;  // full-content transport chooser (null when closed)
    lv_obj_t* mainWrapper_    = nullptr;
    lv_obj_t* contentArea_    = nullptr;
    lv_obj_t* switchWidget_   = nullptr;
    lv_timer_t* uiTimer_      = nullptr;  // 20ms BEATS step-cursor/note-off refresh

    // Which transport sendMidi() targets. Chosen in the transport selector before the switch is
    // turned on; only one of bleRunning_/usbRunning_ is ever true.
    MidiTransport activeTransport_ = MidiTransport::BLE;
    // True once a transport has been chosen, either from the selector this session or from the
    // settings file. When set, the app opens the home view instead of the chooser on launch.
    bool hasSavedTransport_ = false;

    // BLE radio management (MediaKeys pattern)
    struct Device* btDevice_  = nullptr;
    bool radioWasOff_         = false;  // true if we turned the radio on (must turn it back off)
    bool radioEnabling_       = false;  // true while waiting for BT_RADIO_STATE_ON

    // The app's event group, owned by main(). Bluetooth events and transport requests wake its loop.
    struct TaskEventGroup* eventGroup_ = nullptr;
    uint32_t transportRequestBit_ = 0;
    // -1: none, 0: turn the transport off, 1: turn it on. Set by the LVGL task, taken by the app's task.
    std::atomic<int> transportRequest_ {-1};
    struct BtEventSubscription btEventSub_ {};
    bool btEventSubscribed_   = false;

    // USB MIDI state - no radio-enable dance needed, claim/release is synchronous.
    std::atomic<bool> usbRunning_ {false};

    void handleTransportToggle(bool enabled);  // LVGL task: updates the UI and requests the transport change
    void applyTransport(bool enabled);         // App task: starts or stops the transport
    void failTransportStart();                 // Switches the UI back to off
    void enableTransportButton();
    void startBleMidi();   // called once radio is confirmed ON
    void stopBleMidi();    // sends all-notes-off and stops BLE MIDI
    void startUsbMidi();
    void stopUsbMidi();    // sends all-notes-off and stops USB MIDI

    void showTransportSelect();  // opens the BLE/USB chooser over the current view
    void updateTransportButton();  // refresh the toolbar button label
    lv_obj_t* buildTransportSelectOverlay(lv_obj_t* parent);
    // Queues the chosen transport for the kernel file_ops task (this is called from the LVGL task).
    void saveTransportSelection();

    void createWidgetsImpl(lv_obj_t* parent);
    void buildHomeView(lv_obj_t* parent);
    void stopAllModeTimers();
    void resetAllState();

    void handleBtEvent(const struct BtEvent& event);
    void pollBtEvents();
    void subscribeBtEvents();
    void unsubscribeBtEvents();

    static void cbUiTimer(lv_timer_t* t);
    static void cbModeBtn(lv_event_t* e);
    static void cbTransportSwitch(lv_event_t* e);
    static void cbTransportSelect(lv_event_t* e);  // toolbar transport button
    static void cbTransportCard(lv_event_t* e);    // card inside the transport chooser
    static void cbTransportCancel(lv_event_t* e);
};
