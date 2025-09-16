#include "gif_test.h"
#include "application.h"
#include <esp_log.h>
#include <cstring>

static const char* TAG = "GifTest";

void test_gif_from_url(const char* url) {
    ESP_LOGI(TAG, "Starting GIF display test from URL: %s", url ? url : "NULL");

    if (url == nullptr || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid URL provided");
        return;
    }

    auto& app = Application::GetInstance();

    // 显示来自URL的GIF，居中显示
    app.ShowGifFromUrl(url, 0, 0);

    ESP_LOGI(TAG, "URL GIF test completed. Check display for downloaded GIF!");
    ESP_LOGI(TAG, "This demonstrates ESP32-S3 + PSRAM's HTTP/HTTPS GIF loading capabilities!");
}
