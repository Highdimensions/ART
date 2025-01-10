/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <sys/mman.h>  // For the PROT_NONE constant.

#include "base/file_utils.h"
#include "base/mem_map.h"
#include "base/unix_file/fd_file.h"
#include "base/utils.h"
#include "common_compiler_driver_test.h"
#include "driver/compiler_driver.h"
#include "elf/elf_builder.h"
#include "elf_writer_quick.h"
#include "oat/elf_file.h"
#include "oat/elf_file_impl.h"
#include "oat/oat.h"

namespace art {
namespace linker {

class ElfWriterTest : public CommonCompilerDriverTest {
 protected:
  void SetUp() override {
    ReserveImageSpace();
    CommonCompilerTest::SetUp();
    CreateCompilerDriver();
  }

  void WriteElf(File* oat_file,
                const std::vector<uint8_t>& rodata,
                const std::vector<uint8_t>& text,
                const std::vector<uint8_t>& data_img_rel_ro,
                size_t data_img_rel_ro_app_image_offset,
                size_t bss_size,
                size_t bss_methods_offset,
                size_t bss_roots_offset,
                size_t dex_section_size) {
    std::unique_ptr<ElfWriter> elf_writer = CreateElfWriterQuick(
      compiler_driver_->GetCompilerOptions(),
      oat_file);

    elf_writer->Start();
    OutputStream* rodata_section = elf_writer->StartRoData();

    elf_writer->PrepareDynamicSection(rodata.size(),
                                      text.size(),
                                      data_img_rel_ro.size(),
                                      data_img_rel_ro_app_image_offset,
                                      bss_size,
                                      bss_methods_offset,
                                      bss_roots_offset,
                                      dex_section_size);

    ASSERT_TRUE(rodata_section->WriteFully(rodata.data(), rodata.size()));
    elf_writer->EndRoData(rodata_section);

    OutputStream* text_section = elf_writer->StartText();
    ASSERT_TRUE(text_section->WriteFully(text.data(), text.size()));
    elf_writer->EndText(text_section);

    if (!data_img_rel_ro.empty()) {
      OutputStream* data_img_rel_ro_section = elf_writer->StartDataImgRelRo();
      ASSERT_TRUE(data_img_rel_ro_section->WriteFully(data_img_rel_ro.data(),
          data_img_rel_ro.size()));
      elf_writer->EndDataImgRelRo(data_img_rel_ro_section);
    }

    elf_writer->WriteDynamicSection();
    ASSERT_TRUE(elf_writer->End());
  }
};

#define EXPECT_ELF_FILE_ADDRESS(ef, expected_value, symbol_name, build_map) \
  do { \
    void* addr = reinterpret_cast<void*>((ef)->FindSymbolAddress(SHT_DYNSYM, \
                                                                 symbol_name, \
                                                                 build_map)); \
    EXPECT_NE(nullptr, addr); \
    if ((expected_value) == nullptr) { \
      (expected_value) = addr; \
    }                        \
    EXPECT_EQ(expected_value, addr); \
    EXPECT_EQ(expected_value, (ef)->FindDynamicSymbolAddress(symbol_name)); \
  } while (false)

TEST_F(ElfWriterTest, dlsym) {
  std::string elf_location = GetCoreOatLocation();
  std::string elf_filename = GetSystemImageFilename(elf_location.c_str(), kRuntimeISA);
  LOG(INFO) << "elf_filename=" << elf_filename;

  UnreserveImageSpace();
  void* dl_oatdata = nullptr;
  void* dl_oatexec = nullptr;
  void* dl_oatlastword = nullptr;

  std::unique_ptr<File> file(OS::OpenFileForReading(elf_filename.c_str()));
  ASSERT_TRUE(file.get() != nullptr) << elf_filename;
  {
    std::string error_msg;
    std::unique_ptr<ElfFile> ef(ElfFile::Open(file.get(),
                                              /*writable=*/ false,
                                              /*program_header_only=*/ false,
                                              /*low_4gb=*/false,
                                              &error_msg));
    CHECK(ef.get() != nullptr) << error_msg;
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatdata, "oatdata", false);
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatexec, "oatexec", false);
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatlastword, "oatlastword", false);
  }
  {
    std::string error_msg;
    std::unique_ptr<ElfFile> ef(ElfFile::Open(file.get(),
                                              /*writable=*/ false,
                                              /*program_header_only=*/ false,
                                              /*low_4gb=*/ false,
                                              &error_msg));
    CHECK(ef.get() != nullptr) << error_msg;
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatdata, "oatdata", true);
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatexec, "oatexec", true);
    EXPECT_ELF_FILE_ADDRESS(ef, dl_oatlastword, "oatlastword", true);
  }
  {
    std::string error_msg;
    std::unique_ptr<ElfFile> ef(ElfFile::Open(file.get(),
                                              /*writable=*/ false,
                                              /*program_header_only=*/ true,
                                              /*low_4gb=*/ false,
                                              &error_msg));
    CHECK(ef.get() != nullptr) << error_msg;
    size_t size;
    bool success = ef->GetLoadedSize(&size, &error_msg);
    CHECK(success) << error_msg;
    MemMap reservation = MemMap::MapAnonymous("ElfWriterTest#dlsym reservation",
                                              RoundUp(size, MemMap::GetPageSize()),
                                              PROT_NONE,
                                              /*low_4gb=*/ true,
                                              &error_msg);
    CHECK(reservation.IsValid()) << error_msg;
    uint8_t* base = reservation.Begin();
    success =
        ef->Load(file.get(), /*executable=*/ false, /*low_4gb=*/ false, &reservation, &error_msg);
    CHECK(success) << error_msg;
    CHECK(!reservation.IsValid());
    EXPECT_EQ(reinterpret_cast<uintptr_t>(dl_oatdata) + reinterpret_cast<uintptr_t>(base),
        reinterpret_cast<uintptr_t>(ef->FindDynamicSymbolAddress("oatdata")));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(dl_oatexec) + reinterpret_cast<uintptr_t>(base),
        reinterpret_cast<uintptr_t>(ef->FindDynamicSymbolAddress("oatexec")));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(dl_oatlastword) + reinterpret_cast<uintptr_t>(base),
        reinterpret_cast<uintptr_t>(ef->FindDynamicSymbolAddress("oatlastword")));
  }
}

TEST_F(ElfWriterTest, CheckBuildIdPresent) {
  std::string elf_location = GetCoreOatLocation();
  std::string elf_filename = GetSystemImageFilename(elf_location.c_str(), kRuntimeISA);
  LOG(INFO) << "elf_filename=" << elf_filename;

  std::unique_ptr<File> file(OS::OpenFileForReading(elf_filename.c_str()));
  ASSERT_TRUE(file.get() != nullptr);
  {
    std::string error_msg;
    std::unique_ptr<ElfFile> ef(ElfFile::Open(file.get(),
                                              /*writable=*/ false,
                                              /*program_header_only=*/ false,
                                              /*low_4gb=*/ false,
                                              &error_msg));
    CHECK(ef.get() != nullptr) << error_msg;
    EXPECT_TRUE(ef->HasSection(".note.gnu.build-id"));
  }
}

TEST_F(ElfWriterTest, CheckDynamicSection) {
  auto verify = [this](size_t rodata_size,
                       size_t text_size,
                       size_t data_img_rel_ro_size,
                       size_t data_img_rel_ro_app_image_offset,
                       size_t bss_size,
                       size_t bss_methods_offset,
                       size_t bss_roots_offset,
                       size_t dex_section_size) {
    SCOPED_TRACE(testing::Message() << "rodata_size: " << rodata_size
                 << ", text_size: " << text_size
                 << ", data_img_rel_ro_size: " << data_img_rel_ro_size
                 << ", data_img_rel_ro_app_image_offset: " << data_img_rel_ro_app_image_offset
                 << ", bss_size: " << bss_size
                 << ", bss_methods_offset: " << bss_methods_offset
                 << ", bss_roots_offset: " << bss_roots_offset
                 << ", dex_section_size: " << dex_section_size);

    std::vector<uint8_t> rodata(rodata_size, 0xAA);
    std::vector<uint8_t> text(text_size, 0xBB);
    std::vector<uint8_t> data_img_rel_ro(data_img_rel_ro_app_image_offset, 0xCC);
    size_t data_img_rel_ro_app_image_size = data_img_rel_ro_size - data_img_rel_ro_app_image_offset;
    data_img_rel_ro.insert(data_img_rel_ro.cend(), data_img_rel_ro_app_image_size, 0xDD);

    ScratchFile tmp_base, tmp_oat(tmp_base, ".oat");
    WriteElf(tmp_oat.GetFile(),
             rodata,
             text,
             data_img_rel_ro,
             data_img_rel_ro_app_image_offset,
             bss_size,
             bss_methods_offset,
             bss_roots_offset,
             dex_section_size);

    std::string error_msg;
    std::unique_ptr<ElfFile> ef(ElfFile::Open(tmp_oat.GetFile(),
                                              /*writable=*/ false,
                                              /*program_header_only=*/ true,
                                              /*low_4gb=*/ false,
                                              &error_msg));
    ASSERT_NE(ef.get(), nullptr) << error_msg;
    ASSERT_TRUE(ef->Load(tmp_oat.GetFile(),
                         /*executable=*/false,
                         /*low_4gb=*/false,
                         /*reservation=*/nullptr,
                         &error_msg)) << error_msg;

    const uint8_t* oatdata_ptr = ef->FindDynamicSymbolAddress("oatdata");
    ASSERT_NE(oatdata_ptr, nullptr);
    EXPECT_EQ(memcmp(oatdata_ptr, rodata.data(), rodata.size()), 0);

    size_t page_size = GetPageSizeSlow();
    size_t elf_word_size = ef->Is64Bit() ? sizeof(ElfTypes64::Word) : sizeof(ElfTypes32::Word);

    if (text_size != 0u) {
      const uint8_t* text_ptr = ef->FindDynamicSymbolAddress("oatexec");
      ASSERT_NE(text_ptr, nullptr);
      ASSERT_TRUE(IsAlignedParam(text_ptr, page_size));
      EXPECT_EQ(memcmp(text_ptr, text.data(), text.size()), 0);

      const uint8_t* oatlastword_ptr = ef->FindDynamicSymbolAddress("oatlastword");
      ASSERT_NE(oatlastword_ptr, nullptr);
      EXPECT_EQ(static_cast<size_t>(oatlastword_ptr - text_ptr), text_size - elf_word_size);
    } else if (rodata_size != 0u) {
      const uint8_t* oatlastword_ptr = ef->FindDynamicSymbolAddress("oatlastword");
      ASSERT_NE(oatlastword_ptr, nullptr);
      EXPECT_EQ(static_cast<size_t>(oatlastword_ptr - oatdata_ptr), rodata_size - elf_word_size);
    }

    if (data_img_rel_ro_size != 0u) {
      const uint8_t* oatdataimgrelro_ptr = ef->FindDynamicSymbolAddress("oatdataimgrelro");
      ASSERT_NE(oatdataimgrelro_ptr, nullptr);
      ASSERT_TRUE(IsAlignedParam(oatdataimgrelro_ptr, page_size));
      EXPECT_EQ(memcmp(oatdataimgrelro_ptr, data_img_rel_ro.data(), data_img_rel_ro.size()), 0);

      const uint8_t* oatdataimgrelrolastword_ptr =
          ef->FindDynamicSymbolAddress("oatdataimgrelrolastword");
      ASSERT_NE(oatdataimgrelrolastword_ptr, nullptr);
      EXPECT_EQ(static_cast<size_t>(oatdataimgrelrolastword_ptr - oatdataimgrelro_ptr),
          data_img_rel_ro_size - elf_word_size);

      if (data_img_rel_ro_app_image_offset != data_img_rel_ro_size) {
        const uint8_t* oatdataimgrelroappimage_ptr =
            ef->FindDynamicSymbolAddress("oatdataimgrelroappimage");
        ASSERT_NE(oatdataimgrelroappimage_ptr, nullptr);
        EXPECT_EQ(static_cast<size_t>(oatdataimgrelroappimage_ptr - oatdataimgrelro_ptr),
          data_img_rel_ro_app_image_offset);
      }

      if (bss_size != 0u) {
        const uint8_t* bss_ptr = ef->FindDynamicSymbolAddress("oatbss");
        ASSERT_NE(bss_ptr, nullptr);
        ASSERT_TRUE(IsAlignedParam(bss_ptr, page_size));

        if (bss_methods_offset != bss_roots_offset) {
          const uint8_t* oatbssmethods_ptr = ef->FindDynamicSymbolAddress("oatbssmethods");
          ASSERT_NE(oatbssmethods_ptr, nullptr);
          EXPECT_EQ(static_cast<size_t>(oatbssmethods_ptr - bss_ptr), bss_methods_offset);
        }

        if (bss_roots_offset != bss_size) {
          const uint8_t* oatbssroots_ptr = ef->FindDynamicSymbolAddress("oatbssroots");
          ASSERT_NE(oatbssroots_ptr, nullptr);
          EXPECT_EQ(static_cast<size_t>(oatbssroots_ptr - bss_ptr), bss_roots_offset);
        }

        const uint8_t* oatbsslastword_ptr = ef->FindDynamicSymbolAddress("oatbsslastword");
        ASSERT_NE(oatbsslastword_ptr, nullptr);
        EXPECT_EQ(static_cast<size_t>(oatbsslastword_ptr - bss_ptr), bss_size - elf_word_size);
      }

      if (dex_section_size != 0u) {
        const uint8_t* dex_ptr = ef->FindDynamicSymbolAddress("oatdex");
        ASSERT_NE(dex_ptr, nullptr);
        ASSERT_TRUE(IsAlignedParam(dex_ptr, page_size));
        const uint8_t* oatdexlastword_ptr = ef->FindDynamicSymbolAddress("oatdexlastword");
        EXPECT_EQ(static_cast<size_t>(oatdexlastword_ptr - dex_ptr),
            dex_section_size - elf_word_size);
      }
    }
  };

  enum class Symbol {
    kRodata,
    kText,
    kDataImgRelRo,
    kDataImgRelRoAppImage,
    kBss,
    kBssMethods,
    kBssRoots,
    kDex,
    kLast = kDex
  };

  constexpr size_t kNumberOfSymbols = static_cast<size_t>(Symbol::kLast) + 1;

  // Use an unaligned section size to verify that ElfWriter properly aligns sections in this case.
  // We can use an arbitrary value that is greater than or equal to an ElfWord (4 bytes).
  constexpr size_t kSectionSize = 127u;
  // Offset in .data.img.rel.ro section from its beginning. We can use any value in the range
  // [0, kSectionSize).
  constexpr size_t kDataImgRelRoAppImageOffset = kSectionSize / 2;
  // Offsets in .bss from its beginning. We can use any value in the range [0, kSectionSize),
  // kBssMethodsOffset should be less than or equal to kBssRootsOffset.
  constexpr size_t kBssMethodsOffset = kSectionSize / 3;
  constexpr size_t kBssRootsOffset = 2 * kBssMethodsOffset;

  auto exists = [](Symbol symbol, const std::bitset<kNumberOfSymbols> &symbols) {
    return symbols.test(static_cast<size_t>(symbol));
  };

  auto get_size = [&](Symbol symbol, const std::bitset<kNumberOfSymbols> &symbols) -> size_t {
    return exists(symbol, symbols) ? kSectionSize : 0;
  };

  auto get_offset = [&](Symbol symbol, const std::bitset<kNumberOfSymbols> &symbols) -> size_t {
    if (symbol == Symbol::kDataImgRelRoAppImage) {
      return exists(symbol, symbols) ? kDataImgRelRoAppImageOffset : 0u;
    }
    if (symbol == Symbol::kBssMethods) {
      if (!exists(Symbol::kBss, symbols)) {
        return 0u;
      }
      if (exists(symbol, symbols)) {
        return kBssMethodsOffset;
      }
      if (exists(Symbol::kBssRoots, symbols)) {
        return kBssRootsOffset;
      }
      return kSectionSize;
    }
    if (symbol == Symbol::kBssRoots) {
      if (!exists(Symbol::kBss, symbols)) {
        return 0u;
      }
      return exists(symbol, symbols) ? kBssRootsOffset : kSectionSize;
    }
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  };

  auto is_valid_mask = [&](const std::bitset<kNumberOfSymbols> &symbols) {
    if (!exists(Symbol::kDataImgRelRo, symbols)) {
      return !exists(Symbol::kDataImgRelRoAppImage, symbols);
    }
    if (!exists(Symbol::kBss, symbols)) {
      return !exists(Symbol::kBssMethods, symbols) && !exists(Symbol::kBssRoots, symbols);
    }
    return true;
  };

  size_t last_mask = (1 << kNumberOfSymbols) - 1;

  for (size_t mask = 0; mask <= last_mask; mask++) {
    std::bitset<kNumberOfSymbols> symbols(mask);
    if (!is_valid_mask(symbols)) {
      continue;
    }
    verify(get_size(Symbol::kRodata, symbols),
           get_size(Symbol::kText, symbols),
           get_size(Symbol::kDataImgRelRo, symbols),
           get_offset(Symbol::kDataImgRelRoAppImage, symbols),
           get_size(Symbol::kBss, symbols),
           get_offset(Symbol::kBssMethods, symbols),
           get_offset(Symbol::kBssRoots, symbols),
           get_size(Symbol::kDex, symbols));
  }
}

}  // namespace linker
}  // namespace art
