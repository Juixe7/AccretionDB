#include "version_set.h"
#include "fault_injection.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <algorithm>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #define m_open(path, flags, mode)  _open(path, flags, mode)
  #define m_write(fd, buf, len)      _write(fd, buf, static_cast<unsigned int>(len))
  #define m_close(fd)                _close(fd)
  #define m_fsync(fd)                _commit(fd)
  static constexpr int M_FLAGS = _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY;
  static constexpr int M_MODE  = _S_IREAD | _S_IWRITE;
  #ifndef EINTR
    #define EINTR 0
  #endif
#else
  #include <unistd.h>
  #include <fcntl.h>
  #define m_open(path, flags, mode)  open(path, flags, mode)
  #define m_write(fd, buf, len)      write(fd, buf, len)
  #define m_close(fd)                close(fd)
  #define m_fsync(fd)                fdatasync(fd)
  static constexpr int M_FLAGS = O_WRONLY | O_CREAT | O_APPEND;
  static constexpr int M_MODE  = 0644;
#endif

namespace forgelsm {

static bool write_all_m(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        auto written = m_write(fd, p, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        p += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

VersionSet::VersionSet(const std::string& db_name) 
    : db_name_(db_name), current_(std::make_shared<Version>()) {
    manifest_path_ = db_name_ + "/MANIFEST";
}

VersionSet::~VersionSet() {
    if (manifest_fd_ >= 0) {
        m_close(manifest_fd_);
        manifest_fd_ = -1;
    }
}

void VersionSet::append_version(std::shared_ptr<Version> v) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = v;
    active_versions_.push_back(v);
}

bool VersionSet::recover() {
    if (!std::filesystem::exists(manifest_path_)) {
        manifest_fd_ = m_open(manifest_path_.c_str(), M_FLAGS, M_MODE);
        return manifest_fd_ >= 0;
    }

    std::ifstream in(manifest_path_, std::ios::binary);
    if (!in.is_open()) return false;

    std::shared_ptr<Version> v = std::make_shared<Version>();
    
    uint32_t len;
    while (in.read(reinterpret_cast<char*>(&len), sizeof(len))) {
        if (len > 64 * 1024 * 1024) {
            std::cerr << "[VersionSet] WARNING: Corrupted tail in MANIFEST (length > 64MB). Truncating.\n";
            break;
        }
        std::string rec(len, '\0');
        if (!in.read(&rec[0], len)) {
            std::cerr << "[VersionSet] WARNING: Corrupted tail in MANIFEST ignored.\n";
            break;
        }
        
        VersionEdit edit;
        if (!edit.decode_from(rec)) {
            std::cerr << "[VersionSet] WARNING: Invalid VersionEdit record.\n";
            break;
        }
        
        if (edit.has_next_file_sequence()) {
            next_file_number_ = edit.next_file_sequence();
        }

        for (const auto& [level, seq] : edit.deleted_files()) {
            if (level < 0 || level > 1) continue;
            auto& f = v->files_[level];
            f.erase(std::remove_if(f.begin(), f.end(), [seq](const FileMetaData& meta) {
                return meta.sequence == seq;
            }), f.end());
        }

        for (const auto& [level, meta] : edit.new_files()) {
            if (level < 0 || level >= Version::MAX_LEVELS) continue;
            v->files_[level].push_back(meta);
        }
    }
    
    std::sort(v->files_[0].begin(), v->files_[0].end(), [](const FileMetaData& a, const FileMetaData& b) {
        return a.sequence > b.sequence; // Newest first
    });
    
    for (int i = 1; i < Version::MAX_LEVELS; ++i) {
        std::sort(v->files_[i].begin(), v->files_[i].end(), [](const FileMetaData& a, const FileMetaData& b) {
            return a.min_key < b.min_key;
        });
    }

    append_version(v);
    
    manifest_fd_ = m_open(manifest_path_.c_str(), M_FLAGS, M_MODE);
    if (manifest_fd_ >= 0) {
        manifest_file_size_ = std::filesystem::file_size(manifest_path_);
    }
    return manifest_fd_ >= 0;
}

bool VersionSet::log_and_apply(VersionEdit* edit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (edit->has_next_file_sequence()) {
        next_file_number_ = edit->next_file_sequence();
    } else {
        edit->set_next_file_sequence(next_file_number_);
    }

    std::string encoded;
    edit->encode_to(encoded);
    
    uint32_t len = static_cast<uint32_t>(encoded.size());
    std::string record;
    record.append(reinterpret_cast<const char*>(&len), sizeof(len));
    record.append(encoded);

    if (!write_all_m(manifest_fd_, record.data(), record.size())) return false;
    if (m_fsync(manifest_fd_) != 0) return false;
    manifest_file_size_ += record.size();

    FaultInjection::check("crash_during_manifest_update");

    auto current_v = current_;
    auto new_v = std::make_shared<Version>();
    
    for (int level = 0; level < Version::MAX_LEVELS; ++level) {
        for (const auto& f : current_v->files_[level]) {
            bool deleted = false;
            for (const auto& [del_level, seq] : edit->deleted_files()) {
                if (del_level == level && seq == f.sequence) {
                    deleted = true;
                    break;
                }
            }
            if (!deleted) {
                new_v->files_[level].push_back(f);
            }
        }
        
        for (const auto& [add_level, meta] : edit->new_files()) {
            if (add_level == level) {
                new_v->files_[level].push_back(meta);
            }
        }
    }
    
    std::sort(new_v->files_[0].begin(), new_v->files_[0].end(), [](const FileMetaData& a, const FileMetaData& b) {
        return a.sequence > b.sequence;
    });
    for (int i = 1; i < Version::MAX_LEVELS; ++i) {
        std::sort(new_v->files_[i].begin(), new_v->files_[i].end(), [](const FileMetaData& a, const FileMetaData& b) {
            return a.min_key < b.min_key;
        });
    }

    current_ = new_v;
    active_versions_.push_back(new_v);
    for (const auto& [level, meta] : edit->new_files()) {
        pending_outputs_.erase(meta.sequence);
    }

    if (manifest_file_size_ > 1024 * 1024) {
        std::string tmp_path = manifest_path_ + ".tmp";
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        int tmp_fd = m_open(tmp_path.c_str(), M_FLAGS, M_MODE);
        if (tmp_fd >= 0) {
            VersionEdit snap;
            snap.set_next_file_sequence(next_file_number_);
            for (int level = 0; level < Version::MAX_LEVELS; ++level) {
            if (!new_v->files_[level].empty()) {
                for (const auto& f : new_v->files_[level]) {
                    snap.add_file(level, f.sequence, f.file_size, f.min_key, f.max_key);
                }
            }
            }
            std::string enc;
            snap.encode_to(enc);
            uint32_t slen = enc.size();
            std::string srec;
            srec.append(reinterpret_cast<const char*>(&slen), sizeof(slen));
            srec.append(enc);
            
            if (write_all_m(tmp_fd, srec.data(), srec.size()) && m_fsync(tmp_fd) == 0) {
                m_close(tmp_fd);
                m_close(manifest_fd_);
                std::filesystem::rename(tmp_path, manifest_path_);
                manifest_fd_ = m_open(manifest_path_.c_str(), M_FLAGS, M_MODE);
                if (manifest_fd_ >= 0) {
                    manifest_file_size_ = srec.size();
                }
            } else {
                m_close(tmp_fd);
            }
        }
    }
    return true;
}

void VersionSet::purge_obsolete_files(const std::string& db_dir) {
    std::set<uint32_t> live_files;
    std::set<uint32_t> pending;
    uint32_t max_seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = pending_outputs_;
        max_seq = next_file_number_.load();
        auto it = active_versions_.begin();
        while (it != active_versions_.end()) {
            if (auto v = it->lock()) {
                for (int level = 0; level < Version::MAX_LEVELS; ++level) {
            if (v && !v->files_[level].empty()) {
                for (const auto& f : v->files_[level]) {
                        live_files.insert(f.sequence);
                    }
                }
                }
                ++it;
            } else {
                it = active_versions_.erase(it);
            }
        }
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(db_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        if (filename.rfind("sst_", 0) == 0 && filename.size() > 4) {
            try {
                uint32_t seq = std::stoul(filename.substr(4));
                if (live_files.find(seq) == live_files.end() && pending.find(seq) == pending.end() && seq < max_seq) {
                    std::filesystem::remove(entry.path(), ec);
                }
            } catch(...) {}
        }
    }
}

} // namespace forgelsm
