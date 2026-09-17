#include "dual_eye_display.h"

#include "config.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <miniz.h>

#define TAG "DualEyeDisplay"

#define EYE_PIXELS (DISPLAY_WIDTH * DISPLAY_HEIGHT)
#define EYE_BYTES (EYE_PIXELS * sizeof(uint16_t))

#define ANIM_MAGIC 0x31414544u  // 'D','E','A','1' little-endian
#define ANIM_VERSION 1
#define ANIM_HEADER_SIZE (sizeof(DualEyeDisplay::AnimationHeader))

extern const uint8_t cute_rgb565_z_start[] asm("_binary_cute_rgb565_z_start");
extern const uint8_t cute_rgb565_z_end[] asm("_binary_cute_rgb565_z_end");
extern const uint8_t happy_rgb565_z_start[] asm("_binary_happy_rgb565_z_start");
extern const uint8_t happy_rgb565_z_end[] asm("_binary_happy_rgb565_z_end");
extern const uint8_t surprised_rgb565_z_start[] asm("_binary_surprised_rgb565_z_start");
extern const uint8_t surprised_rgb565_z_end[] asm("_binary_surprised_rgb565_z_end");
extern const uint8_t angry_rgb565_z_start[] asm("_binary_angry_rgb565_z_start");
extern const uint8_t angry_rgb565_z_end[] asm("_binary_angry_rgb565_z_end");
extern const uint8_t sad_rgb565_z_start[] asm("_binary_sad_rgb565_z_start");
extern const uint8_t sad_rgb565_z_end[] asm("_binary_sad_rgb565_z_end");
extern const uint8_t sleepy_rgb565_z_start[] asm("_binary_sleepy_rgb565_z_start");
extern const uint8_t sleepy_rgb565_z_end[] asm("_binary_sleepy_rgb565_z_end");
extern const uint8_t shy_rgb565_z_start[] asm("_binary_shy_rgb565_z_start");
extern const uint8_t shy_rgb565_z_end[] asm("_binary_shy_rgb565_z_end");
extern const uint8_t shocked_rgb565_z_start[] asm("_binary_shocked_rgb565_z_start");
extern const uint8_t shocked_rgb565_z_end[] asm("_binary_shocked_rgb565_z_end");

struct EyeAsset {
    const char* name;
    const uint8_t* start;
    const uint8_t* end;
};

static const EyeAsset kEyeAssets[] = {
    {"cute", cute_rgb565_z_start, cute_rgb565_z_end},
    {"happy", happy_rgb565_z_start, happy_rgb565_z_end},
    {"surprised", surprised_rgb565_z_start, surprised_rgb565_z_end},
    {"angry", angry_rgb565_z_start, angry_rgb565_z_end},
    {"sad", sad_rgb565_z_start, sad_rgb565_z_end},
    {"sleepy", sleepy_rgb565_z_start, sleepy_rgb565_z_end},
    {"shy", shy_rgb565_z_start, shy_rgb565_z_end},
    {"shocked", shocked_rgb565_z_start, shocked_rgb565_z_end},
};

// Packed RGB565 assets are little-endian. GC9D01 expects the high byte first
// on SPI, so swap each pixel before handing the buffer to esp_lcd.
static void SwapPixelBytes(uint8_t* bytes, size_t length) {
    for (size_t i = 0; i + 1 < length; i += sizeof(uint16_t)) {
        const uint8_t high = bytes[i];
        bytes[i] = bytes[i + 1];
        bytes[i + 1] = high;
    }
}

bool DualEyeDisplay::Inflate(const uint8_t* start, size_t size, uint8_t* dest, size_t dest_len) {
    // tinfl_decompress_mem_to_mem() keeps tinfl_decompressor on the caller's stack (~4KB+).
    // Board init runs on the 8KB main task, so that overflows and reboots with StoreProhibited.
    if (inflater_ == nullptr) {
        inflater_ =
            heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (inflater_ == nullptr) {
            inflater_ = malloc(sizeof(tinfl_decompressor));
        }
        if (inflater_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate zlib decompressor");
            return false;
        }
    }
    auto* inflater = static_cast<tinfl_decompressor*>(inflater_);

    tinfl_init(inflater);
    size_t src_len = size;
    size_t out_len = dest_len;
    const tinfl_status status =
        tinfl_decompress(inflater, start, &src_len, dest, dest, &out_len,
                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || out_len != dest_len) {
        ESP_LOGE(TAG, "Inflate failed: status=%d len=%u", static_cast<int>(status),
                 static_cast<unsigned>(out_len));
        return false;
    }
    return true;
}

DualEyeDisplay::DualEyeDisplay(esp_lcd_panel_io_handle_t left_io, esp_lcd_panel_handle_t left_panel,
                               esp_lcd_panel_io_handle_t right_io,
                               esp_lcd_panel_handle_t right_panel)
    : left_panel_(left_panel), right_panel_(right_panel) {
    width_ = DISPLAY_WIDTH;
    height_ = DISPLAY_HEIGHT;
    mutex_ = xSemaphoreCreateMutex();
    flush_done_ = xSemaphoreCreateBinary();
    animation_stopped_ = xSemaphoreCreateBinary();
    frame_ =
        static_cast<uint16_t*>(heap_caps_malloc(EYE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (frame_ == nullptr) {
        frame_ =
            static_cast<uint16_t*>(heap_caps_malloc(EYE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
    }
    if (frame_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate eye frame buffer");
        return;
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushDone,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(left_io, &cbs, this));
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(right_io, &cbs, this));

    DrawEmotion("cute");
}

DualEyeDisplay::~DualEyeDisplay() {
    StopAnimation();
    if (frame_ != nullptr) {
        heap_caps_free(frame_);
        frame_ = nullptr;
    }
    if (delta_ != nullptr) {
        heap_caps_free(delta_);
        delta_ = nullptr;
    }
    if (accum_ != nullptr) {
        heap_caps_free(accum_);
        accum_ = nullptr;
    }
    if (inflater_ != nullptr) {
        heap_caps_free(inflater_);
        inflater_ = nullptr;
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
    if (flush_done_ != nullptr) {
        vSemaphoreDelete(flush_done_);
    }
    if (animation_stopped_ != nullptr) {
        vSemaphoreDelete(animation_stopped_);
    }
}

bool DualEyeDisplay::Lock(int timeout_ms) {
    if (mutex_ == nullptr) {
        return true;
    }
    return xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : portMAX_DELAY)) ==
           pdTRUE;
}

void DualEyeDisplay::Unlock() {
    if (mutex_ != nullptr) {
        xSemaphoreGive(mutex_);
    }
}

bool DualEyeDisplay::OnFlushDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                                 void* user_ctx) {
    auto* self = static_cast<DualEyeDisplay*>(user_ctx);
    BaseType_t hp_task = pdFALSE;
    xSemaphoreGiveFromISR(self->flush_done_, &hp_task);
    return hp_task == pdTRUE;
}

bool DualEyeDisplay::WaitFlush() {
    return xSemaphoreTake(flush_done_, pdMS_TO_TICKS(200)) == pdTRUE;
}

const char* DualEyeDisplay::MapEmotion(const char* emotion) {
    if (emotion == nullptr || emotion[0] == '\0') {
        return "cute";
    }
    if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0 ||
        strcmp(emotion, "funny") == 0 || strcmp(emotion, "confident") == 0 ||
        strcmp(emotion, "relaxed") == 0 || strcmp(emotion, "delicious") == 0 ||
        strcmp(emotion, "cool") == 0 || strcmp(emotion, "silly") == 0) {
        return "happy";
    }
    if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0) {
        return "sad";
    }
    if (strcmp(emotion, "angry") == 0) {
        return "angry";
    }
    if (strcmp(emotion, "surprised") == 0) {
        return "surprised";
    }
    if (strcmp(emotion, "shocked") == 0 || strcmp(emotion, "thinking") == 0 ||
        strcmp(emotion, "confused") == 0) {
        return "shocked";
    }
    if (strcmp(emotion, "sleepy") == 0) {
        return "sleepy";
    }
    if (strcmp(emotion, "shy") == 0 || strcmp(emotion, "embarrassed") == 0) {
        return "shy";
    }
    if (strcmp(emotion, "cute") == 0 || strcmp(emotion, "loving") == 0 ||
        strcmp(emotion, "kissy") == 0 || strcmp(emotion, "winking") == 0 ||
        strcmp(emotion, "neutral") == 0 || strcmp(emotion, "robot_2") == 0) {
        return "cute";
    }
    return "cute";
}

void DualEyeDisplay::DrawFrameToPanels() {
    xSemaphoreTake(flush_done_, 0);
    ESP_ERROR_CHECK(
        esp_lcd_panel_draw_bitmap(left_panel_, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, frame_));
    if (!WaitFlush()) {
        ESP_LOGW(TAG, "Left eye flush timeout");
    }
    xSemaphoreTake(flush_done_, 0);
    ESP_ERROR_CHECK(
        esp_lcd_panel_draw_bitmap(right_panel_, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, frame_));
    if (!WaitFlush()) {
        ESP_LOGW(TAG, "Right eye flush timeout");
    }
}

bool DualEyeDisplay::DrawEmotion(const char* emotion) {
    if (frame_ == nullptr || left_panel_ == nullptr || right_panel_ == nullptr) {
        return false;
    }

    const char* mapped = MapEmotion(emotion);
    const EyeAsset* asset = nullptr;
    for (const auto& item : kEyeAssets) {
        if (strcmp(item.name, mapped) == 0) {
            asset = &item;
            break;
        }
    }
    if (asset == nullptr) {
        return false;
    }

    auto* bytes = reinterpret_cast<uint8_t*>(frame_);
    if (!Inflate(asset->start, static_cast<size_t>(asset->end - asset->start), bytes, EYE_BYTES)) {
        ESP_LOGE(TAG, "Decompress %s failed", asset->name);
        return false;
    }
    SwapPixelBytes(bytes, EYE_BYTES);
    DrawFrameToPanels();

    current_emotion_ = mapped;
    ESP_LOGI(TAG, "Emotion %s -> %s", emotion, mapped);
    return true;
}

void DualEyeDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    const char* mapped = MapEmotion(emotion);
    if (current_emotion_ != nullptr && strcmp(current_emotion_, mapped) == 0 && !animation_run_) {
        return;
    }
    // A static expression takes ownership of the panels, so any running
    // animation has to wind down first. StopAnimation() does not take mutex_,
    // and the animation task never takes it either, so this cannot deadlock.
    StopAnimation();
    DrawEmotion(mapped);
}

bool DualEyeDisplay::StartAnimation(const char* name, const void* data, size_t size) {
    if (data == nullptr || size < ANIM_HEADER_SIZE) {
        ESP_LOGE(TAG, "Animation %s is empty", name);
        return false;
    }

    AnimationHeader header{};
    memcpy(&header, data, sizeof(header));
    const size_t table_bytes = static_cast<size_t>(header.frame_count) * sizeof(uint32_t);
    if (header.magic != ANIM_MAGIC || header.version != ANIM_VERSION ||
        header.width != DISPLAY_WIDTH || header.height != DISPLAY_HEIGHT ||
        header.frame_count == 0 || header.delay_ms == 0 || size < ANIM_HEADER_SIZE + table_bytes) {
        ESP_LOGE(TAG, "Animation %s has an unsupported header", name);
        return false;
    }

    DisplayLockGuard lock(this);
    StopAnimation();

    if (frame_ == nullptr) {
        return false;
    }
    if (delta_ == nullptr) {
        delta_ =
            static_cast<uint8_t*>(heap_caps_malloc(EYE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (accum_ == nullptr) {
        accum_ =
            static_cast<uint8_t*>(heap_caps_malloc(EYE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (delta_ == nullptr || accum_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate animation buffers");
        return false;
    }

    memset(accum_, 0, EYE_BYTES);
    animation_name_ = name;
    animation_data_ = data;
    animation_size_ = size;
    animation_run_ = true;

    // Core 0 keeps the ~20ms of SPI flushing per frame away from the audio task
    // on core 1. Priority 4 stays below the audio and protocol tasks.
    if (xTaskCreatePinnedToCore(AnimationTaskEntry, "eye_anim", 4096, this, 4, &animation_task_,
                                0) != pdPASS) {
        animation_run_ = false;
        animation_task_ = nullptr;
        animation_name_ = nullptr;
        animation_data_ = nullptr;
        animation_size_ = 0;
        ESP_LOGE(TAG, "Failed to create animation task");
        return false;
    }

    ESP_LOGI(TAG, "Animation %s started (%u frames, %u ms)", name, header.frame_count,
             header.delay_ms);
    return true;
}

void DualEyeDisplay::StopAnimation() {
    if (!animation_run_ && animation_task_ == nullptr) {
        return;
    }
    animation_run_ = false;
    if (animation_task_ != nullptr) {
        // Wake the task out of its frame delay. Without this the caller would
        // wait up to a whole frame period (90ms for the shipped clips) before
        // the panels are handed over, which is exactly the delay a button press
        // must not have.
        xTaskNotifyGive(animation_task_);
        if (animation_stopped_ != nullptr) {
            // The task checks animation_run_ once per frame, so one frame period
            // plus a flush is the normal hand-off time.
            if (xSemaphoreTake(animation_stopped_, pdMS_TO_TICKS(500)) != pdTRUE) {
                ESP_LOGW(TAG, "Animation task did not stop in time");
            }
        }
    }
    animation_task_ = nullptr;
    animation_name_ = nullptr;
    animation_data_ = nullptr;
    animation_size_ = 0;
}

void DualEyeDisplay::AnimationTaskEntry(void* arg) {
    static_cast<DualEyeDisplay*>(arg)->AnimationLoop();
}

bool DualEyeDisplay::DecodeAnimationFrame(const uint8_t* compressed, size_t compressed_size) {
    if (!Inflate(compressed, compressed_size, delta_, EYE_BYTES)) {
        return false;
    }

    // apply the delta and byte-swap in one pass. The delta is byte-wise, so the
    // low and high halves of each RGB565 pixel are independent.
    auto* out = reinterpret_cast<uint8_t*>(frame_);
    for (size_t i = 0; i < EYE_BYTES; i += 2) {
        const uint8_t lo = static_cast<uint8_t>(accum_[i] + delta_[i]);
        const uint8_t hi = static_cast<uint8_t>(accum_[i + 1] + delta_[i + 1]);
        accum_[i] = lo;
        accum_[i + 1] = hi;
        out[i] = hi;  // GC9D01 wants the high byte first on SPI.
        out[i + 1] = lo;
    }
    return true;
}

void DualEyeDisplay::AnimationLoop() {
    const auto* base = static_cast<const uint8_t*>(animation_data_);
    AnimationHeader header{};
    memcpy(&header, base, sizeof(header));

    const size_t data_offset =
        ANIM_HEADER_SIZE + static_cast<size_t>(header.frame_count) * sizeof(uint32_t);
    const auto* table = reinterpret_cast<const uint32_t*>(base + ANIM_HEADER_SIZE);
    const TickType_t period = pdMS_TO_TICKS(header.delay_ms);
    const char* name = animation_name_;

    while (animation_run_) {
        // The first frame of the asset is a delta against an all-zero buffer,
        // so every loop restarts from a cleared accumulator.
        memset(accum_, 0, EYE_BYTES);
        size_t offset = data_offset;
        TickType_t last_wake = xTaskGetTickCount();

        for (uint16_t index = 0; index < header.frame_count && animation_run_; ++index) {
            if (!DecodeAnimationFrame(base + offset, table[index])) {
                ESP_LOGE(TAG, "Animation %s aborted at frame %u", name, index);
                animation_run_ = false;
                break;
            }
            offset += table[index];
            DrawFrameToPanels();

            // Sleep out the rest of this frame's period. StopAnimation() pokes the
            // task so a button press ends the wait immediately instead of waiting
            // for the delay to expire.
            const TickType_t deadline = last_wake + period;
            const TickType_t now = xTaskGetTickCount();
            if (now < deadline) {
                ulTaskNotifyTake(pdTRUE, deadline - now);
            }

            // Keep the cadence, but resync after a slow flush instead of running
            // the missed frames back to back.
            last_wake = xTaskGetTickCount();
            if (last_wake < deadline) {
                last_wake = deadline;
            }
        }
    }

    ESP_LOGI(TAG, "Animation %s stopped", name);
    animation_task_ = nullptr;
    xSemaphoreGive(animation_stopped_);
    vTaskDelete(nullptr);
}

void DualEyeDisplay::SetStatus(const char* status) { ESP_LOGD(TAG, "SetStatus: %s", status); }

void DualEyeDisplay::SetChatMessage(const char* role, const char* content) {
    ESP_LOGD(TAG, "Chat %s: %s", role, content);
}

void DualEyeDisplay::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGD(TAG, "Notify (%d ms): %s", duration_ms, notification);
}

void DualEyeDisplay::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    if (on) {
        // The panels are off; keeping the animation task awake would only burn CPU.
        StopAnimation();
    }
    if (left_panel_ != nullptr) {
        esp_lcd_panel_disp_on_off(left_panel_, !on);
    }
    if (right_panel_ != nullptr) {
        esp_lcd_panel_disp_on_off(right_panel_, !on);
    }
}
