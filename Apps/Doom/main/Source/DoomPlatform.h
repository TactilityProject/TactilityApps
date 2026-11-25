#pragma once

// Shared between the engine (C) and the Tactility glue (C++), so both agree on the render path.

#ifdef ESP_PLATFORM
#include <soc/soc_caps.h>
#endif

// Chips with a Pixel Processing Accelerator (ESP32-P4, ESP32-S31) scale and rotate the
// frame in hardware. Other chips (and host builds) use a software scaler.
#if defined(SOC_PPA_SUPPORTED) && SOC_PPA_SUPPORTED
#define DOOM_USE_PPA 1
#else
#define DOOM_USE_PPA 0
#endif
