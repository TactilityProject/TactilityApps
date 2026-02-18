#pragma once

#include <tt_app.h>
#include <lvgl.h>
#include <TactilityCpp/App.h>

class Magic8Ball final : public App {

private:
    // UI pointers (nulled in onHide)
    lv_obj_t* answerLabel = nullptr;
    lv_obj_t* hintLabel = nullptr;
    lv_obj_t* ballObj = nullptr;

    // State
    int lastIdx = -1;
    bool seeded = false;

    void revealAnswer();

    static void onBallClick(lv_event_t* e);
    static void onKey(lv_event_t* e);

public:
    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
};
