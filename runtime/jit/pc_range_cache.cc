/*
 * Copyright 2024 The Android Open Source Project
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

#include "pc_range_cache.h"

#include "art_method-inl.h"
#include "base/logging.h"
#include "base/utils.h"
#include "oat/oat_quick_method_header.h"

namespace art HIDDEN {
namespace jit {

PCRangeCache::PCRangeCache() {
  // Initialize all cache entries as empty
  for (auto& entry : cache_entries_) {
    entry.Clear();
  }

  // Initialize LRU counters
  for (auto& counter : lru_counters_) {
    counter.store(0, std::memory_order_relaxed);
  }
}

OatQuickMethodHeader* PCRangeCache::FastLookup(uintptr_t pc) {
  total_lookups_.fetch_add(1, std::memory_order_relaxed);

  size_t set_index = HashPC(pc);
  size_t base_index = set_index * kCacheAssociativity;

  // Search through all ways in the set
  for (size_t way = 0; way < kCacheAssociativity; ++way) {
    size_t index = base_index + way;
    const PCRangeEntry& entry = cache_entries_[index];

    const void* code_ptr = entry.GetCodePtr(pc);
    if (code_ptr != nullptr) {
      // Update LRU - use relaxed ordering for performance
      uint32_t new_lru = global_lru_counter_.fetch_add(1, std::memory_order_relaxed);
      lru_counters_[index].store(new_lru, std::memory_order_relaxed);

      cache_hits_.fetch_add(1, std::memory_order_relaxed);
      return OatQuickMethodHeader::FromCodePointer(code_ptr);
    }
  }

  // Cache miss
  return nullptr;
}

void PCRangeCache::AddEntry(const void* code_ptr, ArtMethod* method) {
  DCHECK(code_ptr != nullptr);
  DCHECK(method != nullptr);

  OatQuickMethodHeader* header = OatQuickMethodHeader::FromCodePointer(code_ptr);
  uintptr_t start_pc = reinterpret_cast<uintptr_t>(header->GetCode());
  uintptr_t end_pc = start_pc + header->GetCodeSize();

  size_t set_index = HashPC(start_pc);
  size_t base_index = set_index * kCacheAssociativity;

  // Try to find an empty slot first
  for (size_t way = 0; way < kCacheAssociativity; ++way) {
    size_t index = base_index + way;
    PCRangeEntry& entry = cache_entries_[index];

    if (entry.generation.load(std::memory_order_acquire) == 0) {
      entry.Update(start_pc, end_pc, method, code_ptr);
      uint32_t new_lru = global_lru_counter_.fetch_add(1, std::memory_order_relaxed);
      lru_counters_[index].store(new_lru, std::memory_order_relaxed);
      return;
    }
  }

  // No empty slot, evict LRU entry
  size_t lru_index = FindLRUEntry(set_index);
  PCRangeEntry& entry = cache_entries_[lru_index];
  entry.Update(start_pc, end_pc, method, code_ptr);

  uint32_t new_lru = global_lru_counter_.fetch_add(1, std::memory_order_relaxed);
  lru_counters_[lru_index].store(new_lru, std::memory_order_relaxed);
}

size_t PCRangeCache::FindLRUEntry(size_t set_index) const {
  size_t base_index = set_index * kCacheAssociativity;
  size_t lru_index = base_index;
  uint32_t oldest_lru = lru_counters_[base_index].load(std::memory_order_relaxed);

  for (size_t way = 1; way < kCacheAssociativity; ++way) {
    size_t index = base_index + way;
    uint32_t current_lru = lru_counters_[index].load(std::memory_order_relaxed);

    if (current_lru < oldest_lru) {
      oldest_lru = current_lru;
      lru_index = index;
    }
  }

  return lru_index;
}

void PCRangeCache::InvalidateMethod(ArtMethod* method) {
  if (method == nullptr) {
    return;
  }

  // Scan all cache entries and invalidate those matching the method
  for (auto& entry : cache_entries_) {
    if (entry.MatchesMethod(method)) {
      entry.Clear();
    }
  }
}

void PCRangeCache::Clear() {
  for (auto& entry : cache_entries_) {
    entry.Clear();
  }

  for (auto& counter : lru_counters_) {
    counter.store(0, std::memory_order_relaxed);
  }

  global_lru_counter_.store(1, std::memory_order_relaxed);
  total_lookups_.store(0, std::memory_order_relaxed);
  cache_hits_.store(0, std::memory_order_relaxed);
}

PCRangeCache::Stats PCRangeCache::GetStats() const {
  Stats stats;
  stats.total_lookups = total_lookups_.load(std::memory_order_relaxed);
  stats.cache_hits = cache_hits_.load(std::memory_order_relaxed);
  stats.cache_misses = stats.total_lookups - stats.cache_hits;
  return stats;
}

}  // namespace jit
}  // namespace art
