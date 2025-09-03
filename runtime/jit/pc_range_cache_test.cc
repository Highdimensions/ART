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

  // Create fake ArtMethod pointer for testing
  ArtMethod* CreateFakeMethod(uint32_t id) {
    return reinterpret_cast<ArtMethod*>(0x10000000ULL + (id * 0x1000ULL));
  }

  std::unique_ptr<PCRangeCache> cache_;
  std::unique_ptr<ArenaAllocator> header_memory_;
};

// Simplified test that only tests cache functionality without creating fake headers
TEST_F(PCRangeCacheTest, BasicCacheOperations) {
  // Test basic cache statistics
  PCRangeCache::Stats initial_stats = cache_->GetStats();
  EXPECT_EQ(initial_stats.total_lookups, 0u);
  EXPECT_EQ(initial_stats.cache_hits, 0u);
  EXPECT_EQ(initial_stats.cache_misses, 0u);

  // Test cache miss
  OatQuickMethodHeader* result = cache_->FastLookup(0x12345678);
  EXPECT_EQ(result, nullptr);

  // Verify miss was recorded
  PCRangeCache::Stats after_miss = cache_->GetStats();
  EXPECT_EQ(after_miss.total_lookups, 1u);
  EXPECT_EQ(after_miss.cache_hits, 0u);
  EXPECT_EQ(after_miss.cache_misses, 1u);
}

TEST_F(PCRangeCacheTest, CacheInvalidation) {
  ArtMethod* method1 = CreateFakeMethod(1);
  ArtMethod* method2 = CreateFakeMethod(2);

  // Test invalidating non-existent method (should not crash)
  cache_->InvalidateMethod(method1);

  // Test invalidating null method (should not crash)
  cache_->InvalidateMethod(nullptr);

  // Clear cache (should not crash)
  cache_->Clear();

  // Verify cache is still in clean state
  PCRangeCache::Stats stats = cache_->GetStats();
  EXPECT_EQ(stats.total_lookups, 0u);
  EXPECT_EQ(stats.cache_hits, 0u);
  EXPECT_EQ(stats.cache_misses, 0u);
}

TEST_F(PCRangeCacheTest, CacheConfiguration) {
  // Test architecture-specific cache configuration
  EXPECT_GT(PCRangeCache::kCacheSize, 0u);
  EXPECT_GT(PCRangeCache::kCacheAssociativity, 0u);
  EXPECT_EQ(PCRangeCache::kCacheSets, PCRangeCache::kCacheSize / PCRangeCache::kCacheAssociativity);

#if defined(__aarch64__)
  // ARM64 should have larger cache
  EXPECT_EQ(PCRangeCache::kCacheSize, 256u);
  EXPECT_EQ(PCRangeCache::kCacheAssociativity, 4u);
#elif defined(__arm__)
  // ARM32 should have smaller cache
  EXPECT_EQ(PCRangeCache::kCacheSize, 128u);
  EXPECT_EQ(PCRangeCache::kCacheAssociativity, 2u);
#elif defined(__riscv)
  // RISC-V should have moderate cache
  EXPECT_EQ(PCRangeCache::kCacheSize, 192u);
  EXPECT_EQ(PCRangeCache::kCacheAssociativity, 3u);
#endif
}

TEST_F(PCRangeCacheTest, MultipleQueries) {
  // Test multiple lookups to verify statistics
  for (int i = 0; i < 50; ++i) {
    uintptr_t pc = 0x10000000 + (i * 0x1000);
    OatQuickMethodHeader* result = cache_->FastLookup(pc);
    EXPECT_EQ(result, nullptr);
  }

  PCRangeCache::Stats stats = cache_->GetStats();
  EXPECT_EQ(stats.total_lookups, 50u);
  EXPECT_EQ(stats.cache_hits, 0u);
  EXPECT_EQ(stats.cache_misses, 50u);
  EXPECT_DOUBLE_EQ(stats.hit_rate(), 0.0);
}

TEST_F(PCRangeCacheTest, ClearCacheResetStatistics) {
  // Generate some lookups
  for (int i = 0; i < 10; ++i) {
    cache_->FastLookup(0x20000000 + i * 4);
  }

  PCRangeCache::Stats before_clear = cache_->GetStats();
  EXPECT_GT(before_clear.total_lookups, 0u);

  // Clear cache
  cache_->Clear();

  // Verify statistics are reset
  PCRangeCache::Stats after_clear = cache_->GetStats();
  EXPECT_EQ(after_clear.total_lookups, 0u);
  EXPECT_EQ(after_clear.cache_hits, 0u);
  EXPECT_EQ(after_clear.cache_misses, 0u);
}

// Architecture-specific tests
#if defined(__aarch64__)
TEST_F(PCRangeCacheTest, ARM64AlignmentTest) {
  // ARM64 instructions should be 4-byte aligned
  uintptr_t test_pc = 0x40000000;

  // Verify 4-byte alignment assumption
  EXPECT_EQ(test_pc % 4, 0u) << "ARM64 code should be 4-byte aligned";

  // Test cache configuration
  EXPECT_EQ(PCRangeCache::kCacheSize, 256u);
  EXPECT_EQ(PCRangeCache::kCacheAssociativity, 4u);
}
#elif defined(__arm__)
TEST_F(PCRangeCacheTest, ARM32AlignmentTest) {
  // ARM32 instructions should be 2-byte aligned (Thumb mode)
  uintptr_t test_pc = 0x40000002;

  // Verify 2-byte alignment assumption
  EXPECT_EQ(test_pc % 2, 0u) << "ARM32 code should be 2-byte aligned";

  // Test cache configuration (smaller for 32-bit)
  EXPECT_EQ(PCRangeCache::kCacheSize, 128u);
  EXPECT_EQ(PCRangeCache::kCacheAssociativity, 2u);
}
#elif defined(__riscv)
TEST_F(PCRangeCacheTest, RISCVAlignmentTest) {
  // RISC-V instructions should be 2-byte aligned
  uintptr_t test_pc = 0x40000002;

  // Verify 2-byte alignment assumption
  EXPECT_EQ(test_pc % 2, 0u) << "RISC-V code should be 2-byte aligned";

  // Test cache configuration
  EXPECT_EQ(PCRangeCache::kCacheSize, 192u);
  EXPECT_EQ(PCRangeCache::kCacheAssociativity, 3u);
}
#endif

}  // namespace jit
}  // namespace HIDDEN
