// ForgeLSM Rigorous Concurrent VLog Garbage Collection Impact Benchmark
// Proves: Throughput penalty and latency spikes on foreground traffic during active VLog GC.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <random>
#include <iomanip>
#include <thread>
#include <atomic>
#include <numeric>
#include <sys/stat.h>

using namespace forgelsm;

const std::string DB_PATH = "flsm_gc_impact_db";
const int NUM_KEYS = 30000;
const int VALUE_SIZE = 4096;               // 4KB payload per key
const int RUN_DURATION_SEC = 5;            // Run workload for exactly 5 continuous seconds

size_t get_dir_size(const std::string& dir_path) {
    size_t total_blocks = 0;
    if (std::filesystem::exists(dir_path)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
            if (std::filesystem::is_regular_file(entry)) {
                struct stat st;
                if (stat(entry.path().string().c_str(), &st) == 0) {
                    total_blocks += (st.st_size + 511) / 512;
                }
            }
        }
    }
    return total_blocks * 512;
}

std::string make_key(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "user_%010d", i);
    return std::string(buf);
}

struct WorkloadResult {
    double total_sec = 0.0;
    double ops_sec = 0.0;
    double read_p50_us = 0.0;
    double read_p90_us = 0.0;
    double read_p99_us = 0.0;
    double read_max_us = 0.0;
    double write_p50_us = 0.0;
    double write_p90_us = 0.0;
    double write_p99_us = 0.0;
};

WorkloadResult run_ycsb_workload_a(DB* db, const std::string& label) {
    std::cout << "  Executing YCSB Workload A (" << label << ") for " << RUN_DURATION_SEC << " continuous seconds..." << std::endl;

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_dist(0, NUM_KEYS - 1);
    std::uniform_int_distribution<int> op_dist(0, 99);

    std::string new_payload(VALUE_SIZE, 'B');

    std::vector<double> read_latencies;
    std::vector<double> write_latencies;
    read_latencies.reserve(100000);
    write_latencies.reserve(100000);

    auto start_time = std::chrono::high_resolution_clock::now();
    auto end_target = start_time + std::chrono::seconds(RUN_DURATION_SEC);
    
    int ops = 0;

    while (std::chrono::high_resolution_clock::now() < end_target) {
        int key_idx = key_dist(rng);
        std::string key = make_key(key_idx);
        bool is_read = (op_dist(rng) < 50);

        if (is_read) {
            std::string val;
            auto t0 = std::chrono::high_resolution_clock::now();
            db->Get(key, &val);
            auto t1 = std::chrono::high_resolution_clock::now();
            read_latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        } else {
            auto t0 = std::chrono::high_resolution_clock::now();
            db->Put(key, new_payload);
            auto t1 = std::chrono::high_resolution_clock::now();
            write_latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        ops++;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(end_time - start_time).count();

    std::sort(read_latencies.begin(), read_latencies.end());
    std::sort(write_latencies.begin(), write_latencies.end());

    WorkloadResult res;
    res.total_sec = total_sec;
    res.ops_sec = ops / total_sec;

    if (!read_latencies.empty()) {
        res.read_p50_us = read_latencies[read_latencies.size() * 0.50];
        res.read_p90_us = read_latencies[read_latencies.size() * 0.90];
        res.read_p99_us = read_latencies[read_latencies.size() * 0.99];
        res.read_max_us = read_latencies.back();
    }

    if (!write_latencies.empty()) {
        res.write_p50_us = write_latencies[write_latencies.size() * 0.50];
        res.write_p90_us = write_latencies[write_latencies.size() * 0.90];
        res.write_p99_us = write_latencies[write_latencies.size() * 0.99];
    }

    return res;
}

int main() {
    std::cout << "===============================================================\n";
    std::cout << " ForgeLSM: Concurrent VLog Garbage Collection Impact Benchmark\n";
    std::cout << "===============================================================\n";

    std::filesystem::remove_all(DB_PATH);

    Options opts;
    opts.sync_writes = false;
    opts.vlog_shards = 4;
    opts.flush_threshold = 16 * 1024 * 1024;
    opts.background_gc = false; // Manual GC trigger

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        return 1;
    }

    std::cout << "Phase 1: Inserting " << NUM_KEYS << " keys (4KB payloads = ~120MB data)..." << std::endl;
    std::string payload_a(VALUE_SIZE, 'A');
    for (int i = 0; i < NUM_KEYS; ++i) {
        db->Put(make_key(i), payload_a);
    }
    db->ForceFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    size_t size_after_insert = get_dir_size(DB_PATH);
    std::cout << "  Physical storage size after initial insert: " << (size_after_insert / 1024 / 1024) << " MB\n";

    std::cout << "\nPhase 2: Overwriting all " << NUM_KEYS << " keys to generate VLog dead space..." << std::endl;
    std::string payload_b(VALUE_SIZE, 'B');
    for (int i = 0; i < NUM_KEYS; ++i) {
        db->Put(make_key(i), payload_b);
    }
    db->ForceFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    size_t size_after_update = get_dir_size(DB_PATH);
    std::cout << "  Physical storage size ballooned to: " << (size_after_update / 1024 / 1024) << " MB (Dead Space Created)\n";

    std::cout << "\nPhase 3: Measuring Baseline YCSB Workload A (GC Idle)..." << std::endl;
    WorkloadResult baseline = run_ycsb_workload_a(db, "Baseline - GC Idle");

    std::cout << "\nPhase 4: Launching Background VLog GC & Measuring Concurrent Workload A..." << std::endl;
    
    std::thread gc_thread([&]() {
        db->ForceGC();
    });

    WorkloadResult active_gc = run_ycsb_workload_a(db, "Active Concurrent GC");

    if (gc_thread.joinable()) {
        gc_thread.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    size_t size_after_gc = get_dir_size(DB_PATH);
    size_t space_reclaimed_mb = (size_after_update - size_after_gc) / 1024 / 1024;

    double throughput_penalty_pct = ((baseline.ops_sec - active_gc.ops_sec) / baseline.ops_sec) * 100.0;
    double read_p99_spike_factor = active_gc.read_p99_us / (baseline.read_p99_us > 0 ? baseline.read_p99_us : 1.0);

    std::cout << "\n===============================================================\n";
    std::cout << " CONCURRENT VLOG GC IMPACT BENCHMARK RESULTS\n";
    std::cout << "===============================================================\n";
    std::cout << "  Physical Space Reclaimed: " << space_reclaimed_mb << " MB\n";
    std::cout << "  Storage Size (Before GC): " << (size_after_update / 1024 / 1024) << " MB\n";
    std::cout << "  Storage Size (After GC):  " << (size_after_gc / 1024 / 1024) << " MB\n";
    std::cout << "  -------------------------------------------------------------\n";
    std::cout << "  Foreground Ingest Rate (Baseline): " << std::fixed << std::setprecision(0) << baseline.ops_sec << " ops/sec\n";
    std::cout << "  Foreground Ingest Rate (Active GC):" << std::fixed << std::setprecision(0) << active_gc.ops_sec << " ops/sec\n";
    std::cout << "  Throughput Penalty:               " << std::fixed << std::setprecision(1) << throughput_penalty_pct << "% reduction\n";
    std::cout << "  -------------------------------------------------------------\n";
    std::cout << "  Read p99 Latency (Baseline):  " << std::fixed << std::setprecision(2) << baseline.read_p99_us << " us\n";
    std::cout << "  Read p99 Latency (Active GC): " << std::fixed << std::setprecision(2) << active_gc.read_p99_us << " us\n";
    std::cout << "  Read p99 Latency Spike Factor:" << std::fixed << std::setprecision(2) << read_p99_spike_factor << "x inflation\n";
    std::cout << "  Read Max Latency (Active GC): " << std::fixed << std::setprecision(2) << active_gc.read_max_us << " us (" << (active_gc.read_max_us / 1000.0) << " ms)\n";
    std::cout << "===============================================================\n";

    db->Close();
    delete db;
    std::filesystem::remove_all(DB_PATH);
    return 0;
}
