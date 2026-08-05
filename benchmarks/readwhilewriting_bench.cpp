// ForgeLSM: readwhilewriting Benchmark
// Tests highly concurrent Lock-Free Reads while a single writer thread 
// is aggressively hammering the database with overwrites.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <thread>
#include <iomanip>
#include <random>
#include <atomic>

using namespace forgelsm;

const std::string DB_PATH = "flsm_readwhilewriting";
const int NUM_KEYS = 1000000; // 1 Million keys
const int VAL_SIZE = 1024;    // 1KB value size -> 1GB total dataset size
const int RUN_DURATION_SEC = 10;

// Shared flags
std::atomic<bool> keep_running{true};

struct ThreadStats {
    uint64_t ops = 0;
};

std::string format_key(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%010d", i);
    return std::string(buf);
}

void run_benchmark(DB* db, int num_readers) {
    std::vector<std::thread> readers;
    std::thread writer;
    
    std::vector<ThreadStats> reader_stats(num_readers);
    std::atomic<uint64_t> total_writes{0};
    
    keep_running = true;
    
    auto bench_start = std::chrono::high_resolution_clock::now();
    
    // Writer thread (True microsecond-accurate rate limiting)
    writer = std::thread([&]() {
        std::mt19937 rng(42);
        std::uniform_int_distribution<> key_dist(0, NUM_KEYS - 1);
        std::string val(VAL_SIZE, 'w');
        
        // Rate limit to roughly 2000 ops/sec (~2MB/s with 1KB values)
        auto next_tick = std::chrono::steady_clock::now();
        const std::chrono::microseconds tick_interval(500); 
        
        uint64_t w_ops = 0;
        while (keep_running.load(std::memory_order_relaxed)) {
            int k = key_dist(rng);
            db->Put(format_key(k), val);
            w_ops++;
            
            next_tick += tick_interval;
            // Busy wait to avoid Windows 15.6ms sleep resolution ruining the benchmark
            while (std::chrono::steady_clock::now() < next_tick) {}
        }
        total_writes.store(w_ops, std::memory_order_relaxed);
    });
    
    // Reader threads
    for (int t = 0; t < num_readers; ++t) {
        readers.emplace_back([&, t]() {
            std::mt19937 rng(100 + t);
            std::uniform_int_distribution<> key_dist(0, NUM_KEYS - 1);
            
            uint64_t r_ops = 0;
            std::string val;
            
            while (keep_running.load(std::memory_order_relaxed)) {
                int k = key_dist(rng);
                std::string key = format_key(k);
                db->Get(key, &val);
                r_ops++;
            }
            reader_stats[t].ops = r_ops;
        });
    }
    
    // Wait for the duration
    std::this_thread::sleep_for(std::chrono::seconds(RUN_DURATION_SEC));
    
    // Signal stop
    keep_running = false;
    
    writer.join();
    for (auto& r : readers) {
        r.join();
    }
    
    auto bench_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> total_time = bench_end - bench_start;
    
    // Aggregate results
    uint64_t total_reads = 0;
    
    for (int t = 0; t < num_readers; ++t) {
        total_reads += reader_stats[t].ops;
    }
    
    double read_throughput = total_reads / total_time.count();
    double write_throughput = total_writes.load() / total_time.count();
    
    std::cout << "    [Result] Readers: " << std::setw(2) << num_readers 
              << " | Read Ops/sec: " << std::setw(9) << std::fixed << std::setprecision(0) << read_throughput 
              << " | Write Ops/sec: " << std::setw(7) << std::fixed << std::setprecision(0) << write_throughput << "\n";
}

int main() {
    std::cout << "===============================================================\n";
    std::cout << " ForgeLSM: Multi-threaded Read & Single-threaded Write\n";
    std::cout << " Proves: Lock-Free SkipList Memtable non-blocking behavior\n";
    std::cout << "===============================================================\n";

    std::filesystem::remove_all(DB_PATH);
    
    Options opts;
    opts.sync_writes = false; // We care about CPU lock-free concurrency, not SSD sync limits
    opts.vlog_shards = 16;
    opts.quiet_mode = true;
    
    DB* db = nullptr;
    DB::Open(opts, DB_PATH, &db);
    
    // Phase 1: Pre-load outside the loop
    std::cout << "  Pre-loading " << NUM_KEYS << " keys (" << (NUM_KEYS * VAL_SIZE) / (1024*1024) << " MB dataset) once...\n";
    std::string dummy_val(VAL_SIZE, 'a');
    
    // Multi-threaded preload for speed
    std::vector<std::thread> preload_threads;
    int num_t = 16;
    for (int t = 0; t < num_t; ++t) {
        preload_threads.emplace_back([&, t]() {
            for (int i = t; i < NUM_KEYS; i += num_t) {
                db->Put(format_key(i), dummy_val);
            }
        });
    }
    for (auto& t : preload_threads) t.join();
    std::cout << "  Pre-load complete. Commencing Read-While-Writing scalability tests.\n";

    std::vector<int> reader_counts = {1, 2, 4, 8, 16};
    
    for (int readers : reader_counts) {
        std::cout << "\n--- Testing with 1 Writer Thread and " << readers << " Reader Threads ---\n";
        run_benchmark(db, readers);
    }

    std::cout << "\n===============================================================\n";
    std::cout << " Done.\n";
    std::cout << "===============================================================\n";
    
    db->Close();
    delete db;
    return 0;
}
