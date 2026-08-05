#ifndef FORGELSM_H
#define FORGELSM_H

#include "kvstore.h"
#include "vlog_gc.h"
#include <vector>

namespace forgelsm {

#include "options.h"

class Status {
    bool ok_;
    explicit Status(bool ok = true) : ok_(ok) {}
public:
    static Status OK() { return Status(true); }
    static Status NotFound() { return Status(false); }
    bool ok() const { return ok_; }
    std::string ToString() const { return ok_ ? "OK" : "NotFound"; }
};

struct EngineStats {
    uint64_t user_bytes_written = 0;
    uint64_t storage_bytes_written = 0;
    double write_amplification = 0.0;
    uint64_t total_puts = 0;
    uint64_t total_gets = 0;
    uint64_t total_deletes = 0;
    size_t memtable_entries = 0;
    size_t memtable_bytes = 0;
    std::vector<size_t> level_file_counts;
    bool compaction_active = false;
    bool gc_active = false;
    uint64_t bloom_filter_skips = 0;
    uint64_t sst_searches = 0;
    uint64_t vlog_reads = 0;
    uint64_t live_keys = 0;
    uint64_t tombstones = 0;
    uint64_t live_data_bytes = 0;
};

class DB {
public:
    static Status Open(const Options& opts, const std::string& path, DB** dbptr) {
        *dbptr = new DB(path, opts);
        return Status::OK();
    }

    Status Put(const std::string& key, const std::string& value) {
        KVStore::WriteOptions wopts;
        wopts.sync = opts_.sync_writes;
        store_->put(key, value, wopts);
        return Status::OK();
    }

    Status Get(const std::string& key, std::string* value) {
        if (store_->get(key, *value)) {
            return Status::OK();
        }
        return Status::NotFound();
    }

    Status Delete(const std::string& key) {
        KVStore::WriteOptions wopts;
        wopts.sync = opts_.sync_writes;
        store_->delete_key(key, wopts);
        return Status::OK();
    }

    void PutBatch(const std::vector<std::pair<std::string, std::string>>& records) {
        KVStore::WriteOptions wopts;
        wopts.sync = opts_.sync_writes;
        for (const auto& kv : records) {
            store_->put(kv.first, kv.second, wopts);
        }
    }

    Status Scan(const std::string& start_key, const std::string& end_key,
                std::vector<std::pair<std::string, std::string>>* results) {
        if (store_ && results) {
            store_->sync_active_vlog();
            store_->scan(start_key, end_key, *results);
        }
        return Status::OK();
    }

    Status ScanValues(const std::string& start_key, const std::string& end_key,
                std::vector<std::string>* results) {
        if (store_ && results) {
            store_->sync_active_vlog();
            store_->scan_values(start_key, end_key, *results);
        }
        return Status::OK();
    }

    EngineStats GetStats() const {
        EngineStats stats;
        if (store_) {
            stats.user_bytes_written = store_->metrics().user_bytes_written.load();
            stats.storage_bytes_written = store_->metrics().storage_bytes_written.load();
            if (stats.user_bytes_written > 0) {
                stats.write_amplification = (double)stats.storage_bytes_written / stats.user_bytes_written;
            }
            for (int i = 0; i < Version::MAX_LEVELS; ++i) {
                stats.level_file_counts.push_back(store_->level_file_count(i));
            }
            stats.bloom_filter_skips = store_->metrics().bloom_skips.load();
            stats.sst_searches = store_->metrics().sst_searches.load();
            stats.vlog_reads = store_->metrics().vlog_reads.load();
        }
        return stats;
    }

    void ForceFlush() {}
    void ForceCompaction(size_t) {}
    void ForceSync() {
        if (store_) store_->sync_active_vlog();
    }
    void ForceGC() {
        if (store_) {
            store_->sync_active_vlog();
            run_vlog_gc(store_);
            store_->sync_active_vlog();
        }
    }

    void Close() {
    }

    ~DB() {
        delete store_;
    }

private:
    DB(const std::string& path, const Options& opts) : opts_(opts) {
        store_ = new KVStore(path, opts_);
    }

    KVStore* store_;
    Options opts_;
};

} // namespace forgelsm

#endif // FORGELSM_H
