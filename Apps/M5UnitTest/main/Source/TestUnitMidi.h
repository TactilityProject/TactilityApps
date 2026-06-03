#pragma once
#include "TestViewBase.h"
#include <UnitMidi.h>

class TestUnitMidi final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
    UnitMidi unit_;
    lv_obj_t* lblStatus_  = nullptr;
    lv_obj_t* lblChannel_ = nullptr;
    lv_obj_t* lblProgram_ = nullptr;
    uint8_t   channel_    = 0;
    uint8_t   program_    = 0;
    uint8_t   note_       = 60;  // middle C
    bool      notePlaying_= false;

    // UART device name for MIDI unit (adjust to match your board wiring)
    static constexpr const char* UART_DEVICE = "uart1";

    static void onNoteOnClicked(lv_event_t* e);
    static void onNoteOffClicked(lv_event_t* e);
    static void onChUp(lv_event_t* e);
    static void onChDown(lv_event_t* e);
    static void onProgUp(lv_event_t* e);
    static void onProgDown(lv_event_t* e);
    void updateLabels();
};
