#ifndef FORGELSM_CACHE_H
#define FORGELSM_CACHE_H

#include <cstdint>
#include <string>
#include <vector>
#include <list>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace forgelsm {

// A block is just a chunk of bytes.
using Block = std::vector<char>;
using BlockPtr = std::shared_ptr<Block>;

class alignas(64) LRUShard {
public:
    LRUShard() = default;
    
    void set_capacity(size_t capacity) {
        capacity_ = capacity;
    }

    BlockPtr get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }
        // Move to front
        list_.splice(list_.begin(), list_, it->second.list_it);
        return it->second.value;
    }

    void put(const std::string& key, BlockPtr value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Update value and move to front
            it->second.value = value;
            list_.splice(list_.begin(), list_, it->second.list_it);
            // Size accounting (assuming same size roughly, or we could be precise)
        } else {
            // Insert new
            list_.push_front(key);
            map_[key] = {value, list_.begin()};
            current_size_ += value->capacity(); // Approximate memory usage

            // Evict if over capacity
            while (current_size_ > capacity_ && !list_.empty()) {
                const std::string& oldest_key = list_.back();
                auto old_it = map_.find(oldest_key);
                if (old_it != map_.end()) {
                    current_size_ -= old_it->second.value->capacity();
                    map_.erase(old_it);
                }
                list_.pop_back();
            }
        }
    }

private:
    struct Node {
        BlockPtr value;
        std::list<std::string>::iterator list_it;
    };

    std::mutex mutex_;
    size_t capacity_ = 0;
    size_t current_size_ = 0;
    std::list<std::string> list_;
    std::unordered_map<std::string, Node> map_;
};

class ShardedLRUCache {
public:
    explicit ShardedLRUCache(size_t capacity) {
        size_t per_shard = capacity / kNumShards;
        for (int i = 0; i < kNumShards; ++i) {
            shards_[i].set_capacity(per_shard);
        }
    }

    BlockPtr get(const std::string& key) {
        return shards_[hash(key)].get(key);
    }

    void put(const std::string& key, BlockPtr value) {
        shards_[hash(key)].put(key, value);
    }

private:
    static constexpr int kNumShards = 16;
    LRUShard shards_[kNumShards];

    // Simple hash function to route keys to shards
    size_t hash(const std::string& key) const {
        size_t h = std::hash<std::string>{}(key);
        return h % kNumShards;
    }
};

} // namespace forgelsm

#endif // FORGELSM_CACHE_H
