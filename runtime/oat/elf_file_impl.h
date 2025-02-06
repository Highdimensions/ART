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

#ifndef ART_RUNTIME_OAT_ELF_FILE_IMPL_H_
#define ART_RUNTIME_OAT_ELF_FILE_IMPL_H_

#include <type_traits>
#include <vector>

#include "android-base/logging.h"
#include "base/macros.h"
#include "base/mem_map.h"
#include "base/os.h"
#include "elf_file.h"

namespace art HIDDEN {

template <typename ElfTypes>
class ElfFileImpl : public ElfFile {
 public:
  using Elf_Addr = typename ElfTypes::Addr;
  using Elf_Off = typename ElfTypes::Off;
  using Elf_Half = typename ElfTypes::Half;
  using Elf_Word = typename ElfTypes::Word;
  using Elf_Sword = typename ElfTypes::Sword;
  using Elf_Ehdr = typename ElfTypes::Ehdr;
  using Elf_Shdr = typename ElfTypes::Shdr;
  using Elf_Sym = typename ElfTypes::Sym;
  using Elf_Rel = typename ElfTypes::Rel;
  using Elf_Rela = typename ElfTypes::Rela;
  using Elf_Phdr = typename ElfTypes::Phdr;
  using Elf_Dyn = typename ElfTypes::Dyn;

  static ElfFileImpl* Open(File* file,
                           off_t start,
                           size_t file_length,
                           const std::string& file_location,
                           bool low_4gb,
                           /*out*/ std::string* error_msg);

  const std::string& GetFileLocation() const override { return file_location_; }

  uint8_t* GetBaseAddress() const override { return base_address_; }

  uint8_t* Begin() const override { return map_.Begin(); }

  uint8_t* End() const override { return map_.End(); }

  size_t Size() const override { return map_.Size(); }

  Elf_Ehdr& GetHeader() const;

  Elf_Word GetProgramHeaderNum() const;
  Elf_Phdr* GetProgramHeader(Elf_Word) const;

  Elf_Word GetSectionHeaderNum() const;
  Elf_Shdr* FindSectionByType(Elf_Word type) const;

  // Find .dynsym using .hash for more efficient lookup than FindSymbolAddress.
  const uint8_t* FindDynamicSymbolAddress(const std::string& symbol_name) const override;

  static bool IsSymbolSectionType(Elf_Word section_type);
  Elf_Word GetSymbolNum(Elf_Shdr&) const;
  Elf_Sym* GetSymbol(Elf_Word section_type, Elf_Word i) const;

  Elf_Word GetDynamicNum() const;
  Elf_Dyn& GetDynamic(Elf_Word) const;

  // Retrieves the expected size when the file is loaded at runtime. Returns true if successful.
  bool GetLoadedSize(size_t* size, std::string* error_msg) const override;

  // Get the alignment of the first loadable program segment. Return 0 if no loadable segment found.
  size_t GetElfSegmentAlignmentFromFile() const override;

  // Load segments into memory based on PT_LOAD program headers.
  // executable is true at run time, false at compile time.
  bool Load(bool executable,
            bool low_4gb,
            /*inout*/ MemMap* reservation,
            /*out*/ std::string* error_msg) override;

  bool Is64Bit() const override { return std::is_same_v<ElfTypes, ElfTypes64>; }

 private:
  ElfFileImpl(File* file, off_t start, size_t file_length, const std::string& file_location)
      : file_(file), start_(start), file_length_(file_length), file_location_(file_location) {
    CHECK(file != nullptr);
  }

  bool GetLoadedAddressRange(/*out*/uint8_t** vaddr_begin,
                             /*out*/size_t* vaddr_size,
                             /*out*/std::string* error_msg) const;

  bool Setup(bool low_4gb, std::string* error_msg);

  bool SetMap(MemMap&& map, std::string* error_msg);

  uint8_t* GetProgramHeadersStart() const;
  Elf_Phdr& GetDynamicProgramHeader() const;
  Elf_Dyn* GetDynamicSectionStart() const;
  Elf_Sym* GetSymbolSectionStart(Elf_Word section_type) const;
  const char* GetStringSectionStart(Elf_Word section_type) const;
  Elf_Word* GetHashSectionStart() const;
  Elf_Word GetHashBucketNum() const;
  Elf_Word GetHashChainNum() const;
  Elf_Word GetHashBucket(size_t i, bool* ok) const;
  Elf_Word GetHashChain(size_t i, bool* ok) const;

  bool ValidPointer(const uint8_t* start) const;

  const Elf_Sym* FindDynamicSymbol(const std::string& symbol_name) const;

  // Check that certain sections and their dependencies exist.
  bool CheckSectionsExist(std::string* error_msg) const;

  Elf_Phdr* FindProgamHeaderByType(Elf_Word type) const;

  // Lookup a string by section type. Returns null for special 0 offset.
  const char* GetString(Elf_Word section_type, Elf_Word) const;

  File* const file_;
  const off_t start_;
  const size_t file_length_;
  const std::string file_location_;

  // ELF header mapping. If program_header_only_ is false, will
  // actually point to the entire elf file.
  MemMap map_;
  Elf_Ehdr* header_ = nullptr;
  std::vector<MemMap> segments_;

  // Pointer to start of first PT_LOAD program segment after Load()
  // when program_header_only_ is true.
  uint8_t* base_address_ = nullptr;

  // The program header should always available but use GetProgramHeadersStart() to be sure.
  uint8_t* program_headers_start_ = nullptr;

  // Conditionally available values. Use accessors to ensure they exist if they are required.
  uint8_t* section_headers_start_ = nullptr;
  Elf_Phdr* dynamic_program_header_ = nullptr;
  Elf_Dyn* dynamic_section_start_ = nullptr;
  Elf_Sym* symtab_section_start_ = nullptr;
  Elf_Sym* dynsym_section_start_ = nullptr;
  char* strtab_section_start_ = nullptr;
  char* dynstr_section_start_ = nullptr;
  Elf_Word* hash_section_start_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(ElfFileImpl);
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_ELF_FILE_IMPL_H_
