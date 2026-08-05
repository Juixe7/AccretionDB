#include "crc32.h"
#include <cstring>
#include <vector>
#include <string_view>

#include <mutex>

// CRC32 lookup table (IEEE 802.3 polynomial, reflected).
static uint32_t crc32_table[256];
static std::once_flag crc32_init_flag;

static void init_crc32_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        crc32_table[i] = crc;
    }
}

uint32_t compute_crc32(const uint8_t* data, size_t len) {
    std::call_once(crc32_init_flag, init_crc32_table);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

uint32_t compute_crc32_incremental(uint32_t crc, const uint8_t* data, size_t len) {
    std::call_once(crc32_init_flag, init_crc32_table);
    uint32_t c = crc ^ 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i)
        c = (c >> 8) ^ crc32_table[(c ^ data[i]) & 0xFF];
    return c ^ 0xFFFFFFFF;
}

uint32_t record_checksum(uint32_t key_size, uint32_t vlog_id, uint64_t vlog_offset, uint32_t vlog_len,
                         std::string_view key) {
    uint32_t checksum = 0xFFFFFFFF;
    
    auto update = [&](const void* data, size_t len) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        std::call_once(crc32_init_flag, init_crc32_table);
        for (size_t i = 0; i < len; ++i)
            checksum = (checksum >> 8) ^ crc32_table[(checksum ^ bytes[i]) & 0xFF];
    };

    update(&key_size, sizeof(key_size));
    update(&vlog_id, sizeof(vlog_id));
    update(&vlog_offset, sizeof(vlog_offset));
    update(&vlog_len, sizeof(vlog_len));
    if (key_size > 0 && !key.empty()) {
        update(key.data(), key_size);
    }
    return checksum ^ 0xFFFFFFFF;
}
