#pragma once

#include <Tactility/RecursiveMutex.h>
#include <Tactility/Thread.h>
#include <lvgl.h>
#include <cstdint>
#include <memory>
#include <string>

struct Context;
struct Device;

constexpr size_t receiveBufferSize = 512;
constexpr size_t renderBufferSize = receiveBufferSize + 2; // Leave space for newline at split and null terminator at the end

struct ConsoleViewState {
    lv_obj_t* parent = nullptr;
    lv_obj_t* logTextarea = nullptr;
    lv_obj_t* inputTextarea = nullptr;
    Device* uartDev = nullptr;
    std::unique_ptr<tt::Thread> uartThread;
    bool uartThreadInterrupted = false;
    std::unique_ptr<tt::Thread> viewThread;
    bool viewThreadInterrupted = false;
    tt::RecursiveMutex mutex;
    uint8_t receiveBuffer[receiveBufferSize] = {};
    uint8_t renderBuffer[renderBufferSize] = {};
    size_t receiveBufferPosition = 0;
    std::string terminatorString = "\n";
};

/** Opens @a dev's UART, builds the console UI into @a parent, and starts the read/render
 *  background threads. Call once per session. */
void consoleViewCreate(lv_obj_t* parent, Context* app, Device* dev);

/** Rebuilds just the console widgets into @a parent, reusing the still-running background
 *  threads from an earlier consoleViewCreate() call. Use this (not consoleViewCreate()) when
 *  this window resurfaces after being buried mid-session - window_manager only ever destroys
 *  the widget tree, so the read/render threads and the open UART are still alive and don't need
 *  restarting (doing so would double-start them). */
void consoleViewRebuildWidgets(lv_obj_t* parent, Context* app);

/** Stops both background threads (joining them) and closes the UART. Safe to call even after
 *  the window's widgets have already been torn down - never touches them. */
void consoleViewStop(Context* app);
