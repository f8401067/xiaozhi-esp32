#include "wifi_board.h"
#include "display/lcd_display.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"
#include "cw2017_battery_monitor.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_idf_version.h>
#include <inttypes.h>
#include <soc/soc_caps.h>
#include <esp_lcd_panel_vendor.h>
#include <button_adc.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "AiPassport"

// Physical keys share one ADC pin through a resistor ladder (see config.h).
enum {
    kAdcButtonUp = 0,
    kAdcButtonDown,
    kAdcButtonOk,
    kAdcButtonNum,
};

class AiPassportBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button* adc_button_[kAdcButtonNum];
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    esp_lcd_panel_io_handle_t display_io_ = nullptr;
    esp_lcd_panel_handle_t display_panel_ = nullptr;
    LcdDisplay* display_;
    Cw2017BatteryMonitor* battery_;

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // CW2017 fuel gauge is optional; a missing chip just disables battery UI.
        battery_ = new Cw2017BatteryMonitor(codec_i2c_bus_, BATTERY_CW2017_ADDR);
        battery_->Initialize();
    }

    // Returns true when the key press was only consumed by waking the panel (or
    // there is no display yet), so the caller must not also run the key's own
    // action. The product specification documents "press a function button to
    // wake" after the screen goes off.
    bool KeyHandledByWake() {
        if (display_ == nullptr) {
            return true;
        }
        // Any key press is user activity: restart the auto screen-off countdown.
        display_->NotifyUserActivity();
        if (display_->IsScreenOn()) {
            return false;
        }
        display_->SetScreenOn(true);
        return true;
    }

    // UP/DOWN page the transcript, and move/adjust inside the settings overlay.
    void OnKeyUp() {
        if (KeyHandledByWake()) {
            return;
        }
        if (display_->IsSettingsOpen()) {
            display_->SettingsMove(-1);
        } else {
            display_->ScrollChat(-1);
        }
    }

    void OnKeyDown() {
        if (KeyHandledByWake()) {
            return;
        }
        if (display_->IsSettingsOpen()) {
            display_->SettingsMove(1);
        } else {
            display_->ScrollChat(1);
        }
    }

    // OK click still starts/stops a conversation (there is no wake word on this
    // build, so it is the only way in) unless the settings overlay is open.
    void OnKeyOk() {
        if (KeyHandledByWake()) {
            return;
        }
        if (display_->IsSettingsOpen()) {
            display_->SettingsConfirm();
        } else {
            ToggleChat();
        }
    }

    // OK held for 2 s opens the settings overlay, and closes it again.
    void OnKeyOkLongPress() {
        if (KeyHandledByWake()) {
            return;
        }
        if (display_->IsSettingsOpen()) {
            display_->CloseSettings();
        } else {
            display_->OpenSettings();
        }
    }

    // OK double-click turns the screen off immediately. If it is already off,
    // the press only wakes it (KeyHandledByWake). After that the normal idle
    // timer keeps counting and drops to deep sleep 10 minutes later.
    void OnKeyOkDoubleClick() {
        if (KeyHandledByWake()) {
            return;
        }
        if (display_ != nullptr) {
            display_->SetScreenOn(false);
        }
    }

    void ToggleChat() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    }

    void InitializeButtons() {
        for (int i = 0; i < kAdcButtonNum; i++) {
            adc_button_[i] = nullptr;
        }

        // One ADC1 unit shared by all three ladder keys. AdcButton reuses the
        // handle when adc_config.adc_handle is non-null, so the same physical
        // pin can decode several keys without "adc1 is already in use".
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle_));

        button_adc_config_t adc_cfg = {};
        adc_cfg.adc_handle = &adc_handle_;
        adc_cfg.unit_id = ADC_UNIT_1;
        adc_cfg.adc_channel = ADC_CHANNEL_0;  // GPIO0

        adc_cfg.button_index = kAdcButtonUp;      // UP:   ~0 mV
        adc_cfg.min = BSP_ADC_BUTTON_UP_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_UP_MAX;
        adc_button_[kAdcButtonUp] = new AdcButton(adc_cfg);

        adc_cfg.button_index = kAdcButtonDown;    // DOWN: ~300 mV
        adc_cfg.min = BSP_ADC_BUTTON_DOWN_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_DOWN_MAX;
        adc_button_[kAdcButtonDown] = new AdcButton(adc_cfg);

        adc_cfg.button_index = kAdcButtonOk;      // OK:   ~595 mV
        adc_cfg.min = BSP_ADC_BUTTON_OK_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_OK_MAX;
        adc_button_[kAdcButtonOk] = new AdcButton(adc_cfg);

        // Button callbacks run on the button task; schedule all UI/audio
        // work onto the main task so LVGL and codec access stay on one thread.
        //
        // Key map: UP/DOWN page the transcript (and move/adjust in settings),
        // OK click starts or stops a conversation, OK held for 2 s opens the
        // settings overlay. The former UP/DOWN volume shortcuts are gone so no
        // gesture competes with the settings entry; volume now lives in there.
        auto up = adc_button_[kAdcButtonUp];
        up->OnClick([this]() {
            Application::GetInstance().Schedule([this]() { OnKeyUp(); });
        });

        auto down = adc_button_[kAdcButtonDown];
        down->OnClick([this]() {
            Application::GetInstance().Schedule([this]() { OnKeyDown(); });
        });

        auto ok = adc_button_[kAdcButtonOk];
        ok->OnClick([this]() {
            Application::GetInstance().Schedule([this]() { OnKeyOk(); });
        });
        ok->OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() { OnKeyOkLongPress(); });
        });
        ok->OnDoubleClick([this]() {
            Application::GetInstance().Schedule([this]() { OnKeyOkDoubleClick(); });
        });
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;  // -1 -> software reset
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);

        // Panel-specific power/porch/gamma sequence for the ST7789P3 module
        // used on the Passport (copied from the original badge firmware).
        static const struct {
            uint8_t command;
            uint8_t data[16];
            uint8_t data_length;
            uint16_t delay_ms;
        } kSt7789P3InitCommands[] = {
            {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},
            {0xB7, {0x35}, 1, 0},
            {0xBB, {0x21}, 1, 0},
            {0xC0, {0x2C}, 1, 0},
            {0xC2, {0x01}, 1, 0},
            {0xC3, {0x0B}, 1, 0},
            {0xC4, {0x20}, 1, 0},
            {0xC6, {0x0F}, 1, 0},
            {0xD0, {0xA7, 0xA1}, 2, 0},
            {0xD0, {0xA4, 0xA1}, 2, 0},
            {0xD6, {0xA1}, 1, 0},
            {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
                    0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},
            {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
                    0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},
        };
        for (const auto& cmd : kSt7789P3InitCommands) {
            esp_lcd_panel_io_tx_param(panel_io, cmd.command, cmd.data, cmd.data_length);
            if (cmd.delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(cmd.delay_ms));
            }
        }

        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_set_gap(panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        // Kept for EnterDeepSleep(): the panel needs DISPOFF + SLPIN before the
        // rails are left to idle, otherwise its charge pump keeps drawing mA.
        display_io_ = panel_io;
        display_panel_ = panel;
    }

public:
    AiPassportBoard() : display_(nullptr), battery_(nullptr) {
        // A deep-sleep wakeup is a full chip reset, so this boot-time report is
        // the only way to tell "the key woke it" from "it never woke up".
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
        if (wakeup_causes != 0) {
            ESP_LOGI(TAG, "Boot after sleep, wakeup causes: 0x%" PRIx32, wakeup_causes);
        }
#else
        esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
        if (wakeup_cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
            ESP_LOGI(TAG, "Boot after sleep, wakeup cause: %d", static_cast<int>(wakeup_cause));
        }
#endif
        InitializeCodecI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    void EnterDeepSleep() override;

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!battery_ || !battery_->IsPresent()) {
            return false;
        }
        int soc = battery_->GetBatteryLevel();
        if (soc < 0) {
            return false;
        }
        level = soc;
        // CW2017 reports no charge state and the Passport has no charge-detect
        // GPIO, so report a plain (discharging) reading.
        charging = false;
        discharging = true;
        return true;
    }
};

void AiPassportBoard::EnterDeepSleep() {
    // Only drop to deep sleep when the device is genuinely idle: no open audio
    // channel and the state machine is Idle. Otherwise stay awake and let the
    // screen-off timer retry later.
    if (!Application::GetInstance().CanEnterSleepMode()) {
        return;
    }

    auto* backlight = GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(0);
    }

    // Put the panel itself to sleep. The backlight is already off, but the
    // ST7789 controller keeps its charge pump and gate drivers running as long
    // as it is awake - a few mA for the whole sleep, orders of magnitude more
    // than the C3 itself (uA). SLPIN is what actually drops that current.
    if (display_io_ != nullptr && display_panel_ != nullptr) {
        esp_lcd_panel_disp_on_off(display_panel_, false);          // DISPOFF
        esp_lcd_panel_io_tx_param(display_io_, 0x10, nullptr, 0);  // SLPIN
        vTaskDelay(pdMS_TO_TICKS(10));                             // SLPIN needs 5 ms
    }

    // Gate the codec as well: a sleeping panel saves nothing if the ES8311 and
    // the speaker amp stay powered. On this board the amp enable is not wired
    // to the MCU (AUDIO_CODEC_PA_PIN is NC), so closing the codec device is the
    // only knob available.
    if (auto* codec = GetAudioCodec()) {
        codec->EnableOutput(false);
        codec->EnableInput(false);
    }

    // WiFi/BT need no explicit teardown: the deep-sleep power sequence cuts the
    // radio domain. Calling esp_wifi_stop() here would only add a disconnect
    // round-trip before the chip finally sleeps.

    // The three ADC ladder keys (UP/DOWN/OK) share GPIO0 through a resistor
    // ladder, so any press pulls GPIO0 low: UP ~0 mV, DOWN ~300 mV, OK
    // ~595 mV, all below the 0.25 x VDD input-low threshold.
    //
    // gpio_wakeup_enable() + esp_sleep_enable_gpio_wakeup() only arm the *light*
    // sleep GPIO path; esp_deep_sleep_start() ignores them, which left the chip
    // with no wakeup source at all (a key press did nothing and only a reset or
    // power cycle brought it back). Deep sleep needs the dedicated API.
    constexpr gpio_num_t kKeyGpio = GPIO_NUM_0;
    gpio_config_t key_config = {};
    key_config.pin_bit_mask = 1ULL << kKeyGpio;
    key_config.mode = GPIO_MODE_INPUT;
    key_config.pull_up_en = GPIO_PULLUP_DISABLE;    // 10 kOhm pull-up is on the board
    key_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    key_config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&key_config));

#if SOC_GPIO_SUPPORT_HP_PERIPH_PD_SLEEP_WAKEUP
    // ESP32-C3 has no RTC-IO/ext1 wakeup path (SOC_PM_SUPPORT_EXT1_WAKEUP is
    // unset); the deep-sleep GPIO wakeup is the only way in.
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        1ULL << kKeyGpio, ESP_GPIO_WAKEUP_GPIO_LOW));
#elif defined(SOC_PM_SUPPORT_EXT1_WAKEUP) && SOC_PM_SUPPORT_EXT1_WAKEUP
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << kKeyGpio, ESP_EXT1_WAKEUP_ANY_LOW));
#endif
    ESP_LOGI(TAG, "Entering deep sleep, wake on key (GPIO0 low)");
    esp_deep_sleep_start();
}

DECLARE_BOARD(AiPassportBoard);
