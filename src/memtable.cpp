#include "memtable.h"

Memtable::Memtable() 
    : arena_(std::make_unique<forgelsm::Arena>()),
      table_(std::make_unique<forgelsm::ConcurrentSkipList>(arena_.get())) {}

Memtable::~Memtable() = default;

void Memtable::put(std::string_view key, const VLogPointer& pointer) {
    table_->put(key, pointer);
}

bool Memtable::get(std::string_view key, VLogPointer& out_pointer) const {
    return table_->get(key, out_pointer);
}

size_t Memtable::byte_size() const {
    // The arena accurately reflects total memory footprint including nodes, 
    // keys, array overhead, and allocated block overhead.
    return arena_->memory_usage();
}

Memtable::Iterator Memtable::begin() const {
    return table_->begin();
}
