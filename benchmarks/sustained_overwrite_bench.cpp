// ForgeLSM Sustained Overwrite & Compaction Cliff Benchmark
// Proves: Steady-state write performance and compaction stall impact over continuous random updates.

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
#include <thread>
#include <atomic>

using namespace forgelsm;

const std::string DB_PATH = "flsm_sustained_db";
const std::string CSV_PATH = "sustained_overwrite_timeseries.csv";
const int UNIQUE_KEYS = 500000;
const int VALUE_SIZE = 512;               // 512 bytes payload
const int TOTAL_WRITES = 300000;         // 300k physical fsync writes (approx 10.5 minutes)

std::string make_key(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "key_%010d", i);
    return std::string(buf);
}

int main() {
    std::cout << "===============================================================\n";
    std::cout << " ForgeLSM: Sustained Overwrite & Compaction Cliff Benchmark\n";
    std::cout << "===============================================================\n";

    std::filesystem::remove_all(DB_PATH);
    std::filesystem::remove(CSV_PATH);

    Options opts;
    opts.sync_writes = true; // FORCE PHYSICAL SSD COMPACTION CLIFF
    opts.flush_threshold = 1 * 1024 * 1024; // 1MB Memtable to force continuous flushes
    opts.l0_compaction_trigger = 4;         // Force aggressive L0->L1 compactions
    opts.background_compaction = true;

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        return 1;
    }

    std::ofstream csv(CSV_PATH);
    csv << "elapsed_sec,writes_completed,interval_ops_sec,cumulative_ops_sec\n";

    std::atomic<bool> running{true};
    std::atomic<uint64_t> completed_writes{0};

    // Pre-generate keys
    std::cout << "Pre-generating " << UNIQUE_KEYS << " keys..." << std::endl;
    std::vector<std::string> keys(UNIQUE_KEYS);
    for (int i = 0; i < UNIQUE_KEYS; ++i) {
        keys[i] = make_key(i);
    }

    std::string payload(VALUE_SIZE, 'x');

    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(0, UNIQUE_KEYS - 1);

    auto start_time = std::chrono::high_resolution_clock::now();

    // Time-series logger thread
    std::thread logger([&]() {
        auto last_sample_time = std::chrono::high_resolution_clock::now();
        uint64_t last_completed = 0;

        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            auto now = std::chrono::high_resolution_clock::now();
            uint64_t current_completed = completed_writes.load();

            double sample_sec = std::chrono::duration<double>(now - last_sample_time).count();
            double total_sec = std::chrono::duration<double>(now - start_time).count();

            uint64_t delta_writes = current_completed - last_completed;
            double interval_ops = delta_writes / sample_sec;
            double cumulative_ops = current_completed / total_sec;

            csv << std::fixed << std::setprecision(2) << total_sec << ","
                << current_completed << ","
                << std::setprecision(0) << interval_ops << ","
                << cumulative_ops << "\n";
            csv.flush();

            last_sample_time = now;
            last_completed = current_completed;
        }
    });

    std::cout << "Executing " << TOTAL_WRITES << " continuous random overwrites..." << std::endl;

    // Track initial peak (first 100k) vs final steady state (last 300k)
    double peak_ops = 0.0;
    auto peak_start = std::chrono::high_resolution_clock::now();

    auto steady_start = std::chrono::high_resolution_clock::now();
    uint64_t steady_start_writes = 0;

    for (int i = 0; i < TOTAL_WRITES; ++i) {
        int key_idx = dist(rng);
        db->Put(keys[key_idx], payload);
        completed_writes++;

        if (i == 50000) {
            auto peak_end = std::chrono::high_resolution_clock::now();
            double peak_sec = std::chrono::duration<double>(peak_end - peak_start).count();
            peak_ops = 50000.0 / peak_sec;
        }

        if (i == 200000) {
            steady_start = std::chrono::high_resolution_clock::now();
            steady_start_writes = completed_writes.load();
        }

        if ((i + 1) % 50000 == 0) {
            std::cout << "  Progress: " << (i + 1) << " / " << TOTAL_WRITES << " writes completed..." << std::endl;
        }
    }

    running = false;
    if (logger.joinable()) {
        logger.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(end_time - start_time).count();
    double steady_sec = std::chrono::duration<double>(end_time - steady_start).count();
    double steady_ops = (TOTAL_WRITES - steady_start_writes) / steady_sec;
    double avg_ops = TOTAL_WRITES / total_sec;

    EngineStats stats = db->GetStats();

    std::cout << "\n===============================================================\n";
    std::cout << " SUSTAINED OVERWRITE BENCHMARK RESULTS\n";
    std::cout << "===============================================================\n";
    std::cout << "  Total Writes:         " << TOTAL_WRITES << " updates (" << (TOTAL_WRITES * VALUE_SIZE / 1024 / 1024) << " MB payload)\n";
    std::cout << "  Total Execution Time: " << std::fixed << std::setprecision(2) << total_sec << " s\n";
    std::cout << "  Initial Peak Rate:    " << std::fixed << std::setprecision(0) << peak_ops << " ops/sec (In-Memory phase)\n";
    std::cout << "  Steady-State Rate:    " << std::fixed << std::setprecision(0) << steady_ops << " ops/sec (Compaction Saturation phase)\n";
    std::cout << "  Overall Average Rate: " << std::fixed << std::setprecision(0) << avg_ops << " ops/sec\n";
    std::cout << "  Compaction Cliff Drop:" << std::fixed << std::setprecision(1) << ((1.0 - (steady_ops / peak_ops)) * 100.0) << "% throughput reduction\n";
    std::cout << "  WAF Measured:         " << std::fixed << std::setprecision(2) << stats.write_amplification << "x\n";
    std::cout << "  Time-series exported: " << CSV_PATH << "\n";
    std::cout << "===============================================================\n";

    db->Close();
    delete db;
    std::filesystem::remove_all(DB_PATH);
    return 0;
}
