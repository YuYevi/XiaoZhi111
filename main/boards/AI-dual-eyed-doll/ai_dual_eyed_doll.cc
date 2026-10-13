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
#include "esp_lcd_panel_st7789.h"

#define TAG "AiDualEyedDoll"

// 最小化的 {命令、数据、长度、延时} 记录。esp_lcd 的 ST7789 驱动没有
// vendor_config 钩子（初始化只发送 SLPOUT、MADCTL、COLMOD 和 0xB0），
// 因此面板专用参数直接通过 panel IO 发送。
struct PanelInitCmd {
    uint8_t cmd;
    const uint8_t* data;
    uint16_t data_bytes;
    uint32_t delay_ms;
};

// 0.9 英寸 BOE 圆屏（ST7735S/ST7735P 系列）的电源、帧率、VCOM 和伽马配置，
// 参考模块附带的 KAC STM32F103 SPI 示例。
//
// 以下命令由驱动完成，因此不在这里重复发送：
//   0x11 SLPOUT - 驱动发送后等待 100 ms
//   0x3A COLMOD - 驱动发送 0x55，即 16 位像素格式
//   0x36 MADCTL 0xC8 - 驱动的 BGR 位与 esp_lcd_panel_mirror(true, true)
//   0x21 INVON   - esp_lcd_panel_invert_color(DISPLAY_INVERT_COLOR)
//   0x29 DISPON  - esp_lcd_panel_disp_on_off(true)
//
// MADCTL 0xC8 是面板的参考方向；驱动只会改写 esp_lcd_panel_mirror() 中的
// MX/MY，因此 BGR 位会保留。
static const PanelInitCmd kBoePanelInit[] = {
    {0xB1, (const uint8_t[]){0x02, 0x35, 0x36}, 3, 0},  // FRMCTR1：正常模式帧率
    {0xB2, (const uint8_t[]){0x02, 0x35, 0x36}, 3, 0},  // FRMCTR2：空闲模式帧率
    {0xB3, (const uint8_t[]){0x02, 0x35, 0x36, 0x02, 0x35, 0x36}, 6, 0},  // FRMCTR3：部分模式帧率
    {0xB4, (const uint8_t[]){0x00}, 1, 0},              // INVCTR：关闭点反转
    {0xC0, (const uint8_t[]){0xA2, 0x02, 0x84}, 3, 0},  // PWCTR1：电源控制 1
    {0xC1, (const uint8_t[]){0xC5}, 1, 0},              // PWCTR2：电源控制 2
    {0xC2, (const uint8_t[]){0x0D, 0x00}, 2, 0},        // PWCTR3：电源控制 3
    {0xC3, (const uint8_t[]){0x8A, 0x2A}, 2, 0},        // PWCTR4：电源控制 4
    {0xC4, (const uint8_t[]){0x8D, 0xEE}, 2, 0},        // PWCTR5：电源控制 5
    {0xC5, (const uint8_t[]){0x02}, 1, 0},              // VMCTR1：VCOM 控制
    {0xE0, (const uint8_t[]){0x12, 0x1C, 0x10, 0x18, 0x33, 0x2C, 0x25, 0x28,
                             0x28, 0x27, 0x2F, 0x3C, 0x00, 0x03, 0x03, 0x10},
     16, 0},                                            // GMCTRP1：正伽马
    {0xE1, (const uint8_t[]){0x12, 0x1C, 0x10, 0x18, 0x2D, 0x28, 0x23, 0x28,
                             0x28, 0x26, 0x2F, 0x3B, 0x00, 0x03, 0x03, 0x10},
     16, 0},                                            // GMCTRN1：负伽马
};

// 通过已初始化的 panel IO 发送面板专用配置。
// 调用时 esp_lcd_panel_init() 已经发送完驱动自带的四条命令。
static void ApplyPanelInit(esp_lcd_panel_io_handle_t io, const PanelInitCmd* cmds, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const PanelInitCmd& item = cmds[i];
        ESP_ERROR_CHECK(
            esp_lcd_panel_io_tx_param(io, item.cmd, item.data, item.data_bytes));
        if (item.delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(item.delay_ms));
        }
    }
    ESP_LOGI(TAG, "BOE panel init applied (%u commands)", static_cast<unsigned>(count));
}

class AiDualEyedDollBoard : public WifiBoard {
private:
    // 一个场景由只读资源分区中的逐帧差分动画和对应的内置 Ogg 音频组成。
    struct EyeScene {
        const char* animation;
        const std::string_view* sound;
    };

    // KEY 1 按顺序循环使用此列表。*_blnk 资源来自 *_blink.gif，语音使用宝儿风格的
    // 年轻女性 TTS。资源名最多 15 个字符；CONFIG_MMAP_FILE_NAME_LENGTH 为 16，
    // 更长的名称会在资源表中被截断，导致精确查找失败。
    static constexpr EyeScene kKey1Scenes[] = {
        {"shckblnk.anim.z", &BoardSounds::OGG_BAOER_SHOCKED},
        {"cuteblnk.anim.z", &BoardSounds::OGG_BAOER_CUTE},
        {"hapyblnk.anim.z", &BoardSounds::OGG_BAOER_HAPPY},
    };
    static constexpr int kKey1SceneCount =
        static_cast<int>(sizeof(kKey1Scenes) / sizeof(kKey1Scenes[0]));

    // 电源键按顺序循环使用可爱、开心和难过裁剪动画及对应的岚梦风格年轻女性 TTS。
    // 长按电源键仍然执行关机。
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
    // 在独立任务中播放场景音频，详见 SceneSoundLoop()。
    TaskHandle_t scene_sound_task_ = nullptr;
    Button key1_button_;
    Button pwr_button_;
    Button touch1_button_;
    Button touch2_button_;
    bool screen_on_ = true;
    bool power_button_released_ = false;
    int key1_scene_ = -1;    // 从 -1 开始，使第一次 KEY 1 按键选择 kKey1Scenes[0]。
    int power_scene_ = -1;   // 从 -1 开始，使第一次电源键短按选择 kPowerScenes[0]。
    bool scene_audio_enabled_ = true;  // KEY 1 长按切换此状态。
    // eye_sound 任务下一次要播放的场景音频；收到通知后读取。
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

    // KEY 1 长按现在切换场景语音，不再切换 Wi-Fi 配网 AP；无 SSID 时的首次启动配网不变。

    // KEY 1 按 shocked_blink -> cute_blink -> happy_blink 顺序循环，每次按键都会从头
    // 重启动画和对应语音。即使上一场景仍在播放，动画也会立即切换，语音会在旧音频
    // 清理后重新开始。电源键同理，按 cute -> happy -> sad 顺序循环。
    // 按键回调只选择场景并通知音频任务；面板和音频队列分别由主任务闭包和音频任务操作。
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
            // 在主任务中从第一帧重新启动动画，即使上一场景或语音仍在运行，
            // 按键反馈也能立即显示。
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

    // AudioService::PlaySound() 会把整段音频推入解码队列。队列只有约 2400 ms 容量，
    // 满载时会阻塞调用者。场景音频最长约 3 秒，如果在主任务中调用，会冻结按键处理
    // 和事件循环。因此由此任务负责播放；连续快速按键会合并为一次重新播放。
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
            // 清除上一次按键遗留的音频，否则旧语音会排在新场景前面。
            audio.ResetDecoder();
            audio.PlaySound(*sound);
        }
    }

    // KEY 1 长按：静音或恢复场景语音。恢复时播放 tsundere.ogg 作为提示；静音时重置
    // 解码器，使当前语音立即停止。
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

    // mirror_x/mirror_y 对应 MADCTL 的 MX/MY 位。BOE 厂商序列将面板置于参考方向
    //（0xC8 = MX|MY|BGR），左眼保留两位，右眼因硬件镜像安装而清除 MX。
    // esp_lcd_panel_mirror() 只改写 MX/MY，因此厂商序列中的 BGR 位会保留。
    void CreateEyePanel(gpio_num_t cs_pin, bool mirror_x, bool mirror_y,
                        esp_lcd_panel_io_handle_t* io_out, esp_lcd_panel_handle_t* panel_out) {
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = cs_pin;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, io_out));

        // BOE 面板通过 esp_lcd 内置的 ST7789 驱动驱动，和项目中的其他 ST7735 板卡一致。
        // ST7735S 与 ST7789 兼容 DualEyeDisplay 使用的命令；驱动会在 draw_bitmap() 中
        // 应用 GRAM 偏移。但驱动初始化只有四条命令，因此面板专用配置要在此后通过
        // panel IO 发送。
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(*io_out, &panel_config, panel_out));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(*panel_out));
        ESP_ERROR_CHECK(esp_lcd_panel_init(*panel_out));
        ApplyPanelInit(*io_out, kBoePanelInit, sizeof(kBoePanelInit) / sizeof(kBoePanelInit[0]));
        // BOE 面板将 GRAM (0,0) 映射到 (2,2)；set_gap() 会为 CASET/RASET 窗口应用该偏移。
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(*panel_out, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(*panel_out, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(*panel_out, mirror_x, mirror_y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*panel_out, true));
    }

    void InitializeEyes() {
        ESP_LOGI(TAG, "Init dual ST7735S eyes (0.9\" BOE round, 128x128)");
        ResetEyes();

        esp_lcd_panel_io_handle_t left_io = nullptr;
        esp_lcd_panel_io_handle_t right_io = nullptr;
        esp_lcd_panel_handle_t left_panel = nullptr;
        esp_lcd_panel_handle_t right_panel = nullptr;
        CreateEyePanel(DISPLAY_LEFT_CS_PIN, true, true, &left_io, &left_panel);
        CreateEyePanel(DISPLAY_RIGHT_CS_PIN, false, true, &right_io, &right_panel);
        display_ = new DualEyeDisplay(left_io, left_panel, right_io, right_panel);
    }

    void InitializeButtons() {
        // KEY 1 改为动画循环后，两个触摸键和原有的 KEY 1 对话切换功能均已关闭；
        // 触摸键不再注册处理器，KEY 1 短按改为切换场景。

        pwr_button_.OnPressUp([this]() { power_button_released_ = true; });
        // 短按循环 cute/happy/sad 场景及对应语音；长按关机，行为保持不变。
        pwr_button_.OnClick([this]() { AdvancePowerScene(); });
        pwr_button_.OnLongPress([this]() {
            if (!power_button_released_) {
                ESP_LOGI(TAG, "Ignore power long press held from boot");
                return;
            }
            Application::GetInstance().Schedule([this]() { Shutdown(); });
        });
        power_button_released_ = gpio_get_level(PWR_BUTTON_GPIO) != 0;

        // KEY 1：短按循环场景，长按静音或恢复场景语音。
        // 这替代了旧的 Wi-Fi 配网切换功能，但无已保存 SSID 时仍会正常进入首次配网。
        key1_button_.OnClick([this]() { AdvanceKey1Scene(); });
        key1_button_.OnLongPress([this]() { ToggleSceneAudio(); });

        // 音频任务固定在 Core 1 附近运行。该任务大部分时间阻塞在通知上，只有解复用音频
        // 时才会使用低优先级和 6 KB 栈空间。
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
