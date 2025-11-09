#include "lvgl_gif.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>
#include "sdkconfig.h"

// Forward declarations to avoid including esp_lvgl_port.h here
extern "C" bool lvgl_port_lock(int timeout_ms);
extern "C" void lvgl_port_unlock(void);

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

    // Decode and render the very first frame synchronously so something is visible immediately
    if (gif_) {
        int ret = gd_get_frame(gif_);
        if (ret < 0) {
            ESP_LOGW(TAG, "Failed to decode first frame");
        }
        if (gif_->canvas) {
            gd_render_frame(gif_, gif_->canvas);
        }
    }
    // First frame is considered index 0
    frame_index_ = 0;

    loaded_ = true;
    last_call_ = lv_tick_get();
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

    // If user requested infinite looping, implement it via single-pass + manual rewind
    if (force_infinite_ && gif_->loop_count != 1) {
        ESP_LOGI(TAG, "Forcing infinite: setting loop_count from %d to 1 (manual rewind)", (int)gif_->loop_count);
        gif_->loop_count = 1;
    }

    playing_ = true;
    last_call_ = lv_tick_get();

    // Run decoding on LVGL thread via timer to avoid cross-thread LVGL allocations
    if (timer_ == nullptr) {
        timer_ = lv_timer_create(LvglGif::TimerCb, 5, this); // small period; we self-throttle by frame delay
        if (!timer_) {
            ESP_LOGE(TAG, "Failed to create LVGL timer for GIF");
            playing_ = false;
            return;
        }
    }
    lv_timer_resume(timer_);

    ESP_LOGI(TAG, "GIF animation started (lv_timer)");
}

void LvglGif::Pause() {
    playing_ = false;
    if (timer_) lv_timer_pause(timer_);
    ESP_LOGI(TAG, "GIF animation paused");
}

void LvglGif::Resume() {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot resume");
        return;
    }
    playing_ = true;
    if (timer_) lv_timer_resume(timer_);
    ESP_LOGI(TAG, "GIF animation resumed");
}

void LvglGif::Stop() {
    playing_ = false;
    if (timer_) lv_timer_pause(timer_);
    if (gif_) {
        gd_rewind(gif_);
        frame_index_ = 0; // reset frame index on rewind
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
    force_infinite_ = (count == 0);
    if (force_infinite_) {
        // use single-pass + manual rewind path
        gif_->loop_count = 1;
    } else {
        gif_->loop_count = count;
    }
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

// LVGL timer callback: run one decode tick on LVGL thread
void LvglGif::TimerCb(lv_timer_t* t) {
    if (!t) return;
    LvglGif* self = static_cast<LvglGif*>(lv_timer_get_user_data(t));
    if (!self) return;
    self->TickOnce();
}

void LvglGif::TickOnce() {
    if (!playing_ || !gif_) {
        return;
    }

    uint32_t elapsed = lv_tick_elaps(last_call_);
    uint32_t orig_ms = (uint32_t)gif_->gce.delay * 10u;
    // Heuristic: large frames need some throttle to avoid starving LVGL
    const uint32_t pixels = (uint32_t)gif_->width * (uint32_t)gif_->height;
    const bool heavy_frame = (pixels >= 160000u); // ~400x400 and above
    const uint32_t min_ms = heavy_frame ? 60u : 30u;
    uint32_t frame_ms = orig_ms == 0u ? min_ms : (orig_ms < min_ms ? min_ms : orig_ms);
    if (elapsed < frame_ms) {
        return;
    }
    last_call_ = lv_tick_get();

    // Decode next frame (we are on LVGL thread so lv_malloc is safe)
    int has_next = gd_get_frame(gif_);
    if (has_next <= 0) {
        if (has_next == 0) {
            ESP_LOGI(TAG, "GIF reached trailer (loop_count=%d)", (int)gif_->loop_count);
        } else {
            ESP_LOGI(TAG, "gd_get_frame returned error (%d); treating as end", has_next);
        }
        if (force_infinite_) {
            gd_rewind(gif_);
            gif_->loop_count = 1; // keep single-pass scheme for manual infinite loop
            frame_index_ = 0;
            ESP_LOGI(TAG, "GIF rewound for infinite loop (manual), loop_count=%d", (int)gif_->loop_count);
            return;
        } else {
            playing_ = false;
            if (timer_) lv_timer_pause(timer_);
            return;
        }
    }

    // Increase decoded frame index (first decoded after ctor's initial render is 1)
    frame_index_++;

    if (gif_->canvas) {
        // Render to canvas and notify UI
        gd_render_frame(gif_, gif_->canvas);
        if (frame_callback_) {
            frame_callback_(); // already in LVGL thread
        }
    }
}

void LvglGif::NextFrame() {
    // Kept for compatibility but unused when decoder task is enabled
    if (!loaded_ || !gif_) return;
}

void LvglGif::Cleanup() {
    // Stop playing ASAP to prevent new frame schedules
    playing_ = false;

    // Cancel any pending async callbacks that reference this object
    // Call in a loop to ensure all queued timers are removed
    while (lv_async_call_cancel(LvglGif::AsyncFrameCb, this) == LV_RESULT_OK) {
        // keep cancelling until none left
    }

    // Request decoder task to stop and self-delete safely before freeing gif_
    if (decode_task_ != nullptr) {
        TaskHandle_t t = decode_task_;
        // Signal the task to exit by nulling the handle (DecoderLoop checks this)
        decode_task_ = nullptr;
        // Wait until the task actually deletes itself to avoid races with gd_* access
        // Cap the wait to avoid infinite loop in pathological cases
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        while (eTaskGetState(t) != eDeleted && (xTaskGetTickCount() < deadline)) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Double-check: cancel any stragglers after the task has stopped
    while (lv_async_call_cancel(LvglGif::AsyncFrameCb, this) == LV_RESULT_OK) {
    }

    // Free static task resources if allocated
    if (decode_stack_) { heap_caps_free(decode_stack_); decode_stack_ = nullptr; decode_stack_words_ = 0; }
    if (decode_tcb_)   { heap_caps_free(decode_tcb_);   decode_tcb_ = nullptr; }

    // Delete (unused) LVGL timer if any
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    // Close GIF decoder (after decoder task is gone)
    if (gif_) {
        gd_close_gif(gif_);
        gif_ = nullptr;
    }

    loaded_ = false;

    // Clear image descriptor
    memset(&img_dsc_, 0, sizeof(img_dsc_));
}
