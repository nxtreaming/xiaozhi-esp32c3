#ifndef OFFLINE_IMAGE_MANAGER_H
#define OFFLINE_IMAGE_MANAGER_H

#include <string>
#include <vector>
#include <functional>

/**
 * @brief 离线图片管理器
 * 
 * 提供完全离线的图片管理功能，不依赖语音识别服务器
 * 支持按键控制和Web界面控制
 */
class OfflineImageManager {
public:
    struct ImageInfo {
        std::string filename;
        size_t size;
        std::string display_name;
    };

    using ImageListCallback = std::function<void(const std::vector<ImageInfo>& images)>;
    using StatusCallback = std::function<void(const std::string& message)>;

    static OfflineImageManager& GetInstance();

    /**
     * @brief 初始化离线图片管理器
     */
    void Initialize();

    /**
     * @brief 设置状态回调（用于显示消息）
     */
    void SetStatusCallback(StatusCallback callback);

    /**
     * @brief 启动图片上传服务
     * @param ssid_prefix WiFi热点名称前缀
     * @return 是否启动成功
     */
    bool StartImageUploadService(const std::string& ssid_prefix = "ImageUpload");

    /**
     * @brief 停止图片上传服务
     */
    void StopImageUploadService();

    /**
     * @brief 检查图片上传服务是否运行
     */
    bool IsImageUploadServiceRunning() const;

    /**
     * @brief 检查是否处于图片浏览模式
     */
    bool IsBrowsingImages() const;

    /**
     * @brief 获取图片上传服务信息
     */
    std::string GetImageUploadServiceInfo() const;

    /**
     * @brief 获取存储的图片列表
     */
    std::vector<ImageInfo> GetStoredImages();


    /**
     * @brief 删除指定的图片
     * @param filename 图片文件名
     * @return 是否成功
     */
    bool DeleteStoredImage(const std::string& filename);

    /**
     * @brief 清空所有存储的图片
     * @return 删除的文件数量
     */
    int ClearAllImages();

    /**
     * @brief 获取存储空间信息
     * @param total_bytes 总容量（字节）
     * @param used_bytes 已使用容量（字节）
     * @return 是否成功获取
     */
    bool GetStorageInfo(size_t& total_bytes, size_t& used_bytes);

    /**
     * @brief 按键控制处理
     * 循环切换功能：启动服务 -> 显示图片列表 -> 显示下一张图片 -> ... -> 停止服务
     */
    void HandleButtonPress();

    /**
     * @brief 长按按键处理
     * 执行管理操作：清空图片、获取存储信息等
     */
    void HandleButtonLongPress();

private:
    OfflineImageManager() = default;
    ~OfflineImageManager() = default;
    OfflineImageManager(const OfflineImageManager&) = delete;
    OfflineImageManager& operator=(const OfflineImageManager&) = delete;

    StatusCallback status_callback_;
    
    // 按键控制状态
    enum class ButtonState {
        IDLE,           // 空闲状态
        SERVICE_RUNNING, // 服务运行中
        BROWSING_IMAGES  // 浏览图片中
    };
    
    ButtonState button_state_ = ButtonState::IDLE;
    std::vector<ImageInfo> current_images_;
    size_t current_image_index_ = 0;

    void ShowStatus(const std::string& message);
    void UpdateImageList();
};

#endif // OFFLINE_IMAGE_MANAGER_H
