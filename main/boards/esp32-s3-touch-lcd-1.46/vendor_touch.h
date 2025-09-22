#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize SPD2010 touch using I2C v2 bus. INT pin may be GPIO_NUM_NC.
esp_err_t spd2010_touch_init(i2c_master_bus_handle_t bus, gpio_num_t int_gpio);
void spd2010_touch_deinit(void);

// Try read first touch point. Returns true if pressed and sets x/y.
bool spd2010_touch_read_first(uint16_t *x, uint16_t *y);

// LVGL v9 read callback wrapper (pointer device)
void spd2010_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif
