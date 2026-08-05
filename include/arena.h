#ifndef FORGELSM_ARENA_H
#define FORGELSM_ARENA_H

#include <atomic>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace forgelsm {

// A monotonic bump-pointer allocator optimized for cache locality and fast path 
// lock-free allocation. Memory is pre-allocated in large blocks, eliminating 
// heap fragmentation and per-node allocation overheads during memtable insertion.
class Arena {
public:
    Arena();
    ~Arena();

    // Disable copy/move semantics to pin block memory natively.
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Fast, thread-safe allocation using an atomic bump pointer.
    // Handles internal 8-byte alignment natively.
    char* allocate(size_t bytes);

    // Total memory allocated by the user
    size_t memory_usage() const;

private:
    char* allocate_fallback(size_t bytes);
    char* allocate_new_block(size_t block_bytes);

    // 4MB default block size balances OS page allocation overhead with memtable sizing.
    static constexpr size_t K_BLOCK_SIZE = 4096 * 1024; 
    
    struct Block {
        char* data;
        std::atomic<size_t> offset{0};
        
        explicit Block(size_t size) : data(new char[size]) {}
        ~Block() { delete[] data; }
    };
    
    // Fast path state: 
    // A single atomic pointer to the active block.
    // Each block has its own atomic offset.
    std::atomic<Block*> current_block_{nullptr};

    // Slow path state:
    // Protects vector expansion and new block allocations.
    std::mutex fallback_mutex_;
    std::vector<Block*> blocks_;
    std::atomic<size_t> memory_usage_{0};
    std::atomic<size_t> bytes_allocated_{0};
};

} // namespace forgelsm
#endif // FORGELSM_ARENA_H
