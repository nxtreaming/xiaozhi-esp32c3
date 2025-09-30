#include "vendor_touch.h"
#include <esp_log.h>
#include <esp_check.h>
#include <string.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "SPD2010_VND";
static i2c_master_dev_handle_t s_dev = NULL;
static gpio_num_t s_int_gpio = GPIO_NUM_NC;
static bool s_ready = false;
static bool s_cpu_run = false;

// Global touch state for gesture tracking
static spd2010_touch_data_t s_touch_state = {0};


// SPD2010 registers (16-bit)
#define REG_POINT_MODE   0x5000
#define REG_START        0x4600
#define REG_CPU_START    0x0400
#define REG_CLEAR_INT    0x0200
#define REG_STATUS_LEN   0x2000
#define REG_HDP          0x0003
#define REG_HDP_STATUS   0xFC02

#define SPD2010_ADDR     0x53

static esp_err_t wr16(uint16_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[2 + 8];
    if (len > 8) return ESP_ERR_INVALID_ARG;
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    if (len) memcpy(buf + 2, data, len);
    // Increase timeout to tolerate slow rise times on weak pull-ups
    return i2c_master_transmit(s_dev, buf, 2 + len, 100);
}

static esp_err_t rd16(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    // Increase timeout to tolerate slow rise times on weak pull-ups
    return i2c_master_transmit_receive(s_dev, addr, 2, data, len, 100);
}

// Some boards expect 16-bit payload as big-endian for write 'value'
static bool s_payload_big_endian = false; // default little-endian: {LSB, MSB}

static inline esp_err_t write_point_mode(void) {
    uint8_t v[2] = {0,0};
    return wr16(REG_POINT_MODE, v, 2);
}
static inline esp_err_t write_start(void) {
    uint8_t v[2] = {0,0};
    return wr16(REG_START, v, 2);
}
static inline esp_err_t write_cpu_start_dyn(void) {
    uint8_t v[2] = {1,0};
    if (s_payload_big_endian) { v[0] = 0; v[1] = 1; }
    return wr16(REG_CPU_START, v, 2);
}
static inline esp_err_t write_clear_int_dyn(void) {
    uint8_t v[2] = {1,0};
    if (s_payload_big_endian) { v[0] = 0; v[1] = 1; }
    return wr16(REG_CLEAR_INT, v, 2);
}
// Backward-compatible wrappers for earlier call-sites
static inline esp_err_t write_cpu_start(void) { return write_cpu_start_dyn(); }
static inline esp_err_t write_clear_int(void) { return write_clear_int_dyn(); }


esp_err_t spd2010_touch_init(i2c_master_bus_handle_t bus, gpio_num_t int_gpio)
{
    ESP_LOGI(TAG, "Initializing SPD2010 touch controller...");
    ESP_LOGI(TAG, "I2C address: 0x%02X, INT GPIO: %d", SPD2010_ADDR, int_gpio);

    // Wait for touch controller to be ready after hardware reset (TCA9554)
    esp_rom_delay_us(300000); // 300ms

    s_int_gpio = int_gpio;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SPD2010_ADDR,
        // Keep low speed to improve margins when pull-ups are weak
        .scl_speed_hz = 50 * 1000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add dev");

    // Configure INT pin (optional)
    if (s_int_gpio != GPIO_NUM_NC) {
        gpio_reset_pin(s_int_gpio);
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << s_int_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = 1,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        ESP_LOGI(TAG, "INT initial level: %d", (int)gpio_get_level(s_int_gpio));
    }

    // Robust bring-up: try to transition BIOS -> CPU -> RUN with retries
    s_ready = false; s_cpu_run = false;
    uint8_t last_state = 0xFF;
    for (int tries = 0; tries < 20; ++tries) {
        uint8_t s[4] = {0};
        esp_err_t r = rd16(REG_STATUS_LEN, s, sizeof(s));
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "Probe %d: rd16 failed: %s", tries + 1, esp_err_to_name(r));
            esp_rom_delay_us(10000);
            continue;
        }
        bool in_bios = (s[1] & 0x40) != 0;
        bool in_cpu  = (s[1] & 0x20) != 0;
        bool cpu_run = (s[1] & 0x08) != 0;
        uint16_t read_len = (uint16_t)(s[3] << 8) | s[2];
        uint8_t cur_state = (cpu_run ? 0x4 : 0) | (in_cpu ? 0x2 : 0) | (in_bios ? 0x1 : 0);
        if (cur_state != last_state) {
            ESP_LOGI(TAG, "Probe %d: BIOS=%d CPU=%d RUN=%d len=%u [raw:%02X %02X %02X %02X]",
                     tries + 1, in_bios, in_cpu, cpu_run, (unsigned)read_len, s[0], s[1], s[2], s[3]);
            last_state = cur_state;
        }

        if (in_bios) {
            esp_err_t e1 = write_clear_int_dyn();
            esp_rom_delay_us(1000);
            esp_err_t e2 = write_cpu_start_dyn();
            ESP_LOGI(TAG, "BIOS: CLR_INT=%s CPU_START=%s%s", esp_err_to_name(e1), esp_err_to_name(e2), s_payload_big_endian?" (BE)":" (LE)");
            esp_rom_delay_us(5000);
            if ((tries % 5) == 4) {
                s_payload_big_endian = !s_payload_big_endian;
                ESP_LOGW(TAG, "Switch payload endianness to %s for CPU_START", s_payload_big_endian?"BE":"LE");
            }
            continue;
        }
        if (in_cpu && !cpu_run) {
            esp_err_t e0 = write_point_mode();
            esp_rom_delay_us(3000);
            esp_err_t e1 = write_start();
            esp_rom_delay_us(3000);
            esp_err_t e2 = write_clear_int_dyn();
            ESP_LOGI(TAG, "CPU:no-run: PNT=%s START=%s CLR_INT=%s%s", esp_err_to_name(e0), esp_err_to_name(e1), esp_err_to_name(e2), s_payload_big_endian?" (BE)":" (LE)");
            esp_rom_delay_us(3000);
            continue;
        }
        if (cpu_run) {
            s_cpu_run = true;
            s_ready = true;
            break;
        }
        esp_rom_delay_us(5000);
    }

    ESP_LOGI(TAG, "Init result: ready=%d cpu_run=%d", (int)s_ready, (int)s_cpu_run);
    if (!s_ready) {
        ESP_LOGE(TAG, "SPD2010 failed to enter RUN state after retries");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void spd2010_touch_deinit(void)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
}

// Internal function to read full touch data with gesture support
static bool spd2010_touch_read_internal(spd2010_touch_data_t *touch_data)
{
    if (!s_dev) return false;

    // Optimization: If INT is high and we were not pressed, skip I2C read
    // But if we were pressed, always read to detect release
    static bool s_was_touched = false;
    if (s_ready && s_int_gpio != GPIO_NUM_NC && gpio_get_level(s_int_gpio) != 0 && !s_was_touched) {
        return false;  // No new data and not tracking a touch
    }

    // Read status register
    uint8_t s[4];
    if (rd16(REG_STATUS_LEN, s, sizeof(s)) != ESP_OK) return false;

    bool pt_exist = (s[0] & 0x01) != 0;
    bool gesture_flag = (s[0] & 0x02) != 0;
    bool aux_flag = (s[0] & 0x08) != 0;
    bool in_bios = (s[1] & 0x40) != 0;
    bool in_cpu  = (s[1] & 0x20) != 0;
    bool cpu_run = (s[1] & 0x08) != 0;
    uint16_t read_len = (uint16_t)(s[3] << 8) | s[2];

    s_cpu_run = cpu_run;
    if (cpu_run) s_ready = true;

    // Debug: log state transitions occasionally
    static uint8_t last_state = 0xFF; // bit2:run, bit1:cpu, bit0:bios
    uint8_t cur_state = (cpu_run ? 0x4 : 0) | (in_cpu ? 0x2 : 0) | (in_bios ? 0x1 : 0);
    static int poll_cnt = 0;
    if (cur_state != last_state || (++poll_cnt % 200) == 0) {
        ESP_LOGI(TAG, "Status: BIOS=%d CPU=%d RUN=%d pt=%d len=%u", in_bios, in_cpu, cpu_run, pt_exist, (unsigned)read_len);
        last_state = cur_state;
        if (poll_cnt >= 200) poll_cnt = 0;
    }

    // If controller is in BIOS, try to start CPU like the vendor demo does
    if (in_bios) {
        write_clear_int();
        esp_rom_delay_us(200);
        write_cpu_start();
        esp_rom_delay_us(200);
        return false;
    }

    // If CPU mode but not running yet, send point mode/start sequence
    if (in_cpu && !cpu_run) {
        write_point_mode();
        esp_rom_delay_us(200);
        write_start();
        esp_rom_delay_us(200);
        write_clear_int();
        return false;
    }

    // Handle AUX data (auxiliary data from touch controller)
    if (cpu_run && aux_flag) {
        ESP_LOGD(TAG, "AUX data detected, clearing interrupt");
        write_clear_int();
        return false;
    }

    // Check if there's touch data or gesture to read
    if ((!pt_exist && !gesture_flag) || read_len < 10) {
        // No touch data or gesture, safe to skip
        // Optional: clear INT if it's asserted but no valid data
        if (cpu_run && s_int_gpio != GPIO_NUM_NC && gpio_get_level(s_int_gpio) == 0) {
            write_clear_int();
        }
        return false;
    }

    // If we reach here, we have touch data or gesture to read
    // Don't gate by INT here - status register is the source of truth

    // Read first packet via HDP and parse
    uint8_t buf[4 + 10 * 6] = {0};
    bool read_ok = (rd16(REG_HDP, buf, read_len) == ESP_OK);

    // CRITICAL: Always check HDP_STATUS and clear INT after reading HDP,
    // even if the read failed or data is invalid. Otherwise the chip gets stuck.
    uint8_t hs[8];
    bool status_ok = (rd16(REG_HDP_STATUS, hs, sizeof(hs)) == ESP_OK);

    if (status_ok) {
        uint8_t status = hs[5];
        uint16_t next_len = (uint16_t)(hs[2] | (hs[3] << 8));

        if (status == 0x82) {
            // Data ready and complete, clear INT
            write_clear_int();
        } else if (status == 0x00) {
            // More data pending, drain it
            if (next_len > 0 && next_len < 256) {
                uint8_t tmp[256];
                size_t to_read = next_len > sizeof(tmp) ? sizeof(tmp) : next_len;
                rd16(REG_HDP, tmp, to_read);
                // Check status again after draining
                if (rd16(REG_HDP_STATUS, hs, sizeof(hs)) == ESP_OK && hs[5] == 0x82) {
                    write_clear_int();
                }
            } else {
                // Invalid next_len, force clear to recover
                write_clear_int();
            }
        } else {
            // Unknown status, force clear to avoid stuck
            write_clear_int();
        }
    } else {
        // Can't read HDP_STATUS, force clear INT to recover
        write_clear_int();
    }

    // Now parse the data if read was successful
    if (!read_ok) return false;

    // Check packet type (first byte after 4-byte header)
    uint8_t check_id = buf[4];

    // Reset touch data
    if (touch_data) {
        touch_data->point_count = 0;
        touch_data->gesture = SPD2010_GESTURE_NONE;
    }

    // Parse touch points (check_id <= 0x0A indicates touch point data)
    if ((check_id <= 0x0A) && pt_exist) {
        uint8_t point_count = (read_len - 4) / 6;  // Each point is 6 bytes
        if (point_count > 10) point_count = 10;    // Max 10 points

        if (touch_data) {
            touch_data->point_count = point_count;
            touch_data->gesture = SPD2010_GESTURE_NONE;

            // Parse all touch points
            for (uint8_t i = 0; i < point_count; i++) {
                uint8_t offset = i * 6;
                touch_data->points[i].id = buf[4 + offset];
                touch_data->points[i].x = (((buf[7 + offset] & 0xF0) << 4) | buf[5 + offset]);
                touch_data->points[i].y = (((buf[7 + offset] & 0x0F) << 8) | buf[6 + offset]);
                touch_data->points[i].weight = buf[8 + offset];
            }

            // Track down/up events for gesture recognition
            if (point_count > 0) {
                if ((touch_data->points[0].weight != 0) && (!touch_data->down)) {
                    touch_data->down = true;
                    touch_data->up = false;
                    touch_data->down_x = touch_data->points[0].x;
                    touch_data->down_y = touch_data->points[0].y;
                } else if ((touch_data->points[0].weight == 0) && (touch_data->down)) {
                    touch_data->up = true;
                    touch_data->down = false;
                    touch_data->up_x = touch_data->points[0].x;
                    touch_data->up_y = touch_data->points[0].y;
                }
            }
        }

        bool has_touch = (point_count > 0);
        s_was_touched = has_touch;  // Update tracking flag
        return has_touch;
    }
    // Parse gesture (check_id == 0xF6 indicates gesture data)
    else if ((check_id == 0xF6) && gesture_flag) {
        if (touch_data) {
            touch_data->point_count = 0;
            touch_data->up = false;
            touch_data->down = false;
            touch_data->gesture = (spd2010_gesture_t)(buf[6] & 0x07);

            ESP_LOGI(TAG, "Gesture detected: 0x%02x", touch_data->gesture);
        }
        s_was_touched = false;  // Gesture doesn't count as touch
        return false;  // Gesture doesn't count as "pressed"
    }

    s_was_touched = false;  // No touch data
    return false;
}

// Public API: Read full touch data including all points and gestures
bool spd2010_touch_read_full(spd2010_touch_data_t *data)
{
    if (!data) return false;
    return spd2010_touch_read_internal(data);
}

// Public API: Read first touch point only (backward compatible)
bool spd2010_touch_read_first(uint16_t *x, uint16_t *y)
{
    spd2010_touch_data_t data = {0};
    bool pressed = spd2010_touch_read_internal(&data);

    if (pressed && data.point_count > 0) {
        if (x) *x = data.points[0].x;
        if (y) *y = data.points[0].y;
        return true;
    }

    return false;
}

// External functions to access Application slideshow control
extern bool app_is_slideshow_running(void);
extern void app_slideshow_next(void);
extern void app_slideshow_prev(void);

void spd2010_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    // Static variables for swipe detection
    static bool s_was_pressed = false;
    static uint16_t s_down_x = 0, s_down_y = 0;
    static uint16_t s_last_x = 0, s_last_y = 0;

    // Swipe cooldown to prevent rapid consecutive triggers
    // Need longer cooldown to allow GIF cleanup to complete (especially for large GIFs)
    static TickType_t s_last_swipe_time = 0;
    const TickType_t kSwipeCooldown = pdMS_TO_TICKS(2000);  // 2000ms cooldown

    // Read full touch data including gestures
    bool pressed = spd2010_touch_read_internal(&s_touch_state);

    if (pressed && s_touch_state.point_count > 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = s_touch_state.points[0].x;
        data->point.y = s_touch_state.points[0].y;

        // Track touch down position
        if (!s_was_pressed) {
            s_was_pressed = true;
            s_down_x = s_touch_state.points[0].x;
            s_down_y = s_touch_state.points[0].y;
            ESP_LOGI(TAG, "Touch DOWN at x=%u y=%u", s_down_x, s_down_y);
        }

        // Update last position for swipe calculation
        s_last_x = s_touch_state.points[0].x;
        s_last_y = s_touch_state.points[0].y;

        // Log touch movement for debugging (only when position changes significantly)
        static uint16_t last_log_x = 0, last_log_y = 0;
        if (abs((int)s_last_x - (int)last_log_x) > 100 || abs((int)s_last_y - (int)last_log_y) > 100) {
            ESP_LOGD(TAG, "Touch MOVE x=%u y=%u (dx=%d dy=%d)",
                     s_last_x, s_last_y,
                     (int)s_last_x - (int)s_down_x,
                     (int)s_last_y - (int)s_down_y);
            last_log_x = s_last_x;
            last_log_y = s_last_y;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;

        // Detect swipe gesture on touch release
        if (s_was_pressed) {
            s_was_pressed = false;

            // Calculate swipe distance
            int dx = (int)s_last_x - (int)s_down_x;
            int dy = (int)s_last_y - (int)s_down_y;

            // Always log touch release with swipe info for debugging
            ESP_LOGI(TAG, "Touch UP: down=(%u,%u) last=(%u,%u) dx=%d dy=%d",
                     s_down_x, s_down_y, s_last_x, s_last_y, dx, dy);

            // Swipe threshold in display coordinates (412x412 pixels)
            // ~100 pixels = about 1/4 screen width, comfortable swipe distance
            const int kSwipeThreshold = 100;

            // Check if horizontal swipe is dominant and exceeds threshold
            if (abs(dx) > kSwipeThreshold && abs(dx) > abs(dy)) {
                // Check cooldown to prevent rapid consecutive swipes
                TickType_t now = xTaskGetTickCount();
                TickType_t elapsed = now - s_last_swipe_time;

                ESP_LOGI(TAG, "Swipe detected: dx=%d dy=%d, elapsed=%u ms, cooldown=%u ms",
                         dx, dy, (unsigned)(elapsed * portTICK_PERIOD_MS),
                         (unsigned)(kSwipeCooldown * portTICK_PERIOD_MS));

                if (elapsed > kSwipeCooldown) {
                    if (app_is_slideshow_running()) {
                        if (dx < 0) {
                            // Swipe left (right to left) -> Next
                            ESP_LOGI(TAG, "Swipe LEFT detected (dx=%d) -> SlideShowNext", dx);
                            app_slideshow_next();
                            s_last_swipe_time = now;  // Update cooldown timer
                        } else {
                            // Swipe right (left to right) -> Previous
                            ESP_LOGI(TAG, "Swipe RIGHT detected (dx=%d) -> SlideShowPrev", dx);
                            app_slideshow_prev();
                            s_last_swipe_time = now;  // Update cooldown timer
                        }
                    } else {
                        ESP_LOGI(TAG, "Swipe detected but slideshow not running (dx=%d, dy=%d)", dx, dy);
                    }
                } else {
                    ESP_LOGI(TAG, "Swipe IGNORED (cooldown: %u ms remaining)",
                             (unsigned)((kSwipeCooldown - elapsed) * portTICK_PERIOD_MS));
                }
            } else {
                ESP_LOGD(TAG, "Touch UP: swipe too small (threshold=%d, |dx|=%d, |dy|=%d)",
                         kSwipeThreshold, abs(dx), abs(dy));
            }
        }

        // Log hardware gestures when detected (from SPD2010 chip)
        if (s_touch_state.gesture != SPD2010_GESTURE_NONE) {
            const char *gesture_name = "UNKNOWN";
            switch (s_touch_state.gesture) {
                case SPD2010_GESTURE_SWIPE_UP: gesture_name = "SWIPE_UP"; break;
                case SPD2010_GESTURE_SWIPE_DOWN: gesture_name = "SWIPE_DOWN"; break;
                case SPD2010_GESTURE_SWIPE_LEFT: gesture_name = "SWIPE_LEFT"; break;
                case SPD2010_GESTURE_SWIPE_RIGHT: gesture_name = "SWIPE_RIGHT"; break;
                case SPD2010_GESTURE_ZOOM_IN: gesture_name = "ZOOM_IN"; break;
                case SPD2010_GESTURE_ZOOM_OUT: gesture_name = "ZOOM_OUT"; break;
                case SPD2010_GESTURE_ROTATE: gesture_name = "ROTATE"; break;
                default: break;
            }
            ESP_LOGI(TAG, "Hardware Gesture: %s (0x%02x)", gesture_name, s_touch_state.gesture);
            s_touch_state.gesture = SPD2010_GESTURE_NONE;  // Clear after logging
        }

        // Only log release events occasionally to avoid spam
        static int release_count = 0;
        if (++release_count % 100 == 0) {
            ESP_LOGD(TAG, "Touch RELEASED (logged every 100 calls)");
        }
    }
}
