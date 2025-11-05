#pragma once

#include "iot/thing.h"
#include <memory>
#include <vector>
#include <string>

namespace iot {

class ImageStorageControl : public Thing {
public:
    ImageStorageControl();
    
private:
    // IoT方法实现
    std::string ListStoredImages(const std::string& params);
    std::string ShowStoredImage(const std::string& params);
    std::string DeleteStoredImage(const std::string& params);
    std::string GetStorageInfo(const std::string& params);
    std::string ClearAllImages(const std::string& params);
    
    // 辅助方法
    void UpdateStorageStatus();
};

// 工厂函数
std::unique_ptr<Thing> CreateImageStorageControl();

} // namespace iot
