#include "image_upload_server.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>
#include <cstring>
#include <memory>
#include <sstream>
#include <vector>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <cstdlib>
#include <cstdio>

#include "storage/gif_storage.h"

#define TAG "ImageUploadServer"

namespace {

std::string JsonEscape(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                    output += buffer;
                } else {
                    output += c;
                }
                break;
        }
    }
    return output;
}

const char* StageToString(ImageUploadServer::UploadStage stage) {
    switch (stage) {
        case ImageUploadServer::UploadStage::kUploading:
            return "uploading";
        case ImageUploadServer::UploadStage::kSaving:
            return "saving";
        case ImageUploadServer::UploadStage::kCompleted:
            return "completed";
        case ImageUploadServer::UploadStage::kError:
            return "error";
        case ImageUploadServer::UploadStage::kIdle:
        default:
            return "idle";
    }
}

struct StoredFileInfo {
    std::string name;
    size_t size = 0;
    time_t upload_time = 0;
};

void CollectStoredFiles(const char* filename, size_t size, time_t upload_time, void* user_data) {
    auto* files = static_cast<std::vector<StoredFileInfo>*>(user_data);
    files->push_back(StoredFileInfo{std::string(filename), size, upload_time});
}

std::string FormatRelativeDuration(time_t seconds_since_boot) {
    if (seconds_since_boot < 0) {
        seconds_since_boot = 0;
    }
    const int hours = static_cast<int>(seconds_since_boot / 3600);
    const int minutes = static_cast<int>((seconds_since_boot % 3600) / 60);
    const int seconds = static_cast<int>(seconds_since_boot % 60);

    char buffer[48];
    snprintf(buffer, sizeof(buffer), "设备启动后 %02d:%02d:%02d", hours, minutes, seconds);
    return std::string(buffer);
}

std::string FormatTimestamp(time_t ts) {
    // Treat timestamps earlier than year 2000 as "time since boot"
    constexpr time_t kReasonableEpoch = 946684800; // 2000-01-01 00:00:00 UTC
    if (ts <= 0) {
        return "未知";
    }

    if (ts < kReasonableEpoch) {
        return FormatRelativeDuration(ts);
    }

    struct tm timeinfo = {};
#if defined(_WIN32)
    localtime_s(&timeinfo, &ts);
#else
    localtime_r(&ts, &timeinfo);
#endif

    char buffer[32];
    if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo) == 0) {
        return "未知";
    }
    return std::string(buffer);
}

} // namespace

ImageUploadServer& ImageUploadServer::GetInstance() {
    static ImageUploadServer instance;
    return instance;
}

ImageUploadServer::ImageUploadServer() {
    ssid_prefix_ = "ImageUpload";
    ResetProgress();
}

ImageUploadServer::~ImageUploadServer() {
    Stop();
}

void ImageUploadServer::SetImageReceivedCallback(ImageReceivedCallback callback) {
    image_callback_ = callback;
}

void ImageUploadServer::ResetProgress() {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_ = UploadStatus{};
    progress_.message = "ready";
}

void ImageUploadServer::StartUploadProgress(size_t total_bytes) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.stage = UploadStage::kUploading;
    progress_.upload_total = total_bytes;
    progress_.upload_received = 0;
    progress_.storage_total = 0;
    progress_.storage_written = 0;
    progress_.success = false;
    progress_.message = "正在上传到设备...";
    progress_.filename.clear();
}

void ImageUploadServer::UpdateUploadProgress(size_t received_bytes) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.upload_received = received_bytes;
}

void ImageUploadServer::SetCurrentFilename(const std::string& filename) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.filename = filename;
}

void ImageUploadServer::SetProgressError(const std::string& message) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.stage = UploadStage::kError;
    progress_.message = message;
    progress_.success = false;
}

void ImageUploadServer::SetStorageTotal(size_t total_bytes) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.storage_total = total_bytes;
}

void ImageUploadServer::NotifyStorageStart(size_t total_bytes) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.stage = UploadStage::kSaving;
    progress_.storage_written = 0;
    progress_.storage_total = total_bytes;
    progress_.message = "正在保存到存储...";
}

void ImageUploadServer::NotifyStorageProgress(size_t written, size_t total) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    if (total > 0) {
        progress_.storage_total = total;
    }
    progress_.storage_written = written;
}

void ImageUploadServer::NotifyStorageResult(bool success, const std::string& message) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.stage = success ? UploadStage::kCompleted : UploadStage::kError;
    progress_.success = success;
    if (success) {
        progress_.upload_received = progress_.upload_total;
        progress_.storage_written = progress_.storage_total;
    }
    progress_.message = message;
}

std::string ImageUploadServer::BuildStatusJson() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    std::ostringstream oss;
    oss << "{\"stage\":\"" << StageToString(progress_.stage) << "\",";
    oss << "\"filename\":\"" << JsonEscape(progress_.filename) << "\",";
    oss << "\"upload\":{\"received\":" << progress_.upload_received
        << ",\"total\":" << progress_.upload_total << "},";
    oss << "\"storage\":{\"written\":" << progress_.storage_written
        << ",\"total\":" << progress_.storage_total << "},";
    oss << "\"success\":" << (progress_.success ? "true" : "false") << ",";
    oss << "\"message\":\"" << JsonEscape(progress_.message) << "\"}";
    return oss.str();
}

bool ImageUploadServer::Start(const std::string& ssid_prefix) {
    if (server_ != nullptr) {
        ESP_LOGW(TAG, "Server already running");
        return true;
    }
    
    ssid_prefix_ = ssid_prefix;
    
    try {
        StartAccessPoint();
        StartWebServer();
        ESP_LOGI(TAG, "Image upload server started successfully");
        ESP_LOGI(TAG, "SSID: %s", ssid_.c_str());
        ESP_LOGI(TAG, "Upload URL: %s", GetUploadUrl().c_str());
        return true;
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Failed to start server: %s", e.what());
        Stop();
        return false;
    }
}

void ImageUploadServer::Stop() {
    StopWebServer();
    StopAccessPoint();
    ResetProgress();
    ESP_LOGI(TAG, "Image upload server stopped");
}

void ImageUploadServer::StartAccessPoint() {
    // 生成唯一的SSID
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", ssid_prefix_.c_str(), mac[4], mac[5]);
    ssid_ = std::string(ssid);
    
    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());
    ap_netif_ = esp_netif_create_default_wifi_ap();
    
    // 设置IP地址
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif_);
    esp_netif_set_ip_info(ap_netif_, &ip_info);
    esp_netif_dhcps_start(ap_netif_);
    
    // 初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 注册WiFi事件处理器
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &ImageUploadServer::WifiEventHandler,
                                                        this,
                                                        &wifi_event_instance_));
    
    // 配置WiFi热点
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, ssid_.c_str());
    wifi_config.ap.ssid_len = ssid_.length();
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    
    // 启动WiFi热点
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "Access Point started with SSID: %s", ssid_.c_str());
}

void ImageUploadServer::StartWebServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;  // 增加栈大小以处理文件上传
    
    ESP_ERROR_CHECK(httpd_start(&server_, &config));
    
    // 注册主页处理器
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = IndexHandler,
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &index_uri));
    
    // 注册图片上传处理器
    httpd_uri_t upload_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = UploadHandler,
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &upload_uri));
    
    // 注册状态查询处理器
    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = StatusHandler,
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &status_uri));

    httpd_uri_t files_uri = {
        .uri = "/files",
        .method = HTTP_GET,
        .handler = FilesHandler,
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &files_uri));

    httpd_uri_t delete_uri = {
        .uri = "/files/delete",
        .method = HTTP_POST,
        .handler = DeleteFileHandler,
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &delete_uri));
    
    ESP_LOGI(TAG, "Web server started");
}

void ImageUploadServer::StopWebServer() {
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

void ImageUploadServer::StopAccessPoint() {
    // 注销事件处理器
    if (wifi_event_instance_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_instance_);
        wifi_event_instance_ = nullptr;
    }
    
    // 停止WiFi
    esp_wifi_stop();
    esp_wifi_deinit();
    
    // 释放网络接口
    if (ap_netif_) {
        esp_netif_destroy(ap_netif_);
        ap_netif_ = nullptr;
    }
}

esp_err_t ImageUploadServer::IndexHandler(httpd_req_t *req) {
    auto* self = static_cast<ImageUploadServer*>(req->user_ctx);
    std::string html = self->GenerateUploadPage();
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html.c_str(), html.length());
    return ESP_OK;
}

esp_err_t ImageUploadServer::UploadHandler(httpd_req_t *req) {
    auto* self = static_cast<ImageUploadServer*>(req->user_ctx);
    self->StartUploadProgress(req->content_len);

    time_t upload_time = 0;
    char upload_ts_header[32];
    if (httpd_req_get_hdr_value_str(req, "X-Upload-Timestamp", upload_ts_header, sizeof(upload_ts_header)) == ESP_OK) {
        long long header_ts = strtoll(upload_ts_header, nullptr, 10);
        if (header_ts > 0) {
            upload_time = static_cast<time_t>(header_ts / 1000);
        }
    }
    long tz_offset_minutes = 0;
    char tz_offset_header[16];
    if (httpd_req_get_hdr_value_str(req, "X-Upload-TzOffset", tz_offset_header, sizeof(tz_offset_header)) == ESP_OK) {
        tz_offset_minutes = strtol(tz_offset_header, nullptr, 10);
    }
    if (upload_time > 0) {
        time_t adjusted = upload_time - tz_offset_minutes * 60;
        if (adjusted > 0) {
            upload_time = adjusted;
        }
    }
    if (upload_time == 0) {
        upload_time = static_cast<time_t>(esp_timer_get_time() / 1000000ULL);
    }

    // 检查Content-Type
    char content_type[100];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        ESP_LOGE(TAG, "No Content-Type header found");
        self->SetProgressError("缺少Content-Type");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Content-Type");
        return ESP_FAIL;
    }

    // 检查是否是multipart/form-data
    if (strstr(content_type, "multipart/form-data") == nullptr) {
        ESP_LOGE(TAG, "Invalid Content-Type: %s", content_type);
        self->SetProgressError("Content-Type错误");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Content-Type");
        return ESP_FAIL;
    }

    // 获取boundary
    char* boundary = strstr(content_type, "boundary=");
    if (!boundary) {
        ESP_LOGE(TAG, "No boundary found in Content-Type");
        self->SetProgressError("缺少boundary");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary found");
        return ESP_FAIL;
    }
    boundary += 9; // 跳过"boundary="

    ESP_LOGI(TAG, "Receiving file upload, Content-Length: %d", req->content_len);

    // 限制文件大小 (5MB)
    const size_t max_file_size = 5 * 1024 * 1024;
    if (req->content_len > max_file_size) {
        ESP_LOGE(TAG, "File too large: %d bytes", req->content_len);
        self->SetProgressError("文件太大，超过5MB限制");
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "File too large", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // 分配缓冲区接收数据
    const size_t buffer_size = 4096;
    auto buffer = std::make_unique<char[]>(buffer_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        self->SetProgressError("内存不足，无法接收文件");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // 接收并处理multipart数据
    size_t total_received = 0;
    std::string filename;
    std::vector<uint8_t> image_data;
    bool in_file_data = false;
    std::string boundary_str = "--" + std::string(boundary);
    std::string end_boundary = boundary_str + "--";

    while (total_received < req->content_len) {
        int received = httpd_req_recv(req, buffer.get(),
                                    std::min(buffer_size, req->content_len - total_received));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "Socket timeout");
                self->SetProgressError("上传超时");
                httpd_resp_send_408(req);
            } else {
                ESP_LOGE(TAG, "Failed to receive data");
                self->SetProgressError("接收数据失败");
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
            }
            return ESP_FAIL;
        }

        total_received += received;
        self->UpdateUploadProgress(total_received);

        // 简化的multipart解析 - 查找文件数据
        std::string chunk(buffer.get(), received);

        if (!in_file_data) {
            // 查找文件名
            size_t filename_pos = chunk.find("filename=\"");
            if (filename_pos != std::string::npos) {
                filename_pos += 10; // 跳过 filename="
                size_t filename_end = chunk.find("\"", filename_pos);
                if (filename_end != std::string::npos) {
                    filename = chunk.substr(filename_pos, filename_end - filename_pos);
                    ESP_LOGI(TAG, "Found filename: %s", filename.c_str());
                    self->SetCurrentFilename(filename);
                }
            }

            // 查找文件数据开始位置（双换行后）
            size_t data_start = chunk.find("\r\n\r\n");
            if (data_start != std::string::npos) {
                in_file_data = true;
                data_start += 4; // 跳过 \r\n\r\n
                // 添加文件数据
                for (size_t i = data_start; i < chunk.size(); i++) {
                    image_data.push_back(static_cast<uint8_t>(chunk[i]));
                }
            }
        } else {
            // 已经在文件数据中，继续添加
            for (int i = 0; i < received; i++) {
                image_data.push_back(static_cast<uint8_t>(buffer[i]));
            }
        }
    }

    if (image_data.empty()) {
        ESP_LOGE(TAG, "No file data received");
        self->SetProgressError("未收到有效的文件数据");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No file data received");
        return ESP_FAIL;
    }

    // 移除结尾的boundary数据
    if (image_data.size() > boundary_str.length() + 10) {
        // 简单处理：移除最后的一些字节（包含boundary等尾数据）
        image_data.resize(image_data.size() - boundary_str.length() - 10);
    }
    self->SetStorageTotal(image_data.size());

    ESP_LOGI(TAG, "Received image: %s, size: %d bytes", filename.c_str(), image_data.size());

    // 调用回调函数处理图片数据
    if (self->image_callback_ && !image_data.empty()) {
        self->image_callback_(image_data.data(), image_data.size(), filename, upload_time);
    }

    // 发送成功响应
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Image uploaded successfully\"}", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

esp_err_t ImageUploadServer::StatusHandler(httpd_req_t *req) {
    auto* self = static_cast<ImageUploadServer*>(req->user_ctx);
    std::string json = self->BuildStatusJson();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

esp_err_t ImageUploadServer::FilesHandler(httpd_req_t *req) {
    std::vector<StoredFileInfo> files;
    esp_err_t ret = gif_storage_list(CollectStoredFiles, &files);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to list files");
        return ret;
    }

    std::sort(files.begin(), files.end(), [](const StoredFileInfo& a, const StoredFileInfo& b) {
        return a.upload_time > b.upload_time;
    });

    std::ostringstream oss;
    oss << "{\"files\":[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }
        oss << "{\"name\":\"" << JsonEscape(files[i].name) << "\",";
        oss << "\"size\":" << files[i].size << ",";
        oss << "\"uploadTime\":\"" << JsonEscape(FormatTimestamp(files[i].upload_time)) << "\"}";
    }
    oss << "]}";

    auto payload = oss.str();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, payload.c_str(), payload.length());
    return ESP_OK;
}

esp_err_t ImageUploadServer::DeleteFileHandler(httpd_req_t *req) {
    std::string filename;
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        std::string query(query_len, '\0');
        if (httpd_req_get_url_query_str(req, query.data(), query_len) == ESP_OK) {
            char value[128];
            if (httpd_query_key_value(query.c_str(), "name", value, sizeof(value)) == ESP_OK) {
                filename = value;
            }
        }
    }

    if (filename.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }

    // basic sanitization
    if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    if (!gif_storage_exists(filename.c_str())) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    esp_err_t ret = gif_storage_delete(filename.c_str());
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ret;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"deleted\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void ImageUploadServer::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " connected", MAC2STR(event->mac));
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " disconnected", MAC2STR(event->mac));
    }
}

std::string ImageUploadServer::GenerateUploadPage() {
    return R"HTML(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>图片上传</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }
        .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #333; text-align: center; }
        .upload-area { border: 2px dashed #ccc; border-radius: 10px; padding: 40px; text-align: center; margin: 20px 0; }
        .upload-area.dragover { border-color: #007bff; background-color: #f0f8ff; }
        input[type="file"] { display: none; }
        .upload-btn { background: #007bff; color: white; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; }
        .upload-btn:hover { background: #0056b3; }
        .progress { width: 100%; height: 20px; background: #f0f0f0; border-radius: 10px; margin: 10px 0; overflow: hidden; }
        .progress-bar { height: 100%; background: #28a745; width: 0%; transition: width 0.3s; }
        .progress-text { text-align: center; font-size: 14px; color: #555; margin-bottom: 10px; display: none; }
        .status { margin: 10px 0; padding: 10px; border-radius: 5px; }
        .success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }
        .error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }
        .preview { max-width: 200px; max-height: 200px; margin: 10px auto; display: block; border-radius: 5px; }
        .file-list { margin-top: 30px; background: #fff; padding: 20px; border-radius: 10px; box-shadow: 0 2px 8px rgba(0,0,0,0.06); }
        .file-list-header { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px; }
        .file-list table { width: 100%; border-collapse: collapse; margin-top: 15px; }
        .file-list th, .file-list td { padding: 10px; text-align: left; border-bottom: 1px solid #eee; font-size: 14px; }
        .file-list th { background: #f7f7f7; color: #555; }
        .file-list tr:hover td { background: #f9fafb; }
        .filename-cell { display: flex; justify-content: space-between; align-items: center; gap: 8px; }
        .delete-btn { background: #dc3545; color: #fff; border: none; border-radius: 4px; padding: 4px 8px; font-size: 12px; cursor: pointer; }
        .delete-btn:hover { background: #c82333; }
        .file-empty { text-align: center; color: #777; padding: 15px 0; font-size: 14px; }
        .table-wrapper { width: 100%; overflow-x: auto; }
        .upload-btn.secondary { background: #6c757d; }
        .upload-btn.secondary:hover { background: #5a6268; }
    </style>
</head>
<body>
    <div class="container">
        <h1>📷 图片上传</h1>
        <div class="upload-area" id="uploadArea">
            <p>点击选择图片或拖拽图片到此处</p>
            <button class="upload-btn" onclick="document.getElementById('fileInput').click()">选择图片</button>
            <input type="file" id="fileInput" accept="image/*" multiple>
        </div>
        <div class="progress" id="progress" style="display:none;">
            <div class="progress-bar" id="progressBar"></div>
        </div>
        <div class="progress-text" id="progressText"></div>
        <div id="status"></div>
        <div id="preview"></div>
        <div class="file-list">
            <div class="file-list-header">
                <h2>📂 已上传 GIF</h2>
                <button class="upload-btn secondary" id="refreshFiles">刷新列表</button>
            </div>
            <div class="table-wrapper">
                <table>
                    <thead>
                        <tr>
                            <th>文件名</th>
                            <th>大小</th>
                            <th>上传时间</th>
                        </tr>
                    </thead>
                    <tbody id="fileTableBody"></tbody>
                </table>
            </div>
            <div class="file-empty" id="fileEmpty">暂无 GIF 文件</div>
        </div>
    </div>

    <script>
        const uploadArea = document.getElementById('uploadArea');
        const fileInput = document.getElementById('fileInput');
        const progress = document.getElementById('progress');
        const progressBar = document.getElementById('progressBar');
        const progressText = document.getElementById('progressText');
        const status = document.getElementById('status');
        const preview = document.getElementById('preview');
        const fileTableBody = document.getElementById('fileTableBody');
        const fileEmpty = document.getElementById('fileEmpty');
        const refreshFilesBtn = document.getElementById('refreshFiles');
        let statusTimer = null;
        let hasSeenServerStage = false;

        refreshFilesBtn.addEventListener('click', loadFileList);

        // 拖拽上传
        uploadArea.addEventListener('dragover', (e) => {
            e.preventDefault();
            uploadArea.classList.add('dragover');
        });

        uploadArea.addEventListener('dragleave', () => {
            uploadArea.classList.remove('dragover');
        });

        uploadArea.addEventListener('drop', (e) => {
            e.preventDefault();
            uploadArea.classList.remove('dragover');
            const files = e.dataTransfer.files;
            handleFiles(files);
        });

        fileInput.addEventListener('change', (e) => {
            handleFiles(e.target.files);
        });

        function handleFiles(files) {
            for (let file of files) {
                if (file.type.startsWith('image/')) {
                    uploadFile(file);
                }
            }
        }

        function uploadFile(file) {
            const formData = new FormData();
            formData.append('image', file);

            // 显示预览
            const reader = new FileReader();
            reader.onload = (e) => {
                preview.innerHTML = '<img src="' + e.target.result + '" class="preview" alt="预览">';
            };
            reader.readAsDataURL(file);

            // 显示进度条并开始轮询状态
            progress.style.display = 'block';
            progressBar.style.width = '0%';
            progressBar.textContent = '0%';
            progressText.style.display = 'block';
            progressText.textContent = '准备上传...';
            status.innerHTML = '';
            hasSeenServerStage = false;
            stopStatusPolling();
            startStatusPolling();

            const xhr = new XMLHttpRequest();
            const fileSize = file.size || 0;
            
            xhr.upload.addEventListener('progress', (e) => {
                const loaded = e.loaded || 0;
                const total = (e.lengthComputable && e.total) ? e.total : fileSize;
                if (total > 0) {
                    const percentComplete = Math.min(50, (loaded / total) * 50);
                    progressBar.style.width = percentComplete + '%';
                    progressBar.textContent = percentComplete.toFixed(0) + '%';
                    progressText.textContent = '正在上传到设备...';
                }
            });

            xhr.addEventListener('load', () => {
                if (xhr.status !== 200) {
                    status.innerHTML = '<div class="status error">上传失败，请重试</div>';
                    stopStatusPolling();
                }
            });

            xhr.addEventListener('error', () => {
                stopStatusPolling();
                progress.style.display = 'none';
                progressText.style.display = 'none';
                status.innerHTML = '<div class="status error">网络错误，请检查连接</div>';
            });

            xhr.open('POST', '/upload');
            const now = Date.now();
            xhr.setRequestHeader('X-Upload-Timestamp', now.toString());
            xhr.setRequestHeader('X-Upload-TzOffset', new Date().getTimezoneOffset().toString());
            xhr.send(formData);
        }

        function startStatusPolling() {
            fetchStatus();
            statusTimer = setInterval(fetchStatus, 600);
        }

        function stopStatusPolling() {
            if (statusTimer) {
                clearInterval(statusTimer);
                statusTimer = null;
            }
        }

        async function fetchStatus() {
            try {
                const response = await fetch('/status', { cache: 'no-store' });
                if (!response.ok) {
                    return;
                }
                const data = await response.json();
                updateProgressFromStatus(data);
            } catch (error) {
                console.error('Failed to fetch status', error);
            }
        }

        function updateProgressFromStatus(data) {
            if (!data) {
                return;
            }

            const stage = data.stage || 'idle';
            if (stage === 'idle') {
                return;
            }

            if (stage === 'uploading' || stage === 'saving') {
                hasSeenServerStage = true;
            } else if (!hasSeenServerStage) {
                return;
            }

            const upload = data.upload || {};
            const storage = data.storage || {};
            const uploadPortion = upload.total ? Math.min(1, (upload.received || 0) / upload.total) : 0;
            const storagePortion = storage.total ? Math.min(1, (storage.written || 0) / storage.total) : 0;
            let percentComplete = 0;

            if (stage === 'uploading') {
                percentComplete = uploadPortion * 50;
            } else if (stage === 'saving') {
                percentComplete = 50 + storagePortion * 50;
            } else {
                percentComplete = 100;
            }

            progress.style.display = 'block';
            progressText.style.display = 'block';
            progressBar.style.width = percentComplete + '%';
            progressBar.textContent = percentComplete.toFixed(0) + '%';

            const defaultMessages = {
                uploading: '正在上传到设备...',
                saving: '正在保存到存储...',
                completed: '上传并保存成功',
                error: '上传失败，请重试'
            };

            if (data.message) {
                progressText.textContent = data.message;
            } else if (defaultMessages[stage]) {
                progressText.textContent = defaultMessages[stage];
            }

            if (stage === 'completed') {
                status.innerHTML = '<div class="status success">GIF 上传并保存成功</div>';
                loadFileList();
                stopStatusPolling();
                hasSeenServerStage = false;
                setTimeout(() => {
                    progress.style.display = 'none';
                    progressText.style.display = 'none';
                }, 800);
            } else if (stage === 'error') {
                status.innerHTML = '<div class="status error">' + (data.message || '上传失败，请重试') + '</div>';
                stopStatusPolling();
                hasSeenServerStage = false;
                setTimeout(() => {
                    progress.style.display = 'none';
                    progressText.style.display = 'none';
                }, 800);
            } else {
                status.innerHTML = '';
            }
        }

        async function loadFileList() {
            try {
                const response = await fetch('/files', { cache: 'no-store' });
                if (!response.ok) {
                    throw new Error('Failed to load files');
                }
                const data = await response.json();
                renderFileList(data.files || []);
            } catch (error) {
                console.error('Failed to load file list', error);
            }
        }

        function renderFileList(files) {
            fileTableBody.innerHTML = '';
            if (!files.length) {
                fileEmpty.style.display = 'block';
                return;
            }
            fileEmpty.style.display = 'none';
            files.forEach((file) => {
                const row = document.createElement('tr');
                row.innerHTML = `
                    <td>
                        <div class="filename-cell">
                            <span>${file.name}</span>
                            <button class="delete-btn" data-name="${encodeURIComponent(file.name)}">删除</button>
                        </div>
                    </td>
                    <td>${formatBytes(file.size)}</td>
                    <td>${file.uploadTime || '未知'}</td>
                `;
                fileTableBody.appendChild(row);
            });
            fileTableBody.querySelectorAll('.delete-btn').forEach((btn) => {
                btn.addEventListener('click', () => {
                    const encodedName = btn.dataset.name;
                    const originalName = decodeURIComponent(encodedName);
                    if (!confirm(`确定要删除 ${originalName} 吗？`)) {
                        return;
                    }
                    deleteFile(encodedName, originalName);
                });
            });
        }

        function formatBytes(bytes) {
            if (bytes >= 1024 * 1024) {
                return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
            }
            if (bytes >= 1024) {
                return (bytes / 1024).toFixed(2) + ' KB';
            }
            return bytes + ' B';
        }

        async function deleteFile(encodedName, originalName) {
            try {
                const response = await fetch(`/files/delete?name=${encodedName}`, {
                    method: 'POST'
                });
                if (!response.ok) {
                    const text = await response.text();
                    throw new Error(text || '删除失败');
                }
                status.innerHTML = `<div class="status success">${originalName} 已删除</div>`;
                loadFileList();
            } catch (error) {
                console.error('Failed to delete file', error);
                status.innerHTML = `<div class="status error">删除失败：${error.message}</div>`;
            }
        }

        loadFileList();

    </script>
</body>
</html>)HTML";
}
