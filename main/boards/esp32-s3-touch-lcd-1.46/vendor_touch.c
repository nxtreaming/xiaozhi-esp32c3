#include "vendor_touch.h"
#include <esp_log.h>
#include <esp_check.h>
#include <string.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "config.h"

static const char *TAG = "SPD2010_VND";
static TaskHandle_t s_touch_task = NULL;
static SemaphoreHandle_t s_touch_lock = NULL;
typedef struct {
    bool valid;
    bool pressed;
    uint16_t x;
    uint16_t y;
} touch_cache_t;
static touch_cache_t s_touch_cache = { .valid = false, .pressed = false, .x = 0, .y = 0 };

// Forward declarations
static void spd2010_touch_task_entry(void *arg);
bool spd2010_touch_read_first(uint16_t *x, uint16_t *y);


static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static gpio_num_t s_int_gpio = GPIO_NUM_NC;
static volatile int s_i2c_fail_streak = 0;
static volatile bool s_need_clear_int = false;

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
    esp_err_t err = i2c_master_transmit(s_dev, buf, 2 + len, 100);
    if (err == ESP_OK) {
        s_i2c_fail_streak = 0;
    } else {
        s_i2c_fail_streak++;
        if (s_bus) {
            (void)i2c_master_bus_reset(s_bus);
        }
    }
    return err;
}

// Prototypes for helpers used below
static esp_err_t rd16(uint16_t reg, uint8_t *data, size_t len);
static esp_err_t write_point_mode(void);
static esp_err_t write_start(void);
static esp_err_t write_cpu_start(void);
static esp_err_t write_clear_int(void);
static bool read_status_len_consistent(uint8_t s_out[4]);


// Force SPD2010 to leave BIOS and enter CPU/RUN within timeout
static bool spd2010_force_startup(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t deadline = start + pdMS_TO_TICKS(timeout_ms);
    uint8_t last = 0xFF;
    while (xTaskGetTickCount() < deadline) {
        uint8_t s[4];
        if (!read_status_len_consistent(s)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool in_bios = (s[1] & 0x40) != 0;
        bool in_cpu  = (s[1] & 0x20) != 0;
        bool cpu_run = (s[1] & 0x08) != 0;
        uint8_t cur = (cpu_run?4:0)|(in_cpu?2:0)|(in_bios?1:0);
        if (cur != last) {
            ESP_LOGI(TAG, "Force: BIOS=%d CPU=%d RUN=%d", in_bios, in_cpu, cpu_run);
            last = cur;
        }
        if (cpu_run) return true; // already running
        if (in_bios) {
            write_clear_int();
            vTaskDelay(pdMS_TO_TICKS(10));
            write_cpu_start();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (in_cpu && !cpu_run) {
            write_point_mode();
            vTaskDelay(pdMS_TO_TICKS(10));
            write_start();
            vTaskDelay(pdMS_TO_TICKS(20));
            write_clear_int();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // Neither BIOS nor CPU flags set; wait and retry
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static esp_err_t rd16(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    esp_err_t err = i2c_master_transmit_receive(s_dev, addr, 2, data, len, 100);
    if (err == ESP_OK) {
        s_i2c_fail_streak = 0;
    } else {
        s_i2c_fail_streak++;
        if (s_bus) {
            (void)i2c_master_bus_reset(s_bus);
        }
    }
    return err;
}
// Read REG_STATUS_LEN twice and require consistency + sanity
static bool read_status_len_consistent(uint8_t s_out[4])
{
    for (int i = 0; i < 3; ++i) {
        uint8_t s1[4], s2[4];
        if (rd16(REG_STATUS_LEN, s1, sizeof(s1)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        // small gap to avoid reading mid-update
        esp_rom_delay_us(800);
        if (rd16(REG_STATUS_LEN, s2, sizeof(s2)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (memcmp(s1, s2, sizeof(s1)) != 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        bool in_bios = (s1[1] & 0x40) != 0;
        bool in_cpu  = (s1[1] & 0x20) != 0;
        bool cpu_run = (s1[1] & 0x08) != 0;
        uint16_t len = (uint16_t)(s1[3] << 8) | s1[2];
        // sanity: mutually-exclusive BIOS vs CPU/RUN; length reasonable (< 2KB)
        if (in_bios && (in_cpu || cpu_run)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (len == 0xFFFF || len > 2048) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        memcpy(s_out, s1, sizeof(s1));
        return true;
    }
    return false;
}


static inline esp_err_t write_point_mode(void) { uint8_t v[2] = {0,0}; return wr16(REG_POINT_MODE, v, 2); }
static inline esp_err_t write_start(void)      { uint8_t v[2] = {0,0}; return wr16(REG_START, v, 2); }
static inline esp_err_t write_cpu_start(void)  { uint8_t v[2] = {1,0}; return wr16(REG_CPU_START, v, 2); }
static inline esp_err_t write_clear_int(void)  { uint8_t v[2] = {1,0}; return wr16(REG_CLEAR_INT, v, 2); }

// Background polling task: decouple I2C polling from LVGL read callback
static void spd2010_touch_task_entry(void *arg)
{
    (void)arg;
    TickType_t delay_ticks = pdMS_TO_TICKS(10); // base ~100Hz (stable)
    for (;;) {
        // Honor clear-int requests from LVGL side (timeout-release path)
        if (s_need_clear_int) {
            write_clear_int();
            s_need_clear_int = false;
        }
        uint16_t x = 0, y = 0;
        bool pressed = spd2010_touch_read_first(&x, &y);
        if (s_touch_lock) {
            if (xSemaphoreTake(s_touch_lock, portMAX_DELAY) == pdTRUE) {
                s_touch_cache.valid = true;
                s_touch_cache.pressed = pressed;
                if (pressed) {
                    s_touch_cache.x = x;
                    s_touch_cache.y = y;
                }
                xSemaphoreGive(s_touch_lock);
            }
        }
        // Backoff when consecutive I2C failures are observed
        if (s_i2c_fail_streak >= 3) {
            delay_ticks = pdMS_TO_TICKS(50);
        } else if (s_i2c_fail_streak == 0) {
            delay_ticks = pdMS_TO_TICKS(10);
        }
        vTaskDelay(delay_ticks);
    }
}

esp_err_t spd2010_touch_init(i2c_master_bus_handle_t bus, gpio_num_t int_gpio)
{
    ESP_LOGI(TAG, "Initializing SPD2010 touch controller...");
    ESP_LOGI(TAG, "I2C address: 0x%02X, INT GPIO: %d", SPD2010_ADDR, int_gpio);

    // Wait for touch controller to be ready after hardware reset
    ESP_LOGI(TAG, "Waiting for touch controller to be ready after reset...");
    vTaskDelay(pdMS_TO_TICKS(300)); // cooperative delay to avoid WDT during startup

    s_bus = bus;
    s_int_gpio = int_gpio;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SPD2010_ADDR,
        // Lower I2C speed to improve signal margins when pull-ups are weak
        .scl_speed_hz = 50 * 1000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add dev");

    if (s_int_gpio != GPIO_NUM_NC) {
        ESP_LOGI(TAG, "Configuring interrupt pin GPIO_%d", s_int_gpio);
        gpio_reset_pin(s_int_gpio);
        gpio_config_t io_conf = {


            .pin_bit_mask = 1ULL << s_int_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = 1,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    } else {
        ESP_LOGW(TAG, "No interrupt pin configured, using polling mode only");
    }

    // Test basic I2C communication first
    uint8_t test_status[4];
    esp_err_t comm_test = rd16(REG_STATUS_LEN, test_status, sizeof(test_status));
    if (comm_test != ESP_OK) {
        ESP_LOGE(TAG, "I2C communication failed: %s", esp_err_to_name(comm_test));
        ESP_LOGE(TAG, "Check I2C wiring and pull-up resistors");
        return comm_test;
    }

    ESP_LOGI(TAG, "I2C communication OK. Initial status: 0x%02X 0x%02X 0x%02X 0x%02X",
             test_status[0], test_status[1], test_status[2], test_status[3]);

    // Deterministic bring-up: ensure we are out of BIOS before proceeding
    ESP_LOGI(TAG, "Trying to start SPD2010 CPU (deterministic bring-up)...");
    bool started = spd2010_force_startup(800); // up to ~800ms
    if (!started) {
        ESP_LOGW(TAG, "SPD2010 did not enter RUN within timeout; continuing with background recovery");
    }

    // Create background polling task (decoupled from LVGL read)
    s_touch_lock = xSemaphoreCreateMutex();
    if (!s_touch_lock) {
        ESP_LOGE(TAG, "Failed to create touch mutex");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(spd2010_touch_task_entry, "spd2010_touch", 3072, NULL, 4, &s_touch_task);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_touch_lock);
        s_touch_lock = NULL;
        ESP_LOGE(TAG, "Failed to create touch polling task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SPD2010 touch controller initialized successfully");
    return ESP_OK;
}

void spd2010_touch_deinit(void)
{
    if (s_touch_task) {
        vTaskDelete(s_touch_task);
        s_touch_task = NULL;
    }
    if (s_touch_lock) {
        vSemaphoreDelete(s_touch_lock);
        s_touch_lock = NULL;
    }
    s_touch_cache.valid = false;

    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
}

bool spd2010_touch_read_first(uint16_t *x, uint16_t *y)
{
    if (!s_dev) return false;

    // Always read status first (don't gate by INT before we know the state)
    uint8_t s[4];
    if (!read_status_len_consistent(s)) return false;

    bool pt_exist = (s[0] & 0x01) != 0;
    bool in_bios = (s[1] & 0x40) != 0;
    bool in_cpu  = (s[1] & 0x20) != 0;
    bool cpu_run = (s[1] & 0x08) != 0;
    uint16_t read_len = (uint16_t)(s[3] << 8) | s[2];

    // Track last successful HDP parse time for gentle recovery
    static TickType_t s_last_hdp_ok_tick = 0;

    // Debug: log state/pt change only (reduce noise)
    static uint8_t last_state = 0xFF; // bit2:run, bit1:cpu, bit0:bios
    static int last_pt = -1;
    uint8_t cur_state = (cpu_run ? 0x4 : 0) | (in_cpu ? 0x2 : 0) | (in_bios ? 0x1 : 0);
    if (cur_state != last_state || (int)pt_exist != last_pt) {
        ESP_LOGD(TAG, "Status: BIOS=%d CPU=%d RUN=%d pt=%d len=%u", in_bios, in_cpu, cpu_run, pt_exist, (unsigned)read_len);
        last_state = cur_state;
        last_pt = (int)pt_exist;
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

    // Recovery: if we haven't successfully parsed HDP for a while, gently re-drive
    {
        TickType_t now = xTaskGetTickCount();
        if (cpu_run && s_last_hdp_ok_tick != 0 && (now - s_last_hdp_ok_tick) > pdMS_TO_TICKS(1500)) {
            write_point_mode();
            esp_rom_delay_us(200);
            write_start();
            esp_rom_delay_us(200);
            write_clear_int();
            s_last_hdp_ok_tick = now; // avoid repeated pokes
        }
    }

    // When idle (no point and no payload), we may skip work if INT is high; otherwise we gently drive
    if (cpu_run && (!pt_exist && read_len == 0)) {
        if (s_int_gpio != GPIO_NUM_NC && gpio_get_level(s_int_gpio) != 0) {
            return false;
        }
        write_point_mode();
        esp_rom_delay_us(200);
        write_start();
        esp_rom_delay_us(200);
        write_clear_int();
        return false;
    }

    if (!pt_exist || read_len < 10) {
        if (cpu_run) write_clear_int();
        return false;
    }

    // Read first packet via HDP and parse (do not gate by INT if pt_exist says there is data)
    uint8_t buf[4 + 10 * 6] = {0};
    if (rd16(REG_HDP, buf, read_len) != ESP_OK) { if (cpu_run) write_clear_int(); return false; }

    // Parse first touch point according to SPD2010 format
    uint8_t b5 = buf[5];
    uint8_t b6 = buf[6];
    uint8_t b7 = buf[7];
    uint8_t w  = buf[8];

    if (w == 0) { if (cpu_run) write_clear_int(); return false; }

    uint16_t px = (uint16_t)(((b7 & 0xF0) << 4) | b5);
    uint16_t py = (uint16_t)(((b7 & 0x0F) << 8) | b6);

    if (x) *x = px;
    if (y) *y = py;

    // After reading HDP, follow demo logic to handle HDP status and INT
    uint8_t hs[8];
    if (rd16(REG_HDP_STATUS, hs, sizeof(hs)) == ESP_OK) {
        uint8_t status = hs[5];
        uint16_t next_len = (uint16_t)(hs[2] | (hs[3] << 8));
        if (status == 0x82) {
            // Clear INT
            write_clear_int();
        } else if (status == 0x00 && next_len > 0) {
            // Read remaining data then check again
            uint8_t tmp[32];
            size_t to_read = next_len > sizeof(tmp) ? sizeof(tmp) : next_len;
            rd16(REG_HDP, tmp, to_read);
            if (rd16(REG_HDP_STATUS, hs, sizeof(hs)) == ESP_OK) {
                if (hs[5] == 0x82) write_clear_int();
            }
        }
    }
    // Mark last successful HDP parse time for recovery logic
    s_last_hdp_ok_tick = xTaskGetTickCount();
    return true;
}

void spd2010_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    bool pressed = false;
    uint16_t x = 0, y = 0;

    if (s_touch_lock) {
        if (xSemaphoreTake(s_touch_lock, 0) == pdTRUE) {
            if (s_touch_cache.valid) {
                pressed = s_touch_cache.pressed;
                x = s_touch_cache.x;
                y = s_touch_cache.y;
            }
            xSemaphoreGive(s_touch_lock);
        } else {
            // If lock is contended, use last snapshot without lock (benign read)
            if (s_touch_cache.valid) {
                pressed = s_touch_cache.pressed;
                x = s_touch_cache.x;
                y = s_touch_cache.y;
            }
        }
    } else {
        if (s_touch_cache.valid) {
            pressed = s_touch_cache.pressed;
            x = s_touch_cache.x;
            y = s_touch_cache.y;
        }
    }

    // Simple swipe detection across LVGL read cycles
    static bool s_pressed = false;
    static uint16_t s_down_x = 0, s_down_y = 0;
    static uint16_t s_last_x = 0, s_last_y = 0;
    static uint32_t s_press_start_ms = 0;
    static uint32_t s_last_move_ms = 0;
    static uint32_t s_force_release_deadline_ms = 0;

    uint32_t now_ms = lv_tick_get();
    // If we recently forced a release, temporarily mask pressed
    if (s_force_release_deadline_ms != 0) {
        if (now_ms < s_force_release_deadline_ms) {
            pressed = false;
        } else {
            s_force_release_deadline_ms = 0;
        }
    }

    // Transform raw coordinates using board config (no scaling)
    lv_display_t *disp = lv_display_get_default();
    lv_coord_t scr_w = disp ? lv_display_get_horizontal_resolution(disp) : DISPLAY_WIDTH;
    lv_coord_t scr_h = disp ? lv_display_get_vertical_resolution(disp)   : DISPLAY_HEIGHT;
    uint16_t tx = x;
    uint16_t ty = y;
    if (DISPLAY_SWAP_XY) { uint16_t t = tx; tx = ty; ty = t; }
    if (DISPLAY_MIRROR_X) { if (tx < scr_w) tx = (uint16_t)(scr_w - 1 - tx); }
    if (DISPLAY_MIRROR_Y) { if (ty < scr_h) ty = (uint16_t)(scr_h - 1 - ty); }
    if (tx >= scr_w) tx = (uint16_t)(scr_w - 1);
    if (ty >= scr_h) ty = (uint16_t)(scr_h - 1);



    if (pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = tx;
        data->point.y = ty;
        const int kDebouncePx = 3;
        if (!s_pressed) {
            s_pressed = true;
            s_down_x = s_last_x = tx;
            s_down_y = s_last_y = ty;
            s_press_start_ms = now_ms;
            s_last_move_ms = now_ms;
        } else {
            int mdx = (int)tx - (int)s_last_x;
            int mdy = (int)ty - (int)s_last_y;
            if (mdx < 0) mdx = -mdx;
            if (mdy < 0) mdy = -mdy;
            if (mdx >= kDebouncePx || mdy >= kDebouncePx) {
                s_last_x = tx;
                s_last_y = ty;
                s_last_move_ms = now_ms;
            }
        }
        // Timeout release protection: >1000ms no meaningful move
        if (s_pressed && (now_ms - s_press_start_ms > 1000) && (now_ms - s_last_move_ms > 1000)) {
            int dx = (int)s_last_x - (int)s_down_x;
            int dy = (int)s_last_y - (int)s_down_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            const int kSwipeThreshold = 20; // more sensitive
            ESP_LOGI(TAG, "Timeout release: dx=%d dy=%d from (%u,%u)->(%u,%u)", dx, dy,
                     (unsigned)s_down_x, (unsigned)s_down_y, (unsigned)s_last_x, (unsigned)s_last_y);
            if (adx > kSwipeThreshold && adx > ady) {
                extern bool app_is_slideshow_running(void);
                extern void app_slideshow_next(void);
                extern void app_slideshow_prev(void);
                if (app_is_slideshow_running()) {
                    if (dx < 0) {
                        ESP_LOGI(TAG, "Gesture: swipe left -> Next");
                        app_slideshow_next();
                    } else {
                        ESP_LOGI(TAG, "Gesture: swipe right -> Prev");
                        app_slideshow_prev();
                    }
                }
            }
            s_pressed = false;
            s_need_clear_int = true; // ask background to clear INT
            s_force_release_deadline_ms = now_ms + 150; // mask pressed briefly
            data->state = LV_INDEV_STATE_RELEASED;
        }
        // Rate-limit press logs to avoid flooding
        static uint32_t s_last_press_log = 0;
        if (now_ms - s_last_press_log > 250) {
            ESP_LOGI(TAG, "Touch PRESSED x=%u y=%u", (unsigned)tx, (unsigned)ty);
            s_last_press_log = now_ms;
        }

    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        if (s_pressed) {
            s_pressed = false;
            int dx = (int)s_last_x - (int)s_down_x;
            int dy = (int)s_last_y - (int)s_down_y;
            ESP_LOGI(TAG, "Release: dx=%d dy=%d from (%u,%u)->(%u,%u)", dx, dy,
                     (unsigned)s_down_x, (unsigned)s_down_y, (unsigned)s_last_x, (unsigned)s_last_y);
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            const int kSwipeThreshold = 20; // more sensitive
            if (adx > kSwipeThreshold && adx > ady) {
                // Bridge to application slideshow controls (C wrappers)
                extern bool app_is_slideshow_running(void);
                extern void app_slideshow_next(void);
                extern void app_slideshow_prev(void);
                if (app_is_slideshow_running()) {
                    if (dx < 0) {
                        ESP_LOGI(TAG, "Gesture: swipe left -> Next");
                        app_slideshow_next();
                    } else {
                        ESP_LOGI(TAG, "Gesture: swipe right -> Prev");
                        app_slideshow_prev();
                    }
                }
            }
        }
    }
}
