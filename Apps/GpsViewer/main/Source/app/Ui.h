// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AppContext.h"

#include <lvgl.h>

namespace gpsviewer {

UiState* uiAllocate(AppContext& context);
void uiFree(UiState* ui);

// window_manager_create_ext() callbacks, user_data is the AppContext
void uiCreateWidgets(lv_obj_t* root, void* userData);
void uiDestroyWidgets(void* userData);

// Switches between the normal dark and the tactical red palette. Stores the choice and rebuilds the
// widget tree. LVGL thread only.
void setNightMode(AppContext& context, bool enable);

} // namespace gpsviewer
