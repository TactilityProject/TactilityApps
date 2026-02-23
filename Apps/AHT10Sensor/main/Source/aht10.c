#include "aht10.h"

#include <tactility/device.h>
#include <tactility/drivers/i2c_controller.h>
#include <tactility/freertos/freertos.h>

#define AHT10_ADDR 0x38

static struct Device* s_i2c = NULL;

/* ------------------------------ */
/* Find I2C controller             */
/* ------------------------------ */

static bool find_i2c(struct Device* dev, void* ctx)
{
    s_i2c = dev;
    return false;
}

static bool setup_i2c(void)
{
    s_i2c = NULL;

    device_for_each_of_type(
        &I2C_CONTROLLER_TYPE,
        NULL,
        find_i2c);

    if (!s_i2c)
        return false;

    return device_is_ready(s_i2c);
}

/* ------------------------------ */
/* Init sensor                     */
/* ------------------------------ */

bool aht10_init(void)
{
    if (!setup_i2c())
        return false;

    uint8_t init_cmd[3] = {0xE1, 0x08, 0x00};

    return i2c_controller_write(
               s_i2c,
               AHT10_ADDR,
               init_cmd,
               sizeof(init_cmd),
               pdMS_TO_TICKS(100))
           == ERROR_NONE;
}

/* ------------------------------ */
/* Read measurement                */
/* ------------------------------ */

bool aht10_read(float* temperature, float* humidity)
{
    if (!s_i2c)
        return false;

    uint8_t cmd[3] = {0xAC, 0x33, 0x00};

    if (i2c_controller_write(
            s_i2c,
            AHT10_ADDR,
            cmd,
            sizeof(cmd),
            pdMS_TO_TICKS(100)) != ERROR_NONE)
        return false;

    vTaskDelay(pdMS_TO_TICKS(80));

    uint8_t data[6];

    if (i2c_controller_read(
            s_i2c,
            AHT10_ADDR,
            data,
            sizeof(data),
            pdMS_TO_TICKS(100)) != ERROR_NONE)
        return false;

    uint32_t raw_h =
        ((uint32_t)data[1] << 12) |
        ((uint32_t)data[2] << 4) |
        (data[3] >> 4);

    uint32_t raw_t =
        ((uint32_t)(data[3] & 0x0F) << 16) |
        ((uint32_t)data[4] << 8) |
        data[5];

    *humidity = raw_h * 100.0f / 1048576.0f;
    *temperature = raw_t * 200.0f / 1048576.0f - 50.0f;

    return true;
}

