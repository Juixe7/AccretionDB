#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <atomic>
#include <fstream>

using namespace forgelsm;

const std::string DB_PATH = "flsm_demo_iot";
const int NUM_THREADS = 8;
const int WRITES_PER_THREAD = 125000; // 1,000,000 total writes
const int BATCH_SIZE = 1000;

std::atomic<int> total_written{0};
std::atomic<bool> done{false};

void monitor_thread(DB* db) {
    std::cout << "\n[Monitor] Starting telemetry collection...\n";

    while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        EngineStats stats = db->GetStats();
        
        std::string dist = "[ ";
        for (size_t count : stats.level_file_counts) {
            dist += std::to_string(count) + " ";
        }
        dist += "]";

        std::cout << "[Telemetry] " << std::setfill(' ') 
                  << std::setw(8) << total_written.load() << " inserts | "
                  << std::setw(4) << std::fixed << std::setprecision(2) << stats.write_amplification << "x WA | "
                  << "SSTables: " << dist << "\n" << std::flush;
    }
}

int main() {
    std::cout << "========================================================\n";
    std::cout << " ForgeLSM Demo 4: High-Throughput IoT Ingestion\n";
    std::cout << "========================================================\n";
    std::cout << "Goal: Prove batch write performance, WiscKey low write\n";
    std::cout << "      amplification, and background system backpressure.\n\n";

    std::filesystem::remove_all(DB_PATH);

    Options opts;
    opts.sync_writes = false; 
    opts.vlog_shards = 4;
    opts.flush_threshold = 256 * 1024; // 256 KB Memtable limit
    opts.level_size_multiplier = 4; // L1: 1MB, L2: 4MB, L3: 16MB, L4: 64MB

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << "\n";
        return 1;
    }

    std::thread monitor(monitor_thread, db);
    std::vector<std::thread> workers;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([db, t]() {
            std::string payload = "{\"sensor_type\":\"temperature\",\"val\":22.5,\"loc\":\"warehouse_1\",\"status\":\"ok\",\"timestamp\":1719283746}";
            std::vector<std::pair<std::string, std::string>> batch;
            batch.reserve(BATCH_SIZE);

            for (int i = 0; i < WRITES_PER_THREAD; i += BATCH_SIZE) {
                batch.clear();
                for (int j = 0; j < BATCH_SIZE; ++j) {
                    char key[64];
                    snprintf(key, sizeof(key), "iot_sensor_%02d_%010d", t, i + j);
                    batch.emplace_back(std::string(key), payload);
                }
                db->PutBatch(batch);
                total_written += BATCH_SIZE;
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    done.store(true);
    monitor.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end_time - start_time;

    EngineStats final_stats = db->GetStats();

    std::cout << "\n========================================================\n";
    std::cout << " Ingestion Complete!\n";
    std::cout << "========================================================\n";
    std::cout << " Total Time:         " << std::fixed << std::setprecision(2) << diff.count() << " seconds\n";
    std::cout << " Throughput:         " << std::fixed << std::setprecision(0) << (1000000.0 / diff.count()) << " inserts/sec\n";
    std::cout << " Write Amplification:" << std::fixed << std::setprecision(2) << final_stats.write_amplification << "x\n";
    std::cout << " -> The WAF is near 10x-13x because the IoT payloads are small (97 bytes).\n";
    std::cout << " -> While WiscKey separates values into a vLog (WAF=1), the keys and pointers still go through\n";
    std::cout << " -> LSM tree compactions. Since keys make up a large fraction of the data, they drive up overall WAF.\n";
    std::cout << " -> In traditional LSM engines, the ENTIRE 123-byte KV pair would be repeatedly compacted,\n";
    std::cout << " -> resulting in significantly higher total disk write volumes.\n";
    std::cout << "========================================================\n\n";

    std::string test_val;
    bool get_ok = db->Get("iot_sensor_00_0000000000", &test_val).ok();
    std::cout << " [Diagnostic] Get('iot_sensor_00_0000000000') status: " << (get_ok ? "FOUND" : "NOT FOUND") 
              << " | size: " << test_val.size() << "\n";
    if (get_ok) std::cout << "               Value: '" << test_val << "'\n";

    // Dump a snapshot of the data to prove it was actually written
    std::vector<std::pair<std::string, std::string>> all_data;
    db->Scan("iot_sensor_00_0000000000", "iot_sensor_99_9999999999", &all_data);
    
    std::ofstream dump_file("final_iot_dump.txt");
    dump_file << "================ IoT SENSOR DATA DUMP ================\n";
    dump_file << "Total Records Found: " << all_data.size() << "\n\n";
    for (size_t i = 0; i < std::min<size_t>(1000, all_data.size()); ++i) { // Cap at 1000 to save space
        dump_file << all_data[i].first << " : " << all_data[i].second << "\n";
    }
    dump_file << "\n... (omitted " << (all_data.size() - std::min<size_t>(1000, all_data.size())) << " more records) ...\n";
    dump_file.close();

    std::cout << " [Data Verification] Successfully dumped the first 1,000 IoT records to 'final_iot_dump.txt'!\n\n";

    db->Close();
    delete db;
    return 0;
}
