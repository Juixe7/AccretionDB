// ForgeLSM Bloom Filter Benchmark
// Proves that querying missing keys bypasses disk I/O efficiently.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <iomanip>

using namespace forgelsm;

const std::string DB_PATH = "/tmp/flsm_bloom_bench";
const int NUM_KEYS = 1000000;

void setup_db() {
    std::filesystem::remove_all(DB_PATH);
}

std::string format_key(int i, bool exists) {
    char buf[64];
    if (exists) {
        snprintf(buf, sizeof(buf), "exist_key_%010d", i);
    } else {
        snprintf(buf, sizeof(buf), "missing_key_%010d", i);
    }
    return std::string(buf);
}

int main() {
    setup_db();
    
    Options opts;
    opts.sync_writes = false;
    opts.vlog_shards = 4;
    opts.flush_threshold = 128 * 1024;
    
    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        return 1;
    }
    
    std::cout << "========================================================\n";
    std::cout << " ForgeLSM Bloom Filter (Zero-I/O) Benchmark\n";
    std::cout << "========================================================\n";
    std::cout << "[1] Inserting " << NUM_KEYS << " keys...\n";
    
    std::string val(256, 'X');
    for (int i = 0; i < NUM_KEYS; ++i) {
        db->Put(format_key(i, true), val);
    }
    
    // Force a flush so all data resides in SSTables on disk.
    // This is required to test Bloom filters.
    std::cout << "[2] Flushing memtables to disk (building Bloom Filters)...\n";
    db->ForceFlush();
    
    EngineStats base_stats = db->GetStats();
    
    std::cout << "[3] Querying " << NUM_KEYS << " EXISTING keys...\n";
    auto start_exist = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_KEYS; ++i) {
        std::string out;
        db->Get(format_key(i, true), &out);
    }
    auto end_exist = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> time_exist = end_exist - start_exist;
    
    EngineStats exist_stats = db->GetStats();
    
    std::cout << "[4] Querying " << NUM_KEYS << " MISSING keys...\n";
    auto start_miss = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_KEYS; ++i) {
        std::string out;
        db->Get(format_key(i, false), &out);
    }
    auto end_miss = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> time_miss = end_miss - start_miss;
    
    EngineStats miss_stats = db->GetStats();
    
    // Calculate deltas
    uint64_t exist_bloom_skips = exist_stats.bloom_filter_skips - base_stats.bloom_filter_skips;
    uint64_t exist_sst_searches = exist_stats.sst_searches - base_stats.sst_searches;
    uint64_t exist_vlog_reads = exist_stats.vlog_reads - base_stats.vlog_reads;
    
    uint64_t miss_bloom_skips = miss_stats.bloom_filter_skips - exist_stats.bloom_filter_skips;
    uint64_t miss_sst_searches = miss_stats.sst_searches - exist_stats.sst_searches;
    uint64_t miss_vlog_reads = miss_stats.vlog_reads - exist_stats.vlog_reads;
    
    std::cout << "\n========================================================\n";
    std::cout << std::left << std::setw(20) << "Metric" 
              << std::setw(20) << "Existing Keys" 
              << std::setw(20) << "Missing Keys" << "\n";
    std::cout << "--------------------------------------------------------\n";
    
    std::cout << std::left << std::setw(20) << "Time Taken (sec)" 
              << std::setw(20) << std::fixed << std::setprecision(3) << time_exist.count()
              << std::setw(20) << std::fixed << std::setprecision(3) << time_miss.count() << "\n";
              
    std::cout << std::left << std::setw(20) << "Bloom Skips" 
              << std::setw(20) << exist_bloom_skips
              << std::setw(20) << miss_bloom_skips << "\n";
              
    std::cout << std::left << std::setw(20) << "SST Block Searches" 
              << std::setw(20) << exist_sst_searches
              << std::setw(20) << miss_sst_searches << "\n";
              
    std::cout << std::left << std::setw(20) << "VLog Disk Reads" 
              << std::setw(20) << exist_vlog_reads
              << std::setw(20) << miss_vlog_reads << "\n";
    std::cout << "========================================================\n";
    
    db->Close();
    delete db;
    return 0;
}
