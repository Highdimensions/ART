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
#include <string_view>
#include <vector>

#include "android-base/file.h"
#include "android-base/strings.h"
#include "base/macros.h"
#include "base/utils.h"

namespace art HIDDEN {

using ::android::base::ConsumePrefix;
using ::android::base::ReadFileToString;
using ::android::base::WriteStringToFd;

std::unique_ptr<SdcReader> SdcReader::Load(const std::string filename, std::string* error_msg) {
  std::unique_ptr<SdcReader> reader(new SdcReader());

  // The sdc file is supposed to be small, so read fully into memory for simplicity.
  if (!ReadFileToString(filename, &reader->content_)) {
    *error_msg = ART_FORMAT("Failed to load sdc file '{}': {}", filename, strerror(errno));
    return nullptr;
  }

  std::vector<std::string_view> lines;
  Split(reader->content_, '\n', &lines);
  if (lines.size() != 1) {
    *error_msg = ART_FORMAT("Malformed sdc file '{}'. Expected a single line", filename);
    return nullptr;
  }

  if (!ConsumePrefix(&lines[0], "apex-versions=")) {
    *error_msg = ART_FORMAT("Missing key 'apex-versions' from sdc file '{}'", filename);
    return nullptr;
  }
  reader->apex_versions_ = lines[0];

  return reader;
}

bool SdcWriter::Save(std::string* error_msg) {
  if (!file_.ClearContent()) {
    *error_msg = ART_FORMAT(
        "Failed to clear sdc file '{}' for writing: {}", file_.GetPath(), strerror(errno));
    return false;
  }
  if (!WriteStringToFd(ART_FORMAT("apex-versions={}\n", apex_versions_), file_.Fd())) {
    *error_msg = ART_FORMAT("Failed to write sdc file '{}': {}", file_.GetPath(), strerror(errno));
    return false;
  }
  int res = file_.FlushClose();
  if (res != 0) {
    *error_msg =
        ART_FORMAT("Failed to flush close sdc file '{}': {}", file_.GetPath(), strerror(-res));
    return false;
  }
  return true;
}

}  // namespace art
