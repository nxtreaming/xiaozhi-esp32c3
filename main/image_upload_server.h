#ifndef _IMAGE_UPLOAD_SERVER_H_
#define _IMAGE_UPLOAD_SERVER_H_

#include <string>
#include <functional>
#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>

class ImageUploadServer {
public:
    static ImageUploadServer& GetInstance();
    
    // 回调函数类型定义
    using ImageReceivedCallback = std::function<void(const uint8_t* data, size_t size, const std::string& filename)>;
    
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
    
    // WiFi事件处理
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    
    // 工具函数
    std::string GenerateUploadPage();
    bool SaveImageToFlash(const uint8_t* data, size_t size, const std::string& filename);
};

#endif // _IMAGE_UPLOAD_SERVER_H_
