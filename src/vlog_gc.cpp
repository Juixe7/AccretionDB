#include "vlog_gc.h"
#include "fault_injection.h"
#include "kvstore.h"
#include "sstable_iterator.h"

#include <filesystem>
#include <iostream>
#include <vector>
#include <set>

// Definition for run_vlog_gc
void run_vlog_gc(KVStore* store) {
    if (!store) return;

    uint32_t gc_target_id = 0;
    std::shared_ptr<VLog> old_vlog;
    {
        std::lock_guard<std::mutex> lk(store->vlogs_mutex_);
        if (store->vlogs_.size() > 1) {
            for (auto it = store->vlogs_.begin(); it != store->vlogs_.end(); ++it) {
                if (it->first < store->current_vlog_id_) {
                    gc_target_id = it->first;
                    old_vlog = it->second;
                    break; // Always target the oldest file
                }
            }
        }
    }

    if (gc_target_id == 0 || !old_vlog) return; // Nothing to GC

    size_t live_bytes = 0;
    size_t total_bytes = 0;
    uint64_t offset = 0;
    VLogRecord record;
    std::vector<VLogRecord> live_records;

    // Stream the old VLog sequentially
    while (old_vlog->read_next(offset, record)) {
        size_t record_size = record.key.size() + record.value.size();
        total_bytes += record_size;

        VLogPointer current_ptr;
        bool is_live = store->get_pointer(record.key, current_ptr);

        // A value is live if the key exists AND the pointer points to the exact same location
        if (is_live && current_ptr.file_id == gc_target_id && current_ptr.offset == record.pointer.offset) {
            live_bytes += record_size;
            live_records.push_back(std::move(record));
        }
    }

    // Heuristic: If > 50% of the file is still live, abort the GC to save write amplification
    if (total_bytes > 0 && (double)live_bytes / total_bytes > 0.5) {
        std::cout << "[VLog GC] Skipped VLog " << gc_target_id 
                  << " (Live: " << live_bytes << "/" << total_bytes << " bytes, > 50%)\n";
        return;
    }

    // If we passed the heuristic, we commit to the rewrite!
    size_t rewritten = 0;
    for (const auto& rec : live_records) {
        KVStore::WriteRequest gc_req;
        gc_req.key = rec.key;
        gc_req.value = rec.value;
        gc_req.is_gc = true;
        gc_req.gc_old_vlog_id = gc_target_id;
        
        store->execute_write_request(&gc_req);
        
        if (!gc_req.error) {
            store->subtract_user_bytes(rec.key.size() + rec.value.size());
            rewritten++;
        } else {
            std::cerr << "[VLog GC] WARNING: failed to rewrite live pointer for key " << rec.key << "\n";
        }
    }

    FaultInjection::check("crash_during_vlog_rewrite");

    // Close open handles, remove from store map, and delete file
    old_vlog->close_files();
    {
        std::lock_guard<std::mutex> lk(store->vlogs_mutex_);
        store->vlogs_.erase(gc_target_id);
    }
    std::string path_to_del = store->vlog_path(gc_target_id);
    old_vlog.reset();

    std::error_code ec;
    std::filesystem::remove(path_to_del, ec);
    std::filesystem::remove(path_to_del + ".dead", ec);

    std::cout << "[VLog GC] Rewrote " << rewritten << " live values (" << live_bytes << "/" << total_bytes 
              << " bytes) and deleted old VLog " << gc_target_id << " from disk.\n";
}

