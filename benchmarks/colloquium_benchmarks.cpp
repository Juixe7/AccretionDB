#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <random>
#include <iomanip>
#include <thread>
#include <algorithm>
#include <fstream>

using namespace forgelsm;

const std::string DB_PATH = "/tmp/flsm_colloquium";
const int NUM_KEYS = 1000000; // 1 Million keys (1GB dataset)
const int NUM_OPS = 100000;   // 100,000 operations per workload
const int VAL_SIZE = 1024; // 1KB value size typical for YCSB

std::string format_key(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "user%010d", i);
    return std::string(buf);
}

struct LatencyStats {
    double p50;
    double p90;
    double p99;
    double p999;
    double avg;
    double throughput;
};

LatencyStats calculate_stats(std::vector<double>& latencies_us, double total_time_s) {
    if (latencies_us.empty()) return LatencyStats{0,0,0,0,0,0};
    std::sort(latencies_us.begin(), latencies_us.end());
    size_t n = latencies_us.size();
    
    LatencyStats stats;
    stats.p50 = latencies_us[n * 0.50];
    stats.p90 = latencies_us[n * 0.90];
    stats.p99 = latencies_us[n * 0.99];
    stats.p999 = latencies_us[n * 0.999];
    
    double sum = 0;
    for (double l : latencies_us) sum += l;
    stats.avg = sum / n;
    
    stats.throughput = n / total_time_s;
    return stats;
}

void run_workload(DB* db, const std::string& name, int read_percent, int update_percent, int scan_percent, std::ofstream& csv) {
    std::cout << "\nRunning " << name << " (" << read_percent << "% Read, " 
              << update_percent << "% Update, " << scan_percent << "% Scan)...\n";
    
    std::mt19937 rng(42);
    std::uniform_int_distribution<> op_dist(1, 100);
    std::uniform_int_distribution<> key_dist(0, NUM_KEYS - 1);
    
    std::vector<double> latencies;
    latencies.reserve(NUM_OPS);
    
    std::string dummy_val(VAL_SIZE, 'x');
    
    auto workload_start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_OPS; ++i) {
        int op = op_dist(rng);
        int k = key_dist(rng);
        std::string key = format_key(k);
        
        auto op_start = std::chrono::high_resolution_clock::now();
        
        if (op <= read_percent) {
            std::string val;
            db->Get(key, &val);
        } else if (op <= read_percent + update_percent) {
            db->Put(key, dummy_val);
        } else {
            // Scan up to 50 elements
            std::string end_key = format_key(k + 50);
            std::vector<std::pair<std::string, std::string>> results;
            db->Scan(key, end_key, &results);
        }
        
        auto op_end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::micro> diff = op_end - op_start;
        latencies.push_back(diff.count());
    }
    
    auto workload_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> total_diff = workload_end - workload_start;
    
    LatencyStats stats = calculate_stats(latencies, total_diff.count());
    
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0) << stats.throughput << " ops/sec\n";
    std::cout << "  Latencies (us): Avg=" << stats.avg << ", p50=" << stats.p50 
              << ", p90=" << stats.p90 << ", p99=" << stats.p99 << ", p99.9=" << stats.p999 << "\n";
              
    csv << name << "," << stats.throughput << "," << stats.avg << "," 
        << stats.p50 << "," << stats.p90 << "," << stats.p99 << "," << stats.p999 << "\n";
}

int main() {
    std::filesystem::remove_all(DB_PATH);
    
    Options opts;
    opts.sync_writes = false; 
    opts.quiet_mode = true; // Turn off logs so we can see the benchmark results cleanly
    
    DB* db = nullptr;
    DB::Open(opts, DB_PATH, &db);
    
    std::cout << "Loading " << NUM_KEYS << " initial records (1KB each, ~1GB total). This will take a moment...\n";
    std::string val(VAL_SIZE, 'a');
    for(int i=0; i<NUM_KEYS; ++i) {
        db->Put(format_key(i), val);
    }
    
    std::cout << "Waiting for background compactions to settle...\n";
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    std::ofstream csv("colloquium_results.csv");
    csv << "Workload,Throughput(ops/s),AvgLatency(us),p50(us),p90(us),p99(us),p99.9(us)\n";
    
    std::cout << "\n=======================================================\n";
    std::cout << " ForgeLSM - Systems Engineering Benchmarks (YCSB)\n";
    std::cout << "=======================================================\n";
    
    run_workload(db, "Workload A (50-50 RW)", 50, 50, 0, csv);
    run_workload(db, "Workload B (95-5 RW)", 95, 5, 0, csv);
    run_workload(db, "Workload C (100% Read)", 100, 0, 0, csv);
    run_workload(db, "Workload E (95 Scan-5 W)", 0, 5, 95, csv);
    
    csv.close();
    
    EngineStats stats = db->GetStats();
    std::cout << "\n=======================================================\n";
    std::cout << " Amplification Metrics\n";
    std::cout << "=======================================================\n";
    std::cout << "  Write Amplification: " << std::fixed << std::setprecision(2) << stats.write_amplification << "x\n";
    
    db->Close();
    delete db;
    
    std::cout << "\nResults exported to colloquium_results.csv for graphing.\n";
    return 0;
}
