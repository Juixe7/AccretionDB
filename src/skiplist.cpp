#include "skiplist.h"
#include <cassert>
#include <cstring>
#include <random>

namespace forgelsm {

ConcurrentSkipList::ConcurrentSkipList(Arena* arena) 
    : arena_(arena), 
      // Head node key is empty, height is K_MAX_HEIGHT
      head_(allocate_node("", VLogPointer{0,0,0}, K_MAX_HEIGHT)),
      max_height_(1) {
    
    // Initialize head next pointers to nullptr
    for (int i = 0; i < K_MAX_HEIGHT; i++) {
        head_->next[i].store(nullptr, std::memory_order_relaxed);
    }
}

ConcurrentSkipList::Node* ConcurrentSkipList::allocate_node(std::string_view key, const VLogPointer& ptr, int height) {
    // Layout: [Node fields] + [atomic<Node*> array of size height] + [key bytes]
    // The node struct already includes next[1], so we add (height - 1) * sizeof(atomic)
    size_t array_bytes = (height > 1) ? (height - 1) * sizeof(std::atomic<Node*>) : 0;
    size_t node_size = sizeof(Node) + array_bytes + key.size();
    
    char* mem = arena_->allocate(node_size);
    Node* result = reinterpret_cast<Node*>(mem);
    
    std::construct_at(&result->value, ptr);
    result->key_length = static_cast<uint32_t>(key.size());
    result->height = static_cast<uint32_t>(height);
    
    for (int i = 0; i < height; i++) {
        std::construct_at(&result->next[i], nullptr);
    }
    
    // Copy the key bytes directly into the arena immediately following the pointer array.
    char* key_dest = reinterpret_cast<char*>(&result->next[0] + height);
    std::memcpy(key_dest, key.data(), key.size());
    
    return result;
}

int ConcurrentSkipList::random_height() {
    // Increase height with probability 1/4
    static const unsigned int kBranching = 4;
    int height = 1;
    while (height < K_MAX_HEIGHT && ((std::rand() % kBranching) == 0)) {
        height++;
    }
    return height;
}

bool ConcurrentSkipList::key_is_after_node(std::string_view key, Node* n) const {
    // null node is considered infinite (after all keys)
    return (n != nullptr) && (n->key() < key);
}

ConcurrentSkipList::Node* ConcurrentSkipList::find_greater_or_equal(std::string_view key, Node** prev) const {
    Node* x = head_;
    int level = max_height_.load(std::memory_order_acquire) - 1;
    
    while (true) {
        // memory_order_acquire ensures we safely observe the fully constructed 
        // node that was published by a writer's memory_order_release.
        Node* next = x->next[level].load(std::memory_order_acquire);
        
        if (key_is_after_node(key, next)) {
            // Keep searching in this level
            x = next;
        } else {
            // Key is less than or equal to next's key. 
            // We must drop down a level or we have found the target.
            if (prev != nullptr) {
                prev[level] = x;
            }
            if (level == 0) {
                return next;
            } else {
                level--;
            }
        }
    }
}

bool ConcurrentSkipList::get(std::string_view key, VLogPointer& out_pointer) const {
    Node* node = find_greater_or_equal(key, nullptr);
    if (node != nullptr && node->key() == key) {
        out_pointer = node->value.load(std::memory_order_acquire);
        return true;
    }
    return false;
}

void ConcurrentSkipList::put(std::string_view key, const VLogPointer& pointer) {
    Node* prev[K_MAX_HEIGHT];
    
    // Acquire the spinlock to serialize writes.
    while (write_lock_.test_and_set(std::memory_order_acquire)) {
        // Spin
    }

    Node* node = find_greater_or_equal(key, prev);

    if (node != nullptr && node->key() == key) {
        node->value.store(pointer, std::memory_order_release);
        write_lock_.clear(std::memory_order_release);
        return;
    }

    int height = random_height();
    int current_max = max_height_.load(std::memory_order_relaxed);
    if (height > current_max) {
        for (int i = current_max; i < height; i++) {
            prev[i] = head_;
        }
        max_height_.store(height, std::memory_order_relaxed);
    }

    Node* new_node = allocate_node(key, pointer, height);

    for (int i = 0; i < height; i++) {
        new_node->next[i].store(prev[i]->next[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        prev[i]->next[i].store(new_node, std::memory_order_release);
    }

    write_lock_.clear(std::memory_order_release);
}

// --- Iterator Implementation ---

ConcurrentSkipList::Iterator::Iterator(const ConcurrentSkipList* list) 
    : list_(list), node_(nullptr) {
    seek_to_first();
}

bool ConcurrentSkipList::Iterator::valid() const {
    return node_ != nullptr;
}

void ConcurrentSkipList::Iterator::seek_to_first() {
    node_ = list_->head_->next[0].load(std::memory_order_acquire);
}

void ConcurrentSkipList::Iterator::next() {
    assert(valid());
    node_ = node_->next[0].load(std::memory_order_acquire);
}

std::string_view ConcurrentSkipList::Iterator::key() const {
    assert(valid());
    return node_->key();
}

VLogPointer ConcurrentSkipList::Iterator::value() const {
    assert(valid());
    return node_->value.load(std::memory_order_acquire);
}

void ConcurrentSkipList::Iterator::seek(std::string_view target) {
    node_ = list_->find_greater_or_equal(target, nullptr);
}

} // namespace forgelsm
