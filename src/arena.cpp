#include "arena.h"
#include <cassert>
#include <new>

namespace forgelsm {

Arena::Arena() {
    // We defer the first block allocation until the first actual allocate() call
    // to keep the constructor lightweight and fast.
}

Arena::~Arena() {
    for (Block* block : blocks_) {
        delete block;
    }
}

char* Arena::allocate(size_t bytes) {
    // Ensure strict 8-byte alignment for all allocations. This prevents undefined
    // behavior and performance penalties when reading primitives (like uint64_t) 
    // from the returned memory, especially on ARM architectures.
    const size_t align = 8;
    // Round up bytes to a multiple of align (8)
    size_t aligned_bytes = (bytes + align - 1) & ~(align - 1);
    bytes_allocated_.fetch_add(aligned_bytes, std::memory_order_relaxed);

    // Fast path: Atomic bump allocation.
    Block* block = current_block_.load(std::memory_order_acquire);
    if (block != nullptr) {
        size_t current_offset = block->offset.fetch_add(aligned_bytes, std::memory_order_relaxed);
        if (current_offset + aligned_bytes <= K_BLOCK_SIZE) {
            return block->data + current_offset;
        }
    }
    
    // Fast path failed, fallback to allocating a new block or finding space.
    return allocate_fallback(aligned_bytes);
}

char* Arena::allocate_fallback(size_t bytes) {
    // Only one thread should handle allocating new blocks.
    std::lock_guard<std::mutex> lock(fallback_mutex_);

    // Re-check the fast path state after acquiring the lock in case another 
    // thread already allocated a new block.
    Block* block = current_block_.load(std::memory_order_relaxed);
    if (block != nullptr) {
        // We do not use the fast path's fetch_add here because we are holding the lock,
        // but it's safe to just do fetch_add and check.
        size_t current_offset = block->offset.fetch_add(bytes, std::memory_order_relaxed);
        if (current_offset + bytes <= K_BLOCK_SIZE) {
            return block->data + current_offset;
        }
    }

    // Still not enough space. 
    // If the requested size is larger than a quarter of our standard block size, 
    // allocate a bespoke block for it to avoid wasting the current standard block.
    if (bytes > K_BLOCK_SIZE / 4) {
        char* result = allocate_new_block(bytes);
        return result;
    }

    // Allocate a new standard block.
    // We use allocate_new_block to actually allocate and register the memory.
    // But allocate_new_block just returns a char pointer.
    // Wait, allocate_new_block needs to be refactored to work with Block*.
    
    Block* new_block = new Block(K_BLOCK_SIZE);
    blocks_.push_back(new_block);
    memory_usage_.fetch_add(K_BLOCK_SIZE + sizeof(Block*), std::memory_order_relaxed);
    
    // Claim the bytes immediately
    size_t current_offset = new_block->offset.fetch_add(bytes, std::memory_order_relaxed);
    char* result = new_block->data + current_offset;
    
    // Publish the new block
    current_block_.store(new_block, std::memory_order_release);
    return result;
}

char* Arena::allocate_new_block(size_t block_bytes) {
    // Bespoke block allocation
    Block* block = new Block(block_bytes);
    blocks_.push_back(block);
    memory_usage_.fetch_add(block_bytes + sizeof(Block*), std::memory_order_relaxed);
    
    // For bespoke block, we just claim the entire size immediately.
    block->offset.store(block_bytes, std::memory_order_relaxed);
    return block->data;
}

size_t Arena::memory_usage() const {
    return bytes_allocated_.load(std::memory_order_relaxed);
}

} // namespace forgelsm
