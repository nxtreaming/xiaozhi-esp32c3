#include "iot/image_storage_control.h"
#include "application.h"
#include "storage/gif_storage.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>

static const char* TAG = "ImageStorageControl";

namespace iot {

ImageStorageControl::ImageStorageControl() : Thing("ImageStorageControl") {
    // 添加方法
    AddMethod("ListStoredImages", [this](const std::string& params) -> std::string {
        return ListStoredImages(params);
    });
    
    AddMethod("ShowStoredImage", [this](const std::string& params) -> std::string {
        return ShowStoredImage(params);
    });
    
    AddMethod("DeleteStoredImage", [this](const std::string& params) -> std::string {
        return DeleteStoredImage(params);
    });
    
    AddMethod("GetStorageInfo", [this](const std::string& params) -> std::string {
        return GetStorageInfo(params);
    });
    
    AddMethod("ClearAllImages", [this](const std::string& params) -> std::string {
        return ClearAllImages(params);
    });
    
    // 添加状态属性
    AddProperty("storage_initialized", "false");
    AddProperty("total_images", "0");
    AddProperty("storage_usage_percent", "0");
    
    // 初始化存储状态
    UpdateStorageStatus();
    
    ESP_LOGI(TAG, "ImageStorageControl initialized");
}

std::string ImageStorageControl::ListStoredImages(const std::string& params) {
    ESP_LOGI(TAG, "Listing stored images");
    
    std::vector<std::string> files;
    esp_err_t ret = gif_storage_list_files(files);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to list files: %s", esp_err_to_name(ret));
        return "{\"success\": false, \"message\": \"无法读取存储文件\"}";
    }
    
    if (files.empty()) {
        return "{\"success\": true, \"message\": \"没有存储的图片\", \"files\": []}";
    }
    
    // 构建JSON响应
    std::string response = "{\"success\": true, \"message\": \"找到 " + 
                          std::to_string(files.size()) + " 个图片文件\", \"files\": [";
    
    for (size_t i = 0; i < files.size(); i++) {
        response += "\"" + files[i] + "\"";
        if (i < files.size() - 1) {
            response += ", ";
        }
    }
    response += "]}";
    
    // 更新状态
    SetProperty("total_images", std::to_string(files.size()));
    
    ESP_LOGI(TAG, "Listed %d stored images", files.size());
    return response;
}

std::string ImageStorageControl::ShowStoredImage(const std::string& params) {
    ESP_LOGI(TAG, "Showing stored image with params: %s", params.c_str());
    
    // 解析文件名参数
    std::string filename;
    size_t filename_pos = params.find("\"filename\":");
    if (filename_pos != std::string::npos) {
        size_t start = params.find("\"", filename_pos + 11);
        size_t end = params.find("\"", start + 1);
        if (start != std::string::npos && end != std::string::npos) {
            filename = params.substr(start + 1, end - start - 1);
        }
    }
    
    if (filename.empty()) {
        return "{\"success\": false, \"message\": \"请指定要显示的图片文件名\"}";
    }
    
    // 检查文件是否存在
    if (!gif_storage_exists(filename.c_str())) {
        return "{\"success\": false, \"message\": \"图片文件不存在: " + filename + "\"}";
    }
    
    // 读取并显示图片
    uint8_t* data = nullptr;
    size_t size = 0;
    esp_err_t ret = gif_storage_read(filename.c_str(), &data, &size);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read image: %s", esp_err_to_name(ret));
        return "{\"success\": false, \"message\": \"读取图片失败: " + filename + "\"}";
    }
    
    // 显示图片
    auto& app = Application::GetInstance();
    app.Schedule([&app, data, size, filename]() {
        if (auto display = Board::GetInstance().GetDisplay()) {
            display->ShowGif(data, size);
            display->ShowNotification(("正在显示: " + filename).c_str(), 2000);
        }
        free(data); // 释放内存
    });
    
    ESP_LOGI(TAG, "Showing image: %s (%d bytes)", filename.c_str(), size);
    return "{\"success\": true, \"message\": \"正在显示图片: " + filename + "\"}";
}

std::string ImageStorageControl::DeleteStoredImage(const std::string& params) {
    ESP_LOGI(TAG, "Deleting stored image with params: %s", params.c_str());
    
    // 解析文件名参数
    std::string filename;
    size_t filename_pos = params.find("\"filename\":");
    if (filename_pos != std::string::npos) {
        size_t start = params.find("\"", filename_pos + 11);
        size_t end = params.find("\"", start + 1);
        if (start != std::string::npos && end != std::string::npos) {
            filename = params.substr(start + 1, end - start - 1);
        }
    }
    
    if (filename.empty()) {
        return "{\"success\": false, \"message\": \"请指定要删除的图片文件名\"}";
    }
    
    // 检查文件是否存在
    if (!gif_storage_exists(filename.c_str())) {
        return "{\"success\": false, \"message\": \"图片文件不存在: " + filename + "\"}";
    }
    
    // 删除文件
    esp_err_t ret = gif_storage_delete(filename.c_str());
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete image: %s", esp_err_to_name(ret));
        return "{\"success\": false, \"message\": \"删除图片失败: " + filename + "\"}";
    }
    
    // 更新存储状态
    UpdateStorageStatus();
    
    ESP_LOGI(TAG, "Deleted image: %s", filename.c_str());
    return "{\"success\": true, \"message\": \"已删除图片: " + filename + "\"}";
}

std::string ImageStorageControl::GetStorageInfo(const std::string& params) {
    ESP_LOGI(TAG, "Getting storage info");
    
    size_t total_bytes = 0, used_bytes = 0;
    esp_err_t ret = gif_storage_get_info(&total_bytes, &used_bytes);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get storage info: %s", esp_err_to_name(ret));
        return "{\"success\": false, \"message\": \"无法获取存储信息\"}";
    }
    
    // 获取文件数量
    std::vector<std::string> files;
    gif_storage_list_files(files);
    
    // 计算使用百分比
    int usage_percent = (total_bytes > 0) ? (used_bytes * 100 / total_bytes) : 0;
    
    // 转换为可读格式
    auto format_bytes = [](size_t bytes) -> std::string {
        if (bytes >= 1024 * 1024) {
            return std::to_string(bytes / (1024 * 1024)) + "MB";
        } else if (bytes >= 1024) {
            return std::to_string(bytes / 1024) + "KB";
        } else {
            return std::to_string(bytes) + "B";
        }
    };
    
    std::string response = "{\"success\": true, \"message\": \"存储信息\", "
                          "\"total_size\": \"" + format_bytes(total_bytes) + "\", "
                          "\"used_size\": \"" + format_bytes(used_bytes) + "\", "
                          "\"free_size\": \"" + format_bytes(total_bytes - used_bytes) + "\", "
                          "\"usage_percent\": " + std::to_string(usage_percent) + ", "
                          "\"total_images\": " + std::to_string(files.size()) + "}";
    
    // 更新状态属性
    SetProperty("storage_usage_percent", std::to_string(usage_percent));
    SetProperty("total_images", std::to_string(files.size()));
    
    ESP_LOGI(TAG, "Storage info: %s used, %s total, %d files", 
             format_bytes(used_bytes).c_str(), format_bytes(total_bytes).c_str(), files.size());
    
    return response;
}

std::string ImageStorageControl::ClearAllImages(const std::string& params) {
    ESP_LOGI(TAG, "Clearing all images");
    
    std::vector<std::string> files;
    esp_err_t ret = gif_storage_list_files(files);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to list files for clearing: %s", esp_err_to_name(ret));
        return "{\"success\": false, \"message\": \"无法读取存储文件\"}";
    }
    
    if (files.empty()) {
        return "{\"success\": true, \"message\": \"没有图片需要清空\"}";
    }
    
    int deleted_count = 0;
    int failed_count = 0;
    
    for (const auto& filename : files) {
        esp_err_t delete_ret = gif_storage_delete(filename.c_str());
        if (delete_ret == ESP_OK) {
            deleted_count++;
        } else {
            failed_count++;
            ESP_LOGW(TAG, "Failed to delete %s: %s", filename.c_str(), esp_err_to_name(delete_ret));
        }
    }
    
    // 更新存储状态
    UpdateStorageStatus();
    
    std::string message = "已删除 " + std::to_string(deleted_count) + " 个图片";
    if (failed_count > 0) {
        message += "，" + std::to_string(failed_count) + " 个删除失败";
    }
    
    ESP_LOGI(TAG, "Cleared images: %d deleted, %d failed", deleted_count, failed_count);
    return "{\"success\": true, \"message\": \"" + message + "\"}";
}

void ImageStorageControl::UpdateStorageStatus() {
    size_t total_bytes = 0, used_bytes = 0;
    esp_err_t ret = gif_storage_get_info(&total_bytes, &used_bytes);
    
    if (ret == ESP_OK) {
        SetProperty("storage_initialized", "true");
        int usage_percent = (total_bytes > 0) ? (used_bytes * 100 / total_bytes) : 0;
        SetProperty("storage_usage_percent", std::to_string(usage_percent));
        
        std::vector<std::string> files;
        gif_storage_list_files(files);
        SetProperty("total_images", std::to_string(files.size()));
    } else {
        SetProperty("storage_initialized", "false");
        SetProperty("storage_usage_percent", "0");
        SetProperty("total_images", "0");
    }
}

// 创建Thing的工厂函数
std::unique_ptr<Thing> CreateImageStorageControl() {
    return std::make_unique<ImageStorageControl>();
}

} // namespace iot
