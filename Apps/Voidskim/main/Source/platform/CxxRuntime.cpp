#include "CxxRuntime.h"

#ifdef ESP_PLATFORM

// A function-local static with a destructor (Jet keeps a few scratch std::vectors like this)
// registers that destructor with __cxa_atexit, passing __dso_handle. Neither exists for an ELF
// loaded app, so they're provided here. The destructors are run when the game ends, which frees
// the scratch memory instead of leaking it on every launch.

extern "C" {

void* __dso_handle = &__dso_handle;

namespace {
struct Exit {
    void (*function)(void*);
    void* argument;
};
constexpr int MAX_EXITS = 32;
Exit exits[MAX_EXITS];
int exitCount = 0;
}

int __cxa_atexit(void (*function)(void*), void* argument, void* /*dso*/) {
    if (exitCount >= MAX_EXITS) {
        return -1;
    }
    exits[exitCount++] = { function, argument };
    return 0;
}

}

void runStaticDestructors() {
    while (exitCount > 0) {
        const Exit& exit = exits[--exitCount];
        exit.function(exit.argument);
    }
}

#else

// Host builds: the C++ runtime runs them when the library is unloaded
void runStaticDestructors() {}

#endif

#ifdef ESP_PLATFORM

// Global operator new/delete, used by this app (and Jet) only: an ELF-loaded app binds its own
// definitions first.
//
// ESP-IDF's malloc() puts allocations below a size threshold (typically 16 KB) in internal RAM. The
// game makes many such allocations (Jet's per-mesh vertex and triangle vectors, materials, track
// data), which together would use up the internal heap that Wi-Fi, Bluetooth and the drivers need.
// So they prefer PSRAM, falling back to internal RAM when there's no PSRAM.

#include <tactility/memory.h>

#include <new>

static void* allocatePreferExternal(std::size_t size) {
    static const MemoryPolicy policy = { .required = 0, .desired = MEMORY_CAPABILITY_EXTERNAL, .alignment = 0 };
    return memory_alloc_with_policy(size != 0 ? size : 1, &policy);
}

void* operator new(std::size_t size) { return allocatePreferExternal(size); }
void* operator new[](std::size_t size) { return allocatePreferExternal(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocatePreferExternal(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocatePreferExternal(size); }
void operator delete(void* p) noexcept { memory_free(p); }
void operator delete[](void* p) noexcept { memory_free(p); }
void operator delete(void* p, std::size_t) noexcept { memory_free(p); }
void operator delete[](void* p, std::size_t) noexcept { memory_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { memory_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { memory_free(p); }

#endif
