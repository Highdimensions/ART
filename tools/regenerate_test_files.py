#!/usr/bin/python3
#
# Copyright 2025, The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# There are many run-tests which generate their sources automatically.
# It is desirable to keep the checked-in source code, as we re-run generators very rarely.
#
# This script will re-run the generators only if their dependent files have changed and then
# complain if the outputs no longer matched what's in the source tree.

import os
import sys
import utils

DEBUG = False

def debug_print(msg):
    if DEBUG:
        print(f"[DEBUG]: {msg}", file=sys.stderr)

def main():
    utils.cd_to_art_root()

    print("[+] Running regen-test-files...")
    regen_test_files_output = utils.run_command(["test/utils/regen-test-files"])
    debug_print(regen_test_files_output)

    print("[+] Checking if TEST_MAPPING has been modified...")
    changed_files = utils.get_changed_files()
    if "TEST_MAPPING" in changed_files:
        print("[+] TEST_MAPPING has been modified, please create a new commit with the test changes!")
        print("The output of regen-test-files was:")
        print(regen_test_files_output)
        print("Copy the text above and include it in the commit message.")
        return 1
    else:
        print("[+] Nothing to do here!")
        return 0

if __name__ == "__main__":
    sys.exit(main())
