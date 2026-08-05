#include "forgelsm.h"
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <filesystem>

using namespace forgelsm;

void print_help() {
    std::cout << "\n========================================================\n";
    std::cout << " ForgeLSM Interactive CLI\n";
    std::cout << "========================================================\n";
    std::cout << "  put <key> <value>             : Insert or update a key\n";
    std::cout << "  get <key>                     : Retrieve a key\n";
    std::cout << "  delete <key>                  : Delete a key\n";
    std::cout << "  scan <start> <end>            : Range scan for keys\n";
    std::cout << "  batch <k1> <v1> <k2> <v2> ... : Atomic batch insert\n";
    std::cout << "  help                          : Show this menu\n";
    std::cout << "  exit                          : Safely shutdown and exit\n";
    std::cout << "========================================================\n";
}

int main() {
    std::string DB_PATH = "flsm_interactive";
    
    // We will not remove the database here so that if you exit and restart,
    // your data is still there! This perfectly demonstrates persistence.
    // std::filesystem::remove_all(DB_PATH); 

    Options opts;
    opts.sync_writes = true; 
    opts.quiet_mode = false; // Allow them to see background flushes!

    DB* db = nullptr;
    Status s = DB::Open(opts, DB_PATH, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << "\n";
        return 1;
    }

    print_help();

    std::string line;
    while (true) {
        std::cout << "\nflsm> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "exit" || cmd == "quit") {
            break;
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "put") {
            std::string key, val;
            ss >> key;
            std::getline(ss, val);
            if (!val.empty() && val[0] == ' ') val = val.substr(1); // Trim leading space
            if (key.empty() || val.empty()) {
                std::cout << "Usage: put <key> <value>\n";
            } else {
                db->Put(key, val);
                std::cout << "OK.\n";
            }
        } else if (cmd == "get") {
            std::string key, val;
            ss >> key;
            if (key.empty()) {
                std::cout << "Usage: get <key>\n";
            } else {
                Status status = db->Get(key, &val);
                if (status.ok()) std::cout << val << "\n";
                else std::cout << "NOT FOUND\n";
            }
        } else if (cmd == "delete") {
            std::string key;
            ss >> key;
            if (key.empty()) {
                std::cout << "Usage: delete <key>\n";
            } else {
                db->Delete(key);
                std::cout << "OK.\n";
            }
        } else if (cmd == "scan") {
            std::string start, end;
            ss >> start >> end;
            if (start.empty() || end.empty()) {
                std::cout << "Usage: scan <start_key> <end_key>\n";
            } else {
                std::vector<std::pair<std::string, std::string>> results;
                db->Scan(start, end, &results);
                for (const auto& kv : results) {
                    std::cout << "  " << kv.first << " : " << kv.second << "\n";
                }
                std::cout << results.size() << " rows returned.\n";
            }
        } else if (cmd == "batch") {
            std::vector<std::pair<std::string, std::string>> pairs;
            std::string k, v;
            while (ss >> k >> v) {
                pairs.emplace_back(k, v);
            }
            if (pairs.empty()) {
                std::cout << "Usage: batch <k1> <v1> <k2> <v2> ...\n";
            } else {
                db->PutBatch(pairs);
                std::cout << "Batch inserted " << pairs.size() << " keys atomically.\n";
            }
        } else {
            std::cout << "Unknown command. Type 'help' for options.\n";
        }
    }

    std::cout << "\nShutting down ForgeLSM safely...\n";
    db->Close();
    delete db;
    return 0;
}
