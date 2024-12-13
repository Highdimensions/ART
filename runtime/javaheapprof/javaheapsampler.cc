/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "base/atomic.h"
#include "base/locks.h"
#include "gc/heap.h"
#include "javaheapprof/javaheapsampler.h"
#ifdef ART_TARGET_ANDROID
#include "perfetto/heap_profile.h"
#endif
#include "runtime.h"

namespace art HIDDEN {

size_t HeapSampler::NextTlabSize(size_t target) {
  if (IsEnabled()) {
    // Scale the target tlab size by sampling interval, so the cost of
    // profiling can be reduced by increasing the sampling interval.
    // The factor 1/8 is somewhat arbitrarily chosen to avoid interfering with
    // Perfetto's subsequent sampling of the allocations when we call
    // AHeapProfile_reportAllocation.
    target = std::min(target, GetSamplingInterval() / 8);
    if (target == 0) {
      target = 1;
    }

    // Draw randomly from exponential distribution with the goal of having
    // equal probability of sampling any given byte. This is the same logic
    // used in AHeapProfile_reportAllocation for sampling.
    double rate = 1.0 / static_cast<double>(target);
    std::exponential_distribution<double> dist(rate);
    art::MutexLock mu(art::Thread::Current(), rng_lock_);
    int64_t next = static_cast<int64_t>(dist(rng_));
    return next + 1;
  }
  return target;
}

void HeapSampler::ReportAllocation(art::mirror::Object* obj, size_t allocation_size) {
#ifdef ART_TARGET_ANDROID
  uint64_t perf_alloc_id = reinterpret_cast<uint64_t>(obj);
  AHeapProfile_reportAllocation(perfetto_heap_id_, perf_alloc_id, allocation_size);
#else
  (void)obj;
  (void)allocation_size;
#endif
}

size_t HeapSampler::GetSamplingInterval() {
  return p_sampling_interval_.load(std::memory_order_acquire);
}

void HeapSampler::SetSamplingInterval(size_t sampling_interval) {
  p_sampling_interval_.store(sampling_interval, std::memory_order_release);
}

}  // namespace art
