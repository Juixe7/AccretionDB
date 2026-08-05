#ifndef FORGELSM_SSTABLE_H
#define FORGELSM_SSTABLE_H

#include "vlog.h"
#include "bloom.h"
#include "cache.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

struct SSTableEntry {
    std::string key;
    VLogPointer pointer;
};

struct IndexEntry {
    std::string max_key;
    uint32_t offset;
    uint32_t length;
};

// Writes a sorted set of key-pointer pairs to an SSTable file.
//
// File layout (BLOCK ORIENTED):
//   [Data Blocks: 4KB chunks of entries]
//   [Index Block: serialized array of max_key, block_offset, block_size]
//   [Bloom Filter Bytes]
//   [Footer: uint32_t index_offset, uint32_t bloom_offset, uint32_t bloom_size, uint32_t checksum]
//
// Entry format:
//   [uint32_t key_size][key bytes][uint32_t file_id][uint64_t offset][uint32_t length]
class SSTableWriter {
public:
    static bool write(const std::string& path, const std::vector<SSTableEntry>& entries);
};

struct EngineMetrics;

// Loads and queries an SSTable file.
class SSTableReader {
public:
    SSTableReader() = default;
    ~SSTableReader();

    // Load Index and Bloom Filter into memory.
    bool load(const std::string& path);

    // Query key. Uses cache for block lookups.
    bool get(std::string_view key, VLogPointer& out_pointer, forgelsm::ShardedLRUCache* cache, EngineMetrics* metrics = nullptr) const;

    uint32_t sequence() const { return sequence_; }
    const std::string& path() const { return path_; }

    const std::string& min_key() const { return min_key_; }
    const std::string& max_key() const { return max_key_; }

    bool overlaps(std::string_view min_k, std::string_view max_k) const {
        if (index_.empty()) return false;
        return !(max_key() < min_k || min_key() > max_k);
    }

    const std::vector<IndexEntry>& index() const { return index_; }
    const BloomFilter& bloom() const { return bloom_; }
    const uint8_t* mapped_data() const { return mapped_data_; }

private:
    std::string              path_;
    uint32_t                 sequence_ = 0;
    std::string              min_key_;
    std::string              max_key_;
    std::vector<IndexEntry>  index_;
    BloomFilter              bloom_;
    int                      fd_ = -1;
#ifdef _WIN32
    HANDLE                   hFile_ = INVALID_HANDLE_VALUE;
    HANDLE                   hMap_ = NULL;
    const uint8_t*           mapped_data_ = nullptr;
    size_t                   mapped_size_ = 0;
#endif
};

#endif // FORGELSM_SSTABLE_H
