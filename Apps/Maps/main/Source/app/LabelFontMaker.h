// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>

namespace maps {

/**
 * Makes the map's label fonts once, from the small TrueType file the app ships (assets/fonts/LabelSans.ttf: Adwaita Sans
 * with the Latin, Greek and Cyrillic letters common on maps, which Tactility's own fonts lack), with the binfont-module's binfont_generate(). The fonts are
 * cached on the card as <root>/fonts/lbl<em size><FONT_VERSION>.bin: raise FONT_VERSION when the sizes or the subset
 * change, the sizes of the older version are deleted once the new ones are written. A short-lived low priority task does the work, so input and the first frame are never held up; the
 * card is only touched through the I/O task. The map loop looks at isFinished() and opens the cached files.
 */
class LabelFontMaker {
public:
    static constexpr int COUNT = 3;
    /** 'c': the subset from tools/label_codepoints.txt (0.3.0). */
    static constexpr char FONT_VERSION = 'c';
    /**
     * Em sizes with the letter sizes of Tactility's small, default and large fonts on the T-Deck. The lines are taller
     * (17, 21, 26 px) because the subset's tallest accents (Vietnamese, Cyrillic) count in the line height.
     */
    static constexpr int EM_SIZES[COUNT] = { 11, 14, 18 };

    ~LabelFontMaker() { stop(); }

    /** The cache file of font i. */
    static void cachePath(const char* root, int i, char* out, size_t size);
    /** Whether the cache folder, the ttf and the paths are set up; call before start(). */
    void configure(IoTask& ioTask, const char* root, const char* ttfPath);
    /** Starts making the fonts in the mask (bit i for font i); false when the task could not be started. */
    bool start(unsigned missingMask);
    /** True once the task is done, whether or not it succeeded; it never starts again. */
    bool isFinished() const { return finished.load(); }
    bool isRunning() const { return task != nullptr && !finished.load(); }
    /** Bit i is set when the cache file of font i was written. */
    unsigned writtenMask() const { return written.load(); }
    /** Stops the task (between two fonts) and frees it; the app calls it before the I/O task stops. */
    void stop();

private:
    static void taskMain(void* context);
    void run();

    IoTask* io = nullptr;
    char rootPath[160] = "";
    char ttfPath[260] = "";
    unsigned wanted = 0;
    TaskHandle_t task = nullptr;
    std::atomic<bool> finished = false;
    std::atomic<bool> stopRequested = false;
    std::atomic<unsigned> written = 0;
};

} // namespace maps
