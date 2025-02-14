/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_ELF_FILE_PARSER_H_
#define ART_COMPILER_OPTIMIZING_ELF_FILE_PARSER_H_

#include "base/hash_map.h"
#include "base/logging.h"
#include "base/macros.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#include "llvm/Object/ELF.h"
#include "llvm/Object/StackMapParser.h"
#pragma GCC diagnostic pop

namespace art HIDDEN {
namespace arm64_llvm {

class ELFFileParser {
 public:
  using ELFT = llvm::object::ELF64LE;
  using ELFFile = llvm::object::ELFFile<ELFT>;

  ELFFileParser(llvm::ArrayRef<char> object_file_buffer)
      : object_file_(CreateELFFile(object_file_buffer)) {
    ParseSections();
    BuildSymbolTable();
  }

  const ELFT::Shdr& GetSection(std::string_view section_name) const {
    auto it = sections_.find(section_name);
    CHECK(it != sections_.end());
    return *it->second;
  }

  const ELFT::Shdr* MaybeGetSection(std::string_view section_name) const {
    auto it = sections_.find(section_name);
    if (it == sections_.end()) {
      return nullptr;
    }
    return it->second;
  }

  llvm::ArrayRef<uint8_t> GetSectionContents(std::string_view section_name) const {
    const ELFT::Shdr& section = GetSection(section_name);
    return GetSectionContents(section);
  }

  llvm::ArrayRef<uint8_t> GetSectionContents(const ELFT::Shdr& section) const {
    auto contents_or_err = object_file_.getSectionContents(section);
    CHECK(bool(contents_or_err));
    return *contents_or_err;
  }

  llvm::ArrayRef<uint8_t> GetSectionContentsOrEmpty(std::string_view section_name) const {
    const ELFT::Shdr* section = MaybeGetSection(section_name);
    if (section == nullptr) {
      return {};
    }
    auto contents_or_err = object_file_.getSectionContents(*section);
    CHECK(bool(contents_or_err));
    return *contents_or_err;
  }

  ELFT::RelaRange GetSectionRelocations(std::string_view section_name) const {
    const ELFT::Shdr* section = MaybeGetSection(section_name);
    if (section == nullptr) {
      return {};
    }
    auto relas_or_err = object_file_.relas(*section);
    CHECK(bool(relas_or_err));
    return *relas_or_err;
  }

  std::string_view GetSymbolName(uint32_t symbol) const {
    CHECK_LT(symbol, symbols_.size());
    auto result_or_err = symbols_[symbol].getName(string_table_);
    CHECK(bool(result_or_err));
    return *result_or_err;
  }

  std::string_view GetSectionNameOfSymbol(uint32_t symbol) const {
    CHECK_LT(symbol, symbols_.size());
    uint32_t section_index = symbols_[symbol].st_shndx;
    auto sections_range_or_err = object_file_.sections();
    CHECK(bool(sections_range_or_err));
    ELFT::ShdrRange& sections_range = *sections_range_or_err;
    CHECK_LT(section_index, sections_range.size());
    auto result_or_err = object_file_.getSectionName(sections_range[section_index]);
    CHECK(bool(result_or_err));
    return *result_or_err;
  }

  llvm::ArrayRef<std::string_view> GetRodataSections() const { return rodata_sections_; }

  const ELFT::Sym* MaybeGetSymbol(std::string_view symbol_name) const;

  llvm::SmallVector<uint8_t> GetSymbolContentsOrEmpty(std::string_view symbol_name) const;

 private:
  using ELFSectionsMap = HashMap<std::string_view, const ELFT::Shdr*>;

  static ELFFile CreateELFFile(llvm::ArrayRef<char> object_file_buffer) {
    auto file_or_err =
        ELFFile::create(llvm::StringRef(object_file_buffer.data(), object_file_buffer.size()));
    CHECK(bool(file_or_err));
    return std::move(*file_or_err);
  }

  void ParseSections() {
    auto sections_range_or_err = object_file_.sections();
    CHECK(bool(sections_range_or_err));
    ELFT::ShdrRange& sections_range = *sections_range_or_err;

    for (const ELFT::Shdr& section : sections_range) {
      auto name_or_err = object_file_.getSectionName(section);
      CHECK(bool(name_or_err));
      llvm::StringRef name = *name_or_err;

      if (name.starts_with(".rodata")) {
        rodata_sections_.push_back(name);
      }

      // LOG(INFO) << "Section name: '" << std::string_view(name) << "'";
      sections_.insert({name, &section});
    }
  }

  void BuildSymbolTable() {
    const ELFT::Shdr& strtab_section = GetSection(".strtab");
    auto strtab_or_err = object_file_.getStringTable(strtab_section);
    CHECK(bool(strtab_or_err));
    string_table_ = *strtab_or_err;

    const ELFT::Shdr& symtab_section = GetSection(".symtab");
    auto symbols_or_err = object_file_.symbols(&symtab_section);
    CHECK(bool(symbols_or_err));
    symbols_ = *symbols_or_err;
  }

  ELFFile object_file_;

  ELFSectionsMap sections_;
  llvm::StringRef string_table_;
  ELFT::SymRange symbols_;
  llvm::SmallVector<std::string_view> rodata_sections_;
};

void DumpStackMap(
    const llvm::StackMapParser<ELFFileParser::ELFT::TargetEndianness>& stack_map_parser);

}  // namespace arm64_llvm
}  // namespace art HIDDEN

#endif  // ART_COMPILER_OPTIMIZING_ELF_FILE_PARSER_H_
