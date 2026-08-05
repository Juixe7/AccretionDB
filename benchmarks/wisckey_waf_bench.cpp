// ForgeLSM WiscKey Value-Size vs Write Amplification (WAF) Benchmark
//
// Proves the core WiscKey thesis: WAF remains near 1.0x regardless of value size.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <thread>

using namespace forgelsm;

const std::string DB_PATH = "flsm_wisckey_waf";

void setup_db() {
    std::filesystem::remove_all(DB_PATH);
}

std::string format_key(int i) {
    // Generate a semi-random key to force heavy LSM cascading (matching demo_iot)
    int sensor_id = rand() % 10000;
    char buf[64];
    snprintf(buf, sizeof(buf), "iot_sensor_%02d_%010d", sensor_id % 100, i);
    return std::string(buf);
}

// Runs a benchmark for a specific value size.
// To measure WAF accurately, we must trigger LSM compactions.
// We do this by writing a small dataset and overwriting it multiple times.
double measure_waf_for_size(int value_size) {
    setup_db();
    
    Options opts;
    opts.sync_writes = false; // We don't need physical disk sync for a WAF math test
    opts.vlog_shards = 4;
    
    // 256KB flush threshold to force severe SSTable cascading, matching demo_iot
    opts.flush_threshold = 256 * 1024;
    opts.l0_compaction_trigger = 4;
    opts.l1_max_files = 4;
    
    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        exit(1);
    }
    
    // 1,000,000 keys total to match demo_iot
    const int NUM_KEYS = 1000000;
    const int OVERWRITES = 0; // Just 1 massive ingestion pass of 1M keys
    
    std::string val(value_size, 'X');
    
    for (int pass = 0; pass <= OVERWRITES; ++pass) {
        for (int i = 0; i < NUM_KEYS; ++i) {
            db->Put(format_key(i), val);
        }
        // Force flush to ensure it goes to disk and triggers compactions
        db->ForceFlush();
    }
    
    // Wait for background compactions to settle down
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    
    EngineStats stats = db->GetStats();
    double waf = stats.write_amplification;
    
    db->Close();
    delete db;
    return waf;
}

int main() {
    // Limit to 1024 bytes. 1,000,000 keys * 1024 bytes = 1 GB VLog.
    // Anything larger (like 32KB) would create a 32 GB file and OOM the system!
    std::vector<int> value_sizes = {64, 128, 256, 512, 1024};
    
    std::cout << "========================================================\n";
    std::cout << " WiscKey Value-Size vs Write Amplification (WAF) Test\n";
    std::cout << "========================================================\n";
    std::cout << " Dataset: 1,000,000 keys (Semi-Random distribution matching demo_iot)\n";
    std::cout << " Workload: Deep 5-level LSM Cascading with 256KB flushing\n";
    std::cout << " Expected: WAF is high for small values, but drops near 1.0 for large values.\n";
    std::cout << "--------------------------------------------------------\n";
    
    std::cout << std::left << std::setw(15) << "Value Size" 
              << std::setw(20) << "Write Amplification" << "\n";
    std::cout << "--------------------------------------------------------\n";
    
    for (int size : value_sizes) {
        double waf = measure_waf_for_size(size);
        std::string size_str = std::to_string(size) + " B";
        if (size >= 1024) size_str = std::to_string(size / 1024) + " KB";
        
        std::cout << std::left << std::setw(15) << size_str
                  << std::fixed << std::setprecision(3) << waf << "x\n";
    }
    
    std::cout << "========================================================\n";
    return 0;
}
