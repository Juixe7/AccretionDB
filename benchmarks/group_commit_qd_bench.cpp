// ForgeLSM Rigorous Group Commit Queue Depth Scaling Benchmark
// Proves: Physical SSD fdatasync / FlushFileBuffers leader-follower batching efficiency across 1 to 64 threads.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <thread>
#include <iomanip>
#include <algorithm>
#include <mutex>
#include <numeric>

using namespace forgelsm;

const std::string DB_PATH = "flsm_qd_bench_db";
const int WRITES_PER_THREAD = 20000;
const int PAYLOAD_SIZE = 128;

std::string make_key(int thread_id, int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "t%02d_k_%010d", thread_id, i);
    return std::string(buf);
}

struct QDResult {
    int threads = 0;
    int total_writes = 0;
    double total_sec = 0.0;
    double ops_sec = 0.0;
    double scaling_factor = 0.0;
    double p50_us = 0.0;
    double p90_us = 0.0;
    double p99_us = 0.0;
};

QDResult run_qd_test(int num_threads, double baseline_ops) {
    std::filesystem::remove_all(DB_PATH);

    Options opts;
    opts.sync_writes = true; // FORCE PHYSICAL DISK SYNC
    opts.vlog_shards = 16;
    opts.flush_threshold = 32 * 1024 * 1024;

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        return {};
    }

    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    std::mutex lat_mutex;
    std::vector<double> all_latencies;
    all_latencies.reserve(num_threads * WRITES_PER_THREAD);

    std::string payload(PAYLOAD_SIZE, 'S');

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            std::vector<double> thread_lats;
            thread_lats.reserve(WRITES_PER_THREAD);

            for (int i = 0; i < WRITES_PER_THREAD; ++i) {
                std::string k = make_key(t, i);
                auto t0 = std::chrono::high_resolution_clock::now();
                db->Put(k, payload);
                auto t1 = std::chrono::high_resolution_clock::now();
                thread_lats.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            }

            std::lock_guard<std::mutex> lk(lat_mutex);
            all_latencies.insert(all_latencies.end(), thread_lats.begin(), thread_lats.end());
        });
    }

    for (auto& th : workers) {
        th.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(end_time - start_time).count();

    db->Close();
    delete db;
    std::filesystem::remove_all(DB_PATH);

    std::sort(all_latencies.begin(), all_latencies.end());

    int total_writes = num_threads * WRITES_PER_THREAD;
    QDResult res;
    res.threads = num_threads;
    res.total_writes = total_writes;
    res.total_sec = total_sec;
    res.ops_sec = total_writes / total_sec;
    res.scaling_factor = (baseline_ops > 0.0) ? (res.ops_sec / baseline_ops) : 1.0;

    if (!all_latencies.empty()) {
        res.p50_us = all_latencies[all_latencies.size() * 0.50];
        res.p90_us = all_latencies[all_latencies.size() * 0.90];
        res.p99_us = all_latencies[all_latencies.size() * 0.99];
    }

    return res;
}

int main() {
    std::cout << "===============================================================\n";
    std::cout << " ForgeLSM: Group Commit Queue Depth (QD) Scaling Benchmark\n";
    std::cout << " Configuration: sync_writes = true (Physical Flush per batch)\n";
    std::cout << "===============================================================\n";

    std::vector<int> thread_counts = {1, 2, 4, 8, 16, 32, 64};
    std::vector<QDResult> results;

    double baseline_ops = 0.0;

    for (int t : thread_counts) {
        std::cout << "  Testing " << std::setw(2) << t << " Threads (sync_writes = true)..." << std::flush;
        QDResult res = run_qd_test(t, baseline_ops);
        if (t == 1) {
            baseline_ops = res.ops_sec;
            res.scaling_factor = 1.0;
        }
        results.push_back(res);
        std::cout << " Done. Throughput: " << std::setw(8) << std::fixed << std::setprecision(0) << res.ops_sec << " ops/sec  "
                  << "(Scaling: " << std::setw(5) << std::fixed << std::setprecision(2) << res.scaling_factor << "x)\n";
    }

    std::cout << "\n===============================================================\n";
    std::cout << " GROUP COMMIT QUEUE DEPTH SCALING RESULTS\n";
    std::cout << "===============================================================\n";
    std::cout << " Threads | Total Writes | Throughput (ops/s) | Speedup | p50 (us) | p99 (us)\n";
    std::cout << " --------+--------------+--------------------+---------+----------+---------\n";

    for (const auto& r : results) {
        std::cout << "   " << std::setw(2) << r.threads << "    | "
                  << std::setw(12) << r.total_writes << " | "
                  << std::setw(18) << std::fixed << std::setprecision(0) << r.ops_sec << " | "
                  << std::setw(6) << std::fixed << std::setprecision(2) << r.scaling_factor << "x | "
                  << std::setw(8) << std::fixed << std::setprecision(1) << r.p50_us << " | "
                  << std::setw(8) << std::fixed << std::setprecision(1) << r.p99_us << "\n";
    }

    std::cout << "===============================================================\n";
    return 0;
}
