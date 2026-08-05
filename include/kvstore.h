#ifndef FORGELSM_KVSTORE_H
#define FORGELSM_KVSTORE_H

#include "wal.h"
#include "vlog.h"
#include "memtable.h"
#include "sstable.h"
#include "version_set.h"
#include "cache.h"

#include <memory>
#include <string>
#include <string_view>
#include <list>
#include <vector>
#include <stdexcept>
#include <limits>
#include <shared_mutex>
#include <deque>
#include "options.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <map>
#include "thread_pool.h"

// Tombstone helper
inline bool is_tombstone(const VLogPointer& ptr) {
    return ptr.length == 0 && ptr.offset == std::numeric_limits<uint64_t>::max();
}

struct EngineMetrics {
    std::atomic<uint64_t> user_bytes_written{0};
    std::atomic<uint64_t> storage_bytes_written{0};
    std::atomic<uint64_t> get_calls{0};
    std::atomic<uint64_t> sst_considered{0};
    std::atomic<uint64_t> bloom_skips{0};
    std::atomic<uint64_t> sst_searches{0};
    std::atomic<uint64_t> vlog_reads{0};
    std::atomic<uint64_t> block_cache_hits{0};
    std::atomic<uint64_t> block_cache_misses{0};
    std::atomic<uint64_t> compaction_count{0};
    std::atomic<uint64_t> compaction_duration_ms{0};
    std::atomic<uint64_t> p99_latency_us{0};

    EngineMetrics() = default;
    EngineMetrics(const EngineMetrics& other) {
        user_bytes_written = other.user_bytes_written.load();
        storage_bytes_written = other.storage_bytes_written.load();
        get_calls = other.get_calls.load();
        sst_considered = other.sst_considered.load();
        bloom_skips = other.bloom_skips.load();
        sst_searches = other.sst_searches.load();
        vlog_reads = other.vlog_reads.load();
        block_cache_hits = other.block_cache_hits.load();
        block_cache_misses = other.block_cache_misses.load();
        compaction_count = other.compaction_count.load();
        compaction_duration_ms = other.compaction_duration_ms.load();
        p99_latency_us = other.p99_latency_us.load();
    }
    EngineMetrics& operator=(const EngineMetrics& other) {
        if (this != &other) {
            user_bytes_written = other.user_bytes_written.load();
            storage_bytes_written = other.storage_bytes_written.load();
            get_calls = other.get_calls.load();
            sst_considered = other.sst_considered.load();
            bloom_skips = other.bloom_skips.load();
            sst_searches = other.sst_searches.load();
            vlog_reads = other.vlog_reads.load();
            block_cache_hits = other.block_cache_hits.load();
            block_cache_misses = other.block_cache_misses.load();
            compaction_count = other.compaction_count.load();
            compaction_duration_ms = other.compaction_duration_ms.load();
            p99_latency_us = other.p99_latency_us.load();
        }
        return *this;
    }

    void reset() {
        user_bytes_written = 0;
        storage_bytes_written = 0;
        get_calls = 0;
        sst_considered = 0;
        bloom_skips = 0;
        sst_searches.store(0);
        vlog_reads.store(0);
        block_cache_hits.store(0);
        block_cache_misses.store(0);
        compaction_count.store(0);
        compaction_duration_ms.store(0);
        p99_latency_us.store(0);
    }
};

// KVStore — engine core (Phase 2).
//
// Write path (strict order):
//   1. Append to VLog (get VLogPointer)
//   2. Append to WAL (including VLogPointer)
//   3. Sync VLog (flush to OS)
//   4. Sync WAL (durable commit)
//   5. Insert into Memtable (makes it visible)
//
// Read path:
//   active memtable → immutable memtable → SSTables (newest-first) → VLog read
//
// WAL files: wal_NNNNNN.log (monotonically increasing).
// Rotation: create new WAL → fsync → switch → delete old (I19 safe).
class KVStore {
public:
    enum WriterState : uint8_t {
        STATE_INIT = 0,
        STATE_GROUP_LEADER = 1,
        STATE_COMPLETED = 2
    };

    struct WriteOptions {
        bool sync;
        WriteOptions() : sync(true) {}
    };

    struct WriteRequest {
        std::string                  key;
        std::string                  value;
        bool                         sync{true};
        bool                         is_delete{false};
        bool                         is_gc{false};
        uint32_t                     gc_old_vlog_id{0};
        
        std::atomic<uint8_t>         state{STATE_INIT};
        std::exception_ptr           error;

        WriteRequest*                link_older{nullptr};
        WriteRequest*                link_newer{nullptr};

        std::mutex                   state_mutex;
        std::condition_variable      state_cv;
        std::atomic<bool>            made_waitable{false};
    };
    explicit KVStore(const std::string& data_dir, const forgelsm::Options& opts = forgelsm::Options());
    ~KVStore();

    void put(std::string_view key, std::string_view value, const WriteOptions& options = WriteOptions());
    void delete_key(std::string_view key, const WriteOptions& options = WriteOptions());
    bool get(std::string_view key, std::string& out_value) const;
    bool get_pointer(std::string_view key, VLogPointer& out_ptr) const;

    size_t active_byte_size() const { 
        return active()->byte_size(); 
    }

    bool   wal_tainted() const;
    size_t memtable_entries() const;

    EngineMetrics& metrics() { return metrics_; }
    const EngineMetrics& metrics() const { return metrics_; }

    void add_storage_bytes(uint64_t bytes) { metrics_.storage_bytes_written += bytes; }
    void add_user_bytes(uint64_t bytes) { metrics_.user_bytes_written += bytes; }
    void subtract_user_bytes(uint64_t bytes) { metrics_.user_bytes_written -= bytes; }

    // ── Observability accessors (used by HttpServer) ──────────────
    size_t level_file_count(int level) const {
        if (!versions_ || level >= forgelsm::Version::MAX_LEVELS) return 0;
        return versions_->current()->files_[level].size();
    }
    size_t l0_size() const { 
        if (!versions_) return 0;
        size_t s = 0;
        for (const auto& f : versions_->current()->files_[0]) s += f.file_size;
        return s;
    }
    size_t l1_size() const { 
        if (!versions_) return 0;
        size_t s = 0;
        for (const auto& f : versions_->current()->files_[1]) s += f.file_size;
        return s;
    }
    size_t l0_hard_limit()       const { return L0_HARD_LIMIT; }
    
    // Test helper to explicitly disable bloom filter and evaluate invariant equivalence
    void bypass_bloom(bool bypass) { disable_bloom_ = bypass; }
    void sync_active_vlog();
    void scan(const std::string& start_key, const std::string& end_key, std::vector<std::pair<std::string, std::string>>& results) const;
    void scan_values(const std::string& start_key, const std::string& end_key, std::vector<std::string>& results) const;

private:
    void     recover();
    void     load_sstables();
    void     scan_wal_files(std::vector<std::string>& paths, uint32_t& max_id) const;
    void     execute_write_request(WriteRequest* req);
    bool     link_one(WriteRequest* w);
    void create_missing_newer_links(WriteRequest* head, WriteRequest* stop_at);
    uint8_t  await_state(WriteRequest* w, uint8_t goal_mask);
    void     set_state(WriteRequest* w, uint8_t new_state);
    
    void     maybe_flush();
    void     flush_immutable(std::shared_ptr<Memtable> imm);
    void     rotate_wal();
    void     bg_compaction_worker();
    uint32_t next_sst_sequence() const;

    std::string manifest_path() const;

    std::string wal_path(uint32_t id) const;
    std::string vlog_path(uint32_t id) const;
    std::string sst_path(uint32_t seq) const;
    const std::string& data_dir() const { return data_dir_; }

    std::string                  data_dir_;
    mutable EngineMetrics        metrics_;
    mutable std::mutex           wal_mutex_;
    std::unique_ptr<WAL>         wal_;
    mutable forgelsm::ShardedLRUCache block_cache_{64 * 1024 * 1024}; // 64MB Cache
    
    mutable std::map<uint32_t, std::shared_ptr<VLog>> vlogs_;
    mutable std::mutex          vlogs_mutex_;
    uint32_t                                  current_vlog_id_ = 1;
    std::vector<uint32_t>                     pending_gc_vlogs_;

    mutable std::mutex    memtable_mutex_;
    std::shared_ptr<Memtable>    active_{nullptr};
    std::shared_ptr<Memtable>    immutables_[4];
    std::atomic<uint64_t>        current_imm_idx_{0};

    std::shared_ptr<Memtable> active() const {
        std::lock_guard<std::mutex> lk(memtable_mutex_);
        return active_;
    }
    std::shared_ptr<Memtable> immutable(int i) const {
        std::lock_guard<std::mutex> lk(memtable_mutex_);
        return immutables_[i];
    }
    
    // Global panic state
    std::atomic<bool>            is_panic_{false};

    std::atomic<WriteRequest*> write_queue_{nullptr};
    std::mutex commit_mutex_;
    std::mutex                   flush_wait_mutex_;

    std::condition_variable      bg_flush_cv_;
    std::condition_variable      bg_compaction_cv_;
    std::atomic<bool>            bg_compaction_running_{false};
    std::atomic<bool>            bg_flush_running_{false};

    std::unique_ptr<forgelsm::VersionSet> versions_;
    
    // Table cache to hold loaded SSTableReaders
    mutable std::list<uint32_t> table_cache_order_;
    mutable std::map<uint32_t, std::pair<std::shared_ptr<SSTableReader>, std::list<uint32_t>::iterator>> table_cache_;
    mutable std::mutex          table_cache_mutex_;

    uint32_t                     current_wal_id_ = 1;
    bool                         disable_bloom_ = false;

    forgelsm::ThreadPool             flush_pool_{1};
    forgelsm::ThreadPool             compaction_pool_{2}; 

public:
    const forgelsm::Options& opts() const { return opts_; }
    size_t flush_threshold_bytes() const { return opts_.flush_threshold > 0 ? opts_.flush_threshold : 1024 * 1024; }
private:
    forgelsm::Options opts_;
    static constexpr size_t L0_HARD_LIMIT   = 15;

    std::shared_ptr<SSTableReader> get_sstable_reader(uint32_t seq) const;

    friend void run_compaction(KVStore* store);
    friend void run_vlog_gc(KVStore* store);
};

#endif // FORGELSM_KVSTORE_H


