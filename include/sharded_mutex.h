#pragma once

#include <shared_mutex>
#include <array>
#include <thread>

namespace forgelsm {

class ShardedSharedMutex {
private:
    static constexpr size_t NUM_SHARDS = 16;
    // Align to 64 bytes (cache line size) to prevent false sharing
    struct alignas(64) PaddedMutex {
        std::shared_mutex m;
    };
    std::array<PaddedMutex, NUM_SHARDS> shards_;

    // All exclusive locks MUST go through lock() — never lock shards_[i] directly
public:
    void lock() {
        for (auto& shard : shards_) {
            shard.m.lock();
        }
    }

    void unlock() {
        for (auto it = shards_.rbegin(); it != shards_.rend(); ++it) {
            it->m.unlock();
        }
    }

    void lock_shared() {
        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        shards_[tid % NUM_SHARDS].m.lock_shared();
    }

    void unlock_shared() {
        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        shards_[tid % NUM_SHARDS].m.unlock_shared();
    }
};

} // namespace forgelsm
