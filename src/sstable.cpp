#include "sstable.h"
#include "kvstore.h"
#include "crc32.h"
#include "io_util.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#ifdef _WIN32
  #include <io.h>
  #define sst_open(p, f, m) _open(p, f, m)
  #define sst_close(fd)     _close(fd)
#else
  #include <unistd.h>
  #define sst_open(p, f, m) open(p, f, m)
  #define sst_close(fd)     close(fd)
#endif
#include <vector>

// ── SSTableWriter ──────────────────────────────────────────────

bool SSTableWriter::write(const std::string& path,
                          const std::vector<SSTableEntry>& entries) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    std::vector<IndexEntry> index;
    std::vector<uint8_t> block_buf;
    size_t block_limit = 4096;
    uint32_t current_offset = 0;

    std::vector<uint32_t> restarts;
    std::string last_key = "";
    int restart_interval = 16;
    int counter = 0;

    uint32_t checksum = 0;

    auto flush_block_buf = [&]() -> bool {
        if (block_buf.empty()) return true;
        
        for (uint32_t r : restarts) {
            uint8_t temp[4];
            std::memcpy(temp, &r, 4);
            block_buf.insert(block_buf.end(), temp, temp + 4);
        }
        uint32_t nr = static_cast<uint32_t>(restarts.size());
        uint8_t temp[4];
        std::memcpy(temp, &nr, 4);
        block_buf.insert(block_buf.end(), temp, temp + 4);

        checksum = compute_crc32_incremental(checksum, block_buf.data(), block_buf.size());
        out.write(reinterpret_cast<const char*>(block_buf.data()), static_cast<std::streamsize>(block_buf.size()));
        if (out.fail()) return false;
        
        current_offset += static_cast<uint32_t>(block_buf.size());
        
        block_buf.clear();
        restarts.clear();
        last_key = "";
        counter = 0;
        return true;
    };

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        
        uint32_t shared = 0;
        if (counter % restart_interval != 0 && !block_buf.empty()) {
            size_t min_len = std::min(last_key.size(), entry.key.size());
            while (shared < min_len && last_key[shared] == entry.key[shared]) {
                shared++;
            }
        } else {
            shared = 0;
        }

        uint32_t unshared = static_cast<uint32_t>(entry.key.size()) - shared;
        uint32_t val_len = 16; // sizeof(VLogPointer)
        
        size_t entry_size = sizeof(uint32_t) * 3 + unshared + val_len;
        size_t estimated_trailer = (restarts.size() + (shared == 0 ? 1 : 0)) * 4 + 4;

        if (!block_buf.empty() && block_buf.size() + entry_size + estimated_trailer > block_limit) {
            index.push_back({ entries[i-1].key, current_offset, static_cast<uint32_t>(block_buf.size() + restarts.size() * 4 + 4) });
            if (!flush_block_buf()) return false;
            shared = 0;
            unshared = static_cast<uint32_t>(entry.key.size());
            entry_size = sizeof(uint32_t) * 3 + unshared + val_len;
        }

        if (shared == 0) {
            restarts.push_back(static_cast<uint32_t>(block_buf.size()));
        }

        size_t old = block_buf.size();
        block_buf.resize(old + entry_size);
        uint8_t* p = block_buf.data() + old;

        std::memcpy(p, &shared, 4);   p += 4;
        std::memcpy(p, &unshared, 4); p += 4;
        std::memcpy(p, &val_len, 4);  p += 4;
        std::memcpy(p, entry.key.data() + shared, unshared); p += unshared;
        std::memcpy(p, &entry.pointer.file_id, 4); p += 4;
        std::memcpy(p, &entry.pointer.offset, 8); p += 8;
        std::memcpy(p, &entry.pointer.length, 4); p += 4;

        last_key = entry.key;
        counter++;
    }

    if (!block_buf.empty()) {
        index.push_back({ entries.back().key, current_offset, static_cast<uint32_t>(block_buf.size() + restarts.size() * 4 + 4) });
        if (!flush_block_buf()) return false;
    }

    uint32_t index_offset = current_offset;
    for (const auto& idx : index) {
        uint32_t ks = static_cast<uint32_t>(idx.max_key.size());
        
        checksum = compute_crc32_incremental(checksum, reinterpret_cast<const uint8_t*>(&ks), 4);
        out.write(reinterpret_cast<const char*>(&ks), sizeof(uint32_t));
        
        checksum = compute_crc32_incremental(checksum, reinterpret_cast<const uint8_t*>(idx.max_key.data()), ks);
        out.write(idx.max_key.data(), ks);
        
        checksum = compute_crc32_incremental(checksum, reinterpret_cast<const uint8_t*>(&idx.offset), 4);
        out.write(reinterpret_cast<const char*>(&idx.offset), sizeof(uint32_t));
        
        checksum = compute_crc32_incremental(checksum, reinterpret_cast<const uint8_t*>(&idx.length), 4);
        out.write(reinterpret_cast<const char*>(&idx.length), sizeof(uint32_t));
    }
    if (out.fail()) return false;
    uint32_t index_end = static_cast<uint32_t>(out.tellp());
    
    std::vector<std::string> keys;
    keys.reserve(entries.size());
    for (const auto& entry : entries) keys.push_back(entry.key);
    
    BloomFilter bloom;
    bloom.build(keys, 0.01); 

    uint32_t bloom_offset = index_end;
    uint32_t k = bloom.num_hashes();
    
    checksum = compute_crc32_incremental(checksum, reinterpret_cast<const uint8_t*>(&k), 4);
    out.write(reinterpret_cast<const char*>(&k), 4);
    
    checksum = compute_crc32_incremental(checksum, bloom.data().data(), bloom.data().size());
    out.write(reinterpret_cast<const char*>(bloom.data().data()), static_cast<std::streamsize>(bloom.data().size()));
    if (out.fail()) return false;
    
    uint32_t bloom_size_total = 4 + static_cast<uint32_t>(bloom.data().size());

    out.write(reinterpret_cast<const char*>(&index_offset), sizeof(uint32_t));
    out.write(reinterpret_cast<const char*>(&bloom_offset), sizeof(uint32_t));
    out.write(reinterpret_cast<const char*>(&bloom_size_total), sizeof(uint32_t));
    out.write(reinterpret_cast<const char*>(&checksum), sizeof(uint32_t));
    
    out.flush();
    if (out.fail()) return false;
    out.close();

    return true;
}

// ── SSTableReader ──────────────────────────────────────────────

static uint32_t parse_sequence(const std::string& path) {
    auto pos = path.rfind("sst_");
    if (pos == std::string::npos) return 0;
    return static_cast<uint32_t>(std::strtoul(path.c_str() + pos + 4, nullptr, 10));
}

bool SSTableReader::load(const std::string& path) {
    path_ = path;
    sequence_ = parse_sequence(path);
    index_.clear();
    
#ifdef _WIN32
    hFile_ = CreateFileA(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile_ == INVALID_HANDLE_VALUE) {
        std::cerr << " [SSTable Error] CreateFileA failed for " << path_ << " err=" << GetLastError() << "\n";
        return false;
    }
    
    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile_, &liSize)) {
        std::cerr << " [SSTable Error] GetFileSizeEx failed for " << path_ << " err=" << GetLastError() << "\n";
        return false;
    }
    mapped_size_ = liSize.QuadPart;
    
    hMap_ = CreateFileMappingA(hFile_, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap_) {
        std::cerr << " [SSTable Error] CreateFileMappingA failed for " << path_ << " err=" << GetLastError() << "\n";
        return false;
    }
    
    mapped_data_ = reinterpret_cast<const uint8_t*>(MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, 0));
    if (!mapped_data_) {
        std::cerr << " [SSTable Error] MapViewOfFile failed for " << path_ << " err=" << GetLastError() << "\n";
        return false;
    }
    
    size_t file_size = mapped_size_;
    if (file_size < 16) return false;

    uint32_t footer[4];
    std::memcpy(footer, mapped_data_ + file_size - 16, 16);
    
    uint32_t index_offset     = footer[0];
    uint32_t bloom_offset     = footer[1];
    uint32_t bloom_size_total = footer[2];
    uint32_t stored_checksum  = footer[3];

    size_t payload_size = file_size - 16;
    
    uint32_t computed = compute_crc32(mapped_data_, payload_size);
    if (computed != stored_checksum) {
        std::cerr << "[SSTable] WARNING: checksum mismatch in " << path << " (computed " << computed << " vs stored " << stored_checksum << ")\n";
        return false;
    }

    size_t off = index_offset;
    while (off < bloom_offset) {
        uint32_t ks;
        std::memcpy(&ks, mapped_data_ + off, sizeof(uint32_t)); off += sizeof(uint32_t);
        std::string mx_key(reinterpret_cast<const char*>(mapped_data_ + off), ks); off += ks;
        uint32_t block_off, block_len;
        std::memcpy(&block_off, mapped_data_ + off, sizeof(uint32_t)); off += sizeof(uint32_t);
        std::memcpy(&block_len, mapped_data_ + off, sizeof(uint32_t)); off += sizeof(uint32_t);
        index_.push_back({mx_key, block_off, block_len});
    }

    if (!index_.empty()) {
        size_t first_off = index_[0].offset;
        uint32_t ks;
        std::memcpy(&ks, mapped_data_ + first_off + 4, sizeof(uint32_t)); 
        min_key_.assign(reinterpret_cast<const char*>(mapped_data_ + first_off + 12), ks); 
        max_key_ = index_.back().max_key;
    }

    if (bloom_size_total >= 4 && bloom_offset + 4 <= payload_size) {
        uint32_t k;
        std::memcpy(&k, mapped_data_ + bloom_offset, 4);
        uint32_t actual_bloom_size = bloom_size_total - 4;
        bloom_.load_raw(mapped_data_ + bloom_offset + 4, actual_bloom_size, k);
    }
    return true;
#else
    int flags = O_RDONLY;
    fd_ = sst_open(path_.c_str(), flags, 0);
    if (fd_ < 0) return false;

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;

    auto file_size = in.tellg();
    if (file_size < 16) return false;

    in.seekg(static_cast<std::streamoff>(file_size) - 16, std::ios::beg);
    uint32_t footer[4];
    in.read(reinterpret_cast<char*>(footer), 16);
    
    uint32_t index_offset     = footer[0];
    uint32_t bloom_offset     = footer[1];
    uint32_t bloom_size_total = footer[2];
    uint32_t stored_checksum  = footer[3];

    size_t payload_size = static_cast<size_t>(static_cast<std::streamoff>(file_size) - 16);
    in.seekg(0, std::ios::beg);
    
    uint32_t computed = 0;
    char c_buf[4096];
    size_t remaining = payload_size;
    while (remaining > 0) {
        size_t to_read = std::min(remaining, sizeof(c_buf));
        in.read(c_buf, to_read);
        if (in.gcount() == 0) break;
        computed = compute_crc32_incremental(computed, reinterpret_cast<const uint8_t*>(c_buf), in.gcount());
        remaining -= in.gcount();
    }
    if (computed != stored_checksum) {
        std::cerr << "[SSTable] WARNING: checksum mismatch in " << path << " (computed " << computed << " vs stored " << stored_checksum << ")\n";
        return false;
    }

    in.clear(); // Clear EOF flags
    in.seekg(index_offset, std::ios::beg);
    size_t off = index_offset;
    while (off < bloom_offset) {
        uint32_t ks;
        in.read(reinterpret_cast<char*>(&ks), sizeof(uint32_t)); off += sizeof(uint32_t);
        std::string mx_key(ks, '\0');
        in.read(&mx_key[0], ks); off += ks;
        uint32_t block_off, block_len;
        in.read(reinterpret_cast<char*>(&block_off), sizeof(uint32_t)); off += sizeof(uint32_t);
        in.read(reinterpret_cast<char*>(&block_len), sizeof(uint32_t)); off += sizeof(uint32_t);
        index_.push_back({mx_key, block_off, block_len});
    }

    if (!index_.empty()) {
        size_t first_off = index_[0].offset;
        in.seekg(first_off + 4, std::ios::beg);
        uint32_t ks;
        in.read(reinterpret_cast<char*>(&ks), sizeof(uint32_t));
        in.seekg(first_off + 12, std::ios::beg);
        min_key_.resize(ks);
        in.read(&min_key_[0], ks);
        max_key_ = index_.back().max_key;
    }

    if (bloom_size_total >= 4 && bloom_offset + 4 <= payload_size) {
        in.seekg(bloom_offset, std::ios::beg);
        uint32_t k;
        in.read(reinterpret_cast<char*>(&k), 4);
        uint32_t actual_bloom_size = bloom_size_total - 4;
        bloom_.load(path, bloom_offset + 4, actual_bloom_size, k);
    }
    return true;
#endif
}

SSTableReader::~SSTableReader() {
#ifdef _WIN32
    if (mapped_data_) UnmapViewOfFile(mapped_data_);
    if (hMap_) CloseHandle(hMap_);
    if (hFile_ != INVALID_HANDLE_VALUE) CloseHandle(hFile_);
#else
    if (fd_ >= 0) {
        sst_close(fd_);
    }
#endif
}

bool SSTableReader::get(std::string_view key, VLogPointer& out_pointer, forgelsm::ShardedLRUCache* cache, EngineMetrics* metrics) const {
    if (key < min_key_ || key > max_key_) return false;

    auto it = std::lower_bound(index_.begin(), index_.end(), key,
        [](const IndexEntry& e, std::string_view k) { return e.max_key < k; });

    if (it == index_.end()) return false;

#ifdef _WIN32
    (void)cache;
    (void)metrics;
    if (!mapped_data_ || it->offset + it->length > mapped_size_) return false;
    const char* block_data = reinterpret_cast<const char*>(mapped_data_ + it->offset);
    size_t block_size = it->length;
#else
    std::string cache_key = path_ + ":" + std::to_string(it->offset);
    forgelsm::BlockPtr block = cache->get(cache_key);

    if (block) {
        if (metrics) metrics->block_cache_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (metrics) metrics->block_cache_misses.fetch_add(1, std::memory_order_relaxed);
        block = std::make_shared<forgelsm::Block>(it->length);
        if (fd_ >= 0 && forgelsm::platform_pread(fd_, block->data(), it->length, it->offset)) {
            cache->put(cache_key, block);
        } else {
            return false;
        }
    }
    const char* block_data = block->data();
    size_t block_size = block->size();
#endif

    if (block_size < 4) return false;
    uint32_t num_restarts;
    std::memcpy(&num_restarts, block_data + block_size - 4, 4);
    if (num_restarts == 0 || num_restarts > (block_size - 4) / 4) return false;

    uint32_t restarts_offset = block_size - 4 - num_restarts * 4;
    
    int left = 0;
    int right = num_restarts - 1;
    int target_restart = 0;
    
    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (restarts_offset + mid * 4 + 4 > block_size) throw std::runtime_error("SSTable corrupted: restarts index out of bounds");
        uint32_t r_off;
        std::memcpy(&r_off, block_data + restarts_offset + mid * 4, 4);
        
        if (r_off + 12 > restarts_offset) throw std::runtime_error("SSTable corrupted: entry header out of bounds");
        
        uint32_t shared, unshared, val_len;
        const char* p = block_data + r_off;
        std::memcpy(&shared, p, 4); p += 4;
        std::memcpy(&unshared, p, 4); p += 4;
        std::memcpy(&val_len, p, 4); p += 4;
        
        if (r_off + 12 + unshared + val_len > restarts_offset) throw std::runtime_error("SSTable corrupted: entry payload out of bounds");
        
        std::string_view mid_key(p, unshared); 
        
        if (mid_key < key) {
            target_restart = mid;
            left = mid + 1;
        } else if (mid_key > key) {
            right = mid - 1;
        } else {
            p += unshared;
            if (val_len == 16) {
                std::memcpy(&out_pointer.file_id, p, 4);
                std::memcpy(&out_pointer.offset, p + 4, 8);
                std::memcpy(&out_pointer.length, p + 12, 4);
            }
            return true;
        }
    }

    if (restarts_offset + target_restart * 4 + 4 > block_size) throw std::runtime_error("SSTable corrupted");
    uint32_t scan_off;
    std::memcpy(&scan_off, block_data + restarts_offset + target_restart * 4, 4);
    if (scan_off > restarts_offset) throw std::runtime_error("SSTable corrupted: scan_off out of bounds");
    
    std::string current_key;
    const char* p = block_data + scan_off;
    
    while (p < block_data + restarts_offset) {
        if (p + 12 > block_data + restarts_offset) throw std::runtime_error("SSTable corrupted: linear scan header OOB");
        uint32_t shared, unshared, val_len;
        std::memcpy(&shared, p, 4); p += 4;
        std::memcpy(&unshared, p, 4); p += 4;
        std::memcpy(&val_len, p, 4); p += 4;
        
        if (p + unshared + val_len > block_data + restarts_offset) throw std::runtime_error("SSTable corrupted: linear scan payload OOB");
        
        current_key.resize(shared);
        current_key.append(p, unshared); p += unshared;
        
        VLogPointer ptr;
        if (val_len == 16) {
            std::memcpy(&ptr.file_id, p, 4);
            std::memcpy(&ptr.offset, p + 4, 8);
            std::memcpy(&ptr.length, p + 12, 4);
        }
        p += val_len;
        
        if (current_key == key) {
            out_pointer = ptr;
            return true;
        } else if (current_key > key) {
            return false;
        }
    }
    return false;
}
