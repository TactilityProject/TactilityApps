#pragma once

#include <lvgl.h>

struct Context;

// Builds the unit-selection list directly into @a parent. Stateless - nothing to tear down
// beyond deleting the widgets (handled by the caller via lv_obj_clean on the wrapper).
void testListViewCreate(lv_obj_t* parent, Context* app);
