#pragma once
#include "TestViewBase.h"
#include <UnitMidi.h>

struct Context;

struct TestUnitMidi {
    Context* app_         = nullptr;
    UnitMidi unit_;
    lv_obj_t* lblStatus_  = nullptr;
    lv_obj_t* lblChannel_ = nullptr;
    lv_obj_t* lblProgram_ = nullptr;
    uint8_t   channel_    = 0;
    uint8_t   program_    = 0;
    uint8_t   note_       = 60;  // middle C
    bool      notePlaying_= false;
};

void testUnitMidiStart(TestUnitMidi* self, lv_obj_t* parent, Context* app);
void testUnitMidiStop(TestUnitMidi* self);
