// ForgeLSM Thread Scaling Benchmark
// Proves: Multi-core write scalability of the Lock-Free SkipList and Sharded VLog.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <thread>
#include <iomanip>

using namespace forgelsm;

const std::string DB_PATH = "/tmp/flsm_scaling_bench";

void setup_db() {
    std::filesystem::remove_all(DB_PATH);
}

std::string format_key(int thread_id, int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "t%02d_key_%010d", thread_id, i);
    return std::string(buf);
}

double run_scaling_test(int num_threads, int writes_per_thread, bool sync_writes, int num_shards) {
    setup_db();
    Options opts;
    opts.sync_writes = sync_writes;
    opts.vlog_shards = num_shards;
    opts.flush_threshold = 32 * 1024 * 1024; // Large memtable to prevent flush bottlenecks during test
    
    DB* db = nullptr;
    DB::Open(opts, DB_PATH, &db);

    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([db, t, writes_per_thread]() {
            // Write tiny values (64 bytes) to focus on lock contention, not memory bandwidth
            std::string val(64, 'x');
            for (int i = 0; i < writes_per_thread; ++i) {
                db->Put(format_key(t, i), val);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    db->Close();
    delete db;

    int total_writes = num_threads * writes_per_thread;
    return total_writes / diff.count(); // ops/sec
}

void bench_scaling_matrix() {
    std::cout << "\n--- BRUTAL BENCHMARK: Thread Scaling & Mutex Contention ---\n";
    std::cout << "  VLog Shards: 16 (Maximized horizontal scaling)\n";
    std::cout << "  Payload: 64 bytes (Focuses on lock contention, not bandwidth)\n\n";

    const int TOTAL_WRITES = 200000;
    std::vector<int> thread_counts = {1, 2, 4, 8, 16};

    std::cout << "  [Phase A] CPU/Lock Contention Test (sync_writes = false)\n";
    std::cout << "  Proves: The Lock-Free Memtable and Sharded VLog scale across CPU cores.\n";
    
    double baseline_ops = 0.0;
    
    for (int t : thread_counts) {
        int writes_per_thread = TOTAL_WRITES / t;
        double ops = run_scaling_test(t, writes_per_thread, false, 16);
        if (t == 1) baseline_ops = ops;
        
        double scaling_factor = ops / baseline_ops;
        std::cout << "    " << std::setw(2) << t << " Threads: " 
                  << std::setw(10) << std::fixed << std::setprecision(0) << ops << " ops/sec  "
                  << "(Scaling: " << std::fixed << std::setprecision(2) << scaling_factor << "x)\n";
    }

    std::cout << "\n  [Phase B] Physical Hardware I/O Contention Test (sync_writes = true)\n";
    std::cout << "  Proves: The physical SSD fdatasync() limit when hammered concurrently.\n";
    
    // Reduce total writes for physical sync test to avoid melting the SSD
    const int SYNC_WRITES = 4000;
    
    for (int t : thread_counts) {
        int writes_per_thread = SYNC_WRITES / t;
        double ops = run_scaling_test(t, writes_per_thread, true, 16);
        std::cout << "    " << std::setw(2) << t << " Threads: " 
                  << std::setw(10) << std::fixed << std::setprecision(0) << ops << " ops/sec\n";
    }
}

int main() {
    std::cout << "==============================================\n";
    std::cout << " ForgeLSM: Thread Scaling Matrix\n";
    std::cout << "==============================================\n";

    bench_scaling_matrix();

    std::cout << "\n==============================================\n";
    std::cout << " Done.\n";
    std::cout << "==============================================\n";
    return 0;
}
