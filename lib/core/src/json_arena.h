#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "json.h"

namespace core {

// Fixed-size bump allocator for ArduinoJson, so parsing never touches the heap.
// Allocations that do not fit fail, and ArduinoJson reports NoMemory.
// Call reset() only after every JsonDocument using the arena has been destroyed.
template <size_t Capacity>
class ArenaAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    const size_t total = kHeader + round_up(size);
    if (total > Capacity - used_) {
      ++failures_;
      return nullptr;
    }
    unsigned char* block = buffer_ + used_;
    store_size(block, size);
    last_ = block;
    used_ += total;
    if (used_ > high_water_) {
      high_water_ = used_;
    }
    ++live_;
    return block + kHeader;
  }

  void deallocate(void* ptr) override {
    if (ptr == nullptr) {
      return;
    }
    --live_;
    unsigned char* block = static_cast<unsigned char*>(ptr) - kHeader;
    if (block == last_) {  // the newest block can be returned to the arena
      used_ = static_cast<size_t>(block - buffer_);
      last_ = nullptr;
    }
  }

  void* reallocate(void* ptr, size_t new_size) override {
    if (ptr == nullptr) {
      return allocate(new_size);
    }
    unsigned char* block = static_cast<unsigned char*>(ptr) - kHeader;
    const size_t old_size = load_size(block);
    if (block == last_) {
      const size_t offset = static_cast<size_t>(block - buffer_);
      const size_t total = kHeader + round_up(new_size);
      if (total > Capacity - offset) {
        ++failures_;
        return nullptr;
      }
      store_size(block, new_size);
      used_ = offset + total;
      if (used_ > high_water_) {
        high_water_ = used_;
      }
      return ptr;
    }
    if (new_size <= old_size) {
      return ptr;
    }
    void* moved = allocate(new_size);
    if (moved == nullptr) {
      return nullptr;
    }
    std::memcpy(moved, ptr, old_size);
    --live_;  // the old block is abandoned; allocate() counted the new one
    return moved;
  }

  // Position to rewind() to; documents created after it can then be discarded together.
  size_t mark() const { return used_; }

  // Frees everything allocated after `mark`. Every document that allocated after the
  // mark must already be destroyed.
  void rewind(size_t mark) {
    if (mark < used_) {
      used_ = mark;
    }
    last_ = nullptr;
  }

  // Empties the arena and its per-use statistics.
  void reset() {
    used_ = 0;
    last_ = nullptr;
    live_ = 0;
    high_water_ = 0;
    failures_ = 0;
  }

  // Peak bytes used and failed allocations since the last reset().
  size_t high_water() const { return high_water_; }
  uint32_t failures() const { return failures_; }
  // Blocks allocated and not yet freed; 0 once every document using the arena is gone.
  int32_t live_blocks() const { return live_; }

 private:
  static constexpr size_t kAlign = 8;
  static constexpr size_t kHeader = kAlign;  // keeps payloads 8-byte aligned

  static size_t round_up(size_t size) { return (size + kAlign - 1) & ~(kAlign - 1); }
  static void store_size(unsigned char* block, size_t size) { std::memcpy(block, &size, sizeof(size)); }
  static size_t load_size(const unsigned char* block) {
    size_t size = 0;
    std::memcpy(&size, block, sizeof(size));
    return size;
  }

  static_assert(sizeof(size_t) <= kHeader, "size header must fit");

  alignas(kAlign) unsigned char buffer_[Capacity];
  size_t used_ = 0;
  unsigned char* last_ = nullptr;
  size_t high_water_ = 0;
  uint32_t failures_ = 0;
  int32_t live_ = 0;
};

}  // namespace core
