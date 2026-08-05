// ForgeLSM Chaos Monkey Target
// Continuously inserts keys, flushing to stdout to guarantee durability testing.

#include "forgelsm.h"
#include <iostream>
#include <string>

using namespace forgelsm;

const std::string DB_PATH = "/tmp/flsm_chaos";

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [run|verify] <start_idx|target_idx>" << std::endl;
        return 1;
    }

    std::string mode = argv[1];
    
    Options opts;
    opts.sync_writes = true; // IMPORTANT: Force fdatasync on every write for physical SSD survival
    opts.vlog_shards = 4;
    
    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        return 1;
    }

    if (mode == "run") {
        int start = 0;
        if (argc >= 3) start = std::stoi(argv[2]);
        
        for (int i = start; ; ++i) {
            std::string key = "chaos_key_" + std::to_string(i);
            std::string val = "chaos_val_" + std::to_string(i);
            
            s = db->Put(key, val);
            if (!s.ok()) {
                std::cerr << "Put failed: " << s.ToString() << std::endl;
                break;
            }
            
            // Print the key index that was JUST safely persisted to SSD
            // std::endl forces a flush of the stdout buffer, so the bash script sees it immediately.
            std::cout << i << std::endl;
        }
    } else if (mode == "verify") {
        if (argc < 3) {
            std::cerr << "verify mode requires target index" << std::endl;
            delete db;
            return 1;
        }
        
        int target = std::stoi(argv[2]);
        int missing = 0;
        
        for (int i = 0; i <= target; ++i) {
            std::string key = "chaos_key_" + std::to_string(i);
            std::string expected_val = "chaos_val_" + std::to_string(i);
            std::string val;
            
            if (!db->Get(key, &val).ok()) {
                missing++;
                std::cerr << "MISSING KEY: " << key << std::endl;
            } else if (val != expected_val) {
                missing++;
                std::cerr << "CORRUPT VAL: " << key << " (got: " << val << ")" << std::endl;
            }
        }
        
        std::cout << "MISSING: " << missing << std::endl;
    }
    
    delete db;
    return 0;
}
