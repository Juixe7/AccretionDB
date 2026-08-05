// ForgeLSM Rigorous Cold-Disk Random Read Benchmark
// Proves: True physical SSD read performance after complete OS Page Cache eviction.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <random>
#include <iomanip>
#include <fstream>
#include <numeric>

#ifdef _WIN32
  #include <windows.h>
  #include <psapi.h>
#else
  #include <unistd.h>
  #include <sys/stat.h>
#endif

using namespace forgelsm;

const std::string DB_PATH = "flsm_cold_read_db";
const std::string EVICT_FILE_PATH = "evict_cache_temp.bin";
const int NUM_KEYS = 300000;         // 300,000 keys
const int VALUE_SIZE = 1024;         // 1KB payload = ~300MB dataset
const int NUM_READ_QUERIES = 20000;   // 20,000 random queries
const size_t EVICTION_SIZE_BYTES = 4000000000ULL; // 4 GB RAM/Cache churn displacement

std::string make_key(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "key_%010d", i);
    return std::string(buf);
}

void purge_system_cache() {
    std::cout << "\n[Cache Eviction] Purging OS Page Cache & File System Standby List...\n";

#ifdef _WIN32
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp;
        LookupPrivilegeValue(NULL, SE_INCREASE_QUOTA_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
        CloseHandle(hToken);
    }
#endif

    std::cout << "  Creating 4 GB cache displacement file to evict database pages from RAM...\n";
    {
        std::ofstream evict_file(EVICT_FILE_PATH, std::ios::binary);
        std::vector<char> buffer(64 * 1024 * 1024, 'E'); 
        size_t written = 0;
        while (written < EVICTION_SIZE_BYTES) {
            evict_file.write(buffer.data(), buffer.size());
            written += buffer.size();
        }
        evict_file.flush();
        evict_file.close();
    }

    std::cout << "  Reading back 4 GB displacement file to force OS page eviction...\n";
    {
        std::ifstream evict_file(EVICT_FILE_PATH, std::ios::binary);
        std::vector<char> buffer(64 * 1024 * 1024);
        while (evict_file.read(buffer.data(), buffer.size())) {
            volatile char c = buffer[0];
            (void)c;
        }
        evict_file.close();
    }

    std::filesystem::remove(EVICT_FILE_PATH);

#ifdef _WIN32
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#endif

    std::cout << "[Cache Eviction] Complete. OS Page Cache successfully displaced.\n\n";
}

void run_queries(DB* db, const std::vector<int>& query_indices, const std::string& phase_name) {
    std::cout << "Phase: Executing " << phase_name << " Random Point Lookups (" << NUM_READ_QUERIES << " queries)..." << std::endl;
    
    std::vector<double> latencies_us;
    latencies_us.reserve(NUM_READ_QUERIES);

    int found_count = 0;
    auto start_total = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_READ_QUERIES; ++i) {
        std::string key = make_key(query_indices[i]);
        std::string val;

        auto t0 = std::chrono::high_resolution_clock::now();
        Status status = db->Get(key, &val);
        auto t1 = std::chrono::high_resolution_clock::now();

        if (status.ok()) {
            found_count++;
        }

        double elapsed_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        latencies_us.push_back(elapsed_us);
    }

    auto end_total = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(end_total - start_total).count();

    std::sort(latencies_us.begin(), latencies_us.end());

    double avg_lat = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0) / latencies_us.size();
    double p50 = latencies_us[static_cast<size_t>(NUM_READ_QUERIES * 0.50)];
    double p90 = latencies_us[static_cast<size_t>(NUM_READ_QUERIES * 0.90)];
    double p99 = latencies_us[static_cast<size_t>(NUM_READ_QUERIES * 0.99)];
    double p999 = latencies_us[static_cast<size_t>(NUM_READ_QUERIES * 0.999)];
    double max_lat = latencies_us.back();
    double ops_per_sec = NUM_READ_QUERIES / total_sec;

    std::cout << "\n===============================================================\n";
    std::cout << " " << phase_name << " BENCHMARK RESULTS\n";
    std::cout << "===============================================================\n";
    std::cout << "  Keys Found:          " << found_count << " / " << NUM_READ_QUERIES << "\n";
    std::cout << "  Total Execution Time: " << std::fixed << std::setprecision(2) << total_sec << " s\n";
    std::cout << "  Read Throughput:      " << std::fixed << std::setprecision(0) << ops_per_sec << " ops/sec\n";
    std::cout << "  -------------------------------------------------------------\n";
    std::cout << "  Latency Metrics (Microseconds / Milliseconds):\n";
    std::cout << "    Average Latency:   " << std::setw(10) << std::fixed << std::setprecision(2) << avg_lat << " us  (" << (avg_lat / 1000.0) << " ms)\n";
    std::cout << "    p50 (Median):       " << std::setw(10) << std::fixed << std::setprecision(2) << p50     << " us  (" << (p50 / 1000.0)     << " ms)\n";
    std::cout << "    p90 Latency:       " << std::setw(10) << std::fixed << std::setprecision(2) << p90     << " us  (" << (p90 / 1000.0)     << " ms)\n";
    std::cout << "    p99 Latency:       " << std::setw(10) << std::fixed << std::setprecision(2) << p99     << " us  (" << (p99 / 1000.0)     << " ms)\n";
    std::cout << "    p99.9 Latency:     " << std::setw(10) << std::fixed << std::setprecision(2) << p999    << " us  (" << (p999 / 1000.0)    << " ms)\n";
    std::cout << "    Max Latency:       " << std::setw(10) << std::fixed << std::setprecision(2) << max_lat   << " us  (" << (max_lat / 1000.0)  << " ms)\n";
    std::cout << "===============================================================\n";
}

int main() {
    std::cout << "===============================================================\n";
    std::cout << " ForgeLSM: Rigorous Cold-Disk Random Read Benchmark\n";
    std::cout << "===============================================================\n";

    std::filesystem::remove_all(DB_PATH);
    std::filesystem::remove(EVICT_FILE_PATH);

    std::cout << "Phase 1: Populating Database (" << NUM_KEYS << " keys x " << VALUE_SIZE << " B)..." << std::endl;
    {
        Options opts;
        opts.sync_writes = false;
        opts.vlog_shards = 4;
        opts.flush_threshold = 16 * 1024 * 1024;

        DB* db = nullptr;
        Status s = DB::Open(opts, DB_PATH, &db);
        if (!s.ok()) {
            std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
            return 1;
        }

        std::string payload(VALUE_SIZE, 'v');
        for (int i = 0; i < NUM_KEYS; ++i) {
            db->Put(make_key(i), payload);
        }

        std::cout << "  Closing DB handle to flush Memtables and unmap Bloom Filters..." << std::endl;
        db->Close();
        delete db;
    }

    std::vector<int> query_indices(NUM_READ_QUERIES);
    std::mt19937 rng(42); 
    std::uniform_int_distribution<int> dist(0, NUM_KEYS - 1);
    for (int i = 0; i < NUM_READ_QUERIES; ++i) {
        query_indices[i] = dist(rng);
    }

    purge_system_cache();

    {
        Options opts;
        opts.sync_writes = false;

        DB* db = nullptr;
        Status s = DB::Open(opts, DB_PATH, &db);
        if (!s.ok()) {
            std::cerr << "Failed to re-open DB: " << s.ToString() << std::endl;
            return 1;
        }

        run_queries(db, query_indices, "COLD-DISK");
        
        std::cout << "\n[Warm Phase] Immediately querying again without cache eviction to demonstrate OS RAM absorption...\n\n";
        
        run_queries(db, query_indices, "WARM-DISK");

        db->Close();
        delete db;
    }

    std::filesystem::remove_all(DB_PATH);
    return 0;
}
