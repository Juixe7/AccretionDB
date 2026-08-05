// ForgeLSM Performance Benchmark Suite
//
// Measures:
// 1. Sequential Write Throughput (memtable + VLog + fdatasync)
// 2. Random Write Throughput    (IoT pattern: random key ordering)
// 3. Random Read Latency        (two modes: hot-cache and cold-disk)
// 4. Write Amplification (WAF)  (user bytes vs all storage bytes including compaction rewrites)
//
// Methodology notes:
// - Timing wraps ONLY the operation loop, not DB::Open or setup.
// - Reads are measured in two phases: hot-cache (data in OS page cache)
//   and cold-disk (after ForceFlush + sync + page cache drop).
// - WAF counts compaction rewrites, not just the first write.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <thread>
#include <random>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <cstdlib>   // system()

using namespace forgelsm;

const std::string DB_PATH = "/tmp/flsm_bench";
const int NUM_KEYS = 100000;
const int KEY_SIZE = 16;  // length of formatted key string
const int VAL_SIZE = 128; // standard IoT payload size

// ── Helpers ───────────────────────────────────────────────────────

std::string random_string(int len, std::mt19937& rng) {
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
    std::string str(len, 0);
    for (int i = 0; i < len; ++i) str[i] = charset[dist(rng)];
    return str;
}

std::string format_key(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key_%010d", i);
    return std::string(buf);
}

void setup_db() {
    std::filesystem::remove_all(DB_PATH);
}

// Attempt to drop the Linux OS page cache so subsequent reads come from disk.
// Requires root. Prints a warning if it fails instead of aborting.
bool drop_page_cache() {
    // sync first to flush dirty pages
    ::system("sync");
    int rc = ::system("echo 3 > /proc/sys/vm/drop_caches 2>/dev/null");
    if (rc != 0) {
        std::cout << "  [WARN] Could not drop page cache (need sudo). "
                  << "Cold-disk latency may be underestimated.\n";
        return false;
    }
    return true;
}

// ── Benchmarks ────────────────────────────────────────────────────

void bench_sequential_write() {
    std::cout << "\n--- Benchmark 1: Sequential Write ---\n";
    std::cout << "  Keys: " << NUM_KEYS << "  |  Value size: " << VAL_SIZE << " B\n";
    std::cout << "  Measures: memtable insert + VLog append + fdatasync per shard\n";
    setup_db();

    Options opts;
    DB* db = nullptr;
    DB::Open(opts, DB_PATH, &db);

    std::string val(VAL_SIZE, 'v');

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_KEYS; ++i) {
        db->Put(format_key(i), val);
    }
    auto end = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double> diff = end - start;
    double ops_sec = NUM_KEYS / diff.count();
    double mb_sec  = (NUM_KEYS * (double)(KEY_SIZE + VAL_SIZE)) / (1024.0 * 1024.0 * diff.count());

    std::cout << "  Result: " << std::fixed << std::setprecision(0) << ops_sec
              << " ops/sec  |  " << std::setprecision(2) << mb_sec << " MB/s"
              << "  |  " << diff.count() << " s total\n";

    db->Close();
    delete db;
}

void bench_random_write() {
    std::cout << "\n--- Benchmark 2: Random Write (IoT pattern) ---\n";
    std::cout << "  Keys: " << NUM_KEYS << "  |  Value size: " << VAL_SIZE << " B  |  Key order: random\n";
    std::cout << "  Simulates sensors writing in arbitrary, non-sequential order\n";
    setup_db();

    Options opts;
    DB* db = nullptr;
    DB::Open(opts, DB_PATH, &db);

    std::vector<int> keys(NUM_KEYS);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    std::string val(VAL_SIZE, 'r');

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_KEYS; ++i) {
        db->Put(format_key(keys[i]), val);
    }
    auto end = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double> diff = end - start;
    std::cout << "  Result: " << std::fixed << std::setprecision(0) << (NUM_KEYS / diff.count())
              << " ops/sec  |  " << diff.count() << " s total\n";

    db->Close();
    delete db;
}

void bench_read_latency() {
    std::cout << "\n--- Benchmark 3: Random Read Latency ---\n";
    std::cout << "  Keys: " << NUM_KEYS << "  |  Read order: random shuffle\n";

    setup_db();
    Options opts;
    DB* db = nullptr;
    DB::Open(opts, DB_PATH, &db);

    // Step 1: Populate
    std::string val(VAL_SIZE, 'd');
    for (int i = 0; i < NUM_KEYS; ++i) db->Put(format_key(i), val);

    std::vector<int> keys(NUM_KEYS);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(1337);
    std::shuffle(keys.begin(), keys.end(), rng);

    // ── Phase A: Hot-cache reads (data is in OS page cache) ──────
    for (int phase = 0; phase < 2; ++phase) {
        if (phase == 0) {
            std::cout << "  [Phase A: Hot-cache (memory bounds)]\n";
        } else {
            std::cout << "  [Phase B: Cold-disk (page cache dropped)]\n";
            db->ForceFlush();
            if (!drop_page_cache()) continue;
        }

        std::vector<double> latencies_us;
        latencies_us.reserve(NUM_KEYS);
        std::string out;

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < NUM_KEYS; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            db->Get(format_key(keys[i]), &out);
            auto t1 = std::chrono::high_resolution_clock::now();
            latencies_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;

        std::sort(latencies_us.begin(), latencies_us.end());
        std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
                  << (NUM_KEYS / diff.count()) << " ops/sec\n";
        std::cout << "  Latency p50: " << std::setprecision(2) << latencies_us[NUM_KEYS * 0.50] << " us\n";
        std::cout << "  Latency p95: " << latencies_us[NUM_KEYS * 0.95] << " us\n";
        std::cout << "  Latency p99: " << latencies_us[NUM_KEYS * 0.99] << " us\n";
    }

    db->Close();
    delete db;
}

void bench_write_amplification() {
    std::cout << "\n--- Benchmark 4: Write Amplification (WAF) ---\n";
    std::cout << "  Workload: " << 50000 << " writes over " << 5000 << " unique keys (10x overwrite ratio)\n";
    std::cout << "  WAF = total_storage_bytes_written / user_bytes_written\n";
    std::cout << "  Includes: VLog record overhead + SSTable key entries + compaction rewrites\n";
    setup_db();

    Options opts;
    opts.flush_threshold    = 64 * 1024; // 64KB — flush frequently to trigger compaction
    opts.l0_compaction_trigger = 2;      // compact early to see full WAF effect

    DB* db = nullptr;
    DB::Open(opts, DB_PATH, &db);

    std::mt19937 rng(99);
    const int UNIQUE_KEYS  = 5000;
    const int TOTAL_WRITES = 50000;

    for (int i = 0; i < TOTAL_WRITES; ++i) {
        int k = rng() % UNIQUE_KEYS;
        db->Put(format_key(k), random_string(VAL_SIZE, rng));
    }

    // Force final flush and wait for background compaction to fully settle
    db->ForceFlush();
    std::this_thread::sleep_for(std::chrono::seconds(3));

    EngineStats stats = db->GetStats();

    std::cout << "\n  User Bytes Written:    " << stats.user_bytes_written << " bytes"
              << " (" << (stats.user_bytes_written / 1024 / 1024) << " MB)\n";
    std::cout << "  Storage Bytes Written: " << stats.storage_bytes_written << " bytes"
              << " (" << (stats.storage_bytes_written / 1024 / 1024) << " MB)\n";
    std::cout << "  Write Amplification:   " << std::fixed << std::setprecision(2)
              << stats.write_amplification << "x\n";
    std::cout << "\n  For context:\n";
    std::cout << "    LevelDB (with WAL): typically 10x–40x on this workload\n";
    std::cout << "    RocksDB:            typically  5x–20x\n";
    std::cout << "    WiscKey (ours):     " << std::setprecision(2) << stats.write_amplification
              << "x — only keys compacted, values stay in VLog\n";

    db->Close();
    delete db;
}

// ── Main ──────────────────────────────────────────────────────────

int main() {
    std::cout << "==============================================\n";
    std::cout << " ForgeLSM Performance Benchmark Suite\n";
    std::cout << " Workload: IoT sensor simulation\n";
    std::cout << "==============================================\n";

    bench_sequential_write();
    bench_random_write();
    bench_read_latency();
    bench_write_amplification();

    std::cout << "\n==============================================\n";
    std::cout << " Done.\n";
    std::cout << "==============================================\n";
    return 0;
}
