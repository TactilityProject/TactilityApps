// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../tiles/TilePaths.h"
#include "CacheIndex.h"

#include <cstdint>

namespace maps {

/**
 * Tidies the SD card cache a folder at a time in idle time: it fills the index of the current style from the
 * files when the index was lost, and deletes the folders of other styles and of the old cache layout
 * (cache/<z>/<x>/<y>). It only ever deletes inside <root>/cache.
 */
class CacheWalker {
public:
    /** Guards the index while the walk adds to it, it is shared with the render lanes. */
    struct IndexLock {
        void (*lock)(void* context);
        void (*unlock)(void* context);
        void* context;
    };

    ~CacheWalker();

    /** keepCurrent false deletes the current style's cache too, when the SD card cache is switched off. */
    void begin(const char* root, const char* style, bool keepCurrent = true);
    bool isComplete() const { return phase == Phase::Done; }

    /** Looks at one folder. Returns false once the walk is complete. lock may be null. */
    bool step(CacheIndex& index, const IndexLock* lock = nullptr);

private:
    enum class Phase : uint8_t {
        ListTop,
        Top,
        Done
    };

    static constexpr int MAX_TOP = 32;
    static constexpr int MAX_COLUMNS = 4096;
    /** A FAT delete holds the card, and the display's shared bus, for up to about 150 ms. */
    static constexpr int MAX_DELETES_PER_STEP = 2;

    bool listColumns(const char* folder);
    void indexColumn(CacheIndex& index, const IndexLock* lock, int z, int x);
    /** Deletes up to MAX_DELETES_PER_STEP files of a column. Returns true once the column is gone. */
    bool deleteColumn(const char* zoomFolder, int x);
    bool removeInCache(const char* path, bool folder);

    char root[PATH_MAX_LENGTH] = "";
    char style[16] = "";
    bool keep = true;
    Phase phase = Phase::Done;
    char top[MAX_TOP][16] {};
    int topCount = 0;
    int topIndex = 0;
    /** The zoom folder being worked on, -1 before the first. */
    int zoom = -1;
    int* columns = nullptr;
    int columnCount = -1;
    int column = 0;
};

} // namespace maps
