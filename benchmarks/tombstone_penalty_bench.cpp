// ForgeLSM: Tombstone & Deletion Penalty Benchmark
// Proves the efficiency of L0 shadowing for Point Queries and the classic LSM penalty for Range Scans.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <random>

using namespace forgelsm;

const std::string DB_PATH = "flsm_tombstone_bench";
const int TOTAL_KEYS = 1000000;
const int DELETE_KEYS = 900000;
const int VALUE_SIZE = 128; // Small value size so we focus on key-iteration

std::string make_key(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "key_%010d", i);
    return std::string(buf);
}

int main() {
    std::cout << "===============================================================\n";
    std::cout << " ForgeLSM: Tombstone & Deletion Penalty Benchmark\n";
    std::cout << "===============================================================\n";

    std::filesystem::remove_all(DB_PATH);

    Options opts;
    opts.sync_writes = false;
    opts.vlog_shards = 4;
    opts.flush_threshold = 128 * 1024 * 1024; // Massive memtable to control flushes manually
    opts.background_compaction = false; // Disable background compaction to force L0/L1 split

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB\n";
        return 1;
    }

    // Phase 1: Deep Ingestion
    std::cout << "\n[Phase 1] Deep Ingestion: Inserting " << TOTAL_KEYS << " keys...\n";
    std::string payload(VALUE_SIZE, 'A');
    for (int i = 0; i < TOTAL_KEYS; ++i) {
        db->Put(make_key(i), payload);
    }
    
    // Force Flush to push all 1M keys into L1
    std::cout << "  Flushing 1,000,000 live records to L1...\n";
    db->ForceFlush();
    
    // Phase 2: Baseline Scan
    std::cout << "\n[Phase 2] Baseline Scan: Scanning all " << TOTAL_KEYS << " live records...\n";
    std::vector<std::pair<std::string, std::string>> baseline_results;
    
    auto t0 = std::chrono::high_resolution_clock::now();
    db->Scan(make_key(0), make_key(TOTAL_KEYS), &baseline_results);
    auto t1 = std::chrono::high_resolution_clock::now();
    
    double baseline_scan_sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "  Baseline Scan Time: " << std::fixed << std::setprecision(4) << baseline_scan_sec << " s\n";
    std::cout << "  Records Yielded:    " << baseline_results.size() << "\n";

    // Phase 3: Mass Deletion (Shadowing)
    std::cout << "\n[Phase 3] Mass Deletion: Issuing " << DELETE_KEYS << " Deletes (Tombstones)...\n";
    // We will delete the FIRST 900,000 keys
    for (int i = 0; i < DELETE_KEYS; ++i) {
        db->Delete(make_key(i));
    }
    
    // Force Flush to push 900k tombstones into L0
    std::cout << "  Flushing 900,000 Tombstones to L0 (Shadowing L1 data)...\n";
    db->ForceFlush();

    // Phase 4: Point Query Shadowing Test
    std::cout << "\n[Phase 4] L0 Shadowing Test: Querying 50,000 deleted keys...\n";
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, DELETE_KEYS - 1);
    
    auto p0 = std::chrono::high_resolution_clock::now();
    int not_found_count = 0;
    for (int i = 0; i < 50000; ++i) {
        std::string val;
        Status st = db->Get(make_key(dist(rng)), &val);
        if (!st.ok()) {
            not_found_count++;
        }
    }
    auto p1 = std::chrono::high_resolution_clock::now();
    double point_query_sec = std::chrono::duration<double>(p1 - p0).count();
    
    std::cout << "  Point Query Time (50k keys): " << std::fixed << std::setprecision(4) << point_query_sec << " s\n";
    std::cout << "  Correctly identified as deleted: " << not_found_count << " / 50000\n";

    // Phase 5: The Tombstone Scan Penalty
    std::cout << "\n[Phase 5] The Tombstone Scan Penalty: Scanning entire dataset again...\n";
    std::vector<std::pair<std::string, std::string>> penalty_results;
    
    auto s0 = std::chrono::high_resolution_clock::now();
    db->Scan(make_key(0), make_key(TOTAL_KEYS), &penalty_results);
    auto s1 = std::chrono::high_resolution_clock::now();
    
    double penalty_scan_sec = std::chrono::duration<double>(s1 - s0).count();
    std::cout << "  Tombstone Scan Time: " << std::fixed << std::setprecision(4) << penalty_scan_sec << " s\n";
    std::cout << "  Records Yielded:     " << penalty_results.size() << " (Expected: 100000)\n";
    
    double scan_penalty_factor = penalty_scan_sec / baseline_scan_sec;

    std::cout << "\n===============================================================\n";
    std::cout << " TOMBSTONE PENALTY BENCHMARK RESULTS\n";
    std::cout << "===============================================================\n";
    std::cout << "  Point Query Speed (L0 short-circuit): " << std::fixed << std::setprecision(0) << (50000.0 / point_query_sec) << " ops/sec\n";
    std::cout << "  Baseline Scan (1M records):           " << std::fixed << std::setprecision(4) << baseline_scan_sec << " s\n";
    std::cout << "  Tombstone Scan (100k records + 900k tombstones): " << penalty_scan_sec << " s\n";
    std::cout << "  -------------------------------------------------------------\n";
    std::cout << "  Tombstone Scan Penalty Factor:        " << std::fixed << std::setprecision(2) << scan_penalty_factor << "x\n";
    std::cout << "===============================================================\n";

    db->Close();
    delete db;
    std::filesystem::remove_all(DB_PATH);
    return 0;
}
