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

#include "jit/pc_range_cache.h"

#include <memory>
#include <vector>

#include "art_method-inl.h"
#include "base/arena_allocator.h"
#include "base/memory_region.h"
#include "common_runtime_test.h"
#include "oat/oat_quick_method_header.h"
#include "scoped_thread_state_change-inl.h"

namespace art HIDDEN {
namespace jit {

class PCRangeCacheTest : public CommonRuntimeTest {
 protected:
  void SetUp() override {
    CommonRuntimeTest::SetUp();
    cache_ = std::make_unique<PCRangeCache>();

    // Use Runtime's arena pool instead of creating our own
    header_memory_ = std::make_unique<ArenaAllocator>(Runtime::Current()->GetArenaPool());
  }

  void TearDown() override {
    cache_.reset();
    header_memory_.reset();
    CommonRuntimeTest::TearDown();
  }

  // Helper to create a fake method header for testing
  std::pair<OatQuickMethodHeader*, const void*> CreateFakeMethodHeader(size_t code_size) {
    // Calculate sizes with proper alignment
    size_t alignment = GetInstructionSetCodeAlignment(kRuntimeISA);
    size_t header_size = OatQuickMethodHeader::InstructionAlignedSize();
    size_t total_size = header_size + code_size;

    // Allocate aligned memory
    void* memory = header_memory_->Alloc(total_size + alignment, ArenaAllocKind::kArenaAllocMisc);
    uint8_t* aligned_memory =
        reinterpret_cast<uint8_t*>(RoundUp(reinterpret_cast<uintptr_t>(memory), alignment));

    // Create method header - use zero for method_info to avoid complex initialization
    OatQuickMethodHeader* header = new (aligned_memory) OatQuickMethodHeader(0u);

    // Calculate code pointer
    const void* code_ptr = aligned_memory + header_size;

    // Fill with architecture-specific NOPs
    FillWithNops(const_cast<void*>(code_ptr), code_size);

    return std::make_pair(header, code_ptr);
  }

  // Fill memory region with architecture-appropriate NOP instructions
  void FillWithNops(void* ptr, size_t size) {
#if defined(__aarch64__)
    // ARM64: 0xD503201F is NOP instruction
    uint32_t* code_ptr = reinterpret_cast<uint32_t*>(ptr);
    for (size_t i = 0; i < size / sizeof(uint32_t); ++i) {
      code_ptr[i] = 0xD503201F;
    }
#elif defined(__arm__)
    // ARM32 Thumb: 0x46C0 is NOP instruction
    uint16_t* code_ptr = reinterpret_cast<uint16_t*>(ptr);
    for (size_t i = 0; i < size / sizeof(uint16_t); ++i) {
      code_ptr[i] = 0x46C0;
    }
#elif defined(__riscv)
    // RISC-V: 0x00000013 is NOP (addi x0, x0, 0)
    uint32_t* code_ptr = reinterpret_cast<uint32_t*>(ptr);
    for (size_t i = 0; i < size / sizeof(uint32_t); ++i) {
      code_ptr[i] = 0x00000013;
    }
#else
    // x86: 0x90 is NOP instruction
    memset(ptr, 0x90, size);
#endif
  }

  // Create fake ArtMethod pointer for testing
  ArtMethod* CreateFakeMethod(uint32_t id) {
    return reinterpret_cast<ArtMethod*>(0x10000000ULL + (id * 0x1000ULL));
  }

  std::unique_ptr<PCRangeCache> cache_;
  std::unique_ptr<ArenaAllocator> header_memory_;
};

TEST_F(PCRangeCacheTest, BasicLookup) {
  // Test basic cache functionality
  std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
  OatQuickMethodHeader* header = header_pair.first;
  const void* code_ptr = header_pair.second;
  ArtMethod* method = CreateFakeMethod(1);

  uintptr_t code_start = reinterpret_cast<uintptr_t>(code_ptr);

  // Initially should miss
  OatQuickMethodHeader* result = cache_->FastLookup(code_start);
  EXPECT_EQ(result, nullptr);

  // Add entry
  {
    ScopedObjectAccess soa(Thread::Current());
    cache_->AddEntry(code_ptr, method);
  }
  // Should hit now
  result = cache_->FastLookup(code_start);
  EXPECT_EQ(result, header);

  // Test PC within range
  result = cache_->FastLookup(code_start + 32);
  EXPECT_EQ(result, header);

  // Test PC at end boundary (should not hit)
  result = cache_->FastLookup(code_start + 64);
  EXPECT_EQ(result, nullptr);

  // Test PC way outside range
  result = cache_->FastLookup(code_start + 1024);
  EXPECT_EQ(result, nullptr);
}

TEST_F(PCRangeCacheTest, InvalidateMethod) {
  std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(128);
  OatQuickMethodHeader* header = header_pair.first;
  const void* code_ptr = header_pair.second;
  ArtMethod* method = CreateFakeMethod(2);

  uintptr_t code_start = reinterpret_cast<uintptr_t>(code_ptr);

  {
    ScopedObjectAccess soa(Thread::Current());
    cache_->AddEntry(code_ptr, method);
  }
  // Verify entry exists
  OatQuickMethodHeader* result = cache_->FastLookup(code_start);
  EXPECT_EQ(result, header);

  // Invalidate method
  cache_->InvalidateMethod(method);

  // Should miss now
  result = cache_->FastLookup(code_start);
  EXPECT_EQ(result, nullptr);
}

TEST_F(PCRangeCacheTest, MultipleEntries) {
  constexpr size_t kNumMethods = 10;
  std::vector<std::pair<OatQuickMethodHeader*, const void*>> headers;
  std::vector<ArtMethod*> methods;
  std::vector<uintptr_t> code_starts;

  // Create and add multiple methods
  for (size_t i = 0; i < kNumMethods; ++i) {
    std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64 + i * 16);
    ArtMethod* method = CreateFakeMethod(i + 10);

    headers.push_back(header_pair);
    methods.push_back(method);
    code_starts.push_back(reinterpret_cast<uintptr_t>(header_pair.second));

    {
      ScopedObjectAccess soa(Thread::Current());
      cache_->AddEntry(header_pair.second, method);
    }
  }

  // Verify all methods can be found
  for (size_t i = 0; i < kNumMethods; ++i) {
    OatQuickMethodHeader* result = cache_->FastLookup(code_starts[i]);
    EXPECT_EQ(result, headers[i].first) << "Failed to find method " << i;

    // Also test middle of the range
    result = cache_->FastLookup(code_starts[i] + 32);
    EXPECT_EQ(result, headers[i].first) << "Failed to find method " << i << " in middle";
  }
}

TEST_F(PCRangeCacheTest, CacheStatistics) {
  PCRangeCache::Stats initial_stats = cache_->GetStats();
  EXPECT_EQ(initial_stats.total_lookups, 0u);
  EXPECT_EQ(initial_stats.cache_hits, 0u);
  EXPECT_EQ(initial_stats.cache_misses, 0u);

  std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
  const void* code_ptr = header_pair.second;
  ArtMethod* method = CreateFakeMethod(20);
  uintptr_t code_start = reinterpret_cast<uintptr_t>(code_ptr);

  // Perform some lookups (should all miss)
  cache_->FastLookup(code_start);
  cache_->FastLookup(code_start + 10);
  cache_->FastLookup(code_start + 20);

  PCRangeCache::Stats stats_after_misses = cache_->GetStats();
  EXPECT_EQ(stats_after_misses.total_lookups, 3u);
  EXPECT_EQ(stats_after_misses.cache_hits, 0u);
  EXPECT_EQ(stats_after_misses.cache_misses, 3u);
  EXPECT_DOUBLE_EQ(stats_after_misses.hit_rate(), 0.0);

  // Add entry and test hits
  {
    ScopedObjectAccess soa(Thread::Current());
    cache_->AddEntry(code_ptr, method);
  }

  cache_->FastLookup(code_start);        // Hit
  cache_->FastLookup(code_start + 32);   // Hit
  cache_->FastLookup(code_start + 100);  // Miss (outside range)

  PCRangeCache::Stats final_stats = cache_->GetStats();
  EXPECT_EQ(final_stats.total_lookups, 6u);
  EXPECT_EQ(final_stats.cache_hits, 2u);
  EXPECT_EQ(final_stats.cache_misses, 4u);
  EXPECT_NEAR(final_stats.hit_rate(), 2.0 / 6.0, 0.001);
}

TEST_F(PCRangeCacheTest, CacheEviction) {
  // Test LRU eviction by filling cache beyond its capacity
  std::vector<std::pair<OatQuickMethodHeader*, const void*>> headers;
  std::vector<ArtMethod*> methods;
  std::vector<uintptr_t> code_starts;

  // Add more entries than cache associativity to test eviction
  size_t num_entries = PCRangeCache::kCacheSize + 10;

  for (size_t i = 0; i < num_entries; ++i) {
    std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
    ArtMethod* method = CreateFakeMethod(i + 100);

    headers.push_back(header_pair);
    methods.push_back(method);
    code_starts.push_back(reinterpret_cast<uintptr_t>(header_pair.second));

    {
      ScopedObjectAccess soa(Thread::Current());
      cache_->AddEntry(header_pair.second, method);
    }
  }

  // Some early entries should have been evicted
  size_t found_count = 0;
  for (size_t i = 0; i < num_entries; ++i) {
    OatQuickMethodHeader* result = cache_->FastLookup(code_starts[i]);
    if (result == headers[i].first) {
      found_count++;
    }
  }

  // Should find most entries, but not all due to eviction
  EXPECT_LE(found_count, PCRangeCache::kCacheSize);
  EXPECT_GT(found_count, PCRangeCache::kCacheSize / 2);  // Should find a reasonable number
}

TEST_F(PCRangeCacheTest, ClearCache) {
  // Add some entries
  for (size_t i = 0; i < 5; ++i) {
    std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
    ArtMethod* method = CreateFakeMethod(i + 200);
    {
      ScopedObjectAccess soa(Thread::Current());
      cache_->AddEntry(header_pair.second, method);
    }

    // Perform lookup to generate statistics
    cache_->FastLookup(reinterpret_cast<uintptr_t>(header_pair.second));
  }

  PCRangeCache::Stats stats_before = cache_->GetStats();
  EXPECT_GT(stats_before.total_lookups, 0u);

  // Clear cache
  cache_->Clear();

  // Statistics should be reset
  PCRangeCache::Stats stats_after = cache_->GetStats();
  EXPECT_EQ(stats_after.total_lookups, 0u);
  EXPECT_EQ(stats_after.cache_hits, 0u);
  EXPECT_EQ(stats_after.cache_misses, 0u);

  // All lookups should miss
  for (size_t i = 0; i < 5; ++i) {
    std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
    OatQuickMethodHeader* result =
        cache_->FastLookup(reinterpret_cast<uintptr_t>(header_pair.second));
    EXPECT_EQ(result, nullptr);
  }
}

TEST_F(PCRangeCacheTest, EdgeCases) {
  std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
  OatQuickMethodHeader* header = header_pair.first;
  const void* code_ptr = header_pair.second;
  ArtMethod* method = CreateFakeMethod(300);
  uintptr_t code_start = reinterpret_cast<uintptr_t>(code_ptr);

  {
    ScopedObjectAccess soa(Thread::Current());
    cache_->AddEntry(code_ptr, method);
  }
  // Test exact boundaries
  EXPECT_EQ(cache_->FastLookup(code_start), header);        // Start boundary
  EXPECT_EQ(cache_->FastLookup(code_start + 63), header);   // Last valid byte
  EXPECT_EQ(cache_->FastLookup(code_start + 64), nullptr);  // First invalid byte

  // Test null method invalidation (should not crash)
  cache_->InvalidateMethod(nullptr);
  EXPECT_EQ(cache_->FastLookup(code_start), header);  // Should still be there

  // Test invalidating non-existent method
  ArtMethod* non_existent = CreateFakeMethod(999);
  cache_->InvalidateMethod(non_existent);
  EXPECT_EQ(cache_->FastLookup(code_start), header);  // Should still be there
}

// Architecture-specific tests
#if defined(__aarch64__)
TEST_F(PCRangeCacheTest, ARM64AlignmentTest) {
  // ARM64 instructions should be 4-byte aligned
  std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
  uintptr_t code_start = reinterpret_cast<uintptr_t>(header_pair.second);

  // Verify 4-byte alignment
  EXPECT_EQ(code_start % 4, 0u) << "ARM64 code should be 4-byte aligned";

  // Test cache configuration
  EXPECT_EQ(PCRangeCache::kCacheSize, 256u);
  EXPECT_EQ(PCRangeCache::kCacheAssociativity, 4u);
}
#elif defined(__arm__)
TEST_F(PCRangeCacheTest, ARM32AlignmentTest) {
  // ARM32 instructions should be 2-byte aligned (Thumb mode)
  std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
  uintptr_t code_start = reinterpret_cast<uintptr_t>(header_pair.second);

  // Verify 2-byte alignment
  EXPECT_EQ(code_start % 2, 0u) << "ARM32 code should be 2-byte aligned";

  // Test cache configuration (smaller for 32-bit)
  EXPECT_EQ(PCRangeCache::kCacheSize, 128u);
  EXPECT_EQ(PCRangeCache::kCacheAssociativity, 2u);
}
#elif defined(__riscv)
TEST_F(PCRangeCacheTest, RISCVAlignmentTest) {
  // RISC-V instructions should be 2-byte aligned
  std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
  uintptr_t code_start = reinterpret_cast<uintptr_t>(header_pair.second);

  // Verify 2-byte alignment
  EXPECT_EQ(code_start % 2, 0u) << "RISC-V code should be 2-byte aligned";

  // Test cache configuration
  EXPECT_EQ(PCRangeCache::kCacheSize, 192u);
  EXPECT_EQ(PCRangeCache::kCacheAssociativity, 3u);
}
#endif

TEST_F(PCRangeCacheTest, ConcurrentAccess) {
  // Note: This is a basic test for concurrent access patterns
  // Real concurrent testing would require more sophisticated threading

  std::pair<OatQuickMethodHeader*, const void*> header_pair = CreateFakeMethodHeader(64);
  OatQuickMethodHeader* header = header_pair.first;
  const void* code_ptr = header_pair.second;
  ArtMethod* method = CreateFakeMethod(600);
  uintptr_t code_start = reinterpret_cast<uintptr_t>(code_ptr);

  {
    ScopedObjectAccess soa(Thread::Current());
    cache_->AddEntry(code_ptr, method);
  }

  // Simulate concurrent lookups (single-threaded simulation)
  for (int i = 0; i < 100; ++i) {
    OatQuickMethodHeader* result = cache_->FastLookup(code_start + (i % 64));
    EXPECT_EQ(result, header);
  }

  // Verify statistics make sense
  PCRangeCache::Stats stats = cache_->GetStats();
  EXPECT_EQ(stats.total_lookups, 100u);
  EXPECT_EQ(stats.cache_hits, 100u);
  EXPECT_EQ(stats.cache_misses, 0u);
}
}  // namespace jit
}  // namespace HIDDEN
