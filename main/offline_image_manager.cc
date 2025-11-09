#include "offline_image_manager.h"
#include "application.h"
#include "storage/gif_storage.h"
#include "image_upload_server.h"
#include <esp_log.h>
#include <algorithm>

#define TAG "OfflineImageManager"

OfflineImageManager& OfflineImageManager::GetInstance() {
    static OfflineImageManager instance;
    return instance;
}

void OfflineImageManager::Initialize() {
    ESP_LOGI(TAG, "Initializing offline image manager");
    UpdateImageList();
}

void OfflineImageManager::SetStatusCallback(StatusCallback callback) {
    status_callback_ = callback;
}

void OfflineImageManager::ShowStatus(const std::string& message) {
    ESP_LOGI(TAG, "Status: %s", message.c_str());
    if (status_callback_) {
        status_callback_(message);
    }
}

bool OfflineImageManager::StartImageUploadService(const std::string& ssid_prefix) {
    ESP_LOGI(TAG, "Starting image upload service with SSID prefix: %s", ssid_prefix.c_str());
    
    auto& app = Application::GetInstance();
    bool success = app.StartImageUploadServer(ssid_prefix);
    
    if (success) {
        button_state_ = ButtonState::SERVICE_RUNNING;
        std::string info = app.GetImageUploadServerInfo();
        ShowStatus("图片上传服务已启动\n" + info);
        return true;
    } else {
        ShowStatus("图片上传服务启动失败");
        return false;
    }
}

void OfflineImageManager::StopImageUploadService() {
    ESP_LOGI(TAG, "Stopping image upload service");
    
    auto& app = Application::GetInstance();
    app.StopImageUploadServer();
    
    button_state_ = ButtonState::IDLE;
    ShowStatus("图片上传服务已停止");
}

bool OfflineImageManager::IsImageUploadServiceRunning() const {
    auto& app = Application::GetInstance();
    return app.IsImageUploadServerRunning();
}

std::string OfflineImageManager::GetImageUploadServiceInfo() const {
    auto& app = Application::GetInstance();
    return app.GetImageUploadServerInfo();
}

std::vector<OfflineImageManager::ImageInfo> OfflineImageManager::GetStoredImages() {
    std::vector<ImageInfo> images;
    
    gif_storage_list([](const char* filename, size_t size, void* user_data) {
        auto* images_ptr = static_cast<std::vector<ImageInfo>*>(user_data);
        
        ImageInfo info;
        info.filename = filename;
        info.size = size;
        info.display_name = filename;
        
        // 简化显示名称
        if (info.display_name.length() > 20) {
            info.display_name = info.display_name.substr(0, 17) + "...";
        }
        
        images_ptr->push_back(info);
    }, &images);
    
    // 按文件名排序
    std::sort(images.begin(), images.end(), 
        [](const ImageInfo& a, const ImageInfo& b) {
            return a.filename < b.filename;
        });
    
    return images;
}

bool OfflineImageManager::DeleteStoredImage(const std::string& filename) {
    ESP_LOGI(TAG, "Deleting stored image: %s", filename.c_str());
    
    if (!gif_storage_exists(filename.c_str())) {
        ShowStatus("图片文件不存在: " + filename);
        return false;
    }
    
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/storage/%s", filename.c_str());
    
    if (unlink(filepath) == 0) {
        UpdateImageList();
        ShowStatus("已删除图片: " + filename);
        return true;
    } else {
        ShowStatus("删除图片失败: " + filename);
        return false;
    }
}

int OfflineImageManager::ClearAllImages() {
    ESP_LOGI(TAG, "Clearing all stored images");
    
    int deleted_count = 0;
    auto images = GetStoredImages();
    
    for (const auto& image : images) {
        char filepath[256];
        snprintf(filepath, sizeof(filepath), "/storage/%s", image.filename.c_str());
        
        if (unlink(filepath) == 0) {
            deleted_count++;
            ESP_LOGI(TAG, "Deleted: %s", image.filename.c_str());
        }
    }
    
    UpdateImageList();
    
    if (deleted_count > 0) {
        ShowStatus("已删除 " + std::to_string(deleted_count) + " 个图片文件");
    } else {
        ShowStatus("没有图片文件需要删除");
    }
    
    return deleted_count;
}

bool OfflineImageManager::GetStorageInfo(size_t& total_bytes, size_t& used_bytes) {
    esp_err_t ret = gif_storage_info(&total_bytes, &used_bytes);
    return ret == ESP_OK;
}

void OfflineImageManager::UpdateImageList() {
    current_images_ = GetStoredImages();
    current_image_index_ = 0;
}

void OfflineImageManager::HandleButtonPress() {
    ESP_LOGI(TAG, "Button press - current state: %d", (int)button_state_);
    
    switch (button_state_) {
        case ButtonState::IDLE:
            // 启动图片上传服务
            StartImageUploadService();
            break;
            
        case ButtonState::SERVICE_RUNNING:
            // 切换到浏览图片模式
            StopImageUploadService();
            UpdateImageList();
            if (!current_images_.empty()) {
                button_state_ = ButtonState::BROWSING_IMAGES;
                current_image_index_ = 0;
                // 启动slideshow，用户可以通过手势切换
                auto& app = Application::GetInstance();
                app.SlideShow();
                ShowStatus("图片播放模式 - 滑动切换图片 (" + std::to_string(current_images_.size()) + " 张)");
            } else {
                ShowStatus("没有存储的图片文件");
                button_state_ = ButtonState::IDLE;
            }
            break;
            
        case ButtonState::BROWSING_IMAGES:
            // 切换到下一张图片（通过slideshow手势控制）
            if (!current_images_.empty()) {
                auto& app = Application::GetInstance();
                app.SlideShowNext();  // 使用slideshow的下一张功能
                current_image_index_ = (current_image_index_ + 1) % current_images_.size();
                ShowStatus("下一张图片 (" + std::to_string(current_image_index_ + 1) +
                          "/" + std::to_string(current_images_.size()) + ")");
            } else {
                ShowStatus("没有图片文件");
                button_state_ = ButtonState::IDLE;
            }
            break;
    }
}

void OfflineImageManager::HandleButtonLongPress() {
    ESP_LOGI(TAG, "Button long press - current state: %d", (int)button_state_);
    
    switch (button_state_) {
        case ButtonState::IDLE:
            // 显示存储信息
            {
                size_t total_bytes, used_bytes;
                if (GetStorageInfo(total_bytes, used_bytes)) {
                    size_t free_bytes = total_bytes - used_bytes;
                    double used_percent = (double)used_bytes / total_bytes * 100.0;
                    
                    std::string info = "存储信息:\n";
                    info += "总容量: " + std::to_string(total_bytes / 1024) + " KB\n";
                    info += "已使用: " + std::to_string(used_bytes / 1024) + " KB (" + 
                           std::to_string((int)used_percent) + "%)\n";
                    info += "可用: " + std::to_string(free_bytes / 1024) + " KB\n";
                    info += "图片数量: " + std::to_string(GetStoredImages().size());
                    
                    ShowStatus(info);
                } else {
                    ShowStatus("获取存储信息失败");
                }
            }
            break;
            
        case ButtonState::SERVICE_RUNNING:
            // 停止服务并返回空闲状态
            StopImageUploadService();
            break;
            
        case ButtonState::BROWSING_IMAGES:
            // 删除当前图片
            if (!current_images_.empty() && current_image_index_ < current_images_.size()) {
                std::string filename = current_images_[current_image_index_].filename;
                if (DeleteStoredImage(filename)) {
                    // 更新图片列表
                    UpdateImageList();
                    if (current_images_.empty()) {
                        button_state_ = ButtonState::IDLE;
                        ShowStatus("所有图片已删除");
                    } else {
                        // 调整索引
                        if (current_image_index_ >= current_images_.size()) {
                            current_image_index_ = 0;
                        }
                    }
                }
            } else {
                button_state_ = ButtonState::IDLE;
                ShowStatus("退出图片浏览模式");
            }
            break;
    }
}
