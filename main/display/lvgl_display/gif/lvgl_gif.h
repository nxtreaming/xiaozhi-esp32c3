#pragma once

#include "../lvgl_image.h"
#include "gifdec.h"
#include <lvgl.h>
#include <memory>
#include <functional>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * C++ implementation of LVGL GIF widget
 * Provides GIF animation functionality using gifdec library
 */
class LvglGif {
public:
    explicit LvglGif(const lv_img_dsc_t* img_dsc);
    virtual ~LvglGif();

    // LvglImage interface implementation
    virtual const lv_img_dsc_t* image_dsc() const;

    /**
     * Start/restart GIF animation
     */
    void Start();

    /**
     * Pause GIF animation
     */
    void Pause();

    /**
     * Resume GIF animation
     */
    void Resume();

    /**
     * Stop GIF animation and rewind to first frame
     */
    void Stop();

    /**
     * Check if GIF is currently playing
     */
    bool IsPlaying() const;

    /**
     * Check if GIF was loaded successfully
     */
    bool IsLoaded() const;

    /**
     * Get loop count
     */
    int32_t GetLoopCount() const;

    /**
     * Set loop count
     */
    void SetLoopCount(int32_t count);

    /**
     * Get GIF dimensions
     */
    uint16_t width() const;
    uint16_t height() const;

    /**
     * Set frame update callback
     */
    void SetFrameCallback(std::function<void()> callback);

private:
    // GIF decoder instance
    gd_GIF* gif_;

    // LVGL image descriptor
    lv_img_dsc_t img_dsc_;

    // Animation timer (runs in LVGL thread)
    lv_timer_t* timer_;

    // Last frame update time
    uint32_t last_call_;

    // Decoded frame index (first displayed frame rendered in ctor is index 0)
    uint32_t frame_index_ = 0;

    // Animation state
    std::atomic<bool> playing_;
    bool loaded_;
    // If true, implement infinite looping by forcing single-pass + manual rewind
    bool force_infinite_ = false;

    // Frame update callback
    std::function<void()> frame_callback_;

    // (Legacy) Background decoder members kept for compatibility but unused now
    TaskHandle_t decode_task_ = nullptr;
    StaticTask_t* decode_tcb_ = nullptr;
    StackType_t* decode_stack_ = nullptr; // in words
    uint32_t decode_stack_words_ = 0;
    // Async callback executed in LVGL thread to notify frame updated
    static void AsyncFrameCb(void* user_data);

    // LVGL timer callback and one-shot tick handler (decode on LVGL thread)
    static void TimerCb(lv_timer_t* t);
    void TickOnce();

    /**
     * Update to next frame (kept for compatibility if needed)
     */
    void NextFrame();

    /**
     * Cleanup resources
     */
    void Cleanup();
};
