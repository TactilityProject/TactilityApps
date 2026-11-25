#pragma once

// Force-included into Jet's sources (see main/CMakeLists.txt).
//
// Jet's ESP32 fast paths place hot code and tables in internal RAM (IRAM_ATTR, DRAM_ATTR). A
// Tactility app is ELF-loaded, where those placement sections aren't something to rely on, so
// they're made no-ops: everything lands in the app's normal sections. esp_attr.h is included first
// so its include guard keeps later includes from redefining them.

#ifdef ESP_PLATFORM
#include <esp_attr.h>
#undef IRAM_ATTR
#define IRAM_ATTR
#undef DRAM_ATTR
#define DRAM_ATTR
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif
#endif
