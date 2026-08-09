#pragma once

#include <lvgl.h>
#include <vector>

struct Context;
struct Device;

struct ConnectViewState {
    std::vector<Device*> uartDevices;
    lv_obj_t* busDropdown = nullptr;
    lv_obj_t* speedTextarea = nullptr;
};

/** Builds the connect form into @a parent. On success (Connect pressed, UART opened), calls
 *  showConsoleView(app, dev). */
void connectViewCreate(lv_obj_t* parent, Context* app);

/** Persists the current bus/speed selection to preferences. */
void connectViewStop(Context* app);
