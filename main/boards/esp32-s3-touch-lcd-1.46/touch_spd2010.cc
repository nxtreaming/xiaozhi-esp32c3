// Minimal SPD2010 touch driver for LVGL integration (ESP-IDF I2C master v2)
#include "touch_spd2010.h"

#include <string.h>
#include <algorithm>
#include <cstdlib>

#include <esp_log.h>
#include <esp_rom_sys.h>

#include <lvgl.h>
#include "application.h"

static const char *TAG = "SPD2010_TOUCH";

// Registers used by SPD2010 touch protocol
static constexpr uint16_t REG_POINT_MODE   = 0x5000; // write 0x0000 -> point mode
static constexpr uint16_t REG_START        = 0x4600; // write 0x0000 -> start once
static constexpr uint16_t REG_CPU_START    = 0x0400; // write 0x0001 -> start CPU
static constexpr uint16_t REG_CLEAR_INT    = 0x0200; // write 0x0001 -> clear INT
static constexpr uint16_t REG_STATUS_LEN   = 0x2000; // read 4 bytes
static constexpr uint16_t REG_HDP          = 0x0003; // read variable len
static constexpr uint16_t REG_HDP_STATUS   = 0xFC02; // read 8 bytes

// Keep a single active instance for LVGL callback bridge
static Spd2010Touch *g_touch = nullptr;

Spd2010Touch::Spd2010Touch(i2c_master_bus_handle_t i2c_bus,
                           gpio_num_t int_gpio,
                           int screen_width,
                           int screen_height,
                           bool mirror_x,
                           bool mirror_y,
                           bool swap_xy)
    : I2cDevice(i2c_bus, SPD2010_I2C_ADDR),
      int_gpio_(int_gpio),
      w_(screen_width), h_(screen_height),
      mirror_x_(mirror_x), mirror_y_(mirror_y), swap_xy_(swap_xy) {
}

esp_err_t Spd2010Touch::Init() {
    // Configure INT as input (no IRQ required; LVGL polls)
    if (int_gpio_ != GPIO_NUM_NC) {
        gpio_reset_pin(int_gpio_);
        gpio_set_direction(int_gpio_, GPIO_MODE_INPUT);
        gpio_pullup_en(int_gpio_);
    }

    // Basic sanity: try reading FW header (optional, non-fatal)
    uint8_t tmp[4] = {0};
    esp_err_t err = ReadReg16(REG_STATUS_LEN, tmp, sizeof(tmp));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Probe failed (status read): %s", esp_err_to_name(err));
        // still continue; device may be in BIOS and need CPU start sequence
    }

    // Initialize shared state and start polling task
    state_mutex_ = xSemaphoreCreateMutex();
    if (!state_mutex_) {
        ESP_LOGE(TAG, "Failed to create touch state mutex");
        return ESP_ERR_NO_MEM;
    }
    // Use modest priority to avoid starving IDLE/LVGL tasks
    if (xTaskCreate(Spd2010Touch::TouchTask, "spd2010_touch", 3072, this, 2, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch task");
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
        return ESP_FAIL;
    }

    // Bind callback bridge
    g_touch = this;
    ESP_LOGI(TAG, "Touch task created and initialized");
    return ESP_OK;
}

void Spd2010Touch::LvglReadCb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    if (!g_touch) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // Track press/release state for simple swipe detection
    static bool s_pressed = false;
    static uint16_t s_down_x = 0, s_down_y = 0;
    static uint16_t s_last_x = 0, s_last_y = 0;

    // Use cached state updated by a background task instead of doing I2C here
    bool pressed = false; uint16_t x = 0, y = 0;
    if (g_touch->state_mutex_ && xSemaphoreTake(g_touch->state_mutex_, 0) == pdTRUE) {
        pressed = g_touch->pressed_;
        x = g_touch->cur_x_;
        y = g_touch->cur_y_;
        xSemaphoreGive(g_touch->state_mutex_);
    } else {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    if (pressed) {
        data->point.x = (lv_coord_t)std::min<int>(std::max<int>(x, 0), g_touch->w_ - 1);
        data->point.y = (lv_coord_t)std::min<int>(std::max<int>(y, 0), g_touch->h_ - 1);
        data->state = LV_INDEV_STATE_PRESSED;
        // Optional debug log (rate-limited)
        static uint32_t last_log = 0;
        uint32_t now = lv_tick_get();
        if (now - last_log > 250) {
            ESP_LOGI(TAG, "Touch PR x=%d y=%d", (int)data->point.x, (int)data->point.y);
            last_log = now;
        }
        if (!s_pressed) {
            s_pressed = true;
            s_down_x = s_last_x = (uint16_t)data->point.x;
            s_down_y = s_last_y = (uint16_t)data->point.y;
        } else {
            s_last_x = (uint16_t)data->point.x;
            s_last_y = (uint16_t)data->point.y;
        }
    } else {
        data->state = LV_INDEV_STATE_REL;
        if (s_pressed) {
            s_pressed = false;
            int dx = (int)s_last_x - (int)s_down_x;
            int dy = (int)s_last_y - (int)s_down_y;
            const int kSwipeThreshold = 40;
            if (abs(dx) > kSwipeThreshold && abs(dx) > abs(dy)) {
                if (dx < 0) {
                    if (Application::GetInstance().IsSlideShowRunning()) {
                        ESP_LOGI(TAG, "Gesture: swipe left -> Next");
                        Application::GetInstance().SlideShowNext();
                    }
                } else {
                    if (Application::GetInstance().IsSlideShowRunning()) {
                        ESP_LOGI(TAG, "Gesture: swipe right -> Prev");
                        Application::GetInstance().SlideShowPrev();
                    }
                }
            }
        }
    }
}

esp_err_t Spd2010Touch::ReadReg16(uint16_t reg, uint8_t *buf, size_t len) {
    uint8_t addr[2] = { static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF) };
    // Use small timeout to prevent long blocking in LVGL task
    return i2c_master_transmit_receive(i2c_device_, addr, 2, buf, len, 20);
}

esp_err_t Spd2010Touch::WriteReg16(uint16_t reg, const uint8_t *data, size_t len) {
    uint8_t tmp[2 + 8];
    if (len > 8) {
        // shouldn't happen in our usage; fallback to heap if needed
        uint8_t *dyn = (uint8_t *)malloc(2 + len);
        if (!dyn) return ESP_ERR_NO_MEM;
        dyn[0] = reg >> 8; dyn[1] = reg & 0xFF;
        memcpy(dyn + 2, data, len);
        esp_err_t r = i2c_master_transmit(i2c_device_, dyn, 2 + len, 20);
        free(dyn);
        return r;
    }
    tmp[0] = reg >> 8; tmp[1] = reg & 0xFF;
    if (len) memcpy(tmp + 2, data, len);
    return i2c_master_transmit(i2c_device_, tmp, 2 + len, 20);
}

esp_err_t Spd2010Touch::WritePointMode() {
    const uint8_t v[2] = {0x00, 0x00};
    esp_err_t r = WriteReg16(REG_POINT_MODE, v, 2);
    esp_rom_delay_us(200);
    return r;
}

esp_err_t Spd2010Touch::WriteStart() {
    const uint8_t v[2] = {0x00, 0x00};
    esp_err_t r = WriteReg16(REG_START, v, 2);
    esp_rom_delay_us(200);
    return r;
}

esp_err_t Spd2010Touch::WriteCpuStart() {
    const uint8_t v[2] = {0x01, 0x00};
    esp_err_t r = WriteReg16(REG_CPU_START, v, 2);
    esp_rom_delay_us(200);
    return r;
}

esp_err_t Spd2010Touch::WriteClearInt() {
    const uint8_t v[2] = {0x01, 0x00};
    esp_err_t r = WriteReg16(REG_CLEAR_INT, v, 2);
    esp_rom_delay_us(200);
    return r;
}

esp_err_t Spd2010Touch::ReadStatusLen(uint8_t *status_low, uint8_t *status_high, uint16_t *len) {
    uint8_t b[4] = {0};
    esp_err_t r = ReadReg16(REG_STATUS_LEN, b, sizeof(b));
    if (r != ESP_OK) return r;
    if (status_low)  *status_low  = b[0];
    if (status_high) *status_high = b[1];
    if (len) *len = static_cast<uint16_t>((b[3] << 8) | b[2]);
    return ESP_OK;
}

esp_err_t Spd2010Touch::ReadHdp(uint16_t read_len) {
    if (read_len > kMaxHdpBuf) read_len = kMaxHdpBuf;
    memset(hdp_buf_, 0, sizeof(hdp_buf_));
    return ReadReg16(REG_HDP, hdp_buf_, read_len);
}

esp_err_t Spd2010Touch::ReadHdpStatus(uint8_t *status, uint16_t *next_len) {
    uint8_t b[8] = {0};
    esp_err_t r = ReadReg16(REG_HDP_STATUS, b, sizeof(b));
    if (r != ESP_OK) return r;
    if (status)   *status   = b[5];
    if (next_len) *next_len = static_cast<uint16_t>((b[3] << 8) | b[2]);
    return ESP_OK;
}

esp_err_t Spd2010Touch::ReadHdpRemain(uint16_t next_len) {
    if (next_len == 0) return ESP_OK;
    uint16_t rd = std::min<uint16_t>(next_len, kMaxHdpBuf);
    return ReadReg16(REG_HDP, hdp_buf_, rd);
}

bool Spd2010Touch::ParseFirstPoint(uint16_t *x, uint16_t *y, uint8_t *points) {
    if (!x || !y) return false;
    // hdp_buf_ layout per demo: [4] is first ID, then blocks of 6 bytes per point
    // Make a quick sanity check
    uint8_t first_id = hdp_buf_[4];
    if (first_id > 0x0A) {
        if (points) *points = 0;
        return false;
    }

    // Estimate number of points available from content length: we don't have it now, but decode first one
    // First point offset = 0
    uint8_t o = 0;
    uint8_t b5 = hdp_buf_[5 + o];
    uint8_t b6 = hdp_buf_[6 + o];
    uint8_t b7 = hdp_buf_[7 + o];
    // b7: high nibbles X/Y
    uint16_t px = static_cast<uint16_t>(((b7 & 0xF0) << 4) | b5);
    uint16_t py = static_cast<uint16_t>(((b7 & 0x0F) << 8) | b6);

    // Weight 0 means no valid touch for first point
    uint8_t weight = hdp_buf_[8 + o];
    if (weight == 0) {
        if (points) *points = 0;
        return false;
    }

    if (points) *points = 1;
    *x = px; *y = py;
    return true;
}

void Spd2010Touch::Transform(uint16_t &x, uint16_t &y) const {
    int xi = x, yi = y;
    if (swap_xy_) std::swap(xi, yi);
    if (mirror_x_) xi = (w_ - 1) - xi;
    if (mirror_y_) yi = (h_ - 1) - yi;
    x = static_cast<uint16_t>(std::clamp(xi, 0, w_ - 1));
    y = static_cast<uint16_t>(std::clamp(yi, 0, h_ - 1));
}

void Spd2010Touch::TouchTask(void *arg) {
    auto self = static_cast<Spd2010Touch*>(arg);
    const TickType_t kDelay = pdMS_TO_TICKS(20);
    // Give touch controller some time to boot before first I2C access
    vTaskDelay(pdMS_TO_TICKS(300));
    int fail_count = 0;
    uint8_t prev_state = 0xFF; // track state for occasional logs
    while (true) {
        // If not yet initialized, run a light init sequence regardless of INT
        if (!self->ready_) {
            uint8_t status_low = 0, status_high = 0; uint16_t read_len = 0;
            if (self->ReadStatusLen(&status_low, &status_high, &read_len) == ESP_OK) {
                bool in_bios = (status_high & 0x40) != 0;
                bool in_cpu  = (status_high & 0x20) != 0;
                if (in_bios) {
                    self->WriteClearInt();
                    self->WriteCpuStart();
                } else if (in_cpu) {
                    self->WritePointMode();
                    self->WriteStart();
                    self->WriteClearInt();
                    // After starting point mode, mark ready
                    self->ready_ = true;
                } else {
                    // Assume running; mark ready to allow INT-gated reads
                    self->ready_ = true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // After ready: If INT line exists and is high (no new data), do NOT access I2C
        // Just mark released and wait for next loop to avoid spurious timeouts.
        if (self->int_gpio_ != GPIO_NUM_NC && gpio_get_level(self->int_gpio_) != 0) {
            if (self->state_mutex_ && xSemaphoreTake(self->state_mutex_, portMAX_DELAY) == pdTRUE) {
                self->pressed_ = false;
                xSemaphoreGive(self->state_mutex_);
            }
            vTaskDelay(kDelay);
            continue;
        }

        // Read status
        uint8_t status_low = 0, status_high = 0; uint16_t read_len = 0;
        if (self->ReadStatusLen(&status_low, &status_high, &read_len) != ESP_OK) {
            fail_count++;
            // Exponential backoff on repeated failures
            TickType_t backoff = (fail_count < 5) ? kDelay : (fail_count < 20 ? pdMS_TO_TICKS(100) : pdMS_TO_TICKS(500));
            vTaskDelay(backoff);
            continue;
        }
        fail_count = 0;

        bool in_bios = (status_high & 0x40) != 0;
        bool in_cpu  = (status_high & 0x20) != 0;
        bool pt_exist = (status_low & 0x01) != 0;
        bool gesture  = (status_low & 0x02) != 0;

        if (in_bios) {
            // Limit CPU start attempts to avoid blocking when bus is problematic
            static int s_cpu_start_tries = 0;
            static uint32_t s_last_cpu_start = 0;
            uint32_t now = lv_tick_get();
            if (s_cpu_start_tries < 3 && (now - s_last_cpu_start) > 100) {
                self->WriteClearInt();
                self->WriteCpuStart();
                s_last_cpu_start = now;
                s_cpu_start_tries++;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (in_cpu) {
            static uint32_t s_last_start = 0;
            uint32_t now = lv_tick_get();
            if (now - s_last_start > 100) {
                self->WritePointMode();
                self->WriteStart();
                self->WriteClearInt();
                s_last_start = now;
                self->ready_ = true;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // Aggressive poke like vendor demo to ensure reporting while running
        self->WritePointMode();
        self->WriteStart();
        self->WriteClearInt();
        if ((!pt_exist && !gesture) && read_len == 0) {
            self->WriteClearInt();
            if (self->state_mutex_ && xSemaphoreTake(self->state_mutex_, portMAX_DELAY) == pdTRUE) {
                self->pressed_ = false;
                xSemaphoreGive(self->state_mutex_);
            }
            vTaskDelay(kDelay);
            continue;
        }

        if (pt_exist || gesture) {
            if (read_len > kMaxHdpBuf) read_len = kMaxHdpBuf;
            if (self->ReadHdp(read_len) == ESP_OK) {
                uint16_t x = 0, y = 0; uint8_t points = 0;
                if (self->ParseFirstPoint(&x, &y, &points) && points > 0) {
                    self->Transform(x, y);
                    if (self->state_mutex_ && xSemaphoreTake(self->state_mutex_, portMAX_DELAY) == pdTRUE) {
                        self->cur_x_ = x; self->cur_y_ = y; self->pressed_ = true;
                        xSemaphoreGive(self->state_mutex_);
                    }
                }
            }
            // Drain HDP status/remain
            uint8_t hdp_status = 0; uint16_t next_len = 0;
            if (self->ReadHdpStatus(&hdp_status, &next_len) == ESP_OK) {
                if (hdp_status == 0x82) {
                    self->WriteClearInt();
                } else if (hdp_status == 0x00) {
                    if (next_len) self->ReadHdpRemain(next_len);
                    if (self->ReadHdpStatus(&hdp_status, &next_len) == ESP_OK && hdp_status == 0x82) {
                        self->WriteClearInt();
                    }
                }
            }
        }

        // Occasional state log (once per 2s or on change)
        uint8_t state_id = (in_bios ? 1 : (in_cpu ? 2 : (pt_exist ? 3 : 0)));
        static uint32_t last_state_log = 0;
        uint32_t now = lv_tick_get();
        if (state_id != prev_state || (now - last_state_log) > 2000) {
            prev_state = state_id; last_state_log = now;
            const char* st = state_id==1?"BIOS": state_id==2?"CPU_RUN": state_id==3?"PT_EXIST":"IDLE";
            ESP_LOGI(TAG, "TP state=%s len=%u", st, (unsigned)read_len);
        }

        vTaskDelay(kDelay);
    }
}
