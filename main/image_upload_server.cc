#include "image_upload_server.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>
#include <cstring>
#include <memory>

#define TAG "ImageUploadServer"

ImageUploadServer& ImageUploadServer::GetInstance() {
    static ImageUploadServer instance;
    return instance;
}

ImageUploadServer::ImageUploadServer() {
    ssid_prefix_ = "ImageUpload";
}

ImageUploadServer::~ImageUploadServer() {
    Stop();
}

void ImageUploadServer::SetImageReceivedCallback(ImageReceivedCallback callback) {
    image_callback_ = callback;
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

    // 检查Content-Type
    char content_type[100];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        ESP_LOGE(TAG, "No Content-Type header found");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Content-Type");
        return ESP_FAIL;
    }

    // 检查是否是multipart/form-data
    if (strstr(content_type, "multipart/form-data") == nullptr) {
        ESP_LOGE(TAG, "Invalid Content-Type: %s", content_type);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Content-Type");
        return ESP_FAIL;
    }

    // 获取boundary
    char* boundary = strstr(content_type, "boundary=");
    if (!boundary) {
        ESP_LOGE(TAG, "No boundary found in Content-Type");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary found");
        return ESP_FAIL;
    }
    boundary += 9; // 跳过"boundary="

    ESP_LOGI(TAG, "Receiving file upload, Content-Length: %d", req->content_len);

    // 限制文件大小 (5MB)
    const size_t max_file_size = 5 * 1024 * 1024;
    if (req->content_len > max_file_size) {
        ESP_LOGE(TAG, "File too large: %d bytes", req->content_len);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "File too large", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // 分配缓冲区接收数据
    const size_t buffer_size = 4096;
    auto buffer = std::make_unique<char[]>(buffer_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
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
                httpd_resp_send_408(req);
            } else {
                ESP_LOGE(TAG, "Failed to receive data");
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
            }
            return ESP_FAIL;
        }

        total_received += received;

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

    // 移除结尾的boundary数据
    if (image_data.size() > boundary_str.length() + 10) {
        // 简单处理：移除最后的一些字节（包含boundary）
        image_data.resize(image_data.size() - boundary_str.length() - 10);
    }

    ESP_LOGI(TAG, "Received image: %s, size: %d bytes", filename.c_str(), image_data.size());

    // 调用回调函数处理图片数据
    if (self->image_callback_ && !image_data.empty()) {
        self->image_callback_(image_data.data(), image_data.size(), filename);
    }

    // 发送成功响应
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Image uploaded successfully\"}", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

esp_err_t ImageUploadServer::StatusHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ready\"}", HTTPD_RESP_USE_STRLEN);
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
        .status { margin: 10px 0; padding: 10px; border-radius: 5px; }
        .success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }
        .error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }
        .preview { max-width: 200px; max-height: 200px; margin: 10px auto; display: block; border-radius: 5px; }
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
        <div id="status"></div>
        <div id="preview"></div>
    </div>

    <script>
        const uploadArea = document.getElementById('uploadArea');
        const fileInput = document.getElementById('fileInput');
        const progress = document.getElementById('progress');
        const progressBar = document.getElementById('progressBar');
        const status = document.getElementById('status');
        const preview = document.getElementById('preview');

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

            // 显示进度条
            progress.style.display = 'block';
            progressBar.style.width = '0%';

            const xhr = new XMLHttpRequest();
            
            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percentComplete = (e.loaded / e.total) * 100;
                    progressBar.style.width = percentComplete + '%';
                }
            });

            xhr.addEventListener('load', () => {
                progress.style.display = 'none';
                if (xhr.status === 200) {
                    status.innerHTML = '<div class="status success">✅ 图片上传成功！</div>';
                } else {
                    status.innerHTML = '<div class="status error">❌ 上传失败，请重试</div>';
                }
            });

            xhr.addEventListener('error', () => {
                progress.style.display = 'none';
                status.innerHTML = '<div class="status error">❌ 网络错误，请检查连接</div>';
            });

            xhr.open('POST', '/upload');
            xhr.send(formData);
        }
    </script>
</body>
</html>)HTML";
}
