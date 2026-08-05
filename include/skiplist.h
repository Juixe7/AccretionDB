#ifndef FORGELSM_SKIPLIST_H
#define FORGELSM_SKIPLIST_H

#include "arena.h"
#include "vlog.h"
#include <atomic>
#include <string_view>
#include <cstdint>

namespace forgelsm {

// A highly concurrent SkipList optimized for an LSM-tree memtable.
// 
// Concurrency Model (Spinlock-Write + Lock-Free Read):
// - Memtables in LSMs are ephemeral and immutable once flushed. We rely on the Arena 
//   to reclaim memory en-masse upon memtable destruction. This completely eliminates 
//   ABA problems and the need for complex Garbage Collection (like Hazard Pointers).
// - READS (`get`, `Iterator`): 100% Lock-Free. Readers traverse the list using 
//   `std::memory_order_acquire` to safely observe forward pointers inserted by writers.
// - WRITES (`put`): Serialized via a lightweight spinlock. This drastically simplifies 
//   the multi-level CAS logic while preserving extremely high read-throughput, 
//   mirroring canonical designs like LevelDB.
class ConcurrentSkipList {
private:
    struct Node {
        std::atomic<VLogPointer> value;
        uint32_t key_length;
        
        // Storing height explicitly to know the array bounds.
        uint32_t height;

        // Flexible array member: atomic pointers physically follow the node struct.
        // We use size 1 as a placeholder, but allocate exact bytes via Arena.
        std::atomic<Node*> next[1]; 

        // The key string bytes are stored immediately after the `next` array in the Arena.
        // This yields massive cache locality compared to pointer chasing.
        std::string_view key() const {
            return std::string_view(
                reinterpret_cast<const char*>(&next[0] + height), 
                key_length
            );
        }
    };

public:
    explicit ConcurrentSkipList(Arena* arena);
    
    // Disable copy/move
    ConcurrentSkipList(const ConcurrentSkipList&) = delete;
    ConcurrentSkipList& operator=(const ConcurrentSkipList&) = delete;

    // Lock-free forward iteration (safe concurrently with inserts)
    class Iterator {
    public:
        explicit Iterator(const ConcurrentSkipList* list);
        
        bool valid() const;
        void next();
        std::string_view key() const;
        VLogPointer value() const;
        void seek_to_first();
        void seek(std::string_view target);

    private:
        const ConcurrentSkipList* list_;
        Node* node_;
    };

    void put(std::string_view key, const VLogPointer& pointer);
    bool get(std::string_view key, VLogPointer& out_pointer) const;

    Iterator begin() const { return Iterator(this); }

private:
    Node* allocate_node(std::string_view key, const VLogPointer& ptr, int height);
    int random_height();
    
    // Returns the node at or immediately before the key at each level.
    // Fills `prev` array if not null. Returns true if exact key is found.
    Node* find_greater_or_equal(std::string_view key, Node** prev) const;
    bool key_is_after_node(std::string_view key, Node* n) const;

    static constexpr int K_MAX_HEIGHT = 12;
    Arena* arena_;
    Node* const head_;
    
    // The current maximum height of any node in the list.
    // Read lock-free, written under the spinlock.
    std::atomic<int> max_height_;

    // Lightweight spinlock to serialize writes.
    // Reads (Get, Iteration) DO NOT acquire this lock.
    std::atomic_flag write_lock_ = ATOMIC_FLAG_INIT;
};

} // namespace forgelsm

#endif // FORGELSM_SKIPLIST_H
