#pragma once

#include <Tactility/Lock.h>
#include <tactility/lvgl_module.h>

class LvglLock final : public tt::Lock {

public:

    void lock() const override {
        lvgl_lock();
    }

    bool try_lock(TickType_t timeout) const override {
        return lvgl_try_lock(timeout);
    }

    void unlock() const override {
        lvgl_unlock();
    }
};


