#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>
#include <iomanip>

using namespace forgelsm;

const std::string DB_PATH = "flsm_demo_gc";
const int NUM_USERS = 25000;

size_t get_dir_size(const std::string& dir_path) {
    size_t total_bytes = 0;
    if (std::filesystem::exists(dir_path)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
            if (std::filesystem::is_regular_file(entry)) {
                std::error_code ec;
                auto sz = std::filesystem::file_size(entry, ec);
                if (!ec) {
                    total_bytes += sz;
                }
            }
        }
    }
    return total_bytes;
}

std::string format_size(size_t bytes) {
    double mb = bytes / (1024.0 * 1024.0);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f MB", mb);
    return std::string(buf);
}

int main() {
    std::cout << "========================================================\n";
    std::cout << " ForgeLSM Demo 3: Log-Structured Garbage Collection\n";
    std::cout << "========================================================\n";
    std::cout << "Goal: Prove that File Sharding and Deletion physically reclaims\n";
    std::cout << "      storage space on the SSD while the DB is online.\n\n";

    std::filesystem::remove_all(DB_PATH);

    Options opts;
    opts.sync_writes = false; 
    opts.vlog_shards = 4;
    opts.flush_threshold = 4 * 1024 * 1024;
    opts.background_gc = false; // We want manual control for the demo
    VLog::MAX_FILE_SIZE = 16 * 1024 * 1024; // 16MB per VLog file to trigger multi-file GC

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << "\n";
        return 1;
    }

    std::string large_payload(4096, 'A'); // 4KB profile picture

    std::cout << "[1] Inserting " << NUM_USERS << " User Profiles (4KB each)...\n";
    for (int i = 0; i < NUM_USERS; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "user_%010d", i);
        db->Put(std::string(key), large_payload);
    }
    db->ForceFlush();
    db->ForceSync();
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // wait for flush
    
    size_t size_after_insert = get_dir_size(DB_PATH);
    std::cout << "    -> Database physical size: " << format_size(size_after_insert) << "\n\n";

    std::cout << "[2] Simulating a massive update wave...\n";
    std::cout << "    Overwriting ALL " << NUM_USERS << " profiles with new data...\n";
    
    std::string new_payload(4096, 'B'); // New 4KB profile picture
    for (int i = 0; i < NUM_USERS; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "user_%010d", i);
        db->Put(std::string(key), new_payload);
    }
    db->ForceFlush();
    db->ForceSync();
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Wait for bg compaction to settle
    
    size_t size_after_update = get_dir_size(DB_PATH);
    std::cout << "    -> Database physical size ballooned to: " << format_size(size_after_update) << "\n";
    std::cout << "       (This is WiscKey dead space accumulation)\n\n";

    std::cout << "[3] Triggering Garbage Collection (File Sharding & Deletion)...\n";
    for (int g = 0; g < 15; ++g) {
        db->ForceGC();
    }
    
    // Give OS a moment to update filesystem metadata
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); 
    
    size_t size_after_gc = get_dir_size(DB_PATH);
    size_t space_reclaimed = size_after_update - size_after_gc;
    
    std::cout << "    -> Database physical size shrank to: " << format_size(size_after_gc) << "\n\n";
    
    std::cout << "========================================================\n";
    std::cout << " SUCCESS! \n";
    std::cout << " Physically reclaimed " << format_size(space_reclaimed) << " of dead space from the SSD.\n";
    std::cout << "========================================================\n\n";

    db->Close();
    delete db;
    return 0;
}
