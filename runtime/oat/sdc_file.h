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

#ifndef ART_RUNTIME_OAT_SDC_FILE_H_
#define ART_RUNTIME_OAT_SDC_FILE_H_

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/macros.h"
#include "base/os.h"

namespace art HIDDEN {

// A helper class to read a secure dex metadata companion (SDC) file.
//
// Secure dex metadata companion (SDC) file is a file type that augments a secure dex metadata (SDM)
// file with additional metadata.
// It is a text file in the format of:
//   key1=value1\n
//   key2=value2\n
//   ...
// This is an extensible format, so versioning is not needed. Currently, it's only supported to have
// exactly one key, which is "apex-versions", but the parsing logic can be updated to support more
// keys.
class SdcReader {
 public:
  static std::unique_ptr<SdcReader> Load(const std::string filename, std::string* error_msg);

  std::string_view GetApexVersions() const { return apex_versions_; }

 private:
  SdcReader() = default;

  std::string content_;
  std::string_view apex_versions_;
};

// A helper class to write a secure dex metadata companion (SDC) file.
//
// Takes ownership of the file.
class EXPORT SdcWriter {
 public:
  explicit SdcWriter(File&& file) : file_(std::move(file)) {}

  void SetApexVersions(std::string_view value) { apex_versions_ = value; }

  bool Save(std::string* error_msg);

 private:
  File file_;
  std::string apex_versions_;
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_SDC_FILE_H_
