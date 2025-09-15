// GIF URL测试示例
// 演示如何从HTTP/HTTPS URL加载和显示GIF

#include "gif_test.h"
#include "application.h"
#include <esp_log.h>
#include <cstring>

static const char* TAG = "GifUrlTest";

// 测试用的GIF URL示例
void test_sample_gif_urls() {
    ESP_LOGI(TAG, "Testing GIF loading from various URLs");
    
    // 示例1: 指定的标准GIF (HTTP)
    const char* small_gif_url = "http://122.51.57.185:18080/412_think.gif";
    ESP_LOGI(TAG, "Testing small GIF from: %s", small_gif_url);
    test_gif_from_url(small_gif_url);
    
    // 等待一段时间让用户观察
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // 示例2: 可以添加更多测试URL
    // const char* another_gif_url = "https://example.com/test.gif";
    // test_gif_from_url(another_gif_url);
}

// 通过串口命令测试自定义URL
void test_custom_gif_url(const char* custom_url) {
    if (custom_url == nullptr || strlen(custom_url) == 0) {
        ESP_LOGE(TAG, "Please provide a valid GIF URL");
        ESP_LOGI(TAG, "Usage example: test_gif_url http://example.com/animation.gif");
        return;
    }
    
    ESP_LOGI(TAG, "Testing custom GIF URL: %s", custom_url);
    test_gif_from_url(custom_url);
}

// 方便直接调用的默认测试入口
void test_gif_url_default() {
    const char* url = "http://122.51.57.185:18080/412_think.gif";
    ESP_LOGI(TAG, "Testing default GIF URL: %s", url);
    test_gif_from_url(url);
}
