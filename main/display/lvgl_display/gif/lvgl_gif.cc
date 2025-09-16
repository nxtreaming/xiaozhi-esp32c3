#include "lvgl_gif.h"
#include <esp_log.h>
#include <cstring>

#define TAG "LvglGif"

LvglGif::LvglGif(const lv_img_dsc_t* img_dsc)
    : gif_(nullptr), timer_(nullptr), last_call_(0), playing_(false), loaded_(false) {
    if (!img_dsc || !img_dsc->data) {
        ESP_LOGE(TAG, "Invalid image descriptor");
        return;
    }

    gif_ = gd_open_gif_data(img_dsc->data);
    if (!gif_) {
        ESP_LOGE(TAG, "Failed to open GIF from image descriptor");
    }

    // Setup LVGL image descriptor
    memset(&img_dsc_, 0, sizeof(img_dsc_));
    img_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
#if GIFDEC_USE_RGB565
    img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    img_dsc_.header.w = gif_->width;
    img_dsc_.header.h = gif_->height;
    img_dsc_.header.stride = gif_->width * 2;
    img_dsc_.data = gif_->canvas;
    img_dsc_.data_size = gif_->width * gif_->height * 2;
#else
    img_dsc_.header.cf = LV_COLOR_FORMAT_ARGB8888;
    img_dsc_.header.w = gif_->width;
    img_dsc_.header.h = gif_->height;
    img_dsc_.header.stride = gif_->width * 4;
    img_dsc_.data = gif_->canvas;
    img_dsc_.data_size = gif_->width * gif_->height * 4;
#endif

    // Render first frame
    if (gif_->canvas) {
        gd_render_frame(gif_, gif_->canvas);
    }

    loaded_ = true;
    ESP_LOGI(TAG, "GIF loaded from image descriptor: %dx%d", gif_->width, gif_->height);
}

// Destructor
LvglGif::~LvglGif() {
    Cleanup();
}

// LvglImage interface implementation
const lv_img_dsc_t* LvglGif::image_dsc() const {
    if (!loaded_) {
        return nullptr;
    }
    return &img_dsc_;
}

// Animation control methods
void LvglGif::Start() {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot start");
        return;
    }

    playing_ = true;
    last_call_ = lv_tick_get();

    if (decode_task_ == nullptr) {
        BaseType_t ok = xTaskCreate(
            DecoderTaskTrampoline, "gif_decode", 4096, this, tskIDLE_PRIORITY + 2, &decode_task_);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create GIF decoder task");
            decode_task_ = nullptr;
            playing_ = false;
            return;
        }
    }

    ESP_LOGI(TAG, "GIF animation started (decoder task)");
}

void LvglGif::Pause() {
    playing_ = false;
    ESP_LOGI(TAG, "GIF animation paused");
}

void LvglGif::Resume() {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot resume");
        return;
    }
    playing_ = true;
    ESP_LOGI(TAG, "GIF animation resumed");
}

void LvglGif::Stop() {
    playing_ = false;
    if (gif_) {
        gd_rewind(gif_);
        ESP_LOGI(TAG, "GIF animation stopped and rewound");
    }
}

bool LvglGif::IsPlaying() const {
    return playing_;
}

bool LvglGif::IsLoaded() const {
    return loaded_;
}

int32_t LvglGif::GetLoopCount() const {
    if (!loaded_ || !gif_) {
        return -1;
    }
    return gif_->loop_count;
}

void LvglGif::SetLoopCount(int32_t count) {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot set loop count");
        return;
    }
    gif_->loop_count = count;
}

uint16_t LvglGif::width() const {
    if (!loaded_ || !gif_) {
        return 0;
    }
    return gif_->width;
}

uint16_t LvglGif::height() const {
    if (!loaded_ || !gif_) {
        return 0;
    }
    return gif_->height;
}

void LvglGif::SetFrameCallback(std::function<void()> callback) {
    frame_callback_ = callback;
}

// Static
void LvglGif::AsyncFrameCb(void* user_data) {
    LvglGif* self = static_cast<LvglGif*>(user_data);
    if (self && self->frame_callback_) {
        self->frame_callback_();
    }
}

void LvglGif::DecoderTaskTrampoline(void* arg) {
    LvglGif* self = static_cast<LvglGif*>(arg);
    if (self) self->DecoderLoop();
}

void LvglGif::DecoderLoop() {
    for (;;) {
        if (decode_task_ == nullptr) {
            vTaskDelete(nullptr);
        }
        if (!playing_ || !gif_) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t elapsed = lv_tick_elaps(last_call_);
        uint32_t frame_ms = (uint32_t)gif_->gce.delay * 10u;
        if (frame_ms < 40u) frame_ms = 40u;
        if (elapsed < frame_ms) {
            vTaskDelay(pdMS_TO_TICKS(frame_ms - elapsed));
            continue;
        }
        last_call_ = lv_tick_get();

        // Decode next frame (heavy work off LVGL thread)
        int has_next = gd_get_frame(gif_);
        if (has_next == 0) {
            playing_ = false;
            continue;
        }
        if (gif_->canvas) {
            gd_render_frame(gif_, gif_->canvas);
            // Notify LVGL to refresh image in LVGL thread
            lv_async_call(AsyncFrameCb, this);
        }
        taskYIELD();
    }
}

void LvglGif::NextFrame() {
    // Kept for compatibility but unused when decoder task is enabled
    if (!loaded_ || !gif_) return;
}

void LvglGif::Cleanup() {
    // Stop decoder task if running
    playing_ = false;
    if (decode_task_ != nullptr) {
        TaskHandle_t t = decode_task_;
        decode_task_ = nullptr;
        vTaskDelete(t);
    }

    // Delete (unused) LVGL timer if any
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    // Close GIF decoder
    if (gif_) {
        gd_close_gif(gif_);
        gif_ = nullptr;
    }

    loaded_ = false;
    
    // Clear image descriptor
    memset(&img_dsc_, 0, sizeof(img_dsc_));
}
