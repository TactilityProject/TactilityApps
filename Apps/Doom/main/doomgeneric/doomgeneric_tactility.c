#include "doomgeneric.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Hook functions implemented in DoomEsp.cpp
extern void doom_draw_frame(const uint32_t *buffer);
extern int doom_get_key(int* pressed, unsigned char* key);

void DG_Init() {
    // Initialization of hardware is done by the Tactility app before the doom loop starts
}

void DG_DrawFrame() {
    doom_draw_frame(DG_ScreenBuffer);
}

void DG_SleepMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t DG_GetTicksMs() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

int DG_GetKey(int* pressed, unsigned char* key) {
    return doom_get_key(pressed, key);
}

void DG_SetWindowTitle(const char * title) {
    // No window title on raw LCD
}
