// Minimal SPD2010 touch driver for LVGL integration (ESP-IDF I2C master v2)
#pragma once

#include <stdint.h>
#include <stddef.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_err.h>

#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "i2c_device.h"

// SPD2010 I2C address
#ifndef SPD2010_I2C_ADDR
#define SPD2010_I2C_ADDR 0x53
#endif

// Max touch points supported
#ifndef SPD2010_MAX_POINTS
#define SPD2010_MAX_POINTS 5
#endif

class Spd2010Touch : public I2cDevice {
public:
    Spd2010Touch(i2c_master_bus_handle_t i2c_bus,
                 gpio_num_t int_gpio,
                 int screen_width,
                 int screen_height,
                 bool mirror_x,
                 bool mirror_y,
                 bool swap_xy);

    esp_err_t Init();

    // LVGL read callback wrapper (LVGL v9 API)
    static void LvglReadCb(lv_indev_t *indev, lv_indev_data_t *data);

private:
    // Low level helpers for 16-bit register access
    esp_err_t ReadReg16(uint16_t reg, uint8_t *buf, size_t len);
    esp_err_t WriteReg16(uint16_t reg, const uint8_t *data, size_t len);

    // SPD2010 protocol helpers
    esp_err_t WritePointMode();
    esp_err_t WriteStart();
    esp_err_t WriteCpuStart();
    esp_err_t WriteClearInt();
    esp_err_t ReadStatusLen(uint8_t *status_low, uint8_t *status_high, uint16_t *len);
    esp_err_t ReadHdp(uint16_t read_len);
    esp_err_t ReadHdpStatus(uint8_t *status, uint16_t *next_len);
    esp_err_t ReadHdpRemain(uint16_t next_len);

    // Parse HDP buffer into first touch point
    bool ParseFirstPoint(uint16_t *x, uint16_t *y, uint8_t *points);

    // Coordinate transform according to display orientation
    void Transform(uint16_t &x, uint16_t &y) const;

private:
    gpio_num_t int_gpio_;
    int w_;
    int h_;
    bool mirror_x_;
    bool mirror_y_;
    bool swap_xy_;

    // small buffer to hold last HDP read
    static constexpr size_t kMaxHdpBuf = 4 + 10 * 6; // header + up to 10 points
    uint8_t hdp_buf_[kMaxHdpBuf];

    // Polled state shared with LVGL callback
    SemaphoreHandle_t state_mutex_ = nullptr;
    volatile bool pressed_ = false;
    volatile uint16_t cur_x_ = 0;
    volatile uint16_t cur_y_ = 0;
    volatile bool ready_ = false; // SPD2010 initialized to point mode and started
    TaskHandle_t task_ = nullptr;

    // Polling task
    static void TouchTask(void *arg);
};
