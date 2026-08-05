#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <random>

using namespace forgelsm;

const std::string DB_PATH = "flsm_gc_waf_bench";

void run_gc_waf_test(int value_size) {
    std::filesystem::remove_all(DB_PATH);

    Options opts;
    opts.sync_writes = false; 
    opts.vlog_shards = 4;
    opts.flush_threshold = 4 * 1024 * 1024;
    opts.background_gc = true; // CRITICAL: Enable real-world background GC!
    VLog::MAX_FILE_SIZE = 8 * 1024 * 1024; // 8MB per VLog file

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << "\n";
        return;
    }

    long long total_db_size = 200LL * 1024 * 1024; // 200 MB baseline
    long long total_overwrite_size = 2000LL * 1024 * 1024; // 2 GB of overwrites

    const int NUM_KEYS = total_db_size / value_size;
    const int OVERWRITES = total_overwrite_size / value_size;
    
    std::cout << "[GC Bench] Testing " << value_size << " B payloads.\n";
    std::cout << "           Keys: " << NUM_KEYS << ", Overwrites: " << OVERWRITES << "\n";

    
    std::string payload(value_size, 'X');

    // Initial Population
    for (int i = 0; i < NUM_KEYS; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "user_%010d", i);
        db->Put(std::string(key), payload);
    }
    db->ForceFlush();

    // Random Overwrites to trigger heavy GC
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, NUM_KEYS - 1);
    
    for (int i = 0; i < OVERWRITES; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "user_%010d", dist(rng));
        db->Put(std::string(key), payload);
        
        // Give GC a tiny chance to breathe and do its work in the background
        if (i % 2000 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    // Force final compaction and GC
    db->ForceFlush();
    for(int i=0; i<10; ++i) db->ForceGC(); // Ensure GC cleans up dead space

    EngineStats stats = db->GetStats();
    
    double waf = (double)stats.storage_bytes_written / stats.user_bytes_written;
    
    std::cout << std::left << std::setw(15) << (std::to_string(value_size) + " B")
              << std::fixed << std::setprecision(3) << waf << "x\n";

    db->Close();
    delete db;
}

int main() {
    std::cout << "========================================================\n";
    std::cout << " WiscKey Garbage Collection (GC) WAF Test\n";
    std::cout << "========================================================\n";
    std::cout << " Dataset: Sustained Random Overwrites (Active GC)\n";
    std::cout << " Expected: WAF > 1.0 due to GC copying live records.\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << std::left << std::setw(15) << "Value Size" << "GC Write Amplification\n";
    std::cout << "--------------------------------------------------------\n";

    std::vector<int> value_sizes = {1024, 4096, 16384, 32768};
    for (int sz : value_sizes) {
        run_gc_waf_test(sz);
    }
    
    std::cout << "========================================================\n";
    return 0;
}
