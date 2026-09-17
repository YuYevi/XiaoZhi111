#include "board_animations.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <sdkconfig.h>
#include <spi_flash_mmap.h>

#include <cstring>

#define TAG "BoardAnimations"

// The partition image is written by esp_mmap_assets at build time, see
// managed_components/espressif__esp_mmap_assets/py_tool/spiffs_assets_gen.py:
//
//   +0   u32   file count
//   +4   u32   checksum over everything from +12 to the end of the image
//   +8   u32   length of table + payloads (NOT just the table)
//   +12  table[file count], each entry laid out as
//              char name[CONFIG_MMAP_FILE_NAME_LENGTH]   NUL padded
//              u32  size
//              u32  offset    relative to the end of the table
//              u16  width     images only, 0 for our blobs
//              u16  height
//   then the payloads back to back, each prefixed with two 0x5A bytes.
//
// The entry stride depends on CONFIG_MMAP_FILE_NAME_LENGTH, which defaults to
// 16 while main/assets.cc hardcodes 32. The stride therefore has to be derived
// from the same setting here, otherwise the table is parsed with the wrong
// alignment and every lookup returns garbage.
#ifdef CONFIG_MMAP_FILE_NAME_LENGTH
#define ANIM_NAME_LENGTH CONFIG_MMAP_FILE_NAME_LENGTH
#else
#error "esp_mmap_assets is not part of the build, so the assets partition image is never generated"
#endif

namespace {

constexpr size_t kNameLength = ANIM_NAME_LENGTH;
constexpr size_t kEntrySize = kNameLength + 4 + 4 + 2 + 2;
constexpr size_t kHeaderSize = 12;
constexpr uint8_t kPayloadMagic = 0x5A;  // 'Z'

const char* g_root = nullptr;
size_t g_partition_size = 0;
bool g_map_attempted = false;

// Maps the partition once and keeps it mapped for the rest of the run. The
// animation task reads frames straight out of this window, so unmapping it
// would pull the ground out from under a running animation.
bool EnsureMapped() {
    if (g_map_attempted) {
        return g_root != nullptr;
    }
    g_map_attempted = true;

    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (partition == nullptr) {
        ESP_LOGE(TAG, "No 'assets' partition; eye animations are unavailable");
        return false;
    }

    // esp_partition_mmap maps whole 64KB pages. The partition is 1MB, so it is
    // page aligned, but the mapping area is a shared resource worth reporting on.
    const int free_pages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    const size_t needed_pages = partition->size / (64 * 1024);
    ESP_LOGI(TAG, "MMapping %s (%u KB) for eye animations, %d free pages, %u needed",
             partition->label, static_cast<unsigned>(partition->size / 1024), free_pages,
             static_cast<unsigned>(needed_pages));

    esp_partition_mmap_handle_t handle = 0;
    const void* root = nullptr;
    const esp_err_t err =
        esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &root, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap the assets partition: %s", esp_err_to_name(err));
        return false;
    }

    g_root = static_cast<const char*>(root);
    g_partition_size = partition->size;
    const uint32_t count = *reinterpret_cast<const uint32_t*>(g_root);
    ESP_LOGI(TAG, "Assets partition ready with %u entries", static_cast<unsigned>(count));
    return true;
}

}  // namespace

BoardAnimations::Asset BoardAnimations::Get(const char* name) {
    if (name == nullptr || name[0] == '\0' || !EnsureMapped()) {
        return {nullptr, 0};
    }

    const uint32_t count = *reinterpret_cast<const uint32_t*>(g_root + 0);
    const uint32_t combined_len = *reinterpret_cast<const uint32_t*>(g_root + 8);
    const size_t table_bytes = static_cast<size_t>(count) * kEntrySize;
    if (count == 0 || kHeaderSize + table_bytes > g_partition_size ||
        kHeaderSize + combined_len > g_partition_size) {
        ESP_LOGE(TAG, "Assets table looks invalid (count=%u, len=%u)", static_cast<unsigned>(count),
                 static_cast<unsigned>(combined_len));
        return {nullptr, 0};
    }

    const char* table = g_root + kHeaderSize;
    // Payload offsets are relative to the end of the table, not to the start of
    // the image.
    const char* payloads = table + table_bytes;
    for (uint32_t i = 0; i < count; ++i) {
        const char* entry = table + i * kEntrySize;
        if (strncmp(entry, name, kNameLength) != 0) {
            continue;
        }

        uint32_t size = 0;
        uint32_t offset = 0;
        memcpy(&size, entry + kNameLength, sizeof(size));
        memcpy(&offset, entry + kNameLength + sizeof(size), sizeof(offset));

        const char* data = payloads + offset;
        if (data[0] != kPayloadMagic || data[1] != kPayloadMagic) {
            ESP_LOGE(TAG, "Asset %s has no payload magic", name);
            return {nullptr, 0};
        }
        return {data + 2, static_cast<size_t>(size)};
    }

    ESP_LOGE(TAG, "Asset %s not found in the assets partition", name);
    return {nullptr, 0};
}
