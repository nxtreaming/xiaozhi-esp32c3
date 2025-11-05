#include "iot/image_upload_control.h"
#include "application.h"
#include "esp_log.h"

static const char* TAG = "ImageUploadControl";

namespace iot {

ImageUploadControl::ImageUploadControl() : Thing("ImageUploadControl") {
    // 添加方法
    AddMethod("StartImageUploadServer", [this](const std::string& params) -> std::string {
        return StartImageUploadServer(params);
    });
    
    AddMethod("StopImageUploadServer", [this](const std::string& params) -> std::string {
        return StopImageUploadServer(params);
    });
    
    AddMethod("GetImageUploadServerStatus", [this](const std::string& params) -> std::string {
        return GetImageUploadServerStatus(params);
    });
    
    // 添加状态属性
    AddProperty("server_running", "false");
    AddProperty("server_ssid", "");
    AddProperty("server_url", "");
    AddProperty("connected_clients", "0");
    
    ESP_LOGI(TAG, "ImageUploadControl initialized");
}

std::string ImageUploadControl::StartImageUploadServer(const std::string& params) {
    ESP_LOGI(TAG, "Starting image upload server with params: %s", params.c_str());
    
    auto& app = Application::GetInstance();
    
    // 解析参数，如果有的话
    std::string ssid_prefix = "ImageUpload";
    if (!params.empty() && params != "{}") {
        // 简单解析JSON参数
        size_t prefix_pos = params.find("\"ssid_prefix\":");
        if (prefix_pos != std::string::npos) {
            size_t start = params.find("\"", prefix_pos + 14);
            size_t end = params.find("\"", start + 1);
            if (start != std::string::npos && end != std::string::npos) {
                ssid_prefix = params.substr(start + 1, end - start - 1);
            }
        }
    }
    
    bool success = app.StartImageUploadServer(ssid_prefix);
    
    if (success) {
        // 更新状态
        SetProperty("server_running", "true");
        SetProperty("server_ssid", app.GetImageUploadServerInfo());
        SetProperty("server_url", "http://192.168.4.1");
        SetProperty("connected_clients", "0");
        
        ESP_LOGI(TAG, "Image upload server started successfully");
        return "{\"success\": true, \"message\": \"图片上传服务已启动\", \"ssid\": \"" + 
               app.GetImageUploadServerInfo() + "\", \"url\": \"http://192.168.4.1\"}";
    } else {
        ESP_LOGE(TAG, "Failed to start image upload server");
        return "{\"success\": false, \"message\": \"启动图片上传服务失败\"}";
    }
}

std::string ImageUploadControl::StopImageUploadServer(const std::string& params) {
    ESP_LOGI(TAG, "Stopping image upload server");
    
    auto& app = Application::GetInstance();
    app.StopImageUploadServer();
    
    // 更新状态
    SetProperty("server_running", "false");
    SetProperty("server_ssid", "");
    SetProperty("server_url", "");
    SetProperty("connected_clients", "0");
    
    ESP_LOGI(TAG, "Image upload server stopped");
    return "{\"success\": true, \"message\": \"图片上传服务已停止\"}";
}

std::string ImageUploadControl::GetImageUploadServerStatus(const std::string& params) {
    auto& app = Application::GetInstance();
    bool running = app.IsImageUploadServerRunning();
    
    if (running) {
        std::string info = app.GetImageUploadServerInfo();
        return "{\"running\": true, \"message\": \"图片上传服务运行中\", \"ssid\": \"" + 
               info + "\", \"url\": \"http://192.168.4.1\"}";
    } else {
        return "{\"running\": false, \"message\": \"图片上传服务未运行\"}";
    }
}

// 创建Thing的工厂函数
std::unique_ptr<Thing> CreateImageUploadControl() {
    return std::make_unique<ImageUploadControl>();
}

} // namespace iot
