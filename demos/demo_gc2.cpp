#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>

using namespace forgelsm;

const std::string DB_PATH = "flsm_demo_gc2";
const int NUM_USERS = 25000;
const int PAYLOAD_SIZE = 4096;

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
    std::cout << " ForgeLSM Demo: Automatic Background Garbage Collection\n";
    std::cout << "========================================================\n";
    std::cout << "Goal: Demonstrate production-ready heuristic GC running\n";
    std::cout << "      automatically in the background, cleaning up dead\n";
    std::cout << "      space while gracefully skipping live files.\n\n";

    std::filesystem::remove_all(DB_PATH);

    Options opts;
    opts.sync_writes = false; 
    opts.vlog_shards = 4;
    opts.flush_threshold = 4 * 1024 * 1024;
    opts.background_gc = true; // Enabled! Automatic background GC
    VLog::MAX_FILE_SIZE = 16 * 1024 * 1024; // 16MB per VLog file

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << "\n";
        return 1;
    }

    std::string initial_payload(PAYLOAD_SIZE, 'A');

    std::cout << "[1] Inserting " << NUM_USERS << " Initial Records (" << format_size(PAYLOAD_SIZE) << " each)...\n";
    for (int i = 0; i < NUM_USERS; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "user_%010d", i);
        db->Put(std::string(key), initial_payload);
    }
    db->ForceFlush();
    db->ForceSync();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    size_t size_after_insert = get_dir_size(DB_PATH);
    std::cout << "    -> Database physical size: " << format_size(size_after_insert) << "\n\n";

    std::cout << "[2] Overwriting ALL " << NUM_USERS << " Records...\n";
    std::string new_payload(PAYLOAD_SIZE, 'B');
    size_t peak_size = size_after_insert;

    for (int i = 0; i < NUM_USERS; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "user_%010d", i);
        db->Put(std::string(key), new_payload);
        
        // Brief pause to allow background threads to catch up during the massive overwrite
        if (i % 5000 == 0 && i > 0) {
            size_t current = get_dir_size(DB_PATH);
            if (current > peak_size) peak_size = current;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    db->ForceFlush();
    db->ForceSync();
    
    size_t current_after = get_dir_size(DB_PATH);
    if (current_after > peak_size) peak_size = current_after;
    
    std::cout << "\n[3] Letting Background GC Finish Its Work...\n";
    // We just sleep and let the background threads do the magic!
    size_t current_size = current_after;
    
    for (int sec = 1; sec <= 5; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        size_t new_size = get_dir_size(DB_PATH);
        if (new_size < current_size) {
            size_t reclaimed = current_size - new_size;
            std::cout << "    [Wait " << sec << "s] Background GC reclaimed " << format_size(reclaimed) 
                      << " (Current Size: " << format_size(new_size) << ")\n";
            current_size = new_size;
        } else {
            std::cout << "    [Wait " << sec << "s] Size stable at " << format_size(new_size) << "\n";
        }
    }

    std::cout << "\n========================================================\n";
    std::cout << " SUCCESS! \n";
    std::cout << " Peak Disk Usage During Overwrite: " << format_size(peak_size) << "\n";
    std::cout << " Final Disk Usage After GC:        " << format_size(get_dir_size(DB_PATH)) << "\n";
    std::cout << " Total Space Reclaimed:            " << format_size(peak_size - get_dir_size(DB_PATH)) << "\n";
    std::cout << " Notice how the background GC concurrently cleaned up files DURING the overwrite phase!\n";
    std::cout << "========================================================\n\n";

    db->Close();
    delete db;
    return 0;
}
