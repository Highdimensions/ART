#!/usr/bin/python3
#
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# Transforms an Arm64 ELF binary into an x86_64 format ELF binary containing the same Arm64 code.
# This allows the output x86_64 format ELF binary to be linked against other x86_64 ELF files so
# that the Arm64 code can be run using e.g: the ART simulator.
#
# The transformation has three steps:
#   1. All rela.debug* and rela.eh_frame sections are stripped off the file as the relocations these
#      use are unsupported.
#   2. The Arm64 ELF file is obj-copied into an x86_64 ELF format.
#   3. All relocations in the file are updated to compatible x86_64 relocations. Note: this
#      requires special veneers in the Arm64 code for each relocation to ensure the correct
#      addresses are used. See asm_support_arm64.S for more details.
#

import argparse
import os
import subprocess
import sys
import mmap
import logging as log
from pathlib import Path

# ELF64 sizes
ELF64_OFF = 0x8
ELF64_XWORD = 0x8
ELF64_WORD = 0x4
ELF64_HALF = 0x2

# Return an integer from a series of little-endian bytes.
# Parameters:
#   little_bytes (bytes): little-endian bytes.
def bytes_to_int(little_bytes):
  return int.from_bytes(little_bytes, byteorder="little")

# Return a tuple that corresponds to an ELF entry based on the offset to the entry and the size of
# the entry.
# Parameters:
#   offset (int): offset to the ELF entry in bytes.
#   size (int):   size of the ELF entry in bytes.
def get_elf_entry_tuple(offset, size):
  return (offset, offset + size)

# Return the integer value of an ELF entry from a section of an ELF file using a tuple.
# Parameters:
#   section (bytes):   section of an ELF file in bytes, could be the whole file.
#   elf_slice (tuple): tuple representing the offset, from the start of the section, to the start
#                      and end of the ELF entry in bytes.
def get_elf_entry(section, elf_slice):
  return bytes_to_int(section[slice(*elf_slice)])

# Return offset, size of and number of section headers in the given input file.
# Parameters:
#   mm (mmap): mmap of the ELF file.
def get_section_headers(mm):
  # ELF header reference: https://refspecs.linuxbase.org/elf/gabi4+/ch4.eheader.html
  # Offset of header entries in bytes.
  E_SHOFF = get_elf_entry_tuple(0x28, ELF64_OFF)
  E_SHENTSIZE = get_elf_entry_tuple(0x3A, ELF64_HALF)
  E_SHNUM = get_elf_entry_tuple(0x3C, ELF64_HALF)

  e_shoff = get_elf_entry(mm, E_SHOFF)
  e_shentsize = get_elf_entry(mm, E_SHENTSIZE)
  e_shnum = get_elf_entry(mm, E_SHNUM)

  return (e_shoff, e_shentsize, e_shnum)

# Return all rela sections in the given input file. Each rela section entry consists of an offset
# to the rela section and the size of the rela section.
# Parameters:
#   mm (mmap):         mmap of the ELF file.
#   e_shoff (int):     offset, in bytes, from the start of the ELF file to the section headers
#                      section.
#   e_shentsize (int): size of each section header entry in bytes.
#   e_shnum (int):     number of section header entries.
def get_rela_sections(mm, e_shoff, e_shentsize, e_shnum):
  # ELF section header reference: https://refspecs.linuxbase.org/elf/gabi4+/ch4.sheader.html
  # Offset of section header entries in bytes.
  SH_TYPE = get_elf_entry_tuple(0x4, ELF64_WORD)
  SH_OFFSET = get_elf_entry_tuple(0x18, ELF64_OFF)
  SH_SIZE = get_elf_entry_tuple(0x20, ELF64_XWORD)

  # Type number of RELA sections is 4.
  RELA_SECTION_TYPE = 4

  rela_sections = []
  for i in range(e_shnum):
    # Offset to the current section header entry.
    offset = (e_shoff + (e_shentsize * i))
    sh_entry = mm[offset:(offset + e_shentsize)]

    sh_type = get_elf_entry(sh_entry, SH_TYPE)

    if (sh_type == RELA_SECTION_TYPE):
      sh_offset = get_elf_entry(sh_entry, SH_OFFSET)
      sh_size = get_elf_entry(sh_entry, SH_SIZE)
      rela_sections.append((sh_offset, sh_size))

  return rela_sections

# Update all the relocation types in a given relocation section.
# Parameters:
#   mm (mmap):       mmap of the ELF file.
#   sh_offset (int): offset, in bytes, from the start of the ELF file to the relocation section.
#   sh_size (int):   size of the relocation section in bytes.
def update_relocations_in_section(mm, sh_offset, sh_size):
  # Relocation section reference: https://refspecs.linuxbase.org/elf/gabi4+/ch4.reloc.html
  # Each rela entry is 24 bytes (3 64bit words).
  RELA_ENTRY_SIZE = 24

  # Relocation type is the lower 4 bytes of r_info(byte 8-15), which is byte 8-11
  R_TYPE = get_elf_entry_tuple(0x8, ELF64_WORD)

  # Define the relocation type mapping. This mapping need to be aligned with the expected mapping
  # in the assembly code.
  # Relocation types references documents:
  #   AArch64: https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst#relocation-types
  #   x86_64:  https://gitlab.com/x86-psABIs/x86-64-ABI
  # Mapping from R_AARCH64_NONE (0) to R_X86_64_PC64 (24)
  from_type = 0
  to_type = 24

  num_entries = int(sh_size / RELA_ENTRY_SIZE)

  for i in range(num_entries):
    offset = sh_offset + (RELA_ENTRY_SIZE * i)
    relocation_entry_tuple = get_elf_entry_tuple(offset, RELA_ENTRY_SIZE)
    relocation_entry = mm[slice(*relocation_entry_tuple)]

    relocation_type = get_elf_entry(relocation_entry, R_TYPE)
    if (relocation_type != from_type):
      log.error(f"Relocation type ({relocation_type}) does not have a mapping.")
      sys.exit(1)

    # Overwrite the original relocation type with our mapped type.
    mm[offset+R_TYPE[0]:offset+R_TYPE[1]] = to_type.to_bytes(4, byteorder="little")

# Update specific Arm64 relocation types to compatible x86_64 relocation types.
# Parameters:
#   in_file (string):  input x86_64 format ELF file.
#   out_file (string): output x86_64 format ELF file with updated relocations.
def update_relocations(in_file, out_file):
  # Copy the file as we will modify it in place.
  subprocess.run(f"cp {in_file} {out_file}", shell=True, check=True)

  # Open and map the whole file.
  file = open(out_file, "a+b")
  mm = mmap.mmap(file.fileno(), 0)

  (e_shoff, e_shentsize, e_shnum) = get_section_headers(mm)
  rela_sections = get_rela_sections(mm, e_shoff, e_shentsize, e_shnum)

  for (offset,size) in rela_sections:
    update_relocations_in_section(mm, offset, size)

  # Flush the changes to the mmap and close it.
  mm.flush()
  mm.close()
  file.close()

# Transform an Arm64 format ELF file into an x86_64 format ELF file containing Arm64 code.
#   in_file (string):  input Arm64 format ELF file.
#   out_file (string): output x86_64 format ELF file with updated relocations.
def transform_arm64_to_x86_64(in_file, out_file):
  # .debug_* and .eh_frame sections (https://refspecs.linuxfoundation.org/LSB_3.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html)
  # both use unsupported relocations to arbitrary code locations which we can't affect with in code
  # changes, therefore remove them.
  log.info("Stripping debug and eh_frame sections...")
  strip_args = "--strip-debug --remove-section='.rela.eh_frame'"
  subprocess.run(f"{args.objcopy} {strip_args} {in_file} {in_file}-stripped",
      shell=True, check=True)


  log.info("Obj-Copying to x86_64...")
  objcopy_args = "-O elf64-x86-64 -I elf64-aarch64"
  subprocess.run(f"{args.objcopy} {objcopy_args} {in_file}-stripped {in_file}-copied",
      shell=True, check=True)

  log.info("Updating relocations...")
  update_relocations(f"{in_file}-copied", f"{out_file}")

def get_input_file():
  # The TMP_DIR environment variable should be in the out/soong/ directory. This is used to search
  # the out/soong directory for the built arm64 output file.
  out_dir = Path(f"{os.environ['TMPDIR']}/../").resolve()
  if (out_dir.name != "soong" or out_dir.parent.name != "out"):
    log.error(f"TMP_DIR path not in 'out/soong/' as expected: {out_dir}")
    sys.exit(1)

  # Find the latest modified arm64 ELF file which matches the name of the expected x86_64 output.
  # This Arm64 file should have been built prior to this script running as part of a cc_object
  # module.
  input_file = Path(args.output).stem + ".o"
  input_file = input_file.replace("x86_64", "arm64")
  potential_inputs = out_dir.glob(f"**/cc_object_*/**/{input_file}")
  if (not potential_inputs):
    log.error(f"Could not find: cc_object_*/**/{input_file}.")
    sys.exit(1)

  return max(potential_inputs, key=os.path.getctime)

def main():
  if (args.verbose):
    log.basicConfig(format="%(levelname)s: %(message)s", level=log.DEBUG)
  else:
    log.basicConfig(format="%(levelname)s: %(message)s")

  input_file = get_input_file()

  log.info("Transforming Arm64 to x86_64")
  transform_arm64_to_x86_64(input_file, args.output)

  log.info("Done!")


if __name__ == "__main__":
  parser = argparse.ArgumentParser(description="Transform an Arm64 ELF file into an x86_64 format"
    + " ELF file.")
  parser.add_argument("--objcopy", type=Path, default="llvm-objcopy", help="Path to llvm-objcopy")
  parser.add_argument("-v", "--verbose", action="store_true")
  parser.add_argument("output", help="x86_64 format ELF file.")
  args = parser.parse_args()

  main()
