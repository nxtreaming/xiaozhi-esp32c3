#pragma once

#include "iot/thing.h"
#include <memory>

namespace iot {

class ImageUploadControl : public Thing {
public:
    ImageUploadControl();
    
private:
    // IoT方法实现
    std::string StartImageUploadServer(const std::string& params);
    std::string StopImageUploadServer(const std::string& params);
    std::string GetImageUploadServerStatus(const std::string& params);
};

// 工厂函数
std::unique_ptr<Thing> CreateImageUploadControl();

} // namespace iot
