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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_LLVM_UTILS_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_LLVM_UTILS_H_

#include "aarch64/instructions-aarch64.h"
#include "arch/instruction_set.h"
#include "base/arena_containers.h"
#include "base/logging.h"
#include "base/macros.h"
#include "data_type.h"
#include "gc_root.h"
#include "mirror/object.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/ProfDataUtils.h"
#include "llvm/IR/Type.h"
#pragma GCC diagnostic pop

namespace art HIDDEN {
namespace arm64_llvm {

inline constexpr unsigned kUncompressedGCAddressSpace = 1;
inline constexpr unsigned kCompressedGCPointerAddressSpace = 2;
inline constexpr unsigned kMethodPointerAddressSpace = 10;
inline constexpr unsigned kCriticalNativeFunctionAddressSpace = 11;

inline constexpr uint32_t kCriticalNativeFunctionRegister = 30;  // x30/lr

inline constexpr uint32_t kImplicitSuspendCheckPatchSize = vixl::aarch64::kInstructionSize;
inline constexpr uint32_t kCriticalNativePatchSize = 3 * vixl::aarch64::kInstructionSize;
inline constexpr uint32_t kEntrypointThunkPatchSize = vixl::aarch64::kInstructionSize;

inline constexpr size_t kCurrentMethodIndex = 0;
inline constexpr size_t kCallerMethodIndex = 1;
inline constexpr size_t kParametersBeginIndex = 2;

inline constexpr size_t kClinitCheckPrologueSize = 15;
inline constexpr size_t kStackOverflowCheckPrologueSize = 2;

inline constexpr std::string_view kArtGCStrategyName = "art";
inline constexpr std::string_view kParameterAllocaMetadata = "art.parameter.alloca";
inline constexpr std::string_view kInstructionAllocaMetadata = "art.instruction.alloca";
inline constexpr std::string_view kGCPointerAllocaMetadata = "art.gc.ptr.alloca";
inline constexpr std::string_view kGCPointerCastMetadata = "art.gc.ptr.cast";
inline constexpr std::string_view kGCPointerCastToIntMetadata = "art.gc.ptr.cast.to.int";
inline constexpr std::string_view kHeapReferencePoisoningMetadata = "art.heap.reference.poisoning";
inline constexpr std::string_view kMemCpyI16Metadata = "art.memcpy.i16";
inline constexpr std::string_view kMemCpyI32Metadata = "art.memcpy.i32";
inline constexpr std::string_view kMemCpyI8ZextToI16Metadata = "art.memcpy.i8.zext.to.i16";
inline constexpr std::string_view kStringEqualsMetadata = "art.string.equals";

inline constexpr std::string_view kPlaceholderFunctionMetadata = "art.placeholder.function";
inline constexpr std::string_view kRewriteToPatchpointMetadata = "art.rewrite.to.patchpoint";

inline constexpr std::string_view kARTPersonalityFunctionName = "__art_personality";

struct LLVMTypes {
  llvm::Type* ptr = nullptr;
  llvm::Type* uncompressed_gc_ptr = nullptr;
  llvm::Type* compressed_gc_ptr = nullptr;
  llvm::Type* method_ptr = nullptr;
  llvm::Type* boolean = nullptr;
  llvm::Type* i8 = nullptr;
  llvm::Type* i16 = nullptr;
  llvm::Type* i32 = nullptr;
  llvm::Type* i64 = nullptr;
  llvm::Type* f32 = nullptr;
  llvm::Type* f64 = nullptr;
  llvm::Type* void_ = nullptr;
};

inline llvm::Type* GetLLVMType(DataType::Type type, const LLVMTypes& llvm_types) {
  switch (type) {
    case DataType::Type::kReference:
      return llvm_types.uncompressed_gc_ptr;
    case DataType::Type::kBool:
      return llvm_types.boolean;
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
      return llvm_types.i8;
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      return llvm_types.i16;
    case DataType::Type::kUint32:
    case DataType::Type::kInt32:
      return llvm_types.i32;
    case DataType::Type::kUint64:
    case DataType::Type::kInt64:
      return llvm_types.i64;
    case DataType::Type::kFloat32:
      return llvm_types.f32;
    case DataType::Type::kFloat64:
      return llvm_types.f64;
    case DataType::Type::kVoid:
      return llvm_types.void_;
  }
}

inline LLVMTypes GetLLVMTypes(llvm::LLVMContext& llvm_context) {
  LLVMTypes llvm_types;
  llvm_types.ptr = llvm::PointerType::get(llvm_context, 0);
  llvm_types.uncompressed_gc_ptr =
      llvm::PointerType::get(llvm_context, kUncompressedGCAddressSpace);
  llvm_types.compressed_gc_ptr =
      llvm::PointerType::get(llvm_context, kCompressedGCPointerAddressSpace);
  llvm_types.method_ptr = llvm::PointerType::get(llvm_context, kMethodPointerAddressSpace);
  llvm_types.boolean = llvm::Type::getInt1Ty(llvm_context);
  llvm_types.i8 = llvm::Type::getInt8Ty(llvm_context);
  llvm_types.i16 = llvm::Type::getInt16Ty(llvm_context);
  llvm_types.i32 = llvm::Type::getInt32Ty(llvm_context);
  llvm_types.i64 = llvm::Type::getInt64Ty(llvm_context);
  llvm_types.f32 = llvm::Type::getFloatTy(llvm_context);
  llvm_types.f64 = llvm::Type::getDoubleTy(llvm_context);
  llvm_types.void_ = llvm::Type::getVoidTy(llvm_context);
  return llvm_types;
}

// Helper functions for setting cold and hot branches. Clang generates the 2000:1 ratio when using
// __builtin_expect(), so we do that here as well.
inline void ExpectTrueBranch(llvm::Instruction* instruction) {
  llvm::setBranchWeights(*instruction, {2000, 1}, true);
}

inline void ExpectFalseBranch(llvm::Instruction* instruction) {
  llvm::setBranchWeights(*instruction, {1, 2000}, true);
}

enum class PatchpointKind : uint32_t {
  // No patching needed.
  kNone,

  // Stackmap intrinsic used to record allocas that contain heap references.
  kGCPointerAllocaMap,
  // Used to prevent the optimizer from moving stack stores into the unwind block.
  kTryBoundaryStackReadClobber,

  // ldr x21, [x21]
  kImplicitSuspendCheck,

  // Doesn't do anything, just needed to keep heap references live.
  kReachabilityFence,

  // sub sp, sp, #size
  // blr lr
  // add sp, sp, #size
  kCriticalNativeCall,

  kLoadGcRoot,   // ldr  wA, [xB]
  kLoadBoolean,  // ldrb wA, [xB]
  kLoadInt8,     // ldrb wA, [xB]
  kLoadInt16,    // ldrh wA, [xB]
  kLoadInt32,    // ldr  wA, [xB]
  kLoadInt64,    // ldr  xA, [xB]
  kLoadFloat32,  // ldr  sA, [xB]
  kLoadFloat64,  // ldr  dA, [xB]

  kLoadAcquireGcRoot,   // ldar  wA, [xB]
  kLoadAcquireBoolean,  // ldarb wA, [xB]
  kLoadAcquireInt8,     // ldarb wA, [xB]
  kLoadAcquireInt16,    // ldarh wA, [xB]
  kLoadAcquireInt32,    // ldar  wA, [xB]
  kLoadAcquireInt64,    // ldar  xA, [xB]

  // ldr wzr, [xA]
  kDiscardedLoad,

  kStoreGcRoot,   // str  wA, [xB]
  kStoreBoolean,  // strb wA, [xB]
  kStoreInt8,     // strb wA, [xB]
  kStoreInt16,    // strh wA, [xB]
  kStoreInt32,    // str  wA, [xB]
  kStoreInt64,    // str  xA, [xB]
  kStoreFloat32,  // str  sA, [xB]
  kStoreFloat64,  // str  dA, [xB]

  kStoreReleaseGcRoot,   // stlr  wA, [xB]
  kStoreReleaseBoolean,  // stlrb wA, [xB]
  kStoreReleaseInt8,     // stlrb wA, [xB]
  kStoreReleaseInt16,    // stlrh wA, [xB]
  kStoreReleaseInt32,    // stlr  wA, [xB]
  kStoreReleaseInt64,    // stlr  xA, [xB]

  // NOTE: Exact offsets will be patched at link time.
  // adrp xA, #0x0
  // ldr [x|w]A, [xA, #0] or add xA, xA, #0
  kLoadClassBootImageLinkTimePcRelative,
  kLoadClassBootImageRelRo,
  kLoadClassAppImageRelRo,
  kLoadClassBootImageIntrinsic,
  kLoadClassBssEntry,
  kLoadClassBssEntryPublic,
  kLoadClassBssEntryPackage,
  kLoadStringBootImageRelRo,
  kLoadStringBootImageLinkTimePcRelative,
  kLoadStringBssEntry,
  kLoadMethodTypeBssEntry,
  kLoadMethodBootImageLinkTimePcRelative,
  kLoadMethodBootImageRelRo,
  kLoadMethodAppImageRelRo,
  kLoadMethodBootImageJni,
  kLoadMethodBssEntry,

  // NOTE: The actual offset will be patched at link time.
  // bl #0
  kEntrypointThunkCallStatepoint,  // uses @llvm.experimental.statepoint
  kEntrypointThunkCallPatchpoint,  // uses @llvm.experimental.patchpoint

  // Marks the start of a catch block.
  kCatchBlock,

  kLast = kCatchBlock,
};

// Encodes the patchpoints's kind and a 32-bit index into the patchpoints's 64-bit ID.
inline uint64_t EncodePatchpointID(PatchpointKind kind, uint32_t index) {
  return static_cast<uint64_t>(kind) | (static_cast<uint64_t>(index) << 32);
}

inline PatchpointKind DecodePatchpointKindFromID(uint64_t id) {
  DCHECK_LE(static_cast<uint32_t>(id), static_cast<uint32_t>(PatchpointKind::kLast));
  return static_cast<PatchpointKind>(static_cast<uint32_t>(id));
}

inline uint32_t DecodePatchpointIndexFromID(uint64_t id) { return static_cast<uint32_t>(id >> 32); }

inline int GetPatchpointAdrpLoadSize(PatchpointKind kind) {
  switch (kind) {
    case PatchpointKind::kLoadClassBootImageLinkTimePcRelative:
    case PatchpointKind::kLoadClassBootImageRelRo:
    case PatchpointKind::kLoadClassBootImageIntrinsic:
    case PatchpointKind::kLoadClassBssEntry:
    case PatchpointKind::kLoadClassBssEntryPublic:
    case PatchpointKind::kLoadClassBssEntryPackage:
    case PatchpointKind::kLoadStringBootImageRelRo:
    case PatchpointKind::kLoadStringBootImageLinkTimePcRelative:
    case PatchpointKind::kLoadStringBssEntry:
    case PatchpointKind::kLoadMethodTypeBssEntry:
      return 8 * sizeof(GcRoot<mirror::Object>);
    case PatchpointKind::kLoadMethodBootImageRelRo:
    case PatchpointKind::kLoadMethodAppImageRelRo:
    case PatchpointKind::kLoadMethodBootImageLinkTimePcRelative:
    case PatchpointKind::kLoadClassAppImageRelRo:
      // NOTE: Boot image/app image is in the low 4GiB and the entry is 32-bit, so use a 32-bit
      // load.
      return 4 * static_cast<int>(kArm64PointerSize);
    case PatchpointKind::kLoadMethodBssEntry:
    case PatchpointKind::kLoadMethodBootImageJni:
      return 8 * static_cast<int>(kArm64PointerSize);
    case PatchpointKind::kNone:
    case PatchpointKind::kGCPointerAllocaMap:
    case PatchpointKind::kTryBoundaryStackReadClobber:
    case PatchpointKind::kImplicitSuspendCheck:
    case PatchpointKind::kReachabilityFence:
    case PatchpointKind::kCriticalNativeCall:
    case PatchpointKind::kLoadGcRoot:
    case PatchpointKind::kLoadBoolean:
    case PatchpointKind::kLoadInt8:
    case PatchpointKind::kLoadInt16:
    case PatchpointKind::kLoadInt32:
    case PatchpointKind::kLoadInt64:
    case PatchpointKind::kLoadFloat32:
    case PatchpointKind::kLoadFloat64:
    case PatchpointKind::kLoadAcquireGcRoot:
    case PatchpointKind::kLoadAcquireBoolean:
    case PatchpointKind::kLoadAcquireInt8:
    case PatchpointKind::kLoadAcquireInt16:
    case PatchpointKind::kLoadAcquireInt32:
    case PatchpointKind::kLoadAcquireInt64:
    case PatchpointKind::kDiscardedLoad:
    case PatchpointKind::kStoreGcRoot:
    case PatchpointKind::kStoreBoolean:
    case PatchpointKind::kStoreInt8:
    case PatchpointKind::kStoreInt16:
    case PatchpointKind::kStoreInt32:
    case PatchpointKind::kStoreInt64:
    case PatchpointKind::kStoreFloat32:
    case PatchpointKind::kStoreFloat64:
    case PatchpointKind::kStoreReleaseGcRoot:
    case PatchpointKind::kStoreReleaseBoolean:
    case PatchpointKind::kStoreReleaseInt8:
    case PatchpointKind::kStoreReleaseInt16:
    case PatchpointKind::kStoreReleaseInt32:
    case PatchpointKind::kStoreReleaseInt64:
    case PatchpointKind::kEntrypointThunkCallStatepoint:
    case PatchpointKind::kEntrypointThunkCallPatchpoint:
    case PatchpointKind::kCatchBlock:
      LOG(FATAL) << "Unexpected patchpoint kind";
      UNREACHABLE();
  }
}

inline uint32_t LoadInstruction(const ArenaVector<uint8_t>& code, uint32_t offset) {
  DCHECK_LE(offset + 4, code.size());
  DCHECK_ALIGNED(offset, 4);
  return (static_cast<uint32_t>(code[offset + 0]) << 0) |
         (static_cast<uint32_t>(code[offset + 1]) << 8) |
         (static_cast<uint32_t>(code[offset + 2]) << 16) |
         (static_cast<uint32_t>(code[offset + 3]) << 24);
}

inline void StoreInstruction(ArenaVector<uint8_t>& code, uint32_t offset, uint32_t inst) {
  DCHECK_LE(offset + 4, code.size());
  DCHECK_ALIGNED(offset, 4);
  code[offset + 0] = (inst >> 0) & 0xff;
  code[offset + 1] = (inst >> 8) & 0xff;
  code[offset + 2] = (inst >> 16) & 0xff;
  code[offset + 3] = (inst >> 24) & 0xff;
}

inline constexpr uint32_t kLdr8Opcode = 0x3940'0000;
inline constexpr uint32_t kLdr16Opcode = 0x7940'0000;
inline constexpr uint32_t kLdr32Opcode = 0xb940'0000;
inline constexpr uint32_t kLdr64Opcode = 0xf940'0000;
inline constexpr uint32_t kLdrFP32Opcode = 0xbd40'0000;
inline constexpr uint32_t kLdrFP64Opcode = 0xfd40'0000;
inline constexpr uint32_t kLdr8OffsetScaleShift = 0;
inline constexpr uint32_t kLdr16OffsetScaleShift = 1;
inline constexpr uint32_t kLdr32OffsetScaleShift = 2;
inline constexpr uint32_t kLdr64OffsetScaleShift = 3;
inline constexpr uint32_t kLdrDestShift = 0;
inline constexpr uint32_t kLdrAddressShift = 5;
inline constexpr uint32_t kLdrOffsetShift = 10;
inline constexpr uint32_t kLdrOffsetWidth = 12;

inline constexpr uint32_t kLdrLiteralMask = 0xff00'0000;
inline constexpr uint32_t kLdrLiteral32Opcode = 0x1800'0000;
inline constexpr uint32_t kLdrLiteral64Opcode = 0x5800'0000;
inline constexpr uint32_t kLdrLiteralFP32Opcode = 0x1c00'0000;
inline constexpr uint32_t kLdrLiteralFP64Opcode = 0x5c00'0000;
inline constexpr uint32_t kLdrLiteralFP128Opcode = 0x7c00'0000;
inline constexpr uint32_t kLdrLiteralOffsetShift = 5;

inline constexpr uint32_t kLdar8Opcode = 0x08df'fc00;
inline constexpr uint32_t kLdar16Opcode = 0x48df'fc00;
inline constexpr uint32_t kLdar32Opcode = 0x88df'fc00;
inline constexpr uint32_t kLdar64Opcode = 0xc8df'fc00;
inline constexpr uint32_t kLdarDestShift = 0;
inline constexpr uint32_t kLdarAddressShift = 5;

inline constexpr uint32_t kLdur64Opcode = 0xf840'0000;
inline constexpr uint32_t kLdurFP64Opcode = 0xfc40'0000;
inline constexpr uint32_t kLdurDestShift = 0;
inline constexpr uint32_t kLdurAddressShift = 5;
inline constexpr uint32_t kLdurOffsetShift = 12;
inline constexpr uint32_t kLdurOffsetWidth = 9;

inline constexpr uint32_t kAdrMask = 0x9f00'0000;
inline constexpr uint32_t kAdrImmMask = 0x60ff'ffe0;
inline constexpr uint32_t kAdrOpcode = 0x1000'0000;
inline constexpr uint32_t kAdrImmHiShift = 5;
inline constexpr uint32_t kAdrImmLoShift = 29;

inline constexpr uint32_t kAdrpOpcode = 0x90000000;
inline constexpr uint32_t kAdrpDestShift = 0;

inline constexpr uint32_t kStr8Opcode = 0x3900'0000;
inline constexpr uint32_t kStr16Opcode = 0x7900'0000;
inline constexpr uint32_t kStr32Opcode = 0xb900'0000;
inline constexpr uint32_t kStr64Opcode = 0xf900'0000;
inline constexpr uint32_t kStrFP32Opcode = 0xbd00'0000;
inline constexpr uint32_t kStrFP64Opcode = 0xfd00'0000;
inline constexpr uint32_t kStr8OffsetScaleShift = 0;
inline constexpr uint32_t kStr16OffsetScaleShift = 1;
inline constexpr uint32_t kStr32OffsetScaleShift = 2;
inline constexpr uint32_t kStr64OffsetScaleShift = 3;
inline constexpr uint32_t kStrValueShift = 0;
inline constexpr uint32_t kStrAddressShift = 5;
inline constexpr uint32_t kStrOffsetShift = 10;
inline constexpr uint32_t kStrOffsetWidth = 12;

inline constexpr uint32_t kStlr8Opcode = 0x089f'fc00;
inline constexpr uint32_t kStlr16Opcode = 0x489f'fc00;
inline constexpr uint32_t kStlr32Opcode = 0x889f'fc00;
inline constexpr uint32_t kStlr64Opcode = 0xc89f'fc00;
inline constexpr uint32_t kStlrValueShift = 0;
inline constexpr uint32_t kStlrAddressShift = 5;

inline constexpr uint32_t kStur64Opcode = 0xf800'0000;
inline constexpr uint32_t kSturFP64Opcode = 0xfc00'0000;
inline constexpr uint32_t kSturValueShift = 0;
inline constexpr uint32_t kSturAddressShift = 5;
inline constexpr uint32_t kSturOffsetShift = 12;
inline constexpr uint32_t kSturOffsetWidth = 9;

inline constexpr uint32_t kAdd32ImmediateOpcode = 0x1100'0000;
inline constexpr uint32_t kAdd64ImmediateOpcode = 0x9100'0000;
inline constexpr uint32_t kAddImmediateResultShift = 0;
inline constexpr uint32_t kAddImmediateLhsShift = 5;
inline constexpr uint32_t kAddImmediateImmediateShift = 10;
inline constexpr uint32_t kAddImmediateImmediateWidth = 12;
inline constexpr uint32_t kAddImmediateShiftBit = 1u << 22;

inline constexpr uint32_t kSub32ImmediateOpcode = 0x5100'0000;
inline constexpr uint32_t kSub64ImmediateOpcode = 0xd100'0000;
inline constexpr uint32_t kSubImmediateResultShift = 0;
inline constexpr uint32_t kSubImmediateLhsShift = 5;
inline constexpr uint32_t kSubImmediateImmediateShift = 10;
inline constexpr uint32_t kSubImmediateImmediateWidth = 12;
inline constexpr uint32_t kSubImmediateShiftBit = 1u << 22;

inline constexpr uint32_t kMov32Opcode = 0x2a00'03e0;
inline constexpr uint32_t kMov64Opcode = 0xaa00'03e0;
inline constexpr uint32_t kMovDestShift = 0;
inline constexpr uint32_t kMovSourceShift = 16;

inline constexpr uint32_t kBlrOpcode = 0xd63f'0000;
inline constexpr uint32_t kBlrCalleeShift = 5;

inline constexpr uint32_t kBlOpcode = 0x9400'0000;
inline constexpr uint32_t kBlImmWidth = 26;
inline constexpr uint32_t kBlImmScaleShift = 2;

inline constexpr uint32_t kWzrRegisterNumber = 31;
inline constexpr uint32_t kSpRegisterNumber = 31;

// Creates a `ldrb wA, [xB, #offset]` instruction.
inline uint32_t CreateLdr8(uint32_t dest_register, uint32_t address_register, uint32_t offset = 0) {
  uint32_t inst = kLdr8Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdrDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kLdr8OffsetScaleShift);
  uint32_t imm12 = offset >> kLdr8OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kLdrOffsetWidth);
  inst |= imm12 << kLdrOffsetShift;
  return inst;
}

// Creates a `ldrh wA, [xB, #offset]` instruction.
inline uint32_t CreateLdr16(uint32_t dest_register,
                            uint32_t address_register,
                            uint32_t offset = 0) {
  uint32_t inst = kLdr16Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdrDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kLdr16OffsetScaleShift);
  uint32_t imm12 = offset >> kLdr16OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kLdrOffsetWidth);
  inst |= imm12 << kLdrOffsetShift;
  return inst;
}

// Creates a `ldr wA, [xB, #offset]` instruction.
inline uint32_t CreateLdr32(uint32_t dest_register,
                            uint32_t address_register,
                            uint32_t offset = 0) {
  uint32_t inst = kLdr32Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdrDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kLdr32OffsetScaleShift);
  uint32_t imm12 = offset >> kLdr32OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kLdrOffsetWidth);
  inst |= imm12 << kLdrOffsetShift;
  return inst;
}

// Creates a `ldr xA, [xB, #offset]` instruction.
inline uint32_t CreateLdr64(uint32_t dest_register,
                            uint32_t address_register,
                            uint32_t offset = 0) {
  uint32_t inst = kLdr64Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdrDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kLdr64OffsetScaleShift);
  uint32_t imm12 = offset >> kLdr64OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kLdrOffsetWidth);
  inst |= imm12 << kLdrOffsetShift;
  return inst;
}

// Creates a `ldr sA, [xB, #offset]` instruction.
inline uint32_t CreateLdrFP32(uint32_t dest_register,
                              uint32_t address_register,
                              uint32_t offset = 0) {
  uint32_t inst = kLdrFP32Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdrDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kLdr32OffsetScaleShift);
  uint32_t imm12 = offset >> kLdr32OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kLdrOffsetWidth);
  inst |= imm12 << kLdrOffsetShift;
  return inst;
}

// Creates a `ldr dA, [xB, #offset]` instruction.
inline uint32_t CreateLdrFP64(uint32_t dest_register,
                              uint32_t address_register,
                              uint32_t offset = 0) {
  uint32_t inst = kLdrFP64Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdrDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kLdr64OffsetScaleShift);
  uint32_t imm12 = offset >> kLdr64OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kLdrOffsetWidth);
  inst |= imm12 << kLdrOffsetShift;
  return inst;
}

// Creates a `ldarb wA, [xB]` instruction.
inline uint32_t CreateLdar8(uint32_t dest_register, uint32_t address_register) {
  uint32_t inst = kLdar8Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdarDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdarAddressShift;
  return inst;
}

// Creates a `ldarh wA, [xB]` instruction.
inline uint32_t CreateLdar16(uint32_t dest_register, uint32_t address_register) {
  uint32_t inst = kLdar16Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdarDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdarAddressShift;
  return inst;
}

// Creates a `ldar wA, [xB]` instruction.
inline uint32_t CreateLdar32(uint32_t dest_register, uint32_t address_register) {
  uint32_t inst = kLdar32Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdarDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdarAddressShift;
  return inst;
}

// Creates a `ldar xA, [xB]` instruction.
inline uint32_t CreateLdar64(uint32_t dest_register, uint32_t address_register) {
  uint32_t inst = kLdar64Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdarDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdarAddressShift;
  return inst;
}

// Creates a `ldur xA, [xB]` instruction.
inline uint32_t CreateLdur64(uint32_t dest_register, uint32_t address_register) {
  uint32_t inst = kLdur64Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdurDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdurAddressShift;
  return inst;
}

// Creates a `ldur dA, [xB]` instruction.
inline uint32_t CreateLdurFP64(uint32_t dest_register, uint32_t address_register) {
  uint32_t inst = kLdurFP64Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kLdurDestShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kLdurAddressShift;
  return inst;
}

inline uint32_t CreateLdr(PatchpointKind kind,
                          uint32_t dest_register,
                          uint32_t address_register,
                          uint32_t offset = 0) {
  switch (kind) {
    case PatchpointKind::kLoadBoolean:
    case PatchpointKind::kLoadInt8:
      return CreateLdr8(dest_register, address_register, offset);
    case PatchpointKind::kLoadInt16:
      return CreateLdr16(dest_register, address_register, offset);
    case PatchpointKind::kLoadGcRoot:
    case PatchpointKind::kLoadInt32:
      return CreateLdr32(dest_register, address_register, offset);
    case PatchpointKind::kLoadInt64:
      return CreateLdr64(dest_register, address_register, offset);
    case PatchpointKind::kLoadFloat32:
      return CreateLdrFP32(dest_register, address_register, offset);
    case PatchpointKind::kLoadFloat64:
      return CreateLdrFP64(dest_register, address_register, offset);
    case PatchpointKind::kLoadAcquireBoolean:
    case PatchpointKind::kLoadAcquireInt8:
      DCHECK_EQ(offset, 0u);
      return CreateLdar8(dest_register, address_register);
    case PatchpointKind::kLoadAcquireInt16:
      DCHECK_EQ(offset, 0u);
      return CreateLdar16(dest_register, address_register);
    case PatchpointKind::kLoadAcquireGcRoot:
    case PatchpointKind::kLoadAcquireInt32:
      DCHECK_EQ(offset, 0u);
      return CreateLdar32(dest_register, address_register);
    case PatchpointKind::kLoadAcquireInt64:
      DCHECK_EQ(offset, 0u);
      return CreateLdar64(dest_register, address_register);
    case PatchpointKind::kNone:
    case PatchpointKind::kGCPointerAllocaMap:
    case PatchpointKind::kTryBoundaryStackReadClobber:
    case PatchpointKind::kImplicitSuspendCheck:
    case PatchpointKind::kReachabilityFence:
    case PatchpointKind::kCriticalNativeCall:
    case PatchpointKind::kDiscardedLoad:
    case PatchpointKind::kStoreGcRoot:
    case PatchpointKind::kStoreBoolean:
    case PatchpointKind::kStoreInt8:
    case PatchpointKind::kStoreInt16:
    case PatchpointKind::kStoreInt32:
    case PatchpointKind::kStoreInt64:
    case PatchpointKind::kStoreFloat32:
    case PatchpointKind::kStoreFloat64:
    case PatchpointKind::kStoreReleaseGcRoot:
    case PatchpointKind::kStoreReleaseBoolean:
    case PatchpointKind::kStoreReleaseInt8:
    case PatchpointKind::kStoreReleaseInt16:
    case PatchpointKind::kStoreReleaseInt32:
    case PatchpointKind::kStoreReleaseInt64:
    case PatchpointKind::kLoadClassBootImageLinkTimePcRelative:
    case PatchpointKind::kLoadClassBootImageRelRo:
    case PatchpointKind::kLoadClassBootImageIntrinsic:
    case PatchpointKind::kLoadClassAppImageRelRo:
    case PatchpointKind::kLoadClassBssEntry:
    case PatchpointKind::kLoadClassBssEntryPublic:
    case PatchpointKind::kLoadClassBssEntryPackage:
    case PatchpointKind::kLoadStringBootImageRelRo:
    case PatchpointKind::kLoadStringBootImageLinkTimePcRelative:
    case PatchpointKind::kLoadStringBssEntry:
    case PatchpointKind::kLoadMethodTypeBssEntry:
    case PatchpointKind::kLoadMethodBootImageRelRo:
    case PatchpointKind::kLoadMethodAppImageRelRo:
    case PatchpointKind::kLoadMethodBootImageLinkTimePcRelative:
    case PatchpointKind::kLoadMethodBootImageJni:
    case PatchpointKind::kLoadMethodBssEntry:
    case PatchpointKind::kEntrypointThunkCallStatepoint:
    case PatchpointKind::kEntrypointThunkCallPatchpoint:
    case PatchpointKind::kCatchBlock:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
  }
}

// Creates a `strb wA, [xB, #offset]` instruction.
inline uint32_t CreateStr8(uint32_t value_register,
                           uint32_t address_register,
                           uint32_t offset = 0) {
  uint32_t inst = kStr8Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kStr8OffsetScaleShift);
  uint32_t imm12 = offset >> kStr8OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kStrOffsetWidth);
  inst |= imm12 << kStrOffsetShift;
  return inst;
}

// Creates a `strh wA, [xB, #offset]` instruction.
inline uint32_t CreateStr16(uint32_t value_register,
                            uint32_t address_register,
                            uint32_t offset = 0) {
  uint32_t inst = kStr16Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kStr16OffsetScaleShift);
  uint32_t imm12 = offset >> kStr16OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kStrOffsetWidth);
  inst |= imm12 << kStrOffsetShift;
  return inst;
}

// Creates a `str wA, [xB, #offset]` instruction.
inline uint32_t CreateStr32(uint32_t value_register,
                            uint32_t address_register,
                            uint32_t offset = 0) {
  uint32_t inst = kStr32Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kStr32OffsetScaleShift);
  uint32_t imm12 = offset >> kStr32OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kStrOffsetWidth);
  inst |= imm12 << kStrOffsetShift;
  return inst;
}

// Creates a `str xA, [xB, #offset]` instruction.
inline uint32_t CreateStr64(uint32_t value_register,
                            uint32_t address_register,
                            uint32_t offset = 0) {
  uint32_t inst = kStr64Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kStr64OffsetScaleShift);
  uint32_t imm12 = offset >> kStr64OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kStrOffsetWidth);
  inst |= imm12 << kStrOffsetShift;
  return inst;
}

// Creates a `str sA, [xB, #offset]` instruction.
inline uint32_t CreateStrFP32(uint32_t value_register,
                              uint32_t address_register,
                              uint32_t offset = 0) {
  uint32_t inst = kStrFP32Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kStr32OffsetScaleShift);
  uint32_t imm12 = offset >> kStr32OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kStrOffsetWidth);
  inst |= imm12 << kStrOffsetShift;
  return inst;
}

// Creates a `str dA, [xB, #offset]` instruction.
inline uint32_t CreateStrFP64(uint32_t value_register,
                              uint32_t address_register,
                              uint32_t offset = 0) {
  uint32_t inst = kStrFP64Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStrAddressShift;
  DCHECK_ALIGNED(offset, 1u << kStr64OffsetScaleShift);
  uint32_t imm12 = offset >> kStr64OffsetScaleShift;
  DCHECK_LT(imm12, 1u << kStrOffsetWidth);
  inst |= imm12 << kStrOffsetShift;
  return inst;
}

// Creates a `stlrb wA, [xB]` instruction.
inline uint32_t CreateStlr8(uint32_t value_register, uint32_t address_register) {
  uint32_t inst = kStlr8Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStlrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStlrAddressShift;
  return inst;
}

// Creates a `stlrh wA, [xB]` instruction.
inline uint32_t CreateStlr16(uint32_t value_register, uint32_t address_register) {
  uint32_t inst = kStlr16Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStlrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStlrAddressShift;
  return inst;
}

// Creates a `stlr wA, [xB]` instruction.
inline uint32_t CreateStlr32(uint32_t value_register, uint32_t address_register) {
  uint32_t inst = kStlr32Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStlrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStlrAddressShift;
  return inst;
}

// Creates a `stlr xA, [xB]` instruction.
inline uint32_t CreateStlr64(uint32_t value_register, uint32_t address_register) {
  uint32_t inst = kStlr64Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kStlrValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kStlrAddressShift;
  return inst;
}

// Creates a `stur xA, [xB]` instruction.
inline uint32_t CreateStur64(uint32_t value_register, uint32_t address_register) {
  uint32_t inst = kStur64Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kSturValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kSturAddressShift;
  return inst;
}

// Creates a `stur xA, [xB]` instruction.
inline uint32_t CreateSturFP64(uint32_t value_register, uint32_t address_register) {
  uint32_t inst = kSturFP64Opcode;
  DCHECK_LT(value_register, 32u);
  inst |= value_register << kSturValueShift;
  DCHECK_LT(address_register, 32u);
  inst |= address_register << kSturAddressShift;
  return inst;
}

inline uint32_t CreateStr(PatchpointKind kind,
                          uint32_t value_register,
                          uint32_t address_register,
                          uint32_t offset = 0) {
  switch (kind) {
    case PatchpointKind::kStoreBoolean:
    case PatchpointKind::kStoreInt8:
      return CreateStr8(value_register, address_register, offset);
    case PatchpointKind::kStoreInt16:
      return CreateStr16(value_register, address_register, offset);
    case PatchpointKind::kStoreGcRoot:
    case PatchpointKind::kStoreInt32:
      return CreateStr32(value_register, address_register, offset);
    case PatchpointKind::kStoreInt64:
      return CreateStr64(value_register, address_register, offset);
    case PatchpointKind::kStoreFloat32:
      return CreateStrFP32(value_register, address_register, offset);
    case PatchpointKind::kStoreFloat64:
      return CreateStrFP64(value_register, address_register, offset);
    case PatchpointKind::kStoreReleaseBoolean:
    case PatchpointKind::kStoreReleaseInt8:
      DCHECK_EQ(offset, 0u);
      return CreateStlr8(value_register, address_register);
    case PatchpointKind::kStoreReleaseInt16:
      DCHECK_EQ(offset, 0u);
      return CreateStlr16(value_register, address_register);
    case PatchpointKind::kStoreReleaseGcRoot:
    case PatchpointKind::kStoreReleaseInt32:
      DCHECK_EQ(offset, 0u);
      return CreateStlr32(value_register, address_register);
    case PatchpointKind::kStoreReleaseInt64:
      DCHECK_EQ(offset, 0u);
      return CreateStlr64(value_register, address_register);
    case PatchpointKind::kNone:
    case PatchpointKind::kGCPointerAllocaMap:
    case PatchpointKind::kTryBoundaryStackReadClobber:
    case PatchpointKind::kImplicitSuspendCheck:
    case PatchpointKind::kReachabilityFence:
    case PatchpointKind::kCriticalNativeCall:
    case PatchpointKind::kLoadBoolean:
    case PatchpointKind::kLoadInt8:
    case PatchpointKind::kLoadInt16:
    case PatchpointKind::kLoadGcRoot:
    case PatchpointKind::kLoadInt32:
    case PatchpointKind::kLoadInt64:
    case PatchpointKind::kLoadFloat32:
    case PatchpointKind::kLoadFloat64:
    case PatchpointKind::kLoadAcquireGcRoot:
    case PatchpointKind::kLoadAcquireBoolean:
    case PatchpointKind::kLoadAcquireInt8:
    case PatchpointKind::kLoadAcquireInt16:
    case PatchpointKind::kLoadAcquireInt32:
    case PatchpointKind::kLoadAcquireInt64:
    case PatchpointKind::kDiscardedLoad:
    case PatchpointKind::kLoadClassBootImageLinkTimePcRelative:
    case PatchpointKind::kLoadClassBootImageRelRo:
    case PatchpointKind::kLoadClassBootImageIntrinsic:
    case PatchpointKind::kLoadClassAppImageRelRo:
    case PatchpointKind::kLoadClassBssEntry:
    case PatchpointKind::kLoadClassBssEntryPublic:
    case PatchpointKind::kLoadClassBssEntryPackage:
    case PatchpointKind::kLoadStringBootImageRelRo:
    case PatchpointKind::kLoadStringBootImageLinkTimePcRelative:
    case PatchpointKind::kLoadStringBssEntry:
    case PatchpointKind::kLoadMethodTypeBssEntry:
    case PatchpointKind::kLoadMethodBootImageRelRo:
    case PatchpointKind::kLoadMethodAppImageRelRo:
    case PatchpointKind::kLoadMethodBootImageLinkTimePcRelative:
    case PatchpointKind::kLoadMethodBootImageJni:
    case PatchpointKind::kLoadMethodBssEntry:
    case PatchpointKind::kEntrypointThunkCallStatepoint:
    case PatchpointKind::kEntrypointThunkCallPatchpoint:
    case PatchpointKind::kCatchBlock:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
  }
}

// Creates an `adrp xA, #0x0` instruction.
inline uint32_t CreateAdrp(uint32_t dest_register) {
  return kAdrpOpcode | (dest_register << kAdrpDestShift);
}

// Creates an `add wA, wB, #immediate` instruction.
inline uint32_t CreateAdd32Immediate(uint32_t result_register,
                                     uint32_t lhs_register,
                                     uint32_t immediate = 0) {
  uint32_t inst = kAdd32ImmediateOpcode;
  DCHECK_LT(result_register, 32u);
  inst |= result_register << kAddImmediateResultShift;
  DCHECK_LT(lhs_register, 32u);
  inst |= lhs_register << kAddImmediateLhsShift;

  // Encode the immediate.
  uint32_t low_mask = (1u << kAddImmediateImmediateWidth) - 1;
  uint32_t high_mask = low_mask << kAddImmediateImmediateWidth;
  if ((immediate & low_mask) == low_mask) {
    uint32_t imm12 = immediate;
    inst |= imm12 << kAddImmediateImmediateShift;
  } else {
    DCHECK_EQ(immediate, (immediate & high_mask)) << "Immediate value cannot be encoded in add";
    uint32_t imm12 = immediate >> kAddImmediateImmediateWidth;
    inst |= kAddImmediateShiftBit;
    inst |= imm12 << kAddImmediateImmediateShift;
  }

  return inst;
}

// Creates an `add xA, xB, #immediate` instruction.
inline uint32_t CreateAdd64Immediate(uint32_t result_register,
                                     uint32_t lhs_register,
                                     uint32_t immediate = 0) {
  uint32_t inst = kAdd64ImmediateOpcode;
  DCHECK_LT(result_register, 32u);
  inst |= result_register << kAddImmediateResultShift;
  DCHECK_LT(lhs_register, 32u);
  inst |= lhs_register << kAddImmediateLhsShift;

  // Encode the immediate.
  uint32_t low_mask = (1u << kAddImmediateImmediateWidth) - 1;
  uint32_t high_mask = low_mask << kAddImmediateImmediateWidth;
  if ((immediate & low_mask) == immediate) {
    uint32_t imm12 = immediate;
    inst |= imm12 << kAddImmediateImmediateShift;
  } else {
    DCHECK_EQ(immediate, (immediate & high_mask)) << "Immediate value cannot be encoded in add";
    uint32_t imm12 = immediate >> kAddImmediateImmediateWidth;
    inst |= kAddImmediateShiftBit;
    inst |= imm12 << kAddImmediateImmediateShift;
  }

  return inst;
}

// Creates a `sub wA, wB, #immediate` instruction.
inline uint32_t CreateSub32Immediate(uint32_t result_register,
                                     uint32_t lhs_register,
                                     uint32_t immediate = 0) {
  uint32_t inst = kSub32ImmediateOpcode;
  DCHECK_LT(result_register, 32u);
  inst |= result_register << kSubImmediateResultShift;
  DCHECK_LT(lhs_register, 32u);
  inst |= lhs_register << kSubImmediateLhsShift;

  // Encode the immediate.
  uint32_t low_mask = (1u << kSubImmediateImmediateWidth) - 1;
  uint32_t high_mask = low_mask << kSubImmediateImmediateWidth;
  if ((immediate & low_mask) == low_mask) {
    uint32_t imm12 = immediate;
    inst |= imm12 << kSubImmediateImmediateShift;
  } else {
    DCHECK_EQ(immediate, (immediate & high_mask)) << "Immediate value cannot be encoded in sub";
    uint32_t imm12 = immediate >> kSubImmediateImmediateWidth;
    inst |= kSubImmediateShiftBit;
    inst |= imm12 << kSubImmediateImmediateShift;
  }

  return inst;
}

// Creates a `sub xA, xB, #immediate` instruction.
inline uint32_t CreateSub64Immediate(uint32_t result_register,
                                     uint32_t lhs_register,
                                     uint32_t immediate = 0) {
  uint32_t inst = kSub64ImmediateOpcode;
  DCHECK_LT(result_register, 32u);
  inst |= result_register << kSubImmediateResultShift;
  DCHECK_LT(lhs_register, 32u);
  inst |= lhs_register << kSubImmediateLhsShift;

  // Encode the immediate.
  uint32_t low_mask = (1u << kSubImmediateImmediateWidth) - 1;
  uint32_t high_mask = low_mask << kSubImmediateImmediateWidth;
  if ((immediate & low_mask) == immediate) {
    uint32_t imm12 = immediate;
    inst |= imm12 << kSubImmediateImmediateShift;
  } else {
    DCHECK_EQ(immediate, (immediate & high_mask)) << "Immediate value cannot be encoded in sub";
    uint32_t imm12 = immediate >> kSubImmediateImmediateWidth;
    inst |= kSubImmediateShiftBit;
    inst |= imm12 << kSubImmediateImmediateShift;
  }

  return inst;
}

// Creates a `mov wA, wB` instruction.
inline uint32_t CreateMov32(uint32_t dest_register, uint32_t source_register) {
  uint32_t inst = kMov32Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kMovDestShift;
  DCHECK_LT(source_register, 32u);
  inst |= source_register << kMovSourceShift;
  return inst;
}

// Creates a `mov xA, xB` instruction.
inline uint32_t CreateMov64(uint32_t dest_register, uint32_t source_register) {
  uint32_t inst = kMov64Opcode;
  DCHECK_LT(dest_register, 32u);
  inst |= dest_register << kMovDestShift;
  DCHECK_LT(source_register, 32u);
  inst |= source_register << kMovSourceShift;
  return inst;
}

// Creates a `blr xA` instruction.
inline uint32_t CreateBlr(uint32_t callee_register) {
  uint32_t inst = kBlrOpcode;
  DCHECK_LT(callee_register, 32u);
  inst |= callee_register << kBlrCalleeShift;
  return inst;
}

// Creates a `bl #offset` instruction.
inline uint32_t CreateBl(int32_t offset) {
  uint32_t inst = kBlOpcode;
  DCHECK_ALIGNED(offset, 1 << kBlImmScaleShift) << "BL offset is not aligned to 4 bytes";
  offset /= 1 << kBlImmScaleShift;
  DCHECK_EQ(static_cast<uint32_t>(std::abs(offset)) & ~((uint32_t{1} << kBlImmWidth) - 1), 0u)
      << "BL offset cannot fit into 26 bits.";
  uint32_t imm26 = static_cast<uint32_t>(offset) & ((uint32_t{1} << kBlImmWidth) - 1);
  inst |= imm26;
  return inst;
}

}  // namespace arm64_llvm
}  // namespace art HIDDEN

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_LLVM_UTILS_H_
