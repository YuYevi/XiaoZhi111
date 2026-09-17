#include "wifi_board.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/rtc_io.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>
#include <atomic>
#include <string_view>

#include "adc_battery_monitor.h"
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "board_animations.h"
#include "board_sounds.h"
#include "button.h"
#include "codecs/es8389_audio_codec.h"
#include "config.h"
#include "dual_eye_display.h"
#include "esp_lcd_gc9d01.h"

#define TAG "AiDualEyedDoll"

static const gc9d01_lcd_init_cmd_t kGc9d01InitCommands[] = {
    {0xFE, nullptr, 0, 0},
    {0xEF, nullptr, 0, 0},
    {0x80, (const uint8_t[]){0xFF}, 1, 0},
    {0x81, (const uint8_t[]){0xFF}, 1, 0},
    {0x82, (const uint8_t[]){0xFF}, 1, 0},
    {0x83, (const uint8_t[]){0xFF}, 1, 0},
    {0x84, (const uint8_t[]){0xFF}, 1, 0},
    {0x85, (const uint8_t[]){0xFF}, 1, 0},
    {0x86, (const uint8_t[]){0xFF}, 1, 0},
    {0x87, (const uint8_t[]){0xFF}, 1, 0},
    {0x88, (const uint8_t[]){0xFF}, 1, 0},
    {0x89, (const uint8_t[]){0xFF}, 1, 0},
    {0x8A, (const uint8_t[]){0xFF}, 1, 0},
    {0x8B, (const uint8_t[]){0xFF}, 1, 0},
    {0x8C, (const uint8_t[]){0xFF}, 1, 0},
    {0x8D, (const uint8_t[]){0xFF}, 1, 0},
    {0x8E, (const uint8_t[]){0xFF}, 1, 0},
    {0x8F, (const uint8_t[]){0xFF}, 1, 0},
    {0x3A, (const uint8_t[]){0x05}, 1, 0},
    {0xEC, (const uint8_t[]){0x01}, 1, 0},
    {0x74, (const uint8_t[]){0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00}, 7, 0},
    {0x98, (const uint8_t[]){0x3E}, 1, 0},
    {0x99, (const uint8_t[]){0x3E}, 1, 0},
    {0xB5, (const uint8_t[]){0x0D, 0x0D}, 2, 0},
    {0x60, (const uint8_t[]){0x38, 0x0F, 0x79, 0x67}, 4, 0},
    {0x61, (const uint8_t[]){0x38, 0x11, 0x79, 0x67}, 4, 0},
    {0x64, (const uint8_t[]){0x38, 0x17, 0x71, 0x5F, 0x79, 0x67}, 6, 0},
    {0x65, (const uint8_t[]){0x38, 0x13, 0x71, 0x5B, 0x79, 0x67}, 6, 0},
    {0x6A, (const uint8_t[]){0x00, 0x00}, 2, 0},
    {0x6C, (const uint8_t[]){0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50}, 7, 0},
    {0x6E, (const uint8_t[]){0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0F, 0x0F, 0x0D, 0x0D, 0x0B,
                             0x0B, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x0A, 0x0C, 0x0C,
                             0x0E, 0x0E, 0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04},
     32, 0},
    {0xBF, (const uint8_t[]){0x01}, 1, 0},
    {0xF9, (const uint8_t[]){0x40}, 1, 0},
    {0x9B, (const uint8_t[]){0x3B}, 1, 0},
    {0x93, (const uint8_t[]){0x33, 0x7F, 0x00}, 3, 0},
    {0x7E, (const uint8_t[]){0x30}, 1, 0},
    {0x70, (const uint8_t[]){0x0D, 0x02, 0x08, 0x0D, 0x02, 0x08}, 6, 0},
    {0x71, (const uint8_t[]){0x0D, 0x02, 0x08}, 3, 0},
    {0x91, (const uint8_t[]){0x0E, 0x09}, 2, 0},
    {0xC3, (const uint8_t[]){0x18}, 1, 0},
    {0xC4, (const uint8_t[]){0x18}, 1, 0},
    {0xC9, (const uint8_t[]){0x3C}, 1, 0},
    {0xF0, (const uint8_t[]){0x13, 0x15, 0x04, 0x05, 0x01, 0x38}, 6, 0},
    {0xF2, (const uint8_t[]){0x13, 0x15, 0x04, 0x05, 0x01, 0x34}, 6, 0},
    {0xF1, (const uint8_t[]){0x4B, 0xB8, 0x7B, 0x34, 0x35, 0xEF}, 6, 0},
    {0xF3, (const uint8_t[]){0x47, 0xB4, 0x72, 0x34, 0x35, 0xDA}, 6, 0},
    {0x36, (const uint8_t[]){0x00}, 1, 0},
    {0x11, nullptr, 0, 200},
    {0x29, nullptr, 0, 0},
    {0x2C, nullptr, 0, 0},
};

class AiDualEyedDollBoard : public WifiBoard {
private:
    // One scene = one frame-delta animation in the read-only assets partition
    // plus the embedded Ogg clip that plays when the scene is shown.
    struct EyeScene {
        const char* animation;
        const std::string_view* sound;
    };

    // KEY 1 walks this list in order and wraps around. The *_blnk assets come
    // from Pictures/动图/*_blink.gif; the voice clips are the baobao-flavoured
    // lines (lazy, deadpan young-female TTS voice). Asset names must stay at 15
    // characters or fewer: CONFIG_MMAP_FILE_NAME_LENGTH is 16, so longer names
    // are truncated in the assets table and the lookup by exact name fails.
    static constexpr EyeScene kKey1Scenes[] = {
        {"shckblnk.anim.z", &BoardSounds::OGG_BAOER_SHOCKED},
        {"cuteblnk.anim.z", &BoardSounds::OGG_BAOER_CUTE},
        {"hapyblnk.anim.z", &BoardSounds::OGG_BAOER_HAPPY},
    };
    static constexpr int kKey1SceneCount =
        static_cast<int>(sizeof(kKey1Scenes) / sizeof(kKey1Scenes[0]));

    // The power button walks this list: the cute/happy/sad crop animations
    // together with the lanmeng-flavoured voice clips (lively young-female
    // TTS voice). Holding still still powers the board off.
    static constexpr EyeScene kPowerScenes[] = {
        {"cute.anim.z", &BoardSounds::OGG_LANMENG_CUTE},
        {"happy.anim.z", &BoardSounds::OGG_LANMENG_HAPPY},
        {"sad.anim.z", &BoardSounds::OGG_LANMENG_SAD},
    };
    static constexpr int kPowerSceneCount =
        static_cast<int>(sizeof(kPowerScenes) / sizeof(kPowerScenes[0]));

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    DualEyeDisplay* display_ = nullptr;
    AdcBatteryMonitor* battery_monitor_ = nullptr;
    // Plays the scene sound away from the main task, see SceneSoundLoop().
    TaskHandle_t scene_sound_task_ = nullptr;
    Button key1_button_;
    Button pwr_button_;
    Button touch1_button_;
    Button touch2_button_;
    bool screen_on_ = true;
    bool power_button_released_ = false;
    int key1_scene_ = -1;    // -1 so the first KEY 1 press lands on kKey1Scenes[0].
    int power_scene_ = -1;   // -1 so the first power click lands on kPowerScenes[0].
    bool scene_audio_enabled_ = true;  // KEY 1 long press toggles this.
    // Scene sound the eye_sound task should play next; read after the notify.
    std::atomic<const std::string_view*> pending_sound_{nullptr};

    void ConfigureWakeAndPowerOff() {
        rtc_gpio_init(PWR_BUTTON_GPIO);
        rtc_gpio_set_direction(PWR_BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(PWR_BUTTON_GPIO);
        rtc_gpio_pulldown_dis(PWR_BUTTON_GPIO);
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 0));

        rtc_gpio_init(PWR_CONTROL_PIN);
        rtc_gpio_set_direction(PWR_CONTROL_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_hold_dis(PWR_CONTROL_PIN);
        rtc_gpio_set_level(PWR_CONTROL_PIN, 0);
        esp_deep_sleep_start();
    }

    bool QualifyPowerOn() {
        const auto reset_reason = esp_reset_reason();
        const auto wakeup_cause = esp_sleep_get_wakeup_cause();
        if (reset_reason != ESP_RST_POWERON && wakeup_cause != ESP_SLEEP_WAKEUP_EXT0) {
            return true;
        }

        ESP_LOGI(TAG, "Hold power button for %d ms to power on", POWER_BUTTON_HOLD_MS);
        const int64_t start_time = esp_timer_get_time();
        while (esp_timer_get_time() - start_time < POWER_BUTTON_HOLD_MS * 1000LL) {
            if (gpio_get_level(PWR_BUTTON_GPIO) != 0) {
                ESP_LOGI(TAG, "Power button released before startup qualification");
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(POWER_BUTTON_POLL_MS));
        }
        return gpio_get_level(PWR_BUTTON_GPIO) == 0;
    }

    void HoldPower() {
        rtc_gpio_init(PWR_CONTROL_PIN);
        rtc_gpio_set_direction(PWR_CONTROL_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(PWR_CONTROL_PIN, 1);
    }

    void SetScreenOn(bool on) {
        auto backlight = GetBacklight();
        auto display = GetDisplay();
        if (on) {
            if (display != nullptr) {
                display->SetPowerSaveMode(false);
            }
            backlight->RestoreBrightness();
            screen_on_ = true;
        } else {
            backlight->SetBrightness(0);
            if (display != nullptr) {
                display->SetPowerSaveMode(true);
            }
            screen_on_ = false;
        }
    }

    void Shutdown() {
        ESP_LOGI(TAG, "Power off");
        SetScreenOn(false);
        auto codec = GetAudioCodec();
        if (codec != nullptr) {
            codec->EnableOutput(false);
        }
        ConfigureWakeAndPowerOff();
    }

    // KEY 1 long press now toggles the scene voice clips instead of the WiFi
    // config AP; the no-SSID boot provisioning path is untouched.

    // KEY 1 cycles shocked_blink -> cute_blink -> happy_blink, and every press
    // restarts both the animation and the matching voice clip from the
    // beginning. So a press made while nothing is playing gets picture and
    // sound in the same instant, and a press made while the previous scene is
    // still running is never swallowed: the animation cuts over immediately and
    // the sound restarts as soon as the previous clip's push into the audio
    // queue has drained.
    //
    // The power button does the same over its own cute -> happy -> sad list.
    // Both button handlers only pick the next scene and poke the sound task;
    // the panels and the audio queue are touched from the scheduled closures
    // and the sound task respectively.
    void AdvanceKey1Scene() {
        key1_scene_ = (key1_scene_ + 1) % kKey1SceneCount;
        StartScene(kKey1Scenes[key1_scene_], "KEY_1");
    }

    void AdvancePowerScene() {
        power_scene_ = (power_scene_ + 1) % kPowerSceneCount;
        StartScene(kPowerScenes[power_scene_], "PWR");
    }

    void StartScene(const EyeScene& scene, const char* source) {
        ESP_LOGI(TAG, "%s scene: %s", source, scene.animation);
        Application::GetInstance().Schedule([this, scene]() {
            if (display_ == nullptr) {
                return;
            }
            const auto asset = BoardAnimations::Get(scene.animation);
            if (asset.data == nullptr) {
                ESP_LOGE(TAG, "Eye scene %s is missing from the assets partition",
                         scene.animation);
                return;
            }
            // The animation restarts from its first frame here, on the main
            // task, so the press is visible immediately even while the previous
            // scene or its sound clip is still running.
            display_->StartAnimation(scene.animation, asset.data, asset.size);
        });
        pending_sound_.store(scene.sound, std::memory_order_relaxed);
        if (scene_sound_task_ != nullptr) {
            xTaskNotifyGive(scene_sound_task_);
        }
    }

    static void SceneSoundTaskEntry(void* arg) {
        static_cast<AiDualEyedDollBoard*>(arg)->SceneSoundLoop();
    }

    // AudioService::PlaySound() pushes the whole clip into a decode queue that
    // is only MAX_DECODE_PACKETS_IN_QUEUE deep (2400ms of audio) and blocks its
    // caller whenever the queue is full. The scene clips run up to ~3s, so
    // calling it from the main task would freeze button handling and the event
    // loop for a second or more - the exact opposite of "every press gets a
    // response".
    //
    // This task owns the playback instead. The notification coalesces, so a
    // burst of presses ends in a single restart rather than a queue of clips.
    void SceneSoundLoop() {
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (!scene_audio_enabled_) {
                continue;
            }
            const auto* sound = pending_sound_.load(std::memory_order_relaxed);
            if (sound == nullptr) {
                continue;
            }
            auto& audio = Application::GetInstance().GetAudioService();
            // Clear anything left over from an earlier press, otherwise the old
            // clip would still be in front of the new animation.
            audio.ResetDecoder();
            audio.PlaySound(*sound);
        }
    }

    // KEY 1 long press: mute or unmute the scene voice clips. Switching back on
    // plays tsundere.ogg as confirmation; switching off resets the decoder so
    // the clip that is currently playing stops promptly.
    void ToggleSceneAudio() {
        scene_audio_enabled_ = !scene_audio_enabled_;
        ESP_LOGI(TAG, "KEY_1 long press: scene audio %s",
                 scene_audio_enabled_ ? "on" : "off");
        if (!scene_audio_enabled_) {
            Application::GetInstance().Schedule([this]() {
                Application::GetInstance().GetAudioService().ResetDecoder();
            });
            return;
        }
        pending_sound_.store(&BoardSounds::OGG_TSUNDERE, std::memory_order_relaxed);
        if (scene_sound_task_ != nullptr) {
            xTaskNotifyGive(scene_sound_task_);
        }
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void ResetEyes() {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = 1ULL << DISPLAY_RST_PIN;
        io_conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io_conf);
        gpio_set_level(DISPLAY_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(DISPLAY_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(DISPLAY_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    void CreateEyePanel(gpio_num_t cs_pin, bool mirror_x, esp_lcd_panel_io_handle_t* io_out,
                        esp_lcd_panel_handle_t* panel_out) {
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = cs_pin;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, io_out));

        gc9d01_vendor_config_t vendor_config = {
            .init_cmds = kGc9d01InitCommands,
            .init_cmds_size =
                static_cast<uint16_t>(sizeof(kGc9d01InitCommands) / sizeof(kGc9d01InitCommands[0])),
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01(*io_out, &panel_config, panel_out));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(*panel_out));
        ESP_ERROR_CHECK(esp_lcd_panel_init(*panel_out));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(*panel_out, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(*panel_out, mirror_x, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*panel_out, true));
    }

    void InitializeEyes() {
        ESP_LOGI(TAG, "Init dual GC9D01N eyes");
        ResetEyes();

        esp_lcd_panel_io_handle_t left_io = nullptr;
        esp_lcd_panel_io_handle_t right_io = nullptr;
        esp_lcd_panel_handle_t left_panel = nullptr;
        esp_lcd_panel_handle_t right_panel = nullptr;
        CreateEyePanel(DISPLAY_LEFT_CS_PIN, false, &left_io, &left_panel);
        CreateEyePanel(DISPLAY_RIGHT_CS_PIN, true, &right_io, &right_panel);
        display_ = new DualEyeDisplay(left_io, left_panel, right_io, right_panel);
    }

    void InitializeButtons() {
        // The two touch pads and the original KEY 1 chat toggle were switched off
        // when KEY 1 was reworked into the animation cycle; the pads have no
        // handler any more and KEY 1 short press advances the scene instead.

        pwr_button_.OnPressUp([this]() { power_button_released_ = true; });
        // Short press cycles the cute/happy/sad scenes with their voice clips;
        // holding still powers the board off (unchanged).
        pwr_button_.OnClick([this]() { AdvancePowerScene(); });
        pwr_button_.OnLongPress([this]() {
            if (!power_button_released_) {
                ESP_LOGI(TAG, "Ignore power long press held from boot");
                return;
            }
            Application::GetInstance().Schedule([this]() { Shutdown(); });
        });
        power_button_released_ = gpio_get_level(PWR_BUTTON_GPIO) != 0;

        // KEY 1: short press cycles scenes, long press mutes/unmutes the scene
        // voice clips. (This replaces the old WiFi config toggle; the board
        // still provisions normally on first boot without a saved SSID.)
        key1_button_.OnClick([this]() { AdvanceKey1Scene(); });
        key1_button_.OnLongPress([this]() { ToggleSceneAudio(); });

        // Core 1 keeps this next to the audio and protocol tasks. It blocks on the
        // notification nearly all the time, so its low priority and 6KB stack only
        // matter while a clip is being demuxed.
        if (xTaskCreatePinnedToCore(SceneSoundTaskEntry, "eye_sound", 6144, this, 3,
                                    &scene_sound_task_, 1) != pdPASS) {
            scene_sound_task_ = nullptr;
            ESP_LOGE(TAG, "Failed to create the scene sound task; scenes will be silent");
        }
    }

    void InitializeBattery() {
        battery_monitor_ = new AdcBatteryMonitor(POWER_BATTERY_ADC_UNIT, POWER_BATTERY_ADC_CHANNEL,
                                                 200000, 200000, CHARGE_DETECT_PIN);
    }

public:
    AiDualEyedDollBoard()
        : key1_button_(KEY_1_BUTTON_GPIO, false, 2000),
          pwr_button_(PWR_BUTTON_GPIO, false, POWER_BUTTON_HOLD_MS),
          touch1_button_(TOUCH_1_GPIO, true),
          touch2_button_(TOUCH_2_GPIO, true) {
        if (!QualifyPowerOn()) {
            ConfigureWakeAndPowerOff();
        }
        HoldPower();
        InitializeI2c();
        InitializeSpi();
        InitializeEyes();
        InitializeButtons();
        InitializeBattery();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8389AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8389_ADDR, true,
            AUDIO_INPUT_CHANNELS);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = battery_monitor_->IsCharging();
        discharging = battery_monitor_->IsDischarging();
        level = battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(AiDualEyedDollBoard);
