#include "forgelsm.h"
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <atomic>
#include <random>
#include <fstream>

using namespace forgelsm;

const std::string DB_PATH = "flsm_demo_crypto";
std::atomic<bool> done{false};
std::atomic<int> scan_count{0};
std::atomic<int> total_inserts{0};

// Helper to format a double to 2 decimal places as a string
std::string format_price(double price) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", price);
    return std::string(buf);
}

std::string format_vol(double vol) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", vol);
    return std::string(buf);
}

// Generates random limit orders
void market_maker_thread(DB* db, const std::string& ticker, double base_price) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<> price_dist(base_price, base_price * 0.005); // 0.5% volatility
    std::uniform_real_distribution<> vol_dist(0.01, 15.5); // Realistic random ETH/BTC volumes
    std::uniform_int_distribution<> side_dist(0, 1);
    
    int order_id = 0;
    while (!done.load()) {
        double price = price_dist(gen);
        double volume = vol_dist(gen);
        bool is_bid = (side_dist(gen) == 0);
        
        // Keys: ETH_USD_BID_3000.50_0000000001
        char key_buf[128];
        snprintf(key_buf, sizeof(key_buf), "%s_%s_%010.2f_%010d", 
                 ticker.c_str(), (is_bid ? "BID" : "ASK"), price, order_id++);
                 
        db->Put(std::string(key_buf), format_vol(volume)); 
        total_inserts++;
        
        // Simulate high-frequency market activity delay
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// Matching engine continuously scans the order book
void matching_engine_thread(DB* db) {
    auto last_print_time = std::chrono::steady_clock::now();
    
    while (!done.load()) {
        std::vector<std::pair<std::string, std::string>> bids;
        std::vector<std::pair<std::string, std::string>> asks;
        
        // Scan all Bids and Asks for ETH
        db->Scan("ETH_USD_BID_0000000.00", "ETH_USD_BID_9999999.99", &bids);
        db->Scan("ETH_USD_ASK_0000000.00", "ETH_USD_ASK_9999999.99", &asks);
        
        scan_count++;
        
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_print_time).count();
        
        // Print the table as a snapshot every 3 seconds instead of spamming the screen
        if (duration >= 3) {
            last_print_time = now;
            
            std::cout << "\n=================================================================\n";
            std::cout << " [MARKET SNAPSHOT] ForgeLSM Crypto Order Book \n";
            std::cout << "=================================================================\n";
            
            std::cout << "  --- ASKS (Sellers) ---\n";
            for (size_t i = 0; i < std::min<size_t>(5, asks.size()); ++i) {
                std::cout << "  " << asks[i].first << " : " << asks[i].second << " ETH\n";
            }
            
            std::cout << "  ----------------------\n";
            std::cout << "         SPREAD         \n";
            std::cout << "  ----------------------\n";
            
            if (!bids.empty()) {
                size_t start = (bids.size() > 5) ? bids.size() - 5 : 0;
                for (size_t i = bids.size(); i > start; --i) {
                    std::cout << "  " << bids[i-1].first << " : " << bids[i-1].second << " ETH\n";
                }
            }
            std::cout << "  --- BIDS (Buyers)  ---\n";
            std::cout << "=================================================================\n\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    std::filesystem::remove_all(DB_PATH);

    Options opts;
    opts.sync_writes = false; 
    opts.vlog_shards = 4;
    opts.quiet_mode = false; // We WANT to see the engine's background flushes!

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << "\n";
        return 1;
    }

    std::cout << "======================================================================\n";
    std::cout << " ForgeLSM Demo 2: Real-time Crypto Exchange Engine\n";
    std::cout << "======================================================================\n";
    std::cout << " Architecture Overview:\n";
    std::cout << "  - Writers (Market Makers) generate limit orders and inject them into\n";
    std::cout << "    the lock-free SkipList Memtable at maximum speed.\n";
    std::cout << "  - Background Threads silently Flush the Memtables and Compact the\n";
    std::cout << "    LSM-Tree on disk. You will see these logs interweaved naturally.\n";
    std::cout << "  - Readers (Matching Engine) constantly scan the full Order Book.\n";
    std::cout << "  - MVCC Guarantee: The Background Compactions and the Active Writers\n";
    std::cout << "    NEVER block the Matching Engine from getting a consistent scan!\n";
    std::cout << "======================================================================\n\n";

    // Spawn High-Frequency Market Makers
    std::thread market_maker_1(market_maker_thread, db, "ETH_USD", 3000.0);
    std::thread market_maker_2(market_maker_thread, db, "ETH_USD", 3000.0);
    
    // Spawn the matching engine that scans the database
    std::thread matching_engine(matching_engine_thread, db);

    // Run for 10 seconds
    std::this_thread::sleep_for(std::chrono::seconds(10));
    
    done.store(true);
    market_maker_1.join();
    market_maker_2.join();
    matching_engine.join();

    std::cout << "\n======================================================================\n";
    std::cout << " Demo Complete. Final Telemetry:\n";
    std::cout << "======================================================================\n";
    std::cout << "  Total Limit Orders Processed : " << total_inserts.load() << "\n";
    std::cout << "  Total Full Order Book Scans  : " << scan_count.load() << "\n";
    std::cout << "  Write & Read threads successfully ran in parallel without deadlocks.\n";
    std::cout << "======================================================================\n\n";

    // Show a specific, targeted sub-range scan to prove we can scan anything
    std::vector<std::pair<std::string, std::string>> narrow_scan;
    db->Scan("ETH_USD_BID_0003000.00", "ETH_USD_BID_0003001.00", &narrow_scan);
    std::cout << " [Targeted Range Scan] Searching specifically for Bids between $3000.00 and $3001.00...\n";
    std::cout << " Found " << narrow_scan.size() << " orders in that exact $1.00 range! (Printing top 5):\n";
    for (size_t i = 0; i < std::min<size_t>(5, narrow_scan.size()); ++i) {
        std::cout << "    -> " << narrow_scan[i].first << " : " << narrow_scan[i].second << " ETH\n";
    }

    // Dump EVERYTHING to a file so the user can verify all 90,000+ matches
    std::vector<std::pair<std::string, std::string>> all_bids;
    std::vector<std::pair<std::string, std::string>> all_asks;
    db->Scan("ETH_USD_BID_0000000.00", "ETH_USD_BID_9999999.99", &all_bids);
    db->Scan("ETH_USD_ASK_0000000.00", "ETH_USD_ASK_9999999.99", &all_asks);
    
    std::ofstream dump_file("final_order_book_dump.txt");
    dump_file << "================ FULL ORDER BOOK DUMP ================\n";
    dump_file << "Total Bids: " << all_bids.size() << "\n";
    for (const auto& bid : all_bids) dump_file << bid.first << " : " << bid.second << "\n";
    dump_file << "\nTotal Asks: " << all_asks.size() << "\n";
    for (const auto& ask : all_asks) dump_file << ask.first << " : " << ask.second << "\n";
    dump_file.close();

    std::cout << "\n [Full Dump] Successfully wrote all " << (all_bids.size() + all_asks.size()) 
              << " matching orders to 'final_order_book_dump.txt'!\n\n";

    db->Close();
    delete db;
    return 0;
}
