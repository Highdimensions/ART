/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "safe_copy.h"

#include <errno.h>
#include <string.h>
#include <sys/user.h>
#include <sys/mman.h>

#include "android-base/logging.h"
#include "runtime_globals.h"
#include "common_runtime_test.h"

namespace art {

#if defined(__linux__)

class SafeCopyTest : public CommonRuntimeTest {};

TEST_F(SafeCopyTest, smoke) {
  DCHECK_EQ(gPageSize, static_cast<size_t>(sysconf(_SC_PAGE_SIZE)));

  // Map four pages, mark the second one as PROT_NONE, unmap the last one.
  void* map = mmap(nullptr, gPageSize * 4, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, map);
  char* page1 = static_cast<char*>(map);
  char* page2 = page1 + gPageSize;
  char* page3 = page2 + gPageSize;
  char* page4 = page3 + gPageSize;
  ASSERT_EQ(0, mprotect(page1 + gPageSize, gPageSize, PROT_NONE));
  ASSERT_EQ(0, munmap(page4, gPageSize));

  page1[0] = 'a';
  page1[gPageSize - 1] = 'z';

  page3[0] = 'b';
  page3[gPageSize - 1] = 'y';

  std::vector<char> storage(gPageSize);
  char* buf = storage.data();

  // Completely valid read.
  memset(buf, 0xCC, gPageSize);
  EXPECT_EQ(static_cast<ssize_t>(gPageSize), SafeCopy(buf, page1, gPageSize)) << strerror(errno);
  EXPECT_EQ(0, memcmp(buf, page1, gPageSize));

  // Reading into a guard page.
  memset(buf, 0xCC, gPageSize);
  EXPECT_EQ(static_cast<ssize_t>(gPageSize - 1), SafeCopy(buf, page1 + 1, gPageSize));
  EXPECT_EQ(0, memcmp(buf, page1 + 1, gPageSize - 1));

  // Reading from a guard page into a real page.
  memset(buf, 0xCC, gPageSize);
  EXPECT_EQ(0, SafeCopy(buf, page2 + gPageSize - 1, gPageSize));

  // Reading off of the end of a mapping.
  memset(buf, 0xCC, gPageSize);
  EXPECT_EQ(static_cast<ssize_t>(gPageSize), SafeCopy(buf, page3, gPageSize * 2));
  EXPECT_EQ(0, memcmp(buf, page3, gPageSize));

  // Completely invalid.
  EXPECT_EQ(0, SafeCopy(buf, page1 + gPageSize, gPageSize));

  // Clean up.
  ASSERT_EQ(0, munmap(map, gPageSize * 3));
}

TEST_F(SafeCopyTest, alignment) {
  DCHECK_EQ(gPageSize, static_cast<size_t>(sysconf(_SC_PAGE_SIZE)));

  // Copy the middle of a mapping to the end of another one.
  void* src_map = mmap(nullptr, gPageSize * 3, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, src_map);

  // Add a guard page to make sure we don't write past the end of the mapping.
  void* dst_map = mmap(nullptr, gPageSize * 4, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, dst_map);

  char* src = static_cast<char*>(src_map);
  char* dst = static_cast<char*>(dst_map);
  ASSERT_EQ(0, mprotect(dst + 3 * gPageSize, gPageSize, PROT_NONE));

  src[512] = 'a';
  src[gPageSize * 3 - 512 - 1] = 'z';

  EXPECT_EQ(static_cast<ssize_t>(gPageSize * 3 - 1024),
            SafeCopy(dst + 1024, src + 512, gPageSize * 3 - 1024));
  EXPECT_EQ(0, memcmp(dst + 1024, src + 512, gPageSize * 3 - 1024));

  ASSERT_EQ(0, munmap(src_map, gPageSize * 3));
  ASSERT_EQ(0, munmap(dst_map, gPageSize * 4));
}

#endif  // defined(__linux__)

}  // namespace art
