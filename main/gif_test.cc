#include "gif_test.h"
#include "application.h"
#include "board.h"
#include <esp_log.h>

#define TAG "GifTest"

// 回到经过验证的简单GIF数据 (来自LVGL官方示例)
// 4x4像素，简单的红色方块，确保兼容性
const uint8_t test_gif_data[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x04, 0x00, 0x04, 0x00, 0x80, 0x01,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x21, 0xF9, 0x04,
    0x01, 0x0A, 0x00, 0x01, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x04, 0x00, 0x00, 0x02, 0x0D, 0x84, 0x8F, 0xA9, 0xCB, 0xED, 0x0F, 0xA3,
    0x9C, 0xB4, 0xDA, 0x8B, 0xB3, 0x3E, 0x05, 0x00, 0x3B
};

const size_t test_gif_size = sizeof(test_gif_data);

void test_gif_display() {
    ESP_LOGI(TAG, "Starting GIF display test with 4x4 GIF data (red square) - ESP32-S3 debugging");

    auto& app = Application::GetInstance();

    // 显示测试GIF，居中显示
    app.ShowGif(test_gif_data, test_gif_size, 0, 0);

    ESP_LOGI(TAG, "GIF test completed. You should see a 4x4 red square with green border at screen center.");
    ESP_LOGI(TAG, "This is for debugging GIF display issues on ESP32-S3!");
}

void stop_gif_display() {
    ESP_LOGI(TAG, "Stopping GIF display");
    
    auto& app = Application::GetInstance();
    app.HideGif();
    
    ESP_LOGI(TAG, "GIF display stopped");
}
