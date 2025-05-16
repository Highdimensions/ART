#include <cstdio>
#include <stddef.h>
#include "x86_cache.h"

#if defined(__i386__) || defined(__x86_64__)
// Default sizes based on the old hard-coded values for Atom/Silvermont (x86) and Core 2 (x86-64)...
size_t __x86_data_cache_size = 24 * 1024;
size_t __x86_data_cache_size_half = __x86_data_cache_size / 2;
// ...overwritten at runtime based on the cpu's reported cache sizes.
void init_x86_cache_info() {
  // Handle the case where during early boot /sys fs may not yet be ready,
  // In that case (basically just init), we keep the defaults.
  if (sysconf(_SC_LEVEL1_DCACHE_SIZE) != 0) {
    __x86_data_cache_size = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    __x86_data_cache_size_half = __x86_data_cache_size / 2;
  }
}
#endif
