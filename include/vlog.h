#ifndef FORGELSM_VLOG_H
#define FORGELSM_VLOG_H

#include <vector>
#include <cstdint>
#include <string>
#include <string_view>

// Pointer to a value stored in the Value Log.
struct VLogPointer {
    uint32_t file_id;   // 0 in Phase 2 (single file)
    uint64_t offset;    // byte offset of record start (key_size field)
    uint32_t length;    // value bytes (excluding headers and key)
};

struct VLogRecord {
    std::string key;
    std::string value;
    VLogPointer pointer;
};

// Append-only Value Log for WiscKey key-value separation.
//
// Record format: [uint32_t key_size][uint32_t value_size][key_bytes][value_bytes]
//
// Offset is tracked via an internal current_offset_ variable (user-space).
// NEVER derived from lseek on the file descriptor.
class VLog {
public:
    explicit VLog(const std::string& path, uint32_t file_id = 0);
    ~VLog();

    VLog(const VLog&) = delete;
    VLog& operator=(const VLog&) = delete;

    uint32_t file_id() const { return file_id_; }
    uint64_t current_offset() const { return current_offset_; }

    // Append value, return pointer. Returns false on I/O error.
    bool append(std::string_view key, std::string_view value, VLogPointer& out_pointer);

    void mark_for_deletion() { marked_for_deletion_ = true; }

    bool is_gc_skipped() const { return gc_skipped_; }
    void set_gc_skipped(bool skipped) { gc_skipped_ = skipped; }

    // Flush to stable storage. Returns false on error.
    bool sync();
    void close_files();

    // Read value at pointer. Returns false on error.
    bool read_at(const VLogPointer& pointer, std::string& out_value) const;

    // Read the next record sequentially. Returns false on EOF or error.
    // Advances current_offset internally.
    bool read_next(uint64_t& current_offset, VLogRecord& out_record) const;

public:
    static size_t MAX_FILE_SIZE;

private:
    std::string path_;
    std::vector<uint8_t> buffer_;
    intptr_t    write_fd_;         // persistent fd (append mode)
    intptr_t    read_fd_;          // persistent fd (read-only)
    uint32_t    file_id_;
    uint64_t    current_offset_;   // user-space offset tracking
    bool        marked_for_deletion_ = false;
    bool        gc_skipped_ = false;
};

#endif // FORGELSM_VLOG_H
