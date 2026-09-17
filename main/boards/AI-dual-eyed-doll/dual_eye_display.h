#ifndef DUAL_EYE_DISPLAY_H
#define DUAL_EYE_DISPLAY_H

#include "display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstddef>

struct tinfl_decompressor_tag;

class DualEyeDisplay : public Display {
public:
    DualEyeDisplay(esp_lcd_panel_io_handle_t left_io, esp_lcd_panel_handle_t left_panel,
                   esp_lcd_panel_io_handle_t right_io, esp_lcd_panel_handle_t right_panel);
    ~DualEyeDisplay() override;

    void SetEmotion(const char* emotion) override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void SetPowerSaveMode(bool on) override;

    // Frame-delta animations packed by scripts/dual_eye_anim/pack_eye_anim.py.
    // Loops until StopAnimation(), another drawing call, or power save.
    // `data` must stay valid for as long as the animation runs, which is what the
    // mmapped assets partition in board_animations.cc provides.
    bool StartAnimation(const char* name, const void* data, size_t size);
    void StopAnimation();
    bool IsAnimationRunning() const { return animation_run_; }

private:
    // On-disk layout of a *.anim.z asset, see pack_eye_anim.py.
    struct AnimationHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t frame_count;
        uint16_t width;
        uint16_t height;
        uint16_t delay_ms;
        uint16_t reserved;
    } __attribute__((packed));

    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

    bool DrawEmotion(const char* emotion);
    static const char* MapEmotion(const char* emotion);
    static bool OnFlushDone(esp_lcd_panel_io_handle_t panel_io,
                            esp_lcd_panel_io_event_data_t* edata, void* user_ctx);
    bool WaitFlush();

    bool Inflate(const uint8_t* start, size_t size, uint8_t* dest, size_t dest_len);
    void DrawFrameToPanels();
    bool DecodeAnimationFrame(const uint8_t* compressed, size_t compressed_size);
    void AnimationLoop();
    static void AnimationTaskEntry(void* arg);

    esp_lcd_panel_handle_t left_panel_ = nullptr;
    esp_lcd_panel_handle_t right_panel_ = nullptr;
    uint16_t* frame_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    SemaphoreHandle_t flush_done_ = nullptr;
    const char* current_emotion_ = nullptr;

    // Animation state. All three buffers hold one RGB565 frame each.
    void* inflater_ = nullptr;  // tinfl_decompressor, lazily allocated, ~11KB on the heap.
    uint8_t* delta_ = nullptr;  // Decompressed per-frame delta.
    uint8_t* accum_ = nullptr;  // Running sum of deltas, little-endian RGB565.
    TaskHandle_t animation_task_ = nullptr;
    SemaphoreHandle_t animation_stopped_ = nullptr;
    const char* animation_name_ = nullptr;
    const void* animation_data_ = nullptr;
    size_t animation_size_ = 0;
    volatile bool animation_run_ = false;
};

#endif  // DUAL_EYE_DISPLAY_H
