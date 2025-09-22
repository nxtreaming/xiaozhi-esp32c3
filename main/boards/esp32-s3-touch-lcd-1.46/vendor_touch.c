#include "vendor_touch.h"
#include <esp_log.h>
#include <esp_check.h>
#include <string.h>
#include <esp_rom_sys.h>

static const char *TAG = "SPD2010_VND";
static i2c_master_dev_handle_t s_dev = NULL;
static gpio_num_t s_int_gpio = GPIO_NUM_NC;

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
    return i2c_master_transmit(s_dev, buf, 2 + len, 20);
}

static esp_err_t rd16(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, addr, 2, data, len, 20);
}

static inline esp_err_t write_point_mode(void) { uint8_t v[2] = {0,0}; return wr16(REG_POINT_MODE, v, 2); }
static inline esp_err_t write_start(void)      { uint8_t v[2] = {0,0}; return wr16(REG_START, v, 2); }
static inline esp_err_t write_cpu_start(void)  { uint8_t v[2] = {1,0}; return wr16(REG_CPU_START, v, 2); }
static inline esp_err_t write_clear_int(void)  { uint8_t v[2] = {1,0}; return wr16(REG_CLEAR_INT, v, 2); }

esp_err_t spd2010_touch_init(i2c_master_bus_handle_t bus, gpio_num_t int_gpio)
{
    ESP_LOGI(TAG, "Initializing SPD2010 touch controller...");
    ESP_LOGI(TAG, "I2C address: 0x%02X, INT GPIO: %d", SPD2010_ADDR, int_gpio);
    
    // Wait for touch controller to be ready after hardware reset
    ESP_LOGI(TAG, "Waiting for touch controller to be ready after reset...");
    esp_rom_delay_us(500000); // 500ms additional wait after TCA9554 reset
    
    s_int_gpio = int_gpio;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SPD2010_ADDR,
        .scl_speed_hz = 100 * 1000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add dev");
    
    if (s_int_gpio != GPIO_NUM_NC) {
        ESP_LOGI(TAG, "Configuring interrupt pin GPIO_%d", s_int_gpio);
        gpio_reset_pin(s_int_gpio);
        gpio_set_direction(s_int_gpio, GPIO_MODE_INPUT);
        gpio_pullup_en(s_int_gpio);
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
    
    // Try multiple initialization approaches
    bool success = false;
    
    // Approach 1: Standard sequence with longer delays
    ESP_LOGI(TAG, "Trying standard initialization sequence...");
    write_clear_int();
    esp_rom_delay_us(100000); // 100ms
    
    write_cpu_start();
    esp_rom_delay_us(200000); // 200ms - much longer wait
    
    // Check status
    if (rd16(REG_STATUS_LEN, test_status, sizeof(test_status)) == ESP_OK) {
        bool in_bios = (test_status[1] & 0x40) != 0;
        bool in_cpu  = (test_status[1] & 0x20) != 0;
        ESP_LOGI(TAG, "After standard init: BIOS=%d, CPU=%d", in_bios, in_cpu);
        
        if (in_cpu && !in_bios) {
            success = true;
            ESP_LOGI(TAG, "Standard initialization successful");
        }
    }
    
    // Approach 2: If standard failed, try alternative sequence
    if (!success) {
        ESP_LOGI(TAG, "Standard init failed, trying alternative sequence...");
        
        // Try writing different values to CPU_START register
        uint8_t alt_cmd[2] = {0x00, 0x01}; // Try different byte order
        wr16(REG_CPU_START, alt_cmd, 2);
        esp_rom_delay_us(200000);
        
        if (rd16(REG_STATUS_LEN, test_status, sizeof(test_status)) == ESP_OK) {
            bool in_bios = (test_status[1] & 0x40) != 0;
            bool in_cpu  = (test_status[1] & 0x20) != 0;
            ESP_LOGI(TAG, "After alternative init: BIOS=%d, CPU=%d", in_bios, in_cpu);
            
            if (in_cpu && !in_bios) {
                success = true;
                ESP_LOGI(TAG, "Alternative initialization successful");
            }
        }
    }
    
    // If we got to CPU mode, try to enter RUN mode
    if (success) {
        write_point_mode();
        esp_rom_delay_us(50000);
        write_start();
        esp_rom_delay_us(50000);
        write_clear_int();
        esp_rom_delay_us(50000);
        
        // Check final state
        if (rd16(REG_STATUS_LEN, test_status, sizeof(test_status)) == ESP_OK) {
            bool in_bios = (test_status[1] & 0x40) != 0;
            bool in_cpu  = (test_status[1] & 0x20) != 0;
            ESP_LOGI(TAG, "Final state: BIOS=%d, CPU=%d", in_bios, in_cpu);
            
            if (!in_bios && !in_cpu) {
                ESP_LOGI(TAG, "Touch controller is now in RUN mode - ready for touch detection");
            } else {
                ESP_LOGW(TAG, "Touch controller in intermediate state, but may still work");
            }
        }
    } else {
        ESP_LOGW(TAG, "Could not initialize touch controller properly");
        ESP_LOGW(TAG, "Touch may still work in basic polling mode");
    }
    
    ESP_LOGI(TAG, "SPD2010 touch controller initialized successfully");
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
    
    // Optional INT gating: if present and high, skip
    if (s_int_gpio != GPIO_NUM_NC && gpio_get_level(s_int_gpio) != 0) return false;

    // Standard SPD2010 read sequence
    uint8_t s[4];
    if (rd16(REG_STATUS_LEN, s, sizeof(s)) != ESP_OK) return false;
    
    bool pt_exist = (s[0] & 0x01) != 0;
    uint16_t read_len = (uint16_t)(s[3] << 8) | s[2];
    
    if (!pt_exist || read_len < 10) { 
        write_clear_int(); 
        return false; 
    }

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
