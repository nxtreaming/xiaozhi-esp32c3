#include "lcd_display.h"

#include <vector>
#include <font_awesome_symbols.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include "assets/lang_config.h"
#include <cstring>
#include "settings.h"

#include "board.h"
#include <math.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <lvgl.h>

extern "C" {
#include "storage/gif_storage.h"
}

#define TAG "LcdDisplay"

// Color definitions for dark theme
#define DARK_BACKGROUND_COLOR       lv_color_hex(0x121212)     // Dark background
#define DARK_TEXT_COLOR             lv_color_white()           // White text
#define DARK_CHAT_BACKGROUND_COLOR  lv_color_hex(0x1E1E1E)     // Slightly lighter than background
#define DARK_USER_BUBBLE_COLOR      lv_color_hex(0x1A6C37)     // Dark green
#define DARK_ASSISTANT_BUBBLE_COLOR lv_color_hex(0x333333)     // Dark gray
#define DARK_SYSTEM_BUBBLE_COLOR    lv_color_hex(0x2A2A2A)     // Medium gray
#define DARK_SYSTEM_TEXT_COLOR      lv_color_hex(0xAAAAAA)     // Light gray text
#define DARK_BORDER_COLOR           lv_color_hex(0x333333)     // Dark gray border
#define DARK_LOW_BATTERY_COLOR      lv_color_hex(0xFF0000)     // Red for dark mode

// Color definitions for light theme
#define LIGHT_BACKGROUND_COLOR       lv_color_white()           // White background
#define LIGHT_TEXT_COLOR             lv_color_black()           // Black text
#define LIGHT_CHAT_BACKGROUND_COLOR  lv_color_hex(0xE0E0E0)     // Light gray background
#define LIGHT_USER_BUBBLE_COLOR      lv_color_hex(0x95EC69)     // WeChat green
#define LIGHT_ASSISTANT_BUBBLE_COLOR lv_color_white()           // White
#define LIGHT_SYSTEM_BUBBLE_COLOR    lv_color_hex(0xE0E0E0)     // Light gray
#define LIGHT_SYSTEM_TEXT_COLOR      lv_color_hex(0x666666)     // Dark gray text
#define LIGHT_BORDER_COLOR           lv_color_hex(0xE0E0E0)     // Light gray border
#define LIGHT_LOW_BATTERY_COLOR      lv_color_black()           // Black for light mode

// Theme color structure
struct ThemeColors {
    lv_color_t background;
    lv_color_t text;
    lv_color_t chat_background;
    lv_color_t user_bubble;
    lv_color_t assistant_bubble;
    lv_color_t system_bubble;
    lv_color_t system_text;
    lv_color_t border;
    lv_color_t low_battery;
};

// Define dark theme colors
static const ThemeColors DARK_THEME = {
    .background = DARK_BACKGROUND_COLOR,
    .text = DARK_TEXT_COLOR,
    .chat_background = DARK_CHAT_BACKGROUND_COLOR,
    .user_bubble = DARK_USER_BUBBLE_COLOR,
    .assistant_bubble = DARK_ASSISTANT_BUBBLE_COLOR,
    .system_bubble = DARK_SYSTEM_BUBBLE_COLOR,
    .system_text = DARK_SYSTEM_TEXT_COLOR,
    .border = DARK_BORDER_COLOR,
    .low_battery = DARK_LOW_BATTERY_COLOR
};

// Define light theme colors
static const ThemeColors LIGHT_THEME = {
    .background = LIGHT_BACKGROUND_COLOR,
    .text = LIGHT_TEXT_COLOR,
    .chat_background = LIGHT_CHAT_BACKGROUND_COLOR,
    .user_bubble = LIGHT_USER_BUBBLE_COLOR,
    .assistant_bubble = LIGHT_ASSISTANT_BUBBLE_COLOR,
    .system_bubble = LIGHT_SYSTEM_BUBBLE_COLOR,
    .system_text = LIGHT_SYSTEM_TEXT_COLOR,
    .border = LIGHT_BORDER_COLOR,
    .low_battery = LIGHT_LOW_BATTERY_COLOR
};

// Current theme - initialize based on default config
static ThemeColors current_theme = LIGHT_THEME;

LV_FONT_DECLARE(font_awesome_30_4);

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy,
                           DisplayFonts fonts)
    : LcdDisplay(panel_io, panel, fonts) {
    width_ = width;
    height_ = height;

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    // Restore safer LVGL task stack to avoid overflow at startup
    port_cfg.task_stack = 6144;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    // Choose multi-line buffer in internal DMA memory (~3–4KB target for SPI) and enable double buffer
    const uint32_t bytes_per_line_spi = static_cast<uint32_t>(width_) * 2u;
    uint32_t lines_spi = (4u * 1024u) / (bytes_per_line_spi ? bytes_per_line_spi : 1u);
    if (lines_spi < 4u) lines_spi = 4u;       // at least 4 lines to reduce flush blocking
    if (lines_spi > 16u) lines_spi = 16u;     // cap to reasonable upper bound
    const uint32_t buffer_size_spi = bytes_per_line_spi * lines_spi;
    // Adjust by internal RAM availability to keep headroom for tasks
    size_t free_int_spi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t kReserveForTasks = 20 * 1024; // leave headroom for decoder task etc.
    bool use_spiram_buf_spi = false;
    uint32_t chosen_buffer_size_spi = buffer_size_spi;
    if (free_int_spi <= (kReserveForTasks + bytes_per_line_spi * 2u)) {
        use_spiram_buf_spi = true; // not enough headroom for 2 lines
    } else {
        size_t max_for_buf = free_int_spi - kReserveForTasks;
        uint32_t max_rounded = (uint32_t)(max_for_buf / bytes_per_line_spi) * bytes_per_line_spi;
        if (max_rounded < chosen_buffer_size_spi) chosen_buffer_size_spi = max_rounded;
        if (chosen_buffer_size_spi < bytes_per_line_spi * 2u) {
            use_spiram_buf_spi = true; // fallback if too small
            chosen_buffer_size_spi = buffer_size_spi; // keep multi-line size but in PSRAM
        }
    }

    (void)use_spiram_buf_spi;  // silence unused warning when forcing PSRAM buffer


    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = chosen_buffer_size_spi,
        // Use single buffering to cut internal SRAM usage roughly in half
        .double_buffer = false,
        // SPI path: keep draw buffer in internal DMA memory, no trans buffer needed
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            // Use internal DMA-capable draw buffer for SPI for reliable flush
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // Update the theme
    if (current_theme_name_ == "dark") {
        current_theme = DARK_THEME;
    } else if (current_theme_name_ == "light") {
        current_theme = LIGHT_THEME;
    }

    SetupUI();
}

// RGB LCD实现
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy,
                           DisplayFonts fonts)
    : LcdDisplay(panel_io, panel, fonts) {
    width_ = width;
    height_ = height;

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    lvgl_port_init(&port_cfg);

    // Choose multi-line single buffer in internal DMA memory (~14KB target)
    const uint32_t bytes_per_line_rgb = static_cast<uint32_t>(width_) * 2u;
    uint32_t lines_rgb = (14u * 1024u) / (bytes_per_line_rgb ? bytes_per_line_rgb : 1u);
    if (lines_rgb < 4u) lines_rgb = 4u;
    if (lines_rgb > 24u) lines_rgb = 24u;
    const uint32_t buffer_size_rgb = bytes_per_line_rgb * lines_rgb;
    // Adjust by internal RAM availability to keep headroom for tasks
    size_t free_int_rgb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t kReserveForTasksRgb = 20 * 1024;
    bool use_spiram_buf_rgb = false;
    uint32_t chosen_buffer_size_rgb = buffer_size_rgb;
    if (free_int_rgb <= (kReserveForTasksRgb + bytes_per_line_rgb * 2u)) {
        use_spiram_buf_rgb = true;
    } else {
        size_t max_for_buf = free_int_rgb - kReserveForTasksRgb;
        uint32_t max_rounded = (uint32_t)(max_for_buf / bytes_per_line_rgb) * bytes_per_line_rgb;
        if (max_rounded < chosen_buffer_size_rgb) chosen_buffer_size_rgb = max_rounded;
        if (chosen_buffer_size_rgb < bytes_per_line_rgb * 2u) {
            use_spiram_buf_rgb = true;
            chosen_buffer_size_rgb = buffer_size_rgb;
        }
    }

    (void)use_spiram_buf_rgb;  // silence unused warning when forcing PSRAM buffer


    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        // Reduce draw buffer to lower internal RAM usage
        // Further reduce draw buffer to ease internal RAM pressure
        .buffer_size = chosen_buffer_size_rgb,
        // Disable double buffering to save memory on tight systems
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 0,
            // Force LVGL draw buffer to PSRAM to minimize internal SRAM usage
            .buff_spiram = 1,
            .swap_bytes = 0,
            // Re-enable full_refresh/direct_mode to reduce intermediate copies
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            // Disable big buffer mode to reduce internal RAM footprint
            .bb_mode = false,
            // Disable tearing avoidance to save additional internal buffers
            .avoid_tearing = false,
        }
    };

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // Update the theme
    if (current_theme_name_ == "dark") {
        current_theme = DARK_THEME;
    } else if (current_theme_name_ == "light") {
        current_theme = LIGHT_THEME;
    }

    SetupUI();
}

// Style for GIF image object (initialized once)
static lv_style_t s_gif_style;
static bool s_gif_style_inited = false;

static void ensure_gif_style() {
    if(s_gif_style_inited)
        return;
    lv_style_init(&s_gif_style);
    lv_style_set_bg_opa(&s_gif_style, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_gif_style, 0);
    lv_style_set_outline_width(&s_gif_style, 0);
    lv_style_set_shadow_width(&s_gif_style, 0);
    // Optional paddings for a tight image box
    lv_style_set_pad_all(&s_gif_style, 0);
    s_gif_style_inited = true;
}

void LcdDisplay::SetGifPos(int x, int y) {
    int cx = x, cy = y;
    if (x == 0 && y == 0 && gif_controller_) {
        cx = (width_ - gif_controller_->width()) / 2;
        cy = (height_ - gif_controller_->height()) / 2;
    }
    if (gif_img_) {
        lv_obj_set_pos(gif_img_, cx, cy);
    }
    if (gif_img_b_) {
        lv_obj_set_pos(gif_img_b_, cx, cy);
    }
}

LcdDisplay::~LcdDisplay() {
    // 先销毁 GIF 控制器并释放托管缓冲区，防止泄漏
    DestroyGif();

    // 再删除 GIF 的 LVGL 对象
    if (gif_img_ != nullptr) {
        lv_obj_del(gif_img_);
        gif_img_ = nullptr;
    }
    if (gif_img_b_ != nullptr) {
        lv_obj_del(gif_img_b_);
        gif_img_b_ = nullptr;
    }

    // 然后再清理 LVGL 其他对象
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (center_message_popup_ != nullptr) {
        lv_obj_del(center_message_popup_);
    }
    if (center_message_timer_ != nullptr) {
        esp_timer_delete(center_message_timer_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, fonts_.text_font, 0);
    lv_obj_set_style_text_color(screen, current_theme.text, 0);
    lv_obj_set_style_bg_color(screen, current_theme.background, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, current_theme.background, 0);
    lv_obj_set_style_border_color(container_, current_theme.border, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, fonts_.emoji_font->line_height);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, current_theme.background, 0);
    lv_obj_set_style_text_color(status_bar_, current_theme.text, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, 5, 0);
    lv_obj_set_style_bg_color(content_, current_theme.chat_background, 0); // Background for chat area
    lv_obj_set_style_border_color(content_, current_theme.border, 0); // Border color for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, 10, 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_left(status_bar_, 2, 0);
    lv_obj_set_style_pad_right(status_bar_, 2, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    // 设置状态栏的内容垂直居中
    lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 创建emotion_label_在状态栏最左侧
    emotion_label_ = lv_label_create(status_bar_);
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(emotion_label_, current_theme.text, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);
    lv_obj_set_style_margin_right(emotion_label_, 5, 0); // 添加右边距，与后面的元素分隔

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, current_theme.text, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, current_theme.text, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, current_theme.text, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(network_label_, current_theme.text, 0);
    lv_obj_set_style_margin_left(network_label_, 5, 0); // 添加左边距，与前面的元素分隔

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, current_theme.text, 0);
    lv_obj_set_style_margin_left(battery_label_, 5, 0); // 添加左边距，与前面的元素分隔

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, fonts_.text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, current_theme.low_battery, 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    lv_obj_t* low_battery_label = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label, lv_color_white(), 0);
    lv_obj_center(low_battery_label);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

#define  MAX_MESSAGES 50
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    //避免出现空的消息框
    if(strlen(content) == 0) return;

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 1, 0);
    lv_obj_set_style_border_color(msg_bubble, current_theme.border, 0);
    lv_obj_set_style_pad_all(msg_bubble, 8, 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // 计算文本实际宽度
    lv_coord_t text_width = lv_txt_get_width(content, strlen(content), fonts_.text_font, 0);

    // 计算气泡宽度
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 屏幕宽度的85%
    lv_coord_t min_width = 20;
    lv_coord_t bubble_width;

    // 确保文本宽度不小于最小宽度
    if (text_width < min_width) {
        text_width = min_width;
    }

    // 如果文本宽度小于最大宽度，使用文本宽度
    if (text_width < max_width) {
        bubble_width = text_width;
    } else {
        bubble_width = max_width;
    }

    // 设置消息文本的宽度
    lv_obj_set_width(msg_text, bubble_width);  // 减去padding
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg_text, fonts_.text_font, 0);

    // 设置气泡宽度
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, current_theme.user_bubble, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, current_theme.text, 0);

        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Add some margin
        lv_obj_set_style_margin_right(msg_bubble, 10, 0);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, current_theme.assistant_bubble, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, current_theme.text, 0);

        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Add some margin
        lv_obj_set_style_margin_left(msg_bubble, -4, 0);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, current_theme.system_bubble, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, current_theme.system_text, 0);

        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -10, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // 为系统消息创建全宽容器以确保居中对齐
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // 使容器透明且无边框
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // 将消息气泡移入此容器
        lv_obj_set_parent(msg_bubble, container);

        // 将气泡居中对齐在容器中
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);

        // 自动滚动底部
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;

    // 检查消息数量是否超过限制
    uint32_t msg_count = lv_obj_get_child_cnt(content_);
    while (msg_count >= MAX_MESSAGES) {
        // 删除最早的消息（第一个子节点）
        lv_obj_t* oldest_msg = lv_obj_get_child(content_, 0);
        if (oldest_msg != nullptr) {
            lv_obj_del(oldest_msg);
            msg_count--;
        }else{
            break;
        }
    }
}

#else

#include <stdlib.h>
#include <time.h>
#include <lvgl.h>
#include <esp_system.h>

#define SCREEN_WIDTH 412
#define SCREEN_HEIGHT 412

// 眼睛结构体
typedef struct eye_t {
    lv_obj_t *eye;      // 眼白
    lv_obj_t *pupil;    // 瞳孔
    lv_obj_t *eyelid;   // 上眼皮
} eye_t;

// 创建眼睛
eye_t create_eye(lv_obj_t *parent, int x, int y, int size) {
    eye_t eye;

    // 眼白 (基础圆形)
    eye.eye = lv_obj_create(parent);
    lv_obj_set_size(eye.eye, size, size);
    lv_obj_set_pos(eye.eye, x, y);
    lv_obj_set_style_radius(eye.eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye.eye, lv_color_white(), 0);
    lv_obj_set_style_border_width(eye.eye, 2, 0);
    lv_obj_set_style_border_color(eye.eye, lv_color_black(), 0);

    // 瞳孔 (黑色圆形)
    eye.pupil = lv_obj_create(eye.eye);
    lv_obj_set_size(eye.pupil, size/3, size/3);
    lv_obj_align(eye.pupil, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(eye.pupil, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye.pupil, lv_color_black(), 0);

    // 上眼皮 (半圆形遮罩)
    // eye.eyelid = lv_obj_create(eye.eye);
    // lv_obj_set_size(eye.eyelid, size+4, size/2+2);
    // lv_obj_set_pos(eye.eyelid, -2, -size/2-2); // 初始位置在眼睛上方
    // lv_obj_set_style_radius(eye.eyelid, size/2, 0);
    // lv_obj_set_style_bg_color(eye.eyelid, lv_palette_main(LV_PALETTE_GREY), 0);
    // lv_obj_set_style_bg_opa(eye.eyelid, LV_OPA_COVER, 0);
    return eye;
}

// 随机眨眼动画
void set_random_blink(lv_obj_t *eyelid, int eye_size) {
    lv_anim_t blink;
    lv_anim_init(&blink);
    lv_anim_set_var(&blink, eyelid);
    lv_anim_set_exec_cb(&blink, [](void *var, int32_t v) {
        lv_obj_set_y((lv_obj_t*)var, 120/2 - 2 + v);
    });

    // 随机参数
    int blink_speed = 1000 ; // 100-300ms+  rand() % 200
    int delay = 4000;     // 2-5秒间隔   +  rand() % 3000

    lv_anim_set_values(&blink, 0, 120/2 + 2);
    lv_anim_set_time(&blink, blink_speed);
    lv_anim_set_playback_time(&blink, blink_speed);
    lv_anim_set_repeat_count(&blink, 1);
    lv_anim_set_delay(&blink, delay);
    lv_anim_set_repeat_count(&blink, LV_ANIM_REPEAT_INFINITE);//一直重复动画
    lv_anim_start(&blink);
}

// 随机瞳孔左右移动动画
void set_random_pupil_movement(lv_obj_t *pupil, int eye_size) {
    lv_anim_t move;
    lv_anim_init(&move);
    lv_anim_set_var(&move, pupil);

    // 随机方向 (0:左, 1:右)
    int direction = rand() % 2;
    int distance = 5 + rand() % 10; // 5-15像素

    lv_anim_set_exec_cb(&move, [](void *var, int32_t v) {
        lv_obj_set_x((lv_obj_t*)var, v);
    });

    lv_anim_set_values(&move, 0, direction ? distance : -distance);
    lv_anim_set_time(&move, 500 + rand() % 1500); // 0.5-2秒
    lv_anim_set_playback_time(&move, 500 + rand() % 1500);
    lv_anim_set_repeat_count(&move, 1);

    lv_anim_set_repeat_count(&move, LV_ANIM_REPEAT_INFINITE);//一直重复动画
    lv_anim_start(&move);
}

// 设置眼睛动画
void setup_eye_animations(eye_t *eye, int eye_size) {
    set_random_blink(eye->eye, eye_size);
    set_random_pupil_movement(eye->pupil, eye_size);
}

//#define  ENABLE_EYES_SIMULATION 1

#ifdef ENABLE_EYES_SIMULATION

void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_size(screen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xf0f0f0), 0);

    srand(time(NULL));

    // 计算眼睛在412x412屏幕上的居中位置
    int eye_size = 70;
    int eye_spacing = 140;  // 两眼中心距离，增加到140像素
    int screen_center_x = SCREEN_WIDTH / 2;   // 206
    int screen_center_y = SCREEN_HEIGHT / 2;  // 206

    // 左眼位置：屏幕中心向左偏移一半眼距，再减去眼睛半径
    int left_eye_x = screen_center_x - eye_spacing/2 - eye_size/2;  // 180 - 50 - 35 = 95
    int left_eye_y = screen_center_y - eye_size/2;  // 180 - 35 = 145

    // 右眼位置：屏幕中心向右偏移一半眼距，再减去眼睛半径
    int right_eye_x = screen_center_x + eye_spacing/2 - eye_size/2;  // 180 + 50 - 35 = 195
    int right_eye_y = screen_center_y - eye_size/2;  // 180 - 35 = 145

    // 创建左眼
    eye_t left_eye = create_eye(screen, left_eye_x, left_eye_y, eye_size);
    setup_eye_animations(&left_eye, eye_size);

    // 创建右眼
    eye_t right_eye = create_eye(screen, right_eye_x, right_eye_y, eye_size);
    setup_eye_animations(&right_eye, eye_size);
}

#else

void  LcdDisplay:: SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, fonts_.text_font, 0);
    lv_obj_set_style_text_color(screen, current_theme.text, 0);
    lv_obj_set_style_bg_color(screen, current_theme.background, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, current_theme.background, 0);
    lv_obj_set_style_border_color(container_, current_theme.border, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, fonts_.text_font->line_height);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, current_theme.background, 0);
    lv_obj_set_style_text_color(status_bar_, current_theme.text, 0);

    /* Content */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, 5, 0);
    lv_obj_set_style_bg_color(content_, current_theme.chat_background, 0);
    lv_obj_set_style_border_color(content_, current_theme.border, 0); // Border color for content

    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN); // 垂直布局（从上到下）
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY); // 子对象居中对齐，等距分布

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(emotion_label_, current_theme.text, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);

    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9); // 限制宽度为屏幕宽度的 90%
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP); // 设置为自动换行模式
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0); // 设置文本居中对齐
    lv_obj_set_style_text_color(chat_message_label_, current_theme.text, 0);

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_left(status_bar_, 2, 0);
    lv_obj_set_style_pad_right(status_bar_, 2, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(network_label_, current_theme.text, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, current_theme.text, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, current_theme.text, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, current_theme.text, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, current_theme.text, 0);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, fonts_.text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, current_theme.low_battery, 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    lv_obj_t* low_battery_label = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label, lv_color_white(), 0);
    lv_obj_center(low_battery_label);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

}
#endif /* !ENABLE_EYES_SIMULATION */

#endif

// void  LcdDisplay:: SetupBluetoothUI() {
//     // 全屏深色背景（节省功耗）
//     auto screen = lv_screen_active();
//     lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1A2B), 0);

//     /* 蓝牙图标（大号居中） */
//     bluetooth_icon_ = lv_label_create(screen);
//     lv_label_set_text(bluetooth_icon_, FONT_AWESOME_USER); // 默认断开状态图标
//     lv_obj_set_style_text_font(bluetooth_icon_, fonts_.icon_font, 0);
//     lv_obj_set_style_text_color(bluetooth_icon_, lv_palette_main(LV_PALETTE_GREY), 0);
//     lv_obj_center(bluetooth_icon_);

//     /* 状态文字（图标下方） */
//     status_label_ = lv_label_create(screen);
//     lv_label_set_text(status_label_, "蓝牙未连接");
//     lv_obj_set_style_text_font(status_label_, fonts_.text_font, 0);
//     lv_obj_set_style_text_color(status_label_, lv_palette_main(LV_PALETTE_GREY), 0);
//     lv_obj_align_to(status_label_, bluetooth_icon_, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);

//     /* 设备名称（小字底部） */
//     device_label_ = lv_label_create(screen);
//     lv_label_set_text(device_label_, "");
//     lv_obj_set_style_text_font(device_label_, fonts_.text_font, 0);
//     lv_obj_align(device_label_, LV_ALIGN_BOTTOM_MID, 0, -10);
// }
// // 状态更新函数（外部调用）
// void LcdDisplay::UpdateBluetoothStatus(bool is_connected, char* device_name) {
//     // 更新图标
//     lv_label_set_text(bluetooth_icon_,
//         is_connected ? FONT_AWESOME_BLUETOOTH : FONT_AWESOME_USER);

//     // 更新颜色
//     lv_obj_set_style_text_color(bluetooth_icon_,
//         is_connected ? lv_palette_main(LV_PALETTE_BLUE) : lv_palette_main(LV_PALETTE_GREY),
//         0);

//     // 更新状态文字
//     lv_label_set_text(status_label_,
//         is_connected ? "蓝牙已连接" : "蓝牙未连接");

//     // 更新设备名称（连接时显示）
//     lv_label_set_text(device_label_,
//         is_connected ? device_name : "");
// }

void LcdDisplay::SetEmotion(const char* emotion) {
    struct Emotion {
        const char* icon;
        const char* text;
    };

    static const std::vector<Emotion> emotions = {
        {"😶", "neutral"},
        {"🙂", "happy"},
        {"😆", "laughing"},
        {"😂", "funny"},
        {"😔", "sad"},
        {"😠", "angry"},
        {"😭", "crying"},
        {"😍", "loving"},
        {"😳", "embarrassed"},
        {"😯", "surprised"},
        {"😱", "shocked"},
        {"🤔", "thinking"},
        {"😉", "winking"},
        {"😎", "cool"},
        {"😌", "relaxed"},
        {"🤤", "delicious"},
        {"😘", "kissy"},
        {"😏", "confident"},
        {"😴", "sleepy"},
        {"😜", "silly"},
        {"🙄", "confused"}
    };

    // 查找匹配的表情
    std::string_view emotion_view(emotion);
    auto it = std::find_if(emotions.begin(), emotions.end(),
        [&emotion_view](const Emotion& e) { return e.text == emotion_view; });

    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }

    // 如果找到匹配的表情就显示对应图标，否则显示默认的neutral表情
    lv_obj_set_style_text_font(emotion_label_, fonts_.emoji_font, 0);
    if (it != emotions.end()) {
        lv_label_set_text(emotion_label_, it->icon);
    } else {
        lv_label_set_text(emotion_label_, "😶");
    }
}

void LcdDisplay::SetIcon(const char* icon) {
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_label_set_text(emotion_label_, icon);
}

void LcdDisplay::SetTheme(const std::string& theme_name) {
    DisplayLockGuard lock(this);

    if (theme_name == "dark" || theme_name == "DARK") {
        current_theme = DARK_THEME;
    } else if (theme_name == "light" || theme_name == "LIGHT") {
        current_theme = LIGHT_THEME;
    } else {
        // Invalid theme name, return false
        ESP_LOGE(TAG, "Invalid theme name: %s", theme_name.c_str());
        return;
    }

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Update the screen colors
    lv_obj_set_style_bg_color(screen, current_theme.background, 0);
    lv_obj_set_style_text_color(screen, current_theme.text, 0);

    // Update container colors
    if (container_ != nullptr) {
        lv_obj_set_style_bg_color(container_, current_theme.background, 0);
        lv_obj_set_style_border_color(container_, current_theme.border, 0);
    }

    // Update status bar colors
    if (status_bar_ != nullptr) {
        lv_obj_set_style_bg_color(status_bar_, current_theme.background, 0);
        lv_obj_set_style_text_color(status_bar_, current_theme.text, 0);

        // Update status bar elements
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_color(network_label_, current_theme.text, 0);
        }
        if (status_label_ != nullptr) {
            lv_obj_set_style_text_color(status_label_, current_theme.text, 0);
        }
        if (notification_label_ != nullptr) {
            lv_obj_set_style_text_color(notification_label_, current_theme.text, 0);
        }
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_color(mute_label_, current_theme.text, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_color(battery_label_, current_theme.text, 0);
        }
        if (emotion_label_ != nullptr) {
            lv_obj_set_style_text_color(emotion_label_, current_theme.text, 0);
        }
    }

    // Update content area colors
    if (content_ != nullptr) {
        lv_obj_set_style_bg_color(content_, current_theme.chat_background, 0);
        lv_obj_set_style_border_color(content_, current_theme.border, 0);

        // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
        // Iterate through all children of content (message containers or bubbles)
        uint32_t child_count = lv_obj_get_child_cnt(content_);
        for (uint32_t i = 0; i < child_count; i++) {
            lv_obj_t* obj = lv_obj_get_child(content_, i);
            if (obj == nullptr) continue;

            lv_obj_t* bubble = nullptr;

            // 检查这个对象是容器还是气泡
            // 如果是容器（用户或系统消息），则获取其子对象作为气泡
            // 如果是气泡（助手消息），则直接使用
            if (lv_obj_get_child_cnt(obj) > 0) {
                // 可能是容器，检查它是否为用户或系统消息容器
                // 用户和系统消息容器是透明的
                lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, 0);
                if (bg_opa == LV_OPA_TRANSP) {
                    // 这是用户或系统消息的容器
                    bubble = lv_obj_get_child(obj, 0);
                } else {
                    // 这可能是助手消息的气泡自身
                    bubble = obj;
                }
            } else {
                // 没有子元素，可能是其他UI元素，跳过
                continue;
            }

            if (bubble == nullptr) continue;

            // 使用保存的用户数据来识别气泡类型
            void* bubble_type_ptr = lv_obj_get_user_data(bubble);
            if (bubble_type_ptr != nullptr) {
                const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

                // 根据气泡类型应用正确的颜色
                if (strcmp(bubble_type, "user") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme.user_bubble, 0);
                } else if (strcmp(bubble_type, "assistant") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme.assistant_bubble, 0);
                } else if (strcmp(bubble_type, "system") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme.system_bubble, 0);
                }

                // Update border color
                lv_obj_set_style_border_color(bubble, current_theme.border, 0);

                // Update text color for the message
                if (lv_obj_get_child_cnt(bubble) > 0) {
                    lv_obj_t* text = lv_obj_get_child(bubble, 0);
                    if (text != nullptr) {
                        // 根据气泡类型设置文本颜色
                        if (strcmp(bubble_type, "system") == 0) {
                            lv_obj_set_style_text_color(text, current_theme.system_text, 0);
                        } else {
                            lv_obj_set_style_text_color(text, current_theme.text, 0);
                        }
                    }
                }
            } else {
                // 如果没有标记，回退到之前的逻辑（颜色比较）
                // ...保留原有的回退逻辑...
                lv_color_t bg_color = lv_obj_get_style_bg_color(bubble, 0);

                // 改进bubble类型检测逻辑，不仅使用颜色比较
                bool is_user_bubble = false;
                bool is_assistant_bubble = false;
                bool is_system_bubble = false;

                // 检查用户bubble
                if (lv_color_eq(bg_color, DARK_USER_BUBBLE_COLOR) ||
                    lv_color_eq(bg_color, LIGHT_USER_BUBBLE_COLOR) ||
                    lv_color_eq(bg_color, current_theme.user_bubble)) {
                    is_user_bubble = true;
                }
                // 检查系统bubble
                else if (lv_color_eq(bg_color, DARK_SYSTEM_BUBBLE_COLOR) ||
                         lv_color_eq(bg_color, LIGHT_SYSTEM_BUBBLE_COLOR) ||
                         lv_color_eq(bg_color, current_theme.system_bubble)) {
                    is_system_bubble = true;
                }
                // 剩余的都当作助手bubble处理
                else {
                    is_assistant_bubble = true;
                }

                // 根据bubble类型应用正确的颜色
                if (is_user_bubble) {
                    lv_obj_set_style_bg_color(bubble, current_theme.user_bubble, 0);
                } else if (is_assistant_bubble) {
                    lv_obj_set_style_bg_color(bubble, current_theme.assistant_bubble, 0);
                } else if (is_system_bubble) {
                    lv_obj_set_style_bg_color(bubble, current_theme.system_bubble, 0);
                }

                // Update border color
                lv_obj_set_style_border_color(bubble, current_theme.border, 0);

                // Update text color for the message
                if (lv_obj_get_child_cnt(bubble) > 0) {
                    lv_obj_t* text = lv_obj_get_child(bubble, 0);
                    if (text != nullptr) {
                        // 回退到颜色检测逻辑
                        if (lv_color_eq(bg_color, current_theme.system_bubble) ||
                            lv_color_eq(bg_color, DARK_SYSTEM_BUBBLE_COLOR) ||
                            lv_color_eq(bg_color, LIGHT_SYSTEM_BUBBLE_COLOR)) {
                            lv_obj_set_style_text_color(text, current_theme.system_text, 0);
                        } else {
                            lv_obj_set_style_text_color(text, current_theme.text, 0);
                        }
                    }
                }
            }
        }
#else
        // Simple UI mode - just update the main chat message
        if (chat_message_label_ != nullptr) {
            lv_obj_set_style_text_color(chat_message_label_, current_theme.text, 0);
        }

        if (emotion_label_ != nullptr) {
            lv_obj_set_style_text_color(emotion_label_, current_theme.text, 0);
        }
#endif
    }

    // Update low battery popup
    if (low_battery_popup_ != nullptr) {
        lv_obj_set_style_bg_color(low_battery_popup_, current_theme.low_battery, 0);
    }

    // No errors occurred. Save theme to settings
    Display::SetTheme(theme_name);
}

void LcdDisplay::ShowGif(const uint8_t* gif_data, size_t gif_size, int x, int y) {
    // Execute LVGL operations on LVGL task to avoid cross-thread mutex issues
    struct Ctx { LcdDisplay* self; const uint8_t* data; size_t size; int x; int y; SemaphoreHandle_t done; };
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        ESP_LOGE(TAG, "ShowGif: failed to create semaphore");
        return;
    }
    Ctx* ctx = (Ctx*)heap_caps_malloc(sizeof(Ctx), MALLOC_CAP_INTERNAL);
    if (!ctx) {
        ESP_LOGE(TAG, "ShowGif: failed to alloc ctx");
        vSemaphoreDelete(done);
        return;
    }
    ctx->self = this; ctx->data = gif_data; ctx->size = gif_size; ctx->x = x; ctx->y = y; ctx->done = done;
    auto async_cb = [](void* p){
        Ctx* c = (Ctx*)p;
        c->self->ShowGifImpl_(c->data, c->size, c->x, c->y);
        xSemaphoreGive(c->done);
        heap_caps_free(c);
    };
    lv_async_call(async_cb, ctx);
    (void)xSemaphoreTake(done, pdMS_TO_TICKS(5000));
    vSemaphoreDelete(done);
}

void LcdDisplay::ShowGifImpl_(const uint8_t* gif_data, size_t gif_size, int x, int y) {
    ESP_LOGI(TAG, "Attempting to show GIF at position (%d, %d), size: %lu bytes", x, y, (unsigned long)gif_size);

    if (!gif_data || gif_size == 0) {
        ESP_LOGE(TAG, "Invalid GIF data: data=%p, size=%lu", gif_data, (unsigned long)gif_size);
        return;
    }
    if (gif_size < 10 || memcmp(gif_data, "GIF", 3) != 0) {
        ESP_LOGE(TAG, "Invalid GIF header, size=%lu", (unsigned long)gif_size);
        return;
    }

    ESP_LOGI(TAG, "GIF header validation passed: %.6s", gif_data);
    ESP_LOGI(TAG, "SPIRAM before Show: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    std::unique_ptr<LvglGif> old_controller;
    lv_obj_t* active_obj = (active_gif_view_ == 0 ? gif_img_ : gif_img_b_);
    if (gif_controller_ && active_obj) {
        if (last_gif_data_ == gif_data && last_gif_size_ == gif_size) {
            lv_obj_clear_flag(active_obj, LV_OBJ_FLAG_HIDDEN);
            if (!gif_controller_->IsPlaying()) {
                gif_controller_->Start();
            }
            SetGifPos(x, y);
            lv_obj_move_foreground(active_obj);
            ESP_LOGI(TAG, "GIF reused without restart");
            return;
        }
        // Different content: stop old GIF asynchronously to avoid blocking LVGL task
        ESP_LOGI(TAG, "Stopping previous GIF (async)");
        old_controller = std::move(gif_controller_);
        // Trigger async cleanup - old_controller will be destroyed when it goes out of scope
        // but we add a small delay before starting new GIF to allow cleanup to progress
    }

    lv_image_dsc_t src{};
    src.header.magic = LV_IMAGE_HEADER_MAGIC;
    src.header.cf = LV_COLOR_FORMAT_UNKNOWN;
    src.data = gif_data;
    src.data_size = gif_size;

    auto new_controller = std::make_unique<LvglGif>(&src);
    if (!new_controller || !new_controller->IsLoaded()) {
        ESP_LOGE(TAG, "Failed to initialize GIF controller");
        return;
    }
    // Loop GIF indefinitely until user swipes (0 = infinite loops)
    new_controller->SetLoopCount(0);

    // Ensure two LVGL image views exist (for seamless switching)
    if (!gif_img_) {
        gif_img_ = lv_image_create(lv_screen_active());
        if (!gif_img_) { return; }
        ensure_gif_style();
        lv_obj_add_style(gif_img_, &s_gif_style, 0);
        lv_obj_add_flag(gif_img_, LV_OBJ_FLAG_HIDDEN);
    }
    if (!gif_img_b_) {
        gif_img_b_ = lv_image_create(lv_screen_active());
        if (!gif_img_b_) { return; }
        ensure_gif_style();
        lv_obj_add_style(gif_img_b_, &s_gif_style, 0);
        lv_obj_add_flag(gif_img_b_, LV_OBJ_FLAG_HIDDEN);
    }

    // CRITICAL: Stop old GIF BEFORE starting new one to prevent concurrent decoder tasks
    // Just set playing_ = false, the decoder task will exit on its own
    // The destructor will handle full cleanup asynchronously
    if (old_controller) {
        ESP_LOGI(TAG, "Stopping old GIF playback");
        old_controller->Stop();  // Sets playing_ = false, non-blocking
    }

    // Render on the inactive view, keep current visible until swap
    lv_obj_t* target = (active_gif_view_ == 0 ? gif_img_b_ : gif_img_);
    lv_image_set_src(target, new_controller->image_dsc());
    new_controller->SetFrameCallback([this, target]() {
        if (target) { lv_obj_invalidate(target); }
    });

    // Now safe to start new GIF (old one is stopped)
    new_controller->Start();

    SetGifPos(x, y);
    lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(target);

    if (active_obj) {
        lv_obj_add_flag(active_obj, LV_OBJ_FLAG_HIDDEN);
    }

    // Commit controller and swap active view index
    gif_controller_ = std::move(new_controller);
    last_gif_data_ = gif_data;
    last_gif_size_ = gif_size;
    active_gif_view_ ^= 1u;

    ESP_LOGI(TAG, "GIF started via LvglGif controller (official style)");

    // old_controller will be destroyed here (async cleanup in destructor)
}

void LcdDisplay::HideGif() {
    struct Ctx { LcdDisplay* self; SemaphoreHandle_t done; };
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) { ESP_LOGE(TAG, "HideGif: failed to create semaphore"); return; }
    Ctx* ctx = (Ctx*)heap_caps_malloc(sizeof(Ctx), MALLOC_CAP_INTERNAL);
    if (!ctx) { vSemaphoreDelete(done); ESP_LOGE(TAG, "HideGif: failed to alloc ctx"); return; }
    ctx->self = this; ctx->done = done;
    auto async_cb = [](void* p){
        Ctx* c = (Ctx*)p;
        c->self->HideGifImpl_();
        xSemaphoreGive(c->done);
        heap_caps_free(c);
    };
    lv_async_call(async_cb, ctx);
    (void)xSemaphoreTake(done, pdMS_TO_TICKS(2000));
    vSemaphoreDelete(done);
}

void LcdDisplay::HideGifImpl_() {
    // Pause + hide only; keep controller and lv_image to avoid re-alloc each cycle
    if (gif_controller_) {
        gif_controller_->Pause();
    }
    if (gif_img_) {
        lv_obj_add_flag(gif_img_, LV_OBJ_FLAG_HIDDEN);
    }
    if (gif_img_b_) {
        lv_obj_add_flag(gif_img_b_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "SPIRAM after Hide (paused): %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void LcdDisplay::DestroyGif() {
    DisplayLockGuard lock(this);
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    last_gif_data_ = nullptr;
    last_gif_size_ = 0;
    if (managed_gif_buffer_ != nullptr) {
        heap_caps_free(managed_gif_buffer_);
        managed_gif_buffer_ = nullptr;
        managed_gif_buffer_size_ = 0;
    }
    if (gif_img_ != nullptr) {
        // Detach source and hide, but keep the lv_image object to avoid re-allocating LVGL draw handlers
        lv_image_set_src(gif_img_, NULL);
        lv_obj_add_flag(gif_img_, LV_OBJ_FLAG_HIDDEN);
    }
    if (gif_img_b_ != nullptr) {
        lv_image_set_src(gif_img_b_, NULL);
        lv_obj_add_flag(gif_img_b_, LV_OBJ_FLAG_HIDDEN);
    }
    active_gif_view_ = 0;
}

void LcdDisplay::ShowGifWithManagedBuffer(uint8_t* gif_data, size_t gif_size, int x, int y) {
    DisplayLockGuard lock(this);

    ESP_LOGI(TAG, "Showing GIF with managed buffer: %zu bytes", gif_size);

    // Validate input parameters
    if (gif_data == nullptr || gif_size == 0) {
        ESP_LOGE(TAG, "Invalid managed GIF data: data=%p, size=%zu", gif_data, gif_size);
        if (gif_data != nullptr) {
            heap_caps_free(gif_data);
        }
        return;
    }

    // Validate GIF header
    if (gif_size < 6 || memcmp(gif_data, "GIF", 3) != 0) {
        ESP_LOGE(TAG, "Invalid managed GIF header, size=%zu", gif_size);
        heap_caps_free(gif_data);
        return;
    }

    // Seamless swap: don't blank image, prepare new controller first
    std::unique_ptr<LvglGif> old_ctrl;
    if (gif_controller_) {
        old_ctrl = std::move(gif_controller_);
    }

    // Keep a local reference so ShowGif's internal HideGif can't free it
    uint8_t* temp_buffer = gif_data;

    // Build controller directly with managed buffer
    lv_image_dsc_t src{};
    src.header.magic = LV_IMAGE_HEADER_MAGIC;
    src.header.cf = LV_COLOR_FORMAT_UNKNOWN;
    src.data = temp_buffer;
    src.data_size = gif_size;
    gif_controller_ = std::make_unique<LvglGif>(&src);
    if (!gif_controller_ || !gif_controller_->IsLoaded()) {
        ESP_LOGW(TAG, "GIF decode failed; freeing download buffer");
        heap_caps_free(temp_buffer);
        gif_controller_.reset();
        return;
    }
    if (!gif_img_) {
        gif_img_ = lv_image_create(lv_screen_active());
        if (!gif_img_) { gif_controller_.reset(); heap_caps_free(temp_buffer); return; }
        ensure_gif_style();
        lv_obj_add_style(gif_img_, &s_gif_style, 0);
    }
    lv_image_set_src(gif_img_, gif_controller_->image_dsc());
    gif_controller_->SetFrameCallback([this]() {
        if (gif_img_) { lv_obj_invalidate(gif_img_); }
    });
    gif_controller_->Start();
    SetGifPos(x, y);
    lv_obj_clear_flag(gif_img_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(gif_img_);
    last_gif_data_ = temp_buffer;
    last_gif_size_ = gif_size;
    // Track owned managed buffer for later free
    uint8_t* old_managed = managed_gif_buffer_;
    managed_gif_buffer_ = temp_buffer;
    managed_gif_buffer_size_ = gif_size;
    // Release previous controller and its managed buffer (if any) after swap
    if (old_ctrl) {
        old_ctrl->Stop();
        old_ctrl.reset();
    }
    if (old_managed) {
        heap_caps_free(old_managed);
    }
    ESP_LOGI(TAG, "GIF with managed buffer displayed successfully");
}

// HTTP下载数据结构
struct HttpDownloadData {
    uint8_t* buffer;
    size_t buffer_size;
    size_t data_len;
    size_t content_length;
    size_t max_size;
    bool success;
    // Throttling and yielding helpers
    TickType_t last_log_tick;
    int last_percent_logged;
    size_t last_yield_bytes;
};

// HTTP事件处理回调
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    HttpDownloadData* download_data = (HttpDownloadData*)evt->user_data;

    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            download_data->success = false;
            break;

        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;

        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;

        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                size_t content_length = atoi(evt->header_value);
                ESP_LOGI(TAG, "Content-Length: %zu bytes", content_length);

                download_data->content_length = content_length;
                // 检查文件大小是否合理 (最大10MB)
                if (content_length > 10 * 1024 * 1024) {
                    ESP_LOGE(TAG, "GIF file too large: %zu bytes (max 10MB)", content_length);
                    download_data->success = false;
                    return ESP_FAIL;
                }

                // 重新分配缓冲区以适应实际大小
                if (content_length > download_data->buffer_size) {
                    uint8_t* new_buffer = (uint8_t*)heap_caps_realloc(download_data->buffer,
                                                                     content_length,
                                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (new_buffer == nullptr) {
                        ESP_LOGE(TAG, "Failed to reallocate buffer for %zu bytes", content_length);
                        download_data->success = false;
                        return ESP_FAIL;
                    }
                    download_data->buffer = new_buffer;
                    download_data->buffer_size = content_length;
                    download_data->max_size = content_length;
                }
            }
            break;

        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // 检查缓冲区空间
                if (download_data->data_len + evt->data_len > download_data->max_size) {
                    ESP_LOGE(TAG, "Download data exceeds buffer size");
                    download_data->success = false;
                    return ESP_FAIL;
                }

                // 复制数据到缓冲区
                memcpy(download_data->buffer + download_data->data_len, evt->data, evt->data_len);
                download_data->data_len += evt->data_len;

                // 仅每提升>=20%打印一次进度，减少日志影响
                if (download_data->content_length > 0) {
                    int progress = (download_data->data_len * 100) / download_data->content_length;
                    if (download_data->last_percent_logged < 0 ||
                        progress >= download_data->last_percent_logged + 20) {
                        ESP_LOGI(TAG, "Download progress: %d%% (%zu/%zu bytes)",
                                 progress, download_data->data_len, download_data->content_length);
                        download_data->last_percent_logged = progress;
                    }
                }

                // 周期性让出CPU，避免喂狗失败
                if (download_data->data_len - download_data->last_yield_bytes >= (64 * 1024)) {
                    vTaskDelay(1);
                    download_data->last_yield_bytes = download_data->data_len;
                }
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH, total downloaded: %zu bytes", download_data->data_len);
            download_data->success = true;
            break;

        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;

        default:
            break;
    }
    return ESP_OK;
}

void LcdDisplay::ShowGifFromUrl(const char* url, int x, int y) {
    if (url == nullptr || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid URL provided");
        return;
    }

    ESP_LOGI(TAG, "Starting GIF download from URL: %s", url);

    // 检查可用内存
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Available PSRAM: %zu bytes", free_heap);

    if (free_heap < 1024 * 1024) { // 至少需要1MB空闲内存
        ESP_LOGE(TAG, "Insufficient memory for GIF download: %zu bytes available", free_heap);
        return;
    }

    // 初始化下载数据结构
    HttpDownloadData download_data = {0};
    download_data.buffer_size = 512 * 1024; // 初始512KB缓冲区
    download_data.max_size = 10 * 1024 * 1024; // 最大10MB
    download_data.data_len = 0;
    download_data.content_length = 0;
    download_data.success = false;

    // 分配初始缓冲区 (优先使用PSRAM)
    download_data.buffer = (uint8_t*)heap_caps_malloc(download_data.buffer_size,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (download_data.buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate download buffer");
        return;
    }

    // 配置HTTP客户端
    esp_http_client_config_t config = {};
    config.url = url;
    // 初始化下载进度节流状态
    download_data.last_log_tick = xTaskGetTickCount();
    download_data.last_percent_logged = -1;
    download_data.last_yield_bytes = 0;

    config.event_handler = http_event_handler;
    config.user_data = &download_data;
    config.timeout_ms = 30000; // 30秒超时
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;

    // 如果是HTTPS，启用证书验证
    if (strncmp(url, "https://", 8) == 0) {
        config.use_global_ca_store = true;
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        heap_caps_free(download_data.buffer);
        return;
    }

    // 执行HTTP GET请求
    ESP_LOGI(TAG, "Starting HTTP GET request...");
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);

        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);

        if (status_code == 200 && download_data.success && download_data.data_len > 0) {
            ESP_LOGI(TAG, "GIF download successful: %zu bytes", download_data.data_len);

            // 验证GIF文件头
            if (download_data.data_len >= 6 &&
                (memcmp(download_data.buffer, "GIF87a", 6) == 0 ||
                 memcmp(download_data.buffer, "GIF89a", 6) == 0)) {

                ESP_LOGI(TAG, "Valid GIF file detected, displaying...");

                // 使用管理缓冲区的方法显示GIF
                // 这会转移缓冲区的所有权给显示系统
                ShowGifWithManagedBuffer(download_data.buffer, download_data.data_len, x, y);

                // 重要：缓冲区所有权已转移，不要在这里释放
                download_data.buffer = nullptr;

            } else {
                ESP_LOGE(TAG, "Downloaded file is not a valid GIF");
            }
        } else {
            ESP_LOGE(TAG, "HTTP request failed: status=%d, success=%d, data_len=%zu",
                    status_code, download_data.success, download_data.data_len);
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    // 清理资源
    esp_http_client_cleanup(client);

    // 释放下载缓冲区（如果所有权没有被转移）
    if (download_data.buffer != nullptr) {
        heap_caps_free(download_data.buffer);
        ESP_LOGI(TAG, "Download buffer freed (not used for GIF)");
    }

    ESP_LOGI(TAG, "GIF download and display process completed");
}

void LcdDisplay::ShowGifFromFlash(const char* filename, int x, int y) {
    if (filename == nullptr || strlen(filename) == 0) {
        ESP_LOGE(TAG, "Invalid filename provided");
        return;
    }

    ESP_LOGI(TAG, "Loading GIF from Flash: %s", filename);

    uint8_t* gif_data = nullptr;
    size_t gif_size = 0;

    // Read GIF from Flash storage
    esp_err_t ret = gif_storage_read(filename, &gif_data, &gif_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read GIF from Flash: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Successfully loaded GIF from Flash: %s (%zu bytes)", filename, gif_size);

    // Display the GIF using managed buffer (transfers ownership)
    ShowGifWithManagedBuffer(gif_data, gif_size, x, y);

    // Note: gif_data ownership is transferred to ShowGifWithManagedBuffer
    // Do not free it here
}

void LcdDisplay::ShowCenterMessage(const std::string &message, int duration_ms) {
    ShowCenterMessage(message.c_str(), duration_ms);
}

void LcdDisplay::ShowCenterMessage(const char* message, int duration_ms) {
    DisplayLockGuard guard(this);
    if (!guard.locked()) {
        return;
    }

    // 创建中央消息定时器（如果不存在）
    if (center_message_timer_ == nullptr) {
        esp_timer_create_args_t center_message_timer_args = {
            .callback = [](void *arg) {
                LcdDisplay *display = static_cast<LcdDisplay*>(arg);
                DisplayLockGuard lock(display);
                if (display->center_message_popup_ != nullptr) {
                    lv_obj_add_flag(display->center_message_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "center_message_timer",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&center_message_timer_args, &center_message_timer_));
    }

    // 创建中央消息弹窗（如果不存在）
    if (center_message_popup_ == nullptr) {
        lv_obj_t *screen = lv_scr_act();

        // 创建半透明背景
        center_message_popup_ = lv_obj_create(screen);
        lv_obj_set_size(center_message_popup_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(center_message_popup_, 0, 0);
        lv_obj_set_style_bg_color(center_message_popup_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(center_message_popup_, LV_OPA_70, 0);
        lv_obj_set_style_border_width(center_message_popup_, 0, 0);
        lv_obj_set_style_pad_all(center_message_popup_, 20, 0);

        // 创建消息标签
        center_message_label_ = lv_label_create(center_message_popup_);
        lv_obj_set_width(center_message_label_, LV_HOR_RES - 40);
        lv_obj_center(center_message_label_);
        lv_obj_set_style_text_color(center_message_label_, lv_color_white(), 0);
        lv_obj_set_style_text_align(center_message_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(center_message_label_, LV_LABEL_LONG_WRAP);

        // 初始状态为隐藏
        lv_obj_add_flag(center_message_popup_, LV_OBJ_FLAG_HIDDEN);
    }

    // 设置消息文本
    lv_label_set_text(center_message_label_, message);

    // 显示弹窗，确保在最前面
    lv_obj_clear_flag(center_message_popup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(center_message_popup_);

    // 启动定时器自动隐藏
    esp_timer_stop(center_message_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(center_message_timer_, duration_ms * 1000));
}
