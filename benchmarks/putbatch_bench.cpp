// ForgeLSM Batch Write Benchmark
// Proves: PutBatch significantly outperforms sequential Puts for bulk ingestion.

#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <iomanip>

using namespace forgelsm;

const std::string DB_PATH = "/tmp/flsm_batch_bench";

void setup_db() {
    std::filesystem::remove_all(DB_PATH);
}

std::string format_key(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "batch_key_%010d", i);
    return std::string(buf);
}

double run_sequential_test(int num_writes, bool sync_writes) {
    setup_db();
    Options opts;
    opts.sync_writes = sync_writes;
    opts.vlog_shards = 4;
    
    DB* db = nullptr;
    DB::Open(opts, DB_PATH, &db);

    std::string val(256, 'x'); // 256 byte payload
    
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_writes; ++i) {
        db->Put(format_key(i), val);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    db->Close();
    delete db;

    return num_writes / diff.count(); // ops/sec
}

double run_batch_test(int num_writes, int batch_size, bool sync_writes) {
    setup_db();
    Options opts;
    opts.sync_writes = sync_writes;
    opts.vlog_shards = 4;
    
    DB* db = nullptr;
    DB::Open(opts, DB_PATH, &db);

    std::string val(256, 'x'); // 256 byte payload
    
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_writes; i += batch_size) {
        std::vector<std::pair<std::string, std::string>> batch;
        batch.reserve(batch_size);
        for (int j = 0; j < batch_size && i + j < num_writes; ++j) {
            batch.emplace_back(format_key(i + j), val);
        }
        db->PutBatch(batch);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    db->Close();
    delete db;

    return num_writes / diff.count(); // ops/sec
}

int main() {
    std::cout << "==============================================\n";
    std::cout << " ForgeLSM: Sequential vs Batch Write Benchmark\n";
    std::cout << "==============================================\n";

    const int TOTAL_WRITES = 500000;
    const int SYNC_WRITES = 10000; // lower for physical sync test
    const int BATCH_SIZE = 1000;

    std::cout << "  [Phase A] Memory / Lock Contention (sync_writes = false)\n";
    std::cout << "  Payload: 256 bytes | Total Writes: " << TOTAL_WRITES << "\n";
    
    double seq_ops = run_sequential_test(TOTAL_WRITES, false);
    std::cout << "    Sequential Put(): " << std::setw(10) << std::fixed << std::setprecision(0) << seq_ops << " ops/sec\n";
    
    double batch_ops = run_batch_test(TOTAL_WRITES, BATCH_SIZE, false);
    std::cout << "    PutBatch() [sz=" << BATCH_SIZE << "]: " << std::setw(6) << std::fixed << std::setprecision(0) << batch_ops << " ops/sec\n";
    
    std::cout << "    Performance Gain:  " << std::fixed << std::setprecision(2) << (batch_ops / seq_ops) << "x faster\n\n";

    std::cout << "  [Phase B] Physical SSD I/O (sync_writes = true)\n";
    std::cout << "  Payload: 256 bytes | Total Writes: " << SYNC_WRITES << "\n";
    
    double sync_seq_ops = run_sequential_test(SYNC_WRITES, true);
    std::cout << "    Sequential Put(): " << std::setw(10) << std::fixed << std::setprecision(0) << sync_seq_ops << " ops/sec\n";
    
    double sync_batch_ops = run_batch_test(SYNC_WRITES, BATCH_SIZE, true);
    std::cout << "    PutBatch() [sz=" << BATCH_SIZE << "]: " << std::setw(6) << std::fixed << std::setprecision(0) << sync_batch_ops << " ops/sec\n";
    
    std::cout << "    Performance Gain:  " << std::fixed << std::setprecision(2) << (sync_batch_ops / sync_seq_ops) << "x faster\n";

    std::cout << "\n==============================================\n";
    return 0;
}
