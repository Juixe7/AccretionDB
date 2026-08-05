#include "vlog.h"
#include "io_util.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <system_error>
#include <filesystem>
#include <vector>


// ── Platform abstraction ───────────────────────────────────────
#ifdef _WIN32
  #include <windows.h>
  #define VLOG_FD_TYPE HANDLE
  #define VLOG_INVALID_FD INVALID_HANDLE_VALUE
  inline HANDLE vlog_open(const char* p, int f, int) {
      DWORD access = GENERIC_READ;
      if (f & 1) access |= GENERIC_WRITE;
      DWORD creation = (f & 8) ? OPEN_ALWAYS : OPEN_EXISTING;
      HANDLE h = CreateFileA(p, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
      if (h != INVALID_HANDLE_VALUE && (f & 8)) {
          SetFilePointer(h, 0, NULL, FILE_END);
      }
      return h;
  }
  inline int vlog_write(HANDLE fd, const void* b, size_t n) {
      DWORD written;
      return WriteFile(fd, b, static_cast<DWORD>(n), &written, NULL) ? written : -1;
  }
  inline int vlog_read(HANDLE fd, void* b, size_t n) {
      DWORD read_bytes;
      return ReadFile(fd, b, static_cast<DWORD>(n), &read_bytes, NULL) ? read_bytes : -1;
  }
  inline int vlog_close(HANDLE fd) { return CloseHandle(fd) ? 0 : -1; }
  inline int vlog_fsync(HANDLE fd) { return FlushFileBuffers(fd) ? 0 : -1; }
  inline int64_t vlog_lseek(HANDLE fd, int64_t o, int w) {
      LARGE_INTEGER li;
      li.QuadPart = o;
      li.LowPart = SetFilePointer(fd, li.LowPart, &li.HighPart, w == 0 ? FILE_BEGIN : (w == 1 ? FILE_CURRENT : FILE_END));
      return li.QuadPart;
  }
  static constexpr int VLOG_APPEND_FLAGS = 1 | 8;
  static constexpr int VLOG_READ_FLAGS   = 2;
  static constexpr int VLOG_MODE         = 0;
  #ifndef EINTR
    #define EINTR 0
  #endif
#else
  #include <unistd.h>
  #include <fcntl.h>
  #define VLOG_FD_TYPE int
  #define VLOG_INVALID_FD -1
  #define vlog_open(p, f, m)    open(p, f, m)
  #define vlog_write(fd, b, n)  write((int)(intptr_t)(fd), b, n)
  #define vlog_read(fd, b, n)   read((int)(intptr_t)(fd), b, n)
  #define vlog_close(fd)        close((int)(intptr_t)(fd))
  #define vlog_fsync(fd)        fdatasync((int)(intptr_t)(fd))
  #define vlog_lseek(fd, o, w)  lseek((int)(intptr_t)(fd), o, w)
  static constexpr int VLOG_APPEND_FLAGS = O_WRONLY | O_APPEND | O_CREAT;
  static constexpr int VLOG_READ_FLAGS   = O_RDONLY;
  static constexpr int VLOG_MODE         = 0644;
#endif

// ── Helpers ────────────────────────────────────────────────────

static bool vlog_write_all(intptr_t fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t rem = len;
    while (rem > 0) {
        auto n = vlog_write((VLOG_FD_TYPE)fd, p, rem);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        
        p   += n;
        rem -= static_cast<size_t>(n);
    }
    return true;
}

// ── VLog implementation ────────────────────────────────────────

size_t VLog::MAX_FILE_SIZE = 4 * 1024 * 1024; // 4 MB default

VLog::VLog(const std::string& path, uint32_t file_id)
    : path_(path), write_fd_((intptr_t)VLOG_INVALID_FD), read_fd_((intptr_t)VLOG_INVALID_FD), file_id_(file_id), current_offset_(0) {
    write_fd_ = (intptr_t)vlog_open(path_.c_str(), VLOG_APPEND_FLAGS, VLOG_MODE);
    if ((VLOG_FD_TYPE)write_fd_ == VLOG_INVALID_FD) {
        std::cerr << "[VLog] FATAL: cannot open write fd: " << path_ << " error: " << errno << " (" << std::strerror(errno) << ")\n";
        std::exit(1);
    }

    read_fd_ = (intptr_t)vlog_open(path_.c_str(), VLOG_READ_FLAGS, 0);
    if ((VLOG_FD_TYPE)read_fd_ == VLOG_INVALID_FD) {
        std::cerr << "[VLog] FATAL: cannot open read fd: " << path_ << " error: " << errno << " (" << std::strerror(errno) << ")\n";
        std::exit(1);
    }

    // Initialize current_offset_ from file size (one-time lseek, NOT used per-append).
    auto size = vlog_lseek((VLOG_FD_TYPE)write_fd_, 0, SEEK_END);
    current_offset_ = (size > 0) ? static_cast<uint64_t>(size) : 0;
    buffer_.reserve(8 * 1024 * 1024); // Preallocate 8MB
}

void VLog::close_files() {
    if ((VLOG_FD_TYPE)write_fd_ != VLOG_INVALID_FD) {
        sync();
        vlog_close((VLOG_FD_TYPE)write_fd_);
        write_fd_ = (intptr_t)VLOG_INVALID_FD;
    }
    if ((VLOG_FD_TYPE)read_fd_ != VLOG_INVALID_FD) {
        vlog_close((VLOG_FD_TYPE)read_fd_);
        read_fd_ = (intptr_t)VLOG_INVALID_FD;
    }
}

VLog::~VLog() {
    close_files();
    
    if (marked_for_deletion_) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        std::filesystem::remove(path_ + ".dead", ec);
    }
}

bool VLog::append(std::string_view key, std::string_view value, VLogPointer& out_pointer) {
    uint32_t key_size = static_cast<uint32_t>(key.size());
    uint32_t value_size = static_cast<uint32_t>(value.size());

    // Serialize: [key_size][value_size][key_bytes][value_bytes]
    uint32_t header_size = sizeof(uint32_t) * 2;
    size_t record_len = header_size + key_size + value_size;

    uint64_t write_offset = current_offset_ + buffer_.size();

    size_t off = buffer_.size();
    buffer_.resize(off + record_len);
    std::memcpy(buffer_.data() + off, &key_size, sizeof(uint32_t)); off += sizeof(uint32_t);
    std::memcpy(buffer_.data() + off, &value_size, sizeof(uint32_t)); off += sizeof(uint32_t);
    if (key_size > 0) {
        std::memcpy(buffer_.data() + off, key.data(), key_size);
        off += key_size;
    }
    if (value_size > 0) {
        std::memcpy(buffer_.data() + off, value.data(), value_size);
    }

    out_pointer.file_id = file_id_;
    out_pointer.offset  = write_offset + header_size + key_size;
    out_pointer.length  = value_size;
    
    if (buffer_.size() >= 4 * 1024 * 1024) {
        sync();
    }

    return true;
}

bool VLog::sync() {
    if (!buffer_.empty()) {


        
        {

            if (!vlog_write_all(write_fd_, buffer_.data(), buffer_.size())) return false;
        }
        current_offset_ += buffer_.size();
        buffer_.clear();
    }
    

    
    {

        if (vlog_fsync((VLOG_FD_TYPE)write_fd_) != 0) {
            std::cerr << "[VLog] ERROR: fsync failed (errno=" << errno << ")\n";
            return false;
        }
    }
    return true;
}

// Read value at pointer.
// Safely implemented using concurrent lock-free reads.
bool VLog::read_at(const VLogPointer& pointer, std::string& out_value) const {
    intptr_t fd = (write_fd_ != (intptr_t)VLOG_INVALID_FD) ? write_fd_ : read_fd_;
    if (fd == (intptr_t)VLOG_INVALID_FD) return false;
    
    out_value.resize(pointer.length);
    if (pointer.length > 0 && !forgelsm::platform_pread(fd, out_value.data(), pointer.length, pointer.offset))
        return false;

    return true;
}

bool VLog::read_next(uint64_t& current_offset, VLogRecord& out_record) const {
    uint32_t headers[2] = {0, 0};
    if (!forgelsm::platform_pread(read_fd_, headers, sizeof(headers), current_offset)) {
        return false; // EOF or error
    }
    
    uint32_t key_size = headers[0];
    uint32_t value_size = headers[1];
    
    out_record.key.resize(key_size);
    out_record.value.resize(value_size);
    
    if (key_size > 0 && !forgelsm::platform_pread(read_fd_, out_record.key.data(), key_size, current_offset + sizeof(uint32_t) * 2)) {
        return false;
    }
    
    if (value_size > 0 && !forgelsm::platform_pread(read_fd_, out_record.value.data(), value_size, current_offset + sizeof(uint32_t) * 2 + key_size)) {
        return false;
    }
    
    out_record.pointer.file_id = file_id_;
    out_record.pointer.offset = current_offset + sizeof(uint32_t) * 2 + key_size;
    out_record.pointer.length = value_size;
    
    current_offset += sizeof(uint32_t) * 2 + key_size + value_size;
    return true;
}
