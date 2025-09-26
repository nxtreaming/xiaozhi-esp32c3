#include "vendor_touch.h"
#include <esp_log.h>
#include <esp_check.h>
#include <string.h>
#include <esp_rom_sys.h>

static const char *TAG = "SPD2010_VND";
static i2c_master_dev_handle_t s_dev = NULL;
static gpio_num_t s_int_gpio = GPIO_NUM_NC;
static bool s_ready = false;
static bool s_cpu_run = false;


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

bool spd2010_touch_read_first(uint16_t *x, uint16_t *y)
{
    if (!s_dev) return false;

    // If controller is known-ready and INT is high (no new data), skip I2C entirely
    if (s_ready && s_int_gpio != GPIO_NUM_NC && gpio_get_level(s_int_gpio) != 0) {
        return false;
    }

    // Read status
    uint8_t s[4];
    if (rd16(REG_STATUS_LEN, s, sizeof(s)) != ESP_OK) return false;

    bool pt_exist = (s[0] & 0x01) != 0;
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

    // Optional INT gating only when CPU is running (optimization)
    if (s_int_gpio != GPIO_NUM_NC && gpio_get_level(s_int_gpio) != 0 && cpu_run) {
        return false;
    }

    if (!pt_exist || read_len < 10) {
        if (cpu_run) write_clear_int();
        return false;
    }

    // Read first packet via HDP and parse
    uint8_t buf[4 + 10 * 6] = {0};
    if (rd16(REG_HDP, buf, read_len) != ESP_OK) return false;

    // Parse first touch point according to SPD2010 format
    uint8_t b5 = buf[5];
    uint8_t b6 = buf[6];
    uint8_t b7 = buf[7];
    uint8_t w  = buf[8];

    if (w == 0) return false;

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

    return true;
}

void spd2010_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x=0, y=0;
    bool pressed = spd2010_touch_read_first(&x, &y);
    if (pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        ESP_LOGI(TAG, "Touch PRESSED x=%u y=%u", (unsigned)x, (unsigned)y);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        // Only log release events occasionally to avoid spam
        static int release_count = 0;
        if (++release_count % 100 == 0) {
            ESP_LOGD(TAG, "Touch RELEASED (logged every 100 calls)");
        }
    }
}
