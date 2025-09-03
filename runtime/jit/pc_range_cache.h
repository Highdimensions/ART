/*
 * Copyright 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_JIT_PC_RANGE_CACHE_H_
#define ART_RUNTIME_JIT_PC_RANGE_CACHE_H_

#include <array>
#include <atomic>
#include <cstdint>

#include "arch/instruction_set.h"
#include "base/macros.h"
#include "base/mutex.h"

namespace art HIDDEN {

class ArtMethod;
class OatQuickMethodHeader;

namespace jit {

// Fast PC-to-method lookup cache for JIT compiled code
// Uses lock-free design with architecture-specific optimizations
class PCRangeCache {
 public:
  // Cache configuration - defined as constants for compile-time computation
#if defined(__aarch64__) || defined(__x86_64__)
  // 64-bit architectures: larger cache for better hit rate
  static constexpr size_t kCacheSize = 256;
  static constexpr size_t kCacheAssociativity = 4;  // 4-way set-associative
#elif defined(__arm__) || defined(__i386__)
  // 32-bit architectures: smaller cache to reduce memory pressure
  static constexpr size_t kCacheSize = 128;
  static constexpr size_t kCacheAssociativity = 2;  // 2-way set-associative
#elif defined(__riscv)
  // RISC-V: moderate cache size
  static constexpr size_t kCacheSize = 192;
  static constexpr size_t kCacheAssociativity = 3;  // 3-way set-associative
#else
  // Default for unknown architectures
  static constexpr size_t kCacheSize = 128;
  static constexpr size_t kCacheAssociativity = 2;
#endif

  static constexpr size_t kCacheSets = kCacheSize / kCacheAssociativity;
  static_assert((kCacheSize % kCacheAssociativity) == 0, "Cache size must be divisible by associativity");

  PCRangeCache();
  ~PCRangeCache() = default;

  // Fast O(1) lookup for method header based on PC
  // Returns nullptr if not found in cache
  OatQuickMethodHeader* FastLookup(uintptr_t pc);

  // Add a new entry to the cache
  void AddEntry(const void* code_ptr, ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);

  // Invalidate cache entry for a method (used during code collection)
  void InvalidateMethod(ArtMethod* method);

  // Clear all cache entries
  void Clear();

  // Get cache statistics for debugging
  struct Stats {
    uint64_t total_lookups;
    uint64_t cache_hits;
    uint64_t cache_misses;
    double hit_rate() const {
      return total_lookups > 0 ? static_cast<double>(cache_hits) / total_lookups : 0.0;
    }
  };
  Stats GetStats() const;

 private:
  // Cache entry structure - designed to fit in single cache line
  struct alignas(64) PCRangeEntry {
    // PC range covered by this method
    std::atomic<uintptr_t> start_pc{0};
    std::atomic<uintptr_t> end_pc{0};

    // Associated method and code pointer
    std::atomic<ArtMethod*> method{nullptr};
    std::atomic<const void*> code_ptr{nullptr};

    // Generation counter for lock-free updates
    std::atomic<uint32_t> generation{0};

    // Padding to ensure cache line alignment
    uint8_t padding[64 - sizeof(std::atomic<uintptr_t>) * 2 -
                    sizeof(std::atomic<ArtMethod*>) -
                    sizeof(std::atomic<const void*>) -
                    sizeof(std::atomic<uint32_t>)];

    // Check if this entry contains the given PC
    bool Contains(uintptr_t pc) const {
      uint32_t gen = generation.load(std::memory_order_acquire);
      if (gen == 0) {
        return false;
      }

      uintptr_t start = start_pc.load(std::memory_order_relaxed);
      uintptr_t end = end_pc.load(std::memory_order_relaxed);

      return pc >= start && pc < end;
    }

    // Get code pointer if PC is in range
    const void* GetCodePtr(uintptr_t pc) const {
      if (Contains(pc)) {
        return code_ptr.load(std::memory_order_relaxed);
      }
      return nullptr;
    }

    // Clear the entry
    void Clear() {
      generation.store(0, std::memory_order_release);
      start_pc.store(0, std::memory_order_relaxed);
      end_pc.store(0, std::memory_order_relaxed);
      method.store(nullptr, std::memory_order_relaxed);
      code_ptr.store(nullptr, std::memory_order_relaxed);
    }

    // Update entry atomically
    void Update(uintptr_t start, uintptr_t end, ArtMethod* meth, const void* code) {
      uint32_t new_gen = generation.load(std::memory_order_relaxed) + 1;
      if (new_gen == 0) new_gen = 1;  // Skip 0 as it means "empty"

      start_pc.store(start, std::memory_order_relaxed);
      end_pc.store(end, std::memory_order_relaxed);
      method.store(meth, std::memory_order_relaxed);
      code_ptr.store(code, std::memory_order_relaxed);

      // Release the new generation - this makes the entry visible
      generation.store(new_gen, std::memory_order_release);
    }

    // Check if entry matches method for invalidation
    bool MatchesMethod(ArtMethod* target_method) const {
      return method.load(std::memory_order_relaxed) == target_method;
    }
  };

  // Hash function optimized for PC addresses
  size_t HashPC(uintptr_t pc) const {
    // Architecture-specific hash function
#if defined(__aarch64__)
    // ARM64: Instructions are 4-byte aligned, use upper bits
    return ((pc >> 4) ^ (pc >> 12)) % kCacheSets;
#elif defined(__arm__)
    // ARM32: Instructions are 2 or 4-byte aligned
    return ((pc >> 2) ^ (pc >> 10)) % kCacheSets;
#elif defined(__x86_64__) || defined(__i386__)
    // x86: Variable length instructions, use multiple bits
    return ((pc >> 3) ^ (pc >> 11) ^ (pc >> 19)) % kCacheSets;
#elif defined(__riscv)
    // RISC-V: 2 or 4-byte aligned instructions
    return ((pc >> 2) ^ (pc >> 10)) % kCacheSets;
#else
    // Generic hash for unknown architectures
    return ((pc >> 3) ^ (pc >> 11)) % kCacheSets;
#endif
  }

  // Find LRU entry in a set
  size_t FindLRUEntry(size_t set_index) const;

  // Cache storage - organized as set-associative cache
  std::array<PCRangeEntry, kCacheSize> cache_entries_;

  // LRU tracking - one counter per cache entry
  mutable std::array<std::atomic<uint32_t>, kCacheSize> lru_counters_;
  mutable std::atomic<uint32_t> global_lru_counter_{1};

  // Statistics - atomic for lock-free access
  mutable std::atomic<uint64_t> total_lookups_{0};
  mutable std::atomic<uint64_t> cache_hits_{0};

  DISALLOW_COPY_AND_ASSIGN(PCRangeCache);
};

}  // namespace jit
}  // namespace art

#endif  // ART_RUNTIME_JIT_PC_RANGE_CACHE_H_
