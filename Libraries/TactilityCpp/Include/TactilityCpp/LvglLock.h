#pragma once

#include <Tactility/Lock.h>
#include <lvgl/lvgl.h>

class LvglLock final : public tt::Lock {

public:

    using tt::Lock::lock;

    bool lock(TickType_t timeout) const override {
        return lvgl_try_lock(timeout);
    }

    void unlock() const override {
        lvgl_unlock();
    }
};


