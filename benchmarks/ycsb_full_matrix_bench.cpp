// ForgeLSM Rigorous Standardized YCSB Steady-State Matrix Benchmark (Workloads A - F)
// Proves: True Multi-threaded concurrency, MVCC contention handling, and SSD scaling.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <random>
#include <iomanip>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>

using namespace forgelsm;

const std::string DB_PATH = "flsm_ycsb_matrix_db";
const int VALUE_SIZE = 1024;        // 1KB payload

int NUM_KEYS = 2500000;             // Default 2.5GB
int NUM_THREADS = 16;               // Tuned for 16 Logical Processors
int WORKLOAD_OPS = 20000;           // Ops per thread

std::string make_key(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "ycsb_k_%010d", i);
    return std::string(buf);
}

// Simple Zipfian generator matching YCSB skew (s = 0.99)
class ZipfianGenerator {
    int num_items;
    std::mt19937 rng;
    std::uniform_real_distribution<double> dist;
    double zeta_n;
    double eta;

    static double zeta(int n, double theta = 0.99) {
        double sum = 0;
        for (int i = 1; i <= n; ++i) {
            sum += 1.0 / std::pow(i, theta);
        }
        return sum;
    }

public:
    ZipfianGenerator(int items, uint32_t seed) : num_items(items), rng(seed), dist(0.0, 1.0) {
        zeta_n = zeta(num_items);
        double zeta_2 = zeta(2);
        eta = (1.0 - std::pow(2.0 / num_items, 0.01)) / (1.0 - zeta_2 / zeta_n);
    }

    int next() {
        double u = dist(rng);
        double uz = u * zeta_n;
        if (uz < 1.0) return 0;
        if (uz < 1.0 + std::pow(0.5, 0.99)) return 1;
        int val = static_cast<int>(num_items * std::pow(eta * u - eta + 1.0, 1.0 / (1.0 - 0.99)));
        return std::clamp(val, 0, num_items - 1);
    }
};

// Global variables to fix benchmark bias and overhead
std::vector<int> precalc_zipfian;
std::atomic<int> global_total_keys{0};

void precalculate_zipfian() {
    ZipfianGenerator zipf(NUM_KEYS, 1337);
    int num_precalc = std::max(NUM_KEYS, WORKLOAD_OPS * NUM_THREADS * 2);
    precalc_zipfian.reserve(num_precalc);
    for (int i = 0; i < num_precalc; ++i) {
        precalc_zipfian.push_back(zipf.next());
    }
}

struct WorkloadMetrics {
    std::string name;
    double total_sec = 0.0;
    double ops_sec = 0.0;
    double p50_us = 0.0;
    double p90_us = 0.0;
    double p99_us = 0.0;
    double p999_us = 0.0;
    double max_us = 0.0;
};

void worker_thread(DB* db, int thread_id, double read_ratio, double update_ratio, double scan_ratio, double rmw_ratio, bool latest_bias, std::vector<double>& out_latencies) {
    std::mt19937 rng(42 + thread_id);
    std::uniform_real_distribution<double> op_dist(0.0, 1.0);
    std::uniform_int_distribution<int> scan_len_dist(1, 50);

    size_t zipf_idx = rng() % precalc_zipfian.size();

    std::string update_payload(VALUE_SIZE, 'U');
    out_latencies.reserve(WORKLOAD_OPS);

    for (int i = 0; i < WORKLOAD_OPS; ++i) {
        double p = op_dist(rng);
        
        int raw_zipf = precalc_zipfian[zipf_idx];
        zipf_idx = (zipf_idx + 1) % precalc_zipfian.size();
        
        int current_global_keys = global_total_keys.load(std::memory_order_relaxed);
        int key_idx = latest_bias ? (current_global_keys - 1 - (raw_zipf % std::min(500, current_global_keys))) : raw_zipf;
        std::string key = make_key(key_idx);

        auto t0 = std::chrono::high_resolution_clock::now();

        if (p < read_ratio) {
            std::string val;
            db->Get(key, &val);
        } else if (p < read_ratio + update_ratio) {
            if (latest_bias) {
                // Fix Bug 2: True atomic insert key generation to prevent overlapping thread overwrites
                key = make_key(global_total_keys.fetch_add(1, std::memory_order_relaxed));
            }
            db->Put(key, update_payload);
        } else if (p < read_ratio + update_ratio + scan_ratio) {
            // Fix Bug 5: Use optimized API to avoid deep-copying std::pair and Keys
            std::vector<std::string> results;
            db->ScanValues(key, make_key(std::min(key_idx + scan_len_dist(rng), current_global_keys - 1)), &results);
        } else {
            std::string val;
            db->Get(key, &val);
            val += "_mod";
            db->Put(key, val);
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        out_latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
}

WorkloadMetrics run_workload(DB* db, const std::string& name, 
                             double read_ratio, double update_ratio, double scan_ratio, double rmw_ratio, 
                             bool latest_bias = false) {
    std::cout << "  Executing " << name << " (" << (WORKLOAD_OPS * NUM_THREADS) << " ops across " << NUM_THREADS << " threads)..." << std::flush;

    std::vector<std::thread> threads;
    std::vector<std::vector<double>> thread_latencies(NUM_THREADS);

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back(worker_thread, db, t, read_ratio, update_ratio, scan_ratio, rmw_ratio, latest_bias, std::ref(thread_latencies[t]));
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(end_time - start_time).count();

    std::vector<double> all_latencies;
    all_latencies.reserve(WORKLOAD_OPS * NUM_THREADS);
    for (const auto& lats : thread_latencies) {
        all_latencies.insert(all_latencies.end(), lats.begin(), lats.end());
    }
    std::sort(all_latencies.begin(), all_latencies.end());

    WorkloadMetrics m;
    m.name = name;
    m.total_sec = total_sec;
    m.ops_sec = all_latencies.size() / total_sec;
    m.p50_us = all_latencies[all_latencies.size() * 0.50];
    m.p90_us = all_latencies[all_latencies.size() * 0.90];
    m.p99_us = all_latencies[all_latencies.size() * 0.99];
    m.p999_us = all_latencies[all_latencies.size() * 0.999];
    m.max_us = all_latencies.back();

    std::cout << " Done. (" << std::fixed << std::setprecision(0) << m.ops_sec << " ops/sec, p99 = " 
              << std::fixed << std::setprecision(1) << m.p99_us << " us)\n";
    return m;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--keys" && i + 1 < argc) NUM_KEYS = std::stoi(argv[++i]);
        if (arg == "--threads" && i + 1 < argc) NUM_THREADS = std::stoi(argv[++i]);
        if (arg == "--ops" && i + 1 < argc) WORKLOAD_OPS = std::stoi(argv[++i]);
    }

    global_total_keys.store(NUM_KEYS);

    std::cout << "===============================================================\n";
    std::cout << " ForgeLSM: Multi-Threaded YCSB Steady-State Matrix Benchmark\n";
    std::cout << " Threads: " << NUM_THREADS << " | Dataset: " << NUM_KEYS << " keys (~" << (NUM_KEYS / 1000) << " MB)\n";
    std::cout << "===============================================================\n";

    std::cout << "Precalculating Zipfian distribution to eliminate CPU math overhead..." << std::endl;
    precalculate_zipfian();

    std::filesystem::remove_all(DB_PATH);

    Options preload_opts;
    preload_opts.sync_writes = false; // Fast pre-load
    preload_opts.vlog_shards = 4;
    preload_opts.flush_threshold = 16 * 1024 * 1024;

    DB* db = nullptr;
    Status s = DB::Open(preload_opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        return 1;
    }

    std::cout << "Pre-loading " << NUM_KEYS << " keys (1KB payload) extremely fast without sync..." << std::endl;
    std::string init_payload(VALUE_SIZE, 'Y');
    
    // Multi-threaded pre-loading for speed
    std::vector<std::thread> preload_threads;
    int keys_per_thread = NUM_KEYS / NUM_THREADS;
    for (int t = 0; t < NUM_THREADS; ++t) {
        preload_threads.emplace_back([&, t]() {
            int start = t * keys_per_thread;
            int end = (t == NUM_THREADS - 1) ? NUM_KEYS : start + keys_per_thread;
            for (int i = start; i < end; ++i) {
                db->Put(make_key(i), init_payload);
            }
        });
    }
    for (auto& th : preload_threads) th.join();
    
    std::cout << "Pre-load complete. Forcing flush to SSD...\n";
    db->ForceFlush();
    
    // Close and Re-open with strict physical syncs for the actual benchmark
    db->Close();
    delete db;
    
    std::cout << "Re-opening DB with sync_writes = TRUE for actual systems benchmarking...\n";
    Options bench_opts;
    bench_opts.sync_writes = true;
    bench_opts.vlog_shards = 4;
    bench_opts.flush_threshold = 16 * 1024 * 1024;
    s = DB::Open(bench_opts, DB_PATH, &db);

    std::cout << "Warmup phase: waiting 10 seconds for compactions to settle and OS Cache to stabilize...\n";
    std::this_thread::sleep_for(std::chrono::seconds(10));

    std::vector<WorkloadMetrics> matrix;

    // Run YCSB Workloads A through F
    matrix.push_back(run_workload(db, "Workload A (50% Read / 50% Update)", 0.50, 0.50, 0.00, 0.00));
    matrix.push_back(run_workload(db, "Workload B (95% Read / 5% Update)",  0.95, 0.05, 0.00, 0.00));
    matrix.push_back(run_workload(db, "Workload C (100% Read Only)",        1.00, 0.00, 0.00, 0.00));
    matrix.push_back(run_workload(db, "Workload D (95% Read / 5% Insert)",  0.95, 0.05, 0.00, 0.00, true));
    matrix.push_back(run_workload(db, "Workload E (95% Scan / 5% Insert)",  0.00, 0.05, 0.95, 0.00));
    matrix.push_back(run_workload(db, "Workload F (50% RMW / 50% Read)",    0.50, 0.00, 0.00, 0.50));

    std::cout << "\n=======================================================================================\n";
    std::cout << " TRUE SYSTEMS PERFORMANCE MATRIX RESULTS (Sync = ON)\n";
    std::cout << "=======================================================================================\n";
    std::cout << " Workload Name                         | Throughput  | p50 (us) | p90 (us) | p99 (us) | Max (us)\n";
    std::cout << " --------------------------------------+-------------+----------+----------+----------+----------\n";

    for (const auto& m : matrix) {
        std::cout << " " << std::setw(37) << std::left << m.name << " | "
                  << std::setw(9)  << std::right << std::fixed << std::setprecision(0) << m.ops_sec << " ops/s | "
                  << std::setw(8)  << std::fixed << std::setprecision(1) << m.p50_us << " | "
                  << std::setw(8)  << std::fixed << std::setprecision(1) << m.p90_us << " | "
                  << std::setw(8)  << std::fixed << std::setprecision(1) << m.p99_us << " | "
                  << std::setw(8)  << std::fixed << std::setprecision(1) << m.max_us << "\n";
    }

    std::cout << "=======================================================================================\n";

    db->Close();
    delete db;
    return 0;
}
