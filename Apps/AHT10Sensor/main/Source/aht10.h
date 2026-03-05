#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool aht10_init(void);
bool aht10_read(float* temperature, float* humidity);

#ifdef __cplusplus
}
#endif

