#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <driver/uart.h>
#include "application.h"
#include "system_info.h"
// #include "assets/lang_config.h"
// #include "settings.h"
// #include "board.h"
// #include "display.h"
// #include "lcd_display.h"
// #include "lvgl.h"

#include <esp_efuse_table.h>
#include "YT_UART.h"
#include "PFS123.h"
#define TAG "main"
void set_gpio() {
    esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);
     // 配置GPIO11为输出
     gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_11),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
}
extern "C" void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    set_gpio(); //初始化电平
    gpio_set_level(GPIO_NUM_11, 1); //功放角失能  1
    // Launch the application
    Application::GetInstance().Start();
    // 
    YT_init();
    PFS123_init();
    // The main thread will exit and release the stack memory
}


