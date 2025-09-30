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

// SPD2010 gesture codes (from sample_data[6] & 0x07)
typedef enum {
    SPD2010_GESTURE_NONE = 0x00,
    SPD2010_GESTURE_SWIPE_UP = 0x01,
    SPD2010_GESTURE_SWIPE_DOWN = 0x02,
    SPD2010_GESTURE_SWIPE_LEFT = 0x03,
    SPD2010_GESTURE_SWIPE_RIGHT = 0x04,
    SPD2010_GESTURE_ZOOM_IN = 0x05,
    SPD2010_GESTURE_ZOOM_OUT = 0x06,
    SPD2010_GESTURE_ROTATE = 0x07,
} spd2010_gesture_t;

// Touch point report
typedef struct {
    uint8_t id;
    uint16_t x;
    uint16_t y;
    uint8_t weight;
} spd2010_touch_point_t;

// Extended touch data with gesture support
typedef struct {
    spd2010_touch_point_t points[10];  // Up to 10 touch points
    uint8_t point_count;               // Number of valid touch points
    spd2010_gesture_t gesture;         // Detected gesture
    bool down;                         // Touch down event
    bool up;                           // Touch up event
    uint16_t down_x;                   // Touch down X coordinate
    uint16_t down_y;                   // Touch down Y coordinate
    uint16_t up_x;                     // Touch up X coordinate
    uint16_t up_y;                     // Touch up Y coordinate
} spd2010_touch_data_t;

// Initialize SPD2010 touch using I2C v2 bus. INT pin may be GPIO_NUM_NC.
esp_err_t spd2010_touch_init(i2c_master_bus_handle_t bus, gpio_num_t int_gpio);
void spd2010_touch_deinit(void);

// Try read first touch point. Returns true if pressed and sets x/y.
bool spd2010_touch_read_first(uint16_t *x, uint16_t *y);

// Read full touch data including all points and gestures
bool spd2010_touch_read_full(spd2010_touch_data_t *data);

// LVGL v9 read callback wrapper (pointer device)
void spd2010_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif
