#ifndef FORGELSM_VERSION_SET_H
#define FORGELSM_VERSION_SET_H

#include "version_edit.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <set>

namespace forgelsm {

class Version {
public:
    static const int MAX_LEVELS = 6;
    std::vector<FileMetaData> files_[MAX_LEVELS]; // Levels 0 through 5
};

class VersionSet {
public:
    explicit VersionSet(const std::string& db_name);
    ~VersionSet();

    bool recover();
    bool log_and_apply(VersionEdit* edit);

    std::shared_ptr<Version> current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    uint32_t new_file_number() {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t seq = ++next_file_number_;
        pending_outputs_.insert(seq);
        return seq;
    }
    
    void remove_pending_output(uint32_t seq) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_outputs_.erase(seq);
    }
    
    uint32_t next_file_number() const {
        return next_file_number_.load();
    }

    const std::string& db_name() const { return db_name_; }

    void purge_obsolete_files(const std::string& db_dir);

private:
    std::string db_name_;
    std::atomic<uint32_t> next_file_number_{1};
    std::shared_ptr<Version> current_;
    mutable std::vector<std::weak_ptr<Version>> active_versions_;
    std::set<uint32_t> pending_outputs_;

    mutable std::mutex mutex_;
    int manifest_fd_ = -1;
    std::string manifest_path_;
    size_t manifest_file_size_ = 0;

    void append_version(std::shared_ptr<Version> v);
};

} // namespace forgelsm

#endif // FORGELSM_VERSION_SET_H
