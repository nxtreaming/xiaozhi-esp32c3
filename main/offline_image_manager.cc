#include "offline_image_manager.h"
#include "application.h"
#include "storage/gif_storage.h"
#include "image_upload_server.h"
#include "boards/common/board.h"
#include "display.h"

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

        // Stop any running slideshow
        app.StopSlideShow();

        UpdateImageList();
        ShowUploadServiceInfo("图片上传服务已启动");
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

bool OfflineImageManager::IsBrowsingImages() const {
    return button_state_ == ButtonState::BROWSING_IMAGES;
}

std::vector<OfflineImageManager::ImageInfo> OfflineImageManager::GetStoredImages() {
    std::vector<ImageInfo> images;
    
    gif_storage_list([](const char* filename, size_t size, time_t upload_time, void* user_data) {
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

static bool ShowStoredImageHelper(const std::string& filename) {
    ESP_LOGI(TAG, "Showing stored image: %s", filename.c_str());

    if (!gif_storage_exists(filename.c_str())) {
        return false;
    }

    uint8_t* data = nullptr;
    size_t size = 0;
    esp_err_t ret = gif_storage_read(filename.c_str(), &data, &size);
    if (ret != ESP_OK) {
        return false;
    }

    auto& app = Application::GetInstance();
    app.Schedule([data, size]() {
        if (auto display = Board::GetInstance().GetDisplay()) {
            display->ShowGif(data, size);
        }
        heap_caps_free(data);
    });
    return true;
}

bool OfflineImageManager::DeleteStoredImage(const std::string& filename) {
    ESP_LOGI(TAG, "Deleting stored image: %s", filename.c_str());

    if (!gif_storage_exists(filename.c_str())) {
        ShowStatus("图片文件不存在: " + filename);
        return false;
    }

    if (gif_storage_delete(filename.c_str()) == ESP_OK) {
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
        if (gif_storage_delete(image.filename.c_str()) == ESP_OK) {
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

void OfflineImageManager::RefreshImageList(bool show_update_message) {
    UpdateImageList();
    if (show_update_message && button_state_ == ButtonState::SERVICE_RUNNING) {
        ShowUploadServiceInfo("图片列表已更新");
    }
}

void OfflineImageManager::ShowUploadServiceInfo(const std::string& status_prefix) {
    auto& app = Application::GetInstance();
    std::string info = app.GetImageUploadServerInfo();

    std::string message = status_prefix + "\n" + info + "\n\n";
    if (current_images_.empty()) {
        message += "当前没有GIF文件";
    } else {
        message += "当前GIF文件（共" + std::to_string(current_images_.size()) + "个）:\n";
        for (const auto& img : current_images_) {
            size_t kb = (img.size + 1023) / 1024;
            message += "- " + img.filename + " (" + std::to_string(kb) + " KB)\n";
        }
    }

    app.Schedule([message]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->HideGif();
            display->SetChatMessage("system", message.c_str());
        }
    });

    ShowStatus(status_prefix + "\n" + info);
}

void OfflineImageManager::HandleButtonPress() {
    ESP_LOGI(TAG, "Button press - current state: %d", (int)button_state_);

    // 检查实际的服务运行状态，如果状态不一致则同步
    bool service_actually_running = IsImageUploadServiceRunning();
    if (button_state_ == ButtonState::SERVICE_RUNNING && !service_actually_running) {
        ESP_LOGW(TAG, "State mismatch detected, resetting to IDLE");
        button_state_ = ButtonState::IDLE;
    }

    // 如果幻灯片正在运行，先停止它
    auto& app = Application::GetInstance();
    if (app.IsSlideShowRunning()) {
        ESP_LOGI(TAG, "Stopping running slideshow");
        app.StopSlideShow();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    switch (button_state_) {
        case ButtonState::IDLE:
            // 启动图片上传服务
            StartImageUploadService();
            break;
            
        case ButtonState::SERVICE_RUNNING:
            // 停止上传服务并启动幻灯片播放
            StopImageUploadService();
            UpdateImageList();
            if (!current_images_.empty()) {
                auto& app = Application::GetInstance();
                app.SlideShow();
                ShowStatus("幻灯片播放中 (" + std::to_string(current_images_.size()) + " 个文件)");
                button_state_ = ButtonState::IDLE;
            } else {
                ShowStatus("没有存储的图片文件");
                button_state_ = ButtonState::IDLE;
            }
            break;
            
        case ButtonState::BROWSING_IMAGES:
            // 显示下一张图片
            if (!current_images_.empty()) {
                current_image_index_ = (current_image_index_ + 1) % current_images_.size();
                ShowStoredImageHelper(current_images_[current_image_index_].filename);
                ShowStatus("图片浏览 (" + std::to_string(current_image_index_ + 1) +
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
                        ShowStoredImageHelper(current_images_[current_image_index_].filename);
                    }
                }
            } else {
                button_state_ = ButtonState::IDLE;
                ShowStatus("退出图片浏览模式");
            }
            break;
    }
}
