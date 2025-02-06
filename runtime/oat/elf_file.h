/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_RUNTIME_OAT_ELF_FILE_H_
#define ART_RUNTIME_OAT_ELF_FILE_H_

#include <cstddef>
#include <string>

#include "base/macros.h"
#include "base/os.h"
#include "elf/elf_utils.h"

namespace art HIDDEN {

class MemMap;

template <typename ElfTypes>
class ElfFileImpl;

// Explicitly instantiated in elf_file.cc
using ElfFileImpl32 = ElfFileImpl<ElfTypes32>;
using ElfFileImpl64 = ElfFileImpl<ElfTypes64>;

// Used for compile time and runtime for ElfFile access. Because of
// the need for use at runtime, cannot directly use LLVM classes such as
// ELFObjectFile.
class ElfFile {
 public:
  // Loads the program headers.
  // Does not take the ownership of the file and the reservation. They must stay alive during the
  // `Load` call.
  // Note that loading the program headers does not respect the reservation. Only the segments will
  // be loaded into the reservation, by `Load`.
  static ElfFile* Open(File* file,
                       off_t start,
                       size_t file_length,
                       const std::string& file_location,
                       bool low_4gb,
                       /*inout*/ MemMap* reservation,
                       /*out*/ std::string* error_msg);

  virtual ~ElfFile() = default;

  // Load segments into memory based on PT_LOAD program headers.
  virtual bool Load(bool executable, /*out*/ std::string* error_msg) = 0;

  virtual const uint8_t* FindDynamicSymbolAddress(const std::string& symbol_name) const = 0;

  virtual size_t Size() const = 0;

  // The start of the memory map address range for this ELF file.
  virtual uint8_t* Begin() const = 0;

  // The end of the memory map address range for this ELF file.
  virtual uint8_t* End() const = 0;

  // Returns the location of the ELF file, for debugging purposes only.
  // Note that the location is not necessarily a path to a file on disk. It can also be a zip entry
  // inside a zip file.
  virtual const std::string& GetFileLocation() const = 0;

  virtual bool GetLoadedSize(size_t* size, std::string* error_msg) const = 0;

  virtual size_t GetElfSegmentAlignmentFromFile() const = 0;

  virtual const uint8_t* GetBaseAddress() const = 0;

  virtual bool Is64Bit() const = 0;

 protected:
  ElfFile() = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(ElfFile);
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_ELF_FILE_H_
