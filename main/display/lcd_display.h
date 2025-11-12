#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>
#include <memory>
#include "lvgl_display/gif/lvgl_gif.h"

#include <atomic>

class LcdDisplay : public Display {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* center_message_popup_ = nullptr;
    lv_obj_t* center_message_label_ = nullptr;

    // //加 蓝牙模式
    // lv_obj_t* bluetooth_icon_  = nullptr;
    // lv_obj_t* device_label_    = nullptr;
    // lv_obj_t* volume_slider_   = nullptr;
    // lv_obj_t* status_label_   = nullptr;

    DisplayFonts fonts_;
    esp_timer_handle_t center_message_timer_ = nullptr;

    // GIF controller (official-style): decode + frame timer
    std::unique_ptr<LvglGif> gif_controller_;
    // Remember last GIF data to avoid redundant restart
    const void* last_gif_data_ = nullptr;
    size_t last_gif_size_ = 0;
    // Managed download buffer (owned) for ShowGifFromUrl path
    uint8_t* managed_gif_buffer_ = nullptr;
    size_t managed_gif_buffer_size_ = 0;

    // Second LVGL image for seamless GIF switching
    lv_obj_t* gif_img_b_ = nullptr;
    // Which image view currently active: 0 -> gif_img_, 1 -> gif_img_b_
    uint8_t active_gif_view_ = 0;
    bool gif_power_hold_acquired_ = false;

    void SetupUI();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // 添加protected构造函数
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, DisplayFonts fonts)
        : panel_io_(panel_io), panel_(panel), fonts_(fonts) {}

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetIcon(const char* icon) override;
    virtual void ShowCenterMessage(const char* message, int duration_ms = 5000);
    virtual void ShowCenterMessage(const std::string &message, int duration_ms = 5000);

    // void SetupBluetoothUI();
    // void UpdateBluetoothStatus(bool is_connected, char* device_name);


#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    virtual void SetChatMessage(const char* role, const char* content) override;
#endif

    // Add theme switching function
    virtual void SetTheme(const std::string& theme_name) override;

    // GIF display methods
    virtual void ShowGif(const uint8_t* gif_data, size_t gif_size, int x = 0, int y = 0) override;
    void ShowGifFromUrl(const char* url, int x = 0, int y = 0);
    void ShowGifFromFlash(const char* filename, int x = 0, int y = 0);
    virtual void HideGif() override;
    virtual bool IsGifPlaying() const override {
        if (gif_controller_) {
            return gif_controller_->IsPlaying();
        }
        return false;
    }
    // Explicitly destroy GIF object and free any managed buffers
    void DestroyGif();

private:
    // Internal method for showing GIF with managed buffer
    void ShowGifWithManagedBuffer(uint8_t* gif_data, size_t gif_size, int x = 0, int y = 0);
    // Helper: center or position GIF based on x,y (0,0 means center)
    void SetGifPos(int x, int y);

    // Run on LVGL task context implementations
    void ShowGifImpl_(const uint8_t* gif_data, size_t gif_size, int x, int y);
    void HideGifImpl_();

    void AcquireGifPowerHold();
    void ReleaseGifPowerHold();
};

// RGB LCD显示器
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,
                  DisplayFonts fonts);
};

// MIPI LCD显示器
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy,
                   DisplayFonts fonts);
};

// // SPI LCD显示器
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,
                  DisplayFonts fonts);
};

// QSPI LCD显示器
class QspiLcdDisplay : public LcdDisplay {
public:
    QspiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy,
                   DisplayFonts fonts);
};

// MCU8080 LCD显示器
class Mcu8080LcdDisplay : public LcdDisplay {
public:
    Mcu8080LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                      int width, int height, int offset_x, int offset_y,
                      bool mirror_x, bool mirror_y, bool swap_xy,
                      DisplayFonts fonts);
};
#endif // LCD_DISPLAY_H
