/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "sdc_file.h"

#include <memory>
#include <string>

#include "android-base/file.h"
#include "base/common_art_test.h"
#include "base/macros.h"
#include "base/os.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace art HIDDEN {

using ::android::base::ReadFileToString;
using ::android::base::WriteStringToFile;
using ::testing::StartsWith;

class SdcFileTestBase : public CommonArtTest {
 protected:
  void SetUp() override {
    CommonArtTest::SetUp();

    scratch_dir_ = std::make_unique<ScratchDir>();
    test_file_ = scratch_dir_->GetPath() + "test.sdc";
  }

  void TearDown() override {
    scratch_dir_.reset();
    CommonArtTest::TearDown();
  }

  std::unique_ptr<ScratchDir> scratch_dir_;
  std::string test_file_;
};

class SdcReaderTest : public SdcFileTestBase {};

TEST_F(SdcReaderTest, Success) {
  ASSERT_TRUE(WriteStringToFile("apex-versions=12345678\n", test_file_));

  std::string error_msg;
  std::unique_ptr<SdcReader> reader = SdcReader::Load(test_file_, &error_msg);
  ASSERT_NE(reader, nullptr) << error_msg;

  EXPECT_EQ(reader->GetApexVersions(), "12345678");
}

TEST_F(SdcReaderTest, NotFound) {
  std::string error_msg;
  std::unique_ptr<SdcReader> reader = SdcReader::Load(test_file_, &error_msg);
  ASSERT_EQ(reader, nullptr);

  EXPECT_THAT(error_msg, StartsWith("Failed to load sdc file"));
}

TEST_F(SdcReaderTest, ExtraLine) {
  ASSERT_TRUE(WriteStringToFile("apex-versions=12345678\nextra", test_file_));

  std::string error_msg;
  std::unique_ptr<SdcReader> reader = SdcReader::Load(test_file_, &error_msg);
  ASSERT_EQ(reader, nullptr);

  EXPECT_THAT(error_msg, StartsWith("Malformed sdc file"));
}

TEST_F(SdcReaderTest, WrongKey) {
  ASSERT_TRUE(WriteStringToFile("wrong-key=12345678\n", test_file_));

  std::string error_msg;
  std::unique_ptr<SdcReader> reader = SdcReader::Load(test_file_, &error_msg);
  ASSERT_EQ(reader, nullptr);

  EXPECT_THAT(error_msg, StartsWith("Missing key 'apex-versions' from sdc file"));
}

class SdcWriterTest : public SdcFileTestBase {};

TEST_F(SdcWriterTest, Success) {
  std::unique_ptr<File> file(OS::CreateEmptyFileWriteOnly(test_file_.c_str()));
  ASSERT_NE(file, nullptr);
  SdcWriter writer(std::move(*file));

  writer.SetApexVersions("12345678");

  std::string error_msg;
  ASSERT_TRUE(writer.Save(&error_msg)) << error_msg;

  std::string content;
  ASSERT_TRUE(ReadFileToString(test_file_, &content));

  EXPECT_EQ(content, "apex-versions=12345678\n");
}

TEST_F(SdcWriterTest, SaveFailed) {
  ASSERT_TRUE(WriteStringToFile("", test_file_));

  std::unique_ptr<File> file(OS::OpenFileForReading(test_file_.c_str()));
  ASSERT_NE(file, nullptr);
  SdcWriter writer(
      File(file->Release(), file->GetPath(), /*check_usage=*/false, /*read_only_mode=*/false));

  writer.SetApexVersions("12345678");

  std::string error_msg;
  EXPECT_FALSE(writer.Save(&error_msg));
}

}  // namespace art HIDDEN
