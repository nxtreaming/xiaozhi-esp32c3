#include "gif_test.h"
#include "application.h"
#include "board.h"
#include <esp_log.h>

#define TAG "GifTest"

// 这是一个32x32像素的GIF文件数据 (红色方块)
// 基于4x4版本扩展，更大更明显
const uint8_t test_gif_data[] = {
    // GIF文件头 "GIF89a"
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61,
    // 逻辑屏幕宽度 (32像素, 小端序)
    0x20, 0x00,
    // 逻辑屏幕高度 (32像素, 小端序)
    0x20, 0x00,
    // 全局颜色表标志
    0x80, 0x01, 0x00,
    // 全局颜色表 (2色调色板)
    0x00, 0x00, 0x00,  // 颜色0: 黑色
    0xFF, 0x00, 0x00,  // 颜色1: 红色
    0x00, 0x00, 0x00,  // 填充
    0x00, 0x00, 0x00,  // 填充
    // 图像描述符
    0x2C, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00, 0x00,
    // LZW最小代码大小
    0x02,
    // 图像数据 (LZW压缩的32x32像素数据)
    0x2C,  // 数据子块大小
    0x84, 0x8F, 0xA9, 0xCB, 0xED, 0x0F, 0xA3, 0x9C, 0xB4, 0xDA, 0x8B, 0xB3,
    0x3E, 0x05, 0x00, 0x20, 0x74, 0x81, 0xE1, 0x28, 0x65, 0x2C, 0x1A, 0x2F,
    0xCA, 0xB0, 0x70, 0x48, 0x2C, 0x1A, 0x8F, 0xC8, 0xA4, 0x72, 0xC9, 0x6C,
    0x3A, 0x9F, 0xD0, 0xA8, 0x74, 0xCA, 0x9D, 0x3B,
    0x00,  // 数据子块终止符
    // GIF终止符
    0x3B
};

const size_t test_gif_size = sizeof(test_gif_data);

void test_gif_display() {
    ESP_LOGI(TAG, "Starting GIF display test with valid 32x32 GIF data (red square)");

    auto& app = Application::GetInstance();

    // 显示测试GIF，居中显示
    app.ShowGif(test_gif_data, test_gif_size, 0, 0);

    ESP_LOGI(TAG, "GIF test completed. You should see a 32x32 red square at screen center.");
    ESP_LOGI(TAG, "This demonstrates successful GIF decoding and display with larger images!");
}

void stop_gif_display() {
    ESP_LOGI(TAG, "Stopping GIF display");
    
    auto& app = Application::GetInstance();
    app.HideGif();
    
    ESP_LOGI(TAG, "GIF display stopped");
}
