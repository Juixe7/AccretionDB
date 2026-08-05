#include "compaction.h"
#include "fault_injection.h"
#include "kvstore.h"
#include "sstable.h"
#include "sstable_iterator.h"
#include "version_edit.h"
#include "version_set.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
#include <queue>
#include <memory>
#include <chrono>

void run_compaction(KVStore* store) {
    auto start_time = std::chrono::high_resolution_clock::now();
    auto current_v = store->versions_->current();
    
    int source_level = -1;
    double best_score = -1.0;
    
    // Check L0 score
    size_t l0_trigger = store->opts().l0_compaction_trigger > 0 ? store->opts().l0_compaction_trigger : 4;
    double l0_score = (double)current_v->files_[0].size() / l0_trigger;
    if (l0_score >= 1.0) {
        best_score = l0_score;
        source_level = 0;
    }
    
    // Check other levels
    size_t limit = store->flush_threshold_bytes() * store->opts().level_size_multiplier;
    for (int i = 1; i < forgelsm::Version::MAX_LEVELS - 1; ++i) {
        size_t current_size = 0;
        for (const auto& meta : current_v->files_[i]) current_size += meta.file_size;
        
        double score = (double)current_size / limit;
        if (score >= 1.0 && score > best_score) {
            best_score = score;
            source_level = i;
        }
        limit *= store->opts().level_size_multiplier;
    }

    if (source_level == -1) return;

    int target_level = source_level + 1;
    if (target_level >= forgelsm::Version::MAX_LEVELS) return;

    std::vector<forgelsm::FileMetaData> src_inputs;
    if (source_level == 0) {
        src_inputs = current_v->files_[0]; // Pick all of L0
    } else {
        src_inputs.push_back(current_v->files_[source_level][0]); // Pick one file to cascade down
    }

    std::string global_min = "\xFF", global_max = "";
    for (const auto& meta : src_inputs) {
        if (meta.min_key < global_min) global_min = meta.min_key;
        if (meta.max_key > global_max) global_max = meta.max_key;
    }

    auto get_sstable_reader = [&](uint32_t seq) -> std::shared_ptr<SSTableReader> {
        return store->get_sstable_reader(seq);
    };

    std::vector<forgelsm::FileMetaData> target_inputs;
    for (const auto& meta : current_v->files_[target_level]) {
        auto r = get_sstable_reader(meta.sequence);
        if (!r) continue;
        if (r->overlaps(global_min, global_max)) {
            target_inputs.push_back(meta);
        }
    }

    struct IteratorWrapper {
        SSTableReader* reader;
        SSTableIterator* iter;
        int level;
        uint32_t seq;
    };

    struct CompareIter {
        bool operator()(const IteratorWrapper& a, const IteratorWrapper& b) const {
            std::string_view k1 = a.iter->key();
            std::string_view k2 = b.iter->key();
            if (k1 != k2) return k1 > k2;
            if (a.level != b.level) return a.level > b.level;
            return a.seq < b.seq; 
        }
    };

    std::vector<std::unique_ptr<SSTableIterator>> all_iters;
    std::priority_queue<IteratorWrapper, std::vector<IteratorWrapper>, CompareIter> pq;
    std::vector<std::shared_ptr<SSTableReader>> active_readers;

    for (const auto& meta : target_inputs) {
        auto r = get_sstable_reader(meta.sequence);
        if (!r) continue;
        auto iter = std::make_unique<SSTableIterator>(r.get(), &store->block_cache_);
        if (iter->valid()) {
            pq.push({r.get(), iter.get(), target_level, meta.sequence});
            all_iters.push_back(std::move(iter));
            active_readers.push_back(std::move(r));
        }
    }

    for (const auto& meta : src_inputs) {
        auto r = get_sstable_reader(meta.sequence);
        if (!r) continue;
        auto iter = std::make_unique<SSTableIterator>(r.get(), &store->block_cache_);
        if (iter->valid()) {
            pq.push({r.get(), iter.get(), source_level, meta.sequence});
            all_iters.push_back(std::move(iter));
            active_readers.push_back(std::move(r));
        }
    }

    forgelsm::VersionEdit edit;
    
    for (const auto& meta : src_inputs) edit.delete_file(source_level, meta.sequence);
    for (const auto& meta : target_inputs) edit.delete_file(target_level, meta.sequence);

    std::vector<SSTableEntry> chunk;
    size_t chunk_size = 0;

    auto flush_chunk = [&]() {
        if (chunk.empty()) return;
        uint32_t seq = store->versions_->new_file_number();
        std::string path = store->sst_path(seq);
        if (!SSTableWriter::write(path, chunk)) {
            store->versions_->remove_pending_output(seq);
            throw std::runtime_error("[Compaction] Failed to write new SSTable");
        }
        store->add_storage_bytes(24);
        
        size_t est_size = chunk_size + 24; 
        edit.add_file(target_level, seq, est_size, chunk[0].key, chunk.back().key);
        
        chunk.clear();
        chunk_size = 0;
    };

    std::string last_key = "";
    bool first_key = true;

    while (!pq.empty()) {
        auto top = pq.top();
        pq.pop();

        std::string current_key(top.iter->key());
        VLogPointer current_val = top.iter->value();

        top.iter->next();
        if (top.iter->valid()) {
            pq.push(top);
        }

        if (!first_key && current_key == last_key) continue;
        last_key = current_key;
        first_key = false;

        // If target_level is MAX_LEVELS - 1, we can safely drop tombstones.
        if (is_tombstone(current_val) && target_level == forgelsm::Version::MAX_LEVELS - 1) continue;

        chunk.push_back({current_key, current_val});
        chunk_size += current_key.size() + 20;
        store->add_storage_bytes(current_key.size() + 20);
        if (chunk_size >= store->flush_threshold_bytes()) flush_chunk();
    }
    flush_chunk();

    while (!pq.empty()) pq.pop();
    all_iters.clear();
    active_readers.clear();

    FaultInjection::check("crash_during_compaction");
    if (!store->versions_->log_and_apply(&edit)) {
        throw std::runtime_error("[Compaction] VersionSet commit failed");
    }
    
    store->versions_->purge_obsolete_files(store->data_dir());

    auto end_time = std::chrono::high_resolution_clock::now();
    uint64_t duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    store->metrics().compaction_duration_ms.fetch_add(duration, std::memory_order_relaxed);
    store->metrics().compaction_count.fetch_add(1, std::memory_order_relaxed);
}
