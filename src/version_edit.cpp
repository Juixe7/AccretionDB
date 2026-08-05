#include "version_edit.h"
#include <cstring>

namespace forgelsm {

enum Tag : uint32_t {
    kNextFileSequence = 1,
    kDeletedFile = 2,
    kNewFile = 3
};

static void put_uint32(std::string& dst, uint32_t v) {
    dst.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

static void put_uint64(std::string& dst, uint64_t v) {
    dst.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

static void put_string(std::string& dst, const std::string& str) {
    put_uint32(dst, static_cast<uint32_t>(str.size()));
    dst.append(str);
}

static bool get_uint32(const char*& p, const char* limit, uint32_t& v) {
    if (p + sizeof(uint32_t) > limit) return false;
    std::memcpy(&v, p, sizeof(uint32_t));
    p += sizeof(uint32_t);
    return true;
}

static bool get_uint64(const char*& p, const char* limit, uint64_t& v) {
    if (p + sizeof(uint64_t) > limit) return false;
    std::memcpy(&v, p, sizeof(uint64_t));
    p += sizeof(uint64_t);
    return true;
}

static bool get_string(const char*& p, const char* limit, std::string& str) {
    uint32_t len;
    if (!get_uint32(p, limit, len)) return false;
    if (p + len > limit) return false;
    str.assign(p, len);
    p += len;
    return true;
}

void VersionEdit::encode_to(std::string& dst) const {
    if (has_next_file_sequence_) {
        put_uint32(dst, kNextFileSequence);
        put_uint32(dst, next_file_sequence_);
    }

    for (const auto& [level, seq] : deleted_files_) {
        put_uint32(dst, kDeletedFile);
        put_uint32(dst, static_cast<uint32_t>(level));
        put_uint32(dst, seq);
    }

    for (const auto& [level, meta] : new_files_) {
        put_uint32(dst, kNewFile);
        put_uint32(dst, static_cast<uint32_t>(level));
        put_uint32(dst, meta.sequence);
        put_uint64(dst, meta.file_size);
        put_string(dst, meta.min_key);
        put_string(dst, meta.max_key);
    }
}

bool VersionEdit::decode_from(const std::string& src) {
    new_files_.clear();
    deleted_files_.clear();
    has_next_file_sequence_ = false;

    const char* p = src.data();
    const char* limit = p + src.size();

    while (p < limit) {
        uint32_t tag;
        if (!get_uint32(p, limit, tag)) return false;

        switch (tag) {
            case kNextFileSequence: {
                if (!get_uint32(p, limit, next_file_sequence_)) return false;
                has_next_file_sequence_ = true;
                break;
            }
            case kDeletedFile: {
                uint32_t level, seq;
                if (!get_uint32(p, limit, level) || !get_uint32(p, limit, seq)) return false;
                deleted_files_.push_back({static_cast<int>(level), seq});
                break;
            }
            case kNewFile: {
                uint32_t level;
                FileMetaData meta;
                if (!get_uint32(p, limit, level) ||
                    !get_uint32(p, limit, meta.sequence) ||
                    !get_uint64(p, limit, meta.file_size) ||
                    !get_string(p, limit, meta.min_key) ||
                    !get_string(p, limit, meta.max_key)) {
                    return false;
                }
                new_files_.push_back({static_cast<int>(level), meta});
                break;
            }
            default:
                // Unknown tag, maybe from a newer version.
                return false;
        }
    }
    return true;
}

} // namespace forgelsm
