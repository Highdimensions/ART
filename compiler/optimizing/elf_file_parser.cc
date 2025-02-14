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

#include "elf_file_parser.h"

namespace art HIDDEN {
namespace arm64_llvm {

const ELFFileParser::ELFT::Sym* ELFFileParser::MaybeGetSymbol(std::string_view symbol_name) const {
  for (const ELFT::Sym& sym : symbols_) {
    auto symbol_name_or_err = sym.getName(string_table_);
    CHECK(bool(symbol_name_or_err));
    std::string_view current_symbol_name = *symbol_name_or_err;
    if (current_symbol_name == symbol_name) {
      return &sym;
    }
  }

  return nullptr;
}

template <typename T>
static void StoreValue(llvm::SmallVectorImpl<uint8_t>& data, uint64_t offset, T value) {
  DCHECK_LE(offset + sizeof(T), data.size());
  DCHECK_ALIGNED(offset, alignof(T));
  for (uint64_t i = 0; i < sizeof(T); ++i) {
    data[offset + i] = (value >> (8 * i)) & 0xff;
  }
}

llvm::SmallVector<uint8_t> ELFFileParser::GetSymbolContentsOrEmpty(
    std::string_view symbol_name) const {
  const ELFT::Sym* sym = MaybeGetSymbol(symbol_name);
  if (sym == nullptr || sym->st_size == 0) {
    return {};
  }

  auto section_or_err = object_file_.getSection(sym->st_shndx);
  CHECK(bool(section_or_err));
  const ELFT::Shdr* section = *section_or_err;
  auto name_or_err = object_file_.getSectionName(*section);
  CHECK(bool(name_or_err));
  std::string_view section_name = *name_or_err;
  llvm::ArrayRef<uint8_t> section_contents = GetSectionContents(*section);
  ELFT::RelaRange relocations = GetSectionRelocations(fmt::format(".rela{}", section_name));

  uint64_t symbol_offset = sym->st_value;
  uint64_t symbol_size = sym->st_size;
  CHECK_LT(symbol_offset, section_contents.size());
  CHECK_LE(symbol_offset + symbol_size, section_contents.size());
  llvm::SmallVector<uint8_t> symbol_data(section_contents.slice(symbol_offset, symbol_size));

  for (const ELFT::Rela& relocation : relocations) {
    if (relocation.r_offset < symbol_offset || relocation.r_offset >= symbol_offset + symbol_size) {
      continue;
    }

    [[maybe_unused]] uint64_t offset = relocation.r_offset - symbol_offset;
    switch (relocation.getType(false)) {
      case llvm::ELF::R_AARCH64_ABS64: {
        StoreValue<uint64_t>(symbol_data, offset, relocation.r_addend);
        break;
      }
      default:
        LOG(FATAL) << "Unhandled relocation type: " << relocation.getType(false);
        break;
    }
  }

  return symbol_data;
}

void DumpStackMap(
    const llvm::StackMapParser<ELFFileParser::ELFT::TargetEndianness>& stack_map_parser) {
  using ELFT = ELFFileParser::ELFT;
  // Header {
  //   uint8  : Stack Map Version (current version is 3)
  //   uint8  : Reserved (expected to be 0)
  //   uint16 : Reserved (expected to be 0)
  // }
  // uint32 : NumFunctions
  // uint32 : NumConstants
  // uint32 : NumRecords
  // StkSizeRecord[NumFunctions] {
  //   uint64 : Function Address
  //   uint64 : Stack Size (or UINT64_MAX if not statically known)
  //   uint64 : Record Count
  // }
  // Constants[NumConstants] {
  //   uint64 : LargeConstant
  // }
  // StkMapRecord[NumRecords] {
  //   uint64 : PatchPoint ID
  //   uint32 : Instruction Offset
  //   uint16 : Reserved (record flags)
  //   uint16 : NumLocations
  //   Location[NumLocations] {
  //     uint8  : Register | Direct | Indirect | Constant | ConstantIndex
  //     uint8  : Reserved (expected to be 0)
  //     uint16 : Location Size
  //     uint16 : Dwarf RegNum
  //     uint16 : Reserved (expected to be 0)
  //     int32  : Offset or SmallConstant
  //   }
  //   uint32 : Padding (only if required to align to 8 byte)
  //   uint16 : Padding
  //   uint16 : NumLiveOuts
  //   LiveOuts[NumLiveOuts]
  //     uint16 : Dwarf RegNum
  //     uint8  : Reserved
  //     uint8  : Size in Bytes
  //   }
  //   uint32 : Padding (only if required to align to 8 byte)
  // }
  std::ostringstream out;

  out << fmt::format("StkMapRecord = [\n");
  for (const auto& record : stack_map_parser.records()) {
    out << fmt::format("  {{\n");
    out << fmt::format("    PatchPointID = {},\n", record.getID());
    out << fmt::format("    InstructionOffset = {},\n", record.getInstructionOffset());
    out << fmt::format("    NumLocations = {},\n", record.getNumLocations());
    out << fmt::format("    Locations = [\n");
    for (const auto& location : record.locations()) {
      out << fmt::format("      {{\n");
      using LocationKind = llvm::StackMapParser<ELFT::TargetEndianness>::LocationKind;
      const auto kind_name = [&]() -> std::string_view {
        switch (location.getKind()) {
          case LocationKind::Register:
            return "Register";
          case LocationKind::Direct:
            return "Direct";
          case LocationKind::Indirect:
            return "Indirect";
          case LocationKind::Constant:
            return "Constant";
          case LocationKind::ConstantIndex:
            return "ConstantIndex";
        }
      }();
      out << fmt::format("        Kind = {},\n", kind_name);
      out << fmt::format("        LocationSize = {},\n", location.getSizeInBytes());
      out << fmt::format("        DwarfRegNum = {},\n", location.getDwarfRegNum());
      switch (location.getKind()) {
        case LocationKind::Register:
          break;
        case LocationKind::Direct:
        case LocationKind::Indirect:
          out << fmt::format("        Offset = {},\n", location.getOffset());
          break;
        case LocationKind::Constant:
          out << fmt::format("        SmallConstant = {},\n", location.getSmallConstant());
          break;
        case LocationKind::ConstantIndex:
          out << fmt::format("        ConstantIndex = {},\n", location.getConstantIndex());
          break;
      }
      out << fmt::format("      }},\n");
    }
    out << fmt::format("    ],\n");

    out << fmt::format("    NumLiveOuts = {},\n", record.getNumLiveOuts());
    out << fmt::format("    LiveOuts = [\n");
    for (const auto& live_out : record.liveouts()) {
      out << fmt::format("      {{\n");
      out << fmt::format("        DwarfRegNum = {},\n", live_out.getDwarfRegNum());
      out << fmt::format("        SizeInBytes = {},\n", live_out.getSizeInBytes());
      out << fmt::format("      }},\n");
    }
    out << fmt::format("    ],\n");

    out << fmt::format("  }},\n");
  }
  out << fmt::format("],\n");

  LOG(INFO) << out.str();
}

}  // namespace arm64_llvm
}  // namespace art HIDDEN
