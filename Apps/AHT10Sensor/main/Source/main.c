#include <tt_app.h>
#include <tt_lvgl_toolbar.h>

#include "aht10.h"

#include <lvgl.h>
#include <stdio.h>

static lv_obj_t* arc_temp;
static lv_obj_t* label_temp;
static lv_obj_t* label_hum;

static lv_timer_t* sensor_timer = NULL;

/* ------------------------------ */
/* Sensor update                  */
/* ------------------------------ */

static void sensor_update(lv_timer_t* timer)
{
    if (!label_temp || !label_hum)
        return;

    float t, h;

    if (!aht10_read(&t, &h))
        return;

    char buf[32];

    int t_int = (int)t;
    lv_arc_set_value(arc_temp, t_int);

    snprintf(buf, sizeof(buf), "%.1f °C", t);
    lv_label_set_text(label_temp, buf);

    snprintf(buf, sizeof(buf), "Humidity %.1f %%", h);
    lv_label_set_text(label_hum, buf);
}

/* ------------------------------ */
/* Show app                       */
/* ------------------------------ */

static void onShowApp(AppHandle app, void* data, lv_obj_t* parent)
{
    lv_obj_t* toolbar =
        tt_lvgl_toolbar_create_for_app(parent, app);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    arc_temp = lv_arc_create(parent);
    lv_obj_set_size(arc_temp, 200, 200);
    lv_obj_align(arc_temp, LV_ALIGN_CENTER, 0, 30);

    lv_arc_set_range(arc_temp, 0, 50);
    lv_arc_set_value(arc_temp, 25);

    lv_arc_set_rotation(arc_temp, 135);
    lv_arc_set_bg_angles(arc_temp, 0, 270);

    label_temp = lv_label_create(parent);
    lv_label_set_text(label_temp, "--.- °C");
    lv_obj_align(label_temp, LV_ALIGN_CENTER, 0, 10);

    label_hum = lv_label_create(parent);
    lv_label_set_text(label_hum, "Humidity --%");
    lv_obj_align(label_hum, LV_ALIGN_CENTER, 0, 60);

    aht10_init();

    sensor_timer = lv_timer_create(sensor_update, 2000, NULL);
}

/* ------------------------------ */
/* Hide app (IMPORTANT FIX)       */
/* ------------------------------ */

static void onHideApp(AppHandle app, void* data)
{
    if (sensor_timer) {
        lv_timer_del(sensor_timer);
        sensor_timer = NULL;
    }

    label_temp = NULL;
    label_hum = NULL;
    arc_temp = NULL;
}

/* ------------------------------ */
/* Entry                          */
/* ------------------------------ */

int main(int argc, char* argv[])
{
    tt_app_register((AppRegistration) {
        .onShow = onShowApp,
        .onHide = onHideApp
    });

    return 0;
}

