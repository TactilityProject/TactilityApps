// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace maps {

/** What an SD card job is for, in the order the I/O task serves them. */
enum class IoPriority : uint8_t {
    /** Tiles on screen, and the UI waiting for a file. */
    Visible,
    /** Tiles around the screen. */
    Prefetch,
    /** Cache writes, conversions, downloads, deletes, folder walks. */
    Background,
    Count
};

/**
 * The waiting jobs of the I/O task: the highest priority first, the oldest first within a priority.
 * Not thread safe, the I/O task guards it with its own lock.
 */
template <typename Item, int CAPACITY>
class IoQueue {
public:
    bool push(const Item& item, IoPriority priority) {
        if (count >= CAPACITY) {
            return false;
        }
        entries[count++] = { item, priority, nextSequence++ };
        return true;
    }

    bool pop(Item& item, IoPriority* priority = nullptr) {
        int best = -1;
        for (int i = 0; i < count; i++) {
            if (best < 0 || entries[i].priority < entries[best].priority ||
                (entries[i].priority == entries[best].priority && entries[i].sequence < entries[best].sequence)) {
                best = i;
            }
        }
        if (best < 0) {
            return false;
        }
        item = entries[best].item;
        if (priority != nullptr) {
            *priority = entries[best].priority;
        }
        entries[best] = entries[--count];
        return true;
    }

    /** Whether a job more urgent than priority waits. */
    bool hasMoreUrgent(IoPriority priority) const {
        for (int i = 0; i < count; i++) {
            if (entries[i].priority < priority) {
                return true;
            }
        }
        return false;
    }

    int size() const { return count; }

private:
    struct Entry {
        Item item;
        IoPriority priority;
        uint32_t sequence;
    };

    Entry entries[CAPACITY] {};
    int count = 0;
    uint32_t nextSequence = 0;
};

} // namespace maps
