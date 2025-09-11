#include "lvgl_gif.h"

#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "gifdec.h"
}

static const char* TAG = "LvglGif";

LvglGif::LvglGif(const lv_image_dsc_t* img_dsc) {
    if (!img_dsc || !img_dsc->data || img_dsc->data_size == 0) {
        ESP_LOGE(TAG, "Invalid image descriptor");
        return;
    }

    gif_ = gd_open_gif_data(img_dsc->data);
    if (!gif_) {
        ESP_LOGE(TAG, "Failed to open GIF from image descriptor");
        return;
    }
    src_data_ = (const uint8_t*)img_dsc->data;
    src_size_ = img_dsc->data_size;

    // Prepare LVGL image descriptor pointing to decoder canvas (ARGB8888)
    std::memset(&img_dsc_, 0, sizeof(img_dsc_));
    img_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
    img_dsc_.header.cf = LV_COLOR_FORMAT_ARGB8888;
    img_dsc_.header.w = gif_->width;
    img_dsc_.header.h = gif_->height;
    img_dsc_.header.stride = gif_->width * 4;
    img_dsc_.data = gif_->canvas;
    img_dsc_.data_size = gif_->width * gif_->height * 4;

    // Render first frame to initialize canvas
    if (gif_->canvas) {
        gd_render_frame(gif_, gif_->canvas);
    }
    loaded_ = true;

    // Allocate explicit RGB565 buffer to avoid LVGL internal conversion buffers
    size_t px_cnt = (size_t)gif_->width * (size_t)gif_->height;
    buf565_ = (uint16_t*)lv_malloc(px_cnt * 2);
    if (buf565_) {
        std::memset(&img565_dsc_, 0, sizeof(img565_dsc_));
        img565_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        img565_dsc_.header.flags = 0;
        img565_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        img565_dsc_.header.w = gif_->width;
        img565_dsc_.header.h = gif_->height;
        img565_dsc_.header.stride = gif_->width * 2;
        img565_dsc_.data = reinterpret_cast<const uint8_t*>(buf565_);
        img565_dsc_.data_size = px_cnt * 2;
    }

    // Disable image caches while GIF is playing to avoid per-frame accumulation
    lv_image_cache_resize(0, true);
    lv_image_header_cache_resize(0, true);
}

LvglGif::~LvglGif() {
    Cleanup();
}

const lv_image_dsc_t* LvglGif::image_dsc() const {
    if (!loaded_) return nullptr;
    if (buf565_) return &img565_dsc_;
    return &img_dsc_;
}

void LvglGif::Start() {
    if (!loaded_ || !gif_) return;
    if (!timer_) {
        // Use a longer timer period to reduce CPU load (was 10ms)
        timer_ = lv_timer_create([](lv_timer_t* t){
            auto* self = static_cast<LvglGif*>(lv_timer_get_user_data(t));
            self->NextFrame();
        }, 80, this);
    }
    playing_ = true;
    last_call_ = lv_tick_get();
    lv_timer_resume(timer_);
    lv_timer_reset(timer_);
    NextFrame();
}

void LvglGif::Pause() {
    if (timer_) lv_timer_pause(timer_);
    playing_ = false;
    // Keep decoder loaded to meet "load once, then show/hide" requirement
}

void LvglGif::Resume() {
    if (!loaded_ || !gif_ || !timer_) return;
    playing_ = true;
    lv_timer_resume(timer_);
}

void LvglGif::Stop() {
    Cleanup();
}

void LvglGif::Restart() {
    if (!loaded_) return;
    // Re-open decoder if needed
    if (!gif_ && src_data_ && src_size_ > 0) {
        gif_ = gd_open_gif_data(src_data_);
        if (!gif_) return;
        // Rebuild ARGB8888 descriptor backing pointer
        img_dsc_.header.w = gif_->width;
        img_dsc_.header.h = gif_->height;
        img_dsc_.header.stride = gif_->width * 4;
        img_dsc_.data = gif_->canvas;
        img_dsc_.data_size = gif_->width * gif_->height * 4;
        if (gif_->canvas) gd_render_frame(gif_, gif_->canvas);
    } else if (gif_) {
        gd_rewind(gif_);
    }
    playing_ = true;
    if (timer_) { lv_timer_resume(timer_); lv_timer_reset(timer_); }
    NextFrame();
}

void LvglGif::SetMinFrameInterval(uint32_t ms) {
    min_interval_ms_ = ms;
}

bool LvglGif::IsPlaying() const { return playing_; }
bool LvglGif::IsLoaded() const { return loaded_; }

int32_t LvglGif::GetLoopCount() const { return (loaded_ && gif_) ? gif_->loop_count : -1; }
void LvglGif::SetLoopCount(int32_t count) { if (loaded_ && gif_) gif_->loop_count = count; }

uint16_t LvglGif::width() const { return (loaded_ && gif_) ? gif_->width : 0; }
uint16_t LvglGif::height() const { return (loaded_ && gif_) ? gif_->height : 0; }

void LvglGif::SetFrameCallback(std::function<void()> cb) { frame_callback_ = std::move(cb); }

void LvglGif::NextFrame() {
    if (!loaded_ || !gif_ || !playing_) return;
    uint32_t elapsed = lv_tick_elaps(last_call_);
    uint32_t required_ms = (uint32_t)gif_->gce.delay * 10; // GIF delay is in 10ms units
    if (min_interval_ms_ > required_ms) required_ms = min_interval_ms_;
    if (elapsed < required_ms) return;
    last_call_ = lv_tick_get();

    int has_next = gd_get_frame(gif_);
    if (has_next == 0) {
        playing_ = false;
        if (timer_) lv_timer_pause(timer_);
        return;
    }
    if (gif_->canvas) {
        gd_render_frame(gif_, gif_->canvas);
        // Convert BGRA canvas to RGB565 in our buffer to avoid LVGL conversion allocations
        if (buf565_) {
            const uint8_t* src = (const uint8_t*)gif_->canvas;
            uint16_t* dst = buf565_;
            size_t px = (size_t)gif_->width * (size_t)gif_->height;
            for (size_t i = 0; i < px; ++i) {
                uint8_t b = src[i*4 + 0];
                uint8_t g = src[i*4 + 1];
                uint8_t r = src[i*4 + 2];
                uint16_t rgb565 = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                dst[i] = rgb565;
            }
        }
        if (frame_callback_) frame_callback_();
    }
    // Yield a tick to avoid starving other tasks (AFE/WDT)
    vTaskDelay(1);
}

void LvglGif::Cleanup() {
    // Log memory deltas around cleanup steps to diagnose leaks
    size_t spiram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    size_t spiram_after_timer = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "SPIRAM after timer delete: %u (delta %+d)", (unsigned)spiram_after_timer, (int)(spiram_after_timer - spiram_before));

    if (gif_) { gd_close_gif(gif_); gif_ = nullptr; }
    size_t spiram_after_gif = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "SPIRAM after gd_close_gif: %u (delta %+d)", (unsigned)spiram_after_gif, (int)(spiram_after_gif - spiram_after_timer));
    playing_ = false;
    loaded_ = false;
    // Drop any cached decoded buffers and keep caches disabled to prevent re-alloc per cycle
    lv_image_cache_drop(NULL);
    lv_image_cache_resize(0, true);
    lv_image_header_cache_resize(0, true);

    size_t spiram_after_cache = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "SPIRAM after cache drop: %u (delta %+d)", (unsigned)spiram_after_cache, (int)(spiram_after_cache - spiram_after_gif));

    if (buf565_) {
        lv_free(buf565_);
        buf565_ = nullptr;
    }
    size_t spiram_after_buf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "SPIRAM after buf565 free: %u (delta %+d)", (unsigned)spiram_after_buf, (int)(spiram_after_buf - spiram_after_cache));

    std::memset(&img_dsc_, 0, sizeof(img_dsc_));
    std::memset(&img565_dsc_, 0, sizeof(img565_dsc_));
}
