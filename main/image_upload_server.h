#ifndef _IMAGE_UPLOAD_SERVER_H_
#define _IMAGE_UPLOAD_SERVER_H_

#include <string>
#include <functional>
#include <mutex>
#include <ctime>
#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>

class ImageUploadServer {
public:
    static ImageUploadServer& GetInstance();
    
    // 回调函数类型定义
    using ImageReceivedCallback = std::function<void(const uint8_t* data, size_t size, const std::string& filename, time_t upload_time)>;
    
    // 设置图片接收回调
    void SetImageReceivedCallback(ImageReceivedCallback callback);
    
    // 启动图片上传服务器
    bool Start(const std::string& ssid_prefix = "ImageUpload");
    
    // 停止服务器
    void Stop();
    
    // 获取服务器信息
    std::string GetSsid() const { return ssid_; }
    std::string GetWebServerUrl() const { return "http://192.168.4.1"; }
    std::string GetUploadUrl() const { return "http://192.168.4.1/upload"; }
    bool IsRunning() const { return server_ != nullptr; }

    // 存储阶段通知（由应用层调用）
    void NotifyStorageStart(size_t total_bytes);
    void NotifyStorageProgress(size_t written, size_t total);
    void NotifyStorageResult(bool success, const std::string& message);
    
    // Delete copy constructor and assignment operator
    ImageUploadServer(const ImageUploadServer&) = delete;
    ImageUploadServer& operator=(const ImageUploadServer&) = delete;

private:
    ImageUploadServer();
    ~ImageUploadServer();
    
    // WiFi热点和HTTP服务器
    httpd_handle_t server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    std::string ssid_;
    std::string ssid_prefix_;
    
    // 回调函数
    ImageReceivedCallback image_callback_;
    
    // 事件处理
    esp_event_handler_instance_t wifi_event_instance_ = nullptr;
    
    // 内部方法
    void StartAccessPoint();
    void StartWebServer();
    void StopAccessPoint();
    void StopWebServer();
    
    // HTTP处理函数
    static esp_err_t IndexHandler(httpd_req_t *req);
    static esp_err_t UploadHandler(httpd_req_t *req);
    static esp_err_t StatusHandler(httpd_req_t *req);
    static esp_err_t FilesHandler(httpd_req_t *req);
    
    // WiFi事件处理
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    
    // 工具函数
    std::string GenerateUploadPage();
    bool SaveImageToFlash(const uint8_t* data, size_t size, const std::string& filename);

public:
    enum class UploadStage {
        kIdle,
        kUploading,
        kSaving,
        kCompleted,
        kError
    };

    struct UploadStatus {
        UploadStage stage = UploadStage::kIdle;
        std::string filename;
        size_t upload_total = 0;
        size_t upload_received = 0;
        size_t storage_total = 0;
        size_t storage_written = 0;
        bool success = false;
        std::string message;
    };

    void ResetProgress();
    void StartUploadProgress(size_t total_bytes);
    void UpdateUploadProgress(size_t received_bytes);
    void SetCurrentFilename(const std::string& filename);
    void SetProgressError(const std::string& message);
    void SetStorageTotal(size_t total_bytes);
    std::string BuildStatusJson() const;

    mutable std::mutex progress_mutex_;
    UploadStatus progress_;
};

#endif // _IMAGE_UPLOAD_SERVER_H_
