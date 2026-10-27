#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    // --- Function-key driven UI state ---------------------------------------
    // The board owns the three ADC keys and forwards them here, so this class
    // stays the only place that touches LVGL for key-driven UI.
    lv_obj_t* settings_ = nullptr;             // overlay, built on first use
    lv_obj_t* settings_rows_[4] = {};          // row frames: brightness/volume/off/reset
    lv_obj_t* settings_row_labels_[4] = {};    // text inside those frames
    lv_obj_t* settings_hint_ = nullptr;
    int settings_index_ = 0;                   // highlighted row
    bool settings_adjusting_ = false;          // adjusting the row's value
    bool settings_open_ = false;
    int brightness_pct_ = 0;                   // mirror of the NVS brightness
    int volume_pct_ = 0;                       // mirror of the codec volume
    bool chat_reviewing_ = false;              // transcript scrolled off the end
    bool screen_on_ = true;
    int screen_off_seconds_ = 0;                   // seconds since the screen went off (toward deep sleep)

    // --- Auto screen-off and the always-on clock ----------------------------
    lv_obj_t* aod_ = nullptr;               // dim clock overlay, built on first use
    lv_obj_t* aod_time_ = nullptr;
    lv_obj_t* aod_date_ = nullptr;
    lv_obj_t* aod_battery_ = nullptr;       // mirrors the top bar's percentage
    lv_timer_t* idle_timer_ = nullptr;      // ticks once a second
    int idle_seconds_ = 0;                  // seconds since the last user activity
    int auto_off_index_ = 2;                // index into the auto-off delay table (default 3 min)

    void RefreshSettingsRows();
    void RefreshSettingsSelection();
    bool SettingsResetArmed() const;
    void RefreshAod();                      // builds the clock on first use
    void ResetIdleTimer() { idle_seconds_ = 0; }

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    virtual void SetHideSubtitle(bool hide) override;

    // This UI shows the battery as a percentage instead of an icon glyph.
    virtual std::string GetBatteryIndicatorText(int battery_level, bool charging) override;

    // --- Function-key driven UI (mapped by the board's buttons) -------------
    // OK long press opens/closes the settings overlay; OK click keeps its chat
    // duty unless the overlay is open; UP/DOWN page the transcript, or move and
    // adjust inside the overlay.
    void OpenSettings();
    void CloseSettings();
    bool IsSettingsOpen() const { return settings_open_; }
    void SettingsMove(int delta);
    void SettingsConfirm();
    void ScrollChat(int direction);  // -1: one page older, +1: one page newer
    void SetScreenOn(bool on);
    bool IsScreenOn() const { return screen_on_; }
    // Any key press counts as user activity and restarts the auto-off countdown.
    void NotifyUserActivity();
    // Keep the base's std::string overload visible alongside this override.
    using LvglDisplay::ShowNotification;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
