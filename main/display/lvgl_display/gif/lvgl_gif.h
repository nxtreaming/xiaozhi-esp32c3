#pragma once

#include <lvgl.h>
#include <functional>
#include <memory>

// Forward declare decoder struct without redefining typedef from gifdec.h
struct _gd_GIF;

class LvglGif {
public:
    explicit LvglGif(const lv_image_dsc_t* img_dsc);
    ~LvglGif();

    // LVGL image descriptor backed by decoder canvas (ARGB8888)
    const lv_image_dsc_t* image_dsc() const;

    void Start();
    void Pause();
    void Resume();
    void Stop();
    void Restart();
    // Set a minimum interval (ms) between frames to cap FPS
    void SetMinFrameInterval(uint32_t ms);

    bool IsPlaying() const;
    bool IsLoaded() const;

    int32_t GetLoopCount() const;
    void SetLoopCount(int32_t count);

    uint16_t width() const;
    uint16_t height() const;

    void SetFrameCallback(std::function<void()> callback);

private:
    void NextFrame();
    void Cleanup();

private:
    _gd_GIF* gif_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    lv_image_dsc_t img_dsc_{};
    lv_image_dsc_t img565_dsc_{};
    uint16_t* buf565_ = nullptr;
    const uint8_t* src_data_ = nullptr;
    size_t src_size_ = 0;
    uint32_t last_call_ = 0;
    uint32_t min_interval_ms_ = 0; // 0 = no cap, otherwise enforce min interval
    bool playing_ = false;
    bool loaded_ = false;
    std::function<void()> frame_callback_;
};
