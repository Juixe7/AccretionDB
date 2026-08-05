#ifndef FORGELSM_CRC32_H
#define FORGELSM_CRC32_H

#include <cstdint>
#include <string>
#include <string_view>

// Compute CRC32 over arbitrary byte buffer.
uint32_t compute_crc32(const uint8_t* data, size_t len);
uint32_t compute_crc32_incremental(uint32_t crc, const uint8_t* data, size_t len);

// Compute CRC32 over the WAL record fields:
//   key_size (4 bytes) + vlog_id (4 bytes) + vlog_offset (8 bytes) + vlog_len (4 bytes) + key
// This is the checksum stored alongside each WAL record.
uint32_t record_checksum(uint32_t key_size, uint32_t vlog_id, uint64_t vlog_offset, uint32_t vlog_len,
                         std::string_view key);

#endif // FORGELSM_CRC32_H
