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

#include "code_generator_arm64_llvm.h"

#include "aarch64/assembler-aarch64.h"
#include "aarch64/registers-aarch64.h"
#include "arch/arm64/instruction_set_features_arm64.h"
#include "base/bit_utils.h"
#include "class_root-inl.h"
#include "class_table.h"
#include "code_generator_arm64_llvm_utils.h"
#include "code_generator_utils.h"
#include "com_android_art_flags.h"
#include "elf_file_parser.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "gc/accounting/card_table.h"
#include "gc/space/image_space.h"
#include "heap_poisoning.h"
#include "interpreter/mterp/nterp.h"
#include "intrinsics.h"
#include "intrinsics_arm64_llvm.h"
#include "intrinsics_list.h"
#include "linker/linker_patch.h"
#include "llvm_optimization_pipeline_builder.h"
#include "lock_word.h"
#include "mirror/var_handle.h"
#include "offsets.h"
#include "optimizing/nodes.h"
#include "profiling_info_builder.h"
#include "stack_map_stream.h"
#include "string_builder_append.h"
#include "thread.h"
#include "trace.h"
#include "utils/arm64/assembler_arm64.h"
#include "utils/stack_checks.h"

// TODO(LLVM): Make LLVM compile with these warnings.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#pragma GCC diagnostic ignored "-Wused-but-marked-unused"
#pragma GCC diagnostic ignored "-Wdeprecated-copy-with-dtor"
#pragma GCC diagnostic ignored "-Wframe-larger-than"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugFrame.h"
#include "llvm/IR/GCStrategy.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#pragma GCC diagnostic pop

using namespace vixl::aarch64;  // NOLINT(build/namespaces)
namespace art_flags = com::android::art::flags;

#ifdef __
#error "ARM64LLVM Codegen IRBuilder macro already defined."
#endif

// TODO: remove this when not needed anymore
#define TODO()                                               \
  do {                                                       \
    LOG(FATAL) << "TODO: Implement " << __PRETTY_FUNCTION__; \
  } while (false)

// TODO: remove this when not needed anymore
#define UNUSED(x) ((void)(x))

namespace art HIDDEN {

namespace arm64_llvm {

static constexpr std::string_view kCastToUncompressedPlaceholderFunctionName =
    "__cast_to_uncompressed";
static constexpr std::string_view kCastToCompressedPlaceholderFunctionName = "__cast_to_compressed";
static constexpr std::string_view kCastPointerToIntPlaceholderFunctionName =
    "__cast_pointer_to_int";
static constexpr std::string_view kHeapReferencePoisoningPlaceholderFunctionName =
    "__heap_reference_poisoning";
static constexpr std::string_view kImplicitSuspendCheckPlaceholderFunctionName =
    "__implicit_suspend_check";
static constexpr std::string_view kSuspendCheckPlaceholderFunctionName = "__suspend_check";
static constexpr std::string_view kLoadGcRootPlaceholderFunctionName = "__load_gc_root";
static constexpr std::string_view kLoadBooleanPlaceholderFunctionName = "__load_i1";
static constexpr std::string_view kLoadInt8PlaceholderFunctionName = "__load_i8";
static constexpr std::string_view kLoadInt16PlaceholderFunctionName = "__load_i16";
static constexpr std::string_view kLoadInt32PlaceholderFunctionName = "__load_i32";
static constexpr std::string_view kLoadInt64PlaceholderFunctionName = "__load_i64";
static constexpr std::string_view kLoadFloat32PlaceholderFunctionName = "__load_f32";
static constexpr std::string_view kLoadFloat64PlaceholderFunctionName = "__load_f64";
static constexpr std::string_view kLoadAcquireGcRootPlaceholderFunctionName =
    "__load_acquire_gc_root";
static constexpr std::string_view kLoadAcquireBooleanPlaceholderFunctionName = "__load_acquire_i1";
static constexpr std::string_view kLoadAcquireInt8PlaceholderFunctionName = "__load_acquire_i8";
static constexpr std::string_view kLoadAcquireInt16PlaceholderFunctionName = "__load_acquire_i16";
static constexpr std::string_view kLoadAcquireInt32PlaceholderFunctionName = "__load_acquire_i32";
static constexpr std::string_view kLoadAcquireInt64PlaceholderFunctionName = "__load_acquire_i64";
static constexpr std::string_view kDiscardedLoadPlaceholderFunctionName = "__discarded_load";
static constexpr std::string_view kStoreGcRootPlaceholderFunctionName = "__store_gc_root";
static constexpr std::string_view kStoreBooleanPlaceholderFunctionName = "__store_i1";
static constexpr std::string_view kStoreInt8PlaceholderFunctionName = "__store_i8";
static constexpr std::string_view kStoreInt16PlaceholderFunctionName = "__store_i16";
static constexpr std::string_view kStoreInt32PlaceholderFunctionName = "__store_i32";
static constexpr std::string_view kStoreInt64PlaceholderFunctionName = "__store_i64";
static constexpr std::string_view kStoreFloat32PlaceholderFunctionName = "__store_f32";
static constexpr std::string_view kStoreFloat64PlaceholderFunctionName = "__store_f64";
static constexpr std::string_view kStoreReleaseGcRootPlaceholderFunctionName =
    "__store_release_gc_root";
static constexpr std::string_view kStoreReleaseBooleanPlaceholderFunctionName =
    "__store_release_i1";
static constexpr std::string_view kStoreReleaseInt8PlaceholderFunctionName = "__store_release_i8";
static constexpr std::string_view kStoreReleaseInt16PlaceholderFunctionName = "__store_release_i16";
static constexpr std::string_view kStoreReleaseInt32PlaceholderFunctionName = "__store_release_i32";
static constexpr std::string_view kStoreReleaseInt64PlaceholderFunctionName = "__store_release_i64";
static constexpr std::string_view kMemCpyI16PlaceholderFunctionName = "__memcpy_i16";
static constexpr std::string_view kMemCpyI32PlaceholderFunctionName = "__memcpy_i32";
static constexpr std::string_view kMemCpyI8ZextToI16PlaceholderFunctionName =
    "__memcpy_i8_zext_to_i16";
static constexpr std::string_view kStringEqualsPlaceholderFunctionName = "__string_equals";
static constexpr std::string_view kEntrypointThunkPlaceholderFunctionName = "__entrypoint_thunk";

static constexpr std::string_view kCatchBlockAddressesArrayName = "__catch_block_addresses";

// Very similar to `llvm::StatepointGC` in llvm/lib/IR/BuiltinGCs.cpp.
class ArtGCStrategy : public llvm::GCStrategy {
 public:
  ArtGCStrategy() {
    UseStatepoints = true;
    UseRS4GC = true;
    UseCompressedPointers = true;
  }

  std::optional<bool> isGCManagedPointer(const llvm::Type* type) const override {
    const llvm::PointerType* pointer_type = llvm::cast<llvm::PointerType>(type);
    unsigned address_space = pointer_type->getAddressSpace();
    return address_space == kUncompressedGCAddressSpace ||
           address_space == kCompressedGCPointerAddressSpace;
  }
};

// Add ART's own GC strategy to LLVM.
[[maybe_unused]] static llvm::GCRegistry::Add<ArtGCStrategy> art_gc_strategy_adder(
    kArtGCStrategyName, "ART garbage collector");

// Reference load (except object array loads) is using LDR Wt, [Xn, #offset] which can handle
// offset < 16KiB. For offsets >= 16KiB, the load shall be emitted as two or more instructions.
// For the Baker read barrier implementation using link-time generated thunks we need to split
// the offset explicitly.
// constexpr uint32_t kReferenceLoadMinFarOffset = 16 * KB;

// NOLINT on __ macro to suppress wrong warning/fix (misc-macro-parentheses) from clang-tidy.
#define __ codegen->GetIRBuilder()->  // NOLINT
// #define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kArm64PointerSize, x).Int32Value()

class BoundsCheckSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  explicit BoundsCheckSlowPathARM64LLVM(HBoundsCheck* instruction, llvm::BasicBlock* entry_block)
      : SlowPathCodeARM64LLVM(instruction, entry_block, nullptr) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    // __ Bind(GetEntryLabel());
    __ SetInsertPoint(GetEntryBlock());

    std::array<llvm::Value*, 3> arguments = {codegen->GetUndefCurrentMethodPointer(),
                                             codegen->GetValue(instruction_->InputAt(0)),
                                             codegen->GetValue(instruction_->InputAt(1))};
    QuickEntrypointEnum entrypoint = instruction_->AsBoundsCheck()->IsStringCharAt()
                                         ? kQuickThrowStringBounds
                                         : kQuickThrowArrayBounds;
    codegen->SetInvokeRuntimeParametersAndReturnType(
        arguments, codegen->GetVoidType(), llvm::CallingConv::ARTPreserveAll);
    codegen->InvokeRuntime(entrypoint, instruction_, this);
    CheckEntrypointTypes<kQuickThrowStringBounds, void, int32_t, int32_t>();
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
    // The runtime call throws, so we can't get to here.
    DCHECK(GetExitBlock() == nullptr);
    __ CreateUnreachable();
  }

  bool IsFatal() const override { return true; }

  const char* GetDescription() const override { return "BoundsCheckSlowPathARM64LLVM"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM64LLVM);
};

class DivZeroCheckSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  explicit DivZeroCheckSlowPathARM64LLVM(HDivZeroCheck* instruction, llvm::BasicBlock* entry_block)
      : SlowPathCodeARM64LLVM(instruction, entry_block, nullptr) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    // __ Bind(GetEntryLabel());
    __ SetInsertPoint(GetEntryBlock());

    codegen->SetInvokeRuntimeParametersAndReturnType({codegen->GetUndefCurrentMethodPointer()},
                                                     codegen->GetVoidType(),
                                                     llvm::CallingConv::ARTPreserveAll);
    codegen->InvokeRuntime(kQuickThrowDivZero, instruction_, this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
    // The runtime call throws, so we can't get to here.
    DCHECK(GetExitBlock() == nullptr);
    __ CreateUnreachable();
  }

  bool IsFatal() const override { return true; }

  const char* GetDescription() const override { return "DivZeroCheckSlowPathARM64LLVM"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathARM64LLVM);
};

class LoadClassSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  LoadClassSlowPathARM64LLVM(HLoadClass* cls,
                             HInstruction* at,
                             llvm::BasicBlock* entry_block,
                             llvm::BasicBlock* exit_block,
                             llvm::PHINode* result_phi,
                             llvm::Value* class_ptr)
      : SlowPathCodeARM64LLVM(at, entry_block, exit_block),
        cls_(cls),
        result_phi_(result_phi),
        class_ptr_(class_ptr) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
    DCHECK_EQ(instruction_->IsLoadClass(), cls_ == instruction_);
  }

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    bool must_resolve_type = instruction_->IsLoadClass() && cls_->MustResolveTypeOnSlowPath();
    bool must_do_clinit = instruction_->IsClinitCheck() || cls_->MustGenerateClinitCheck();

    // __ Bind(GetEntryLabel());
    __ SetInsertPoint(GetEntryBlock());

    llvm::Value* clinit_arg = nullptr;
    llvm::Value* clinit_result = nullptr;
    // InvokeRuntimeCallingConvention calling_convention;
    if (must_resolve_type) {
      DCHECK(IsSameDexFile(cls_->GetDexFile(), codegen->GetGraph()->GetDexFile()) ||
             codegen->GetCompilerOptions().WithinOatFile(&cls_->GetDexFile()) ||
             ContainsElement(Runtime::Current()->GetClassLinker()->GetBootClassPath(),
                             &cls_->GetDexFile()));
      dex::TypeIndex type_index = cls_->GetTypeIndex();
      llvm::Value* type_index_value =
          codegen->GetConstantInt(codegen->GetUint32Type(), type_index.index_);
      // __ Mov(calling_convention.GetRegisterAt(0).W(), type_index.index_);
      codegen->SetInvokeRuntimeParametersAndReturnType(
          {codegen->GetUndefCurrentMethodPointer(), type_index_value},
          codegen->GetUncompressedGCPointerType(),
          llvm::CallingConv::ARTPreserveAll);
      QuickEntrypointEnum entrypoint =
          cls_->NeedsAccessCheck() ? kQuickResolveTypeAndVerifyAccess : kQuickResolveType;
      DCHECK(instruction_ != nullptr);
      codegen->InvokeRuntime(entrypoint, instruction_, this);
      clinit_arg = codegen->GetInvokeRuntimeResult();
      if (cls_->NeedsAccessCheck()) {
        CheckEntrypointTypes<kQuickResolveTypeAndVerifyAccess, void*, uint32_t>();
      } else {
        CheckEntrypointTypes<kQuickResolveType, void*, uint32_t>();
      }
      // If we also must_do_clinit, the resolved type is now in the correct register.
    } else {
      DCHECK(must_do_clinit);
      clinit_arg =
          instruction_->IsLoadClass() ? class_ptr_ : codegen->GetValue(instruction_->InputAt(0));
    }
    if (must_do_clinit) {
      codegen->SetInvokeRuntimeParametersAndReturnType(
          {codegen->GetUndefCurrentMethodPointer(), clinit_arg},
          codegen->GetUncompressedGCPointerType(),
          llvm::CallingConv::ARTPreserveAll);
      codegen->InvokeRuntime(kQuickInitializeStaticStorage, instruction_, this);
      clinit_result = codegen->GetInvokeRuntimeResult();
      CheckEntrypointTypes<kQuickInitializeStaticStorage, void*, mirror::Class*>();
    }

    // Move the class to the desired location.
    llvm::Value* result = clinit_result != nullptr ? clinit_result : clinit_arg;
    // __ B(GetExitLabel());
    __ CreateBr(GetExitBlock());
    result_phi_->addIncoming(result, __ GetInsertBlock());
  }

  const char* GetDescription() const override { return "LoadClassSlowPathARM64LLVM"; }

 private:
  // The class this slow path will load.
  HLoadClass* const cls_;
  llvm::PHINode* result_phi_;
  llvm::Value* class_ptr_;

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathARM64LLVM);
};

class LoadStringSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  explicit LoadStringSlowPathARM64LLVM(HLoadString* instruction,
                                       llvm::BasicBlock* entry_block,
                                       llvm::BasicBlock* exit_block,
                                       llvm::PHINode* result_phi)
      : SlowPathCodeARM64LLVM(instruction, entry_block, exit_block), result_phi_(result_phi) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    // __ Bind(GetEntryLabel());
    __ SetInsertPoint(GetEntryBlock());

    // InvokeRuntimeCallingConvention calling_convention;
    const dex::StringIndex string_index = instruction_->AsLoadString()->GetStringIndex();
    // __ Mov(calling_convention.GetRegisterAt(0).W(), string_index.index_);
    llvm::Value* string_index_value =
        codegen->GetConstantInt(codegen->GetUint32Type(), string_index.index_);
    DCHECK_EQ(instruction_->GetType(), DataType::Type::kReference);
    codegen->SetInvokeRuntimeParametersAndReturnType(
        {codegen->GetUndefCurrentMethodPointer(), string_index_value},
        codegen->GetUncompressedGCPointerType(),
        llvm::CallingConv::ARTPreserveAll);
    codegen->InvokeRuntime(kQuickResolveString, instruction_, this);
    CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
    llvm::Value* resolved_string = codegen->GetInvokeRuntimeResult();

    // __ B(GetExitLabel());
    __ CreateBr(GetExitBlock());
    // Add the resolved string reference to the result phi node.
    result_phi_->addIncoming(resolved_string, __ GetInsertBlock());
  }

  const char* GetDescription() const override { return "LoadStringSlowPathARM64LLVM"; }

 private:
  llvm::PHINode* result_phi_;

  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathARM64LLVM);
};

class LoadMethodTypeSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  explicit LoadMethodTypeSlowPathARM64LLVM(HLoadMethodType* instruction,
                                           llvm::BasicBlock* entry_block,
                                           llvm::BasicBlock* exit_block,
                                           llvm::PHINode* result_phi)
      : SlowPathCodeARM64LLVM(instruction, entry_block, exit_block), result_phi_(result_phi) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    // __ Bind(GetEntryLabel());
    __ SetInsertPoint(GetEntryBlock());

    // InvokeRuntimeCallingConvention calling_convention;
    const dex::ProtoIndex proto_index = instruction_->AsLoadMethodType()->GetProtoIndex();
    // __ Mov(calling_convention.GetRegisterAt(0).W(), proto_index.index_);
    llvm::Value* proto_index_value =
        codegen->GetConstantInt(codegen->GetUint32Type(), proto_index.index_);
    DCHECK_EQ(instruction_->GetType(), DataType::Type::kReference);
    codegen->SetInvokeRuntimeParametersAndReturnType(
        {codegen->GetUndefCurrentMethodPointer(), proto_index_value},
        codegen->GetUncompressedGCPointerType(),
        llvm::CallingConv::ARTPreserveAll);
    codegen->InvokeRuntime(kQuickResolveMethodType, instruction_, this);
    CheckEntrypointTypes<kQuickResolveMethodType, void*, uint32_t>();
    llvm::Value* resolved_method_type = codegen->GetInvokeRuntimeResult();

    // __ B(GetExitLabel());
    __ CreateBr(GetExitBlock());
    // Add the resolved method type to the result phi node.
    result_phi_->addIncoming(resolved_method_type, __ GetInsertBlock());
  }

  const char* GetDescription() const override { return "LoadMethodTypeSlowPathARM64LLVM"; }

 private:
  llvm::PHINode* result_phi_;

  DISALLOW_COPY_AND_ASSIGN(LoadMethodTypeSlowPathARM64LLVM);
};

class NullCheckSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  explicit NullCheckSlowPathARM64LLVM(HNullCheck* instr, llvm::BasicBlock* entry_block)
      : SlowPathCodeARM64LLVM(instr, entry_block, nullptr) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    // __ Bind(GetEntryLabel());
    __ SetInsertPoint(GetEntryBlock());

    codegen->SetInvokeRuntimeParametersAndReturnType(
        {}, codegen->GetVoidType(), llvm::CallingConv::ARTPreserveAll);
    codegen->InvokeRuntime(kQuickThrowNullPointer, instruction_, this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
    __ CreateUnreachable();
  }

  bool IsFatal() const override { return true; }

  const char* GetDescription() const override { return "NullCheckSlowPathARM64LLVM"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathARM64LLVM);
};

class SuspendCheckSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  SuspendCheckSlowPathARM64LLVM(HSuspendCheck* instruction,
                                llvm::BasicBlock* entry_block,
                                llvm::BasicBlock* exit_block,
                                uint64_t statepoint_id)
      : SlowPathCodeARM64LLVM(instruction, entry_block, exit_block),
        statepoint_id_(statepoint_id) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    // __ Bind(GetEntryLabel());
    DCHECK_EQ(GetEntryBlock()->size(), 1u);
    DCHECK(GetEntryBlock()->getTerminator() != nullptr);
    __ SetInsertPoint(GetEntryBlock()->getFirstInsertionPt());
    codegen->SetInvokeRuntimeParametersAndReturnType({codegen->GetUndefCurrentMethodPointer()},
                                                     codegen->GetVoidType(),
                                                     llvm::CallingConv::ARTPreserveAll,
                                                     statepoint_id_);
    codegen->InvokeRuntime(kQuickTestSuspend, instruction_, this);
    CheckEntrypointTypes<kQuickTestSuspend, void, void>();
    // The branch has already been created.
    // __ CreateBr(GetExitBlock());
  }

  const char* GetDescription() const override { return "SuspendCheckSlowPathARM64LLVM"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathARM64LLVM);

  uint64_t statepoint_id_;
};

class TypeCheckSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  TypeCheckSlowPathARM64LLVM(HInstruction* instruction,
                             llvm::BasicBlock* entry_block,
                             llvm::BasicBlock* exit_block,
                             llvm::PHINode* result_phi,
                             bool is_fatal)
      : SlowPathCodeARM64LLVM(instruction, entry_block, exit_block),
        result_phi_(result_phi),
        is_fatal_(is_fatal) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    // __ Bind(GetEntryLabel());
    __ SetInsertPoint(GetEntryBlock());

    llvm::SmallVector<llvm::Value*> runtime_call_arguments;
    runtime_call_arguments.push_back(codegen->GetUndefCurrentMethodPointer());
    runtime_call_arguments.push_back(codegen->GetValue(instruction_->InputAt(0)));
    runtime_call_arguments.push_back(codegen->GetValue(instruction_->InputAt(1)));
    if (instruction_->IsInstanceOf()) {
      DCHECK(result_phi_ != nullptr);
      codegen->SetInvokeRuntimeParametersAndReturnType(runtime_call_arguments,
                                                       codegen->GetUint64Type());
      codegen->InvokeRuntime(kQuickInstanceofNonTrivial, instruction_, this);
      llvm::Value* result = codegen->GetInvokeRuntimeResult();
      CheckEntrypointTypes<kQuickInstanceofNonTrivial, size_t, mirror::Object*, mirror::Class*>();
      DCHECK_EQ(instruction_->GetType(), DataType::Type::kBool)
          << "InstanceOf instruction should have boolean type";
      llvm::Value* result_bool =
          __ CreateICmpNE(result, codegen->GetConstantZero(result->getType()));
      result_phi_->addIncoming(result_bool, __ GetInsertBlock());
    } else {
      DCHECK(instruction_->IsCheckCast());
      DCHECK(result_phi_ == nullptr);
      codegen->SetInvokeRuntimeParametersAndReturnType(runtime_call_arguments,
                                                       codegen->GetVoidType());
      codegen->InvokeRuntime(kQuickCheckInstanceOf, instruction_, this);
      CheckEntrypointTypes<kQuickCheckInstanceOf, void, mirror::Object*, mirror::Class*>();
    }

    if (is_fatal_) {
      __ CreateUnreachable();
    } else {
      // __ B(GetExitLabel());
      __ CreateBr(GetExitBlock());
    }
  }

  const char* GetDescription() const override { return "TypeCheckSlowPathARM64LLVM"; }
  bool IsFatal() const override { return is_fatal_; }

 private:
  llvm::PHINode* result_phi_;
  const bool is_fatal_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathARM64LLVM);
};

class DeoptimizationSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  explicit DeoptimizationSlowPathARM64LLVM(HDeoptimize* instruction, llvm::BasicBlock* entry_block)
      : SlowPathCodeARM64LLVM(instruction, entry_block, nullptr) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    // __ Bind(GetEntryLabel());
    __ SetInsertPoint(GetEntryBlock());
    uint32_t deoptimization_kind =
        static_cast<uint32_t>(instruction_->AsDeoptimize()->GetDeoptimizationKind());
    llvm::Value* deoptimization_kind_value =
        codegen->GetConstantInt(codegen->GetUint32Type(), deoptimization_kind);
    codegen->SetInvokeRuntimeParametersAndReturnType(
        {codegen->GetUndefCurrentMethodPointer(), deoptimization_kind_value},
        codegen->GetVoidType(),
        llvm::CallingConv::ARTPreserveAll);
    codegen->InvokeRuntime(kQuickDeoptimize, instruction_, this);
    CheckEntrypointTypes<kQuickDeoptimize, void, DeoptimizationKind>();
    __ CreateUnreachable();
  }

  const char* GetDescription() const override { return "DeoptimizationSlowPathARM64LLVM"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathARM64LLVM);
};

class ArraySetSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  explicit ArraySetSlowPathARM64LLVM(HInstruction* instruction,
                                     llvm::BasicBlock* entry_block,
                                     llvm::BasicBlock* exit_block)
      : SlowPathCodeARM64LLVM(instruction, entry_block, exit_block) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen->SetCurrentBlock(instruction_->GetBlock());

    // __ Bind(GetEntryLabel());
    __ SetInsertPoint(GetEntryBlock());

    std::array<llvm::Value*, 4> arguments = {
        codegen->GetUndefCurrentMethodPointer(),
        codegen->GetValue(instruction_->InputAt(0)),
        codegen->GetValue(instruction_->InputAt(1)),
        codegen->GetValue(instruction_->InputAt(2)),
    };
    codegen->SetInvokeRuntimeParametersAndReturnType(arguments, codegen->GetVoidType());
    codegen->InvokeRuntime(kQuickAputObject, instruction_, this);
    CheckEntrypointTypes<kQuickAputObject, void, mirror::Array*, int32_t, mirror::Object*>();
    // __ B(GetExitLabel());
    __ CreateBr(GetExitBlock());
  }

  const char* GetDescription() const override { return "ArraySetSlowPathARM64LLVM"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(ArraySetSlowPathARM64LLVM);
};

#undef __

namespace detail {

// Mark which intrinsics we don't have handcrafted code for.
template <Intrinsics T>
struct IsUnimplemented {
  bool is_unimplemented = false;
};

#define TRUE_OVERRIDE(Name)                     \
  template <>                                   \
  struct IsUnimplemented<Intrinsics::k##Name> { \
    bool is_unimplemented = true;               \
  };
UNIMPLEMENTED_INTRINSIC_LIST_ARM64LLVM(TRUE_OVERRIDE)
#undef TRUE_OVERRIDE

static constexpr bool kIsIntrinsicUnimplemented[] = {false,  // kNone
#define IS_UNIMPLEMENTED(Intrinsic, ...) \
  IsUnimplemented<Intrinsics::k##Intrinsic>().is_unimplemented,
                                                     ART_INTRINSICS_LIST(IS_UNIMPLEMENTED)
#undef IS_UNIMPLEMENTED
};

}  // namespace detail

static llvm::SmallVector<llvm::Type*> GetParameterTypes(
    llvm::ArrayRef<DataType::Type> parameter_types, const LLVMTypes& llvm_types) {
  llvm::SmallVector<llvm::Type*> llvm_parameter_types;
  llvm_parameter_types.reserve(parameter_types.size() + 2);

  // The first parameter (passed in x0) is the current method pointer.
  static_assert(kCurrentMethodIndex == 0);
  llvm_parameter_types.push_back(llvm_types.method_ptr);

  // The second parameter (passed on the stack with offset 0) is the caller method pointer.
  static_assert(kCallerMethodIndex == 1);
  llvm_parameter_types.push_back(llvm_types.method_ptr);

  // The first entry in shorty is the return type, which we skip over.
  for (DataType::Type parameter_type : parameter_types) {
    DCHECK(parameter_type != DataType::Type::kVoid) << "Invalid parameter type 'void'.";
    llvm_parameter_types.push_back(GetLLVMType(parameter_type, llvm_types));
  }

  return llvm_parameter_types;
}

// Returns a list containing the return type at index 0,
// and the parameter types in the rest of the list.
static llvm::SmallVector<DataType::Type> GetMethodReturnTypeAndParameterTypes(
    ArrayRef<HBasicBlock* const> blocks) {
  llvm::SmallVector<DataType::Type> result;
  result.push_back(DataType::Type::kVoid);  // Placeholder for the return type.
  if (blocks.empty()) {
    return result;
  }

  // Collect parameter types from the entry block.
  HBasicBlock* entry_block = blocks[0];
  // Add parameter types.
  for (HInstructionIterator it(entry_block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* instruction = it.Current();
    if (instruction->IsParameterValue()) {
      result.push_back(instruction->GetType());
    }
  }

  for (HBasicBlock* block : blocks) {
    if (block->GetInstructions().IsEmpty()) {
      continue;
    }

    if (block->GetLastInstruction()->IsReturnVoid()) {
      // The function must return void everywhere else, so we are done.
      result[0] = DataType::Type::kVoid;
      break;
    }

    HReturn* return_inst = block->GetLastInstruction()->AsReturnOrNull();
    if (return_inst == nullptr) {
      continue;
    }

    DataType::Type return_type = return_inst->InputAt(0)->GetType();
    switch (return_type) {
      case DataType::Type::kReference:
      case DataType::Type::kUint32:
      case DataType::Type::kInt32:
      case DataType::Type::kUint64:
      case DataType::Type::kInt64:
      case DataType::Type::kFloat32:
      case DataType::Type::kFloat64:
      case DataType::Type::kVoid:
        // Nothing to do, the type doesn't need promotion.
        break;
      case DataType::Type::kBool:
      case DataType::Type::kUint8:
      case DataType::Type::kInt8:
      case DataType::Type::kUint16:
      case DataType::Type::kInt16:
        // Promote the return type to at least Int32. This should be able to handle every value this
        // function may return.
        return_type = DataType::Type::kInt32;
        break;
    }
    result[0] = return_type;
    break;
  }

  return result;
}

// Initializes LLVM components once in a thread-safe way.
static void InitializeLLVM(const std::vector<std::string>& extra_flags) {
  // In C++, static variable initialization is thread-safe.
  [[maybe_unused]] static int dummy = [&]() -> int {
    llvm::InitializeAllDisassemblers();
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    std::vector<const char*> args = {
        "",
        // The maximum number of callee saved registers is 24 (x0-x15, x22-x29), which is only true
        // for a few calls, e.g. suspend checks. Otherwise there are only 8 callee saved registers
        // (x22-x29), which can be used to save heap references. The backend will try to keep some
        // number of gc pointers in registers, which is specified below, and explicitly write the
        // rest to the stack. The pointers that are kept in registers will be handled by the
        // register allocator, which will spill them to the stack if there aren't enough free callee
        // saved registers. An important difference between the two cases where the pointers are
        // saved to the stack, is that the statepoint lowering code can use 4 byte stack slots for
        // compressed pointers, while the register allocator will always use 8 byte spill slots.
        // This means that there can be a significant increase in a function's frame size if we
        // solely rely on the register allocator to spill values to the stack, meaning that we
        // shouldn't specify a large number here. Therefore, we choose 24, the maximum number of
        // callee saved registers we can ever use.
        "--max-registers-for-gc-values=24",
        "--use-registers-for-deopt-values",
        "--fixup-allow-gcptr-in-csr",
        "--simplifycfg-branch-fold-threshold=1",
        "--add-win-eh-prepare-pass",
        // Liveout generation for patchpoints can crash in some cases, so we have to disable it.
        "--enable-patchpoint-liveness=false",
        "--enable-machine-outliner=never",
        "--enable-split-machine-functions=false",
        "--regalloc-consider-statepoint-defs",
    };
    // Add extra flags from compiler options.
    args.reserve(args.size() + extra_flags.size());
    for (const std::string& arg : extra_flags) {
      args.push_back(arg.c_str());
    }
    bool success = llvm::cl::ParseCommandLineOptions(args.size(), args.data());
    DCHECK(success);

    return 0;
  }();
}

CodeGeneratorARM64LLVM::CodeGeneratorARM64LLVM(HGraph* graph,
                                               const CompilerOptions& compiler_options,
                                               OptimizingCompilerStats* stats)
    : CodeGenerator(graph,
                    kNumberOfAllocatableRegisters,
                    kNumberOfAllocatableFPRegisters,
                    kNumberOfAllocatableRegisterPairs,
                    callee_saved_core_registers.GetList(),
                    callee_saved_fp_registers.GetList(),
                    compiler_options,
                    stats,
                    ArrayRef<const bool>(detail::kIsIntrinsicUnimplemented)),
      llvm_context_(),
      ir_builder_(llvm_context_),
      parameter_stack_offsets_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      block_infos_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      all_stack_saved_values_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      instruction_value_map_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      instruction_alloca_map_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      phis_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      stack_saved_value_phis_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      try_boundary_catchswitch_map_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      catch_block_addresses_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      parametervalue_index_map_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      stack_map_infos_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      instruction_visitor_(graph, this, &ir_builder_),
      code_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      cfi_data_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_method_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      app_image_method_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      method_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_type_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      app_image_type_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      public_type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      package_type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_string_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      string_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      method_type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_jni_entrypoint_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_other_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      call_entrypoint_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      baker_read_barrier_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)) {
  LOG(DEBUG) << fmt::format("Compiling method '{}'", graph->PrettyMethod());

  CHECK(!GetGraph()->IsCompilingOsr()) << "OSR compilation is not handled by LLVM.";

  CHECK(!compiler_options.EmitReadBarrier())
      << "The LLVM code generator can't be used with read barriers enabled.";

  InitializeLLVM(compiler_options.LLVMArgs());

  constexpr std::string_view kDefaultTargetTriple = "aarch64-unknown-linux-android31";
  std::string target_triple = llvm::Triple::normalize(kDefaultTargetTriple);

  std::string target_error;
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(target_triple, target_error);
  if (target == nullptr) {
    LOG(FATAL) << "Unable to find target '" << kDefaultTargetTriple
               << "' in LLVM target registry: " << target_error;
  }

  // On target llvm::sys::getHostCPUName() generally returns the first cluster from a heterogeneous
  // architecture which tends to be the little cores
  // On host if the returned CPU is not AArch64 we fallback to generic later
  const std::string& cpu_target = GetCompilerOptions().LLVMCPUTarget();
  std::string cpu = cpu_target.empty() ? llvm::sys::getHostCPUName().str() : cpu_target;

  // TODO: We can get some extra features from the compiler options.
  std::string features = "+reserve-x" + std::to_string(tr.GetCode());
  if (kReserveMarkingRegister) {
    features += ",+reserve-x" + std::to_string(mr.GetCode());
  }
  features += ",+reserve-x" + std::to_string(kImplicitSuspendCheckRegister.GetCode());
  // FIXME: SVE seems to cause issues in the backend:
  //   * Register allocation fails for some anyregcc patchpoints. This seems to be caused by LLVM
  //     saving vector registers before a patchpoint.
  //   * @llvm.sponentry() doesn't seem to work when using scalable vectors.
  features += ",-sve";
  // In case we don't have a CPU known by LLVM, add the target features supplied on the command line
  // to the features string.
  if (cpu == "generic") {
    const Arm64InstructionSetFeatures& arm64_features =
        *GetCompilerOptions().GetInstructionSetFeatures()->AsArm64InstructionSetFeatures();
    if (arm64_features.NeedFixCortexA53_835769()) {
      features += ",+fix-cortex-a53-835769";
    }
    if (arm64_features.HasCRC()) {
      features += ",+crc";
    }
    if (arm64_features.HasLSE()) {
      features += ",+lse";
    }
    if (arm64_features.HasFP16()) {
      features += ",+fullfp16";
    }
    if (arm64_features.HasDotProd()) {
      features += ",+dotprod";
    }
  }

  std::unique_ptr<llvm::MCSubtargetInfo> mcsubtarget_info(
      target->createMCSubtargetInfo(target_triple, cpu, features));
  if (mcsubtarget_info == nullptr) {
    LOG(FATAL) << "Unable to create MCSubtargetInfo with the following target triple '"
               << target_triple << "' cpu '" << cpu << "' and featrues " << features;
  }
  if (!mcsubtarget_info->isCPUStringValid(cpu)) {
    LOG(WARNING) << "The provided cpu target '" << cpu
                 << "' is invalid, falling back to CPU 'generic'";
    cpu = "generic";
  }

  LOG(DEBUG) << "Using target CPU '" << cpu << "' for LLVM code generation";

  llvm::TargetOptions target_options;
  target_options.EmitStackSizeSection = true;
  llvm::CodeGenOptLevel opt_level;
  switch (GetOptimizationLevel().getSpeedupLevel()) {
    case 0:
      opt_level = llvm::CodeGenOptLevel::Default;
      break;
    case 1:
      opt_level = llvm::CodeGenOptLevel::Default;
      break;
    case 2:
      opt_level = llvm::CodeGenOptLevel::Default;
      break;
    case 3:
      opt_level = llvm::CodeGenOptLevel::Aggressive;
      break;
    default:
      opt_level = llvm::CodeGenOptLevel::Default;
      break;
  }
  target_machine_.reset(target->createTargetMachine(
      target_triple,
      cpu,
      features,
      target_options,
      llvm::Reloc::PIC_,
      // In a tiny code model loads from the literal pool use a single load with imm19 offset
      // instead of an adrp + ldr pair with an up to 32-bit offset.
      llvm::CodeModel::Tiny,
      opt_level));
  CHECK(target_machine_ != nullptr);

  llvm::DataLayout data_layout = target_machine_->createDataLayout();
  // This is the only way I found for specifying a different pointer size for an address space.
  // Pointer data layout format:
  // "p<address-space>:<mem-size>:<abi-align>[:<pref-align>[:<index-size>]]"
  static_assert(
      sizeof(mirror::CompressedReference<mirror::Object>) == sizeof(GcRoot<mirror::Object>),
      "art::mirror::CompressedReference<mirror::Object> and art::GcRoot<mirror::Object> have "
      "different sizes.");
  static_assert(
      sizeof(mirror::CompressedReference<mirror::Object>) == sizeof(int32_t),
      "art::mirror::CompressedReference<mirror::Object> and int32_t have different sizes.");
  static_assert(
      alignof(mirror::CompressedReference<mirror::Object>) == alignof(int32_t),
      "art::mirror::CompressedReference<mirror::Object> and int32_t have different alignments.");
  static_assert(sizeof(int32_t) == 4, "Sanity check: int32_t must be 4 bytes.");
  static_assert(alignof(int32_t) == 4, "Sanity check: int32_t must be aligned to 4 bytes.");
  std::string compressed_pointer_data_layout_string = data_layout.getStringRepresentation();
  // Compressed references have a size and alignment of 32 bits.
  compressed_pointer_data_layout_string +=
      fmt::format("-p{}:32:32", kCompressedGCPointerAddressSpace);
  // TODO: Do we need to do this? Investigate!
  // Mark the compressed reference address space as a non-integral pointer.
  compressed_pointer_data_layout_string +=
      fmt::format("-ni:{}:{}", kUncompressedGCAddressSpace, kCompressedGCPointerAddressSpace);
  llvm::Error error =
      llvm::DataLayout::parse(compressed_pointer_data_layout_string).moveInto(data_layout);
  CHECK(!error);
  DCHECK_EQ(data_layout.getPointerSize(0), static_cast<size_t>(kArm64PointerSize));
  DCHECK_EQ(data_layout.getPointerSize(kCompressedGCPointerAddressSpace), 4u);

  // Primitive types.
  llvm_types_ = GetLLVMTypes(llvm_context_);

  const std::string method_name = graph->PrettyMethod();
  module_ = std::make_unique<llvm::Module>(method_name, llvm_context_);
  module_->setDataLayout(data_layout);
  module_->setTargetTriple(target_triple);
}

llvm::OptimizationLevel CodeGeneratorARM64LLVM::GetOptimizationLevel() const {
  switch (GetCompilerOptions().LLVMOptLevel()) {
    case '0':
      return llvm::OptimizationLevel::O0;
    case '1':
      return llvm::OptimizationLevel::O1;
    case '2':
      return llvm::OptimizationLevel::O2;
    case '3':
      return llvm::OptimizationLevel::O3;
    case 's':
      return llvm::OptimizationLevel::Os;
    case 'z':
      return llvm::OptimizationLevel::Oz;
    default:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
  }
}

bool CodeGeneratorARM64LLVM::CPUHasFeature(std::string_view feature) const {
  CHECK(target_machine_ != nullptr);
  const auto& allfeatures = target_machine_->getMCSubtargetInfo()->getAllProcessorFeatures();
  const auto it = std::find_if(
      allfeatures.begin(), allfeatures.end(), [&](const auto& kv) { return kv.Key == feature; });

  return it == allfeatures.end() ? false
                                 : target_machine_->getMCSubtargetInfo()->hasFeature(it->Value);
}

bool CodeGeneratorARM64LLVM::CPUHasCRC() const { return CPUHasFeature("crc"); }

bool CodeGeneratorARM64LLVM::CPUHasFP16() const { return CPUHasFeature("fullfp16"); }

void CodeGeneratorARM64LLVM::CalculateParameterStackOffsets(
    llvm::ArrayRef<DataType::Type> parameter_types) {
  auto offset_from_index = [](uint32_t slot_index) -> uint32_t {
    return static_cast<uint32_t>(kArm64PointerSize) + slot_index * kVRegSize;
  };

  // The first two parameters on the LLVM function are method pointers.
  for (size_t i = 0; i < kParametersBeginIndex; ++i) {
    parameter_stack_offsets_.push_back(
        StackOffsetInfo{.offset = uint32_t(-1), .is_passed_on_stack = false, .is_used = false});
  }

  uint32_t slot_index = 0;
  uint32_t gprs_used = 0;  // General purpose registers.
  uint32_t fprs_used = 0;  // Floating-point and vector registers.

  constexpr uint32_t max_gpr_count = 7;  // x1-x7
  constexpr uint32_t max_fpr_count = 8;  // d0-d7

  for (DataType::Type parameter_type : parameter_types) {
    DCHECK(parameter_type != DataType::Type::kVoid) << "Invalid parameter type 'void'.";
    bool is_passed_on_stack = false;
    if (DataType::IsFloatingPointType(parameter_type) && fprs_used < max_fpr_count) {
      fprs_used += 1;
    } else if (!DataType::IsFloatingPointType(parameter_type) && gprs_used < max_gpr_count) {
      gprs_used += 1;
    } else {
      is_passed_on_stack = true;
    }

    parameter_stack_offsets_.push_back(
        StackOffsetInfo{offset_from_index(slot_index), is_passed_on_stack, .is_used = false});
    slot_index += DataType::Is64BitType(parameter_type) ? 2 : 1;
  }
}

void CodeGeneratorARM64LLVM::InitializeMetadata() {
  // Initialize register metadata.
  {
    auto create_register_metadata = [&](uint32_t reg) -> llvm::Value* {
      std::string register_name = reg == kSpRegisterNumber ? "sp" : fmt::format("x{}", reg);
      llvm::Metadata* metadata =
          llvm::MDNode::get(llvm_context_, llvm::MDString::get(llvm_context_, register_name));
      return llvm::MetadataAsValue::get(llvm_context_, metadata);
    };
    metadata_.thread_register = create_register_metadata(tr.GetCode());
    metadata_.marking_register = create_register_metadata(mr.GetCode());
    metadata_.implicit_suspend_check_register =
        create_register_metadata(kImplicitSuspendCheckRegister.GetCode());
    metadata_.stack_pointer_register = create_register_metadata(kSpRegisterNumber);
  }

  // Initialize memory scopes.
  {
    auto create_scope = [&]() -> llvm::Metadata* {
      // !0 = !{!0}
      llvm::MDTuple* domain = llvm::MDNode::get(GetLLVMContext(), {nullptr});
      domain->replaceOperandWith(0, domain);

      // !1 = !{!1, !0}
      llvm::MDTuple* result = llvm::MDNode::get(GetLLVMContext(), {nullptr, domain});
      result->replaceOperandWith(0, result);
      return result;
    };

    auto create_scope_list = [&](llvm::ArrayRef<llvm::Metadata*> scopes) -> llvm::MDNode* {
      return llvm::MDNode::get(GetLLVMContext(), scopes);
    };

    llvm::Metadata* stack_scope = create_scope();
    llvm::Metadata* heap_scope = create_scope();
    llvm::Metadata* runtime_scope = create_scope();
    llvm::Metadata* thread_object_scope = create_scope();

    metadata_.stack_scope = create_scope_list({stack_scope});
    metadata_.heap_scope = create_scope_list({heap_scope});
    metadata_.runtime_scope = create_scope_list({runtime_scope});
    metadata_.thread_object_scope = create_scope_list({thread_object_scope});

    // The three scopes don't alias each other.
    metadata_.stack_noalias = create_scope_list({heap_scope, runtime_scope, thread_object_scope});
    metadata_.heap_noalias = create_scope_list({stack_scope, runtime_scope, thread_object_scope});
    metadata_.runtime_noalias = create_scope_list({stack_scope, heap_scope, thread_object_scope});
    metadata_.thread_object_noalias = create_scope_list({stack_scope, heap_scope, runtime_scope});
    // Function calls can't write the caller's stack.
    metadata_.call_noalias = create_scope_list({stack_scope});
  }
}

// Creates placeholder functions in the module for operations that need stack map entries, such as
// implicit null checks on object references. These functions should be removed before code
// generation happens.
void CodeGeneratorARM64LLVM::InitializePlaceholderFunctions() {
  llvm::MDNode* empty_node = llvm::MDTuple::get(GetLLVMContext(), {});

  auto make_function = [&](std::string_view function_name,
                           llvm::Type* return_type,
                           llvm::ArrayRef<llvm::Type*> parameter_types,
                           bool needs_definition = false) -> llvm::Function* {
    llvm::FunctionType* function_type =
        llvm::FunctionType::get(return_type, parameter_types, false);
    llvm::Function* result = llvm::Function::Create(
        function_type, llvm::Function::LinkageTypes::ExternalLinkage, function_name, *module_);
    result->setMetadata(kPlaceholderFunctionMetadata, empty_node);

    if (!needs_definition) {
      return result;
    }

    // Add a simple body to the function, so it doesn't get removed from the module by DCE.
    // Also add noinline attribute to prevent the optimizer from inlining it.
    result->addFnAttr(llvm::Attribute::AttrKind::NoInline);
    llvm::BasicBlock* entry_block = llvm::BasicBlock::Create(GetLLVMContext(), "", result);
    llvm::IRBuilder<>& builder = *GetIRBuilder();
    builder.SetInsertPoint(entry_block);
    // Create a patchpoint call that behaves as a black box to LLVM, to prevent it from inferring
    // invalid function attributes. Function attributes are added manually to each patchpoint kind.
    // TODO: Could it be worth it to write an actual body for these functions and let LLVM infer
    // their attributes?
    llvm::SmallVector<llvm::Value*> patchpoint_arguments;
    patchpoint_arguments.reserve(4 + result->arg_size());
    // An invalid ID, which can be caught at stack map generation if it remains in the final IR.
    uint64_t id = 0xAAAAAAAA;
    patchpoint_arguments.push_back(GetConstantInt(GetUint64Type(), id));
    patchpoint_arguments.push_back(GetConstantInt(GetUint32Type(), kInstructionSize));
    patchpoint_arguments.push_back(GetConstantZero(GetPointerType()));
    patchpoint_arguments.push_back(GetConstantZero(GetUint32Type()));
    // The arguments need to be recorded in the patchpoint, so LLVM doesn't discard them.
    for (llvm::Argument& arg : result->args()) {
      patchpoint_arguments.push_back(&arg);
    }
    if (return_type->isVoidTy()) {
      builder.CreateIntrinsic(
          llvm::Intrinsic::experimental_patchpoint_void, {}, patchpoint_arguments);
      builder.CreateRetVoid();
    } else {
      llvm::Value* return_value = builder.CreateIntrinsic(
          llvm::Intrinsic::experimental_patchpoint, {return_type}, patchpoint_arguments);
      builder.CreateRet(return_value);
    }
    return result;
  };

  auto make_load_function = [&](std::string_view function_name,
                                llvm::Type* load_type) -> llvm::Function* {
    std::array<llvm::Type*, 2> parameter_types = {GetUncompressedGCPointerType(), GetUint32Type()};
    llvm::Function* result = make_function(function_name, load_type, parameter_types);
    // Add memory(argmem: read, inaccessiblemem: readwrite) attribute.
    llvm::MemoryEffects memory_effects =
        llvm::MemoryEffects::none()
            .getWithModRef(llvm::IRMemLocation::ArgMem, llvm::ModRefInfo::Ref)
            .getWithModRef(llvm::IRMemLocation::InaccessibleMem, llvm::ModRefInfo::ModRef);
    result->addFnAttr(llvm::Attribute::getWithMemoryEffects(llvm_context_, memory_effects));
    result->setMetadata(kRewriteToPatchpointMetadata, empty_node);
    result->addFnAttr("gc-leaf-function");
    return result;
  };

  auto make_load_acquire_function = [&](std::string_view function_name,
                                        llvm::Type* load_type) -> llvm::Function* {
    std::array<llvm::Type*, 1> parameter_types = {GetUncompressedGCPointerType()};
    llvm::Function* result = make_function(function_name, load_type, parameter_types);
    // Atomic loads should use memory(argmem: readwrite, inaccessiblemem: readwrite).
    llvm::MemoryEffects memory_effects =
        llvm::MemoryEffects::none()
            .getWithModRef(llvm::IRMemLocation::ArgMem, llvm::ModRefInfo::ModRef)
            .getWithModRef(llvm::IRMemLocation::InaccessibleMem, llvm::ModRefInfo::ModRef);
    result->addFnAttr(llvm::Attribute::getWithMemoryEffects(llvm_context_, memory_effects));
    result->setMetadata(kRewriteToPatchpointMetadata, empty_node);
    result->addFnAttr("gc-leaf-function");
    return result;
  };

  auto make_store_function = [&](std::string_view function_name,
                                 llvm::Type* stored_type) -> llvm::Function* {
    std::array<llvm::Type*, 3> parameter_types = {
        stored_type, GetUncompressedGCPointerType(), GetUint32Type()};
    llvm::Function* result = make_function(function_name, GetVoidType(), parameter_types);
    // Add memory(argmem: write, inaccessiblemem: readwrite) attribute.
    llvm::MemoryEffects memory_effects =
        llvm::MemoryEffects::none()
            .getWithModRef(llvm::IRMemLocation::ArgMem, llvm::ModRefInfo::Mod)
            .getWithModRef(llvm::IRMemLocation::InaccessibleMem, llvm::ModRefInfo::ModRef);
    result->addFnAttr(llvm::Attribute::getWithMemoryEffects(llvm_context_, memory_effects));
    if (stored_type->isPointerTy()) {
      // Add readnone attribute to the stored pointer.
      result->addParamAttr(0, llvm::Attribute::AttrKind::ReadNone);
    }
    result->setMetadata(kRewriteToPatchpointMetadata, empty_node);
    result->addFnAttr("gc-leaf-function");
    return result;
  };

  auto make_store_release_function = [&](std::string_view function_name, llvm::Type* stored_type) {
    std::array<llvm::Type*, 2> parameter_types = {stored_type, GetUncompressedGCPointerType()};
    llvm::Function* result = make_function(function_name, GetVoidType(), parameter_types);
    // Atomic stores should use memory(argmem: readwrite, inaccessiblemem: readwrite).
    llvm::MemoryEffects memory_effects =
        llvm::MemoryEffects::none()
            .getWithModRef(llvm::IRMemLocation::ArgMem, llvm::ModRefInfo::ModRef)
            .getWithModRef(llvm::IRMemLocation::InaccessibleMem, llvm::ModRefInfo::ModRef);
    result->addFnAttr(llvm::Attribute::getWithMemoryEffects(llvm_context_, memory_effects));
    if (stored_type->isPointerTy()) {
      // Add readnone attribute to the stored pointer.
      result->addParamAttr(0, llvm::Attribute::AttrKind::ReadNone);
    }
    result->setMetadata(kRewriteToPatchpointMetadata, empty_node);
    result->addFnAttr("gc-leaf-function");
    return result;
  };

  // Cast to uncompressed
  {
    placeholder_functions_.cast_to_uncompressed =
        make_function(kCastToUncompressedPlaceholderFunctionName,
                      GetUncompressedGCPointerType(),
                      {GetCompressedGCPointerType()});
    // NOTE: These are the attributes that clang gives to a function like:
    // int foo(int n) { return n; }
    // We copy these attributes for GC pointer casts.

    // Add nofree, norecurse, nosync, nounwind, willreturn, and memory(none) attributes
    placeholder_functions_.cast_to_uncompressed->addFnAttr(llvm::Attribute::NoFree);
    placeholder_functions_.cast_to_uncompressed->addFnAttr(llvm::Attribute::NoRecurse);
    placeholder_functions_.cast_to_uncompressed->addFnAttr(llvm::Attribute::NoSync);
    placeholder_functions_.cast_to_uncompressed->addFnAttr(llvm::Attribute::NoUnwind);
    placeholder_functions_.cast_to_uncompressed->addFnAttr(llvm::Attribute::WillReturn);
    placeholder_functions_.cast_to_uncompressed->addFnAttr(
        llvm::Attribute::getWithMemoryEffects(llvm_context_, llvm::MemoryEffects::none()));
    placeholder_functions_.cast_to_uncompressed->addFnAttr("gc-leaf-function");
    // Add a metadata tag to more easily distinguish it from other functions.
    placeholder_functions_.cast_to_uncompressed->setMetadata(kGCPointerCastMetadata, empty_node);
  }

  // Cast to compressed
  {
    placeholder_functions_.cast_to_compressed =
        make_function(kCastToCompressedPlaceholderFunctionName,
                      GetCompressedGCPointerType(),
                      {GetUncompressedGCPointerType()});
    // NOTE: These are the attributes that clang gives to a function like:
    // int foo(int n) { return n; }
    // We copy these attributes for GC pointer casts.

    // Add nofree, norecurse, nosync, nounwind, willreturn, and memory(none) attributes
    placeholder_functions_.cast_to_compressed->addFnAttr(llvm::Attribute::NoFree);
    placeholder_functions_.cast_to_compressed->addFnAttr(llvm::Attribute::NoRecurse);
    placeholder_functions_.cast_to_compressed->addFnAttr(llvm::Attribute::NoSync);
    placeholder_functions_.cast_to_compressed->addFnAttr(llvm::Attribute::NoUnwind);
    placeholder_functions_.cast_to_compressed->addFnAttr(llvm::Attribute::WillReturn);
    placeholder_functions_.cast_to_compressed->addFnAttr(
        llvm::Attribute::getWithMemoryEffects(llvm_context_, llvm::MemoryEffects::none()));
    placeholder_functions_.cast_to_compressed->addFnAttr("gc-leaf-function");
    // Add a metadata tag to more easily distinguish it from other functions.
    placeholder_functions_.cast_to_compressed->setMetadata(kGCPointerCastMetadata, empty_node);
  }

  // Cast pointer to int
  {
    placeholder_functions_.cast_pointer_to_int =
        make_function(kCastPointerToIntPlaceholderFunctionName,
                      GetUint64Type(),
                      {GetUncompressedGCPointerType()});
    // NOTE: These are the attributes that clang gives to a function like:
    // int foo(int n) { return n; }
    // We copy these attributes for GC pointer to int casts.

    // Add nofree, norecurse, nosync, nounwind, willreturn, and memory(inaccessiblemem: read)
    // attributes.
    placeholder_functions_.cast_pointer_to_int->addFnAttr(llvm::Attribute::NoFree);
    placeholder_functions_.cast_pointer_to_int->addFnAttr(llvm::Attribute::NoRecurse);
    placeholder_functions_.cast_pointer_to_int->addFnAttr(llvm::Attribute::NoSync);
    placeholder_functions_.cast_pointer_to_int->addFnAttr(llvm::Attribute::NoUnwind);
    placeholder_functions_.cast_pointer_to_int->addFnAttr(llvm::Attribute::WillReturn);
    // Use memory(inaccessiblemem: read) to prevent merging these placeholder calls across
    // relocations.
    placeholder_functions_.cast_pointer_to_int->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        llvm_context_, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref)));
    placeholder_functions_.cast_pointer_to_int->addFnAttr("gc-leaf-function");
    // Add a metadata tag to more easily distinguish it from other functions.
    placeholder_functions_.cast_pointer_to_int->setMetadata(kGCPointerCastToIntMetadata,
                                                            empty_node);
  }

  // Heap reference poisoning
  {
    placeholder_functions_.heap_reference_poisoning =
        make_function(kHeapReferencePoisoningPlaceholderFunctionName,
                      GetUncompressedGCPointerType(),
                      {GetUncompressedGCPointerType()});
    // NOTE: These are the attributes that clang gives to a function like:
    // int foo(int n) { return n; }
    // We copy these attributes for GC pointer to int casts.

    // Add nofree, norecurse, nosync, nounwind, willreturn, and memory(inaccessiblemem: read)
    // attributes.
    placeholder_functions_.heap_reference_poisoning->addFnAttr(llvm::Attribute::NoFree);
    placeholder_functions_.heap_reference_poisoning->addFnAttr(llvm::Attribute::NoRecurse);
    placeholder_functions_.heap_reference_poisoning->addFnAttr(llvm::Attribute::NoSync);
    placeholder_functions_.heap_reference_poisoning->addFnAttr(llvm::Attribute::NoUnwind);
    placeholder_functions_.heap_reference_poisoning->addFnAttr(llvm::Attribute::WillReturn);
    // Use memory(inaccessiblemem: read) to prevent moving these placeholder calls across
    // relocations.
    placeholder_functions_.heap_reference_poisoning->addFnAttr(
        llvm::Attribute::getWithMemoryEffects(
            llvm_context_, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref)));
    placeholder_functions_.heap_reference_poisoning->addFnAttr("gc-leaf-function");
    // Add a metadata tag to more easily distinguish it from other functions.
    placeholder_functions_.heap_reference_poisoning->setMetadata(kHeapReferencePoisoningMetadata,
                                                                 empty_node);
  }

  // Implicit suspend check
  {
    placeholder_functions_.implicit_suspend_check =
        make_function(kImplicitSuspendCheckPlaceholderFunctionName,
                      GetVoidType(),
                      {GetMethodPointerType()},
                      true);
    // Add memory(inaccessiblemem: readwrite) attribute.
    placeholder_functions_.implicit_suspend_check->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        llvm_context_, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::ModRef)));
    placeholder_functions_.implicit_suspend_check->setCallingConv(
        llvm::CallingConv::ARTPreserveAll);
  }

  // Suspend check
  {
    placeholder_functions_.suspend_check =
        make_function(kSuspendCheckPlaceholderFunctionName, GetVoidType(), {});
    // Add memory(inaccessiblemem: readwrite) attribute.
    placeholder_functions_.suspend_check->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        llvm_context_, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::ModRef)));
  }

  // Loads
  placeholder_functions_.load_gc_root =
      make_load_function(kLoadGcRootPlaceholderFunctionName, GetUncompressedGCPointerType());
  placeholder_functions_.load_i1 =
      make_load_function(kLoadBooleanPlaceholderFunctionName, GetBooleanType());
  placeholder_functions_.load_i1->addRetAttr(llvm::Attribute::ZExt);
  placeholder_functions_.load_i8 =
      make_load_function(kLoadInt8PlaceholderFunctionName, GetInt8Type());
  placeholder_functions_.load_i8->addRetAttr(llvm::Attribute::ZExt);
  placeholder_functions_.load_i16 =
      make_load_function(kLoadInt16PlaceholderFunctionName, GetInt16Type());
  placeholder_functions_.load_i16->addRetAttr(llvm::Attribute::ZExt);
  placeholder_functions_.load_i32 =
      make_load_function(kLoadInt32PlaceholderFunctionName, GetInt32Type());
  placeholder_functions_.load_i64 =
      make_load_function(kLoadInt64PlaceholderFunctionName, GetInt64Type());
  placeholder_functions_.load_f32 =
      make_load_function(kLoadFloat32PlaceholderFunctionName, GetFloat32Type());
  placeholder_functions_.load_f64 =
      make_load_function(kLoadFloat64PlaceholderFunctionName, GetFloat64Type());

  // Load acquires
  placeholder_functions_.load_acquire_gc_root = make_load_acquire_function(
      kLoadAcquireGcRootPlaceholderFunctionName, GetUncompressedGCPointerType());
  placeholder_functions_.load_acquire_i1 =
      make_load_acquire_function(kLoadAcquireBooleanPlaceholderFunctionName, GetBooleanType());
  placeholder_functions_.load_acquire_i1->addRetAttr(llvm::Attribute::ZExt);
  placeholder_functions_.load_acquire_i8 =
      make_load_acquire_function(kLoadAcquireInt8PlaceholderFunctionName, GetInt8Type());
  placeholder_functions_.load_acquire_i8->addRetAttr(llvm::Attribute::ZExt);
  placeholder_functions_.load_acquire_i16 =
      make_load_acquire_function(kLoadAcquireInt16PlaceholderFunctionName, GetInt16Type());
  placeholder_functions_.load_acquire_i16->addRetAttr(llvm::Attribute::ZExt);
  placeholder_functions_.load_acquire_i32 =
      make_load_acquire_function(kLoadAcquireInt32PlaceholderFunctionName, GetInt32Type());
  placeholder_functions_.load_acquire_i64 =
      make_load_acquire_function(kLoadAcquireInt64PlaceholderFunctionName, GetInt64Type());

  // Discarded load
  {
    std::array<llvm::Type*, 1> parameter_types = {GetUncompressedGCPointerType()};
    placeholder_functions_.discarded_load =
        make_function(kDiscardedLoadPlaceholderFunctionName, GetVoidType(), parameter_types);
    // Add memory(argmem: read, inaccessiblemem: readwrite) attribute.
    llvm::MemoryEffects memory_effects =
        llvm::MemoryEffects::none()
            .getWithModRef(llvm::IRMemLocation::ArgMem, llvm::ModRefInfo::Ref)
            .getWithModRef(llvm::IRMemLocation::InaccessibleMem, llvm::ModRefInfo::ModRef);
    placeholder_functions_.discarded_load->addFnAttr(
        llvm::Attribute::getWithMemoryEffects(llvm_context_, memory_effects));
    placeholder_functions_.discarded_load->setMetadata(kRewriteToPatchpointMetadata, empty_node);
    placeholder_functions_.discarded_load->addFnAttr("gc-leaf-function");
  }

  // Stores
  placeholder_functions_.store_gc_root =
      make_store_function(kStoreGcRootPlaceholderFunctionName, GetUncompressedGCPointerType());
  placeholder_functions_.store_i1 =
      make_store_function(kStoreBooleanPlaceholderFunctionName, GetBooleanType());
  placeholder_functions_.store_i8 =
      make_store_function(kStoreInt8PlaceholderFunctionName, GetInt8Type());
  placeholder_functions_.store_i16 =
      make_store_function(kStoreInt16PlaceholderFunctionName, GetInt16Type());
  placeholder_functions_.store_i32 =
      make_store_function(kStoreInt32PlaceholderFunctionName, GetInt32Type());
  placeholder_functions_.store_i64 =
      make_store_function(kStoreInt64PlaceholderFunctionName, GetInt64Type());
  placeholder_functions_.store_f32 =
      make_store_function(kStoreFloat32PlaceholderFunctionName, GetFloat32Type());
  placeholder_functions_.store_f64 =
      make_store_function(kStoreFloat64PlaceholderFunctionName, GetFloat64Type());

  // Store releases
  placeholder_functions_.store_release_gc_root = make_store_release_function(
      kStoreReleaseGcRootPlaceholderFunctionName, GetUncompressedGCPointerType());
  placeholder_functions_.store_release_i1 =
      make_store_release_function(kStoreReleaseBooleanPlaceholderFunctionName, GetBooleanType());
  placeholder_functions_.store_release_i8 =
      make_store_release_function(kStoreReleaseInt8PlaceholderFunctionName, GetInt8Type());
  placeholder_functions_.store_release_i16 =
      make_store_release_function(kStoreReleaseInt16PlaceholderFunctionName, GetInt16Type());
  placeholder_functions_.store_release_i32 =
      make_store_release_function(kStoreReleaseInt32PlaceholderFunctionName, GetInt32Type());
  placeholder_functions_.store_release_i64 =
      make_store_release_function(kStoreReleaseInt64PlaceholderFunctionName, GetInt64Type());

  // System.arraycopy with char[] or String.getCharsNoCheck with uncompressed strings
  {
    // Signature: void (ptr %dest, ptr %source, i32 %length)
    placeholder_functions_.memcpy_i16 = make_function(
        kMemCpyI16PlaceholderFunctionName,
        GetVoidType(),
        {GetUncompressedGCPointerType(), GetUncompressedGCPointerType(), GetInt32Type()});
    // NOTE: These are the attributes given to the @llvm.memcpy.* intrinsic.
    // Add nocallback, nofree, nounwind, willreturn, and memory(argmem: readwrite) attributes
    placeholder_functions_.memcpy_i16->addFnAttr(llvm::Attribute::NoCallback);
    placeholder_functions_.memcpy_i16->addFnAttr(llvm::Attribute::NoFree);
    placeholder_functions_.memcpy_i16->addFnAttr(llvm::Attribute::NoUnwind);
    placeholder_functions_.memcpy_i16->addFnAttr(llvm::Attribute::WillReturn);
    placeholder_functions_.memcpy_i16->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        llvm_context_, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef)));
    placeholder_functions_.memcpy_i16->addFnAttr("gc-leaf-function");
    // Add writeonly and readonly attributes to destination and source pointer arguments.
    placeholder_functions_.memcpy_i16->addParamAttr(0, llvm::Attribute::WriteOnly);
    placeholder_functions_.memcpy_i16->addParamAttr(1, llvm::Attribute::ReadOnly);
    // Add a metadata tag to more easily distinguish it from other functions.
    placeholder_functions_.memcpy_i16->setMetadata(kMemCpyI16Metadata, empty_node);
  }

  // System.arraycopy with Object[]
  {
    // Signature: void (ptr %dest, ptr %source, i32 %length)
    placeholder_functions_.memcpy_i32 = make_function(
        kMemCpyI32PlaceholderFunctionName,
        GetVoidType(),
        {GetUncompressedGCPointerType(), GetUncompressedGCPointerType(), GetInt32Type()});
    // NOTE: These are the attributes given to the @llvm.memcpy.* intrinsic.
    // Add nocallback, nofree, nounwind, willreturn, and memory(argmem: readwrite) attributes
    placeholder_functions_.memcpy_i32->addFnAttr(llvm::Attribute::NoCallback);
    placeholder_functions_.memcpy_i32->addFnAttr(llvm::Attribute::NoFree);
    placeholder_functions_.memcpy_i32->addFnAttr(llvm::Attribute::NoUnwind);
    placeholder_functions_.memcpy_i32->addFnAttr(llvm::Attribute::WillReturn);
    placeholder_functions_.memcpy_i32->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        llvm_context_, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef)));
    placeholder_functions_.memcpy_i32->addFnAttr("gc-leaf-function");
    // Add writeonly and readonly attributes to destination and source pointer arguments.
    placeholder_functions_.memcpy_i32->addParamAttr(0, llvm::Attribute::WriteOnly);
    placeholder_functions_.memcpy_i32->addParamAttr(1, llvm::Attribute::ReadOnly);
    // Add a metadata tag to more easily distinguish it from other functions.
    placeholder_functions_.memcpy_i32->setMetadata(kMemCpyI32Metadata, empty_node);
  }

  // String.getCharsNoCheck with compressed strings
  {
    // Signature: void (ptr %dest, ptr %source, i32 %length)
    placeholder_functions_.memcpy_i8_zext_to_i16 = make_function(
        kMemCpyI8ZextToI16PlaceholderFunctionName,
        GetVoidType(),
        {GetUncompressedGCPointerType(), GetUncompressedGCPointerType(), GetInt32Type()});
    // NOTE: These are the attributes given to the @llvm.memcpy.* intrinsic.
    // Add nocallback, nofree, nounwind, willreturn, and memory(argmem: readwrite) attributes
    placeholder_functions_.memcpy_i8_zext_to_i16->addFnAttr(llvm::Attribute::NoCallback);
    placeholder_functions_.memcpy_i8_zext_to_i16->addFnAttr(llvm::Attribute::NoFree);
    placeholder_functions_.memcpy_i8_zext_to_i16->addFnAttr(llvm::Attribute::NoUnwind);
    placeholder_functions_.memcpy_i8_zext_to_i16->addFnAttr(llvm::Attribute::WillReturn);
    placeholder_functions_.memcpy_i8_zext_to_i16->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        llvm_context_, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef)));
    placeholder_functions_.memcpy_i8_zext_to_i16->addFnAttr("gc-leaf-function");
    // Add writeonly and readonly attributes to destination and source pointer arguments.
    placeholder_functions_.memcpy_i8_zext_to_i16->addParamAttr(0, llvm::Attribute::WriteOnly);
    placeholder_functions_.memcpy_i8_zext_to_i16->addParamAttr(1, llvm::Attribute::ReadOnly);
    // Add a metadata tag to more easily distinguish it from other functions.
    placeholder_functions_.memcpy_i8_zext_to_i16->setMetadata(kMemCpyI8ZextToI16Metadata,
                                                              empty_node);
  }

  // String.equals
  {
    // NOTE: The third argument is the number of bytes to compare
    // if mirror::kUseStringCompression == true, otherwise it's the number of chars to comapre.
    //
    // Signature: i1 (ptr %lhs, ptr %rhs, i32 %bytes_to_compare/chars_to_compare)
    placeholder_functions_.string_equals = make_function(
        kStringEqualsPlaceholderFunctionName,
        GetBooleanType(),
        {GetUncompressedGCPointerType(), GetUncompressedGCPointerType(), GetInt32Type()});
    // Add nocallback, nofree, nounwind, willreturn, and memory(argmem: read) attributes
    placeholder_functions_.string_equals->addFnAttr(llvm::Attribute::NoCallback);
    placeholder_functions_.string_equals->addFnAttr(llvm::Attribute::NoFree);
    placeholder_functions_.string_equals->addFnAttr(llvm::Attribute::NoUnwind);
    placeholder_functions_.string_equals->addFnAttr(llvm::Attribute::WillReturn);
    placeholder_functions_.string_equals->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        llvm_context_, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    placeholder_functions_.string_equals->addFnAttr("gc-leaf-function");
    // Add a metadata tag to more easily distinguish it from other functions.
    placeholder_functions_.string_equals->setMetadata(kStringEqualsMetadata, empty_node);
  }
}

static bool SetContains(const ArenaHashSet<HInstruction*>& set, HInstruction* value) {
  return set.find(value) != set.end();
}

template <typename T>
static bool MapContains(const ArenaHashMap<HInstruction*, T>& map, HInstruction* value) {
  return map.find(value) != map.end();
}

static void CollectLiveValuesAcrossCatchBlockHelper(HBasicBlock* parent_block,
                                                    HBasicBlock* current_block,
                                                    llvm::SmallDenseSet<HBasicBlock*>& visited,
                                                    llvm::SmallDenseSet<HBasicBlock*>& visited_path,
                                                    ArenaHashSet<HInstruction*>& live_values) {
  if (visited.contains(current_block)) {
    return;
  }
  visited.insert(current_block);
  DCHECK(!visited_path.contains(current_block));
  visited_path.insert(current_block);
  auto should_record_value = [&](HInstruction* inst, bool is_nop_environment = false) {
    HBasicBlock* inst_block = inst->GetBlock();
    // If the value was defined along the path we are currently visiting, then we shouldn't add it
    // as a live value. This prevents unnecessarily recording values if the try-catch is inside a
    // loop.
    return !inst->IsConstant() && !visited_path.contains(inst_block) &&
           inst_block->Dominates(parent_block) &&
           (is_nop_environment || inst->GetType() == DataType::Type::kReference);
  };
  for (HInstructionIterator it(current_block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* inst = it.Current();
    // Record input values.
    for (HInstruction* input : inst->GetInputs()) {
      if (should_record_value(input)) {
        DCHECK_NE(input->GetType(), DataType::Type::kVoid);
        live_values.insert(input);
      }
    }

    // Record environment values.
    for (HEnvironment* environment = inst->GetEnvironment(); environment != nullptr;
         environment = environment->GetParent()) {
      for (size_t i = 0, env_size = environment->Size(); i < env_size; ++i) {
        HInstruction* env_value = environment->GetInstructionAt(i);
        if (env_value != nullptr && should_record_value(env_value, inst->IsNop())) {
          DCHECK_NE(env_value->GetType(), DataType::Type::kVoid);
          live_values.insert(env_value);
        }
      }
    }
  }

  if (HTryBoundary* try_boundary = current_block->GetLastInstruction()->AsTryBoundaryOrNull()) {
    CollectLiveValuesAcrossCatchBlockHelper(
        parent_block, try_boundary->GetNormalFlowSuccessor(), visited, visited_path, live_values);
    // Record exceptional successors only for entry TryBoundary.
    if (try_boundary->IsEntry()) {
      for (HBasicBlock* catch_block : current_block->GetExceptionalSuccessors()) {
        CollectLiveValuesAcrossCatchBlockHelper(
            parent_block, catch_block, visited, visited_path, live_values);
      }
    }
  } else {
    for (HBasicBlock* successor_block : current_block->GetNormalSuccessors()) {
      CollectLiveValuesAcrossCatchBlockHelper(
          parent_block, successor_block, visited, visited_path, live_values);
    }
  }
  visited_path.erase(current_block);
}

static ArenaHashSet<HInstruction*> CollectLiveValuesAcrossCatchBlock(HBasicBlock* block,
                                                                     ArenaAllocator* allocator) {
  DCHECK(block->IsCatchBlock());
  ArenaHashSet<HInstruction*> live_values(
      allocator->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator));
  llvm::SmallDenseSet<HBasicBlock*> visited;
  llvm::SmallDenseSet<HBasicBlock*> visited_path;
  CollectLiveValuesAcrossCatchBlockHelper(block, block, visited, visited_path, live_values);
  return live_values;
}

static ArenaHashMap<HInstruction*, uint32_t> GetUsedStackSavedValuesInBlock(
    HBasicBlock* block,
    const ArenaHashSet<HInstruction*>& filter,
    bool record_environment,
    ArenaAllocator* allocator) {
  ArenaHashMap<HInstruction*, uint32_t> result(
      allocator->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator));

  uint32_t instruction_index = 0;
  for (HInstructionIterator it(block->GetInstructions()); !it.Done();
       it.Advance(), ++instruction_index) {
    HInstruction* current = it.Current();
    for (HInstruction* input : current->GetInputs()) {
      if (input->GetBlock() != block && SetContains(filter, input)) {
        auto [result_it, inserted] = result.insert({input, instruction_index});
        if (!inserted) {
          result_it->second = std::max(result_it->second, instruction_index);
        }
      }
    }

    if (record_environment) {
      for (HEnvironment* environment = current->GetEnvironment(); environment != nullptr;
           environment = environment->GetParent()) {
        size_t size = environment->Size();
        for (size_t i = 0; i < size; ++i) {
          HInstruction* env_value = environment->GetInstructionAt(i);
          if (env_value != nullptr && env_value->GetBlock() != block &&
              SetContains(filter, env_value)) {
            auto [result_it, inserted] = result.insert({env_value, instruction_index});
            if (!inserted) {
              result_it->second = std::max(result_it->second, instruction_index);
            }
          }
        }
      }
    }
  }

  return result;
}

static void PropagateStackLivenessToPredecessors(CodeGeneratorARM64LLVM* codegen,
                                                 HBasicBlock* use_block,
                                                 uint32_t use_index,
                                                 HInstruction* value) {
  using BasicBlockInfo = CodeGeneratorARM64LLVM::BasicBlockInfo;
  DCHECK(value->GetBlock() != use_block);
  DCHECK(value->GetBlock()->Dominates(use_block));

  HBasicBlock* value_block = value->GetBlock();

  llvm::SmallVector<HBasicBlock*, 32> worklist;
  {
    BasicBlockInfo& use_block_info = codegen->GetBasicBlockInfo(use_block);
    auto [it, inserted] =
        use_block_info.partially_live_stack_saved_values.insert({value, use_index});
    if (!inserted) {
      // We've already done this propagation.
      return;
    }
  }
  worklist.append(use_block->GetPredecessors().begin(), use_block->GetPredecessors().end());
  while (!worklist.empty()) {
    HBasicBlock* block = worklist.pop_back_val();
    // Don't process the basic block that the value is in.
    if (block == value_block) {
      continue;
    }

    BasicBlockInfo& block_info = codegen->GetBasicBlockInfo(block);
    auto [it, inserted] = block_info.live_stack_saved_values.insert(value);
    // We've already visited this block.
    if (!inserted) {
      continue;
    }

    worklist.append(block->GetPredecessors().begin(), block->GetPredecessors().end());
  }
}

void CodeGeneratorARM64LLVM::DoStackSavedValueLivenessAnalysis() {
  ArenaAllocator* allocator = GetGraph()->GetAllocator();

  for (HBasicBlock* block : *block_order_) {
    // Don't generate any info for the exit block.
    if (block->IsExitBlock()) {
      continue;
    }
    HTryBoundary* try_boundary = block->GetLastInstruction()->AsTryBoundaryOrNull();
    if (try_boundary == nullptr || !try_boundary->IsEntry()) {
      continue;
    }
    // Collect the values that need to be saved to the stack.
    for (HBasicBlock* catch_block : try_boundary->GetExceptionHandlers()) {
      ArenaHashSet<HInstruction*> live_values =
          CollectLiveValuesAcrossCatchBlock(catch_block, allocator);
      BasicBlockInfo& catch_block_info = GetBasicBlockInfo(catch_block);
      for (HInstruction* value : live_values) {
        all_stack_saved_values_.insert(value);
        catch_block_info.loaded_alloca_values.insert(value);
      }
    }
  }

  // We don't need to do any stack liveness analysis, so return early.
  if (all_stack_saved_values_.empty()) {
    return;
  }

  // Fill in instruction index map.
  for (HBasicBlock* block : *block_order_) {
    BasicBlockInfo& block_info = GetBasicBlockInfo(block);
    uint32_t instruction_index = 0;
    for (HInstructionIterator it(block->GetInstructions()); !it.Done();
         it.Advance(), ++instruction_index) {
      HInstruction* current = it.Current();
      block_info.instruction_index_map.insert({current, instruction_index});
    }
  }

  // Mark value stack liveness in all basic blocks.
  for (HBasicBlock* block : *block_order_) {
    // Don't generate any info for the exit block.
    if (block->IsExitBlock()) {
      continue;
    }

    ArenaHashMap<HInstruction*, uint32_t> used_live_stack_saved_values =
        GetUsedStackSavedValuesInBlock(
            block, all_stack_saved_values_, !GetGraph()->IsDeadReferenceSafe(), allocator);
    for (auto [live_value, use_index] : used_live_stack_saved_values) {
      PropagateStackLivenessToPredecessors(this, block, use_index, live_value);
    }
  }

  // Remove partially live values if they are used after this block.
  for (HBasicBlock* block : *block_order_) {
    BasicBlockInfo& block_info = GetBasicBlockInfo(block);
    for (HInstruction* value : block_info.live_stack_saved_values) {
      auto it = block_info.partially_live_stack_saved_values.find(value);
      if (it != block_info.partially_live_stack_saved_values.end()) {
        block_info.partially_live_stack_saved_values.erase(it);
      }
    }
  }

  for (HBasicBlock* block : *block_order_) {
    BasicBlockInfo& block_info = GetBasicBlockInfo(block);
    for (HInstruction* value : block_info.loaded_alloca_values) {
      block_info.propagated_stack_saved_values.insert({value, block});
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (HBasicBlock* block : *block_order_) {
      // We don't need to process the exit block or catch blocks.
      if (block->IsExitBlock() || block->IsCatchBlock()) {
        continue;
      }

      llvm::SmallDenseSet<HInstruction*> values_to_add;
      for (HBasicBlock* predecessor : block->GetPredecessors()) {
        for (const auto& [incoming_value, source_block] :
             GetBasicBlockInfo(predecessor).propagated_stack_saved_values) {
          if (incoming_value->GetBlock()->Dominates(block)) {
            values_to_add.insert(incoming_value);
          }
        }
      }

      BasicBlockInfo& block_info = GetBasicBlockInfo(block);
      for (HInstruction* value : values_to_add) {
        for (HBasicBlock* predecessor : block->GetPredecessors()) {
          BasicBlockInfo& predecessor_info = GetBasicBlockInfo(predecessor);
          auto predecessor_it = predecessor_info.propagated_stack_saved_values.find(value);
          HBasicBlock* source_block = nullptr;
          // The value comes from its definition.
          if (predecessor_it == predecessor_info.propagated_stack_saved_values.end()) {
            source_block = value->GetBlock();
          } else {
            source_block = predecessor_it->second;
          }

          auto it = block_info.propagated_stack_saved_values.find(value);
          if (it == block_info.propagated_stack_saved_values.end()) {
            changed = true;
            block_info.propagated_stack_saved_values.insert({value, source_block});
          } else if (it->second != block && it->second != source_block) {
            // The value comes from a different source block, so we need to use a phi for it.
            changed = true;
            block_info.stack_saved_value_phis.insert(value);
            it->second = block;
          }
        }
      }
    }
  }

  /*
  for (HBasicBlock* block : *block_order_) {
    BasicBlockInfo& block_info = GetBasicBlockInfo(block);
    LOG(INFO) << "B" << block->GetBlockId() << ":";
    // ArenaHashSet<HInstruction*> live_stack_saved_values;
    LOG(INFO) << "  live_stack_saved_values:";
    for (HInstruction* value : block_info.live_stack_saved_values) {
      LOG(INFO) << "    " << value->GetId() << ":" << value->GetType();
    }
    // ArenaHashSet<HInstruction*> try_boundary_stack_saved_values;
    LOG(INFO) << "  try_boundary_stack_saved_values:";
    for (HInstruction* value : block_info.try_boundary_stack_saved_values) {
      LOG(INFO) << "    " << value->GetId() << ":" << value->GetType();
    }
    // ArenaHashSet<HInstruction*> loaded_alloca_values;
    LOG(INFO) << "  loaded_alloca_values:";
    for (HInstruction* value : block_info.loaded_alloca_values) {
      LOG(INFO) << "    " << value->GetId() << ":" << value->GetType();
    }
    // ArenaHashMap<HInstruction*, HBasicBlock*> propagated_stack_saved_values;
    LOG(INFO) << "  propagated_stack_saved_values:";
    for (const auto& [value, source_block] : block_info.propagated_stack_saved_values) {
      LOG(INFO) << "    " << value->GetId() << ":" << value->GetType() << ", B"
                << source_block->GetBlockId();
    }
    // ArenaHashSet<HInstruction*> stack_saved_value_phis;
    LOG(INFO) << "  stack_saved_value_phis:";
    for (HInstruction* value : block_info.stack_saved_value_phis) {
      LOG(INFO) << "    " << value->GetId() << ":" << value->GetType();
    }
  }
  */
}

bool CodeGeneratorARM64LLVM::ShouldUseSVE() const {
  return GetCompilerOptions()
      .GetInstructionSetFeatures()
      ->AsArm64InstructionSetFeatures()
      ->HasSVE();
}

size_t CodeGeneratorARM64LLVM::GetSIMDRegisterWidth() const {
  return SupportsPredicatedSIMD() ? GetInstructionSetFeatures().GetSVEVectorLength() / kBitsPerByte
                                  : vixl::aarch64::kQRegSizeInBytes;
}

#define __ GetIRBuilder()->

void CodeGeneratorARM64LLVM::Initialize() {
  llvm::SmallVector<DataType::Type> function_return_and_parameter_types =
      GetMethodReturnTypeAndParameterTypes(ArrayRef<HBasicBlock* const>(*block_order_));
  DataType::Type return_type = function_return_and_parameter_types[0];
  llvm::ArrayRef<DataType::Type> parameter_types =
      llvm::ArrayRef(function_return_and_parameter_types).slice(1);
  // The return type is stored as the first character in the shorty.
  llvm::Type* llvm_return_type = GetLLVMType(return_type);
  const llvm::SmallVector<llvm::Type*> llvm_parameter_types =
      ::art::arm64_llvm::GetParameterTypes(parameter_types, llvm_types_);
  CalculateParameterStackOffsets(parameter_types);

  llvm::FunctionType* function_type =
      llvm::FunctionType::get(llvm_return_type, llvm_parameter_types, false);
  function_ = llvm::Function::Create(
      function_type, llvm::Function::LinkageTypes::ExternalLinkage, module_->getName(), *module_);
  function_->setGC(std::string(kArtGCStrategyName));
  function_->setCallingConv(llvm::CallingConv::ARTInvokeDex);

  llvm::Function* personality_fn =
      llvm::Function::Create(llvm::FunctionType::get(GetVoidType(), false),
                             llvm::Function::LinkageTypes::ExternalLinkage,
                             kARTPersonalityFunctionName,
                             *module_);
  function_->setPersonalityFn(personality_fn);

  // Prevent optimizations from inserting library function calls such as `memcpy` or `memset`.
  function_->addFnAttr("no-builtins");
  switch (GetOptimizationLevel().getSizeLevel()) {
    case 1:  // -Os
      function_->addFnAttr(llvm::Attribute::AttrKind::OptimizeForSize);
      break;
    case 2:  // -Oz
      function_->addFnAttr(llvm::Attribute::AttrKind::OptimizeForSize);
      function_->addFnAttr(llvm::Attribute::AttrKind::MinSize);
      break;
  }

  InitializeMetadata();
  InitializePlaceholderFunctions();

  AddAttributesToFunction(function_return_and_parameter_types);

  size_t blocks_size = GetGraph()->GetBlocks().size();
  block_infos_.reserve(blocks_size);
  for (size_t i = 0; i < blocks_size; ++i) {
    block_infos_.push_back(BasicBlockInfo(GetGraph()->GetAllocator()));
  }
  DoStackSavedValueLivenessAnalysis();
}

void CodeGeneratorARM64LLVM::RunOptimizerPasses() {
  llvm::LoopAnalysisManager loop_analysis_manager;
  llvm::FunctionAnalysisManager function_analysis_manager;
  llvm::CGSCCAnalysisManager cgscc_analysis_manager;
  llvm::ModuleAnalysisManager module_analysis_manager;

  llvm::ModulePassManager pass_manager =
      BuildLLVMPassPipeline(this,
                            target_machine_.get(),
                            GetOptimizationLevel(),
                            GetCompilerOptions().LLVMDuplicateOptPipeline(),
                            loop_analysis_manager,
                            function_analysis_manager,
                            cgscc_analysis_manager,
                            module_analysis_manager);

  pass_manager.run(*module_, module_analysis_manager);
}

// Remove placeholder functions from the module, so no code is generated for them.
void CodeGeneratorARM64LLVM::RemovePlaceholderFunctions() {
  static_assert(sizeof placeholder_functions_ == 39 * sizeof(llvm::Function*),
                "The number of members in placeholder_functions_ changed");
  for (llvm::Function* placeholder_function : {
           placeholder_functions_.implicit_suspend_check,
       }) {
    CHECK_EQ(placeholder_function->getNumUses(), 0u);
    placeholder_function->eraseFromParent();
  }
}

void CodeGeneratorARM64LLVM::AddClinitCheckPrologue() {
  if (!GetCompilerOptions().ShouldCompileWithClinitCheck(GetGraph()->GetArtMethod())) {
    return;
  }

  // Register codes of used registers in the prologue.
  const uint32_t x0 = 0;
  const uint32_t x16 = 16;
  const uint32_t x17 = 17;

  // The following is a direct copy of the assembly generated by ART.
  // TODO: Don't use magic constants.
  //
  // The prologue data contains 15 instructions from the clinit check, and up to two instructions
  // from the stack overflow check.
  constexpr size_t max_size = kClinitCheckPrologueSize + kStackOverflowCheckPrologueSize;
  llvm::SmallVector<uint32_t, max_size> prologue_instructions;
  // b9400010   ldr w16, [x0]
  prologue_instructions.push_back(
      CreateLdr32(x16, x0, ArtMethod::DeclaringClassOffset().Uint32Value()));
  // 3941ce11   ldrb w17, [x16, #115]
  prologue_instructions.push_back(CreateLdr8(x17, x16, kClassStatusByteOffset));
  // 7103c23f   cmp w17, #0xf0 (240)
  prologue_instructions.push_back(0x7103c23f);
  // 54000182   b.hs #+0x30 (label END)
  prologue_instructions.push_back(0x54000182);
  // 7103823f   cmp w17, #0xe0 (224)
  prologue_instructions.push_back(0x7103823f);
  // 54000122   b.hs #+0x24 (label DMB)
  prologue_instructions.push_back(0x54000122);
  // 7103423f   cmp w17, #0xd0 (208)
  prologue_instructions.push_back(0x7103423f);
  // 540000a3   b.lo #+0x14 (label QUICK_RESOLVE)
  prologue_instructions.push_back(0x540000a3);
  // b9404e10   ldr w16, [x16, #76]
  prologue_instructions.push_back(
      CreateLdr32(x16, x16, mirror::Class::ClinitThreadIdOffset().Uint32Value()));
  // b9400e71   ldr w17, [tr, #12] ; 12
  prologue_instructions.push_back(
      CreateLdr32(x17, tr.GetCode(), Thread::TidOffset<kArm64PointerSize>().Uint32Value()));
  // 6b11021f   cmp w16, w17
  prologue_instructions.push_back(0x6b11021f);
  // 54000080   b.eq #+0x10 (label END)
  prologue_instructions.push_back(0x54000080);
  // QUICK_RESOLVE:
  // f9425670   ldr x16, [tr, #1192] ; pQuickResolutionTrampoline
  ThreadOffset64 entrypoint_offset =
      GetThreadOffset<kArm64PointerSize>(kQuickQuickResolutionTrampoline);
  prologue_instructions.push_back(CreateLdr64(x16, tr.GetCode(), entrypoint_offset.Uint32Value()));
  // d61f0200   br x16
  prologue_instructions.push_back(0xd61f0200);
  // DMB:
  // d5033bbf   dmb ish
  prologue_instructions.push_back(0xd5033bbf);
  // END:
  DCHECK_EQ(prologue_instructions.size(), kClinitCheckPrologueSize);

  llvm::Function* function = GetFunction();
  if (function->hasPrologueData()) {
    llvm::Constant* stack_overflow_check_prologue = function->getPrologueData();
    DCHECK(stack_overflow_check_prologue->getType()->isArrayTy());
    DCHECK(stack_overflow_check_prologue->getType()->getArrayElementType() == GetUint32Type());
    DCHECK(llvm::isa<llvm::ConstantDataArray>(stack_overflow_check_prologue));
    llvm::ConstantDataArray* stack_overflow_check_prologue_array =
        llvm::cast<llvm::ConstantDataArray>(stack_overflow_check_prologue);
    unsigned array_size = stack_overflow_check_prologue_array->getNumElements();
    DCHECK_EQ(array_size, kStackOverflowCheckPrologueSize);
    for (unsigned i = 0; i < array_size; ++i) {
      uint64_t int_value = stack_overflow_check_prologue_array->getElementAsInteger(i);
      prologue_instructions.push_back(static_cast<uint32_t>(int_value));
    }
  }

  llvm::Constant* prologue = llvm::ConstantDataArray::get(GetLLVMContext(), prologue_instructions);
  function->setPrologueData(prologue);
}

CodeGeneratorARM64LLVM::ObjectMemoryBuffer CodeGeneratorARM64LLVM::EmitObjectFile() {
  // NOTE: We can also dump the IR here to a file for debugging.
  // module_->print(...);
  if constexpr (false) {
    llvm::dbgs() << "===================================================================\n";
    module_->print(llvm::dbgs(), nullptr);
    llvm::dbgs() << "===================================================================\n";

    std::error_code ec;
    llvm::raw_fd_ostream out("-", ec);
    {
      llvm::legacy::PassManager pass_manager;
      bool result = target_machine_->addPassesToEmitFile(
          pass_manager, out, nullptr, llvm::CodeGenFileType::AssemblyFile);
      if (result != false) {
        LOG(FATAL) << "LLVM doesn't support assembly file emission.";
      }

      out << "===================================================================\n";
      pass_manager.run(*module_);
      out.flush();
    }
    out << "===================================================================\n";
  }

  ObjectMemoryBuffer buffer;
  llvm::raw_svector_ostream out(buffer);

  llvm::legacy::PassManager pass_manager;
  bool result = target_machine_->addPassesToEmitFile(
      pass_manager, out, nullptr, llvm::CodeGenFileType::ObjectFile);
  if (result != false) {
    LOG(FATAL) << "LLVM doesn't support object file emission.";
  }

  pass_manager.run(*module_);

  return buffer;
}

static void AlignCodeData(ArenaVector<uint8_t>& code, uint32_t align) {
  if (code.size() % align != 0) {
    size_t padding_size = align - (code.size() % align);
    code.resize(code.size() + padding_size, 0);  // Pad with zeros.
  }
}

// Returns the required alignment of the specified .rodata section, which is parsed from the suffix
// of '.rodata.cst' or '.rodata.str'.
static uint32_t GetAlignmentFromRodataSectionName(llvm::StringRef section_name) {
  if (section_name == ".rodata") {
    // Return a default alignment value of 8. This should be large enough for all objects.
    return 8;
  } else if (section_name.consume_front(".rodata.cst")) {
    // Format: .rodata.cst<entry_size>
    // NOTE: The entry size and the alignment may be different, but 'entry_size == N * alignment'
    // should always hold, so it is safe to return the entry size from this function.
    uint32_t entry_size = 0;
    bool error = section_name.getAsInteger(10, entry_size);
    CHECK(!error);
    return entry_size;
  } else if (section_name.consume_front(".rodata.str")) {
    // Format: .rodata.str<entry_size>.<alignment>
    uint32_t entry_size;
    bool error = section_name.consumeInteger(10, entry_size);
    CHECK(!error);

    bool consumed_dot = section_name.consume_front(".");
    CHECK(consumed_dot);

    uint32_t alignment;
    error = section_name.getAsInteger(10, alignment);
    CHECK(!error);

    return alignment;
  } else {
    LOG(FATAL) << "Unknown .rodata section name: " << std::string_view(section_name);
    return 0;
  }
}

void CodeGeneratorARM64LLVM::ParseObjectFile(llvm::ArrayRef<char> object_file_buffer) {
  ELFFileParser object_file(object_file_buffer);

  EHFrameParseResult eh_frame_info = ParseEHFrame(object_file);
  // Get the frame size of the function from the '.stack_sizes' section.
  const uint64_t frame_size = ParseFrameSize(object_file);
  DCHECK_IMPLIES(eh_frame_info.frame_size != 0, eh_frame_info.frame_size == frame_size);
  LOG(DEBUG) << "frame size: " << frame_size;
  DCHECK_LE(frame_size, GetMaximumFrameSize());

  int64_t cfa_offset = eh_frame_info.cfa_offset;
  const size_t prologue_size =
      GetFunction()->hasPrologueData()
          ? GetFunction()->getPrologueData()->getType()->getArrayNumElements()
          : 0;
  const bool has_stack_overflow_check =
      prologue_size == kStackOverflowCheckPrologueSize ||
      prologue_size == kClinitCheckPrologueSize + kStackOverflowCheckPrologueSize;
  if (!has_stack_overflow_check && FrameNeedsStackCheck(frame_size, InstructionSet::kArm64)) {
    // TODO: Handle this error. One potential fix would be to add the prologue data to the function
    // here and do code generation again. This is probably a rare enough case that this could be a
    // viable option.
    LOG(FATAL) << "TODO: Function needs a stack overflow check, but doesn't contain one.";
  }

  // Populate `code_`.
  RodataOffsetMap rodata_offsets = ParseCode(object_file);
  ParseRelocations(object_file, rodata_offsets);

  StackMapStream* stack_map_stream = GetStackMapStream();
  stack_map_stream->BeginMethod(frame_size,
                                eh_frame_info.core_spill_mask,
                                eh_frame_info.fp_spill_mask,
                                GetGraph()->GetNumberOfVRegs(),
                                GetGraph()->IsCompilingBaseline(),
                                GetGraph()->IsDebuggable(),
                                GetGraph()->HasShouldDeoptimizeFlag());

  // Add stack map entry for stack overflow check.
  if (has_stack_overflow_check) {
    llvm::Constant* prologue_data = GetFunction()->getPrologueData();
    DCHECK(prologue_data->getType()->isArrayTy());
    uint32_t stack_overflow_check_pc_offset =
        prologue_data->getType()->getArrayNumElements() * kInstructionSize;
    stack_map_stream->BeginStackMapEntry(0, stack_overflow_check_pc_offset);
    stack_map_stream->EndStackMapEntry();
  }

  // Parse stack map.
  ParseStackMap(object_file, frame_size, cfa_offset);

  stack_map_stream->EndMethod(code_.size());
}

CodeGeneratorARM64LLVM::EHFrameParseResult CodeGeneratorARM64LLVM::ParseEHFrame(
    ELFFileParser& object_file) {
  const ELFFileParser::ELFT::Shdr& eh_frame_section = object_file.GetSection(".eh_frame");
  llvm::ArrayRef<uint8_t> eh_frame_data = object_file.GetSectionContents(eh_frame_section);
  const llvm::DataLayout& data_layout = module_->getDataLayout();
  llvm::DWARFDataExtractor dwarf_data_extractor(
      eh_frame_data, data_layout.isLittleEndian(), data_layout.getPointerSize());
  llvm::DWARFDebugFrame eh_frame(llvm::Triple::aarch64, /* IsEH= */ true, eh_frame_section.sh_addr);
  llvm::Error parse_error = eh_frame.parse(dwarf_data_extractor);
  CHECK(!parse_error);

  EHFrameParseResult result{};

  int fde_count = 0;
  for (const llvm::dwarf::FrameEntry& entry : eh_frame.entries()) {
    if ([[maybe_unused]] const llvm::dwarf::CIE* cie = llvm::dyn_cast<llvm::dwarf::CIE>(&entry)) {
      // We don't care about the contents of this.
      continue;
    }
    CHECK_EQ(fde_count, 0) << "More than one frame description found in .eh_frame section";
    ++fde_count;
    DCHECK(llvm::isa<llvm::dwarf::FDE>(&entry));
    const llvm::dwarf::FDE* fde = llvm::cast<llvm::dwarf::FDE>(&entry);
    const llvm::dwarf::CIE* cie = fde->getLinkedCIE();

    // Get CFI data from the section.
    {
      CHECK_LT(fde->getOffset(), eh_frame_data.size());
      CHECK_LE(fde->getOffset() + fde->getLength() + 4, eh_frame_data.size());
      llvm::ArrayRef<uint8_t> fde_data =
          eh_frame_data.slice(fde->getOffset(), fde->getLength() + 4);
      // The first 16 bytes are the fields 'length', 'CIE_pointer', 'initial_location', and
      // 'address_range'
      CHECK_GE(fde_data.size(), 16u);
      llvm::ArrayRef<uint8_t> cfi_data = fde_data.slice(16);
      // Skip first byte if it's 0 (DW_CFA_nop or an empty augmentation string).
      if (cfi_data.size() >= 1 && cfi_data[0] == 0) {
        cfi_data = cfi_data.slice(1);
      }
      cfi_data_.insert(cfi_data_.end(), cfi_data.begin(), cfi_data.end());
    }

    for (const llvm::dwarf::CFIProgram::Instruction& cfi : entry.cfis()) {
      switch (cfi.Opcode) {
        case llvm::dwarf::DW_CFA_def_cfa_offset:
          result.frame_size = dchecked_integral_cast<uint32_t>(cfi.Ops[0]);
          break;
        case llvm::dwarf::DW_CFA_def_cfa_offset_sf:
          result.frame_size = dchecked_integral_cast<uint32_t>(static_cast<int64_t>(cfi.Ops[0]) *
                                                               cie->getDataAlignmentFactor());
          break;
        case llvm::dwarf::DW_CFA_def_cfa: {
          uint32_t register_number = dchecked_integral_cast<uint32_t>(cfi.Ops[0]);
          if (register_number == 29) {
            CHECK_GE(static_cast<int64_t>(cfi.Ops[1]), 0);
            result.cfa_offset = static_cast<int64_t>(cfi.Ops[1]);
          }
          break;
        }
        case llvm::dwarf::DW_CFA_def_cfa_sf: {
          uint32_t register_number = dchecked_integral_cast<uint32_t>(cfi.Ops[0]);
          if (register_number == 29) {
            CHECK_GE(static_cast<int64_t>(cfi.Ops[1]), 0);
            result.cfa_offset = static_cast<int64_t>(cfi.Ops[1]) * cie->getDataAlignmentFactor();
          }
          break;
        }
        case llvm::dwarf::DW_CFA_offset:
        case llvm::dwarf::DW_CFA_offset_extended:
        case llvm::dwarf::DW_CFA_offset_extended_sf: {
          DCHECK_EQ(cfi.Ops.size(), 2u);
          uint32_t register_number = dchecked_integral_cast<uint32_t>(cfi.Ops[0]);
          int64_t offset = static_cast<int64_t>(cfi.Ops[1]) * cie->getDataAlignmentFactor();

          // LOG(INFO) << "DW_CFA_offset: reg" << register_number << ", " << offset;
          CHECK_IMPLIES(register_number == 30, offset == -8)
              << "LR is not spilled to the top of the stack.";

          if (register_number <= 31) {
            // General purpose registers x0-x30 and sp.
            CHECK_EQ(result.core_spill_mask & ((uint32_t{1} << register_number) - 1), 0u)
                << "Core registers are not spilled in high-low, top-down order to the stack.";
            CHECK_EQ(result.fp_spill_mask, 0u)
                << "Core registers must be spilled before SIMD registers";
            result.core_spill_mask |= uint32_t{1} << register_number;
            CHECK_EQ(offset, POPCOUNT(result.core_spill_mask) * -8)
                << "Wrong stack offset for spilled register.";
          } else if (register_number >= 64 && register_number <= 95) {
            // Floating-point/SIMD registers v0-v31.
            CHECK_EQ(result.fp_spill_mask & ((uint32_t{1} << (register_number - 64)) - 1), 0u)
                << "SIMD registers are not spilled in high-low, top-down order to the stack.";
            result.fp_spill_mask |= uint32_t{1} << (register_number - 64);
            CHECK_EQ(offset,
                     (POPCOUNT(result.core_spill_mask) + POPCOUNT(result.fp_spill_mask)) * -8)
                << "Wrong stack offset for spilled register.";
          } else {
            LOG(FATAL) << "TODO: Unhandled DWARF register number: " << register_number;
          }
          break;
        }
        default:
          break;
      }
    }
  }

  return result;
}

uint64_t CodeGeneratorARM64LLVM::ParseFrameSize(ELFFileParser& object_file) {
  llvm::ArrayRef<uint8_t> stack_sizes_contents = object_file.GetSectionContents(".stack_sizes");
  // .stack_sizes section layout:
  //
  // .xword .Lfunc_begin  ; We discard this
  // <stack size in unsigned LEB128 format>
  CHECK_GT(stack_sizes_contents.size(), 8u) << "No entries found in '.stack_sizes' section";
  unsigned uleb128_length = 0;
  const uint64_t frame_size = llvm::decodeULEB128(stack_sizes_contents.data() + 8, &uleb128_length);
  CHECK_EQ(8 + uleb128_length, stack_sizes_contents.size())
      << "More than one entry found in '.stack_sizes' section";
  return frame_size;
}

CodeGeneratorARM64LLVM::RodataOffsetMap CodeGeneratorARM64LLVM::ParseCode(
    ELFFileParser& object_file) {
  RodataOffsetMap rodata_offsets;
  llvm::ArrayRef<uint8_t> llvm_code = object_file.GetTextSectionContents();
  DCHECK(code_.empty());
  code_.insert(code_.end(), llvm_code.begin(), llvm_code.end());
  // Add literal sections with specific alignments, and the regular '.rodata' section.
  for (std::string_view section_name : object_file.GetRodataSections()) {
    llvm::ArrayRef<uint8_t> literal_pool = object_file.GetSectionContentsOrEmpty(section_name);
    // Don't align anything if it's not needed.
    if (literal_pool.empty()) {
      continue;
    }
    // Ensure that the literal pool is aligned to the required alignment.
    uint32_t alignment = GetAlignmentFromRodataSectionName(section_name);
    AlignCodeData(code_, alignment);
    rodata_offsets.insert({section_name, dchecked_integral_cast<uint32_t>(code_.size())});
    code_.insert(code_.end(), literal_pool.begin(), literal_pool.end());
  }
  // Align the code data to the instruction size.
  AlignCodeData(code_, kInstructionSize);
  return rodata_offsets;
}

static uint32_t GetFrameEntryOffset(llvm::Function* function) {
  if (!function->hasPrologueData()) {
    return 0;
  }

  llvm::Constant* prologue_data = function->getPrologueData();
  DCHECK(prologue_data->getType()->isArrayTy());
  uint64_t prologue_size = prologue_data->getType()->getArrayNumElements();
  if (prologue_size >= kClinitCheckPrologueSize) {
    DCHECK(prologue_size == kClinitCheckPrologueSize ||
           prologue_size == kClinitCheckPrologueSize + kStackOverflowCheckPrologueSize);
    return kClinitCheckPrologueSize * kInstructionSize;
  }

  return 0;
}

void CodeGeneratorARM64LLVM::ParseRelocations(ELFFileParser& object_file,
                                              const RodataOffsetMap& rodata_offsets) {
  using ELFT = ELFFileParser::ELFT;
  // Parse relocations for '.text'.
  ELFT::RelaRange relas = object_file.GetSectionRelocations(".rela.text");
  for (size_t i = 0; i < relas.size(); ++i) {
    switch (relas[i].getType(false)) {
      case llvm::ELF::R_AARCH64_LD_PREL_LO19: {
        uint32_t sym = relas[i].getSymbol(false);
        DCHECK(rodata_offsets.contains(object_file.GetSectionNameOfSymbol(sym)));
        uint32_t rodata_offset = rodata_offsets.at(object_file.GetSectionNameOfSymbol(sym));

        uint32_t instruction_offset = relas[i].r_offset;
        int64_t addend = relas[i].r_addend;
        CHECK_GE(addend, 0);
        CHECK_LT(addend, 1 << 19);
        uint32_t literal_offset = rodata_offset + addend;
        CHECK_LT(literal_offset, code_.size());
        CHECK_LT(instruction_offset, literal_offset);

        uint32_t offset = literal_offset - instruction_offset;
        CHECK_ALIGNED(offset, 4);
        uint32_t imm19 = offset / 4;
        CHECK_LT(imm19, 1u << 18) << "Literal offset can't fit in a 19 bit signed immediate.";

        uint32_t inst = LoadInstruction(code_, instruction_offset);
        CHECK((inst & kLdrLiteralMask) == kLdrLiteral32Opcode ||
              (inst & kLdrLiteralMask) == kLdrLiteral64Opcode ||
              (inst & kLdrLiteralMask) == kLdrLiteralFP32Opcode ||
              (inst & kLdrLiteralMask) == kLdrLiteralFP64Opcode ||
              (inst & kLdrLiteralMask) == kLdrLiteralFP128Opcode)
            << fmt::format("Instruction ({:08x}) is not an LDR (literal).", inst);
        inst |= imm19 << kLdrLiteralOffsetShift;
        StoreInstruction(code_, instruction_offset, inst);
        break;
      }
      case llvm::ELF::R_AARCH64_ADR_PREL_LO21: {
        uint32_t sym = relas[i].getSymbol(false);
        DCHECK(rodata_offsets.contains(object_file.GetSectionNameOfSymbol(sym)));
        uint32_t rodata_offset = rodata_offsets.at(object_file.GetSectionNameOfSymbol(sym));

        uint32_t instruction_offset = relas[i].r_offset;
        int64_t addend = relas[i].r_addend;
        CHECK_GE(addend, 0);
        CHECK_LT(addend, 1 << 21);
        uint32_t literal_offset = rodata_offset + addend;
        CHECK_LT(literal_offset, code_.size());
        CHECK_LT(instruction_offset, literal_offset) << object_file.GetSectionNameOfSymbol(sym);

        uint32_t offset = literal_offset - instruction_offset;
        uint32_t immlo = offset % 4;
        uint32_t immhi = offset / 4;
        CHECK_LT(immhi, 1u << 18)
            << "High part of literal offset can't fit in a 19 bit signed immediate.";

        uint32_t inst = LoadInstruction(code_, instruction_offset);
        CHECK((inst & kAdrMask) == kAdrOpcode)
            << fmt::format("Instruction ({:08x}) is not an ADR.", inst);
        CHECK((inst & kAdrImmMask) == 0)
            << fmt::format("Non-zero immediate offset in ADR instruction ({:08x}).", inst);
        inst |= immlo << kAdrImmLoShift;
        inst |= immhi << kAdrImmHiShift;
        StoreInstruction(code_, instruction_offset, inst);
        break;
      }
      case llvm::ELF::R_AARCH64_CALL26: {
        uint32_t sym = relas[i].getSymbol(false);
        llvm::Function* function = GetFunction();
        std::string_view symbol_name = object_file.GetSymbolName(sym);
        std::string_view function_name = function->getName();
        CHECK_EQ(symbol_name, function_name);

        uint32_t instruction_offset = relas[i].r_offset;
        uint32_t inst = LoadInstruction(code_, instruction_offset);
        CHECK(inst == kBlOpcode) << fmt::format("Instruction ({:08x}) is not BL with offset 0.",
                                                inst);
        // We need to branch to the frame entry of the function, which starts at the stack overflow
        // check, after a potential clinit check.
        uint32_t frame_entry_offset = GetFrameEntryOffset(function);
        CHECK_GE(instruction_offset, frame_entry_offset);
        // NOTE: We need a negative offset.
        uint32_t positive_offset = instruction_offset - frame_entry_offset;
        CHECK_ALIGNED(positive_offset, 1u << kBlImmScaleShift);
        uint32_t offset = -(positive_offset >> kBlImmScaleShift);
        CHECK_EQ(offset | ((uint32_t{1} << kBlImmWidth) - 1), 0xffffffff)
            << "BL offset cannot fit into 26 bits.";
        uint32_t imm26 = offset & ((uint32_t{1} << kBlImmWidth) - 1);
        inst |= imm26;
        StoreInstruction(code_, instruction_offset, inst);
        break;
      }
      default:
        LOG(FATAL) << "TODO: Unhandled relocation type: " << relas[i].getType(false);
        break;
    }
  }
}

static bool NeedsVregInfo(HInstruction* instruction) {
  HGraph* graph = instruction->GetBlock()->GetGraph();
  return instruction->IsDeoptimize() || graph->IsDebuggable() || graph->HasMonitorOperations() ||
         instruction->CanThrowIntoCatchBlock();
}

void CodeGeneratorARM64LLVM::ParseStackMap(ELFFileParser& object_file,
                                           uint32_t frame_size,
                                           int64_t cfa_offset) {
  using ELFT = ELFFileParser::ELFT;
  using SMP = llvm::StackMapParser<ELFT::Endianness>;

  llvm::ArrayRef<uint8_t> stack_map_contents =
      object_file.GetSectionContentsOrEmpty(".llvm_stackmaps");
  if (stack_map_contents.empty()) {
    return;
  }

  StackMapStream* stack_map_stream = GetStackMapStream();

  SMP stack_map_parser(stack_map_contents);
  CHECK_EQ(stack_map_parser.getVersion(), 3u);

  auto functions = stack_map_parser.functions();
  DCHECK_EQ(([&]() {
              int count = 0;
              for (auto& _ : functions)
                ++count;
              return count;
            }()),
            1)
      << "More than one functions found in stack map.";
  SMP::FunctionAccessor& function = *functions.begin();
  CHECK_EQ(function.getStackSize(), frame_size);

  auto add_pc_relative_patches = [&](const DexFile* dex_file,
                                     uint32_t offset_or_index,
                                     uint32_t inst_offset,
                                     uint32_t adrp_offset,
                                     ArenaDeque<PcRelativePatchInfo>* patches) {
    NewPcRelativePatch(dex_file,
                       offset_or_index,
                       adrp_offset,
                       /* adrp_offset= */ std::nullopt,
                       patches);
    NewPcRelativePatch(dex_file, offset_or_index, inst_offset, adrp_offset, patches);
  };

  ArenaVector<uint32_t> catch_block_addresses = ParseCatchBlockAddresses(object_file);

  struct CatchStackMapInfo {
    HInstruction* instruction;
    uint32_t native_pc;
    ArenaVector<DexRegisterLocation> environment_locations;
  };

  ArenaVector<CatchStackMapInfo> catch_stack_map_infos(
      GetGraph()->GetAllocator()->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator));
  // Locations of allocas that contain heap references.
  llvm::SmallDenseMap<uint32_t, uint32_t> gc_pointer_stack_offsets;
  for (SMP::RecordAccessor& record : stack_map_parser.records()) {
    uint32_t native_pc = record.getInstructionOffset();
    uint64_t id = record.getID();
    PatchpointKind kind = DecodePatchpointKindFromID(id);

    if (kind == PatchpointKind::kTryBoundaryStackReadClobber) {
      // Nothing to do.
      continue;
    }

    // Collect the offsets of stack slots that can contain heap references.
    // TODO: This should be the first entry in the stack map. We should check that this is indeed
    // the case.
    if (kind == PatchpointKind::kGCPointerAllocaMap) {
      DCHECK_EQ(record.getNumLocations() % 2, 0);
      size_t num_locations = record.getNumLocations();
      for (size_t i = 0; i < num_locations; i += 2) {
        const SMP::LocationAccessor& alloca_location = record.getLocation(i);
        CHECK(alloca_location.getKind() == SMP::LocationKind::Direct);
        uint32_t register_number = alloca_location.getDwarfRegNum();
        CHECK(register_number == 29u || register_number == 31u);
        uint32_t offset;

        if (register_number == 29u) {
          CHECK_LT(alloca_location.getOffset(), 0);
          int64_t fp_offset = static_cast<int64_t>(alloca_location.getOffset());
          CHECK_GE(fp_offset - cfa_offset + static_cast<int64_t>(frame_size), 0);
          offset = static_cast<uint32_t>(fp_offset - cfa_offset + frame_size);
        } else {
          CHECK_GE(alloca_location.getOffset(), 0);
          offset = static_cast<uint32_t>(alloca_location.getOffset());
        }

        const SMP::LocationAccessor& instruction_id_location = record.getLocation(i + 1);
        CHECK(instruction_id_location.getKind() == SMP::LocationKind::Constant);
        uint32_t instruction_id = static_cast<uint32_t>(instruction_id_location.getSmallConstant());
        gc_pointer_stack_offsets.insert({instruction_id, offset});
      }
      continue;
    }

    // Patchpoint decoding may consume some locations for their arguments.
    SMP::RecordAccessor::location_iterator locations_it = record.location_begin();
    SMP::RecordAccessor::location_iterator locations_end = record.location_end();

    if (kind == PatchpointKind::kNone || kind == PatchpointKind::kCriticalNativeCall ||
        kind == PatchpointKind::kImplicitSuspendCheck ||
        kind == PatchpointKind::kEntrypointThunkCallStatepoint) {
      // Calling convention ID.
      CHECK(locations_it != locations_end);
      CHECK(locations_it->getKind() == SMP::LocationKind::Constant);
      ++locations_it;
      // Statepoint flags.
      CHECK(locations_it != locations_end);
      CHECK(locations_it->getKind() == SMP::LocationKind::Constant);
      ++locations_it;
    }

    uint32_t index = DecodePatchpointIndexFromID(id);
    DCHECK_LT(index, stack_map_infos_.size());
    stack_map_infos_[index].is_used = true;
    const StackMapInfo& stack_map_info = stack_map_infos_[index];
    HInstruction* instruction = stack_map_info.instruction;
    uint32_t dex_pc = instruction == nullptr ? 0 : instruction->GetDexPc();

    auto is_valid_register = [](uint32_t reg_num) -> bool {
      return reg_num <= 31 /* General purpose registers. */ ||
             (reg_num >= 64 && reg_num <= 95) /* Floating point registers */;
    };

    auto get_one_register = [&]() -> uint32_t {
      CHECK(locations_it != locations_end)
          << "Missing register location from patchpoint (id=" << id << ")";
      CHECK(locations_it->getKind() == SMP::LocationKind::Register);
      uint32_t reg = locations_it->getDwarfRegNum();
      CHECK(is_valid_register(reg)) << "Invalid dwarf register number: " << reg;
      ++locations_it;
      return reg;
    };

    auto get_two_registers = [&]() -> std::pair<uint32_t, uint32_t> {
      CHECK(locations_it != locations_end)
          << "Missing register location from patchpoint (id=" << id << ")";
      CHECK(locations_it->getKind() == SMP::LocationKind::Register);
      uint32_t first_register = locations_it->getDwarfRegNum();
      CHECK(is_valid_register(first_register))
          << "Invalid dwarf register number: " << first_register;
      ++locations_it;

      CHECK(locations_it != locations_end)
          << "Missing register location from patchpoint (id=" << id << ")";
      CHECK(locations_it->getKind() == SMP::LocationKind::Register);
      uint32_t second_register = locations_it->getDwarfRegNum();
      CHECK(is_valid_register(second_register))
          << "Invalid dwarf register number: " << second_register;
      ++locations_it;

      return std::make_pair(first_register, second_register);
    };

    auto get_constant = [&]() -> uint32_t {
      CHECK(locations_it != locations_end)
          << "Missing constant location from patchpoint (id=" << id << ")";
      CHECK(locations_it->getKind() == SMP::LocationKind::Constant);
      uint32_t constant = locations_it->getSmallConstant();
      ++locations_it;
      return constant;
    };

    bool needs_stack_map_entry = true;
    // Check if the record needs patching.
    switch (kind) {
      case PatchpointKind::kNone:
        // Nothing to do.
        break;
      case PatchpointKind::kGCPointerAllocaMap:
      case PatchpointKind::kTryBoundaryStackReadClobber:
        LOG(FATAL) << "Unreachable";
        UNREACHABLE();
      case PatchpointKind::kImplicitSuspendCheck: {
        native_pc -= kImplicitSuspendCheckPatchSize;
        // ldr x21, [x21]
        const uint32_t inst = CreateLdr64(kImplicitSuspendCheckRegister.GetCode(),
                                          kImplicitSuspendCheckRegister.GetCode());
        StoreInstruction(code_, native_pc, inst);
        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kReachabilityFence: {
        // Discard the recorded heap reference.
        DCHECK(locations_it != locations_end);
        ++locations_it;
        DCHECK(locations_it == locations_end);
        needs_stack_map_entry = false;
        break;
      }
      case PatchpointKind::kCriticalNativeCall: {
        uint32_t frame_increase_size = stack_map_info.index_or_offset;
        DCHECK_NE(frame_increase_size, 0u);

        // The PC value in the stack map points to the end of the call, so we need to adjust it to
        // the beginning.
        native_pc -= kCriticalNativePatchSize;

        // sub sp, sp, #size
        const uint32_t sub_sp_inst =
            CreateSub64Immediate(kSpRegisterNumber, kSpRegisterNumber, frame_increase_size);
        StoreInstruction(code_, native_pc, sub_sp_inst);
        native_pc += vixl::aarch64::kInstructionSize;

        // blr lr
        const uint32_t blr_inst = CreateBlr(kCriticalNativeFunctionRegister);
        StoreInstruction(code_, native_pc, blr_inst);
        native_pc += vixl::aarch64::kInstructionSize;
        uint32_t call_native_pc = native_pc;

        // add sp, sp, #size
        const uint32_t add_sp_inst =
            CreateAdd64Immediate(kSpRegisterNumber, kSpRegisterNumber, frame_increase_size);
        StoreInstruction(code_, native_pc, add_sp_inst);
        native_pc += vixl::aarch64::kInstructionSize;

        native_pc = call_native_pc;
        break;
      }
      case PatchpointKind::kLoadGcRoot:
      case PatchpointKind::kLoadBoolean:
      case PatchpointKind::kLoadInt8:
      case PatchpointKind::kLoadInt16:
      case PatchpointKind::kLoadInt32:
      case PatchpointKind::kLoadInt64: {
        auto [destination_register, address_register] = get_two_registers();
        CHECK_NE(address_register, kSpRegCode)
            << "sp used as the address argument to a load patchpoint";
        const uint32_t offset = get_constant();
        // ldr[b|h] (w|x)A, [xB, #offset]
        const uint32_t inst = CreateLdr(kind, destination_register, address_register, offset);
        StoreInstruction(code_, native_pc, inst);
        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kLoadAcquireGcRoot:
      case PatchpointKind::kLoadAcquireBoolean:
      case PatchpointKind::kLoadAcquireInt8:
      case PatchpointKind::kLoadAcquireInt16:
      case PatchpointKind::kLoadAcquireInt32:
      case PatchpointKind::kLoadAcquireInt64: {
        auto [destination_register, address_register] = get_two_registers();
        CHECK_NE(address_register, kSpRegCode)
            << "sp used as the address argument to a load patchpoint";
        // ldar[b|h] (w|x)A, [xB]
        const uint32_t inst = CreateLdr(kind, destination_register, address_register);
        StoreInstruction(code_, native_pc, inst);
        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kLoadFloat32:
      case PatchpointKind::kLoadFloat64: {
        auto [destination_register, address_register] = get_two_registers();
        CHECK_NE(address_register, kSpRegCode)
            << "sp used as the address argument to a load patchpoint";
        // v0-v31 registers are encoded in the range 64-95
        DCHECK_GE(destination_register, 64u);
        DCHECK_LE(destination_register, 95u);
        destination_register -= 64;
        const uint32_t offset = get_constant();
        // ldr (s|d)A, [xB, #offset]
        const uint32_t inst = CreateLdr(kind, destination_register, address_register, offset);
        StoreInstruction(code_, native_pc, inst);
        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kDiscardedLoad: {
        auto address_register = get_one_register();
        // ldr wzr, [xA]
        const uint32_t inst = CreateLdr32(kWzrRegisterNumber, address_register);
        StoreInstruction(code_, native_pc, inst);
        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kStoreGcRoot:
      case PatchpointKind::kStoreBoolean:
      case PatchpointKind::kStoreInt8:
      case PatchpointKind::kStoreInt16:
      case PatchpointKind::kStoreInt32:
      case PatchpointKind::kStoreInt64: {
        auto [value_register, address_register] = get_two_registers();
        CHECK_NE(address_register, kSpRegCode)
            << "sp used as the address argument to a store patchpoint";
        const uint32_t offset = get_constant();
        // str[b|h] (w|x)A, [xB, #offset]
        const uint32_t inst = CreateStr(kind, value_register, address_register, offset);
        StoreInstruction(code_, native_pc, inst);
        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kStoreReleaseGcRoot:
      case PatchpointKind::kStoreReleaseBoolean:
      case PatchpointKind::kStoreReleaseInt8:
      case PatchpointKind::kStoreReleaseInt16:
      case PatchpointKind::kStoreReleaseInt32:
      case PatchpointKind::kStoreReleaseInt64: {
        auto [value_register, address_register] = get_two_registers();
        CHECK_NE(address_register, kSpRegCode)
            << "sp used as the address argument to a store patchpoint";
        // stlr[b|h] (w|x)A, [xB]
        const uint32_t inst = CreateStr(kind, value_register, address_register);
        StoreInstruction(code_, native_pc, inst);
        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kStoreFloat32:
      case PatchpointKind::kStoreFloat64: {
        auto [value_register, address_register] = get_two_registers();
        CHECK_NE(address_register, kSpRegCode)
            << "sp used as the address argument to a store patchpoint";
        // v0-v31 registers are encoded in the range 64-95
        DCHECK_GE(value_register, 64u);
        DCHECK_LE(value_register, 95u);
        value_register -= 64;
        const uint32_t offset = get_constant();
        // str (s|d)A, [xB, #offset]
        const uint32_t inst = CreateStr(kind, value_register, address_register, offset);
        StoreInstruction(code_, native_pc, inst);
        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kLoadClassBootImageLinkTimePcRelative:
      case PatchpointKind::kLoadMethodBootImageLinkTimePcRelative:
      case PatchpointKind::kLoadStringBootImageLinkTimePcRelative:
      case PatchpointKind::kLoadClassBootImageIntrinsic: {
        uint32_t destination_register = get_one_register();

        // This patchpoint doesn't need a stack map entry.
        CHECK(locations_it == locations_end);
        needs_stack_map_entry = false;

        // adrp xA, #0x0
        const uint32_t adrp_inst = CreateAdrp(destination_register);
        StoreInstruction(code_, native_pc, adrp_inst);
        uint32_t adrp_offset = native_pc;
        native_pc += vixl::aarch64::kInstructionSize;

        // add xA, xA, #0
        const uint32_t add_inst = CreateAdd64Immediate(destination_register, destination_register);
        StoreInstruction(code_, native_pc, add_inst);
        uint32_t add_offset = native_pc;
        native_pc += vixl::aarch64::kInstructionSize;

        ArenaDeque<PcRelativePatchInfo>* patches = nullptr;
        switch (DecodePatchpointKindFromID(id)) {
          case PatchpointKind::kLoadClassBootImageLinkTimePcRelative:
            patches = &boot_image_type_patches_;
            break;
          case PatchpointKind::kLoadClassBootImageIntrinsic:
            patches = &boot_image_other_patches_;
            break;
          case PatchpointKind::kLoadStringBootImageLinkTimePcRelative:
            patches = &boot_image_string_patches_;
            break;
          case PatchpointKind::kLoadMethodBootImageLinkTimePcRelative:
            patches = &boot_image_method_patches_;
            break;
          default:
            LOG(FATAL) << "Unreachable";
            UNREACHABLE();
        }
        add_pc_relative_patches(stack_map_info.dex_file,
                                stack_map_info.index_or_offset,
                                add_offset,
                                adrp_offset,
                                patches);
        break;
      }
      case PatchpointKind::kLoadClassBootImageRelRo:
      case PatchpointKind::kLoadClassAppImageRelRo:
      case PatchpointKind::kLoadClassBssEntry:
      case PatchpointKind::kLoadClassBssEntryPublic:
      case PatchpointKind::kLoadClassBssEntryPackage:
      case PatchpointKind::kLoadStringBootImageRelRo:
      case PatchpointKind::kLoadStringBssEntry:
      case PatchpointKind::kLoadMethodTypeBssEntry:
      case PatchpointKind::kLoadMethodBootImageRelRo:
      case PatchpointKind::kLoadMethodAppImageRelRo:
      case PatchpointKind::kLoadMethodBootImageJni:
      case PatchpointKind::kLoadMethodBssEntry: {
        uint32_t destination_register = get_one_register();

        // This patchpoint doesn't need a stack map entry.
        CHECK(locations_it == locations_end);
        needs_stack_map_entry = false;

        // adrp xA, #0x0
        const uint32_t adrp_inst = CreateAdrp(destination_register);
        StoreInstruction(code_, native_pc, adrp_inst);
        uint32_t adrp_offset = native_pc;
        native_pc += vixl::aarch64::kInstructionSize;

        int load_size = GetPatchpointAdrpLoadSize(DecodePatchpointKindFromID(id));
        if (load_size == 32) {
          // ldr wA, [xA]
          const uint32_t ldr_inst = CreateLdr32(destination_register, destination_register);
          StoreInstruction(code_, native_pc, ldr_inst);
        } else {
          DCHECK_EQ(load_size, 64);
          // ldr xA, [xA]
          const uint32_t ldr_inst = CreateLdr64(destination_register, destination_register);
          StoreInstruction(code_, native_pc, ldr_inst);
        }
        uint32_t ldr_offset = native_pc;
        native_pc += vixl::aarch64::kInstructionSize;

        ArenaDeque<PcRelativePatchInfo>* patches = nullptr;
        switch (DecodePatchpointKindFromID(id)) {
          case PatchpointKind::kLoadClassBootImageRelRo:
          case PatchpointKind::kLoadStringBootImageRelRo:
          case PatchpointKind::kLoadMethodBootImageRelRo: {
            patches = &boot_image_other_patches_;
            break;
          }
          case PatchpointKind::kLoadClassBssEntry:
            patches = &type_bss_entry_patches_;
            break;
          case PatchpointKind::kLoadClassBssEntryPublic:
            patches = &public_type_bss_entry_patches_;
            break;
          case PatchpointKind::kLoadClassBssEntryPackage:
            patches = &package_type_bss_entry_patches_;
            break;
          case PatchpointKind::kLoadStringBssEntry:
            patches = &string_bss_entry_patches_;
            break;
          case PatchpointKind::kLoadMethodTypeBssEntry:
            patches = &method_type_bss_entry_patches_;
            break;
          case PatchpointKind::kLoadMethodBssEntry:
            patches = &method_bss_entry_patches_;
            break;
          case PatchpointKind::kLoadMethodBootImageJni:
            patches = &boot_image_jni_entrypoint_patches_;
            break;
          case PatchpointKind::kLoadClassAppImageRelRo:
            patches = &app_image_type_patches_;
            break;
          case PatchpointKind::kLoadMethodAppImageRelRo:
            patches = &app_image_method_patches_;
            break;
          default:
            LOG(FATAL) << "Unreachable";
            UNREACHABLE();
        }
        add_pc_relative_patches(stack_map_info.dex_file,
                                stack_map_info.index_or_offset,
                                ldr_offset,
                                adrp_offset,
                                patches);
        break;
      }
      case PatchpointKind::kEntrypointThunkCallStatepoint: {
        native_pc -= vixl::aarch64::kInstructionSize;
        // bl #0
        const uint32_t inst = CreateBl(0);
        StoreInstruction(code_, native_pc, inst);

        PatchInfo<uint64_t>& entrypoint_patch =
            call_entrypoint_patches_.emplace_back(nullptr, stack_map_info.index_or_offset);
        entrypoint_patch.label = native_pc;

        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kEntrypointThunkCallPatchpoint: {
        CHECK(locations_it == locations_end);
        needs_stack_map_entry = false;
        // bl #0
        const uint32_t inst = CreateBl(0);
        StoreInstruction(code_, native_pc, inst);

        PatchInfo<uint64_t>& entrypoint_patch =
            call_entrypoint_patches_.emplace_back(nullptr, stack_map_info.index_or_offset);
        entrypoint_patch.label = native_pc;

        native_pc += vixl::aarch64::kInstructionSize;
        break;
      }
      case PatchpointKind::kCatchBlock: {
        needs_stack_map_entry = false;

        HEnvironment* environment = instruction->GetEnvironment();
        ArenaVector<DexRegisterLocation> env_locations(catch_stack_map_infos.get_allocator());
        env_locations.reserve(environment->Size());
        while (environment != nullptr) {
          for (size_t i = 0, environment_size = environment->Size(); i < environment_size; ++i) {
            using Kind = DexRegisterLocation::Kind;
            HInstruction* value = environment->GetInstructionAt(i);
            if (value == nullptr) {
              env_locations.emplace_back(Kind::kNone, 0);
              continue;
            } else if (value->IsParameterValue()) {
              // Parameter value locations aren't recorded in the patchpoint.
              size_t parameter_index = GetIndexOfParameterValue(value->AsParameterValue());
              uint32_t offset = GetParameterStackOffset(parameter_index, frame_size);
              env_locations.emplace_back(Kind::kInStack, offset);
              if (DataType::Is64BitType(value->GetType())) {
                env_locations.emplace_back(Kind::kInStack, offset + kVRegSize);
                ++i;
                DCHECK_LT(i, environment_size);
              }
              continue;
            }

            CHECK(locations_it != locations_end);
            SMP::LocationAccessor& location = *locations_it;
            switch (location.getKind()) {
              case SMP::LocationKind::Register:
              case SMP::LocationKind::Indirect:
                LOG(FATAL) << "Invalid location kind: " << static_cast<int>(location.getKind());
                UNREACHABLE();
              case SMP::LocationKind::Direct: {
                int32_t register_number = location.getDwarfRegNum();
                CHECK(register_number == 29u || register_number == 31u)
                    << "Catch block environment location is not given relative to sp";
                uint32_t offset;
                if (register_number == 29u) {
                  CHECK_LT(location.getOffset(), 0);
                  int64_t fp_offset = static_cast<int64_t>(location.getOffset());
                  CHECK_GE(fp_offset - cfa_offset + static_cast<int64_t>(frame_size), 0);
                  offset = static_cast<uint32_t>(fp_offset - cfa_offset + frame_size);
                } else {
                  CHECK_GE(location.getOffset(), 0) << "Negative stack offset found";
                  offset = static_cast<uint32_t>(location.getOffset());
                }
                env_locations.emplace_back(Kind::kInStack, offset);
                if (DataType::Is64BitType(value->GetType())) {
                  env_locations.emplace_back(Kind::kInStack, offset + kVRegSize);
                  ++i;
                  DCHECK_LT(i, environment_size);
                }
                break;
              }
              case SMP::LocationKind::Constant: {
                uint32_t constant = location.getSmallConstant();
                env_locations.emplace_back(Kind::kConstant, Low32Bits(constant));
                if (DataType::Is64BitType(value->GetType())) {
                  env_locations.emplace_back(Kind::kConstant, High32Bits(constant));
                  ++i;
                  DCHECK_LT(i, environment_size);
                }
                break;
              }
              case SMP::LocationKind::ConstantIndex: {
                uint32_t constant_index = location.getConstantIndex();
                uint64_t constant = stack_map_parser.getConstant(constant_index).getValue();
                CHECK(DataType::Is64BitType(value->GetType()))
                    << "Constant type is expected to be 64 bits wide";
                env_locations.emplace_back(Kind::kConstant, Low32Bits(constant));
                env_locations.emplace_back(Kind::kConstant, High32Bits(constant));
                ++i;
                DCHECK_LT(i, environment_size);
                break;
              }
            }
            ++locations_it;
          }
          environment = environment->GetParent();
        }
        DCHECK_LT(stack_map_info.index_or_offset, catch_block_addresses.size());
        uint32_t catch_block_address = catch_block_addresses[stack_map_info.index_or_offset];
        catch_stack_map_infos.push_back(
            CatchStackMapInfo{instruction, catch_block_address, std::move(env_locations)});
        break;
      }
    }

    if (!needs_stack_map_entry) {
      continue;
    }

    // Copy the logic from CodeGenerator::RecordPcInfo for when to generate a stack map entry.
    if (instruction != nullptr) {
      if (instruction->IsTypeConversion()) {
        continue;
      }
      if (instruction->IsRem()) {
        DataType::Type type = instruction->AsRem()->GetResultType();
        if ((type == DataType::Type::kFloat32) || (type == DataType::Type::kFloat64)) {
          continue;
        }
      }
    }

    if (instruction == nullptr) {
      // Stack overflow checks and native-debug-info entries.
      stack_map_stream->BeginStackMapEntry(dex_pc, native_pc);
      stack_map_stream->EndStackMapEntry();
      continue;
    }

    uint32_t register_mask = 0;
    BitVector* stack_mask =
        ArenaBitVector::Create(GetGraph()->GetAllocator(), 0, true, kArenaAllocCodeGenerator);

    // Number of deopt values.
    CHECK(locations_it != locations_end);
    CHECK(locations_it->getKind() == SMP::LocationKind::Constant);
    uint32_t number_of_deopt_values = locations_it->getSmallConstant();
    ++locations_it;

    bool needs_vreg_info = NeedsVregInfo(instruction);
    ArenaVector<DexRegisterLocation> deopt_values(
        GetGraph()->GetAllocator()->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator));
    if (needs_vreg_info) {
      HEnvironment* environment = instruction->GetEnvironment();
      DCHECK(environment != nullptr)
          << "Instruction needs vreg info, but doesn't have an environment";
      // In general this is not the exact final size of deopt_values, but it is a good estimate.
      deopt_values.reserve(environment->Size());
      uint32_t locations_consumed = 0;
      while (environment != nullptr) {
        for (size_t i = 0, environment_size = environment->Size(); i < environment_size; ++i) {
          using Kind = DexRegisterLocation::Kind;
          HInstruction* current = environment->GetInstructionAt(i);
          // NOTE: We don't filter out parameters here, because we're not dealing with stack slots.
          if (current == nullptr) {
            deopt_values.emplace_back(Kind::kNone, 0);
            continue;
          }

          CHECK(locations_it != locations_end);
          SMP::LocationAccessor& location = *locations_it;
          switch (location.getKind()) {
            case SMP::LocationKind::Register: {
              uint16_t register_num = location.getDwarfRegNum();
              if (register_num <= 31) {
                // General purpose registers x0-x30 + sp.
                deopt_values.emplace_back(DexRegisterLocation::Kind::kInRegister, register_num);
                if (current->GetType() == DataType::Type::kInt64) {
                  deopt_values.emplace_back(Kind::kInRegisterHigh, register_num);
                  ++i;
                  DCHECK_LT(i, environment_size);
                }
                // Record reference location for the stack map.
                if (current->GetType() == DataType::Type::kReference) {
                  register_mask |= uint32_t{1} << register_num;
                }
              } else if (register_num >= 64 && register_num <= 95) {
                // FP registers v0-v31.
                uint32_t actual_register_num = register_num - 64;
                deopt_values.emplace_back(DexRegisterLocation::Kind::kInFpuRegister,
                                          actual_register_num);
                if (current->GetType() == DataType::Type::kFloat64) {
                  deopt_values.emplace_back(Kind::kInFpuRegisterHigh, actual_register_num);
                  ++i;
                  DCHECK_LT(i, environment_size);
                }
              } else {
                LOG(FATAL) << "Unknown dwarf register number " << register_num;
              }
              break;
            }
            // NOTE: We handle Direct and Indirect locations the same way, to have the ability to
            // use 'alloca' values in deopt bundles.
            case SMP::LocationKind::Direct:
            case SMP::LocationKind::Indirect: {
              uint32_t register_number = location.getDwarfRegNum();
              CHECK(register_number == 29u || register_number == 31u)
                  << "Direct/Indirect location is not given relative to sp";
              uint32_t offset;
              if (register_number == 29u) {
                CHECK_LT(location.getOffset(), 0);
                int64_t fp_offset = static_cast<int64_t>(location.getOffset());
                CHECK_GE(fp_offset - cfa_offset + static_cast<int64_t>(frame_size), 0);
                offset = static_cast<uint32_t>(fp_offset - cfa_offset + frame_size);
              } else {
                CHECK_GE(location.getOffset(), 0);
                offset = static_cast<uint32_t>(location.getOffset());
              }
              deopt_values.emplace_back(Kind::kInStack, offset);
              if (DataType::Is64BitType(current->GetType())) {
                deopt_values.emplace_back(Kind::kInStack, offset + kVRegSize);
                ++i;
                DCHECK_LT(i, environment_size);
              }
              // Record reference location for the stack map.
              if (current->GetType() == DataType::Type::kReference) {
                CHECK_GE(offset, 0u);
                stack_mask->SetBit(static_cast<uint32_t>(offset) / kVRegSize);
              }
              break;
            }
            case SMP::LocationKind::Constant: {
              uint32_t constant = location.getSmallConstant();
              deopt_values.emplace_back(Kind::kConstant, Low32Bits(constant));
              if (DataType::Is64BitType(current->GetType())) {
                deopt_values.emplace_back(Kind::kConstant, High32Bits(constant));
                ++i;
                DCHECK_LT(i, environment_size);
              }
              break;
            }
            case SMP::LocationKind::ConstantIndex: {
              uint32_t constant_index = location.getConstantIndex();
              uint64_t constant = stack_map_parser.getConstant(constant_index).getValue();
              deopt_values.emplace_back(Kind::kConstant, Low32Bits(constant));
              if (DataType::Is64BitType(current->GetType())) {
                deopt_values.emplace_back(Kind::kConstant, High32Bits(constant));
                ++i;
                DCHECK_LT(i, environment_size);
              } else {
                CHECK_EQ(High32Bits(constant), 0u);
              }
              break;
            }
          }

          ++locations_it;
          ++locations_consumed;
        }
        environment = environment->GetParent();
      }
      CHECK_EQ(locations_consumed, number_of_deopt_values);
    }

    // Iterate through the rest of the locations
    for (; locations_it != locations_end; ++locations_it) {
      SMP::LocationAccessor& location = *locations_it;
      switch (location.getKind()) {
        case SMP::LocationKind::Constant:
        case SMP::LocationKind::ConstantIndex:
          // Nothing to do.
          break;
        case SMP::LocationKind::Register:
          CHECK_LT(location.getDwarfRegNum(), 32);
          register_mask |= uint32_t{1} << location.getDwarfRegNum();
          break;
        // Direct locations represent heap references saved to the stack because they were needed
        // to be saved for a catch block. They should be treated identically to Indirect locations.
        case SMP::LocationKind::Direct:
        case SMP::LocationKind::Indirect: {
          CHECK_EQ(location.getDwarfRegNum(), 31)
              << "Direct/Indirect location is not given relative to sp";
          int32_t offset = location.getOffset();
          CHECK_GE(offset, 0);
          stack_mask->SetBit(static_cast<uint32_t>(offset) / kVRegSize);
          break;
        }
      }
    }

    if (!all_stack_saved_values_.empty()) {
      auto add_value_to_stack_mask = [&](HInstruction* value) {
        if (value->GetType() != DataType::Type::kReference) {
          return;
        }

        uint32_t offset = -1;
        if (value->IsParameterValue()) {
          size_t parameter_index = GetIndexOfParameterValue(value->AsParameterValue());
          // The parameter stack slot may not actually be used, even though we recorded it as live.
          if (!IsParameterStackSlotUsed(parameter_index)) {
            return;
          }
          offset = GetParameterStackOffset(parameter_index, frame_size);
        } else {
          auto it = gc_pointer_stack_offsets.find(value->GetId());
          if (it == gc_pointer_stack_offsets.end()) {
            return;
          }

          offset = it->second;
        }
        DCHECK_ALIGNED(offset, kVRegSize);
        stack_mask->SetBit(offset / kVRegSize);
      };

      const BasicBlockInfo& block_info = GetBasicBlockInfo(instruction->GetBlock());
      for (HInstruction* value : block_info.live_stack_saved_values) {
        add_value_to_stack_mask(value);
      }

      auto instruction_index_it = block_info.instruction_index_map.find(instruction);
      DCHECK(instruction_index_it != block_info.instruction_index_map.end());
      uint32_t instruction_index = instruction_index_it->second;
      for (auto [value, last_use_index] : block_info.partially_live_stack_saved_values) {
        if (last_use_index >= instruction_index) {
          add_value_to_stack_mask(value);
        }
      }
    }

    uint32_t outer_dex_pc = dex_pc;
    HEnvironment* const environment = instruction->GetEnvironment();
    if (environment != nullptr) {
      HEnvironment* outer_environment = environment;
      while (outer_environment->GetParent() != nullptr) {
        outer_environment = outer_environment->GetParent();
      }
      outer_dex_pc = outer_environment->GetDexPc();
    }

    stack_map_stream->BeginStackMapEntry(outer_dex_pc,
                                         native_pc,
                                         register_mask,
                                         stack_mask,
                                         StackMap::Kind::Default,
                                         needs_vreg_info);
    EmitEnvironment(
        environment, ArrayRef<const DexRegisterLocation>(deopt_values), needs_vreg_info);
    stack_map_stream->EndStackMapEntry();
  }

  for (const CatchStackMapInfo& info : catch_stack_map_infos) {
    HBasicBlock* block = info.instruction->GetBlock();
    DCHECK(block->IsCatchBlock());
    // Get the outer dex_pc. We save the full environment list for DCHECK purposes in kIsDebugBuild.
    std::vector<uint32_t> dex_pc_list_for_verification;
    if (kIsDebugBuild) {
      dex_pc_list_for_verification.push_back(block->GetDexPc());
    }
    HEnvironment* const environment = info.instruction->GetEnvironment();
    DCHECK(environment != nullptr);
    HEnvironment* outer_environment = environment;
    while (outer_environment->GetParent() != nullptr) {
      outer_environment = outer_environment->GetParent();
      if (kIsDebugBuild) {
        dex_pc_list_for_verification.push_back(outer_environment->GetDexPc());
      }
    }

    if (kIsDebugBuild) {
      // dex_pc_list_for_verification is set from innnermost to outermost. Let's reverse it
      // since we are expected to pass from outermost to innermost.
      std::reverse(dex_pc_list_for_verification.begin(), dex_pc_list_for_verification.end());
      DCHECK_EQ(dex_pc_list_for_verification.front(), outer_environment->GetDexPc());
    }

    stack_map_stream->BeginStackMapEntry(outer_environment->GetDexPc(),
                                         info.native_pc,
                                         /* register_mask= */ 0,
                                         /* sp_mask= */ nullptr,
                                         StackMap::Kind::Catch,
                                         /* needs_vreg_info= */ true,
                                         dex_pc_list_for_verification);
    EmitEnvironment(environment,
                    ArrayRef<const DexRegisterLocation>(info.environment_locations),
                    /* needs_vreg_info= */ true,
                    /* is_for_catch_handler= */ true);
    stack_map_stream->EndStackMapEntry();
  }
}

ArenaVector<uint32_t> CodeGeneratorARM64LLVM::ParseCatchBlockAddresses(ELFFileParser& object_file) {
  llvm::SmallVector<uint8_t> catch_block_addresses_array =
      object_file.GetSymbolContentsOrEmpty(kCatchBlockAddressesArrayName);
  DCHECK_ALIGNED(catch_block_addresses_array.size(), 8);

  // The addresses are encoded as an array of 64-bit pointers, which we convert to 32-bit offsets.
  ArenaVector<uint32_t> catch_block_addresses(
      GetGraph()->GetAllocator()->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator));
  catch_block_addresses.reserve(catch_block_addresses_array.size() / 8);

  for (size_t i = 0; i < catch_block_addresses_array.size(); i += 8) {
    uint64_t address = 0;
    for (size_t byte_index = 0; byte_index < sizeof(uint64_t); ++byte_index) {
      address |= static_cast<uint64_t>(catch_block_addresses_array[i + byte_index])
                 << (8 * byte_index);
    }
    DCHECK_LE(address, std::numeric_limits<uint32_t>::max());
    catch_block_addresses.push_back(static_cast<uint32_t>(address));
  }

  return catch_block_addresses;
}

// NOTE: This is a copy of CodeGenerator::EmitEnvironment().
void CodeGeneratorARM64LLVM::EmitEnvironment(
    HEnvironment* environment,
    ArrayRef<const DexRegisterLocation> environment_locations,
    bool needs_vreg_info,
    bool is_for_catch_handler,
    bool innermost_environment) {
  if (environment == nullptr) {
    return;
  }

  StackMapStream* stack_map_stream = GetStackMapStream();
  bool emit_inline_info = environment->GetParent() != nullptr;

  if (emit_inline_info) {
    // We emit the parent environment first.
    size_t environment_size = needs_vreg_info ? environment->Size() : 0;
    EmitEnvironment(environment->GetParent(),
                    environment_locations.SubArray(environment_size),
                    needs_vreg_info,
                    is_for_catch_handler,
                    /* innermost_environment= */ false);
    stack_map_stream->BeginInlineInfoEntry(environment->GetMethod(),
                                           environment->GetDexPc(),
                                           environment_size,
                                           &GetGraph()->GetDexFile(),
                                           this);
  }

  // If a dex register map is not required we just won't emit it.
  if (needs_vreg_info) {
    // The environment locations corresponding to the current environment.
    ArrayRef<const DexRegisterLocation> inner_environment_locations =
        environment_locations.SubArray(0, environment->Size());
    if (innermost_environment && is_for_catch_handler) {
      EmitVRegInfoOnlyCatchPhis(environment, inner_environment_locations);
    } else {
      EmitVRegInfo(environment, inner_environment_locations);
    }
  }

  if (emit_inline_info) {
    stack_map_stream->EndInlineInfoEntry();
  }
}

void CodeGeneratorARM64LLVM::EmitVRegInfo(
    HEnvironment* environment, ArrayRef<const DexRegisterLocation> environment_locations) {
  StackMapStream* stack_map_stream = GetStackMapStream();
  DCHECK_EQ(environment->Size(), environment_locations.size());
  for (DexRegisterLocation deopt_value : environment_locations) {
    stack_map_stream->AddDexRegisterEntry(deopt_value.GetKind(), deopt_value.GetValue());
  }
}

void CodeGeneratorARM64LLVM::EmitVRegInfoOnlyCatchPhis(
    HEnvironment* environment, ArrayRef<const DexRegisterLocation> environment_locations) {
  StackMapStream* stack_map_stream = GetStackMapStream();
  HInstruction* current_phi = environment->GetHolder()->GetBlock()->GetFirstPhi();
  DCHECK_EQ(environment->Size(), environment_locations.size());
  for (size_t vreg = 0; vreg < environment->Size(); ++vreg) {
    while (current_phi != nullptr && current_phi->AsPhi()->GetRegNumber() < vreg) {
      HInstruction* next_phi = current_phi->GetNext();
      DCHECK(next_phi == nullptr ||
             current_phi->AsPhi()->GetRegNumber() <= next_phi->AsPhi()->GetRegNumber())
          << "Phis need to be sorted by vreg number to keep this a linear-time loop.";
      current_phi = next_phi;
    }

    if (current_phi == nullptr || current_phi->AsPhi()->GetRegNumber() != vreg) {
      stack_map_stream->AddDexRegisterEntry(DexRegisterLocation::Kind::kNone, 0);
    } else {
      DataType::Type phi_type = current_phi->GetType();
      DCHECK_EQ(environment_locations[vreg].GetKind(), DexRegisterLocation::Kind::kInStack);
      int32_t offset = environment_locations[vreg].GetStackOffsetInBytes();
      stack_map_stream->AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack, offset);
      if (DataType::Is64BitType(phi_type)) {
        ++vreg;
        DCHECK_LT(vreg, environment->Size());
        constexpr int32_t signed_vreg_size = kVRegSize;
        DCHECK_EQ(environment_locations[vreg].GetKind(), DexRegisterLocation::Kind::kInStack);
        DCHECK_EQ(environment_locations[vreg].GetStackOffsetInBytes(), offset + signed_vreg_size);
        stack_map_stream->AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack,
                                              offset + signed_vreg_size);
      }
    }
  }
}

void CodeGeneratorARM64LLVM::Finalize() {
  // Resolve phis.
  for (const auto [phi, llvm_phi] : phis_) {
    HBasicBlock* phi_block = phi->GetBlock();
    const ArenaVector<HBasicBlock*>& predecessors = phi_block->GetPredecessors();
    DCHECK_EQ(predecessors.size(), phi->InputCount());
    for (size_t i = 0; i < predecessors.size(); ++i) {
      llvm::BasicBlock* source_block = GetLastIRBasicBlock(predecessors[i]);
      SetCurrentBlock(predecessors[i]);
      // Set the insert point to the end of the source block, because GetValue can generate a load
      // if the incoming value is needed in a catch block, and therefore stored on the stack.
      __ SetInsertPoint(source_block->getTerminator());
      llvm::Value* llvm_value = GetValue(phi->InputAt(i));
      if (llvm_phi->getType() != llvm_value->getType()) {
        // Phi nodes can only have int32, int64, float32 or float64 types, so if an incoming value
        // is e.g. boolean, the phi will have type int32 instead. We handle this by converting these
        // values to i32 before adding them to the phi node.
        DCHECK(llvm_value->getType()->isIntegerTy());
        DCHECK_LT(llvm_value->getType()->getIntegerBitWidth(), 32u);
        DCHECK(llvm_phi->getType() == GetInt32Type());
        if (llvm::isa<llvm::Constant>(llvm_value)) {
          // Nothing to do.
        } else if (llvm::isa<llvm::Argument>(llvm_value)) {
          llvm::BasicBlock* entry_block = GetIRBasicBlock(GetGraph()->GetEntryBlock());
          // Insert the new instruction after at the beginning of the entry block.
          __ SetInsertPoint(entry_block->getFirstInsertionPt());
        } else if (llvm::PHINode* llvm_value_phi = llvm::dyn_cast<llvm::PHINode>(llvm_value)) {
          // If the value is a phi, set the insert point after all the phis in its basic block.
          __ SetInsertPoint(llvm_value_phi->getParent()->getFirstInsertionPt());
        } else if (llvm::InvokeInst* invoke = llvm::dyn_cast<llvm::InvokeInst>(llvm_value)) {
          // Insert the new instruction at the beginning of the normal flow successor.
          __ SetInsertPoint(invoke->getNormalDest()->getFirstInsertionPt());
        } else {
          CHECK(llvm::isa<llvm::Instruction>(llvm_value));
          llvm::Instruction* inst = llvm::cast<llvm::Instruction>(llvm_value);
          // Insert the new instruction after `llvm_value`.
          __ SetInsertPoint(inst->getNextNode());
        }
        DataType::Type value_type = phi->InputAt(i)->GetType();
        bool is_signed = !DataType::IsUnsignedType(value_type);
        bool is_originally_constant = llvm::isa<llvm::Constant>(llvm_value);
        llvm_value = __ CreateIntCast(llvm_value, llvm_phi->getType(), is_signed);
        DCHECK_IMPLIES(is_originally_constant, llvm::isa<llvm::Constant>(llvm_value))
            << "IRBuilder didn't constant fold an int cast";
      }
      llvm_phi->addIncoming(llvm_value, source_block);
    }
  }

  for (const StackSavedValuePhiInfo& info : stack_saved_value_phis_) {
    for (HBasicBlock* predecessor : info.block->GetPredecessors()) {
      const ArenaHashMap<HInstruction*, llvm::Value*>& predecessor_value_map =
          GetBasicBlockInfo(predecessor).instruction_value_map;
      llvm::BasicBlock* incoming_block = GetLastIRBasicBlock(predecessor);
      auto it = predecessor_value_map.find(info.value);
      if (it != predecessor_value_map.end()) {
        info.phi->addIncoming(it->second, incoming_block);
      } else {
        auto global_map_it = instruction_value_map_.find(info.value);
        DCHECK(global_map_it != instruction_value_map_.end());
        info.phi->addIncoming(global_map_it->second, incoming_block);
      }
    }
  }

  for (HInstruction* value : all_stack_saved_values_) {
    llvm::BasicBlock* store_insert_block = GetLastIRBasicBlock(value->GetBlock());
    DCHECK(store_insert_block->getTerminator() != nullptr);
    // Place the store before the last instruction in the block.
    __ SetInsertPoint(store_insert_block->getTerminator());
    SetCurrentBlock(value->GetBlock());
    llvm::Value* value_to_store = GetValue(value);
    llvm::Value* alloca = MaybeGetValueAlloca(value);
    DCHECK(alloca != nullptr);
    // Use volatile stores for GC references, to make sure the optimizer doesn't eliminate them.
    bool is_volatile = value->GetType() == DataType::Type::kReference;
    CreateStore(value_to_store, alloca, is_volatile);
  }

  // Add an array containing the catch block addresses.
  {
    llvm::ArrayType* array_type =
        llvm::ArrayType::get(GetPointerType(), catch_block_addresses_.size());
    llvm::Constant* catch_block_address_array_ =
        module_->getOrInsertGlobal(kCatchBlockAddressesArrayName, array_type);
    CHECK(llvm::isa<llvm::GlobalVariable>(catch_block_address_array_));
    llvm::GlobalVariable* catch_block_address_array =
        llvm::cast<llvm::GlobalVariable>(catch_block_address_array_);
    llvm::Constant* init_value =
        llvm::ConstantArray::get(array_type,
                                 llvm::ArrayRef<llvm::Constant*>(catch_block_addresses_.data(),
                                                                 catch_block_addresses_.size()));
    catch_block_address_array->setInitializer(init_value);
    catch_block_address_array->setLinkage(llvm::GlobalVariable::ExternalLinkage);
    catch_block_address_array->setConstant(true);
  }

  // Emit the branch from the alloca block to the entry block.
  __ SetInsertPoint(alloca_block_);
  __ CreateBr(GetIRBasicBlock(GetGraph()->GetEntryBlock()));

  // TODO: Do we need to emit some slow paths here?

  CodeGenerator::Finalize();

  // Heck to print on device:
  // if(llvm::verifyModule(*module_, &llvm::dbgs())) {
  //  std::string error_str;
  //  llvm::raw_string_ostream rso(error_str);
  //  llvm::verifyModule(*module_, &rso);
  //  LOG(INFO) << error_str << "\n";
  //
  //  std::string module_str;
  //  llvm::raw_string_ostream rso2(module_str);
  //  module_->print(rso2, nullptr);
  //  LOG(INFO) << module_str << "\n";
  //}

  CHECK(llvm::verifyModule(*module_, &llvm::dbgs()) == false)
      << "LLVM verifier failed on method " << GetGraph()->PrettyMethod() << (DumpModule(), "");
  RunOptimizerPasses();
  CHECK(llvm::verifyModule(*module_, &llvm::dbgs()) == false)
      << "LLVM verifier failed on method " << GetGraph()->PrettyMethod() << (DumpModule(), "");

  RemovePlaceholderFunctions();

  AddClinitCheckPrologue();

  ObjectMemoryBuffer object_memory_buffer = EmitObjectFile();
  ParseObjectFile(object_memory_buffer);
}

void InstructionCodeGeneratorARM64LLVM::VisitMethodExitHook(
    [[maybe_unused]] HMethodExitHook* instruction) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM64LLVM::VisitMethodEntryHook(
    [[maybe_unused]] HMethodEntryHook* instruction) {
  LOG(FATAL) << "Unreachable";
}

void CodeGeneratorARM64LLVM::MaybeIncrementHotness(bool is_frame_entry) {
  DCHECK(Runtime::Current()->IsAotCompiler());
  if (!GetCompilerOptions().CountHotnessInCompiledCode()) {
    return;
  }

  llvm::Value* method = nullptr;
  if (is_frame_entry) {
    method = GetCurrentMethodPointerArgument();
  } else {
    // __ Ldr(method, MemOperand(sp, 0));
    llvm::Value* sp_value = GetStackPointerValue();
    method = CreateLoad(GetMethodPointerType(), sp_value);
  }
  // __ Ldrh(counter, MemOperand(method, ArtMethod::HotnessCountOffset().Int32Value()));
  llvm::Value* hotness_count_address =
      CreateGEP(method, ArtMethod::HotnessCountOffset().SizeValue());
  llvm::Value* counter = CreateLoad(GetInt16Type(), hotness_count_address);
  llvm::BasicBlock* increment_block = CreateBasicBlock();
  llvm::BasicBlock* done_block = CreateBasicBlock();
  DCHECK_EQ(0u, interpreter::kNterpHotnessValue);
  // __ Cbz(counter, &done);
  llvm::Value* is_counter_zero = __ CreateICmpEQ(counter, GetConstantZero(counter->getType()));
  __ CreateCondBr(is_counter_zero, done_block, increment_block);
  __ SetInsertPoint(increment_block);
  // __ Add(counter, counter, -1);
  llvm::Value* new_counter = __ CreateSub(counter, GetConstantInt(counter->getType(), 1));
  // __ Strh(counter, MemOperand(method, ArtMethod::HotnessCountOffset().Int32Value()));
  CreateStore(new_counter, hotness_count_address);
  // __ Bind(&done);
  __ CreateBr(done_block);
  __ SetInsertPoint(done_block);
}

void CodeGeneratorARM64LLVM::MaybeRecordTraceEvent(bool is_method_entry) {
  if (!art_flags::always_enable_profile_code()) {
    return;
  } else {
    LOG(FATAL) << "MaybeRecordTraceEvent is unimplemented";
    TODO();
    UNUSED(is_method_entry);
  }
}

void CodeGeneratorARM64LLVM::GenerateFrameEntry() {
  DCHECK_EQ(GetFunction()->size(), 0u);
  alloca_block_ = CreateBasicBlock();
  __ SetInsertPoint(alloca_block_);
  // Dummy alloca to prevent LLVM from optimizing simple recursive functions into infinite loops.
  // NOTE: This alloca will be recorded in a @llvm.experimental.stackmap() call in
  // RecordAllocasInStackMapPass.
  // Example:
  // void foo() {
  //   foo();
  // }
  __ CreateAlloca(GetInt32Type());
  // Clear the insertion point, so that the Bind(...) call below sets the insert point to the entry
  // block.
  __ ClearInsertionPoint();

  // NOTE: The branch to the actual entry block is created in CodeGeneratorARM64LLVM::Finalize.
  Bind(GetGraph()->GetEntryBlock());
  MaybeIncrementHotness(/* is_frame_entry= */ true);
  MaybeRecordTraceEvent(/* is_frame_entry= */ true);
}

void CodeGeneratorARM64LLVM::GenerateFrameExit() {
  llvm::BasicBlock* current_block = __ GetInsertBlock();
  DCHECK(current_block != nullptr);
  if (!current_block->empty()) {
    MaybeRecordTraceEvent((/* is_frame_entry= */ false));
  }
  if (current_block->empty() || !current_block->back().isTerminator()) {
    llvm::Type* return_type = GetFunction()->getReturnType();
    if (return_type->isVoidTy()) {
      __ CreateRetVoid();
    } else {
      // TODO: Is this really unreachable?
      LOG(FATAL) << "Unreachable";
      __ CreateRet(llvm::UndefValue::get(return_type));
    }
  }
}

llvm::Type* CodeGeneratorARM64LLVM::GetPhiType(HPhi* phi) const {
  // Phis with vector values also have float64 type, so we have to check for that.
  if (phi->GetType() != DataType::Type::kFloat64) {
    return GetLLVMType(phi->GetType());
  }

  // Do a breadth first search, where we follow the incoming values of every phi node we encounter,
  // and record them in a set to prevent infinite looping and exponential grow of searched
  // instructions. Once we find a non-phi instruction, we have the actual type of the phi node.
  // TODO: What container should we use here? For now let's use LLVM's dense set implementation.
  llvm::SmallDenseSet<HPhi*> visited_phis;
  llvm::SmallVector<HInstruction*, 32> to_visit;

  to_visit.push_back(phi);
  while (!to_visit.empty()) {
    llvm::SmallVector<HInstruction*, 32> next_to_visit;
    for (HInstruction* instruction : to_visit) {
      if (HPhi* visited_phi = instruction->AsPhiOrNull()) {
        const auto [it, inserted] = visited_phis.insert(visited_phi);
        if (!inserted) {
          // We have already seen this phi node, so go to the next instruction.
          continue;
        }

        for (HInstruction* incoming_value : visited_phi->GetInputs()) {
          next_to_visit.push_back(incoming_value);
        }
      } else if (HVecOperation* visited_vec_op = instruction->AsVecOperationOrNull()) {
        // We have found a vector operation, so we can infer the vector type of the phi node.
        size_t vector_length = visited_vec_op->GetVectorLength();
        llvm::Type* packed_type = GetLLVMType(visited_vec_op->GetPackedType());
        return GetVectorType(packed_type, vector_length);
      } else {
        // We have found a non-phi and non-vector node, so we can infer the type from this.
        return GetLLVMType(instruction->GetType());
      }
    }
    to_visit = std::move(next_to_visit);
  }

  LOG(FATAL) << "Unable to infer type of phi node: didn't find any non-phi incoming values";
  return nullptr;
}

void CodeGeneratorARM64LLVM::Bind(HBasicBlock* block) {
  llvm::BasicBlock* new_block = GetIRBasicBlock(block);

  llvm::BasicBlock* current_block = __ GetInsertBlock();
  __ SetInsertPoint(new_block);

  BasicBlockInfo& block_info = GetBasicBlockInfo(block);
  // Inherit instruction value map from the dominator.
  if (HBasicBlock* dominator = block->GetDominator()) {
    block_info.instruction_value_map = GetBasicBlockInfo(dominator).instruction_value_map;
  }

  if (block->IsCatchBlock()) {
    // Save the block address of the catch block, which we will later use in stack map generation.
    uint32_t catch_block_address_index = catch_block_addresses_.size();
    catch_block_addresses_.push_back(llvm::BlockAddress::get(new_block));

    // Create catch phi allocas.
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      AddValueAlloca(phi, GetLLVMType(phi->GetType()));
    }

    DCHECK(block->GetFirstInstruction()->IsNop());
    DCHECK(block->GetFirstInstruction()->AsNop()->NeedsEnvironment());
    HInstruction* nop = block->GetFirstInstruction();
    HEnvironment* const environment = nop->GetEnvironment();
    DCHECK(environment != nullptr);

    llvm::SmallVector<llvm::Value*> env_values;
    HEnvironment* env_it = environment;
    while (env_it != nullptr) {
      for (size_t i = 0, size = env_it->Size(); i < size; ++i) {
        if (HInstruction* value = env_it->GetInstructionAt(i)) {
          // Don't record the allocas of parameter values, because they will be removed later. The
          // appropriate offsets are added to the stack map during the stack map parsing.
          if (value->IsParameterValue()) {
            continue;
          }

          // LLVM seems to put constant float and double values in a patchpoint into registers,
          // which we have to avoid. So instead of using float and double, we bit cast these values
          // to integers, and record those values in the patchpoint instead.
          if (HFloatConstant* float_constant = value->AsFloatConstantOrNull()) {
            uint32_t float_bits = bit_cast<uint32_t>(float_constant->GetValue());
            llvm::Value* float_bits_value = GetConstantInt(GetUint32Type(), float_bits);
            env_values.push_back(float_bits_value);
          } else if (HDoubleConstant* double_constant = value->AsDoubleConstantOrNull()) {
            uint64_t double_bits = bit_cast<uint64_t>(double_constant->GetValue());
            llvm::Value* double_bits_value = GetConstantInt(GetUint64Type(), double_bits);
            env_values.push_back(double_bits_value);
          } else {
            env_values.push_back(GetValueAllocaOrConstant(value));
          }
        }
      }
      env_it = env_it->GetParent();
    }

    // Create stackmap marking the start of the catch block, that records the environment.
    uint64_t id = EncodePatchpointID(PatchpointKind::kCatchBlock,
                                     AddStackMapInfo(nop, nullptr, catch_block_address_index));
    // Mark this stackmap with memory(write), so that loads of allocas don't get eliminated in the
    // catch block.
    CreateStackMap(id, env_values, llvm::ModRefInfo::Mod);

    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      llvm::AllocaInst* alloca = MaybeGetValueAlloca(phi);
      llvm::Type* alloca_type = alloca->getAllocatedType();
      llvm::Value* loaded_value = CreateLoad(alloca_type, alloca);
      if (alloca->hasMetadata(kGCPointerAllocaMetadata)) {
        loaded_value = CreateCastToUncompressed(loaded_value);
      }
      AddValue(phi, loaded_value);
    }

    for (HInstruction* stack_saved_value : block_info.loaded_alloca_values) {
      if (llvm::AllocaInst* alloca = MaybeGetValueAlloca(stack_saved_value)) {
        llvm::Type* alloca_type = alloca->getAllocatedType();
        llvm::Value* loaded_value = CreateLoad(alloca_type, alloca);
        if (alloca->hasMetadata(kGCPointerAllocaMetadata)) {
          loaded_value = CreateCastToUncompressed(loaded_value);
        }
        auto [it, inserted] =
            block_info.instruction_value_map.insert({stack_saved_value, loaded_value});
        if (!inserted) {
          it->second = loaded_value;
        }
      }
    }
    DCHECK(block_info.stack_saved_value_phis.empty());
  } else {
    // Generate PHI nodes.
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      DCHECK(it.Current()->IsPhi());
      HPhi* phi = it.Current()->AsPhi();
      llvm::Type* phi_type = GetPhiType(phi);
      llvm::PHINode* llvm_phi = __ CreatePHI(phi_type, phi->InputCount());
      AddPhi(phi, llvm_phi);
      AddValue(phi, llvm_phi);
    }

    for (HInstruction* value : block_info.stack_saved_value_phis) {
      // The value must have been saved to the stack previously. If not, it must be a constant, so
      // we don't need to create a phi for it.
      llvm::Type* value_type = GetValueType(value);
      llvm::PHINode* phi = __ CreatePHI(value_type, block->GetPredecessors().size());
      stack_saved_value_phis_.push_back({value, phi, block});
      auto [it, inserted] = block_info.instruction_value_map.insert({value, phi});
      if (!inserted) {
        it->second = phi;
      }
    }
  }

  // Leave the new block as the insert block if we can't generate any instructions in the current
  // one.
  if (current_block != nullptr &&
      (current_block->empty() || !current_block->back().isTerminator())) {
    __ SetInsertPoint(current_block);
  } else {
    SetCurrentBlock(block);
  }
}

void CodeGeneratorARM64LLVM::MoveConstant([[maybe_unused]] Location location,
                                          [[maybe_unused]] int32_t value) {
  UNIMPLEMENTED(FATAL) << "MoveConstant not implemented";
}

void CodeGeneratorARM64LLVM::AddLocationAsTemp([[maybe_unused]] Location location,
                                               [[maybe_unused]] LocationSummary* locations) {
  UNIMPLEMENTED(FATAL) << "AddLocationAsTemp not implemented";
}

void CodeGeneratorARM64LLVM::MaybeMarkGCCard(llvm::Value* object,
                                             llvm::Value* value,
                                             bool emit_null_check) {
  llvm::BasicBlock* done_block = emit_null_check ? CreateBasicBlock() : nullptr;
  if (emit_null_check) {
    // __ Cbz(value, &done);
    llvm::Value* is_zero = __ CreateICmpEQ(value, GetConstantZero(value->getType()));
    CreateBranchIfTrue(is_zero, done_block);
  }
  MarkGCCard(object);
  if (emit_null_check) {
    // __ Bind(&done);
    __ CreateBr(done_block);
    __ SetInsertPoint(done_block);
  }
}

void CodeGeneratorARM64LLVM::MarkGCCard(llvm::Value* object) {
  // Load the address of the card table into `card`.
  // __ Ldr(card, MemOperand(tr, Thread::CardTableOffset<kArm64PointerSize>().Int32Value()));
  llvm::Value* card = CreateLoadFromThreadPointer(
      GetPointerType(), Thread::CardTableOffset<kArm64PointerSize>().Int32Value());
  // Calculate the offset (in the card table) of the card corresponding to `object`.
  // __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  DCHECK(object->getType() == GetUncompressedGCPointerType());
  llvm::Value* object_int_value = CreateCastToInt(object);
  llvm::Value* entry_offset =
      __ CreateLShr(object_int_value, gc::accounting::CardTable::kCardShift);
  // Write the `art::gc::accounting::CardTable::kCardDirty` value into the
  // `object`'s card.
  //
  // Register `card` contains the address of the card table. Note that the card
  // table's base is biased during its creation so that it always starts at an
  // address whose least-significant byte is equal to `kCardDirty` (see
  // art::gc::accounting::CardTable::Create). Therefore the STRB instruction
  // below writes the `kCardDirty` (byte) value into the `object`'s card
  // (located at `card + object >> kCardShift`).
  //
  // This dual use of the value in register `card` (1. to calculate the location
  // of the card to mark; and 2. to load the `kCardDirty` value) saves a load
  // (no need to explicitly load `kCardDirty` as an immediate value).
  // __ Strb(card, MemOperand(card, temp.X()));
  llvm::Value* card_entry_address = CreateGEP(card, entry_offset);
  llvm::Value* card_int_value = __ CreatePtrToInt(card, GetUint64Type());
  card_int_value = __ CreateTrunc(card_int_value, GetUint8Type());
  CreateStore(card_int_value, card_entry_address);
}

void CodeGeneratorARM64LLVM::CheckGCCardIsValid(llvm::Value* object) {
  // Load the address of the card table into `card`.
  // __ Ldr(card, MemOperand(tr, Thread::CardTableOffset<kArm64PointerSize>().Int32Value()));
  static_assert(Thread::CardTableOffset<kArm64PointerSize>().Int32Value() >= 0);
  llvm::Value* card_table = CreateLoadFromThreadPointer(
      GetPointerType(), Thread::CardTableOffset<kArm64PointerSize>().SizeValue());
  // Calculate the offset (in the card table) of the card corresponding to `object`.
  // __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  llvm::Value* object_address_as_int = CreateCastToInt(object);
  llvm::Value* card_table_index =
      __ CreateLShr(object_address_as_int, gc::accounting::CardTable::kCardShift);
  // assert (!clean || !self->is_gc_marking)
  // __ Ldrb(temp, MemOperand(card, temp.X()));
  llvm::Value* card_address = CreateGEP(card_table, card_table_index);
  llvm::Value* card = CreateLoad(GetUint8Type(), card_address);
  static_assert(gc::accounting::CardTable::kCardClean == 0);
  // __ Cbnz(temp, &done);
  // __ Cbz(mr, &done);
  llvm::Value* is_card_not_clean = __ CreateICmpNE(card, GetConstantZero(card->getType()));
  llvm::Value* mr_value = GetMarkingRegisterValue();
  llvm::Value* is_mr_zero = __ CreateICmpEQ(mr_value, GetConstantZero(mr_value->getType()));
  llvm::Value* dont_trap = __ CreateOr(is_card_not_clean, is_mr_zero);

  llvm::BasicBlock* trap_block = CreateBasicBlock();
  llvm::BasicBlock* continue_block = CreateBasicBlock();
  llvm::Instruction* br = __ CreateCondBr(dont_trap, continue_block, trap_block);
  ExpectTrueBranch(br);

  // __ Unreachable();
  __ SetInsertPoint(trap_block);
  __ CreateIntrinsic(llvm::Intrinsic::trap, {}, {});
  __ CreateUnreachable();
  // __ Bind(&done);
  __ SetInsertPoint(continue_block);
}

void CodeGeneratorARM64LLVM::SetupBlockedRegisters() const {
  LOG(FATAL) << "SetupBlockedRegisters shouldn't be called for CodeGeneratorARM64LLVM";
  UNREACHABLE();
}

size_t CodeGeneratorARM64LLVM::SaveCoreRegister([[maybe_unused]] size_t stack_index,
                                                [[maybe_unused]] uint32_t reg_id) {
  UNIMPLEMENTED(FATAL) << "SaveCoreRegister not implemented";
  UNREACHABLE();
}

size_t CodeGeneratorARM64LLVM::RestoreCoreRegister([[maybe_unused]] size_t stack_index,
                                                   [[maybe_unused]] uint32_t reg_id) {
  UNIMPLEMENTED(FATAL) << "RestoreCoreRegister not implemented";
  UNREACHABLE();
}

size_t CodeGeneratorARM64LLVM::SaveFloatingPointRegister([[maybe_unused]] size_t stack_index,
                                                         [[maybe_unused]] uint32_t reg_id) {
  UNIMPLEMENTED(FATAL) << "SaveFloatingPointRegister not implemented";
  UNREACHABLE();
}

size_t CodeGeneratorARM64LLVM::RestoreFloatingPointRegister([[maybe_unused]] size_t stack_index,
                                                            [[maybe_unused]] uint32_t reg_id) {
  UNIMPLEMENTED(FATAL) << "RestoreFloatingPointRegister not implemented";
  UNREACHABLE();
}

void CodeGeneratorARM64LLVM::DumpCoreRegister([[maybe_unused]] std::ostream& stream,
                                              [[maybe_unused]] int reg) const {
  UNIMPLEMENTED(FATAL) << "DumpCoreRegister not implemented";
}

void CodeGeneratorARM64LLVM::DumpFloatingPointRegister([[maybe_unused]] std::ostream& stream,
                                                       [[maybe_unused]] int reg) const {
  UNIMPLEMENTED(FATAL) << "DumpFloatingPointRegister not implemented";
}

const Arm64InstructionSetFeatures& CodeGeneratorARM64LLVM::GetInstructionSetFeatures() const {
  return *GetCompilerOptions().GetInstructionSetFeatures()->AsArm64InstructionSetFeatures();
}

void CodeGeneratorARM64LLVM::MoveLocation([[maybe_unused]] Location destination,
                                          [[maybe_unused]] Location source,
                                          [[maybe_unused]] DataType::Type dst_type) {
  UNIMPLEMENTED(FATAL) << "MoveLocation not implemented";
}

llvm::Value* CodeGeneratorARM64LLVM::Load(DataType::Type type, llvm::Value* address) {
  llvm::Type* llvm_type = GetLLVMType(type);
  return CreateLoad(llvm_type, address);
}

llvm::Value* CodeGeneratorARM64LLVM::LoadVolatile(HInstruction* instruction,
                                                  DataType::Type type,
                                                  llvm::Value* base,
                                                  bool needs_null_check) {
  llvm::Type* loaded_type = GetLLVMType(type);
  DCHECK_NE(type, DataType::Type::kVoid);
  if (needs_null_check && ShouldRecordImplicitNullCheck(instruction)) {
    // A sequentially consistent load is just an LDAR instruction in AArch64, so we can safely use a
    // load acquire placeholder function here.
    return CreateLoadAcquireWithImplicitNullCheck(
        instruction->GetImplicitNullCheck(), loaded_type, base);
  } else {
    return CreateLoad(loaded_type, base, llvm::AtomicOrdering::SequentiallyConsistent);
  }
}

llvm::Value* CodeGeneratorARM64LLVM::HeapReferencePoisoning(llvm::Value* value) {
  DCHECK(value->getType() == GetUncompressedGCPointerType());
  return __ CreateCall(placeholder_functions_.heap_reference_poisoning, value);
}

llvm::Value* CodeGeneratorARM64LLVM::UnpoisonHeapReference(llvm::Value* value) {
  return HeapReferencePoisoning(value);
}

llvm::Value* CodeGeneratorARM64LLVM::PoisonHeapReference(llvm::Value* value) {
  return HeapReferencePoisoning(value);
}

llvm::Value* CodeGeneratorARM64LLVM::MaybeUnpoisonHeapReference(llvm::Value* value) {
  if (kPoisonHeapReferences) {
    return UnpoisonHeapReference(value);
  } else {
    return value;
  }
}

llvm::Value* CodeGeneratorARM64LLVM::MaybePoisonHeapReference(llvm::Value* value) {
  if (kPoisonHeapReferences) {
    return PoisonHeapReference(value);
  } else {
    return value;
  }
}

void CodeGeneratorARM64LLVM::Store(DataType::Type type,
                                   bool is_value_signed,
                                   llvm::Value* value,
                                   llvm::Value* address) {
  llvm::Type* stored_type = GetLLVMType(type);
  if (stored_type != value->getType()) {
    DCHECK(stored_type->isIntegerTy());
    DCHECK(value->getType()->isIntegerTy());
    value = __ CreateIntCast(value, stored_type, is_value_signed);
  }
  CreateStore(value, address);
}

void CodeGeneratorARM64LLVM::StoreVolatile(HInstruction* instruction,
                                           DataType::Type type,
                                           bool is_value_signed,
                                           llvm::Value* value,
                                           llvm::Value* base,
                                           bool needs_null_check) {
  llvm::Type* stored_type = GetLLVMType(type);
  // Check if value needs to be truncated to a narrower integer type.
  if (stored_type != value->getType()) {
    DCHECK(stored_type->isIntegerTy());
    DCHECK(value->getType()->isIntegerTy());
    value = __ CreateIntCast(value, stored_type, is_value_signed);
  }

  if (needs_null_check && ShouldRecordImplicitNullCheck(instruction)) {
    // A sequentially consistent store is just a STLR instruction in AArch64, so we can safely use a
    // store release placeholder function here.
    CreateStoreReleaseWithImplicitNullCheck(instruction->GetImplicitNullCheck(), type, value, base);
  } else {
    CreateStore(value, base, llvm::AtomicOrdering::SequentiallyConsistent);
  }
}

void CodeGeneratorARM64LLVM::SetInvokeRuntimeParametersAndReturnType(
    llvm::ArrayRef<llvm::Value*> parameters,
    llvm::Type* return_type,
    llvm::CallingConv::ID cc,
    std::optional<uint64_t> statepoint_id,
    llvm::SmallVector<llvm::OperandBundleDef, 1> deopt_bundle) {
  DCHECK(invoke_runtime_parameters_.empty());
  DCHECK(invoke_runtime_return_type_ == nullptr);
  DCHECK_GE(parameters.size(), 1u) << "Missing current method pointer from runtime call";
  DCHECK(parameters[0]->getType() == GetMethodPointerType())
      << "First parameter of a runtime call must be the current method pointer";
  invoke_runtime_parameters_.assign(parameters.begin(), parameters.end());
  invoke_runtime_return_type_ = return_type;
  invoke_runtime_cc_ = cc;
  invoke_runtime_statepoint_id_ = statepoint_id;
  invoke_runtime_deopt_bundle_ = std::move(deopt_bundle);
}

CodeGeneratorARM64LLVM::InvokeParametersAndReturnType
CodeGeneratorARM64LLVM::GetInvokeRuntimeParametersAndReturnType() {
  DCHECK(invoke_runtime_return_type_ != nullptr)
      << "Parameters for InvokeRuntime weren't set properly";
  InvokeParametersAndReturnType result = {std::move(invoke_runtime_parameters_),
                                          invoke_runtime_return_type_,
                                          invoke_runtime_cc_,
                                          invoke_runtime_statepoint_id_,
                                          std::move(invoke_runtime_deopt_bundle_)};
  // Reset the values, so we avoid use-after-free of the parameters array.
  invoke_runtime_parameters_ = {};
  invoke_runtime_return_type_ = nullptr;
  invoke_runtime_statepoint_id_ = std::nullopt;
  return result;
}

llvm::CallBase* CodeGeneratorARM64LLVM::GetInvokeRuntimeResult(bool allow_void) {
  DCHECK(invoke_runtime_result_ != nullptr);
  llvm::CallBase* result = invoke_runtime_result_;
  DCHECK_IMPLIES(!allow_void, !result->getType()->isVoidTy());
  invoke_runtime_result_ = nullptr;
  return result;
}

llvm::SmallVector<llvm::Type*> CodeGeneratorARM64LLVM::GetParameterTypes(
    llvm::ArrayRef<llvm::Value*> parameters) const {
  llvm::SmallVector<llvm::Type*> parameter_types;
  parameter_types.reserve(parameters.size());
  for (llvm::Value* parameter : parameters) {
    parameter_types.push_back(parameter->getType());
  }
  return parameter_types;
}

void CodeGeneratorARM64LLVM::InvokeRuntime(QuickEntrypointEnum entrypoint,
                                           HInstruction* instruction,
                                           SlowPathCode* slow_path) {
  // This check depends on the instruction having a location, which is not the case here.
  // FIXME: Can we validate it somehow anyways?
  // ValidateInvokeRuntime(entrypoint, instruction, slow_path);

  ThreadOffset64 entrypoint_offset = GetThreadOffset<kArm64PointerSize>(entrypoint);
  InvokeParametersAndReturnType parameters_and_return_type =
      GetInvokeRuntimeParametersAndReturnType();
  llvm::SmallVector<llvm::Value*, 9>& parameters = parameters_and_return_type.parameters;
  llvm::Type* return_type = parameters_and_return_type.return_type;
  llvm::CallingConv::ID cc = parameters_and_return_type.cc;
  std::optional<uint64_t> statepoint_id = parameters_and_return_type.statepoint_id;
  llvm::ArrayRef<llvm::OperandBundleDef> deopt_bundle = parameters_and_return_type.deopt_bundle;
  DCHECK(return_type != nullptr);

  DCHECK(!GetCompilerOptions().IsJitCompiler());
  DCHECK(entrypoint_offset.Int32Value() >= 0);
  bool requires_stackmap = EntrypointRequiresStackMap(entrypoint);
  bool add_environment = !instruction->IsSuspendCheck() && requires_stackmap &&
                         (slow_path == nullptr || !slow_path->IsFatal());

  llvm::FunctionCallee callee;
  bool use_thunk = slow_path != nullptr;
  if (!use_thunk) {
    llvm::SmallVector<llvm::Type*> parameter_types = GetParameterTypes(parameters);
    llvm::FunctionType* runtime_function_type =
        llvm::FunctionType::get(return_type, parameter_types, false);
    llvm::Value* runtime_function =
        CreateLoadFromThreadPointer(GetPointerType(), entrypoint_offset.Uint32Value());
    callee = llvm::FunctionCallee(runtime_function_type, runtime_function);
  } else if (requires_stackmap) {
    llvm::SmallVector<llvm::Type*> parameter_types = GetParameterTypes(parameters);
    llvm::FunctionType* runtime_function_type =
        llvm::FunctionType::get(return_type, parameter_types, false);
    callee = GetEntrypointThunkPlaceholderFunction(entrypoint_offset, runtime_function_type, cc);
  } else {
    uint32_t stackmap_info_index = statepoint_id.has_value()
                                       ? DecodePatchpointIndexFromID(*statepoint_id)
                                       : AddStackMapInfo(instruction);
    stack_map_infos_[stackmap_info_index].index_or_offset = entrypoint_offset.Uint32Value();
    uint64_t id =
        EncodePatchpointID(PatchpointKind::kEntrypointThunkCallPatchpoint, stackmap_info_index);
    uint32_t parameters_size = parameters.size();
    parameters.insert(parameters.begin(),
                      {GetConstantInt(GetUint64Type(), id),
                       GetConstantInt(GetInt32Type(), kEntrypointThunkPatchSize),
                       GetConstantZero(GetPointerType()),
                       GetConstantInt(GetInt32Type(), parameters_size)});
    if (return_type->isVoidTy()) {
      callee = llvm::Intrinsic::getOrInsertDeclaration(
          GetModule(), llvm::Intrinsic::experimental_patchpoint_void);
    } else {
      callee = llvm::Intrinsic::getOrInsertDeclaration(
          GetModule(), llvm::Intrinsic::experimental_patchpoint, return_type);
    }
  }

  llvm::CallBase* call =
      CreateCallWithCC(instruction, cc, callee, parameters, add_environment, deopt_bundle);
  invoke_runtime_result_ = call;

  if (slow_path != nullptr) {
    // Mark the call as "cold" if we're in a slow path. This means that the function call is rarely
    // executed, and LLVM considers this when doing code layout, placing cold calls outside of the
    // hot path.
    call->addFnAttr(llvm::Attribute::Cold);
    if (slow_path->IsFatal()) {
      call->addFnAttr(llvm::Attribute::NoReturn);
    }
  }

  if (!use_thunk) {
    // Mark the call with "gc-leaf-function" to prevent RewriteStatepointsForGC from rewriting it.
    if (!requires_stackmap) {
      call->addFnAttr(llvm::Attribute::get(GetLLVMContext(), "gc-leaf-function"));
    }

    // If the call requires a stackmap entry or it has deopt parameters, set it's ID.
    if (requires_stackmap || call->hasOperandBundles()) {
      if (!statepoint_id.has_value()) {
        statepoint_id = EncodePatchpointID(PatchpointKind::kNone, AddStackMapInfo(instruction));
      }
      SetStatepointID(instruction, call, *statepoint_id);
    }
  } else if (requires_stackmap) {
    uint32_t stackmap_info_index = statepoint_id.has_value()
                                       ? DecodePatchpointIndexFromID(*statepoint_id)
                                       : AddStackMapInfo(instruction);
    stack_map_infos_[stackmap_info_index].index_or_offset = entrypoint_offset.Uint32Value();
    uint64_t id =
        EncodePatchpointID(PatchpointKind::kEntrypointThunkCallStatepoint, stackmap_info_index);
    SetStatepointID(instruction, call, id);
    call->addFnAttr(llvm::Attribute::get(
        GetLLVMContext(), "statepoint-num-patch-bytes", std::to_string(kEntrypointThunkPatchSize)));
  }
}

void CodeGeneratorARM64LLVM::InvokeRuntimeWithoutRecordingPcInfo(int32_t entry_point_offset,
                                                                 HInstruction* instruction,
                                                                 SlowPathCode* slow_path) {
  ValidateInvokeRuntimeWithoutRecordingPcInfo(instruction, slow_path);

  InvokeParametersAndReturnType parameters_and_return_type =
      GetInvokeRuntimeParametersAndReturnType();
  llvm::ArrayRef<llvm::Value*> parameters = parameters_and_return_type.parameters;
  llvm::Type* return_type = parameters_and_return_type.return_type;
  llvm::CallingConv::ID cc = parameters_and_return_type.cc;
  DCHECK(return_type != nullptr);
  llvm::SmallVector<llvm::Type*> parameter_types = GetParameterTypes(parameters);
  llvm::FunctionType* runtime_function_type =
      llvm::FunctionType::get(return_type, parameter_types, false);

  DCHECK(entry_point_offset >= 0);
  llvm::Value* runtime_function =
      CreateLoadFromThreadPointer(GetPointerType(), static_cast<size_t>(entry_point_offset));
  llvm::CallBase* call = CreateCallWithCC(
      instruction, cc, llvm::FunctionCallee(runtime_function_type, runtime_function), parameters);
  invoke_runtime_result_ = call;
  call->addFnAttr(llvm::Attribute::get(GetLLVMContext(), "gc-leaf-function"));

  if (slow_path != nullptr) {
    // Mark the call as "cold" if we're in a slow path. This means that the function call is rarely
    // executed, and LLVM considers this when doing code layout, placing cold calls outside of the
    // hot path.
    call->addFnAttr(llvm::Attribute::Cold);
    if (slow_path->IsFatal()) {
      call->addFnAttr(llvm::Attribute::NoReturn);
    }
  }

  // If the call has deopt parameters, set it's ID.
  if (call->hasOperandBundles()) {
    uint64_t id = EncodePatchpointID(PatchpointKind::kNone, AddStackMapInfo(instruction));
    SetStatepointID(instruction, call, id);
  }
}

void InstructionCodeGeneratorARM64LLVM::GenerateClassInitializationCheck(
    SlowPathCodeARM64LLVM* slow_path, llvm::Value* class_ptr) {
  // CMP (immediate) is limited to imm12 or imm12<<12, so we would need to materialize
  // the constant 0xf0000000 for comparison with the full 32-bit field. To reduce the code
  // size, load only the high byte of the field and compare with 0xf0.
  // Note: The same code size could be achieved with LDR+MNV(asr #24)+CBNZ but benchmarks
  // show that this pattern is slower (tested on little cores).
  llvm::Type* u8_type = GetUint8Type();
  llvm::Value* status_byte_value = CreateLoadWithOffset(u8_type, class_ptr, kClassStatusByteOffset);
  llvm::Value* do_slow_path =
      __ CreateICmpULT(status_byte_value, GetConstantInt(u8_type, kShiftedVisiblyInitializedValue));
  llvm::Instruction* br =
      __ CreateCondBr(do_slow_path, slow_path->GetEntryBlock(), slow_path->GetExitBlock());
  ExpectFalseBranch(br);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::GenerateBitstringTypeCheckCompare(
    HTypeCheckInstruction* check, llvm::Value* class_ptr) {
  uint32_t path_to_root = check->GetBitstringPathToRoot();
  uint32_t mask = check->GetBitstringMask();
  DCHECK(IsPowerOfTwo(mask + 1));

  llvm::Value* status =
      CreateLoadWithOffset(GetUint32Type(), class_ptr, mirror::Class::StatusOffset().Int32Value());
  status = __ CreateAnd(status, mask);
  return __ CreateICmpEQ(status, GetConstantInt(GetUint32Type(), path_to_root));
}

void CodeGeneratorARM64LLVM::GenerateMemoryBarrier(MemBarrierKind kind) {
  llvm::Value* dmb_arg = nullptr;

  switch (kind) {
    case MemBarrierKind::kAnyAny:
    case MemBarrierKind::kAnyStore: {
      // Creates dmb ish
      dmb_arg = GetConstantInt(GetInt32Type(), 11);
      break;
    }
    case MemBarrierKind::kLoadAny: {
      // Creates dmb ishld
      dmb_arg = GetConstantInt(GetInt32Type(), 9);
      break;
    }
    case MemBarrierKind::kStoreStore: {
      // Creates dmb ishst
      dmb_arg = GetConstantInt(GetInt32Type(), 10);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected memory barrier " << kind;
  }
  // __ Dmb(InnerShareable, type);
  llvm::CallInst* dmb =
      __ CreateIntrinsic(llvm::Intrinsic::AARCH64Intrinsics::aarch64_dmb, {}, dmb_arg);
  AddCallNoaliasMetadata(dmb);
}

bool CodeGeneratorARM64LLVM::CanUseImplicitSuspendCheck() const {
  CHECK(can_use_implicit_suspend_checks_.has_value());
  return GetCompilerOptions().GetImplicitSuspendChecks() && *can_use_implicit_suspend_checks_;
}

void CodeGeneratorARM64LLVM::SetCanUseImplicitSuspendCheck(bool value) {
  CHECK(!can_use_implicit_suspend_checks_.has_value());
  can_use_implicit_suspend_checks_ = value;
}

llvm::BasicBlock* CodeGeneratorARM64LLVM::CreateTryBoundaryCatchSwitch(HTryBoundary* try_boundary) {
  DCHECK(try_boundary->IsEntry());
  llvm::BasicBlock* current_insert_block = __ GetInsertBlock();
  llvm::BasicBlock* catchswitch_block = CreateBasicBlock();
  __ SetInsertPoint(catchswitch_block);

  // Add an artcatchswitch instruction to the block, which tells LLVM where control flow may go
  // after unwinding.
  llvm::ARTCatchSwitchInst* catchswitch =
      __ CreateARTCatchSwitch(try_boundary->GetExceptionHandlers().size());

  for (HBasicBlock* hir_catch_block : try_boundary->GetExceptionHandlers()) {
    llvm::BasicBlock* catch_block = GetIRBasicBlock(hir_catch_block);
    catchswitch->addHandler(catch_block);
    if (catch_block->empty()) {
      __ SetInsertPoint(catch_block);
      __ CreateARTCatchPad();
    }
  }

  __ SetInsertPoint(current_insert_block);
  return catchswitch_block;
}

void CodeGeneratorARM64LLVM::SetTryBoundaryCatchSwitch(HTryBoundary* try_boundary,
                                                       llvm::BasicBlock* catchswitch_block) {
  DCHECK(try_boundary_catchswitch_map_.find(try_boundary) == try_boundary_catchswitch_map_.end());
  try_boundary_catchswitch_map_.insert({try_boundary, catchswitch_block});
}

llvm::BasicBlock* CodeGeneratorARM64LLVM::GetTryBoundaryCatchSwitch(
    const HTryBoundary* try_boundary) {
  auto it = try_boundary_catchswitch_map_.find(try_boundary);
  DCHECK(it != try_boundary_catchswitch_map_.end());
  return it->second;
}

llvm::Type* CodeGeneratorARM64LLVM::GetLLVMType(DataType::Type type) const {
  return ::art::arm64_llvm::GetLLVMType(type, llvm_types_);
}

llvm::Type* CodeGeneratorARM64LLVM::GetVectorType(llvm::Type* packed_type, size_t length) const {
  return llvm::VectorType::get(packed_type, llvm::ElementCount::getFixed(length));
}

llvm::Constant* CodeGeneratorARM64LLVM::GetConstantZero(llvm::Type* type) {
  if (type->isPointerTy()) {
    return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(type));
  } else if (type->isFloatingPointTy()) {
    return llvm::ConstantFP::getZero(type);
  } else {
    DCHECK(type->isIntegerTy());
    return llvm::ConstantInt::get(type, 0);
  }
}

llvm::ConstantInt* CodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type, int32_t value) {
  DCHECK(type->isIntegerTy());
  return llvm::ConstantInt::getSigned(llvm::cast<llvm::IntegerType>(type), value);
}

llvm::ConstantInt* CodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type, int64_t value) {
  DCHECK(type->isIntegerTy());
  return llvm::ConstantInt::getSigned(llvm::cast<llvm::IntegerType>(type), value);
}

llvm::ConstantInt* CodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type, uint32_t value) {
  DCHECK(type->isIntegerTy());
  return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(type), value);
}

llvm::ConstantInt* CodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type, uint64_t value) {
  DCHECK(type->isIntegerTy());
  return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(type), value);
}

llvm::BasicBlock* CodeGeneratorARM64LLVM::CreateBasicBlock(std::string_view str) {
  // Try to insert the new block after the current block.
  llvm::BasicBlock* insert_before = nullptr;
  if (__ GetInsertBlock() != nullptr) {
    insert_before = __ GetInsertBlock() -> getNextNode();
  }
  return llvm::BasicBlock::Create(llvm_context_, str, function_, insert_before);
}

llvm::AllocaInst* CodeGeneratorARM64LLVM::CreateAlloca(llvm::Type* type) {
  bool is_gc_pointer = type == GetUncompressedGCPointerType();
  if (is_gc_pointer) {
    type = GetCompressedGCPointerType();
  }

  llvm::BasicBlock* current_insert_block = __ GetInsertBlock();
  llvm::BasicBlock::iterator current_insert_point = __ GetInsertPoint();
  __ SetInsertPoint(alloca_block_);
  llvm::AllocaInst* alloca = __ CreateAlloca(type);
  // Set alignment to at least kVRegSize.
  alloca->setAlignment(std::max(alloca->getAlign(), llvm::Align(kVRegSize)));
  __ SetInsertPoint(current_insert_block, current_insert_point);

  // Add a metadata node to distinguish GC pointer allocas from regular int32 allocas.
  if (is_gc_pointer) {
    llvm::MDNode* empty_node = llvm::MDTuple::get(GetLLVMContext(), {});
    alloca->setMetadata(kGCPointerAllocaMetadata, empty_node);
  }

  return alloca;
}

void CodeGeneratorARM64LLVM::AddValue(HInstruction* instruction, llvm::Value* value) {
  if (SetContains(all_stack_saved_values_, instruction)) {
    AddValueAlloca(instruction, value->getType());
  }

  DCHECK(instruction != nullptr);
  DCHECK(value != nullptr);
  if (kIsDebugBuild) {
    if (!value->getType()->isVectorTy() && value->getType() != GetMethodPointerType() &&
        !instruction->IsIntermediateAddress()) {
      DCHECK(value->getType() == GetLLVMType(instruction->GetType()));
    }
  }
  DCHECK(instruction_value_map_.find(instruction) == instruction_value_map_.end());
  instruction_value_map_.insert({instruction, value});
}

llvm::AllocaInst* CodeGeneratorARM64LLVM::AddValueAlloca(HInstruction* instruction,
                                                         llvm::Type* type) {
  DCHECK(instruction != nullptr);
  auto it = instruction_alloca_map_.find(instruction);
  if (it != instruction_alloca_map_.end()) {
    return it->second;
  }
  llvm::AllocaInst* alloca = CreateAlloca(type);
  instruction_alloca_map_.insert({instruction, alloca});
  if (instruction->IsParameterValue()) {
    llvm::Constant* parameter_index =
        GetConstantInt(GetUint32Type(), GetIndexOfParameterValue(instruction->AsParameterValue()));
    llvm::MDNode* parameter_index_node =
        llvm::MDTuple::get(GetLLVMContext(), llvm::ConstantAsMetadata::get(parameter_index));
    alloca->setMetadata(kParameterAllocaMetadata, parameter_index_node);
  } else {
    llvm::Constant* instruction_id = GetConstantInt(GetInt32Type(), instruction->GetId());
    llvm::MDNode* instruction_id_node =
        llvm::MDTuple::get(GetLLVMContext(), llvm::ConstantAsMetadata::get(instruction_id));
    alloca->setMetadata(kInstructionAllocaMetadata, instruction_id_node);
  }
  return alloca;
}

llvm::Value* CodeGeneratorARM64LLVM::GetValue(HInstruction* instruction,
                                              DataType::Type value_type) {
  const BasicBlockInfo& current_block_info = GetBasicBlockInfo(current_block_);
  llvm::Value* result = nullptr;
  if (auto current_block_it = current_block_info.instruction_value_map.find(instruction);
      current_block_it != current_block_info.instruction_value_map.end()) {
    result = current_block_it->second;
  } else {
    auto it = instruction_value_map_.find(instruction);
    DCHECK(it != instruction_value_map_.end())
        << "Failed to find instruction ID " << instruction->GetId();
    result = it->second;
  }

  if (value_type != DataType::Type::kVoid && result->getType() != GetLLVMType(value_type)) {
    DCHECK(result->getType()->isIntegerTy());
    DCHECK(DataType::IsIntegralType(value_type));
    bool is_signed = !DataType::IsUnsignedType(instruction->GetType());
    result = __ CreateIntCast(result, GetLLVMType(value_type), is_signed);
  }
  return result;
}

llvm::Value* CodeGeneratorARM64LLVM::GetValueAllocaOrConstant(HInstruction* instruction) {
  if (auto it = instruction_alloca_map_.find(instruction); it != instruction_alloca_map_.end()) {
    return it->second;
  }

  auto it = instruction_value_map_.find(instruction);
  DCHECK(it != instruction_value_map_.end())
      << "Failed to find instruction ID " << instruction->GetId();
  DCHECK(llvm::isa<llvm::Constant>(it->second));
  return it->second;
}

llvm::AllocaInst* CodeGeneratorARM64LLVM::MaybeGetValueAlloca(HInstruction* instruction) {
  auto it = instruction_alloca_map_.find(instruction);
  if (it == instruction_alloca_map_.end()) {
    return nullptr;
  }
  return it->second;
}

llvm::Type* CodeGeneratorARM64LLVM::GetValueType(HInstruction* instruction) {
  auto it = instruction_value_map_.find(instruction);
  DCHECK(it != instruction_value_map_.end())
      << "Failed to find instruction ID " << instruction->GetId();
  return it->second->getType();
}

void CodeGeneratorARM64LLVM::AddPhi(HPhi* phi, llvm::PHINode* llvm_phi) {
  phis_.push_back({phi, llvm_phi});
}

void CodeGeneratorARM64LLVM::MapParameterValueToIndex(HParameterValue* value, unsigned int index) {
  parametervalue_index_map_.insert({value, index});
}

unsigned int CodeGeneratorARM64LLVM::GetIndexOfParameterValue(HParameterValue* value) {
  auto it = parametervalue_index_map_.find(value);
  DCHECK(it != parametervalue_index_map_.end())
      << "Cannot find index for HParameterValue with ID " << value->GetId();

  return it->second;
}

uint32_t CodeGeneratorARM64LLVM::AddStackMapInfo(HInstruction* instruction,
                                                 const DexFile* dex_file,
                                                 uint32_t index_or_offset) {
  if (dex_file == nullptr && instruction != nullptr) {
    dex_file = &GetGraph()->GetDexFile();
  }
  uint32_t index = stack_map_infos_.size();
  stack_map_infos_.push_back({instruction, dex_file, index_or_offset, /* is_used= */ false});
  return index;
}

llvm::SmallVector<llvm::OperandBundleDef, 1> CodeGeneratorARM64LLVM::GetDeoptBundle(
    HInstruction* instruction, bool add_environment) {
  bool needs_vreg_info = NeedsVregInfo(instruction);
  // We need to keep reference environment values alive in case the class doesn't have the
  // @DeadReferenceSafe annotation.
  // FIXME: We should always create the deopt bundle if necessary, but right now we have an issue
  // with late suspend check insertion. In that case, we can't call GetValue() anymore, which
  // we use to map from HIR values to LLVM values, because it can cause usage of dangling
  // llvm::Value pointers or generating invalid code.
  bool eliminate_dead_references =
      instruction->IsSuspendCheck() || GetGraph()->IsDeadReferenceSafe() || !add_environment;

  if (!needs_vreg_info && eliminate_dead_references) {
    return {};
  }

  std::vector<llvm::Value*> deopt_values;
  HEnvironment* environment = instruction->GetEnvironment();
  // We need to collect all values in the environment, including the values from the parent
  // environments.
  BasicBlockInfo& instruction_block_info = GetBasicBlockInfo(instruction->GetBlock());
  while (environment != nullptr) {
    for (size_t i = 0, size = environment->Size(); i < size; ++i) {
      HInstruction* value = environment->GetInstructionAt(i);
      if (value == nullptr) {
        continue;
      }

      // In case we don't need VReg info, we can skip the value if it's a primitive (i.e. not a
      // reference).
      if (!needs_vreg_info && value->GetType() != DataType::Type::kReference) {
        continue;
      }

      llvm::Value* llvm_value = nullptr;
      // We can use the value's stack slot if it's already saved to the stack.
      if (!needs_vreg_info && !value->IsParameterValue() &&
          (SetContains(instruction_block_info.live_stack_saved_values, value) ||
           MapContains(instruction_block_info.partially_live_stack_saved_values, value))) {
        llvm_value = GetValueAllocaOrConstant(value);
      } else {
        llvm_value = GetValue(value);
        switch (value->GetType()) {
          case DataType::Type::kBool:
          case DataType::Type::kUint8:
          case DataType::Type::kInt8:
          case DataType::Type::kUint16:
          case DataType::Type::kInt16:
            // Promote the value to be at least 32 bits wide. The runtime requires vreg values
            // spilled to the stack to be aligned to the vreg size.
            static_assert(kFrameSlotSize == 4, "Frame slot size is expected to be 4 bytes.");
            llvm_value = __ CreateIntCast(
                llvm_value, GetUint32Type(), !DataType::IsUnsignedType(value->GetType()));
            break;
          case DataType::Type::kReference:
          case DataType::Type::kUint32:
          case DataType::Type::kInt32:
          case DataType::Type::kUint64:
          case DataType::Type::kInt64:
          case DataType::Type::kFloat32:
          case DataType::Type::kFloat64:
          case DataType::Type::kVoid:
            // Nothing to do.
            break;
        }
      }
      deopt_values.push_back(llvm_value);
    }
    environment = environment->GetParent();
  }

  llvm::SmallVector<llvm::OperandBundleDef, 1> result;
  // Create the operand bundle if we need VReg info, or we added some references to extend their
  // lifetime.
  if (needs_vreg_info || !deopt_values.empty()) {
    result.emplace_back("deopt", std::move(deopt_values));
  }
  return result;
}

void CodeGeneratorARM64LLVM::SetStatepointID(HInstruction* instruction,
                                             llvm::CallBase* call,
                                             uint64_t id) {
  DCHECK_IMPLIES(NeedsVregInfo(instruction),
                 call->getOperandBundle(llvm::LLVMContext::OB_deopt).has_value());
  call->addFnAttr(llvm::Attribute::get(GetLLVMContext(), "statepoint-id", std::to_string(id)));
}

llvm::Value* CodeGeneratorARM64LLVM::GetThreadPointerValue(llvm::Type* type) {
  llvm::CallInst* tr_value_call = __ CreateIntrinsic(
      llvm::Intrinsic::read_register, llvm_types_.i64, metadata_.thread_register);
  // Add memory(none) attribute. This allows optimizations to merge subsequent reads of the same
  // registers.
  tr_value_call->addFnAttr(
      llvm::Attribute::getWithMemoryEffects(GetLLVMContext(), llvm::MemoryEffects::none()));
  llvm::Value* tr_value = tr_value_call;
  if (type == nullptr || type == llvm_types_.ptr) {
    tr_value = __ CreateIntToPtr(tr_value, llvm_types_.ptr);
  }
  return tr_value;
}

llvm::Value* CodeGeneratorARM64LLVM::GetMarkingRegisterValue(llvm::Type* type) {
  llvm::CallInst* mr_value_call = __ CreateIntrinsic(
      llvm::Intrinsic::read_register, llvm_types_.i64, metadata_.marking_register);
  // Add memory(none) attribute. This allows optimizations to merge subsequent reads of the same
  // registers.
  mr_value_call->addFnAttr(
      llvm::Attribute::getWithMemoryEffects(GetLLVMContext(), llvm::MemoryEffects::none()));
  llvm::Value* mr_value = mr_value_call;
  if (type == llvm_types_.ptr) {
    mr_value = __ CreateIntToPtr(mr_value, llvm_types_.ptr);
  }
  return mr_value;
}

llvm::Value* CodeGeneratorARM64LLVM::GetImplicitSuspendRegisterValue(llvm::Type* type) {
  llvm::CallInst* scr_value_call = __ CreateIntrinsic(
      llvm::Intrinsic::read_register, llvm_types_.i64, metadata_.implicit_suspend_check_register);
  // Add memory(none) attribute. This allows optimizations to merge subsequent reads of the same
  // registers.
  scr_value_call->addFnAttr(
      llvm::Attribute::getWithMemoryEffects(GetLLVMContext(), llvm::MemoryEffects::none()));
  llvm::Value* scr_value = scr_value_call;
  if (type == nullptr || type == llvm_types_.ptr) {
    scr_value = __ CreateIntToPtr(scr_value, llvm_types_.ptr);
  }
  return scr_value;
}

llvm::Value* CodeGeneratorARM64LLVM::GetStackPointerValue() {
  llvm::CallInst* sp_value = __ CreateIntrinsic(
      llvm::Intrinsic::read_register, llvm_types_.i64, metadata_.stack_pointer_register);
  // Add memory(none) attribute. This allows optimizations to merge subsequent reads of the same
  // registers.
  sp_value->addFnAttr(
      llvm::Attribute::getWithMemoryEffects(GetLLVMContext(), llvm::MemoryEffects::none()));
  return __ CreateIntToPtr(sp_value, GetPointerType());
}

llvm::Instruction* CodeGeneratorARM64LLVM::CreateCastToUncompressed(llvm::Value* compressed_ptr) {
  return __ CreateCall(placeholder_functions_.cast_to_uncompressed, compressed_ptr);
}

llvm::Instruction* CodeGeneratorARM64LLVM::CreateCastToCompressed(llvm::Value* uncompressed_ptr) {
  return __ CreateCall(placeholder_functions_.cast_to_compressed, uncompressed_ptr);
}

llvm::Instruction* CodeGeneratorARM64LLVM::CreateCastToInt(llvm::Value* ptr) {
  return __ CreateCall(placeholder_functions_.cast_pointer_to_int, ptr);
}

llvm::Value* CodeGeneratorARM64LLVM::CreateLoad(llvm::Type* type,
                                                llvm::Value* address,
                                                llvm::AtomicOrdering ordering) {
  if (type->isVectorTy()) {
    CHECK(ordering == llvm::AtomicOrdering::Unordered ||
          ordering == llvm::AtomicOrdering::NotAtomic);
    // FIXME: LLVM can't represent atomic loads of vector types, even if they are native types, such
    // as <4 x i32>. In this case we fall back to a non-atomic ordering.
    ordering = llvm::AtomicOrdering::NotAtomic;
  }
  llvm::Type* loaded_type = type;
  bool is_gc_pointer = type == GetUncompressedGCPointerType();
  bool is_boolean = type == GetBooleanType();
  if (is_gc_pointer) {
    loaded_type = GetCompressedGCPointerType();
  }
  if (is_boolean) {
    loaded_type = GetUint8Type();
  }
  llvm::LoadInst* load_inst = __ CreateLoad(loaded_type, address);
  load_inst->setAtomic(ordering);
  llvm::Value* result = load_inst;
  if (is_gc_pointer) {
    result = CreateCastToUncompressed(result);
  }
  if (is_boolean) {
    result = __ CreateTrunc(result, type);
  }
  return result;
}

void CodeGeneratorARM64LLVM::CreateStore(llvm::Value* value,
                                         llvm::Value* address,
                                         llvm::AtomicOrdering ordering,
                                         bool is_volatile) {
  if (value->getType()->isVectorTy()) {
    CHECK(ordering == llvm::AtomicOrdering::Unordered ||
          ordering == llvm::AtomicOrdering::NotAtomic);
    // FIXME: LLVM can't represent atomic stores of vector types, even if they are native types,
    // such as <4 x i32>. In this case we fall back to a non-atomic ordering.
    ordering = llvm::AtomicOrdering::NotAtomic;
  }
  if (value->getType() == GetUncompressedGCPointerType()) {
    value = CreateCastToCompressed(value);
  }
  if (value->getType() == GetBooleanType()) {
    value = __ CreateZExt(value, GetUint8Type());
  }
  llvm::StoreInst* store = __ CreateStore(value, address, is_volatile);
  store->setAtomic(ordering);
}

llvm::Value* CodeGeneratorARM64LLVM::CreateGEP(llvm::Value* address, llvm::Value* offset) {
  return CreateGEP(GetUint8Type(), address, offset);
}

llvm::Value* CodeGeneratorARM64LLVM::CreateGEP(llvm::Value* address, int64_t offset) {
  return CreateGEP(GetUint8Type(), address, offset);
}

llvm::Value* CodeGeneratorARM64LLVM::CreateGEP(llvm::Type* type,
                                               llvm::Value* address,
                                               llvm::Value* offset) {
  if (type == GetUncompressedGCPointerType()) {
    type = GetCompressedGCPointerType();
  }

  return __ CreateGEP(type, address, offset, "", /* IsInBounds= */ true);
}

llvm::Value* CodeGeneratorARM64LLVM::CreateGEP(llvm::Type* type,
                                               llvm::Value* address,
                                               int64_t offset) {
  if (type == GetUncompressedGCPointerType()) {
    type = GetCompressedGCPointerType();
  }

  return __ CreateConstInBoundsGEP1_64(type, address, offset);
}

CodeGeneratorARM64LLVM::CmpXchgResult CodeGeneratorARM64LLVM::CreateAtomicCmpXchg(
    llvm::Value* ptr,
    llvm::Value* cmp,
    llvm::Value* new_value,
    llvm::MaybeAlign align,
    llvm::AtomicOrdering success_ordering,
    llvm::AtomicOrdering failure_ordering,
    bool is_strong) {
  DCHECK(cmp->getType() == new_value->getType());
  bool is_gc_pointer = cmp->getType() == GetUncompressedGCPointerType();
  if (is_gc_pointer) {
    cmp = MaybePoisonHeapReference(cmp);
    cmp = CreateCastToCompressed(cmp);
    new_value = MaybePoisonHeapReference(new_value);
    new_value = CreateCastToCompressed(new_value);
  }

  llvm::AtomicCmpXchgInst* cmp_xchg =
      __ CreateAtomicCmpXchg(ptr, cmp, new_value, align, success_ordering, failure_ordering);
  cmp_xchg->setWeak(!is_strong);

  llvm::Value* loaded_value = __ CreateExtractValue(cmp_xchg, 0);
  llvm::Value* success = __ CreateExtractValue(cmp_xchg, 1);
  if (is_gc_pointer) {
    loaded_value = CreateCastToUncompressed(loaded_value);
    loaded_value = MaybeUnpoisonHeapReference(loaded_value);
  }
  return {loaded_value, success};
}

llvm::Value* CodeGeneratorARM64LLVM::CreateAtomicRMW(llvm::AtomicRMWInst::BinOp op,
                                                     llvm::Value* ptr,
                                                     llvm::Value* val,
                                                     llvm::MaybeAlign align,
                                                     llvm::AtomicOrdering ordering) {
  bool is_gc_pointer = val->getType() == GetUncompressedGCPointerType();
  if (is_gc_pointer) {
    val = MaybePoisonHeapReference(val);
    val = CreateCastToCompressed(val);
  }
  llvm::Value* result = __ CreateAtomicRMW(op, ptr, val, align, ordering);
  if (is_gc_pointer) {
    result = CreateCastToUncompressed(result);
    result = MaybeUnpoisonHeapReference(result);
  }
  return result;
}

llvm::Value* CodeGeneratorARM64LLVM::CreateLoadFromThreadPointer(llvm::Type* type,
                                                                 size_t offset,
                                                                 llvm::AtomicOrdering ordering) {
  llvm::Value* tr_value = GetThreadPointerValue();
  return CreateLoadWithOffset(type, tr_value, offset, ordering);
}

void CodeGeneratorARM64LLVM::CreateStoreToThreadPointer(llvm::Value* value,
                                                        size_t offset,
                                                        llvm::AtomicOrdering ordering) {
  llvm::Value* tr_value = GetThreadPointerValue();
  CreateStoreWithOffset(value, tr_value, offset, ordering);
}

llvm::Value* CodeGeneratorARM64LLVM::CreateLoadWithOffset(llvm::Type* type,
                                                          llvm::Value* address,
                                                          int64_t offset,
                                                          llvm::AtomicOrdering ordering) {
  if (offset != 0) {
    address = CreateGEP(address, offset);
  }
  return CreateLoad(type, address, ordering);
}

void CodeGeneratorARM64LLVM::CreateStoreWithOffset(llvm::Value* value,
                                                   llvm::Value* address,
                                                   int64_t offset,
                                                   llvm::AtomicOrdering ordering) {
  if (offset != 0) {
    address = CreateGEP(address, offset);
  }
  CreateStore(value, address, ordering);
}

static bool IsValidOffsetForLdrStrOffset(int64_t offset, size_t type_size) {
  static_assert(kLdrOffsetWidth == kStrOffsetWidth,
                "ldr and str offset width both should be 12 bits");
  DCHECK(type_size == 1 || type_size == 2 || type_size == 4 || type_size == 8)
      << "Unexpected type size: " << type_size;
  int64_t scaled_offset = offset / static_cast<int64_t>(type_size);
  return scaled_offset >= 0 && scaled_offset < (1u << kLdrOffsetWidth);
}

llvm::Instruction* CodeGeneratorARM64LLVM::CreateLoadWithImplicitNullCheck(HNullCheck* instruction,
                                                                           llvm::Type* type,
                                                                           llvm::Value* address,
                                                                           int64_t offset) {
  while (llvm::GetElementPtrInst* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(address)) {
    // Only handle a single constant index.
    if (gep->getNumOperands() == 2 && llvm::isa<llvm::ConstantInt>(gep->getOperand(1))) {
      int64_t type_size = GetModule()
                              ->getDataLayout()
                              .getTypeAllocSize(gep->getSourceElementType())
                              .getFixedValue();
      address = gep->getPointerOperand();
      offset += type_size * llvm::cast<llvm::ConstantInt>(gep->getOperand(1))->getSExtValue();
    } else {
      break;
    }
  }

  size_t type_size = GetModule()->getDataLayout().getTypeAllocSize(type).getFixedValue();
  if (!IsValidOffsetForLdrStrOffset(offset, type_size)) {
    address = CreateGEP(address, offset);
  }

  PatchpointKind kind = PatchpointKind::kNone;
  if (type == GetUncompressedGCPointerType()) {
    kind = PatchpointKind::kLoadGcRoot;
  } else if (type == GetBooleanType()) {
    kind = PatchpointKind::kLoadBoolean;
  } else if (type == GetInt8Type()) {
    kind = PatchpointKind::kLoadInt8;
  } else if (type == GetInt16Type()) {
    kind = PatchpointKind::kLoadInt16;
  } else if (type == GetInt32Type()) {
    kind = PatchpointKind::kLoadInt32;
  } else if (type == GetInt64Type()) {
    kind = PatchpointKind::kLoadInt64;
  } else if (type == GetFloat32Type()) {
    kind = PatchpointKind::kLoadFloat32;
  } else if (type == GetFloat64Type()) {
    kind = PatchpointKind::kLoadFloat64;
  } else {
    llvm::dbgs() << *type << '\n';
    LOG(FATAL) << "Unexpected load type";
  }

  uint64_t id = EncodePatchpointID(kind, AddStackMapInfo(instruction));
  llvm::Function* callee = GetPlaceholderFunction(kind);
  std::array<llvm::Value*, 2> arguments = {address, GetConstantInt(GetUint32Type(), offset)};
  // NOTE: These calls only need deopt info if they can throw into a catch block, which is covered
  // by NeedsVregInfo().
  llvm::CallBase* call = CreateCallOrInvoke(instruction,
                                            callee,
                                            arguments,
                                            /* normal_successor= */ nullptr,
                                            /* add_environment= */ false);
  SetStatepointID(instruction, call, id);
  return call;
}

llvm::Instruction* CodeGeneratorARM64LLVM::CreateDiscardedLoadWithImplicitNullCheck(
    HNullCheck* instruction, llvm::Value* address, int64_t offset) {
  if (offset != 0) {
    address = CreateGEP(address, offset);
  }

  llvm::Function* callee = GetPlaceholderFunction(PatchpointKind::kDiscardedLoad);
  uint32_t stack_map_info_index = AddStackMapInfo(instruction);
  uint64_t id = EncodePatchpointID(PatchpointKind::kDiscardedLoad, stack_map_info_index);
  // NOTE: These calls only need deopt info if they can throw into a catch block, which is covered
  // by NeedsVregInfo().
  llvm::CallBase* call = CreateCallOrInvoke(instruction,
                                            callee,
                                            {address},
                                            /* normal_successor= */ nullptr,
                                            /* add_environment= */ false);
  SetStatepointID(instruction, call, id);
  return call;
}

llvm::Value* CodeGeneratorARM64LLVM::CreateLoadAcquireWithImplicitNullCheck(HNullCheck* instruction,
                                                                            llvm::Type* type,
                                                                            llvm::Value* address) {
  PatchpointKind kind = PatchpointKind::kNone;
  if (type == GetUncompressedGCPointerType()) {
    kind = PatchpointKind::kLoadAcquireGcRoot;
  } else if (type == GetBooleanType()) {
    kind = PatchpointKind::kLoadAcquireBoolean;
  } else if (type == GetInt8Type()) {
    kind = PatchpointKind::kLoadAcquireInt8;
  } else if (type == GetInt16Type()) {
    kind = PatchpointKind::kLoadAcquireInt16;
  } else if (type == GetInt32Type()) {
    kind = PatchpointKind::kLoadAcquireInt32;
  } else if (type == GetInt64Type()) {
    kind = PatchpointKind::kLoadAcquireInt64;
  } else if (type == GetFloat32Type()) {
    kind = PatchpointKind::kLoadAcquireInt32;
  } else if (type == GetFloat64Type()) {
    kind = PatchpointKind::kLoadAcquireInt64;
  } else {
    LOG(FATAL) << "Unexpected load type";
  }

  uint64_t id = EncodePatchpointID(kind, AddStackMapInfo(instruction));
  llvm::Function* callee = GetPlaceholderFunction(kind);
  // NOTE: These calls only need deopt info if they can throw into a catch block, which is covered
  // by NeedsVregInfo().
  llvm::CallBase* call = CreateCallOrInvoke(instruction,
                                            callee,
                                            {address},
                                            /* normal_successor= */ nullptr,
                                            /* add_environment= */ false);
  SetStatepointID(instruction, call, id);
  if (type->isFloatingPointTy()) {
    DCHECK(call->getType()->isIntegerTy());
    return __ CreateBitCast(call, type);
  } else {
    return call;
  }
}

llvm::Value* CodeGeneratorARM64LLVM::CreateLoadMaybeWithImplicitNullCheck(HInstruction* instruction,
                                                                          llvm::Type* type,
                                                                          llvm::Value* address,
                                                                          int64_t offset) {
  if (ShouldRecordImplicitNullCheck(instruction)) {
    return CreateLoadWithImplicitNullCheck(
        instruction->GetImplicitNullCheck(), type, address, offset);
  } else {
    return CreateLoadWithOffset(type, address, offset);
  }
}

void CodeGeneratorARM64LLVM::CreateStoreWithImplicitNullCheck(HNullCheck* instruction,
                                                              DataType::Type value_type,
                                                              llvm::Value* value,
                                                              llvm::Value* address,
                                                              int64_t offset) {
  while (llvm::GetElementPtrInst* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(address)) {
    // Only handle a single constant index.
    if (gep->getNumOperands() == 2 && llvm::isa<llvm::ConstantInt>(gep->getOperand(1))) {
      int64_t type_size = GetModule()
                              ->getDataLayout()
                              .getTypeAllocSize(gep->getSourceElementType())
                              .getFixedValue();
      address = gep->getPointerOperand();
      offset += type_size * llvm::cast<llvm::ConstantInt>(gep->getOperand(1))->getSExtValue();
    } else {
      break;
    }
  }

  DCHECK(value->getType() == GetLLVMType(value_type));
  size_t type_size =
      GetModule()->getDataLayout().getTypeAllocSize(value->getType()).getFixedValue();
  if (!IsValidOffsetForLdrStrOffset(offset, type_size)) {
    address = CreateGEP(address, offset);
  }

  llvm::Type* type = value->getType();
  PatchpointKind kind = PatchpointKind::kNone;
  if (type == GetUncompressedGCPointerType()) {
    kind = PatchpointKind::kStoreGcRoot;
  } else if (type == GetBooleanType()) {
    kind = PatchpointKind::kStoreBoolean;
  } else if (type == GetInt8Type()) {
    kind = PatchpointKind::kStoreInt8;
  } else if (type == GetInt16Type()) {
    kind = PatchpointKind::kStoreInt16;
  } else if (type == GetInt32Type()) {
    kind = PatchpointKind::kStoreInt32;
  } else if (type == GetInt64Type()) {
    kind = PatchpointKind::kStoreInt64;
  } else if (type == GetFloat32Type()) {
    kind = PatchpointKind::kStoreFloat32;
  } else if (type == GetFloat64Type()) {
    kind = PatchpointKind::kStoreFloat64;
  } else {
    llvm::dbgs() << *type << '\n';
    LOG(FATAL) << "Unexpected store type";
  }

  uint64_t id = EncodePatchpointID(kind, AddStackMapInfo(instruction));
  llvm::Function* callee = GetPlaceholderFunction(kind);
  std::array<llvm::Value*, 3> arguments = {value, address, GetConstantInt(GetUint32Type(), offset)};
  // NOTE: These calls only need deopt info if they can throw into a catch block, which is covered
  // by NeedsVregInfo().
  llvm::CallBase* call = CreateCallOrInvoke(instruction,
                                            callee,
                                            arguments,
                                            /* normal_successor= */ nullptr,
                                            /* add_environment= */ false);
  SetStatepointID(instruction, call, id);
}

void CodeGeneratorARM64LLVM::CreateStoreReleaseWithImplicitNullCheck(HNullCheck* instruction,
                                                                     DataType::Type value_type,
                                                                     llvm::Value* value,
                                                                     llvm::Value* address) {
  DCHECK(value->getType() == GetLLVMType(value_type));
  PatchpointKind kind = PatchpointKind::kNone;
  switch (value_type) {
    case DataType::Type::kReference:
      kind = PatchpointKind::kStoreReleaseGcRoot;
      break;
    case DataType::Type::kBool:
      kind = PatchpointKind::kStoreReleaseBoolean;
      break;
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
      kind = PatchpointKind::kStoreReleaseInt8;
      break;
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      kind = PatchpointKind::kStoreReleaseInt16;
      break;
    case DataType::Type::kUint32:
    case DataType::Type::kInt32:
      kind = PatchpointKind::kStoreReleaseInt32;
      break;
    case DataType::Type::kUint64:
    case DataType::Type::kInt64:
      kind = PatchpointKind::kStoreReleaseInt64;
      break;
    case DataType::Type::kFloat32:
      kind = PatchpointKind::kStoreReleaseInt32;
      break;
    case DataType::Type::kFloat64:
      kind = PatchpointKind::kStoreReleaseInt64;
      break;
    case DataType::Type::kVoid:
      LOG(FATAL) << "Unreachable";
      break;
  }

  uint64_t id = EncodePatchpointID(kind, AddStackMapInfo(instruction));
  llvm::Function* callee = GetPlaceholderFunction(kind);
  if (DataType::IsFloatingPointType(value_type)) {
    llvm::Type* int_type = value_type == DataType::Type::kFloat32 ? GetInt32Type() : GetInt64Type();
    value = __ CreateBitCast(value, int_type);
  }
  // NOTE: These calls only need deopt info if they can throw into a catch block, which is covered
  // by NeedsVregInfo().
  llvm::CallBase* call = CreateCallOrInvoke(instruction,
                                            callee,
                                            {value, address},
                                            /* normal_successor= */ nullptr,
                                            /* add_environment= */ false);
  SetStatepointID(instruction, call, id);
}

void CodeGeneratorARM64LLVM::CreateStoreMaybeWithImplicitNullCheck(HInstruction* instruction,
                                                                   DataType::Type value_type,
                                                                   bool is_value_signed,
                                                                   llvm::Value* value,
                                                                   llvm::Value* address,
                                                                   int64_t offset) {
  llvm::Type* stored_type = GetLLVMType(value_type);
  if (value->getType() != stored_type) {
    DCHECK(value->getType()->isIntegerTy());
    DCHECK(stored_type->isIntegerTy());
    value = __ CreateIntCast(value, stored_type, is_value_signed);
  }
  if (ShouldRecordImplicitNullCheck(instruction)) {
    CreateStoreWithImplicitNullCheck(
        instruction->GetImplicitNullCheck(), value_type, value, address, offset);
  } else {
    CreateStoreWithOffset(value, address, offset);
  }
}

llvm::CallBase* CodeGeneratorARM64LLVM::CreateCallOrInvoke(
    HInstruction* instruction,
    llvm::FunctionCallee callee,
    llvm::ArrayRef<llvm::Value*> arguments,
    llvm::BasicBlock* normal_successor,
    bool add_environment,
    llvm::ArrayRef<llvm::OperandBundleDef> deopt_bundle) {
  llvm::SmallVector<llvm::OperandBundleDef, 1> deopt_bundle_vec;
  if (deopt_bundle.empty()) {
    deopt_bundle_vec = GetDeoptBundle(instruction, add_environment);
    deopt_bundle = deopt_bundle_vec;
  }

  llvm::CallBase* result = nullptr;
  // If the instruction's block has try-catch information, then we should use an invoke.
  if (instruction->CanThrowIntoCatchBlock()) {
    if (normal_successor == nullptr) {
      normal_successor = CreateBasicBlock();
    }
    const HTryBoundary* try_entry =
        &instruction->GetBlock()->GetTryCatchInformation()->GetTryEntry();
    llvm::BasicBlock* unwind_successor = GetTryBoundaryCatchSwitch(try_entry);
    result = __ CreateInvoke(callee, normal_successor, unwind_successor, arguments, deopt_bundle);
    __ SetInsertPoint(normal_successor);
  } else {
    result = __ CreateCall(callee, arguments, deopt_bundle);
  }

  // Inherit the calling convention of the function in case of a static call.
  if (llvm::Function* function = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
    result->setCallingConv(function->getCallingConv());
  }
  return result;
}

llvm::CallBase* CodeGeneratorARM64LLVM::CreateCallWithCC(
    HInstruction* instruction,
    llvm::CallingConv::ID cc,
    llvm::FunctionCallee callee,
    llvm::ArrayRef<llvm::Value*> args,
    bool add_deopt_bundle,
    llvm::ArrayRef<llvm::OperandBundleDef> deopt_bundle) {
  llvm::CallBase* call = CreateCallOrInvoke(instruction,
                                            callee,
                                            args,
                                            /* normal_successor= */ nullptr,
                                            add_deopt_bundle,
                                            deopt_bundle);
  call->setCallingConv(cc);
  AddCallNoaliasMetadata(call);

  // InvokeRuntime call arguments don't necessarily correspond to an HInvoke's arguments, so we
  // don't apply any attributes in that case.
  if (cc != llvm::CallingConv::ARTInvokeRuntime &&
      cc != llvm::CallingConv::ARTInvokeRuntimeHiddenReceiver) {
    AddAttributesToCall(instruction, call);
  }

  return call;
}

llvm::CallBase* CodeGeneratorARM64LLVM::CreateInvokeDexCall(HInstruction* instruction,
                                                            llvm::FunctionType* function_type,
                                                            llvm::Value* callee,
                                                            llvm::ArrayRef<llvm::Value*> args) {
  DCHECK_GE(args.size(), 2u) << "Missing callee method or caller method pointer in InvokeDex call";
  DCHECK(args[0]->getType() == GetMethodPointerType())
      << "First parameter of a dex call must be the callee method pointer";
  DCHECK(args[1]->getType() == GetMethodPointerType())
      << "Second parameter of a dex call must be the caller method pointer";
  return CreateCallWithCC(instruction,
                          llvm::CallingConv::ARTInvokeDex,
                          llvm::FunctionCallee(function_type, callee),
                          args);
}

llvm::CallBase* CodeGeneratorARM64LLVM::CreateInvokeInterfaceCall(
    HInstruction* instruction,
    llvm::FunctionType* function_type,
    llvm::Value* callee,
    llvm::ArrayRef<llvm::Value*> args) {
  DCHECK_GE(args.size(), 3u) << "Missing callee method or caller method pointer in InvokeDex call";
  DCHECK(args[0]->getType() == GetMethodPointerType())
      << "First parameter of an interface call must be the callee method pointer";
  DCHECK(args[1]->getType() == GetMethodPointerType())
      << "Second parameter of an interface call must be the callee method pointer";
  DCHECK(args[2]->getType() == GetMethodPointerType())
      << "Third parameter of an interface call must be the caller method pointer";
  return CreateCallWithCC(instruction,
                          llvm::CallingConv::ARTInvokeInterface,
                          llvm::FunctionCallee(function_type, callee),
                          args);
}

llvm::CallBase* CodeGeneratorARM64LLVM::CreateCriticalNativeCall(
    HInstruction* instruction,
    llvm::FunctionType* function_type,
    llvm::Value* callee,
    llvm::ArrayRef<llvm::Value*> args) {
  DCHECK_GE(args.size(), 1u) << "Missing caller method pointer in CriticalNative call";
  DCHECK(args[0]->getType() == GetMethodPointerType())
      << "First parameter of a dex call must be the callee method pointer";
  return CreateCallWithCC(instruction,
                          llvm::CallingConv::ARTCriticalNative,
                          llvm::FunctionCallee(function_type, callee),
                          args);
}

llvm::Function* CodeGeneratorARM64LLVM::GetPlaceholderFunction(PatchpointKind kind) const {
  switch (kind) {
    case PatchpointKind::kImplicitSuspendCheck:
      return placeholder_functions_.implicit_suspend_check;
    case PatchpointKind::kLoadGcRoot:
      return placeholder_functions_.load_gc_root;
    case PatchpointKind::kLoadBoolean:
      return placeholder_functions_.load_i1;
    case PatchpointKind::kLoadInt8:
      return placeholder_functions_.load_i8;
    case PatchpointKind::kLoadInt16:
      return placeholder_functions_.load_i16;
    case PatchpointKind::kLoadInt32:
      return placeholder_functions_.load_i32;
    case PatchpointKind::kLoadInt64:
      return placeholder_functions_.load_i64;
    case PatchpointKind::kLoadFloat32:
      return placeholder_functions_.load_f32;
    case PatchpointKind::kLoadFloat64:
      return placeholder_functions_.load_f64;
    case PatchpointKind::kLoadAcquireGcRoot:
      return placeholder_functions_.load_acquire_gc_root;
    case PatchpointKind::kLoadAcquireBoolean:
      return placeholder_functions_.load_acquire_i1;
    case PatchpointKind::kLoadAcquireInt8:
      return placeholder_functions_.load_acquire_i8;
    case PatchpointKind::kLoadAcquireInt16:
      return placeholder_functions_.load_acquire_i16;
    case PatchpointKind::kLoadAcquireInt32:
      return placeholder_functions_.load_acquire_i32;
    case PatchpointKind::kLoadAcquireInt64:
      return placeholder_functions_.load_acquire_i64;
    case PatchpointKind::kDiscardedLoad:
      return placeholder_functions_.discarded_load;
    case PatchpointKind::kStoreGcRoot:
      return placeholder_functions_.store_gc_root;
    case PatchpointKind::kStoreBoolean:
      return placeholder_functions_.store_i1;
    case PatchpointKind::kStoreInt8:
      return placeholder_functions_.store_i8;
    case PatchpointKind::kStoreInt16:
      return placeholder_functions_.store_i16;
    case PatchpointKind::kStoreInt32:
      return placeholder_functions_.store_i32;
    case PatchpointKind::kStoreInt64:
      return placeholder_functions_.store_i64;
    case PatchpointKind::kStoreFloat32:
      return placeholder_functions_.store_f32;
    case PatchpointKind::kStoreFloat64:
      return placeholder_functions_.store_f64;
    case PatchpointKind::kStoreReleaseGcRoot:
      return placeholder_functions_.store_release_gc_root;
    case PatchpointKind::kStoreReleaseBoolean:
      return placeholder_functions_.store_release_i1;
    case PatchpointKind::kStoreReleaseInt8:
      return placeholder_functions_.store_release_i8;
    case PatchpointKind::kStoreReleaseInt16:
      return placeholder_functions_.store_release_i16;
    case PatchpointKind::kStoreReleaseInt32:
      return placeholder_functions_.store_release_i32;
    case PatchpointKind::kStoreReleaseInt64:
      return placeholder_functions_.store_release_i64;
    case PatchpointKind::kNone:
    case PatchpointKind::kGCPointerAllocaMap:
    case PatchpointKind::kTryBoundaryStackReadClobber:
    case PatchpointKind::kReachabilityFence:
    case PatchpointKind::kCriticalNativeCall:
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
      LOG(FATAL) << "Unexpected patchpoint kind " << static_cast<int>(kind);
      UNREACHABLE();
  }
}

llvm::CallInst* CodeGeneratorARM64LLVM::CreatePatchpoint(
    llvm::Type* type,
    uint64_t id,
    uint32_t number_of_bytes,
    llvm::ArrayRef<llvm::Value*> arguments,
    llvm::ArrayRef<llvm::Value*> recorded_values,
    std::optional<llvm::ModRefInfo> memory_effects) {
  llvm::SmallVector<llvm::Value*, 8> patchpoint_arguments;
  patchpoint_arguments.reserve(4 + arguments.size() + recorded_values.size());
  // ID
  patchpoint_arguments.push_back(GetConstantInt(GetUint64Type(), id));
  // number_of_bytes
  patchpoint_arguments.push_back(GetConstantInt(GetUint32Type(), number_of_bytes));
  // function
  patchpoint_arguments.push_back(GetConstantZero(GetPointerType()));
  // number_of_arguments
  patchpoint_arguments.push_back(GetConstantInt(GetUint32Type(), arguments.size()));
  // arguments
  patchpoint_arguments.append(arguments.begin(), arguments.end());
  // extra recorded values
  patchpoint_arguments.append(recorded_values.begin(), recorded_values.end());

  llvm::CallInst* call = nullptr;
  if (type->isVoidTy()) {
    call =
        __ CreateIntrinsic(llvm::Intrinsic::experimental_patchpoint_void, {}, patchpoint_arguments);
  } else {
    call =
        __ CreateIntrinsic(llvm::Intrinsic::experimental_patchpoint, {type}, patchpoint_arguments);
  }
  call->setCallingConv(llvm::CallingConv::AnyReg);

  call->addFnAttr(llvm::Attribute::NoFree);
  call->addFnAttr(llvm::Attribute::NoRecurse);
  call->addFnAttr(llvm::Attribute::NoSync);
  call->addFnAttr(llvm::Attribute::NoUnwind);

  if (memory_effects.has_value()) {
    call->addFnAttr(llvm::Attribute::getWithMemoryEffects(GetLLVMContext(),
                                                          llvm::MemoryEffects(*memory_effects)));
  }

  return call;
}

llvm::CallInst* CodeGeneratorARM64LLVM::CreateStackMap(
    uint64_t id,
    llvm::ArrayRef<llvm::Value*> recorded_values,
    std::optional<llvm::ModRefInfo> memory_effects) {
  llvm::SmallVector<llvm::Value*, 8> stackmap_arguments;
  stackmap_arguments.reserve(2 + recorded_values.size());
  // ID
  stackmap_arguments.push_back(GetConstantInt(GetUint64Type(), id));
  // number_of_shadow_bytes
  stackmap_arguments.push_back(GetConstantInt(GetUint32Type(), 0));
  // extra recorded values
  stackmap_arguments.append(recorded_values.begin(), recorded_values.end());

  llvm::CallInst* call =
      __ CreateIntrinsic(llvm::Intrinsic::experimental_stackmap, {}, stackmap_arguments);

  call->addFnAttr(llvm::Attribute::NoFree);
  call->addFnAttr(llvm::Attribute::NoRecurse);
  call->addFnAttr(llvm::Attribute::NoSync);
  call->addFnAttr(llvm::Attribute::NoUnwind);

  if (memory_effects.has_value()) {
    call->addFnAttr(llvm::Attribute::getWithMemoryEffects(GetLLVMContext(),
                                                          llvm::MemoryEffects(*memory_effects)));
  }

  return call;
}

llvm::BranchInst* CodeGeneratorARM64LLVM::CreateBranchIfTrue(llvm::Value* condition,
                                                             llvm::BasicBlock* true_block) {
  llvm::BasicBlock* continue_block = CreateBasicBlock();
  llvm::BranchInst* br = __ CreateCondBr(condition, true_block, continue_block);
  __ SetInsertPoint(continue_block);
  return br;
}

llvm::BranchInst* CodeGeneratorARM64LLVM::CreateBranchIfFalse(llvm::Value* condition,
                                                              llvm::BasicBlock* false_block) {
  llvm::BasicBlock* continue_block = CreateBasicBlock();
  llvm::BranchInst* br = __ CreateCondBr(condition, continue_block, false_block);
  __ SetInsertPoint(continue_block);
  return br;
}

llvm::Value* CodeGeneratorARM64LLVM::GetUndefCurrentMethodPointer() {
  return llvm::UndefValue::get(GetMethodPointerType());
}

llvm::Value* CodeGeneratorARM64LLVM::GetCurrentMethodPointerArgument() {
  return GetFunction()->getArg(kCurrentMethodIndex);
}

static llvm::MDNode* MergeScopes(llvm::MDNode* a, llvm::MDNode* b) {
  llvm::SmallVector<llvm::Metadata*> merged_scopes;
  merged_scopes.append(a->op_begin(), a->op_end());
  merged_scopes.append(b->op_begin(), b->op_end());

  // Sort the merged scopes.
  std::sort(merged_scopes.begin(), merged_scopes.end());
  // Remove duplicate elements.
  merged_scopes.erase(std::unique(merged_scopes.begin(), merged_scopes.end()), merged_scopes.end());

  return llvm::MDNode::get(a->getContext(), merged_scopes);
}

static void AddAliasScopeMetadata(llvm::Instruction* instruction, llvm::MDNode* scope) {
  if (llvm::MDNode* alias_metadata = instruction->getMetadata(llvm::LLVMContext::MD_alias_scope)) {
    scope = MergeScopes(alias_metadata, scope);
  }
  instruction->setMetadata(llvm::LLVMContext::MD_alias_scope, scope);
}

void CodeGeneratorARM64LLVM::AddStackAliasScopeMetadata(llvm::Instruction* instruction) {
  AddAliasScopeMetadata(instruction, metadata_.stack_scope);
}

void CodeGeneratorARM64LLVM::AddHeapAliasScopeMetadata(llvm::Instruction* instruction) {
  AddAliasScopeMetadata(instruction, metadata_.heap_scope);
}

void CodeGeneratorARM64LLVM::AddRuntimeAliasScopeMetadata(llvm::Instruction* instruction) {
  AddAliasScopeMetadata(instruction, metadata_.runtime_scope);
}

void CodeGeneratorARM64LLVM::AddThreadObjectAliasScopeMetadata(llvm::Instruction* instruction) {
  AddAliasScopeMetadata(instruction, metadata_.thread_object_scope);
}

static void AddNoaliasMetadata(llvm::Instruction* instruction, llvm::MDNode* scope) {
  if (llvm::MDNode* alias_metadata = instruction->getMetadata(llvm::LLVMContext::MD_noalias)) {
    scope = MergeScopes(alias_metadata, scope);
  }
  instruction->setMetadata(llvm::LLVMContext::MD_noalias, scope);
}

void CodeGeneratorARM64LLVM::AddStackNoaliasMetadata(llvm::Instruction* instruction) {
  AddNoaliasMetadata(instruction, metadata_.stack_noalias);
}

void CodeGeneratorARM64LLVM::AddHeapNoaliasMetadata(llvm::Instruction* instruction) {
  AddNoaliasMetadata(instruction, metadata_.heap_noalias);
}

void CodeGeneratorARM64LLVM::AddRuntimeNoaliasMetadata(llvm::Instruction* instruction) {
  AddNoaliasMetadata(instruction, metadata_.runtime_noalias);
}

void CodeGeneratorARM64LLVM::AddThreadObjectNoaliasMetadata(llvm::Instruction* instruction) {
  AddNoaliasMetadata(instruction, metadata_.thread_object_noalias);
}

void CodeGeneratorARM64LLVM::AddCallNoaliasMetadata(llvm::Instruction* instruction) {
  AddNoaliasMetadata(instruction, metadata_.call_noalias);
}

void CodeGeneratorARM64LLVM::DumpModule() const { module_->print(llvm::dbgs(), nullptr); }

void InstructionCodeGeneratorARM64LLVM::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                             llvm::CallBase* placeholder_call,
                                                             uint64_t id,
                                                             llvm::DomTreeUpdater* dom_tree_updater,
                                                             llvm::LoopInfo* loop_info) {
  if (instruction->IsNoOp()) {
    return;
  }

  llvm::SmallVector<llvm::OperandBundleDef, 1> deopt_bundle;
  std::optional<llvm::OperandBundleUse> placeholder_deopt_bundle =
      placeholder_call == nullptr ? std::nullopt
                                  : placeholder_call->getOperandBundle(llvm::LLVMContext::OB_deopt);
  if (placeholder_deopt_bundle.has_value()) {
    std::vector<llvm::Value*> deopt_values(placeholder_deopt_bundle->Inputs.begin(),
                                           placeholder_deopt_bundle->Inputs.end());
    deopt_bundle.emplace_back("deopt", std::move(deopt_values));
  }

  if (codegen_->CanUseImplicitSuspendCheck()) {
    // Since an implicit suspend check also needs a stack map entry, we represent it using a
    // @llvm.experimental.patchpoint intrinsic in the final IR. In order to get the list of live
    // heap references at the suspend check point, we take advantage of the RewriteStatepointsForGC
    // pass, which rewrites the following function call to a @llvm.experimental.gc.statepoint
    // intrinsic and explicit relocations. This statepoint is then transformed into a patchpoint by
    // the RewritePatchpoints pass defined in this file.
    //
    // When parsing the stack map section of the generated object file, we can then patch the `nop`
    // instruction emitted by @llvm.experimental.patchpoint to the actual suspend check instruction
    // `ldr x21, [x21]`.
    llvm::Function* placeholder_function =
        codegen_->GetPlaceholderFunction(PatchpointKind::kImplicitSuspendCheck);
    DCHECK(!instruction->CanThrowIntoCatchBlock());
    llvm::CallBase* call = codegen_->CreateCallOrInvoke(instruction,
                                                        placeholder_function,
                                                        {codegen_->GetUndefCurrentMethodPointer()},
                                                        nullptr,
                                                        false,
                                                        deopt_bundle);
    call->addFnAttr(llvm::Attribute::get(codegen_->GetLLVMContext(),
                                         "statepoint-num-patch-bytes",
                                         std::to_string(kImplicitSuspendCheckPatchSize)));
    codegen_->SetStatepointID(instruction, call, id);
    return;
  }

  llvm::Type* flags_type = GetLLVMType(DataType::Type::kUint32);
  llvm::Value* loaded_flags = CreateLoadFromThreadPointer(
      flags_type, Thread::ThreadFlagsOffset<kArm64PointerSize>().SizeValue());
  llvm::Value* compare_flags =
      GetConstantInt(flags_type, Thread::SuspendOrCheckpointRequestFlags());
  llvm::Value* set_flags = __ CreateAnd(loaded_flags, compare_flags);
  llvm::Value* any_flags_set = __ CreateICmpNE(set_flags, GetConstantZero(flags_type));

  llvm::Instruction* slow_path_terminator =
      llvm::SplitBlockAndInsertIfThen(any_flags_set,
                                      __ GetInsertPoint(),
                                      /* Unreachable= */ false,
                                      /* BranchWeights= */ nullptr,
                                      /* DTU= */ dom_tree_updater,
                                      /* LI= */ loop_info,
                                      /* ThenBlock= */ nullptr);
  DCHECK(slow_path_terminator != nullptr);
  llvm::BasicBlock* slow_path_block = slow_path_terminator->getParent();
  llvm::Instruction* br_inst = __ GetInsertBlock() -> getTerminator();
  DCHECK(llvm::isa<llvm::BranchInst>(br_inst));
  llvm::BranchInst* br = llvm::cast<llvm::BranchInst>(br_inst);
  // Set branch weight of slow path here.
  ExpectFalseBranch(br);
  DCHECK(br->isConditional());
  DCHECK_EQ(br->getNumSuccessors(), 2u);
  DCHECK(br->getSuccessor(0) == slow_path_block);
  llvm::BasicBlock* successor_block = br->getSuccessor(1);

  uint32_t patchpoint_index = DecodePatchpointIndexFromID(id);
  id = EncodePatchpointID(PatchpointKind::kNone, patchpoint_index);

  // Manually emit the code for the suspend check, because slow path code emission has already
  // happened previously.
  __ SetInsertPoint(slow_path_block->getFirstInsertionPt());
  codegen_->SetInvokeRuntimeParametersAndReturnType({codegen_->GetUndefCurrentMethodPointer()},
                                                    codegen_->GetVoidType(),
                                                    llvm::CallingConv::ARTPreserveAll,
                                                    id,
                                                    deopt_bundle);
  codegen_->InvokeRuntime(kQuickTestSuspend, instruction);
  CheckEntrypointTypes<kQuickTestSuspend, void, void>();

  __ SetInsertPoint(successor_block->getFirstNonPHIIt());
}

InstructionCodeGeneratorARM64LLVM::InstructionCodeGeneratorARM64LLVM(
    HGraph* graph, CodeGeneratorARM64LLVM* codegen, llvm::IRBuilder<>* ir_builder)
    : InstructionCodeGenerator(graph, codegen),
      ir_builder_(ir_builder),
      codegen_(codegen),
      next_parameter_index_(kParametersBeginIndex) {}

void InstructionCodeGeneratorARM64LLVM::HandleFieldGet(HInstruction* instruction,
                                                       const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());
  uint32_t receiver_input = 0;
  llvm::Value* base = GetValue(instruction->InputAt(receiver_input));
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();
  DCHECK_EQ(DataType::Size(field_info.GetFieldType()), DataType::Size(instruction->GetType()));
  DataType::Type load_type = instruction->GetType();

  llvm::Value* result = nullptr;

  if (load_type == DataType::Type::kReference && codegen_->EmitBakerReadBarrier()) {
    LOG(FATAL) << "Baker read barrier encountered in LLVM code generator.";
    // Object FieldGet with Baker's read barrier case.
    // /* HeapReference<Object> */ out = *(base + offset)
    // Note that potential implicit null checks are handled in this
    // CodeGeneratorARM64::GenerateFieldLoadWithBakerReadBarrier call.
    /*
    result = codegen_->GenerateFieldLoadWithBakerReadBarrier(instruction,
                                                             base,
                                                             offset,
                                                             / * needs_null_check= * / true,
                                                             field_info.IsVolatile());
    */
  } else {
    // General case.
    llvm::Value* field = CreateGEP(base, offset);
    if (field_info.IsVolatile()) {
      // Note that a potential implicit null check is handled in this
      // CodeGeneratorARM64::LoadVolatile call.
      // NB: LoadVolatile will record the pc info if needed.
      result = codegen_->LoadVolatile(instruction,
                                      load_type,
                                      field,
                                      /* needs_null_check= */ true);
    } else {
      // codegen_->Load(load_type, OutputCPURegister(instruction), field);
      // codegen_->MaybeRecordImplicitNullCheck(instruction);
      result = codegen_->CreateLoadMaybeWithImplicitNullCheck(
          instruction, GetLLVMType(load_type), field);
    }
    if (load_type == DataType::Type::kReference) {
      // If read barriers are enabled, emit read barriers other than
      // Baker's using a slow path (and also unpoison the loaded
      // reference, if heap poisoning is enabled).
      result = codegen_->MaybeGenerateReadBarrierSlow(instruction, result, base, offset);
    }
  }

  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::HandleFieldSet(HInstruction* instruction,
                                                       const FieldInfo& field_info,
                                                       bool value_can_be_null,
                                                       WriteBarrierKind write_barrier_kind) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  // Register obj = InputRegisterAt(instruction, 0);
  llvm::Value* obj = GetValue(instruction->InputAt(0));
  // CPURegister value = InputCPURegisterOrZeroRegAt(instruction, 1);
  llvm::Value* value = GetValue(instruction->InputAt(1));
  // CPURegister source = value;
  llvm::Value* source = value;
  Offset offset = field_info.GetFieldOffset();
  DataType::Type field_type = field_info.GetFieldType();

  if (kPoisonHeapReferences && field_type == DataType::Type::kReference) {
    source = codegen_->PoisonHeapReference(source);
  }

  bool is_value_signed = !DataType::IsUnsignedType(instruction->InputAt(1)->GetType());
  if (field_info.IsVolatile()) {
    llvm::Value* obj_offset = CreateGEP(obj, offset.SizeValue());
    codegen_->StoreVolatile(
        instruction, field_type, is_value_signed, source, obj_offset, /* needs_null_check= */ true);
  } else {
    // codegen_->Store(field_type, source, HeapOperand(obj, offset));
    // codegen_->MaybeRecordImplicitNullCheck(instruction);
    codegen_->CreateStoreMaybeWithImplicitNullCheck(
        instruction, field_type, is_value_signed, source, obj, offset.Int32Value());
  }

  const bool needs_write_barrier =
      codegen_->StoreNeedsWriteBarrier(field_type, instruction->InputAt(1), write_barrier_kind);

  if (needs_write_barrier) {
    // TODO(solanes): If we do a `HuntForOriginalReference` call to the value in WBE, we will be
    // able to DCHECK that the write_barrier_kind is kBeingReliedOn when Register(value).IsZero(),
    // and we could remove the `!Register(value).IsZero()` from below.
    bool value_is_zero = llvm::isa<llvm::ConstantPointerNull>(value) ||
                         (llvm::isa<llvm::ConstantInt>(value) &&
                          llvm::cast<llvm::ConstantInt>(value)->getZExtValue() == 0) ||
                         (llvm::isa<llvm::ConstantFP>(value) &&
                          // Only +0.0 has a zero bit pattern
                          llvm::cast<llvm::ConstantFP>(value)->getValue().isPosZero());
    codegen_->MaybeMarkGCCard(obj,
                              value,
                              value_can_be_null &&
                                  write_barrier_kind == WriteBarrierKind::kEmitNotBeingReliedOn &&
                                  !value_is_zero);
  } else if (codegen_->ShouldCheckGCCard(field_type, instruction->InputAt(1), write_barrier_kind)) {
    codegen_->CheckGCCardIsValid(obj);
  }
}

std::pair<llvm::Value*, llvm::Value*> InstructionCodeGeneratorARM64LLVM::NormalizeBinaryOperands(
    DataType::Type result_type, HInstruction* lhs, HInstruction* rhs) {
  llvm::Value* lhs_value = GetValue(lhs, result_type);
  llvm::Value* rhs_value = GetValue(rhs, result_type);

  return {lhs_value, rhs_value};
}

void InstructionCodeGeneratorARM64LLVM::HandleBinaryOp(HBinaryOperation* instr) {
  DataType::Type type = instr->GetType();
  auto [lhs, rhs] = NormalizeBinaryOperands(type, instr->GetLeft(), instr->GetRight());

  llvm::Value* result = nullptr;
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      DCHECK(instr->IsRor() || lhs->getType() == rhs->getType());
      if (instr->IsAdd()) {
        // __ Add(dst, lhs, rhs);
        result = __ CreateAdd(lhs, rhs);
      } else if (instr->IsAnd()) {
        // __ And(dst, lhs, rhs);
        result = __ CreateAnd(lhs, rhs);
      } else if (instr->IsOr()) {
        // __ Orr(dst, lhs, rhs);
        result = __ CreateOr(lhs, rhs);
      } else if (instr->IsSub()) {
        // __ Sub(dst, lhs, rhs);
        result = __ CreateSub(lhs, rhs);
      } else if (instr->IsRor()) {
        // __ Ror(dst, lhs, RegisterFrom(instr->GetLocations()->InAt(1), type));
        // Use the @llvm.fshr.*(...) instrinsic to get a rotate right.
        result = __ CreateIntrinsic(llvm::Intrinsic::fshr, {lhs->getType()}, {lhs, lhs, rhs});
      } else if (instr->IsRol()) {
        // __ Ror(dst, lhs, shift); or __ Ror(dst, lhs, negated);
        // where shift and negated are calculated accordingly
        // Use the @llvm.fshl.*(...) instrinsic to get a rotate left.
        result = __ CreateIntrinsic(llvm::Intrinsic::fshl, {lhs->getType()}, {lhs, lhs, rhs});
      } else if (instr->IsMin()) {
        // __ Cmp(lhs, rhs);
        // __ Csel(dst, lhs, rhs, lt);
        result = __ CreateBinaryIntrinsic(llvm::Intrinsic::smin, lhs, rhs);
      } else if (instr->IsMax()) {
        // __ Cmp(lhs, rhs);
        // __ Csel(dst, lhs, rhs, gt);
        result = __ CreateBinaryIntrinsic(llvm::Intrinsic::smax, lhs, rhs);
      } else {
        DCHECK(instr->IsXor());
        // __ Eor(dst, lhs, rhs);
        result = __ CreateXor(lhs, rhs);
      }
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      DCHECK(lhs->getType() == rhs->getType());
      if (instr->IsAdd()) {
        // __ Fadd(dst, lhs, rhs);
        result = __ CreateFAdd(lhs, rhs);
      } else if (instr->IsSub()) {
        // __ Fsub(dst, lhs, rhs);
        result = __ CreateFSub(lhs, rhs);
      } else if (instr->IsMin()) {
        // __ Fmin(dst, lhs, rhs);
        // NOTE: The @llvm.minimum.f*(...) intrinsic lowers to an `fmin` instruction, which is the
        // save behaviour as ART, but this doesn't match the behaviour of std::fmin(...). That
        // corresponds to @llvm.minnum.f*(...).
        result = __ CreateMinimum(lhs, rhs);
      } else if (instr->IsMax()) {
        // __ Fmax(dst, lhs, rhs);
        // NOTE: The @llvm.maximum.f*(...) intrinsic lowers to an `fmax` instruction, which is the
        // save behaviour as ART, but this doesn't match the behaviour of std::fmax(...). That
        // corresponds to @llvm.maxnum.f*(...).
        result = __ CreateMaximum(lhs, rhs);
      } else {
        LOG(FATAL) << "Unexpected floating-point binary operation";
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected binary operation type " << type;
  }
  DCHECK(result != nullptr);
  AddValue(instr, result);
}

void InstructionCodeGeneratorARM64LLVM::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr());
  DataType::Type type = instr->GetType();
  auto [lhs, rhs] = NormalizeBinaryOperands(type, instr->GetLeft(), instr->GetRight());
  // The shift amount is actually (rhs % type_size), not simply rhs.
  uint32_t type_size = DataType::Size(type);
  llvm::Value* type_size_value = GetConstantInt(rhs->getType(), 8 * type_size);
  rhs = __ CreateURem(rhs, type_size_value);

  llvm::Value* result = nullptr;
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      if (instr->IsShl()) {
        // __ Lsl(dst, lhs, rhs);
        result = __ CreateShl(lhs, rhs);
      } else if (instr->IsShr()) {
        // __ Asr(dst, lhs, rhs);
        result = __ CreateAShr(lhs, rhs);
      } else {
        // __ Lsr(dst, lhs, rhs);
        result = __ CreateLShr(lhs, rhs);
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected shift operation type " << type;
  }
  DCHECK(result != nullptr);
  AddValue(instr, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitAdd(HAdd* instruction) { HandleBinaryOp(instruction); }

void InstructionCodeGeneratorARM64LLVM::VisitAnd(HAnd* instruction) { HandleBinaryOp(instruction); }

void InstructionCodeGeneratorARM64LLVM::VisitRol(HRol* rol) { HandleBinaryOp(rol); }

void InstructionCodeGeneratorARM64LLVM::VisitBitwiseNegatedRight(HBitwiseNegatedRight* instr) {
  auto [lhs, rhs] = NormalizeBinaryOperands(instr->GetType(), instr->GetLeft(), instr->GetRight());
  llvm::Value* negated_rhs = __ CreateNot(rhs);

  llvm::Value* result = nullptr;
  switch (instr->GetOpKind()) {
    case HInstruction::kAnd:
      // __ Bic(dst, lhs, rhs);
      result = __ CreateAnd(lhs, negated_rhs);
      break;
    case HInstruction::kOr:
      // __ Orn(dst, lhs, rhs);
      result = __ CreateOr(lhs, negated_rhs);
      break;
    case HInstruction::kXor:
      // __ Eon(dst, lhs, rhs);
      result = __ CreateXor(lhs, negated_rhs);
      break;
    default:
      LOG(FATAL) << "Unreachable";
  }
  DCHECK(result != nullptr);
  AddValue(instr, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitDataProcWithShifterOp(
    HDataProcWithShifterOp* instruction) {
  DataType::Type type = instruction->GetType();
  HInstruction::InstructionKind kind = instruction->GetInstrKind();
  DCHECK(type == DataType::Type::kInt32 || type == DataType::Type::kInt64);
  auto [lhs, rhs] = NormalizeBinaryOperands(type, instruction->InputAt(0), instruction->InputAt(1));

  // Calculate the actual rhs value.
  HDataProcWithShifterOp::OpKind op_kind = instruction->GetOpKind();
  switch (op_kind) {
    case HDataProcWithShifterOp::kLSL:
      rhs = __ CreateShl(rhs, instruction->GetShiftAmount());
      break;
    case HDataProcWithShifterOp::kLSR:
      rhs = __ CreateLShr(rhs, instruction->GetShiftAmount());
      break;
    case HDataProcWithShifterOp::kASR:
      rhs = __ CreateAShr(rhs, instruction->GetShiftAmount());
      break;
    case HDataProcWithShifterOp::kUXTB:
      rhs = __ CreateTrunc(rhs, GetUint8Type());
      rhs = __ CreateZExt(rhs, lhs->getType());
      break;
    case HDataProcWithShifterOp::kUXTH:
      rhs = __ CreateTrunc(rhs, GetUint16Type());
      rhs = __ CreateZExt(rhs, lhs->getType());
      break;
    case HDataProcWithShifterOp::kUXTW:
      rhs = __ CreateTrunc(rhs, GetUint32Type());
      rhs = __ CreateZExt(rhs, lhs->getType());
      break;
    case HDataProcWithShifterOp::kSXTB:
      rhs = __ CreateTrunc(rhs, GetInt8Type());
      rhs = __ CreateSExt(rhs, lhs->getType());
      break;
    case HDataProcWithShifterOp::kSXTH:
      rhs = __ CreateTrunc(rhs, GetInt16Type());
      rhs = __ CreateSExt(rhs, lhs->getType());
      break;
    case HDataProcWithShifterOp::kSXTW:
      rhs = __ CreateTrunc(rhs, GetInt32Type());
      rhs = __ CreateSExt(rhs, lhs->getType());
      break;
  }

  llvm::Value* result = nullptr;
  switch (kind) {
    case HInstruction::kAdd:
      // __ Add(out, left, right_operand);
      result = __ CreateAdd(lhs, rhs);
      break;
    case HInstruction::kAnd:
      // __ And(out, left, right_operand);
      result = __ CreateAnd(lhs, rhs);
      break;
    case HInstruction::kNeg:
      DCHECK(instruction->InputAt(0)->AsConstant()->IsArithmeticZero());
      // __ Neg(out, right_operand);
      result = __ CreateNeg(rhs);
      break;
    case HInstruction::kOr:
      // __ Orr(out, left, right_operand);
      result = __ CreateOr(lhs, rhs);
      break;
    case HInstruction::kSub:
      // __ Sub(out, left, right_operand);
      result = __ CreateSub(lhs, rhs);
      break;
    case HInstruction::kXor:
      // __ Eor(out, left, right_operand);
      result = __ CreateXor(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unexpected operation kind: " << kind;
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitIntermediateAddress(
    HIntermediateAddress* instruction) {
  llvm::Value* address = GetValue(instruction->InputAt(0));
  llvm::Value* offset = GetValue(instruction->InputAt(1));
  llvm::Value* intermediate_address = CreateGEP(address, offset);
  AddValue(instruction, intermediate_address);
}

void InstructionCodeGeneratorARM64LLVM::VisitIntermediateAddressIndex(
    HIntermediateAddressIndex* instruction) {
  // Register index_reg = InputRegisterAt(instruction, 0);
  llvm::Value* index = GetValue(instruction->InputAt(0));
  uint32_t shift = instruction->GetShift()->AsIntConstant()->GetValue();
  DCHECK_LE(shift, 3u);
  uint32_t offset = instruction->GetOffset()->AsIntConstant()->GetValue();

  if (shift == 0) {
    // __ Add(OutputRegister(instruction), index_reg, offset);
  } else {
    // Register offset_reg = InputRegisterAt(instruction, 1);
    // __ Add(OutputRegister(instruction), offset_reg, Operand(index_reg, LSL, shift));
  }
  llvm::Value* shifted_index = __ CreateShl(index, shift);
  llvm::Value* result =
      __ CreateAdd(shifted_index, GetConstantInt(shifted_index->getType(), offset));
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitMultiplyAccumulate(HMultiplyAccumulate* instr) {
  llvm::Value* mul_left =
      GetValue(instr->InputAt(HMultiplyAccumulate::kInputMulLeftIndex), instr->GetType());
  llvm::Value* mul_right =
      GetValue(instr->InputAt(HMultiplyAccumulate::kInputMulRightIndex), instr->GetType());
  llvm::Value* accumulator =
      GetValue(instr->InputAt(HMultiplyAccumulate::kInputAccumulatorIndex), instr->GetType());

  // LLVM doesn't have an integer multiply-accumulate instruction or intrinsic, but the back-end is
  // able to optimize this sequence to a single machine instruction.
  llvm::Value* mul_result = __ CreateMul(mul_left, mul_right);

  llvm::Value* result = nullptr;
  if (instr->GetOpKind() == HInstruction::kAdd) {
    // __ Madd(res, mul_left, mul_right, accumulator);
    result = __ CreateAdd(mul_result, accumulator);
  } else {
    DCHECK(instr->GetOpKind() == HInstruction::kSub);
    // __ Msub(res, mul_left, mul_right, accumulator);
    result = __ CreateSub(accumulator, mul_result);
  }
  DCHECK(result != nullptr);
  AddValue(instr, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitArrayGet(HArrayGet* instruction) {
  DataType::Type type = instruction->GetType();
  llvm::Type* llvm_type = GetLLVMType(type);
  llvm::Value* obj = GetValue(instruction->InputAt(0));
  // We need to explicitly get an int32 value (or a wider integer), otherwise large unsigned values
  // of narrower types would be treated as negative offsets in LLVM IR.
  llvm::Value* index = GetValue(instruction->InputAt(1), DataType::Type::kInt32);
  uint32_t offset = CodeGenerator::GetArrayDataOffset(instruction);
  const bool maybe_compressed_char_at =
      mirror::kUseStringCompression && instruction->IsStringCharAt();

  // The non-Baker read barrier instrumentation of object ArrayGet instructions
  // does not support the HIntermediateAddress instruction.
  DCHECK(!((type == DataType::Type::kReference) &&
           instruction->GetArray()->IsIntermediateAddress() &&
           codegen_->EmitNonBakerReadBarrier()));

  llvm::Value* result = nullptr;
  if (type == DataType::Type::kReference && codegen_->EmitBakerReadBarrier()) {
    LOG(FATAL) << "Read barrier encountered in LLVM code generation";
  } else {
    // General case.
    // Register length;
    llvm::Value* length = nullptr;
    llvm::Type* length_type = GetInt32Type();
    if (maybe_compressed_char_at) {
      uint32_t count_offset = mirror::String::CountOffset().Uint32Value();
      if (instruction->GetArray()->IsIntermediateAddress()) {
        DCHECK_LT(count_offset, offset);
        int64_t adjusted_offset = static_cast<int64_t>(count_offset) - static_cast<int64_t>(offset);
        // Note that `adjusted_offset` is negative, so this will be a LDUR.
        // __ Ldr(length, MemOperand(obj.X(), adjusted_offset));
        length = codegen_->CreateLoadMaybeWithImplicitNullCheck(
            instruction, length_type, obj, adjusted_offset);
      } else {
        // __ Ldr(length, HeapOperand(obj, count_offset));
        length = codegen_->CreateLoadMaybeWithImplicitNullCheck(
            instruction, length_type, obj, count_offset);
      }
      // codegen_->MaybeRecordImplicitNullCheck(instruction);
    }

    // Register temp = temps.AcquireSameSizeAs(obj);
    llvm::Value* array_base = nullptr;
    if (instruction->GetArray()->IsIntermediateAddress()) {
      // We do not need to compute the intermediate address from the array: the
      // input instruction has done it already. See the comment in
      // `TryExtractArrayAccessAddress()`.
      if (kIsDebugBuild) {
        HIntermediateAddress* interm_addr = instruction->GetArray()->AsIntermediateAddress();
        DCHECK_EQ(interm_addr->GetOffset()->AsIntConstant()->GetValueAsUint64(), offset);
      }
      array_base = obj;
    } else {
      // __ Add(temp, obj, offset);
      array_base = CreateGEP(obj, offset);
    }
    if (maybe_compressed_char_at) {
      static_assert(static_cast<uint32_t>(mirror::StringCompressionFlag::kCompressed) == 0u,
                    "Expecting 0=compressed, 1=uncompressed");
      llvm::BasicBlock* uncompressed_block = codegen_->CreateBasicBlock();
      llvm::BasicBlock* compressed_block = codegen_->CreateBasicBlock();
      llvm::BasicBlock* done_block = codegen_->CreateBasicBlock();
      // __ Tbnz(length.W(), 0, &uncompressed_load);
      // Same as 'branch if (length & 1) != 0'
      llvm::Value* uncompressed_flag = __ CreateAnd(length, 1u);
      llvm::Value* is_uncompressed =
          __ CreateICmpNE(uncompressed_flag, GetConstantZero(uncompressed_flag->getType()));
      __ CreateCondBr(is_uncompressed, uncompressed_block, compressed_block);

      __ SetInsertPoint(compressed_block);
      // __ Ldrb(Register(OutputCPURegister(instruction)),
      //         HeapOperand(temp, XRegisterFrom(index), LSL, 0));
      llvm::Value* compressed_char_address = CreateGEP(array_base, index);
      llvm::Value* compressed_char = CreateLoad(GetUint8Type(), compressed_char_address);
      compressed_char = __ CreateZExt(compressed_char, llvm_type);
      // __ B(&done);
      __ CreateBr(done_block);

      // __ Bind(&uncompressed_load);
      __ SetInsertPoint(uncompressed_block);
      // __ Ldrh(Register(OutputCPURegister(instruction)),
      //         HeapOperand(temp, XRegisterFrom(index), LSL, 1));
      llvm::Value* uncompressed_char_address = CreateGEP(GetUint16Type(), array_base, index);
      llvm::Value* uncompressed_char = CreateLoad(GetUint16Type(), uncompressed_char_address);
      uncompressed_char = __ CreateZExt(uncompressed_char, llvm_type);
      __ CreateBr(done_block);

      // __ Bind(&done);
      __ SetInsertPoint(done_block);
      llvm::PHINode* result_phi = __ CreatePHI(llvm_type, 2);
      result_phi->addIncoming(compressed_char, compressed_block);
      result_phi->addIncoming(uncompressed_char, uncompressed_block);
      result = result_phi;
    } else {
      // source = HeapOperand(temp, XRegisterFrom(index), LSL, DataType::SizeShift(type));
      llvm::Value* source = CreateGEP(llvm_type, array_base, index);
      // codegen_->Load(type, OutputCPURegister(instruction), source);
      // codegen_->MaybeRecordImplicitNullCheck(instruction);
      result = codegen_->CreateLoadMaybeWithImplicitNullCheck(instruction, llvm_type, source);
    }

    if (type == DataType::Type::kReference) {
      static_assert(
          sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
          "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
      result = codegen_->MaybeGenerateReadBarrierSlow(instruction, result, obj, offset, index);
    }
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitArrayLength(HArrayLength* instruction) {
  uint32_t offset = CodeGenerator::GetArrayLengthOffset(instruction);
  llvm::Value* array = GetValue(instruction->InputAt(0));

  // __ Ldr(out, HeapOperand(InputRegisterAt(instruction, 0), offset));
  // codegen_->MaybeRecordImplicitNullCheck(instruction);
  DCHECK_EQ(instruction->GetType(), DataType::Type::kInt32);
  llvm::Value* length_load =
      codegen_->CreateLoadMaybeWithImplicitNullCheck(instruction, GetInt32Type(), array, offset);

  // Add range metadata to annotate the length as a non-negative signed i32 value.
  if (llvm::Instruction* length_load_inst = llvm::dyn_cast<llvm::Instruction>(length_load)) {
    llvm::Metadata* range_values[] = {
        llvm::ConstantAsMetadata::get(__ getInt32(0)),
        // NOTE: !range requires a half-open range, so we use int32_min as the upper bound.
        llvm::ConstantAsMetadata::get(__ getInt32(std::numeric_limits<int32_t>::min())),
    };
    llvm::MDNode* range_md = llvm::MDNode::get(codegen_->GetLLVMContext(), range_values);
    length_load_inst->setMetadata(llvm::LLVMContext::MD_range, range_md);
  }

  llvm::Value* length = length_load;
  // Mask out compression flag from String's array length.
  if (mirror::kUseStringCompression && instruction->IsStringLength()) {
    // __ Lsr(out.W(), out.W(), 1u);
    length = __ CreateLShr(length, 1);
  }
  AddValue(instruction, length);
}

void InstructionCodeGeneratorARM64LLVM::VisitArraySet(HArraySet* instruction) {
  DataType::Type value_type = instruction->GetComponentType();
  llvm::Type* llvm_value_type = GetLLVMType(value_type);
  bool needs_type_check = instruction->NeedsTypeCheck();
  const WriteBarrierKind write_barrier_kind = instruction->GetWriteBarrierKind();
  bool needs_write_barrier =
      codegen_->StoreNeedsWriteBarrier(value_type, instruction->GetValue(), write_barrier_kind);

  llvm::Value* array = GetValue(instruction->InputAt(0));
  // We need to explicitly get an int32 value (or a wider integer), otherwise large unsigned values
  // of narrower types would be treated as negative offsets in LLVM IR.
  llvm::Value* index = GetValue(instruction->InputAt(1), DataType::Type::kInt32);
  llvm::Value* value = GetValue(instruction->InputAt(2));
  llvm::Value* source = value;
  size_t offset = mirror::Array::DataOffset(DataType::Size(value_type)).Uint32Value();

  if (!needs_write_barrier) {
    if (codegen_->ShouldCheckGCCard(value_type, instruction->GetValue(), write_barrier_kind)) {
      codegen_->CheckGCCardIsValid(array);
    }

    DCHECK(!needs_type_check);
    llvm::Value* array_base = nullptr;
    if (instruction->GetArray()->IsIntermediateAddress()) {
      // We do not need to compute the intermediate address from the array: the
      // input instruction has done it already. See the comment in
      // `TryExtractArrayAccessAddress()`.
      if (kIsDebugBuild) {
        HIntermediateAddress* interm_addr = instruction->GetArray()->AsIntermediateAddress();
        DCHECK(interm_addr->GetOffset()->AsIntConstant()->GetValueAsUint64() == offset);
      }
      array_base = array;
    } else {
      // __ Add(array_base, array, offset);
      array_base = CreateGEP(array, offset);
    }
    llvm::Value* element_address = CreateGEP(llvm_value_type, array_base, index);

    if (kPoisonHeapReferences && value_type == DataType::Type::kReference) {
      source = codegen_->PoisonHeapReference(source);
    }

    // codegen_->Store(value_type, source, destination);
    // codegen_->MaybeRecordImplicitNullCheck(instruction);
    bool is_value_signed = !DataType::IsUnsignedType(instruction->GetValue()->GetType());
    codegen_->CreateStoreMaybeWithImplicitNullCheck(
        instruction, value_type, is_value_signed, source, element_address);
  } else {
    DCHECK(!instruction->GetArray()->IsIntermediateAddress());
    SlowPathCodeARM64LLVM* slow_path = nullptr;

    bool can_value_be_null = instruction->GetValueCanBeNull();
    llvm::BasicBlock* do_store_block = nullptr;
    if (can_value_be_null) {
      do_store_block = codegen_->CreateBasicBlock();
      llvm::BasicBlock* continue_block = codegen_->CreateBasicBlock();
      // __ Cbz(Register(value), &do_store);
      llvm::Value* is_null = __ CreateICmpEQ(value, GetConstantZero(value->getType()));
      __ CreateCondBr(is_null, do_store_block, continue_block);
      __ SetInsertPoint(continue_block);
    }

    if (needs_type_check) {
      llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
      llvm::BasicBlock* slow_path_exit_block = codegen_->CreateBasicBlock();
      slow_path = new (codegen_->GetScopedAllocator())
          ArraySetSlowPathARM64LLVM(instruction, slow_path_entry_block, slow_path_exit_block);
      codegen_->AddSlowPath(slow_path);

      const uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
      const uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
      const uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();

      // Note that when Baker read barriers are enabled, the type
      // checks are performed without read barriers.  This is fine,
      // even in the case where a class object is in the from-space
      // after the flip, as a comparison involving such a type would
      // not produce a false positive; it may of course produce a
      // false negative, in which case we would take the ArraySet
      // slow path.

      // /* HeapReference<Class> */ temp = array->klass_
      // __ Ldr(temp, HeapOperand(array, class_offset));
      // codegen_->MaybeRecordImplicitNullCheck(instruction);
      llvm::Value* array_class = codegen_->CreateLoadMaybeWithImplicitNullCheck(
          instruction, GetUncompressedGCPointerType(), array, class_offset);
      // GetAssembler()->MaybeUnpoisonHeapReference(temp);
      array_class = codegen_->MaybeUnpoisonHeapReference(array_class);

      // /* HeapReference<Class> */ temp = temp->component_type_
      // __ Ldr(temp, HeapOperand(temp, component_offset));
      llvm::Value* component_type =
          CreateLoadWithOffset(GetUncompressedGCPointerType(), array_class, component_offset);
      // /* HeapReference<Class> */ temp2 = value->klass_
      // __ Ldr(temp2, HeapOperand(Register(value), class_offset));
      llvm::Value* value_class =
          CreateLoadWithOffset(GetUncompressedGCPointerType(), value, class_offset);
      // If heap poisoning is enabled, no need to unpoison `temp`
      // nor `temp2`, as we are comparing two poisoned references.
      // __ Cmp(temp, temp2);
      llvm::Value* is_same_type = __ CreateICmpEQ(component_type, value_class);

      if (instruction->StaticTypeOfArrayIsObjectArray()) {
        llvm::BasicBlock* do_put_block = codegen_->CreateBasicBlock();
        // __ B(eq, &do_put);
        codegen_->CreateBranchIfTrue(is_same_type, do_put_block);
        // If heap poisoning is enabled, the `temp` reference has
        // not been unpoisoned yet; unpoison it now.
        // GetAssembler()->MaybeUnpoisonHeapReference(temp);
        component_type = codegen_->MaybeUnpoisonHeapReference(component_type);

        // /* HeapReference<Class> */ temp = temp->super_class_
        // __ Ldr(temp, HeapOperand(temp, super_offset));
        llvm::Value* super_class =
            CreateLoadWithOffset(GetUncompressedGCPointerType(), component_type, super_offset);
        // If heap poisoning is enabled, no need to unpoison
        // `temp`, as we are comparing against null below.
        // __ Cbnz(temp, slow_path->GetEntryLabel());
        llvm::Value* is_null =
            __ CreateICmpEQ(super_class, GetConstantZero(super_class->getType()));
        llvm::Instruction* br = __ CreateCondBr(is_null, do_put_block, slow_path_entry_block);
        ExpectTrueBranch(br);
        // __ Bind(&do_put);
        __ SetInsertPoint(do_put_block);
      } else {
        // __ B(ne, slow_path->GetEntryLabel());
        llvm::Instruction* br = codegen_->CreateBranchIfFalse(is_same_type, slow_path_entry_block);
        ExpectTrueBranch(br);
      }
    }

    if (can_value_be_null) {
      DCHECK(do_store_block != nullptr);
      // __ Bind(&do_store);
      __ CreateBr(do_store_block);
      __ SetInsertPoint(do_store_block);
    }

    DCHECK_NE(write_barrier_kind, WriteBarrierKind::kDontEmit);
    // TODO(solanes): The WriteBarrierKind::kEmitNotBeingReliedOn case should be able to skip this
    // write barrier when its value is null (without an extra cbz since we already checked if the
    // value is null for the type check). This will be done as a follow-up since it is a runtime
    // optimization that needs extra care.
    // TODO(solanes): We can also skip it for known zero values which are not relied on i.e. when
    // we have the Zero register as the value. If we do `HuntForOriginalReference` on the value
    // we'll resolve this.
    codegen_->MarkGCCard(array);

    source = codegen_->MaybePoisonHeapReference(source);

    // __ Add(temp_base, array, offset);
    llvm::Value* array_base = CreateGEP(array, offset);
    // destination = HeapOperand(temp_base, XRegisterFrom(index), LSL,
    //                           DataType::SizeShift(value_type));
    llvm::Value* destination_address = CreateGEP(llvm_value_type, array_base, index);

    {
      // __ Str(source, destination);
      if (can_value_be_null || !needs_type_check) {
        // codegen_->MaybeRecordImplicitNullCheck(instruction);
        DCHECK_EQ(value_type, DataType::Type::kReference);
        codegen_->CreateStoreMaybeWithImplicitNullCheck(
            instruction, value_type, /* is_value_signed= */ false, source, destination_address);
      } else {
        CreateStore(source, destination_address);
      }
    }

    if (slow_path != nullptr) {
      // __ Bind(slow_path->GetExitLabel());
      __ CreateBr(slow_path->GetExitBlock());
      __ SetInsertPoint(slow_path->GetExitBlock());
    }
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitBoundsCheck(HBoundsCheck* instruction) {
  // Location index_loc = locations->InAt(0);
  llvm::Value* index = GetValue(instruction->InputAt(0), DataType::Type::kInt32);
  // Location length_loc = locations->InAt(1);
  llvm::Value* length = GetValue(instruction->InputAt(1), DataType::Type::kInt32);

  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
  BoundsCheckSlowPathARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
      BoundsCheckSlowPathARM64LLVM(instruction, slow_path_entry_block);
  codegen_->AddSlowPath(slow_path);

  // This check works for both negative and positive index values.
  // __ Cmp(InputRegisterAt(instruction, cmp_first_input),
  //        InputOperandAt(instruction, cmp_second_input));
  llvm::Value* is_out_of_bounds = __ CreateICmpUGE(index, length);
  // __ B(slow_path->GetEntryLabel(), cond);
  llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_out_of_bounds, slow_path_entry_block);
  ExpectFalseBranch(br);
}

void InstructionCodeGeneratorARM64LLVM::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class is not null.
  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* slow_path_exit_block = codegen_->CreateBasicBlock();
  llvm::Value* class_ptr = GetValue(check->InputAt(0));

  llvm::BasicBlock* current_block = __ GetInsertBlock();
  __ SetInsertPoint(slow_path_exit_block);
  llvm::PHINode* result_phi = __ CreatePHI(class_ptr->getType(), 2);
  __ SetInsertPoint(current_block);

  SlowPathCodeARM64LLVM* slow_path =
      new (codegen_->GetScopedAllocator()) LoadClassSlowPathARM64LLVM(check->GetLoadClass(),
                                                                      check,
                                                                      slow_path_entry_block,
                                                                      slow_path_exit_block,
                                                                      result_phi,
                                                                      class_ptr);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path, class_ptr);
  result_phi->addIncoming(class_ptr, __ GetInsertBlock());
  __ SetInsertPoint(slow_path_exit_block);
  AddValue(check, result_phi);
}

static llvm::CmpInst::Predicate LLVMIntPredicate(IfCondition cond) {
  switch (cond) {
    case kCondEQ:
      return llvm::CmpInst::Predicate::ICMP_EQ;
    case kCondNE:
      return llvm::CmpInst::Predicate::ICMP_NE;
    case kCondLT:
      return llvm::CmpInst::Predicate::ICMP_SLT;
    case kCondLE:
      return llvm::CmpInst::Predicate::ICMP_SLE;
    case kCondGT:
      return llvm::CmpInst::Predicate::ICMP_SGT;
    case kCondGE:
      return llvm::CmpInst::Predicate::ICMP_SGE;
    case kCondB:
      return llvm::CmpInst::Predicate::ICMP_ULT;
    case kCondBE:
      return llvm::CmpInst::Predicate::ICMP_ULE;
    case kCondA:
      return llvm::CmpInst::Predicate::ICMP_UGT;
    case kCondAE:
      return llvm::CmpInst::Predicate::ICMP_UGE;
  }
}

static llvm::CmpInst::Predicate LLVMFPPredicate(IfCondition cond, bool gt_bias) {
  switch (cond) {
    case kCondEQ:
      return llvm::CmpInst::Predicate::FCMP_OEQ;  // Ordered and equal.
    case kCondNE:
      return llvm::CmpInst::Predicate::FCMP_UNE;  // Unordered or not equal.
    case kCondLT:
      return gt_bias ? llvm::CmpInst::Predicate::FCMP_OLT /* Ordered and less than. */
                     : llvm::CmpInst::Predicate::FCMP_ULT /* Unordered or less than. */;
    case kCondLE:
      return gt_bias ? llvm::CmpInst::Predicate::FCMP_OLE /* Ordered and less than or equal. */
                     : llvm::CmpInst::Predicate::FCMP_ULE /* Unordered or less than or equal. */;
    case kCondGT:
      return gt_bias ? llvm::CmpInst::Predicate::FCMP_UGT /* Unordered or greater than. */
                     : llvm::CmpInst::Predicate::FCMP_OGT /* Ordered and greater than. */;
    case kCondGE:
      return gt_bias ? llvm::CmpInst::Predicate::FCMP_UGE /* Unordered or greater than or equal. */
                     : llvm::CmpInst::Predicate::FCMP_OGE /* Ordered and greater than or equal. */;
    default:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitCompare(HCompare* compare) {
  DataType::Type compare_type = compare->GetComparisonType();
  auto [lhs, rhs] = NormalizeBinaryOperands(compare_type, compare->GetLeft(), compare->GetRight());

  llvm::Type* result_type = GetLLVMType(compare->GetType());
  llvm::Value* result = nullptr;
  //  0 if: left == right
  //  1 if: left  > right
  // -1 if: left  < right
  switch (compare_type) {
    case DataType::Type::kUint32:
    case DataType::Type::kUint64:
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      // __ Cmp(left, right);
      llvm::Value* not_equal = __ CreateICmpNE(lhs, rhs);
      llvm::CmpInst::Predicate lt_predicate = DataType::IsUnsignedType(compare_type)
                                                  ? llvm::CmpInst::ICMP_ULT
                                                  : llvm::CmpInst::ICMP_SLT;
      llvm::Value* less_than = __ CreateICmp(lt_predicate, lhs, rhs);
      // __ Cset(result, ne);          // result == +1 if NE or 0 otherwise
      result = __ CreateZExt(not_equal, result_type);
      llvm::Value* negated_result = __ CreateNeg(result);
      // __ Cneg(result, result, lt);  // result == -1 if LT or unchanged otherwise
      result = __ CreateSelect(less_than, negated_result, result);
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      // GenerateFcmp(compare);
      llvm::Value* not_equal = __ CreateFCmpUNE(lhs, rhs);
      llvm::CmpInst::Predicate lt_predicate = LLVMFPPredicate(kCondLT, compare->IsGtBias());
      llvm::Value* less_than = __ CreateFCmp(lt_predicate, lhs, rhs);
      // __ Cset(result, ne);
      result = __ CreateZExt(not_equal, result_type);
      llvm::Value* negated_result = __ CreateNeg(result);
      // __ Cneg(result, result, ARM64FPCondition(kCondLT, compare->IsGtBias()));
      result = __ CreateSelect(less_than, negated_result, result);
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented compare type " << compare_type;
  }
  DCHECK(result != nullptr);
  AddValue(compare, result);
}

void InstructionCodeGeneratorARM64LLVM::HandleCondition(HCondition* instruction) {
  IfCondition cond = instruction->GetCondition();
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));
  if (lhs->getType() != rhs->getType() ||
      (lhs->getType()->isIntegerTy() &&
       DataType::IsUnsignedType(instruction->InputAt(0)->GetType()))) {
    // Extend lhs to at least Int32
    DCHECK(lhs->getType()->isIntegerTy());
    DCHECK(rhs->getType()->isIntegerTy());
    if (lhs->getType() != GetInt32Type()) {
      DCHECK_LT(lhs->getType()->getIntegerBitWidth(), 32u);
      bool is_signed = !DataType::IsUnsignedType(instruction->InputAt(0)->GetType());
      lhs = __ CreateIntCast(lhs, GetInt32Type(), is_signed);
    }
    if (rhs->getType() != GetInt32Type()) {
      DCHECK_LT(rhs->getType()->getIntegerBitWidth(), 32u);
      bool is_signed = !DataType::IsUnsignedType(instruction->InputAt(1)->GetType());
      rhs = __ CreateIntCast(rhs, GetInt32Type(), is_signed);
    }
  }

  llvm::CmpInst::Predicate predicate = lhs->getType()->isFloatingPointTy()
                                           ? LLVMFPPredicate(cond, instruction->IsGtBias())
                                           : LLVMIntPredicate(cond);
  llvm::Value* result = __ CreateCmp(predicate, lhs, rhs);
  AddValue(instruction, result);
}

#define FOR_EACH_CONDITION_INSTRUCTION(M) \
  M(Equal)                                \
  M(NotEqual)                             \
  M(LessThan)                             \
  M(LessThanOrEqual)                      \
  M(GreaterThan)                          \
  M(GreaterThanOrEqual)                   \
  M(Below)                                \
  M(BelowOrEqual)                         \
  M(Above)                                \
  M(AboveOrEqual)
#define DEFINE_CONDITION_VISITORS(Name) \
  void InstructionCodeGeneratorARM64LLVM::Visit##Name(H##Name* comp) { HandleCondition(comp); }
FOR_EACH_CONDITION_INSTRUCTION(DEFINE_CONDITION_VISITORS)
#undef DEFINE_CONDITION_VISITORS
#undef FOR_EACH_CONDITION_INSTRUCTION

void InstructionCodeGeneratorARM64LLVM::VisitDiv(HDiv* div) {
  DataType::Type type = div->GetResultType();
  llvm::Value* lhs = GetValue(div->InputAt(0), type);
  llvm::Value* rhs = GetValue(div->InputAt(1), type);

  llvm::Value* result = nullptr;
  // NOTE: We need to handle overflow here explicitly, because in LLVM IR, signed division overflow
  // is undefined behaviour.
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      // GenerateIntDiv(div);
      // FIXME: Overflow is handled by negating both lhs and rhs if rhs == -1. This produces the
      // correct result in all cases, including INT_MIN / -1, which after negation becomes
      // INT_MIN / 1 == INT_MIN.
      // NOTE: This algorithm is likely better for i32 division than sign-extending the values to 64
      // bits and doing a 64 bit division. The execution time difference between a 32 bit and a 64
      // bit division can be significant, and likely more than the extra 'cmn', 'cneg', 'csinc'
      // instruction sequence we generate in this case.
      //
      // In armv8-a, the 'sdiv' instruction handles overflow in the desired way, but LLVM doesn't
      // generate a single sdiv for this sequence. This is likely a missed optimization.
      // NOTE: In LLVM there is an @llvm.aarch64.sdiv.*() intrinsic, which always produces an sdiv
      // instruction in the final machine code, but the optimizer doesn't seem to know its
      // semantics. This means that there's no constant folding for this intrinsic, and calls with
      // constant divisors don't get optimized into more efficient calculations.
      llvm::Value* is_rhs_neg_one = __ CreateICmpEQ(rhs, GetConstantInt(rhs->getType(), -1));
      llvm::Value* negated_lhs = __ CreateNeg(lhs);
      lhs = __ CreateSelect(is_rhs_neg_one, negated_lhs, lhs);
      llvm::Value* negated_rhs = __ CreateNeg(rhs);
      rhs = __ CreateSelect(is_rhs_neg_one, negated_rhs, rhs);
      result = __ CreateSDiv(lhs, rhs);
      break;
    }
    case DataType::Type::kFloat32: {
      // __ Fdiv(OutputFPRegister(div), InputFPRegisterAt(div, 0), InputFPRegisterAt(div, 1));
      result = __ CreateFDiv(lhs, rhs);
      break;
    }
    case DataType::Type::kFloat64: {
      // __ Fdiv(OutputFPRegister(div), InputFPRegisterAt(div, 0), InputFPRegisterAt(div, 1));
      result = __ CreateFDiv(lhs, rhs);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected div type " << type;
  }
  AddValue(div, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  llvm::Value* value = GetValue(instruction->InputAt(0));

  DataType::Type type = instruction->GetType();
  CHECK(DataType::IsIntegralType(type)) << "Unexpected type " << type << " for DivZeroCheck.";

  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();

  SlowPathCodeARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
      DivZeroCheckSlowPathARM64LLVM(instruction, slow_path_entry_block);
  codegen_->AddSlowPath(slow_path);
  // __ Cbz(InputRegisterAt(instruction, 0), slow_path->GetEntryLabel());
  llvm::Value* is_zero = __ CreateICmpEQ(value, GetConstantZero(value->getType()));
  llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_zero, slow_path_entry_block);
  ExpectFalseBranch(br);
}

void InstructionCodeGeneratorARM64LLVM::VisitDoubleConstant(HDoubleConstant* constant) {
  llvm::Value* llvm_constant = llvm::ConstantFP::get(GetFloat64Type(), constant->GetValue());
  AddValue(constant, llvm_constant);
}

void InstructionCodeGeneratorARM64LLVM::VisitExit([[maybe_unused]] HExit* exit) {
  llvm::Type* return_type = GetFunction()->getReturnType();
  if (return_type->isVoidTy()) {
    __ CreateRetVoid();
  } else {
    __ CreateRet(llvm::UndefValue::get(return_type));
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitFloatConstant(HFloatConstant* constant) {
  // NOTE: constant->GetValue() returns a float, but llvm::ConstantFP::get() takes a double.
  // This is the inteded way to create a float constant in LLVM, since every float value can be
  // represented exactly as a double value.
  llvm::Value* llvm_constant = llvm::ConstantFP::get(GetFloat32Type(), constant->GetValue());
  AddValue(constant, llvm_constant);
}

void InstructionCodeGeneratorARM64LLVM::HandleGoto(HInstruction* got, HBasicBlock* successor) {
  HBasicBlock* block = got->GetBlock();
  HLoopInformation* info = block->GetLoopInformation();
  if (info != nullptr && info->IsBackEdge(*block) && info->HasSuspendCheck()) {
    codegen_->MaybeIncrementHotness(/* is_frame_entry= */ false);
  }

  llvm::Instruction* br = __ CreateBr(codegen_->GetIRBasicBlock(successor));

  // Add a metdata node to loop back-edges with a patchpoint id if a suspend check is needed there.
  // NOTE: The exact value of the id is only needed for implicit suspend checks. For explicit checks
  // this metdata only marks the loop as one needing a suspend check at the back-edge.
  if (info != nullptr && info->IsBackEdge(*block) && info->HasSuspendCheck()) {
    HSuspendCheck* back_edge_suspend_check = info->GetSuspendCheck();
    // If the suspend check needs vreg information, we need to generate the check here explicitly,
    // because by the time we would generate it in the optimization pipeline, the values recorded in
    // the environment have been invalidated.
    if (!NeedsVregInfo(back_edge_suspend_check)) {
      uint64_t id = EncodePatchpointID(PatchpointKind::kImplicitSuspendCheck,
                                       codegen_->AddStackMapInfo(back_edge_suspend_check));
      llvm::MDNode* loop_metadata = CreateLoopIDMetadata(codegen_->GetLLVMContext(), id);
      br->setMetadata(llvm::LLVMContext::MD_loop, loop_metadata);
    }
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitGoto(HGoto* got) {
  HandleGoto(got, got->GetSuccessor());
  codegen_->SetLastIRBasicBlock(got->GetBlock(), __ GetInsertBlock());
}

void InstructionCodeGeneratorARM64LLVM::VisitTryBoundary(HTryBoundary* try_boundary) {
  if (try_boundary->IsEntry()) {
    uint64_t id = EncodePatchpointID(PatchpointKind::kTryBoundaryStackReadClobber, 0);
    llvm::CallInst* stack_read_clobber =
        codegen_->CreatePatchpoint(GetVoidType(), id, 0, {}, {}, llvm::ModRefInfo::Ref);
    codegen_->AddStackAliasScopeMetadata(stack_read_clobber);

    llvm::BasicBlock* catchswitch_block = codegen_->CreateTryBoundaryCatchSwitch(try_boundary);
    codegen_->SetTryBoundaryCatchSwitch(try_boundary, catchswitch_block);
  }

  HandleGoto(try_boundary, try_boundary->GetNormalFlowSuccessor());
  codegen_->SetLastIRBasicBlock(try_boundary->GetBlock(), __ GetInsertBlock());
}

llvm::BranchInst* InstructionCodeGeneratorARM64LLVM::GenerateTestAndBranch(
    HInstruction* instruction,
    size_t condition_input_index,
    llvm::BasicBlock* true_target,
    llvm::BasicBlock* false_target) {
  llvm::Value* cond = GetValue(instruction->InputAt(condition_input_index));
  // Compare `cond` to zero if it's not a boolean.
  if (cond->getType() != GetBooleanType()) {
    DCHECK(cond->getType()->isIntegerTy());
    cond = __ CreateICmpNE(cond, GetConstantZero(cond->getType()));
  }
  return __ CreateCondBr(cond, true_target, false_target);
}

void InstructionCodeGeneratorARM64LLVM::VisitIf(HIf* if_instr) {
  HBasicBlock* true_successor = if_instr->IfTrueSuccessor();
  HBasicBlock* false_successor = if_instr->IfFalseSuccessor();
  llvm::BasicBlock* true_target = codegen_->GetIRBasicBlock(true_successor);
  llvm::BasicBlock* false_target = codegen_->GetIRBasicBlock(false_successor);
  GenerateTestAndBranch(if_instr, /* condition_input_index= */ 0, true_target, false_target);
  codegen_->SetLastIRBasicBlock(if_instr->GetBlock(), __ GetInsertBlock());
}

void InstructionCodeGeneratorARM64LLVM::VisitDeoptimize([[maybe_unused]] HDeoptimize* deoptimize) {
  // NOTE: The ARM64 code generator uses a shared slow path for deoptimize instructions if possible.
  // We don't do this here, which may increase code size in the final binary.
  llvm::BasicBlock* slow_path_entry = codegen_->CreateBasicBlock();
  llvm::BasicBlock* continue_block = codegen_->CreateBasicBlock();
  SlowPathCodeARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
      DeoptimizationSlowPathARM64LLVM(deoptimize, slow_path_entry);
  codegen_->AddSlowPath(slow_path);
  llvm::Instruction* br = GenerateTestAndBranch(
      deoptimize, /* condition_input_index= */ 0, slow_path_entry, continue_block);
  ExpectFalseBranch(br);
  __ SetInsertPoint(continue_block);
}

void InstructionCodeGeneratorARM64LLVM::VisitShouldDeoptimizeFlag(HShouldDeoptimizeFlag* flag) {
  UNUSED(flag);
  TODO();
}

void InstructionCodeGeneratorARM64LLVM::VisitSelect(HSelect* select) {
  DataType::Type type = select->GetType();
  auto [false_value, true_value] =
      NormalizeBinaryOperands(type, select->GetFalseValue(), select->GetTrueValue());
  llvm::Value* cond = GetValue(select->GetCondition());

  if (cond->getType() != GetBooleanType()) {
    DCHECK(cond->getType()->isIntegerTy());
    cond = __ CreateICmpNE(cond, GetConstantZero(cond->getType()));
  }
  llvm::Value* selected_value = __ CreateSelect(cond, true_value, false_value);
  AddValue(select, selected_value);
}

void InstructionCodeGeneratorARM64LLVM::VisitNop(HNop*) {
  // The environment recording already happens when we're starting code generation for the catch
  // block, so we don't need to do anything here.
}

void CodeGeneratorARM64LLVM::IncreaseFrame([[maybe_unused]] size_t adjustment) {
  UNIMPLEMENTED(FATAL) << "IncreaseFrame not implemented";
}

void CodeGeneratorARM64LLVM::DecreaseFrame([[maybe_unused]] size_t adjustment) {
  UNIMPLEMENTED(FATAL) << "DecreaseFrame not implemented";
}

void CodeGeneratorARM64LLVM::GenerateNop() {}

void InstructionCodeGeneratorARM64LLVM::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorARM64LLVM::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction,
                 instruction->GetFieldInfo(),
                 instruction->GetValueCanBeNull(),
                 instruction->GetWriteBarrierKind());
}

void InstructionCodeGeneratorARM64LLVM::VisitInstanceOf(HInstanceOf* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  llvm::Value* obj = GetValue(instruction->InputAt(0));
  llvm::Value* cls = (type_check_kind == TypeCheckKind::kBitstringCheck)
                         ? nullptr
                         : GetValue(instruction->InputAt(1));
  const uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  const uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  const uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  const uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();
  const uint32_t iftable_offset = mirror::Class::IfTableOffset().Uint32Value();
  const uint32_t array_length_offset = mirror::Array::LengthOffset().Uint32Value();
  const uint32_t object_array_data_offset =
      mirror::Array::DataOffset(kHeapReferenceSize).Uint32Value();

  llvm::BasicBlock* done_block = codegen_->CreateBasicBlock();

  llvm::BasicBlock* current_block = __ GetInsertBlock();
  __ SetInsertPoint(done_block);
  DCHECK_EQ(instruction->GetType(), DataType::Type::kBool);
  llvm::PHINode* result_phi = __ CreatePHI(GetBooleanType(), 3);
  __ SetInsertPoint(current_block);
  llvm::Constant* false_val = llvm::ConstantInt::getFalse(codegen_->GetLLVMContext());
  llvm::Constant* true_val = llvm::ConstantInt::getTrue(codegen_->GetLLVMContext());

  // Return 0 if `obj` is null.
  // Avoid null check if we know `obj` is not null.
  if (instruction->MustDoNullCheck()) {
    llvm::BasicBlock* continue_block = codegen_->CreateBasicBlock();
    // __ Cbz(obj, &zero);
    llvm::Value* is_null = __ CreateICmpEQ(obj, GetConstantZero(obj->getType()));
    __ CreateCondBr(is_null, done_block, continue_block);
    result_phi->addIncoming(false_val, __ GetInsertBlock());
    __ SetInsertPoint(continue_block);
  }

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck: {
      ReadBarrierOption read_barrier_option = codegen_->ReadBarrierOptionForInstanceOf(instruction);
      CHECK_EQ(read_barrier_option, ReadBarrierOption::kWithoutReadBarrier)
          << "Read barrier encountered in LLVM code generator";
      // /* HeapReference<Class> */ out = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, read_barrier_option);
      // __ Cmp(out, cls);
      // __ Cset(out, eq);
      llvm::Value* is_equal = __ CreateICmpEQ(obj_class, cls);
      // __ B(&done);
      __ CreateBr(done_block);
      result_phi->addIncoming(is_equal, __ GetInsertBlock());
      __ SetInsertPoint(done_block);
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      ReadBarrierOption read_barrier_option = codegen_->ReadBarrierOptionForInstanceOf(instruction);
      CHECK_EQ(read_barrier_option, ReadBarrierOption::kWithoutReadBarrier)
          << "Read barrier encountered in LLVM code generator";
      // /* HeapReference<Class> */ out = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, read_barrier_option);
      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock();
      // __ Bind(&loop);
      __ CreateBr(loop_block);
      llvm::BasicBlock* before_loop_block = __ GetInsertBlock();
      __ SetInsertPoint(loop_block);
      llvm::PHINode* current_class = __ CreatePHI(obj_class->getType(), 2);
      current_class->addIncoming(obj_class, before_loop_block);
      // /* HeapReference<Class> */ out = out->super_class_
      llvm::Value* super_class =
          GenerateReferenceLoad(instruction, current_class, super_offset, read_barrier_option);
      // If `out` is null, we use it for the result, and jump to `done`.
      // __ Cbz(out, &done);
      llvm::Value* is_super_null =
          __ CreateICmpEQ(super_class, GetConstantZero(super_class->getType()));
      llvm::BasicBlock* loop_continue_block = codegen_->CreateBasicBlock();
      __ CreateCondBr(is_super_null, done_block, loop_continue_block);
      result_phi->addIncoming(false_val, __ GetInsertBlock());
      __ SetInsertPoint(loop_continue_block);
      // __ Cmp(out, cls);
      llvm::Value* is_equal = __ CreateICmpEQ(super_class, cls);
      // __ B(ne, &loop);
      __ CreateCondBr(is_equal, done_block, loop_block);
      current_class->addIncoming(super_class, __ GetInsertBlock());
      result_phi->addIncoming(true_val, __ GetInsertBlock());
      __ SetInsertPoint(done_block);
      break;
    }

    case TypeCheckKind::kClassHierarchyCheck: {
      ReadBarrierOption read_barrier_option = codegen_->ReadBarrierOptionForInstanceOf(instruction);
      CHECK_EQ(read_barrier_option, ReadBarrierOption::kWithoutReadBarrier)
          << "Read barrier encountered in LLVM code generator";
      // /* HeapReference<Class> */ out = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, read_barrier_option);
      // Walk over the class hierarchy to find a match.
      llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock();
      __ CreateBr(loop_block);
      llvm::BasicBlock* before_loop_block = __ GetInsertBlock();
      // __ Bind(&loop);
      __ SetInsertPoint(loop_block);
      llvm::PHINode* current_class = __ CreatePHI(obj_class->getType(), 2);
      current_class->addIncoming(obj_class, before_loop_block);
      // __ Cmp(out, cls);
      llvm::Value* is_equal = __ CreateICmpEQ(current_class, cls);
      // __ B(eq, &success);
      llvm::BasicBlock* loop_continue_block = codegen_->CreateBasicBlock();
      __ CreateCondBr(is_equal, done_block, loop_continue_block);
      result_phi->addIncoming(true_val, __ GetInsertBlock());
      __ SetInsertPoint(loop_continue_block);
      // /* HeapReference<Class> */ out = out->super_class_
      llvm::Value* super_class =
          GenerateReferenceLoad(instruction, current_class, super_offset, read_barrier_option);
      // __ Cbnz(out, &loop);
      llvm::Value* is_super_null =
          __ CreateICmpEQ(super_class, GetConstantZero(super_class->getType()));
      __ CreateCondBr(is_super_null, done_block, loop_block);
      current_class->addIncoming(super_class, __ GetInsertBlock());
      result_phi->addIncoming(false_val, __ GetInsertBlock());
      __ SetInsertPoint(done_block);
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      ReadBarrierOption read_barrier_option = codegen_->ReadBarrierOptionForInstanceOf(instruction);
      CHECK_EQ(read_barrier_option, ReadBarrierOption::kWithoutReadBarrier)
          << "Read barrier encountered in LLVM code generator";
      // /* HeapReference<Class> */ out = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, read_barrier_option);
      llvm::BasicBlock* inexact_check_block = codegen_->CreateBasicBlock();
      // Do an exact check.
      // __ Cmp(out, cls);
      llvm::Value* is_exact = __ CreateICmpEQ(obj_class, cls);
      // __ B(eq, &exact_check);
      __ CreateCondBr(is_exact, done_block, inexact_check_block);
      result_phi->addIncoming(true_val, __ GetInsertBlock());
      __ SetInsertPoint(inexact_check_block);
      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ out = out->component_type_
      llvm::Value* component_type =
          GenerateReferenceLoad(instruction, obj_class, component_offset, read_barrier_option);
      llvm::BasicBlock* continue_block = codegen_->CreateBasicBlock();
      // __ Cbz(out, &done);
      llvm::Value* is_null =
          __ CreateICmpEQ(component_type, GetConstantZero(component_type->getType()));
      __ CreateCondBr(is_null, done_block, continue_block);
      result_phi->addIncoming(false_val, __ GetInsertBlock());
      __ SetInsertPoint(continue_block);
      // __ Ldrh(out, HeapOperand(out, primitive_offset));
      llvm::Value* primitive =
          CreateLoadWithOffset(GetUint16Type(), component_type, primitive_offset);
      // __ Cbnz(out, &zero);
      llvm::Value* is_non_primitive =
          __ CreateICmpEQ(primitive, GetConstantInt(primitive->getType(), Primitive::kPrimNot));
      __ CreateBr(done_block);
      result_phi->addIncoming(is_non_primitive, __ GetInsertBlock());
      __ SetInsertPoint(done_block);
      break;
    }

    case TypeCheckKind::kArrayCheck: {
      // No read barrier since the slow path will retry upon failure.
      // /* HeapReference<Class> */ out = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, kWithoutReadBarrier);
      // __ Cmp(out, cls);
      llvm::Value* is_equal = __ CreateICmpEQ(obj_class, cls);
      // DCHECK(locations->OnlyCallsOnSlowPath());
      llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
      SlowPathCodeARM64LLVM* slow_path =
          new (codegen_->GetScopedAllocator()) TypeCheckSlowPathARM64LLVM(
              instruction, slow_path_entry_block, done_block, result_phi, /* is_fatal = */ false);
      codegen_->AddSlowPath(slow_path);
      // __ B(ne, slow_path->GetEntryLabel());
      __ CreateCondBr(is_equal, done_block, slow_path_entry_block);
      result_phi->addIncoming(true_val, __ GetInsertBlock());
      __ SetInsertPoint(done_block);
      break;
    }

    case TypeCheckKind::kInterfaceCheck: {
      ReadBarrierOption read_barrier_option = codegen_->ReadBarrierOptionForInstanceOf(instruction);
      CHECK_EQ(read_barrier_option, ReadBarrierOption::kWithoutReadBarrier)
          << "Read barrier encountered in LLVM code generator";
      // Fast-path without read barriers.
      // /* HeapReference<Class> */ temp = obj->klass_
      // __ Ldr(temp, HeapOperand(obj, class_offset));
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, kWithoutReadBarrier);
      // /* HeapReference<Class> */ temp = temp->iftable_
      // __ Ldr(temp, HeapOperand(temp, iftable_offset));
      llvm::Value* iftable =
          GenerateReferenceLoad(instruction, obj_class, iftable_offset, kWithoutReadBarrier);
      // Load the size of the `IfTable`. The `Class::iftable_` is never null.
      // __ Ldr(out, HeapOperand(temp, array_length_offset));
      llvm::Value* iftable_size =
          CreateLoadWithOffset(GetUint32Type(), iftable, array_length_offset);
      llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock();
      llvm::BasicBlock* loop_block_continue = codegen_->CreateBasicBlock();
      // __ Bind(&loop);
      __ CreateBr(loop_block);
      llvm::BasicBlock* before_loop_block = __ GetInsertBlock();
      __ SetInsertPoint(loop_block);
      llvm::PHINode* current_iftable = __ CreatePHI(iftable->getType(), 2);
      llvm::PHINode* current_iftable_size = __ CreatePHI(iftable_size->getType(), 2);
      current_iftable->addIncoming(iftable, before_loop_block);
      current_iftable_size->addIncoming(iftable_size, before_loop_block);
      // __ Cbz(out, &done);
      llvm::Value* is_iftable_size_zero =
          __ CreateICmpEQ(current_iftable_size, GetConstantZero(iftable_size->getType()));
      __ CreateCondBr(is_iftable_size_zero, done_block, loop_block_continue);
      result_phi->addIncoming(false_val, __ GetInsertBlock());
      __ SetInsertPoint(loop_block_continue);
      // __ Ldr(temp2, HeapOperand(temp, object_array_data_offset));
      // GetAssembler()->MaybeUnpoisonHeapReference(temp2);
      llvm::Value* current_class = GenerateReferenceLoad(
          instruction, current_iftable, object_array_data_offset, kWithoutReadBarrier);
      // Go to next interface.
      // __ Add(temp, temp, 2 * kHeapReferenceSize);
      llvm::Value* next_iftable = CreateGEP(current_iftable, 2 * kHeapReferenceSize);
      current_iftable->addIncoming(next_iftable, __ GetInsertBlock());
      // __ Sub(out, out, 2);
      llvm::Value* iftable_size_loop =
          __ CreateSub(current_iftable_size, GetConstantInt(current_iftable_size->getType(), 2));
      current_iftable_size->addIncoming(iftable_size_loop, __ GetInsertBlock());
      // Compare the classes and continue the loop if they do not match.
      // __ Cmp(cls, temp2);
      llvm::Value* is_equal = __ CreateICmpEQ(cls, current_class);
      __ CreateCondBr(is_equal, done_block, loop_block);
      // __ Mov(out, 1);
      result_phi->addIncoming(true_val, __ GetInsertBlock());
      __ SetInsertPoint(done_block);
      break;
    }

    case TypeCheckKind::kUnresolvedCheck: {
      // Note that we indeed only call on slow path, but we always go
      // into the slow path for the unresolved check case.
      //
      // We cannot directly call the InstanceofNonTrivial runtime
      // entry point without resorting to a type checking slow path
      // here (i.e. by calling InvokeRuntime directly), as it would
      // require to assign fixed registers for the inputs of this
      // HInstanceOf instruction (following the runtime calling
      // convention), which might be cluttered by the potential first
      // read barrier emission at the beginning of this method.
      //
      // TODO: Introduce a new runtime entry point taking the object
      // to test (instead of its class) as argument, and let it deal
      // with the read barrier issues. This will let us refactor this
      // case of the `switch` code as it was previously (with a direct
      // call to the runtime not using a type checking slow path).
      // This should also be beneficial for the other cases above.

      // TODO(LLVM): This could probably be simplified by calling the runtime entry point directly.
      // DCHECK(locations->OnlyCallsOnSlowPath());
      llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
      SlowPathCodeARM64LLVM* slow_path =
          new (codegen_->GetScopedAllocator()) TypeCheckSlowPathARM64LLVM(
              instruction, slow_path_entry_block, done_block, result_phi, /* is_fatal = */ false);
      codegen_->AddSlowPath(slow_path);
      // __ B(slow_path->GetEntryLabel());
      __ CreateBr(slow_path_entry_block);
      __ SetInsertPoint(done_block);
      break;
    }

    case TypeCheckKind::kBitstringCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, kWithoutReadBarrier);
      llvm::Value* is_instance = GenerateBitstringTypeCheckCompare(instruction, obj_class);
      __ CreateBr(done_block);
      result_phi->addIncoming(is_instance, __ GetInsertBlock());
      __ SetInsertPoint(done_block);
      break;
    }
  }

  DCHECK(__ GetInsertBlock() == done_block);
  AddValue(instruction, result_phi);
}

void InstructionCodeGeneratorARM64LLVM::VisitCheckCast(HCheckCast* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  llvm::Value* obj = GetValue(instruction->InputAt(0));
  llvm::Value* cls = (type_check_kind == TypeCheckKind::kBitstringCheck)
                         ? nullptr
                         : GetValue(instruction->InputAt(1));
  const uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  const uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  const uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  const uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();
  const uint32_t iftable_offset = mirror::Class::IfTableOffset().Uint32Value();
  const uint32_t array_length_offset = mirror::Array::LengthOffset().Uint32Value();
  const uint32_t object_array_data_offset =
      mirror::Array::DataOffset(kHeapReferenceSize).Uint32Value();

  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* done_block = codegen_->CreateBasicBlock();
  bool is_type_check_slow_path_fatal = codegen_->IsTypeCheckSlowPathFatal(instruction);
  SlowPathCodeARM64LLVM* type_check_slow_path = new (codegen_->GetScopedAllocator())
      TypeCheckSlowPathARM64LLVM(instruction,
                                 slow_path_entry_block,
                                 done_block,
                                 /* result_phi= */ nullptr,
                                 is_type_check_slow_path_fatal);
  codegen_->AddSlowPath(type_check_slow_path);

  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    // __ Cbz(obj, &done);
    llvm::Value* is_null = __ CreateICmpEQ(obj, GetConstantZero(obj->getType()));
    codegen_->CreateBranchIfTrue(is_null, done_block);
  }

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kArrayCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, kWithoutReadBarrier);

      // __ Cmp(temp, cls);
      llvm::Value* is_equal = __ CreateICmpEQ(obj_class, cls);
      // Jump to slow path for throwing the exception or doing a
      // more involved array check.
      // __ B(ne, type_check_slow_path->GetEntryLabel());
      __ CreateCondBr(is_equal, done_block, slow_path_entry_block);
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, kWithoutReadBarrier);

      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock();
      // __ Bind(&loop);
      __ CreateBr(loop_block);
      llvm::BasicBlock* loop_entry_block = __ GetInsertBlock();
      __ SetInsertPoint(loop_block);
      llvm::PHINode* current_class = __ CreatePHI(obj_class->getType(), 2);
      current_class->addIncoming(obj_class, loop_entry_block);
      // /* HeapReference<Class> */ temp = temp->super_class_
      llvm::Value* super_class =
          GenerateReferenceLoad(instruction, current_class, super_offset, kWithoutReadBarrier);

      // If the class reference currently in `temp` is null, jump to the slow path to throw the
      // exception.
      // __ Cbz(temp, type_check_slow_path->GetEntryLabel());
      llvm::Value* is_super_null =
          __ CreateICmpEQ(super_class, GetConstantZero(super_class->getType()));
      codegen_->CreateBranchIfTrue(is_super_null, slow_path_entry_block);
      // Otherwise, compare classes.
      //__ Cmp(temp, cls);
      llvm::Value* is_equal = __ CreateICmpEQ(super_class, cls);
      // __ B(ne, &loop);
      __ CreateCondBr(is_equal, done_block, loop_block);
      current_class->addIncoming(super_class, __ GetInsertBlock());
      break;
    }

    case TypeCheckKind::kClassHierarchyCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, kWithoutReadBarrier);

      // Walk over the class hierarchy to find a match.
      llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock();
      llvm::BasicBlock* before_loop_block = __ GetInsertBlock();
      // __ Bind(&loop);
      __ CreateBr(loop_block);
      __ SetInsertPoint(loop_block);
      llvm::PHINode* current_class = __ CreatePHI(obj_class->getType(), 2);
      current_class->addIncoming(obj_class, before_loop_block);
      // __ Cmp(temp, cls);
      llvm::Value* is_equal = __ CreateICmpEQ(current_class, cls);
      // __ B(eq, &done);
      codegen_->CreateBranchIfTrue(is_equal, done_block);

      // /* HeapReference<Class> */ temp = temp->super_class_
      llvm::Value* super_class =
          GenerateReferenceLoad(instruction, current_class, super_offset, kWithoutReadBarrier);

      // If the class reference currently in `temp` is not null, jump
      // back at the beginning of the loop.
      // __ Cbnz(temp, &loop);
      // Otherwise, jump to the slow path to throw the exception.
      // __ B(type_check_slow_path->GetEntryLabel());
      llvm::Value* is_super_valid =
          __ CreateICmpNE(super_class, GetConstantZero(super_class->getType()));
      __ CreateCondBr(is_super_valid, loop_block, slow_path_entry_block);
      current_class->addIncoming(super_class, __ GetInsertBlock());
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, kWithoutReadBarrier);

      // Do an exact check.
      // __ Cmp(temp, cls);
      llvm::Value* is_exact = __ CreateICmpEQ(obj_class, cls);
      // __ B(eq, &done);
      llvm::BasicBlock* non_exact_block = codegen_->CreateBasicBlock();
      __ CreateCondBr(is_exact, done_block, non_exact_block);
      __ SetInsertPoint(non_exact_block);

      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ temp = temp->component_type_
      llvm::Value* component_type =
          GenerateReferenceLoad(instruction, obj_class, component_offset, kWithoutReadBarrier);

      // If the component type is null, jump to the slow path to throw the exception.
      // __ Cbz(temp, type_check_slow_path->GetEntryLabel());
      llvm::Value* is_component_null =
          __ CreateICmpEQ(component_type, GetConstantZero(component_type->getType()));
      llvm::Instruction* br =
          codegen_->CreateBranchIfTrue(is_component_null, slow_path_entry_block);
      ExpectFalseBranch(br);
      // Otherwise, the object is indeed an array. Further check that this component type is not a
      // primitive type.
      // __ Ldrh(temp, HeapOperand(temp, primitive_offset));
      llvm::Value* primitive_kind =
          CreateLoadWithOffset(GetUint16Type(), component_type, primitive_offset);
      static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
      // __ Cbnz(temp, type_check_slow_path->GetEntryLabel());
      llvm::Value* is_primitive =
          __ CreateICmpNE(primitive_kind, GetConstantZero(primitive_kind->getType()));
      __ CreateCondBr(is_primitive, slow_path_entry_block, done_block);
      break;
    }

    case TypeCheckKind::kUnresolvedCheck:
      // We always go into the type check slow path for the unresolved check cases.
      //
      // We cannot directly call the CheckCast runtime entry point
      // without resorting to a type checking slow path here (i.e. by
      // calling InvokeRuntime directly), as it would require to
      // assign fixed registers for the inputs of this HInstanceOf
      // instruction (following the runtime calling convention), which
      // might be cluttered by the potential first read barrier
      // emission at the beginning of this method.
      // __ B(type_check_slow_path->GetEntryLabel());
      __ CreateBr(slow_path_entry_block);
      break;
    case TypeCheckKind::kInterfaceCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, kWithoutReadBarrier);

      // /* HeapReference<Class> */ temp = temp->iftable_
      llvm::Value* iftable =
          GenerateReferenceLoad(instruction, obj_class, iftable_offset, kWithoutReadBarrier);
      // Iftable is never null.
      // __ Ldr(WRegisterFrom(maybe_temp2_loc), HeapOperand(temp.W(), array_length_offset));
      llvm::Value* length = CreateLoadWithOffset(GetUint32Type(), iftable, array_length_offset);
      // Loop through the iftable and check if any class matches.
      llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock();
      llvm::BasicBlock* before_loop_block = __ GetInsertBlock();
      // __ Bind(&start_loop);
      __ CreateBr(loop_block);
      __ SetInsertPoint(loop_block);
      llvm::PHINode* size_it = __ CreatePHI(length->getType(), 2);
      size_it->addIncoming(length, before_loop_block);
      llvm::PHINode* iftable_it = __ CreatePHI(iftable->getType(), 2);
      iftable_it->addIncoming(iftable, before_loop_block);
      // __ Cbz(WRegisterFrom(maybe_temp2_loc), type_check_slow_path->GetEntryLabel());
      llvm::Value* is_size_it_zero = __ CreateICmpEQ(size_it, GetConstantZero(size_it->getType()));
      llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_size_it_zero, slow_path_entry_block);
      ExpectFalseBranch(br);
      // __ Ldr(WRegisterFrom(maybe_temp3_loc), HeapOperand(temp.W(), object_array_data_offset));
      llvm::Value* iftable_entry = CreateLoadWithOffset(
          GetUncompressedGCPointerType(), iftable_it, object_array_data_offset);
      iftable_entry = codegen_->MaybeUnpoisonHeapReference(iftable_entry);
      // Go to next interface.
      // __ Add(temp, temp, 2 * kHeapReferenceSize);
      llvm::Value* next_iftable_it = CreateGEP(iftable_it, 2 * kHeapReferenceSize);
      // __ Sub(WRegisterFrom(maybe_temp2_loc), WRegisterFrom(maybe_temp2_loc), 2);
      llvm::Value* next_size_it = __ CreateSub(size_it, GetConstantInt(size_it->getType(), 2));
      // Compare the classes and continue the loop if they do not match.
      // __ Cmp(cls, WRegisterFrom(maybe_temp3_loc));
      llvm::Value* is_equal = __ CreateICmpEQ(iftable_entry, cls);
      // __ B(ne, &start_loop);
      __ CreateCondBr(is_equal, done_block, loop_block);
      size_it->addIncoming(next_size_it, __ GetInsertBlock());
      iftable_it->addIncoming(next_iftable_it, __ GetInsertBlock());
      break;
    }

    case TypeCheckKind::kBitstringCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      llvm::Value* obj_class =
          GenerateReferenceLoad(instruction, obj, class_offset, kWithoutReadBarrier);
      llvm::Value* is_bitstring = GenerateBitstringTypeCheckCompare(instruction, obj_class);
      llvm::Instruction* br = __ CreateCondBr(is_bitstring, done_block, slow_path_entry_block);
      ExpectTrueBranch(br);
      break;
    }
  }
  // __ Bind(&done);
  // __ Bind(type_check_slow_path->GetExitLabel());
  __ SetInsertPoint(done_block);
}

void InstructionCodeGeneratorARM64LLVM::VisitIntConstant(HIntConstant* constant) {
  DCHECK_EQ(constant->GetType(), DataType::Type::kInt32);
  llvm::Value* llvm_constant = GetConstantInt(GetInt32Type(), constant->GetValue());
  AddValue(constant, llvm_constant);
}

void InstructionCodeGeneratorARM64LLVM::VisitNullConstant(HNullConstant* constant) {
  DCHECK_EQ(constant->GetType(), DataType::Type::kReference);
  llvm::Value* llvm_constant = GetConstantZero(GetUncompressedGCPointerType());
  AddValue(constant, llvm_constant);
}

void InstructionCodeGeneratorARM64LLVM::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  uint32_t number_of_arguments = invoke->GetNumberOfArguments();
  llvm::SmallVector<llvm::Value*> arguments;
  arguments.reserve(number_of_arguments + 2);

  // Comment from `LocationsBuilderARM64::VisitInvokeUnresolved`:
  // The trampoline uses the same calling convention as dex calling conventions,
  // except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
  // the method_idx.

  arguments.push_back(codegen_->GetUndefCurrentMethodPointer());
  // Method index.
  arguments.push_back(GetConstantInt(GetUint32Type(), invoke->GetMethodReference().index));
  for (uint32_t i = 0; i < number_of_arguments; ++i) {
    arguments.push_back(GetValue(invoke->InputAt(i)));
  }

  llvm::Type* return_type = GetLLVMType(invoke->GetType());
  codegen_->SetInvokeRuntimeParametersAndReturnType(
      arguments, return_type, llvm::CallingConv::ARTInvokeUnresolved);
  // We have to reimplement the logic of this function, because it accesses the instruction's
  // locations property, which is null with LLVM.
  // codegen_->GenerateInvokeUnresolvedRuntimeCall(invoke);
  QuickEntrypointEnum entrypoint = kQuickInvokeStaticTrampolineWithAccessCheck;
  switch (invoke->GetInvokeType()) {
    case kStatic:
      entrypoint = kQuickInvokeStaticTrampolineWithAccessCheck;
      break;
    case kDirect:
      entrypoint = kQuickInvokeDirectTrampolineWithAccessCheck;
      break;
    case kVirtual:
      entrypoint = kQuickInvokeVirtualTrampolineWithAccessCheck;
      break;
    case kSuper:
      entrypoint = kQuickInvokeSuperTrampolineWithAccessCheck;
      break;
    case kInterface:
      entrypoint = kQuickInvokeInterfaceTrampolineWithAccessCheck;
      break;
    case kPolymorphic:
    case kCustom:
      LOG(FATAL) << "Unexpected invoke type: " << invoke->GetInvokeType();
      UNREACHABLE();
  }
  codegen_->InvokeRuntime(entrypoint, invoke);
  if (!return_type->isVoidTy()) {
    AddValue(invoke, codegen_->GetInvokeRuntimeResult());
  }
}

void CodeGeneratorARM64LLVM::MaybeGenerateInlineCacheCheck(HInstruction* instruction,
                                                           [[maybe_unused]] llvm::Value* klass) {
  CHECK(!ProfilingInfoBuilder::IsInlineCacheUseful(instruction->AsInvoke(), this));
}

void InstructionCodeGeneratorARM64LLVM::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  llvm::Value* receiver = GetValue(invoke->InputAt(0));
  size_t class_offset = mirror::Object::ClassOffset().SizeValue();
  size_t entry_point =
      ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64PointerSize).SizeValue();

  // /* HeapReference<Class> */ temp = receiver->klass_
  // __ Ldr(temp.W(), HeapOperandFrom(receiver, class_offset));
  // codegen_->MaybeRecordImplicitNullCheck(invoke);
  llvm::Value* klass = codegen_->CreateLoadMaybeWithImplicitNullCheck(
      invoke, GetUncompressedGCPointerType(), receiver, class_offset);

  // Instead of simply (possibly) unpoisoning `temp` here, we should
  // emit a read barrier for the previous class reference load.
  // However this is not required in practice, as this is an
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  // GetAssembler()->MaybeUnpoisonHeapReference(temp.W());
  klass = codegen_->MaybeUnpoisonHeapReference(klass);

  // NOTE(LLVM): This shouldn't happen with LLVM.
  // If we're compiling baseline, update the inline cache.
  codegen_->MaybeGenerateInlineCacheCheck(invoke, klass);

  llvm::Value* callee_method = nullptr;
  // The register ip1 is required to be used for the hidden argument in
  // art_quick_imt_conflict_trampoline.
  if (invoke->GetHiddenArgumentLoadKind() == MethodLoadKind::kRecursive) {
    callee_method = GetValue(invoke->InputAt(invoke->GetNumberOfArguments() - 1));
    // If the load kind is through a runtime call, we will pass the method we
    // fetch the IMT, which will either be a no-op if we don't hit the conflict
    // stub, or will make us always go through the trampoline when there is a
    // conflict.
  } else if (invoke->GetHiddenArgumentLoadKind() != MethodLoadKind::kRuntimeCall) {
    callee_method = codegen_->LoadMethod(invoke->GetHiddenArgumentLoadKind(), invoke);
  }

  // __ Ldr(temp, MemOperand(temp, mirror::Class::ImtPtrOffset(kArm64PointerSize).Uint32Value()));
  llvm::Value* imt_ptr = CreateLoadWithOffset(
      GetPointerType(), klass, mirror::Class::ImtPtrOffset(kArm64PointerSize).Uint32Value());
  uint32_t method_offset =
      static_cast<uint32_t>(ImTable::OffsetOfElement(invoke->GetImtIndex(), kArm64PointerSize));
  // temp = temp->GetImtEntryAt(method_offset);
  // __ Ldr(temp, MemOperand(temp, method_offset));
  llvm::Value* imt_entry = CreateLoadWithOffset(GetMethodPointerType(), imt_ptr, method_offset);
  if (invoke->GetHiddenArgumentLoadKind() == MethodLoadKind::kRuntimeCall) {
    // We pass the method from the IMT in case of a conflict. This will ensure
    // we go into the runtime to resolve the actual method.
    // __ Mov(ip1, temp);
    callee_method = imt_entry;
  }

  uint32_t number_of_arguments = invoke->GetHiddenArgumentLoadKind() == MethodLoadKind::kRecursive
                                     ? invoke->GetNumberOfArguments() - 1
                                     : invoke->GetNumberOfArguments();
  llvm::SmallVector<llvm::Value*> arguments;
  arguments.reserve(number_of_arguments + 3);

  static_assert(kCurrentMethodIndex == 0);
  arguments.push_back(imt_entry);  // Passed in x0. FIXME: Is this needed? This holds when looking
                                   // at the disassembly, but that may be just a coincidence.
  arguments.push_back(callee_method);  // Passed in ip1.
  static_assert(kCallerMethodIndex == 1);
  arguments.push_back(codegen_->GetUndefCurrentMethodPointer());
  for (uint32_t i = 0; i < number_of_arguments; ++i) {
    arguments.push_back(GetValue(invoke->InputAt(i)));
  }
  llvm::SmallVector<llvm::Type*> argument_types = codegen_->GetParameterTypes(arguments);
  llvm::Type* return_type = GetLLVMType(invoke->GetType());
  llvm::FunctionType* function_type = llvm::FunctionType::get(return_type, argument_types, false);

  // lr = temp->GetEntryPoint();
  // __ Ldr(lr, MemOperand(temp, entry_point.Int32Value()));
  llvm::Value* function = CreateLoadWithOffset(GetPointerType(), imt_entry, entry_point);

  // FIXME: This is set at code_generator.cc:940 in `CodeGenerator::AllocateLocations()`.
  //
  // DCHECK(!IsLeafMethod());

  // lr();
  // __ blr(lr);
  // codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  llvm::CallBase* call =
      codegen_->CreateInvokeInterfaceCall(invoke, function_type, function, arguments);
  if (!call->getType()->isVoidTy()) {
    AddValue(invoke, call);
  }
  uint64_t id = EncodePatchpointID(PatchpointKind::kNone, codegen_->AddStackMapInfo(invoke));
  codegen_->SetStatepointID(invoke, call, id);
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke, CodeGeneratorARM64LLVM* codegen) {
  if (invoke->GetIntrinsic() != Intrinsics::kNone) {
    IntrinsicCodeGeneratorARM64LLVM intrinsic(codegen);
    return intrinsic.Dispatch(invoke);
  }
  return false;
}

HInvokeStaticOrDirect::DispatchInfo
CodeGeneratorARM64LLVM::GetSupportedInvokeStaticOrDirectDispatch(
    const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
    [[maybe_unused]] ArtMethod* method) {
  // On ARM64 we support all dispatch types.
  return desired_dispatch_info;
}

llvm::Value* CodeGeneratorARM64LLVM::LoadMethod(MethodLoadKind load_kind, HInvoke* invoke) {
  switch (load_kind) {
    case MethodLoadKind::kBootImageLinkTimePcRelative: {
      return NewMethodBootImageLinkTimePcRelativePatch(invoke->GetResolvedMethodReference());
    }
    case MethodLoadKind::kBootImageRelRo: {
      uint32_t boot_image_offset = GetBootImageOffset(invoke);
      return NewMethodBootImageRelRoPatch(boot_image_offset);
    }
    case MethodLoadKind::kAppImageRelRo: {
      DCHECK(GetCompilerOptions().IsAppImage());
      return NewAppImageMethodPatch(invoke->GetResolvedMethodReference());
    }
    case MethodLoadKind::kBssEntry: {
      return NewMethodBssEntryPatch(invoke->GetMethodReference());
    }
    case MethodLoadKind::kJitDirectAddress: {
      UNIMPLEMENTED(FATAL) << "JIT not implemented for LoadMethod";
      break;
    }
    case MethodLoadKind::kRuntimeCall: {
      // Test situation, don't do anything.
      break;
    }
    default: {
      LOG(FATAL) << "Load kind should have already been handled " << load_kind;
      UNREACHABLE();
    }
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

llvm::Value* CodeGeneratorARM64LLVM::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke) {
  // Make sure that ArtMethod* is passed in kArtMethodRegister as per the calling convention.
  // Location callee_method = temp;  // For all kinds except kRecursive, callee will be in temp.
  llvm::Value* result = nullptr;
  llvm::Value* callee_method = nullptr;
  switch (invoke->GetMethodLoadKind()) {
    case MethodLoadKind::kStringInit: {
      uint32_t offset =
          GetThreadOffset<kArm64PointerSize>(invoke->GetStringInitEntryPoint()).Int32Value();
      // temp = thread->string_init_entrypoint
      // __ Ldr(XRegisterFrom(temp), MemOperand(tr, offset));
      callee_method = CreateLoadFromThreadPointer(GetMethodPointerType(), offset);
      break;
    }
    case MethodLoadKind::kRecursive:
      // callee_method = invoke->GetLocations()->InAt(invoke->GetCurrentMethodIndex());
      callee_method = GetValue(invoke->InputAt(invoke->GetCurrentMethodIndex()));
      break;
    case MethodLoadKind::kRuntimeCall:
      result = GenerateStaticOrDirectRuntimeCall(invoke);
      return result;
    case MethodLoadKind::kBootImageLinkTimePcRelative:
      DCHECK(GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension());
      if (invoke->GetCodePtrLocation() == CodePtrLocation::kCallCriticalNative) {
        callee_method = NewMethodBootImageJniEntrypointPatch(invoke->GetResolvedMethodReference());
        break;
      }
      FALLTHROUGH_INTENDED;
    default:
      callee_method = LoadMethod(invoke->GetMethodLoadKind(), invoke);
      break;
  }

  auto get_arguments = [&]() -> llvm::SmallVector<llvm::Value*> {
    uint32_t number_of_arguments = invoke->GetNumberOfArguments();
    llvm::SmallVector<llvm::Value*> arguments;
    switch (invoke->GetCodePtrLocation()) {
      case CodePtrLocation::kCallSelf:
      case CodePtrLocation::kCallArtMethod:
        arguments.reserve(number_of_arguments + 2);
        // Callee method pointer, will be passed in x0.
        static_assert(kCurrentMethodIndex == 0);
        arguments.push_back(callee_method);
        // Current method pointer, will be placed in [sp].
        static_assert(kCallerMethodIndex == 1);
        arguments.push_back(GetUndefCurrentMethodPointer());
        break;
      case CodePtrLocation::kCallCriticalNative: {
        arguments.reserve(number_of_arguments + 1);
        // Current method pointer, will be passed in x15.
        llvm::Value* current_method = GetValue(invoke->InputAt(invoke->GetCurrentMethodIndex()));
        arguments.push_back(current_method);
        break;
      }
    }

    for (uint32_t i = 0; i < number_of_arguments; ++i) {
      arguments.push_back(GetValue(invoke->InputAt(i)));
    }

    return arguments;
  };

  switch (invoke->GetCodePtrLocation()) {
    case CodePtrLocation::kCallSelf: {
      llvm::SmallVector<llvm::Value*> arguments = get_arguments();

      DCHECK(!GetGraph()->HasShouldDeoptimizeFlag());
      // __ bl(&frame_entry_label_);
      // RecordPcInfo(invoke, invoke->GetDexPc(), slow_path);

      // Since this is a static call, we need to make sure the types match exactly. A type mismatch
      // can happen e.g. if we're passing a constant boolean (represented as a constant Int32) as an
      // argument.
      llvm::FunctionType* function_type = GetFunction()->getFunctionType();
      DCHECK_EQ(arguments.size(), function_type->getNumParams());
      for (size_t i = kParametersBeginIndex; i < arguments.size(); ++i) {
        if (function_type->getParamType(i) != arguments[i]->getType()) {
          DCHECK(function_type->getParamType(i)->isIntegerTy());
          DCHECK(arguments[i]->getType()->isIntegerTy());
          HInstruction* arg_value = invoke->InputAt(i - kParametersBeginIndex);
          bool is_signed = !DataType::IsUnsignedType(arg_value->GetType());
          arguments[i] = __ CreateIntCast(arguments[i], function_type->getParamType(i), is_signed);
        }
      }
      llvm::CallBase* call = CreateCallOrInvoke(invoke, GetFunction(), arguments);
      call->setCallingConv(GetFunction()->getCallingConv());
      result = call;
      llvm::Type* return_type = GetLLVMType(invoke->GetType());
      if (result->getType() != return_type) {
        DCHECK(result->getType() == GetInt32Type());
        DCHECK(return_type->isIntegerTy());
        DCHECK_LT(return_type->getIntegerBitWidth(), result->getType()->getIntegerBitWidth());
        result = __ CreateTrunc(result, return_type);
      }
      uint64_t id = EncodePatchpointID(PatchpointKind::kNone, AddStackMapInfo(invoke));
      SetStatepointID(invoke, call, id);
      break;
    }
    case CodePtrLocation::kCallCriticalNative: {
      llvm::SmallVector<llvm::Value*> arguments = get_arguments();
      llvm::SmallVector<llvm::Type*> argument_types = GetParameterTypes(arguments);
      llvm::Type* return_type = GetLLVMType(invoke->GetType());
      llvm::FunctionType* function_type =
          llvm::FunctionType::get(return_type, argument_types, false);

      if (invoke->GetMethodLoadKind() == MethodLoadKind::kBootImageLinkTimePcRelative) {
        // call_lr();
        llvm::CallBase* call =
            CreateCriticalNativeCall(invoke, function_type, callee_method, arguments);
        result = call;
        uint64_t id = EncodePatchpointID(PatchpointKind::kNone, AddStackMapInfo(invoke));
        SetStatepointID(invoke, call, id);
      } else {
        // LR = callee_method->ptr_sized_fields_.data_;  // EntryPointFromJni
        MemberOffset offset = ArtMethod::EntryPointFromJniOffset(kArm64PointerSize);
        // __ Ldr(lr, MemOperand(XRegisterFrom(callee_method), offset.Int32Value()));
        llvm::Value* func =
            CreateLoadWithOffset(GetPointerType(), callee_method, offset.Int32Value());
        // lr()
        // call_lr();
        llvm::CallBase* call = CreateCriticalNativeCall(invoke, function_type, func, arguments);
        result = call;
        uint64_t id = EncodePatchpointID(PatchpointKind::kNone, AddStackMapInfo(invoke));
        SetStatepointID(invoke, call, id);
      }
      break;
    }
    case CodePtrLocation::kCallArtMethod: {
      // LR = callee_method->ptr_sized_fields_.entry_point_from_quick_compiled_code_;
      MemberOffset offset = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64PointerSize);
      // __ Ldr(lr, MemOperand(XRegisterFrom(callee_method), offset.Int32Value()));
      llvm::Value* func =
          CreateLoadWithOffset(GetPointerType(), callee_method, offset.Int32Value());

      llvm::SmallVector<llvm::Value*> arguments = get_arguments();
      llvm::SmallVector<llvm::Type*> argument_types = GetParameterTypes(arguments);
      llvm::Type* return_type = GetLLVMType(invoke->GetType());
      llvm::FunctionType* function_type =
          llvm::FunctionType::get(return_type, argument_types, false);

      // lr()
      // call_lr();
      llvm::CallBase* call = CreateInvokeDexCall(invoke, function_type, func, arguments);
      result = call;
      uint64_t id = EncodePatchpointID(PatchpointKind::kNone, AddStackMapInfo(invoke));
      SetStatepointID(invoke, call, id);
      break;
    }
  }

  return result;

  // FIXME: This is set at code_generator.cc:940 in `CodeGenerator::AllocateLocations()`.
  //
  // DCHECK(!IsLeafMethod());
}

llvm::Value* CodeGeneratorARM64LLVM::GenerateStaticOrDirectRuntimeCall(
    HInvokeStaticOrDirect* invoke) {
  uint32_t number_of_arguments = invoke->GetNumberOfArguments();
  llvm::SmallVector<llvm::Value*> arguments;
  arguments.reserve(number_of_arguments + 2);

  arguments.push_back(GetUndefCurrentMethodPointer());
  // Method index.
  arguments.push_back(GetConstantInt(GetUint32Type(), invoke->GetMethodReference().index));
  for (uint32_t i = 0; i < number_of_arguments; ++i) {
    arguments.push_back(GetValue(invoke->InputAt(i)));
  }

  llvm::Type* return_type = GetLLVMType(invoke->GetType());
  // NOTE: Runtime calls use the same calling convention as InvokeUnresolved.
  SetInvokeRuntimeParametersAndReturnType(
      arguments, return_type, llvm::CallingConv::ARTInvokeUnresolved);

  QuickEntrypointEnum entrypoint = kQuickInvokeStaticTrampolineWithAccessCheck;
  switch (invoke->GetInvokeType()) {
    case kStatic:
      entrypoint = kQuickInvokeStaticTrampolineWithAccessCheck;
      break;
    case kDirect:
      entrypoint = kQuickInvokeDirectTrampolineWithAccessCheck;
      break;
    case kSuper:
      entrypoint = kQuickInvokeSuperTrampolineWithAccessCheck;
      break;
    case kVirtual:
    case kInterface:
    case kPolymorphic:
    case kCustom:
      LOG(FATAL) << "Unexpected invoke type: " << invoke->GetInvokeType();
      UNREACHABLE();
  }
  InvokeRuntime(entrypoint, invoke);
  return GetInvokeRuntimeResult(/* allow_void= */ true);
}

bool static NeedsZExt(DataType::Type type) {
  return (type == DataType::Type::kBool || type == DataType::Type::kUint8 ||
          type == DataType::Type::kUint16);
}

bool static NeedsSExt(DataType::Type type) {
  return (type == DataType::Type::kInt8 || type == DataType::Type::kInt16);
}

void CodeGeneratorARM64LLVM::AddAttributesToFunction(llvm::ArrayRef<DataType::Type> rt_pt) {
  DCHECK(!rt_pt.empty());
  const auto arg_size = function_->arg_size();
  DCHECK_GE(arg_size, 1U);  // at least this

  // rt_pt size is 1 + #params
  // args size is 1|2 + #params
  DCHECK_GE(arg_size, rt_pt.size());

  // going backwards up to the return type (not included)
  // becuase args contains either 1 or 2 extra elements at the beginning
  int args_idx = arg_size - 1;
  for (std::size_t i = rt_pt.size() - 1; i > 0; --i) {
    const auto type = rt_pt[i];
    if (NeedsZExt(type)) {
      function_->addParamAttr(args_idx, llvm::Attribute::ZExt);
    } else if (NeedsSExt(type)) {
      function_->addParamAttr(args_idx, llvm::Attribute::SExt);
    }
    --args_idx;
  }

  const auto return_type = rt_pt[0];
  if (NeedsZExt(return_type)) {
    function_->addRetAttr(llvm::Attribute::ZExt);
  } else if (NeedsSExt(return_type)) {
    function_->addRetAttr(llvm::Attribute::SExt);
  }
}

void CodeGeneratorARM64LLVM::AddAttributesToCall(HInstruction* instruction, llvm::CallBase* call) {
  if (call->getCallingConv() == llvm::CallingConv::ARTCriticalNative) {
    // Critical native calls use the regular C ABI, so we shouldn't add any sext or zext attributes
    // to arguments and return values.
    return;
  }

  HInvoke* as_invoke = instruction->AsInvokeOrNull();
  if (!as_invoke) {
    // Always add zext attribute to boolean arguments.
    unsigned number_of_arguments = call->arg_size();
    llvm::Type* boolean_type = GetBooleanType();
    for (unsigned i = 0; i < number_of_arguments; ++i) {
      if (call->getArgOperand(i)->getType() == boolean_type) {
        call->addParamAttr(i, llvm::Attribute::ZExt);
      }
    }
    return;
  }

  const uint32_t number_of_arguments = as_invoke->GetNumberOfArguments();

  bool is_patchpoint = call->getIntrinsicID() == llvm::Intrinsic::experimental_patchpoint ||
                       call->getIntrinsicID() == llvm::Intrinsic::experimental_patchpoint_void;
  unsigned arg_size = 0;
  if (is_patchpoint) {
    llvm::Value* arg_size_value = call->getArgOperand(3);
    DCHECK(llvm::isa<llvm::ConstantInt>(arg_size_value));
    // We add the first four meta arguments as well, because the start index will be determined as
    // arg_size - number_of_arguments.
    arg_size = 4 + llvm::cast<llvm::ConstantInt>(arg_size_value)->getZExtValue();
  } else {
    arg_size = call->arg_size();
  }
  DCHECK_GE(arg_size, number_of_arguments);

  // The LLVM call might have some extra arguments at the front, the number of which we can
  // determine by just taking the difference with the HIR argument count.
  unsigned llvm_arg_index_start = arg_size - number_of_arguments;
  // InvokeExact calls have the instruction->InputAt(0) value at the end of the argument list, so we
  // need to shift the start index by one. That argument has a GC pointer type, so it shouldn't
  // receive any attributes.
  if (instruction->IsInvokePolymorphic() &&
      instruction->AsInvokePolymorphic()->IsMethodHandleInvokeExact()) {
    llvm_arg_index_start -= 1;
  }

  for (uint32_t arg_index = 0, llvm_arg_index = llvm_arg_index_start;
       arg_index < number_of_arguments;
       ++arg_index, ++llvm_arg_index) {
    DataType::Type type = as_invoke->InputAt(arg_index)->GetType();
    if (NeedsZExt(type)) {
      call->addParamAttr(llvm_arg_index, llvm::Attribute::ZExt);
    } else if (NeedsSExt(type)) {
      call->addParamAttr(llvm_arg_index, llvm::Attribute::SExt);
    }
  }

  DataType::Type return_type = as_invoke->GetType();
  if (NeedsZExt(return_type)) {
    call->addRetAttr(llvm::Attribute::ZExt);
  } else if (NeedsSExt(return_type)) {
    call->addRetAttr(llvm::Attribute::SExt);
  }
}

llvm::Value* CodeGeneratorARM64LLVM::GenerateVirtualCall(HInvokeVirtual* invoke) {
  // Use the calling convention instead of the location of the receiver, as
  // intrinsics may have put the receiver in a different register. In the intrinsics
  // slow path, the arguments have been moved to the right place, so here we are
  // guaranteed that the receiver is the first register of the calling convention.
  //
  // InvokeDexCallingConvention calling_convention;

  uint32_t number_of_arguments = invoke->GetNumberOfArguments();
  llvm::SmallVector<llvm::Value*> arguments;
  arguments.reserve(number_of_arguments + 2);

  // Method pointer, which is set before CreateInvokeDexCall()
  static_assert(kCurrentMethodIndex == 0);
  arguments.push_back(nullptr);
  static_assert(kCallerMethodIndex == 1);
  arguments.push_back(GetUndefCurrentMethodPointer());
  for (uint32_t i = 0; i < number_of_arguments; ++i) {
    arguments.push_back(GetValue(invoke->InputAt(i)));
  }

  DCHECK(number_of_arguments >= 1);
  llvm::Value* receiver = arguments[2];  // = invoke->InputAt(0);

  size_t method_offset =
      mirror::Class::EmbeddedVTableEntryOffset(invoke->GetVTableIndex(), kArm64PointerSize)
          .SizeValue();
  size_t class_offset = mirror::Object::ClassOffset().SizeValue();
  size_t entry_point =
      ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64PointerSize).SizeValue();

  // /* HeapReference<Class> */ klass = receiver->klass_
  // MaybeRecordImplicitNullCheck(invoke);
  llvm::Value* klass = CreateLoadMaybeWithImplicitNullCheck(
      invoke, GetUncompressedGCPointerType(), receiver, class_offset);

  // Instead of simply (possibly) unpoisoning `klass` here, we should
  // emit a read barrier for the previous class reference load.
  // However this is not required in practice, as this is an
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  klass = MaybeUnpoisonHeapReference(klass);

  // If we're compiling baseline, update the inline cache.
  MaybeGenerateInlineCacheCheck(invoke, klass);

  // method = klass->GetMethodAt(method_offset);
  llvm::Value* method = CreateLoadWithOffset(GetMethodPointerType(), klass, method_offset);
  // func = method->GetEntryPoint();
  llvm::Value* func = CreateLoadWithOffset(GetPointerType(), method, entry_point);

  arguments[0] = method;
  llvm::SmallVector<llvm::Type*> argument_types = GetParameterTypes(arguments);
  llvm::Type* return_type = GetLLVMType(invoke->GetType());
  llvm::FunctionType* function_type = llvm::FunctionType::get(return_type, argument_types, false);

  // func(method, args...);
  llvm::CallBase* call = CreateInvokeDexCall(invoke, function_type, func, arguments);

  uint64_t id = EncodePatchpointID(PatchpointKind::kNone, AddStackMapInfo(invoke));
  SetStatepointID(invoke, call, id);
  // RecordPcInfo(invoke, invoke->GetDexPc(), slow_path);

  return call;
}

llvm::Value* CodeGeneratorARM64LLVM::GenerateInvokePolymorphicCall(HInvokePolymorphic* invoke) {
  uint32_t number_of_arguments = invoke->GetNumberOfArguments();
  llvm::SmallVector<llvm::Value*> arguments;

  arguments.reserve(1 + number_of_arguments);
  arguments.push_back(GetUndefCurrentMethodPointer());
  for (uint32_t i = 0; i < number_of_arguments; ++i) {
    arguments.push_back(GetValue(invoke->InputAt(i)));
  }

  llvm::Type* return_type = GetLLVMType(invoke->GetType());
  SetInvokeRuntimeParametersAndReturnType(
      arguments, return_type, llvm::CallingConv::ARTInvokePolymorphic);
  InvokeRuntime(kQuickInvokePolymorphic, invoke);

  return GetInvokeRuntimeResult(/* allow_void= */ true);
}

llvm::Value* CodeGeneratorARM64LLVM::GenerateUnresolvedFieldAccess(HInstruction* field_access,
                                                                   DataType::Type field_type,
                                                                   uint32_t field_index) {
  bool is_instance =
      field_access->IsUnresolvedInstanceFieldGet() || field_access->IsUnresolvedInstanceFieldSet();
  bool is_get =
      field_access->IsUnresolvedInstanceFieldGet() || field_access->IsUnresolvedStaticFieldGet();

  llvm::SmallVector<llvm::Value*, 4> arguments;
  arguments.push_back(GetUndefCurrentMethodPointer());
  // field_index is passed in x0.
  arguments.push_back(GetConstantInt(GetUint32Type(), field_index));

  if (is_instance) {
    // For instance field accesses, 'this' is passed in x1.
    arguments.push_back(GetValue(field_access->InputAt(0)));
  }

  llvm::Type* return_type = nullptr;
  if (is_get) {
    // The result will be returned in x0, even if it's a floating-point value.
    if (DataType::IsFloatingPointType(field_type)) {
      return_type = DataType::Is64BitType(field_type) ? GetInt64Type() : GetInt32Type();
    } else {
      return_type = GetLLVMType(field_type);
    }
  } else {
    return_type = GetVoidType();
    // The set value should be passed in x1 or x2, even if it's a floating-point value.
    llvm::Value* set_value = GetValue(field_access->InputAt(is_instance ? 1 : 0));
    if (DataType::IsFloatingPointType(field_type)) {
      llvm::Type* field_int_type =
          DataType::Is64BitType(field_type) ? GetInt64Type() : GetInt32Type();
      set_value = __ CreateBitCast(set_value, field_int_type);
    }
    arguments.push_back(set_value);
  }

  QuickEntrypointEnum entrypoint = kQuickSet8Static;  // Initialize to anything to avoid warnings.
  switch (field_type) {
    case DataType::Type::kBool:
      entrypoint = is_instance ? (is_get ? kQuickGetBooleanInstance : kQuickSet8Instance)
                               : (is_get ? kQuickGetBooleanStatic : kQuickSet8Static);
      break;
    case DataType::Type::kInt8:
      entrypoint = is_instance ? (is_get ? kQuickGetByteInstance : kQuickSet8Instance)
                               : (is_get ? kQuickGetByteStatic : kQuickSet8Static);
      break;
    case DataType::Type::kInt16:
      entrypoint = is_instance ? (is_get ? kQuickGetShortInstance : kQuickSet16Instance)
                               : (is_get ? kQuickGetShortStatic : kQuickSet16Static);
      break;
    case DataType::Type::kUint16:
      entrypoint = is_instance ? (is_get ? kQuickGetCharInstance : kQuickSet16Instance)
                               : (is_get ? kQuickGetCharStatic : kQuickSet16Static);
      break;
    case DataType::Type::kInt32:
    case DataType::Type::kFloat32:
      entrypoint = is_instance ? (is_get ? kQuickGet32Instance : kQuickSet32Instance)
                               : (is_get ? kQuickGet32Static : kQuickSet32Static);
      break;
    case DataType::Type::kReference:
      entrypoint = is_instance ? (is_get ? kQuickGetObjInstance : kQuickSetObjInstance)
                               : (is_get ? kQuickGetObjStatic : kQuickSetObjStatic);
      break;
    case DataType::Type::kInt64:
    case DataType::Type::kFloat64:
      entrypoint = is_instance ? (is_get ? kQuickGet64Instance : kQuickSet64Instance)
                               : (is_get ? kQuickGet64Static : kQuickSet64Static);
      break;
    default:
      LOG(FATAL) << "Invalid type " << field_type;
  }
  SetInvokeRuntimeParametersAndReturnType(arguments, return_type);
  InvokeRuntime(entrypoint, field_access);
  llvm::Value* result = GetInvokeRuntimeResult(/* allow_void= */ true);
  if (is_get && DataType::IsFloatingPointType(field_type)) {
    result = __ CreateBitCast(result, GetLLVMType(field_type));
  }
  return result;
}

void CodeGeneratorARM64LLVM::MoveFromReturnRegister([[maybe_unused]] Location trg,
                                                    [[maybe_unused]] DataType::Type type) {
  UNIMPLEMENTED(FATAL) << "MoveFromReturnRegister not implemented";
}

llvm::Value* CodeGeneratorARM64LLVM::NewClassBootImageRelRoPatch(uint32_t boot_image_offset) {
  uint64_t id = EncodePatchpointID(PatchpointKind::kLoadClassBootImageRelRo,
                                   AddStackMapInfo(nullptr, nullptr, boot_image_offset));
  return CreatePatchpoint(GetUncompressedGCPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

llvm::Value* CodeGeneratorARM64LLVM::NewBootImageIntrinsicPatch(uint32_t boot_image_reference) {
  uint64_t id = EncodePatchpointID(PatchpointKind::kLoadClassBootImageIntrinsic,
                                   AddStackMapInfo(nullptr, nullptr, boot_image_reference));
  // NOTE: We add a memory(none) attribute to this call, which is not entirely accurate, but as far
  //       as LLVM is concerned, this operation doesn't use memory.
  return CreatePatchpoint(GetUncompressedGCPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::NoModRef);
}

llvm::Value* CodeGeneratorARM64LLVM::NewMethodBootImageRelRoPatch(uint32_t boot_image_offset) {
  uint64_t id = EncodePatchpointID(PatchpointKind::kLoadMethodBootImageRelRo,
                                   AddStackMapInfo(nullptr, nullptr, boot_image_offset));
  // NOTE: This will actually create a 32-bit load, not a 64-bit load that we would expect from its
  //       type. This is because the boot image is in the low 4GiB and the entry is 32-bit.
  return CreatePatchpoint(GetMethodPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

llvm::Value* CodeGeneratorARM64LLVM::NewAppImageMethodPatch(MethodReference target_method) {
  uint64_t id =
      EncodePatchpointID(PatchpointKind::kLoadMethodAppImageRelRo,
                         AddStackMapInfo(nullptr, target_method.dex_file, target_method.index));
  // NOTE: This will actually create a 32-bit load, not a 64-bit load that we would expect from its
  //       type. This is because the app image is in the low 4GiB and the entry is 32-bit.
  return CreatePatchpoint(GetMethodPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

llvm::Value* CodeGeneratorARM64LLVM::NewMethodBootImageLinkTimePcRelativePatch(
    MethodReference target_method) {
  uint64_t id =
      EncodePatchpointID(PatchpointKind::kLoadMethodBootImageLinkTimePcRelative,
                         AddStackMapInfo(nullptr, target_method.dex_file, target_method.index));
  // NOTE: We add a memory(none) attribute to this call, which is not entirely accurate, but as far
  //       as LLVM is concerned, this operation doesn't use memory.
  return CreatePatchpoint(GetMethodPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::NoModRef);
}

llvm::Value* CodeGeneratorARM64LLVM::NewMethodBootImageJniEntrypointPatch(
    MethodReference target_method) {
  uint64_t id =
      EncodePatchpointID(PatchpointKind::kLoadMethodBootImageJni,
                         AddStackMapInfo(nullptr, target_method.dex_file, target_method.index));
  return CreatePatchpoint(GetPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

llvm::Value* CodeGeneratorARM64LLVM::NewStringBootImageRelRoPatch(uint32_t boot_image_offset) {
  uint64_t id = EncodePatchpointID(PatchpointKind::kLoadStringBootImageRelRo,
                                   AddStackMapInfo(nullptr, nullptr, boot_image_offset));
  return CreatePatchpoint(GetUncompressedGCPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

llvm::Value* CodeGeneratorARM64LLVM::NewMethodBssEntryPatch(MethodReference target_method) {
  uint64_t id =
      EncodePatchpointID(PatchpointKind::kLoadMethodBssEntry,
                         AddStackMapInfo(nullptr, target_method.dex_file, target_method.index));
  return CreatePatchpoint(GetMethodPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

llvm::Value* CodeGeneratorARM64LLVM::NewStringBootImageLinkTimePcRelativePatch(
    const DexFile& dex_file, const dex::StringIndex string_index) {
  uint64_t id = EncodePatchpointID(PatchpointKind::kLoadStringBootImageLinkTimePcRelative,
                                   AddStackMapInfo(nullptr, &dex_file, string_index.index_));
  // NOTE: We add a memory(none) attribute to this call, which is not entirely accurate, but as far
  //       as LLVM is concerned, this operation doesn't use memory.
  return CreatePatchpoint(GetUncompressedGCPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::NoModRef);
}

llvm::Value* CodeGeneratorARM64LLVM::NewBootImageTypePatch(const DexFile& dex_file,
                                                           dex::TypeIndex type_index) {
  uint64_t id = EncodePatchpointID(PatchpointKind::kLoadClassBootImageLinkTimePcRelative,
                                   AddStackMapInfo(nullptr, &dex_file, type_index.index_));
  // NOTE: We add a memory(none) attribute to this call, which is not entirely accurate, but as far
  //       as LLVM is concerned, this operation doesn't use memory.
  return CreatePatchpoint(GetUncompressedGCPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::NoModRef);
}

llvm::Value* CodeGeneratorARM64LLVM::NewAppImageTypePatch(const DexFile& dex_file,
                                                          dex::TypeIndex type_index) {
  uint64_t id = EncodePatchpointID(PatchpointKind::kLoadClassAppImageRelRo,
                                   AddStackMapInfo(nullptr, &dex_file, type_index.index_));
  return CreatePatchpoint(GetUncompressedGCPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

llvm::Value* CodeGeneratorARM64LLVM::NewBssEntryTypePatch(HLoadClass* load_class) {
  PatchpointKind kind = PatchpointKind::kNone;
  switch (load_class->GetLoadKind()) {
    case HLoadClass::LoadKind::kBssEntry:
      kind = PatchpointKind::kLoadClassBssEntry;
      break;
    case HLoadClass::LoadKind::kBssEntryPublic:
      kind = PatchpointKind::kLoadClassBssEntryPublic;
      break;
    case HLoadClass::LoadKind::kBssEntryPackage:
      kind = PatchpointKind::kLoadClassBssEntryPackage;
      break;
    default:
      LOG(FATAL) << "Unexpected load kind: " << load_class->GetLoadKind();
      UNREACHABLE();
  }
  const DexFile& dex_file = load_class->GetDexFile();
  dex::TypeIndex type_index = load_class->GetTypeIndex();
  uint64_t id = EncodePatchpointID(kind, AddStackMapInfo(nullptr, &dex_file, type_index.index_));
  // NOTE: We add a memory(none) attribute to this call, which is not entirely accurate, but as far
  //       as LLVM is concerned, this operation doesn't use memory.
  return CreatePatchpoint(GetUncompressedGCPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

llvm::Value* CodeGeneratorARM64LLVM::NewStringBssEntryPatch(HLoadString* load_string) {
  const DexFile& dex_file = load_string->GetDexFile();
  const dex::StringIndex string_index = load_string->GetStringIndex();
  uint64_t id = EncodePatchpointID(PatchpointKind::kLoadStringBssEntry,
                                   AddStackMapInfo(nullptr, &dex_file, string_index.index_));
  // NOTE: We add a memory(none) attribute to this call, which is not entirely accurate, but as far
  //       as LLVM is concerned, this operation doesn't use memory.
  return CreatePatchpoint(GetUncompressedGCPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

llvm::Value* CodeGeneratorARM64LLVM::NewMethodTypeBssEntryPatch(HLoadMethodType* load_method_type) {
  uint64_t id = EncodePatchpointID(
      PatchpointKind::kLoadMethodTypeBssEntry,
      AddStackMapInfo(
          nullptr, &load_method_type->GetDexFile(), load_method_type->GetProtoIndex().index_));

  return CreatePatchpoint(GetUncompressedGCPointerType(),
                          id,
                          2 * vixl::aarch64::kInstructionSize,
                          /* arguments= */ {},
                          /* recorded_values= */ {},
                          llvm::ModRefInfo::Ref);
}

void InstructionCodeGeneratorARM64LLVM::VisitInvokePolymorphic(HInvokePolymorphic* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }
  // NOTE: GenerateInvokePolymorphicCall is the same as an InvokeRuntime with
  // kQuickInvokePolymorphic entrypoint.
  llvm::Value* result = codegen_->GenerateInvokePolymorphicCall(invoke);
  if (!result->getType()->isVoidTy()) {
    AddValue(invoke, result);
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitInvokeCustom(HInvokeCustom* invoke) {
  uint32_t number_of_arguments = invoke->GetNumberOfArguments();
  llvm::SmallVector<llvm::Value*> arguments;
  arguments.reserve(number_of_arguments + 2);

  // Comment from `LocationsBuilderARM64::VisitInvokeUnresolved`:
  // The trampoline uses the same calling convention as dex calling conventions,
  // except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
  // the method_idx.

  arguments.push_back(codegen_->GetUndefCurrentMethodPointer());
  // Call site index.
  arguments.push_back(GetConstantInt(GetUint32Type(), invoke->GetCallSiteIndex()));
  for (uint32_t i = 0; i < number_of_arguments; ++i) {
    arguments.push_back(GetValue(invoke->InputAt(i)));
  }

  llvm::Type* return_type = GetLLVMType(invoke->GetType());
  // NOTE: HInvokeCustom uses the same calling convention as HInvokeUnresolved.
  codegen_->SetInvokeRuntimeParametersAndReturnType(
      arguments, return_type, llvm::CallingConv::ARTInvokeUnresolved);
  codegen_->InvokeRuntime(kQuickInvokeCustom, invoke);
  if (!return_type->isVoidTy()) {
    AddValue(invoke, codegen_->GetInvokeRuntimeResult());
  }
}

void CodeGeneratorARM64LLVM::NewPcRelativePatch(const DexFile* dex_file,
                                                uint32_t offset_or_index,
                                                uint64_t insn_offset,
                                                std::optional<uint64_t> adrp_offset,
                                                ArenaDeque<PcRelativePatchInfo>* patches) {
  // Add a patch entry.
  PcRelativePatchInfo& info = patches->emplace_back(dex_file, offset_or_index);
  info.label = insn_offset;
  info.pc_insn_label = adrp_offset.value_or(insn_offset);
}

void CodeGeneratorARM64LLVM::EmitJitRootPatches([[maybe_unused]] uint8_t* code,
                                                [[maybe_unused]] const uint8_t* roots_data) {
  LOG(FATAL) << "Unreachable";
}

llvm::FunctionCallee CodeGeneratorARM64LLVM::GetEntrypointThunkPlaceholderFunction(
    ThreadOffset64 entrypoint_offset, llvm::FunctionType* function_type, llvm::CallingConv::ID cc) {
  llvm::FunctionCallee result = GetPlaceholderFunction(
      fmt::format("{}_{}", kEntrypointThunkPlaceholderFunctionName, entrypoint_offset.SizeValue()),
      function_type);
  DCHECK(llvm::isa<llvm::Function>(result.getCallee()));
  llvm::Function* function = llvm::cast<llvm::Function>(result.getCallee());
  if (function->getCallingConv() == llvm::CallingConv::C) {
    function->setCallingConv(cc);
  }
  DCHECK_EQ(function->getCallingConv(), cc);

  return result;
}

llvm::FunctionCallee CodeGeneratorARM64LLVM::GetPlaceholderFunction(std::string_view function_name,
                                                                    llvm::FunctionType* type) {
  return module_->getOrInsertFunction(function_name, type);
}

llvm::Value* CodeGeneratorARM64LLVM::LoadBootImageAddress(uint32_t boot_image_reference) {
  if (GetCompilerOptions().IsBootImage()) {
    return NewBootImageIntrinsicPatch(boot_image_reference);
  } else if (GetCompilerOptions().GetCompilePic()) {
    return NewClassBootImageRelRoPatch(boot_image_reference);
  } else {
    DCHECK(GetCompilerOptions().IsJitCompiler());
    LOG(FATAL) << "Unreachable: LLVM code generator cannot be used for JIT compilation";
    UNREACHABLE();
  }
}

llvm::Value* CodeGeneratorARM64LLVM::LoadIntrinsicDeclaringClass(HInvoke* invoke) {
  DCHECK_NE(invoke->GetIntrinsic(), Intrinsics::kNone);
  if (GetCompilerOptions().IsBootImage()) {
    MethodReference target_method = invoke->GetResolvedMethodReference();
    dex::TypeIndex type_idx = target_method.dex_file->GetMethodId(target_method.index).class_idx_;
    return NewBootImageTypePatch(*target_method.dex_file, type_idx);
  } else {
    uint32_t boot_image_offset = GetBootImageOffsetOfIntrinsicDeclaringClass(invoke);
    return LoadBootImageAddress(boot_image_offset);
  }
}

llvm::Value* CodeGeneratorARM64LLVM::LoadClassRootForIntrinsic(ClassRoot class_root) {
  if (GetCompilerOptions().IsBootImage()) {
    ScopedObjectAccess soa(Thread::Current());
    ObjPtr<mirror::Class> klass = GetClassRoot(class_root);
    return NewBootImageTypePatch(klass->GetDexFile(), klass->GetDexTypeIndex());
  } else {
    uint32_t boot_image_offset = GetBootImageOffset(class_root);
    return LoadBootImageAddress(boot_image_offset);
  }
}
template <linker::LinkerPatch (*Factory)(size_t, const DexFile*, uint32_t, uint32_t)>
inline void CodeGeneratorARM64LLVM::EmitPcRelativeLinkerPatches(
    const ArenaDeque<PcRelativePatchInfo>& infos,
    ArenaVector<linker::LinkerPatch>* linker_patches) {
  for (const PcRelativePatchInfo& info : infos) {
    DCHECK_LE(info.label, std::numeric_limits<size_t>::max());
    DCHECK_LE(info.pc_insn_label, std::numeric_limits<uint32_t>::max());
    linker_patches->push_back(Factory(static_cast<size_t>(info.label),
                                      info.target_dex_file,
                                      static_cast<uint32_t>(info.pc_insn_label),
                                      info.offset_or_index));
  }
}

template <linker::LinkerPatch (*Factory)(size_t, uint32_t, uint32_t)>
linker::LinkerPatch NoDexFileAdapter(size_t literal_offset,
                                     const DexFile* target_dex_file,
                                     uint32_t pc_insn_offset,
                                     uint32_t boot_image_offset) {
  DCHECK(target_dex_file == nullptr);  // Unused for these patches, should be null.
  return Factory(literal_offset, pc_insn_offset, boot_image_offset);
}

void CodeGeneratorARM64LLVM::EmitLinkerPatches(ArenaVector<linker::LinkerPatch>* linker_patches) {
  DCHECK(linker_patches->empty());
  size_t size = boot_image_method_patches_.size() + app_image_method_patches_.size() +
                method_bss_entry_patches_.size() + boot_image_type_patches_.size() +
                type_bss_entry_patches_.size() + public_type_bss_entry_patches_.size() +
                package_type_bss_entry_patches_.size() + boot_image_string_patches_.size() +
                string_bss_entry_patches_.size() + method_type_bss_entry_patches_.size() +
                boot_image_jni_entrypoint_patches_.size() + boot_image_other_patches_.size() +
                call_entrypoint_patches_.size() + baker_read_barrier_patches_.size() +
                app_image_type_patches_.size();
  linker_patches->reserve(size);
  if (GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension()) {
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeMethodPatch>(
        boot_image_method_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeTypePatch>(boot_image_type_patches_,
                                                                        linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeStringPatch>(
        boot_image_string_patches_, linker_patches);
  } else {
    DCHECK(boot_image_method_patches_.empty());
    DCHECK(boot_image_type_patches_.empty());
    DCHECK(boot_image_string_patches_.empty());
  }
  DCHECK_IMPLIES(!GetCompilerOptions().IsAppImage(), app_image_method_patches_.empty());
  DCHECK_IMPLIES(!GetCompilerOptions().IsAppImage(), app_image_type_patches_.empty());
  if (GetCompilerOptions().IsBootImage()) {
    EmitPcRelativeLinkerPatches<NoDexFileAdapter<linker::LinkerPatch::IntrinsicReferencePatch>>(
        boot_image_other_patches_, linker_patches);
  } else {
    EmitPcRelativeLinkerPatches<NoDexFileAdapter<linker::LinkerPatch::BootImageRelRoPatch>>(
        boot_image_other_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::MethodAppImageRelRoPatch>(
        app_image_method_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::TypeAppImageRelRoPatch>(
        app_image_type_patches_, linker_patches);
  }
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::MethodBssEntryPatch>(method_bss_entry_patches_,
                                                                        linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::TypeBssEntryPatch>(type_bss_entry_patches_,
                                                                      linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::PublicTypeBssEntryPatch>(
      public_type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::PackageTypeBssEntryPatch>(
      package_type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::StringBssEntryPatch>(string_bss_entry_patches_,
                                                                        linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::MethodTypeBssEntryPatch>(
      method_type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeJniEntrypointPatch>(
      boot_image_jni_entrypoint_patches_, linker_patches);
  for (const PatchInfo<uint64_t>& info : call_entrypoint_patches_) {
    DCHECK(info.target_dex_file == nullptr);
    linker_patches->push_back(
        linker::LinkerPatch::CallEntrypointPatch(info.label, info.offset_or_index));
  }
  for (const BakerReadBarrierPatchInfo& info : baker_read_barrier_patches_) {
    linker_patches->push_back(linker::LinkerPatch::BakerReadBarrierBranchPatch(
        info.label.GetLocation(), info.custom_data));
  }
  DCHECK_EQ(size, linker_patches->size());
}

bool CodeGeneratorARM64LLVM::NeedsThunkCode(const linker::LinkerPatch& patch) const {
  return patch.GetType() == linker::LinkerPatch::Type::kCallEntrypoint ||
         patch.GetType() == linker::LinkerPatch::Type::kBakerReadBarrierBranch ||
         patch.GetType() == linker::LinkerPatch::Type::kCallRelative;
}

void CodeGeneratorARM64LLVM::EmitThunkCode(const linker::LinkerPatch& patch,
                                           /*out*/ ArenaVector<uint8_t>* code,
                                           /*out*/ std::string* debug_name) {
  arm64::Arm64Assembler assembler(GetGraph()->GetAllocator());
  switch (patch.GetType()) {
    case linker::LinkerPatch::Type::kCallRelative: {
      // The thunk just uses the entry point in the ArtMethod. This works even for calls
      // to the generic JNI and interpreter trampolines.
      Offset offset(
          ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64PointerSize).Int32Value());
      assembler.JumpTo(ManagedRegister(arm64::X0), offset, ManagedRegister(arm64::IP0));
      if (debug_name != nullptr && GetCompilerOptions().GenerateAnyDebugInfo()) {
        *debug_name = "MethodCallThunk";
      }
      break;
    }
    case linker::LinkerPatch::Type::kCallEntrypoint: {
      Offset offset(patch.EntrypointOffset());
      assembler.JumpTo(ManagedRegister(arm64::TR), offset, ManagedRegister(arm64::IP0));
      if (debug_name != nullptr && GetCompilerOptions().GenerateAnyDebugInfo()) {
        *debug_name = "EntrypointCallThunk_" + std::to_string(offset.Uint32Value());
      }
      break;
    }
    case linker::LinkerPatch::Type::kBakerReadBarrierBranch: {
      LOG(FATAL) << "Read barrier encountered in LLVM code generator.";
      UNREACHABLE();
    }
    default:
      LOG(FATAL) << "Unexpected patch type " << patch.GetType();
      UNREACHABLE();
  }

  // Ensure we emit the literal pool if any.
  assembler.FinalizeCode();
  code->resize(assembler.CodeSize());
  MemoryRegion code_region(code->data(), code->size());
  assembler.CopyInstructions(code_region);
}

void InstructionCodeGeneratorARM64LLVM::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // NOTE(LLVM): PrepareForRegisterAllocation is run before LLVM code generation, so this check is
  // correct.
  //
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  llvm::Value* call = codegen_->GenerateStaticOrDirectCall(invoke);
  if (!call->getType()->isVoidTy()) {
    AddValue(invoke, call);
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  llvm::Value* call = codegen_->GenerateVirtualCall(invoke);
  if (!call->getType()->isVoidTy()) {
    AddValue(invoke, call);
  }
  // FIXME: This is set at code_generator.cc:940 in `CodeGenerator::AllocateLocations()`.
  //
  // DCHECK(!codegen_->IsLeafMethod());
}

HLoadClass::LoadKind CodeGeneratorARM64LLVM::GetSupportedLoadClassKind(
    HLoadClass::LoadKind desired_class_load_kind) {
  switch (desired_class_load_kind) {
    case HLoadClass::LoadKind::kInvalid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
    case HLoadClass::LoadKind::kReferrersClass:
      break;
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative:
    case HLoadClass::LoadKind::kBootImageRelRo:
    case HLoadClass::LoadKind::kAppImageRelRo:
    case HLoadClass::LoadKind::kBssEntry:
    case HLoadClass::LoadKind::kBssEntryPublic:
    case HLoadClass::LoadKind::kBssEntryPackage:
      DCHECK(!GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadClass::LoadKind::kJitBootImageAddress:
    case HLoadClass::LoadKind::kJitTableAddress:
      DCHECK(GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadClass::LoadKind::kRuntimeCall:
      break;
  }
  return desired_class_load_kind;
}

// NO_THREAD_SAFETY_ANALYSIS as we manipulate handles whose internal object we know does not
// move.
void InstructionCodeGeneratorARM64LLVM::VisitLoadClass(HLoadClass* cls) NO_THREAD_SAFETY_ANALYSIS {
  HLoadClass::LoadKind load_kind = cls->GetLoadKind();
  // FIXME: Couldn't this be handled just in the switch?
  if (load_kind == HLoadClass::LoadKind::kRuntimeCall) {
    DCHECK_EQ(cls->InputCount(), 1u);
    dex::TypeIndex type_index = cls->GetTypeIndex();
    llvm::Value* type_index_value = GetConstantInt(GetUint32Type(), type_index.index_);
    codegen_->SetInvokeRuntimeParametersAndReturnType(
        {codegen_->GetUndefCurrentMethodPointer(), type_index_value},
        GetUncompressedGCPointerType(),
        llvm::CallingConv::ARTPreserveAll);
    codegen_->InvokeRuntime(kQuickResolveTypeAndVerifyAccess, cls);
    CheckEntrypointTypes<kQuickResolveTypeAndVerifyAccess, void*, uint32_t>();
    AddValue(cls, codegen_->GetInvokeRuntimeResult());
    return;
  }
  DCHECK_EQ(cls->NeedsAccessCheck(),
            load_kind == HLoadClass::LoadKind::kBssEntryPublic ||
                load_kind == HLoadClass::LoadKind::kBssEntryPackage);

  llvm::Value* out = nullptr;

  const ReadBarrierOption read_barrier_option =
      cls->IsInImage() ? kWithoutReadBarrier : codegen_->GetCompilerReadBarrierOption();
  bool generate_null_check = false;
  switch (load_kind) {
    case HLoadClass::LoadKind::kReferrersClass: {
      DCHECK(!cls->CanCallRuntime());
      DCHECK(!cls->MustGenerateClinitCheck());
      // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
      // Register current_method = InputRegisterAt(cls, 0);
      llvm::Value* current_method = GetValue(cls->InputAt(0));
      llvm::Value* loaded_current_method =
          CreateLoadWithOffset(GetUncompressedGCPointerType(),
                               current_method,
                               ArtMethod::DeclaringClassOffset().SizeValue());
      out = codegen_->GenerateGcRootFieldLoad(cls, loaded_current_method, read_barrier_option);
      break;
    }
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(codegen_->GetCompilerOptions().IsBootImage() ||
             codegen_->GetCompilerOptions().IsBootImageExtension());
      DCHECK_EQ(read_barrier_option, kWithoutReadBarrier);
      const DexFile& dex_file = cls->GetDexFile();
      dex::TypeIndex type_index = cls->GetTypeIndex();
      out = codegen_->NewBootImageTypePatch(dex_file, type_index);
      break;
    }
    case HLoadClass::LoadKind::kBootImageRelRo: {
      DCHECK(!codegen_->GetCompilerOptions().IsBootImage());
      uint32_t boot_image_offset = CodeGenerator::GetBootImageOffset(cls);
      out = codegen_->NewClassBootImageRelRoPatch(boot_image_offset);
      break;
    }
    case HLoadClass::LoadKind::kAppImageRelRo: {
      DCHECK(codegen_->GetCompilerOptions().IsAppImage());
      DCHECK_EQ(read_barrier_option, kWithoutReadBarrier);
      const DexFile& dex_file = cls->GetDexFile();
      dex::TypeIndex type_index = cls->GetTypeIndex();
      out = codegen_->NewAppImageTypePatch(dex_file, type_index);
      break;
    }
    case HLoadClass::LoadKind::kBssEntry:
    case HLoadClass::LoadKind::kBssEntryPublic:
    case HLoadClass::LoadKind::kBssEntryPackage: {
      // /* GcRoot<mirror::Class> */ out = *(base_address + offset)  /* PC-relative */
      // All aligned loads are implicitly atomic consume operations on ARM64.
      llvm::Value* root = codegen_->NewBssEntryTypePatch(cls);
      out = codegen_->GenerateGcRootFieldLoad(cls, root, read_barrier_option);
      generate_null_check = true;
      break;
    }
    case HLoadClass::LoadKind::kJitBootImageAddress: {
      UNIMPLEMENTED(FATAL) << "JIT not implemented for VisitLoadClass";
      break;
    }
    case HLoadClass::LoadKind::kJitTableAddress: {
      UNIMPLEMENTED(FATAL) << "JIT not implemented for VisitLoadClass";
      break;
    }
    case HLoadClass::LoadKind::kRuntimeCall:
    case HLoadClass::LoadKind::kInvalid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }

  bool do_clinit = cls->MustGenerateClinitCheck();
  if (generate_null_check || do_clinit) {
    DCHECK(cls->CanCallRuntime());
    llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
    llvm::BasicBlock* slow_path_exit_block = codegen_->CreateBasicBlock();

    llvm::BasicBlock* current_block = __ GetInsertBlock();
    __ SetInsertPoint(slow_path_exit_block);
    llvm::PHINode* result_phi = __ CreatePHI(out->getType(), 2);
    __ SetInsertPoint(current_block);

    SlowPathCodeARM64LLVM* slow_path =
        new (codegen_->GetScopedAllocator()) LoadClassSlowPathARM64LLVM(
            cls, cls, slow_path_entry_block, slow_path_exit_block, result_phi, out);
    codegen_->AddSlowPath(slow_path);
    if (generate_null_check) {
      llvm::Value* is_null = __ CreateICmpEQ(out, GetConstantZero(out->getType()));
      llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_null, slow_path->GetEntryBlock());
      ExpectFalseBranch(br);
    }
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ CreateBr(slow_path->GetExitBlock());
    }
    result_phi->addIncoming(out, __ GetInsertBlock());
    __ SetInsertPoint(slow_path->GetExitBlock());
    // The result of the whole expression is the phi node.
    out = result_phi;
  }

  AddValue(cls, out);
}

void InstructionCodeGeneratorARM64LLVM::VisitLoadMethodHandle(HLoadMethodHandle* load) {
  std::array<llvm::Value*, 2> arguments = {
      codegen_->GetUndefCurrentMethodPointer(),
      GetConstantInt(GetUint32Type(), load->GetMethodHandleIndex()),
  };
  codegen_->SetInvokeRuntimeParametersAndReturnType(
      arguments, GetUncompressedGCPointerType(), llvm::CallingConv::ARTPreserveAll);
  codegen_->InvokeRuntime(kQuickResolveMethodHandle, load);
  CheckEntrypointTypes<kQuickResolveMethodHandle, void*, uint32_t>();
  AddValue(load, codegen_->GetInvokeRuntimeResult());
}

void InstructionCodeGeneratorARM64LLVM::VisitLoadMethodType(HLoadMethodType* load) {
  switch (load->GetLoadKind()) {
    case HLoadMethodType::LoadKind::kBssEntry: {
      llvm::Value* method_type = codegen_->NewMethodTypeBssEntryPatch(load);
      llvm::Value* gcroot = codegen_->GenerateGcRootFieldLoad(
          load, method_type, codegen_->GetCompilerReadBarrierOption());

      llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
      llvm::BasicBlock* slow_path_exit_block = codegen_->CreateBasicBlock();

      llvm::BasicBlock* current_block = __ GetInsertBlock();
      __ SetInsertPoint(slow_path_exit_block);
      llvm::PHINode* result_phi = __ CreatePHI(gcroot->getType(), 2);
      __ SetInsertPoint(current_block);

      SlowPathCodeARM64LLVM* slow_path =
          new (codegen_->GetScopedAllocator()) LoadMethodTypeSlowPathARM64LLVM(
              load, slow_path_entry_block, slow_path_exit_block, result_phi);
      codegen_->AddSlowPath(slow_path);

      llvm::Value* is_null = __ CreateICmpEQ(gcroot, GetConstantZero(gcroot->getType()));
      llvm::Instruction* br = __ CreateCondBr(is_null, slow_path_entry_block, slow_path_exit_block);
      ExpectFalseBranch(br);

      result_phi->addIncoming(gcroot, __ GetInsertBlock());
      __ SetInsertPoint(slow_path_exit_block);

      AddValue(load, result_phi);
      return;
    }
    case HLoadMethodType::LoadKind::kJitTableAddress: {
      UNIMPLEMENTED(FATAL) << "JIT not implemented for VisitLoadMethodType";
      return;
    }
    default:
      DCHECK_EQ(load->GetLoadKind(), HLoadMethodType::LoadKind::kRuntimeCall);
      std::array<llvm::Value*, 2> arguments = {
          codegen_->GetUndefCurrentMethodPointer(),
          GetConstantInt(GetUint32Type(), load->GetProtoIndex().index_),
      };
      codegen_->SetInvokeRuntimeParametersAndReturnType(
          arguments, GetUncompressedGCPointerType(), llvm::CallingConv::ARTPreserveAll);
      codegen_->InvokeRuntime(kQuickResolveMethodType, load);
      CheckEntrypointTypes<kQuickResolveMethodType, void*, uint32_t>();
      AddValue(load, codegen_->GetInvokeRuntimeResult());
      return;
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitLoadException(HLoadException* instruction) {
  // __ Ldr(OutputRegister(instruction), GetExceptionTlsAddress());
  llvm::Value* exception = CreateLoadFromThreadPointer(
      GetUncompressedGCPointerType(), Thread::ExceptionOffset<kArm64PointerSize>().SizeValue());
  AddValue(instruction, exception);
}

void InstructionCodeGeneratorARM64LLVM::VisitClearException(
    [[maybe_unused]] HClearException* clear) {
  // __ Str(wzr, GetExceptionTlsAddress());
  CreateStoreToThreadPointer(GetConstantZero(GetUncompressedGCPointerType()),
                             Thread::ExceptionOffset<kArm64PointerSize>().SizeValue());
}

HLoadString::LoadKind CodeGeneratorARM64LLVM::GetSupportedLoadStringKind(
    HLoadString::LoadKind desired_string_load_kind) {
  switch (desired_string_load_kind) {
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative:
    case HLoadString::LoadKind::kBootImageRelRo:
    case HLoadString::LoadKind::kBssEntry:
      DCHECK(!GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadString::LoadKind::kJitBootImageAddress:
    case HLoadString::LoadKind::kJitTableAddress:
      DCHECK(GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadString::LoadKind::kRuntimeCall:
      break;
  }
  return desired_string_load_kind;
}

// NO_THREAD_SAFETY_ANALYSIS as we manipulate handles whose internal object we know does not
// move.
void InstructionCodeGeneratorARM64LLVM::VisitLoadString(HLoadString* load)
    NO_THREAD_SAFETY_ANALYSIS {
  switch (load->GetLoadKind()) {
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative: {
      const DexFile& dex_file = load->GetDexFile();
      const dex::StringIndex string_index = load->GetStringIndex();
      llvm::Value* result =
          codegen_->NewStringBootImageLinkTimePcRelativePatch(dex_file, string_index);
      AddValue(load, result);
      return;
    }
    case HLoadString::LoadKind::kBootImageRelRo: {
      DCHECK(!codegen_->GetCompilerOptions().IsBootImage());
      uint32_t boot_image_offset = CodeGenerator::GetBootImageOffset(load);
      llvm::Value* result = codegen_->NewStringBootImageRelRoPatch(boot_image_offset);
      AddValue(load, result);
      return;
    }
    case HLoadString::LoadKind::kBssEntry: {
      // Add ADRP with its PC-relative String .bss entry patch.
      // const DexFile& dex_file = load->GetDexFile();
      llvm::Value* string_address = codegen_->NewStringBssEntryPatch(load);

      // /* GcRoot<mirror::String> */ out = *(base_address + offset)  /* PC-relative */
      // All aligned loads are implicitly atomic consume operations on ARM64.
      llvm::Value* out = codegen_->GenerateGcRootFieldLoad(
          load, string_address, codegen_->GetCompilerReadBarrierOption());

      llvm::BasicBlock* slow_path_exit = codegen_->CreateBasicBlock();
      llvm::BasicBlock* slow_path_entry = codegen_->CreateBasicBlock();

      llvm::BasicBlock* current_block = __ GetInsertBlock();
      __ SetInsertPoint(slow_path_exit);
      llvm::PHINode* result_phi = __ CreatePHI(out->getType(), 2);
      __ SetInsertPoint(current_block);

      SlowPathCodeARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
          LoadStringSlowPathARM64LLVM(load, slow_path_entry, slow_path_exit, result_phi);
      codegen_->AddSlowPath(slow_path);
      llvm::Value* is_out_null = __ CreateICmpEQ(out, GetConstantZero(out->getType()));
      llvm::Instruction* br = __ CreateCondBr(is_out_null, slow_path_entry, slow_path_exit);
      ExpectFalseBranch(br);
      result_phi->addIncoming(out, __ GetInsertBlock());
      __ SetInsertPoint(slow_path_exit);
      AddValue(load, result_phi);
      return;
    }
    case HLoadString::LoadKind::kJitBootImageAddress: {
      UNIMPLEMENTED(FATAL) << "JIT not implemented for VisitLoadString";
      return;
    }
    case HLoadString::LoadKind::kJitTableAddress: {
      UNIMPLEMENTED(FATAL) << "JIT not implemented for VisitLoadString";
      return;
    }
    default:
      break;
  }

  codegen_->SetInvokeRuntimeParametersAndReturnType(
      {GetConstantInt(GetUint32Type(), load->GetStringIndex().index_)},
      GetUncompressedGCPointerType(),
      llvm::CallingConv::ARTPreserveAll);
  codegen_->InvokeRuntime(kQuickResolveString, load);
  CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
  llvm::Value* result = codegen_->GetInvokeRuntimeResult();
  AddValue(load, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitLongConstant(HLongConstant* constant) {
  DCHECK_EQ(constant->GetType(), DataType::Type::kInt64);
  llvm::Value* llvm_constant = GetConstantInt(GetInt64Type(), constant->GetValue());
  AddValue(constant, llvm_constant);
}

void InstructionCodeGeneratorARM64LLVM::VisitMonitorOperation(HMonitorOperation* instruction) {
  llvm::Value* parameter = GetValue(instruction->InputAt(0));
  codegen_->SetInvokeRuntimeParametersAndReturnType(
      {codegen_->GetUndefCurrentMethodPointer(), parameter}, GetVoidType());
  codegen_->InvokeRuntime(instruction->IsEnter() ? kQuickLockObject : kQuickUnlockObject,
                          instruction);
  if (instruction->IsEnter()) {
    CheckEntrypointTypes<kQuickLockObject, void, mirror::Object*>();
  } else {
    CheckEntrypointTypes<kQuickUnlockObject, void, mirror::Object*>();
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitMul(HMul* mul) {
  auto [lhs, rhs] = NormalizeBinaryOperands(mul->GetResultType(), mul->GetLeft(), mul->GetRight());
  DCHECK_EQ(lhs->getType(), rhs->getType());

  llvm::Value* result = nullptr;
  switch (mul->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateMul(lhs, rhs);
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateFMul(lhs, rhs);
      break;

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
  AddValue(mul, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitNeg(HNeg* neg) {
  llvm::Value* value = GetValue(neg->InputAt(0), neg->GetResultType());
  llvm::Value* result = nullptr;
  switch (neg->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      // __ Neg(OutputRegister(neg), InputOperandAt(neg, 0));
      result = __ CreateNeg(value);
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      // __ Fneg(OutputFPRegister(neg), InputFPRegisterAt(neg, 0));
      result = __ CreateFNeg(value);
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
  DCHECK(result != nullptr);
  AddValue(neg, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitNewArray(HNewArray* instruction) {
  // Note: if heap poisoning is enabled, the entry point takes care of poisoning the reference.
  QuickEntrypointEnum entrypoint = CodeGenerator::GetArrayAllocationEntrypoint(instruction);
  std::array<llvm::Value*, 3> parameters = {codegen_->GetUndefCurrentMethodPointer(),
                                            GetValue(instruction->InputAt(0)),
                                            GetValue(instruction->InputAt(1))};
  codegen_->SetInvokeRuntimeParametersAndReturnType(parameters, GetUncompressedGCPointerType());
  codegen_->InvokeRuntime(entrypoint, instruction);
  CheckEntrypointTypes<kQuickAllocArrayResolved, void*, mirror::Class*, int32_t>();
  llvm::CallBase* new_array = codegen_->GetInvokeRuntimeResult();
  new_array->addRetAttr(llvm::Attribute::NonNull);
  AddValue(instruction, new_array);
}

void InstructionCodeGeneratorARM64LLVM::VisitNewInstance(HNewInstance* instruction) {
  llvm::Value* class_info_ptr = GetValue(instruction->InputAt(0));
  codegen_->SetInvokeRuntimeParametersAndReturnType(
      {codegen_->GetUndefCurrentMethodPointer(), class_info_ptr}, GetUncompressedGCPointerType());
  codegen_->InvokeRuntime(instruction->GetEntrypoint(), instruction);
  CheckEntrypointTypes<kQuickAllocObjectWithChecks, void*, mirror::Class*>();
  llvm::CallBase* new_instance = codegen_->GetInvokeRuntimeResult();
  new_instance->addRetAttr(llvm::Attribute::NonNull);
  AddValue(instruction, new_instance);
}

void InstructionCodeGeneratorARM64LLVM::VisitNot(HNot* instruction) {
  DataType::Type type = instruction->GetResultType();

  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      llvm::Value* operand = GetValue(instruction->InputAt(0), type);
      llvm::Value* result = __ CreateNot(operand);
      AddValue(instruction, result);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for not operation " << type;
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitBooleanNot(HBooleanNot* instruction) {
  llvm::Value* value = GetValue(instruction->GetInput(), DataType::Type::kBool);
  // Boolean values may actually not have i1 type, so we use an xor instead.
  llvm::Value* result = __ CreateXor(value, 1);
  AddValue(instruction, result);
}

void CodeGeneratorARM64LLVM::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (CanMoveNullCheckToUser(instruction)) {
    return;
  }
  llvm::Value* obj = GetValue(instruction->InputAt(0));
  CreateDiscardedLoadWithImplicitNullCheck(instruction, obj);
}

void CodeGeneratorARM64LLVM::GenerateExplicitNullCheck(HNullCheck* instruction) {
  llvm::BasicBlock* slow_path_entry = CreateBasicBlock();
  SlowPathCodeARM64LLVM* slow_path =
      new (GetScopedAllocator()) NullCheckSlowPathARM64LLVM(instruction, slow_path_entry);
  AddSlowPath(slow_path);

  llvm::Value* obj = GetValue(instruction->InputAt(0));
  llvm::Value* is_null = __ CreateICmpEQ(obj, GetConstantZero(obj->getType()));
  llvm::Instruction* br = CreateBranchIfTrue(is_null, slow_path_entry);
  ExpectFalseBranch(br);
}

void InstructionCodeGeneratorARM64LLVM::VisitNullCheck(HNullCheck* instruction) {
  codegen_->GenerateNullCheck(instruction);
  AddValue(instruction, GetValue(instruction->InputAt(0)));
}

void InstructionCodeGeneratorARM64LLVM::VisitOr(HOr* instruction) { HandleBinaryOp(instruction); }

// NOTE(LLVM): This instruction is the result of register allocation, and should never happen when
// using LLVM.
void InstructionCodeGeneratorARM64LLVM::VisitParallelMove(
    [[maybe_unused]] HParallelMove* instruction) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM64LLVM::VisitParameterValue(HParameterValue* instruction) {
  DCHECK_LT(next_parameter_index_, codegen_->GetFunction()->arg_size());
  llvm::Value* parameter = codegen_->GetFunction()->getArg(next_parameter_index_);

  codegen_->MapParameterValueToIndex(instruction, next_parameter_index_);

  ++next_parameter_index_;

  codegen_->AddValue(instruction, parameter);
}

void InstructionCodeGeneratorARM64LLVM::VisitCurrentMethod(HCurrentMethod* instruction) {
  llvm::Value* current_method = codegen_->GetCurrentMethodPointerArgument();
  codegen_->AddValue(instruction, current_method);
}

void InstructionCodeGeneratorARM64LLVM::VisitPhi([[maybe_unused]] HPhi* instruction) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM64LLVM::VisitRem(HRem* rem) {
  DataType::Type type = rem->GetResultType();
  auto [lhs, rhs] = NormalizeBinaryOperands(type, rem->GetLeft(), rem->GetRight());

  llvm::Value* result = nullptr;
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      llvm::BasicBlock* result_block = codegen_->CreateBasicBlock();
      llvm::Value* is_overflow = __ CreateICmpEQ(rhs, GetConstantInt(rhs->getType(), -1));
      llvm::BasicBlock* overflow_check_block = __ GetInsertBlock();
      codegen_->CreateBranchIfTrue(is_overflow, result_block);
      llvm::Value* non_overflow_result = __ CreateSRem(lhs, rhs);
      llvm::BasicBlock* non_overflow_block = __ GetInsertBlock();
      __ CreateBr(result_block);
      __ SetInsertPoint(result_block);
      llvm::PHINode* result_phi = __ CreatePHI(lhs->getType(), 2);
      result_phi->addIncoming(GetConstantZero(lhs->getType()), overflow_check_block);
      result_phi->addIncoming(non_overflow_result, non_overflow_block);
      result = result_phi;
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      QuickEntrypointEnum entrypoint =
          (type == DataType::Type::kFloat32) ? kQuickFmodf : kQuickFmod;
      codegen_->SetInvokeRuntimeParametersAndReturnType(
          {codegen_->GetUndefCurrentMethodPointer(), lhs, rhs}, lhs->getType());
      codegen_->InvokeRuntime(entrypoint, rem);
      result = codegen_->GetInvokeRuntimeResult();
      if (type == DataType::Type::kFloat32) {
        CheckEntrypointTypes<kQuickFmodf, float, float, float>();
      } else {
        CheckEntrypointTypes<kQuickFmod, double, double, double>();
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected rem type " << type;
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(rem, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitMin(HMin* min) { HandleBinaryOp(min); }

void InstructionCodeGeneratorARM64LLVM::VisitMax(HMax* max) { HandleBinaryOp(max); }

void InstructionCodeGeneratorARM64LLVM::VisitAbs(HAbs* abs) {
  llvm::Value* value = GetValue(abs->InputAt(0), abs->GetResultType());
  llvm::Value* result = nullptr;
  switch (abs->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      // __ Cmp(in_reg, Operand(0));
      // __ Cneg(out_reg, in_reg, lt);
      llvm::Value* is_int_min_poison = llvm::ConstantInt::getFalse(GetBooleanType());
      result =
          __ CreateIntrinsic(llvm::Intrinsic::abs, value->getType(), {value, is_int_min_poison});
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      // __ Fabs(out_reg, in_reg);
      result = __ CreateUnaryIntrinsic(llvm::Intrinsic::fabs, value);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for abs operation " << abs->GetResultType();
  }
  DCHECK(result != nullptr);
  DCHECK(result->getType() == GetLLVMType(abs->GetType()));
  AddValue(abs, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitConstructorFence(
    [[maybe_unused]] HConstructorFence* constructor_fence) {
  codegen_->GenerateMemoryBarrier(MemBarrierKind::kStoreStore);
}

void InstructionCodeGeneratorARM64LLVM::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  codegen_->GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void InstructionCodeGeneratorARM64LLVM::VisitReturn(HReturn* ret) {
  llvm::Value* ret_value = GetValue(ret->InputAt(0));
  // All constant integer and boolean values are represented as constant int32 or int64 values in
  // ART, so it can happen that the type of `ret_value` and the function's return type differ. We
  // need handle this case explicitly.
  llvm::Type* function_return_type = GetFunction()->getReturnType();
  if (ret_value->getType() != function_return_type) {
    DCHECK(function_return_type->isIntegerTy());
    DCHECK_LE(function_return_type->getIntegerBitWidth(), 64u);
    const bool is_signed = !DataType::IsUnsignedType(ret->InputAt(0)->GetType());
    ret_value = __ CreateIntCast(ret_value, function_return_type, is_signed);
  }
  __ CreateRet(ret_value);
  codegen_->GenerateFrameExit();

  // ART moves floating point return values into x0 as well in this case. We don't do that with
  // LLVM.
  DCHECK(!GetGraph()->IsCompilingOsr());
}

void InstructionCodeGeneratorARM64LLVM::VisitReturnVoid([[maybe_unused]] HReturnVoid* instruction) {
  codegen_->GenerateFrameExit();
}

void InstructionCodeGeneratorARM64LLVM::VisitRor(HRor* ror) { HandleBinaryOp(ror); }

void InstructionCodeGeneratorARM64LLVM::VisitShl(HShl* shl) { HandleShift(shl); }

void InstructionCodeGeneratorARM64LLVM::VisitShr(HShr* shr) { HandleShift(shr); }

void InstructionCodeGeneratorARM64LLVM::VisitSub(HSub* instruction) { HandleBinaryOp(instruction); }

void InstructionCodeGeneratorARM64LLVM::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorARM64LLVM::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction,
                 instruction->GetFieldInfo(),
                 instruction->GetValueCanBeNull(),
                 instruction->GetWriteBarrierKind());
}

void InstructionCodeGeneratorARM64LLVM::VisitStringBuilderAppend(
    HStringBuilderAppend* instruction) {
  uint32_t format = static_cast<uint32_t>(instruction->GetFormat()->GetValue());
  const uint32_t full_format = format;
  size_t number_of_argument = instruction->GetNumberOfArguments();

  // Argument passing logic is from `CodeGenerator::CreateStringBuilderAppendLocations`.
  llvm::SmallVector<llvm::Value*> arguments;
  arguments.reserve(number_of_argument + 2);
  // Current method pointer passed in the first stack slot.
  arguments.push_back(codegen_->GetUndefCurrentMethodPointer());
  // __ Mov(w0, instruction->GetFormat()->GetValue());
  arguments.push_back(GetConstantInt(GetUint32Type(), format));

  size_t param_size = 0;
  for (size_t i = 0; i < number_of_argument; ++i) {
    llvm::Value* argument = GetValue(instruction->InputAt(i));
    StringBuilderAppend::Argument arg_type =
        static_cast<StringBuilderAppend::Argument>(format & StringBuilderAppend::kArgMask);
    switch (arg_type) {
      case StringBuilderAppend::Argument::kStringBuilder:
      case StringBuilderAppend::Argument::kString:
      case StringBuilderAppend::Argument::kCharArray:
        static_assert(sizeof(StackReference<mirror::Object>) == sizeof(uint32_t), "Size check.");
        DCHECK(argument->getType() == GetUncompressedGCPointerType());
        break;
      case StringBuilderAppend::Argument::kBoolean:
      case StringBuilderAppend::Argument::kChar:
        // 'boolean' and 'char' are both unsigned, so they should be zero extended to 32 bits.
        argument = __ CreateZExt(argument, GetUint32Type());
        break;
      case StringBuilderAppend::Argument::kInt:
        // Small signed integers should be sign-extended and be passed as i32.
        if (argument->getType() != GetInt32Type()) {
          argument = __ CreateSExt(argument, GetInt32Type());
        }
        break;
      case StringBuilderAppend::Argument::kFloat:
        break;
      case StringBuilderAppend::Argument::kLong:
      case StringBuilderAppend::Argument::kDouble:
        param_size = RoundUp(param_size, sizeof(uint64_t));
        // Skip the low word, let the common code skip the high word.
        param_size += sizeof(uint32_t);
        break;
      default:
        LOG(FATAL) << "Unexpected arg format: 0x" << std::hex
                   << (format & StringBuilderAppend::kArgMask) << " full format: 0x" << full_format;
        UNREACHABLE();
    }
    arguments.push_back(argument);
    format >>= StringBuilderAppend::kBitsPerArg;
    param_size += sizeof(uint32_t);
  }
  DCHECK_EQ(format, 0u);
  DCHECK_EQ(param_size, kVRegSize * instruction->GetNumberOfOutVRegs());

  codegen_->SetInvokeRuntimeParametersAndReturnType(
      arguments, GetUncompressedGCPointerType(), llvm::CallingConv::ARTStringBuilderAppend);
  codegen_->InvokeRuntime(kQuickStringBuilderAppend, instruction);
  llvm::Value* result = codegen_->GetInvokeRuntimeResult();
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  llvm::Value* field_value = codegen_->GenerateUnresolvedFieldAccess(
      instruction, instruction->GetFieldType(), instruction->GetFieldIndex());
  AddValue(instruction, field_value);
}

void InstructionCodeGeneratorARM64LLVM::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  codegen_->GenerateUnresolvedFieldAccess(
      instruction, instruction->GetFieldType(), instruction->GetFieldIndex());
}

void InstructionCodeGeneratorARM64LLVM::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  llvm::Value* field_value = codegen_->GenerateUnresolvedFieldAccess(
      instruction, instruction->GetFieldType(), instruction->GetFieldIndex());
  AddValue(instruction, field_value);
}

void InstructionCodeGeneratorARM64LLVM::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  codegen_->GenerateUnresolvedFieldAccess(
      instruction, instruction->GetFieldType(), instruction->GetFieldIndex());
}

void InstructionCodeGeneratorARM64LLVM::VisitSuspendCheck(HSuspendCheck* instruction) {
  // If the suspend check needs vreg information, we need to generate the check here explicitly,
  // because by the time we would generate it in the optimization pipeline, the values recorded in
  // the environment have been invalidated.
  if (NeedsVregInfo(instruction)) {
    uint64_t id = EncodePatchpointID(PatchpointKind::kImplicitSuspendCheck,
                                     codegen_->AddStackMapInfo(instruction));
    llvm::Function* suspend_check_placeholder = codegen_->GetSuspendCheckPlaceholderFunction();
    llvm::CallBase* suspend_check =
        codegen_->CreateCallOrInvoke(instruction, suspend_check_placeholder, {});
    CHECK(!llvm::isa<llvm::InvokeInst>(suspend_check)) << "Suspend check shouldn't be an invoke!";
    codegen_->SetStatepointID(instruction, suspend_check, id);
  } else if (instruction->IsSuspendCheckEntry()) {
    uint64_t id = EncodePatchpointID(PatchpointKind::kImplicitSuspendCheck,
                                     codegen_->AddStackMapInfo(instruction));
    codegen_->SetEntrySuspendCheckID(id);
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitThrow(HThrow* instruction) {
  llvm::Value* thrown_object = GetValue(instruction->InputAt(0));
  codegen_->SetInvokeRuntimeParametersAndReturnType(
      {codegen_->GetUndefCurrentMethodPointer(), thrown_object}, GetVoidType());
  codegen_->InvokeRuntime(kQuickDeliverException, instruction);
  CheckEntrypointTypes<kQuickDeliverException, void, mirror::Object*>();
  // The throw runtime call should never return.
  __ CreateUnreachable();
}

void InstructionCodeGeneratorARM64LLVM::VisitTypeConversion(HTypeConversion* conversion) {
  DataType::Type result_type = conversion->GetResultType();
  DataType::Type input_type = conversion->GetInputType();

  DCHECK(!DataType::IsTypeConversionImplicit(input_type, result_type))
      << input_type << " -> " << result_type;

  llvm::Value* input = GetValue(conversion->InputAt(0));
  llvm::Value* result = nullptr;
  llvm::Type* result_llvm_type = GetLLVMType(result_type);

  if (DataType::IsIntegralType(result_type) && DataType::IsIntegralType(input_type)) {
    result = __ CreateIntCast(input, result_llvm_type, !DataType::IsUnsignedType(input_type));
  } else if (DataType::IsFloatingPointType(result_type) && DataType::IsIntegralType(input_type)) {
    // __ Scvtf(OutputFPRegister(conversion), InputRegisterAt(conversion, 0));
    if (DataType::IsUnsignedType(input_type)) {
      result = __ CreateUIToFP(input, result_llvm_type);
    } else {
      result = __ CreateSIToFP(input, result_llvm_type);
    }
  } else if (DataType::IsIntegralType(result_type) && DataType::IsFloatingPointType(input_type)) {
    CHECK(result_type == DataType::Type::kInt32 || result_type == DataType::Type::kInt64);
    // __ Fcvtzs(OutputRegister(conversion), InputFPRegisterAt(conversion, 0));
    result = __ CreateFPToSI(input, result_llvm_type);
  } else if (DataType::IsFloatingPointType(result_type) &&
             DataType::IsFloatingPointType(input_type)) {
    // __ Fcvt(OutputFPRegister(conversion), InputFPRegisterAt(conversion, 0));
    result = __ CreateFPCast(input, result_llvm_type);
  } else {
    LOG(FATAL) << "Unexpected or unimplemented type conversion from " << input_type << " to "
               << result_type;
  }

  DCHECK(result != nullptr);
  AddValue(conversion, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitUShr(HUShr* ushr) { HandleShift(ushr); }

void InstructionCodeGeneratorARM64LLVM::VisitXor(HXor* instruction) { HandleBinaryOp(instruction); }

void InstructionCodeGeneratorARM64LLVM::VisitBoundType([[maybe_unused]] HBoundType* instruction) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM64LLVM::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  int32_t start_value = switch_instr->GetStartValue();
  uint32_t num_entries = switch_instr->GetNumEntries();
  llvm::Value* value = GetValue(switch_instr->InputAt(0), DataType::Type::kInt32);
  llvm::BasicBlock* default_block = codegen_->GetIRBasicBlock(switch_instr->GetDefaultBlock());

  llvm::SwitchInst* llvm_switch = __ CreateSwitch(value, default_block, num_entries);
  ArrayRef<HBasicBlock* const> successors =
      ArrayRef<HBasicBlock* const>(switch_instr->GetBlock()->GetSuccessors());
  for (uint32_t i = 0; i < num_entries; ++i) {
    llvm::ConstantInt* case_value = GetConstantInt(value->getType(), start_value + i);
    llvm::BasicBlock* case_block = codegen_->GetIRBasicBlock(successors[i]);
    llvm_switch->addCase(case_value, case_block);
  }

  codegen_->SetLastIRBasicBlock(switch_instr->GetBlock(), __ GetInsertBlock());
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::GenerateReferenceLoad(
    [[maybe_unused]] HInstruction* instruction,
    llvm::Value* obj,
    uint32_t offset,
    ReadBarrierOption read_barrier_option) {
  if (read_barrier_option == kWithReadBarrier) {
    LOG(FATAL) << "Read barrier encountered in LLVM code generator.";
    UNREACHABLE();
  } else {
    // Plain load with no read barrier.
    // /* HeapReference<Object> */ out = *(obj + offset)
    llvm::Value* result = CreateLoadWithOffset(GetUncompressedGCPointerType(), obj, offset);
    result = codegen_->MaybeUnpoisonHeapReference(result);
    return result;
  }
}

llvm::Value* CodeGeneratorARM64LLVM::GenerateGcRootFieldLoad(
    [[maybe_unused]] HInstruction* instruction,
    llvm::Value* loaded_root,
    ReadBarrierOption read_barrier_option) {
  CHECK_NE(read_barrier_option, kWithReadBarrier)
      << "Read barrier encountered in LLVM code generator.";
  // Note that GC roots are not affected by heap poisoning, thus we
  // do not have to unpoison `root_reg` here.
  return loaded_root;
}

llvm::Value* CodeGeneratorARM64LLVM::GenerateFieldLoadWithBakerReadBarrier(
    [[maybe_unused]] HInstruction* instruction,
    [[maybe_unused]] llvm::Value* obj,
    [[maybe_unused]] llvm::Value* field_address,
    [[maybe_unused]] bool needs_null_check,
    [[maybe_unused]] bool use_load_acquire) {
  LOG(FATAL) << "Baker read barrier encountered in LLVM code generator.";
  UNREACHABLE();
}

llvm::Value* CodeGeneratorARM64LLVM::GenerateFieldLoadWithBakerReadBarrier(
    HInstruction* instruction,
    llvm::Value* obj,
    uint32_t offset,
    bool needs_null_check,
    bool use_load_acquire) {
  DCHECK_ALIGNED(offset, sizeof(mirror::HeapReference<mirror::Object>));
  llvm::Value* base = CreateGEP(obj, offset);
  return GenerateFieldLoadWithBakerReadBarrier(
      instruction, obj, base, needs_null_check, use_load_acquire);
}

llvm::Value* CodeGeneratorARM64LLVM::MaybeGenerateReadBarrierSlow(
    [[maybe_unused]] HInstruction* instruction,
    llvm::Value* ref,
    [[maybe_unused]] llvm::Value* obj,
    [[maybe_unused]] uint32_t offset,
    [[maybe_unused]] llvm::Value* index) {
  CHECK(!EmitReadBarrier()) << "Read barrier encountered in LLVM code generator.";
  if (kPoisonHeapReferences) {
    ref = UnpoisonHeapReference(ref);
  }
  return ref;
}

void InstructionCodeGeneratorARM64LLVM::VisitClassTableGet(HClassTableGet* instruction) {
  llvm::Value* obj = GetValue(instruction->InputAt(0));
  if (instruction->GetTableKind() == HClassTableGet::TableKind::kVTable) {
    uint32_t method_offset =
        mirror::Class::EmbeddedVTableEntryOffset(instruction->GetIndex(), kArm64PointerSize)
            .SizeValue();
    llvm::Value* result = CreateLoadWithOffset(GetMethodPointerType(), obj, method_offset);
    AddValue(instruction, result);
  } else {
    uint32_t method_offset =
        static_cast<uint32_t>(ImTable::OffsetOfElement(instruction->GetIndex(), kArm64PointerSize));
    llvm::Value* imt = CreateLoadWithOffset(
        GetPointerType(), obj, mirror::Class::ImtPtrOffset(kArm64PointerSize).Uint32Value());
    llvm::Value* result = CreateLoadWithOffset(GetMethodPointerType(), imt, method_offset);
    AddValue(instruction, result);
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitVecReplicateScalar(HVecReplicateScalar* instruction) {
  size_t vector_length = instruction->GetVectorLength();
  llvm::Value* scalar_value = GetValue(instruction->InputAt(0), instruction->GetPackedType());
  DCHECK(!scalar_value->getType()->isVectorTy());

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      result = __ CreateVectorSplat(vector_length, scalar_value);
      break;
    }
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecExtractScalar(HVecExtractScalar* instruction) {
  llvm::Value* src = GetValue(instruction->InputAt(0));
  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      // NOTE: Only element 0 extraction is implemented in HVecExtractScalar.
      result = __ CreateExtractElement(src, uint64_t{0});
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecReduce(HVecReduce* instruction) {
  llvm::Value* src = GetValue(instruction->InputAt(0));
  DCHECK(instruction->GetPackedType() == DataType::Type::kInt32 ||
         instruction->GetPackedType() == DataType::Type::kInt64)
      << "Unsupported SIMD type: " << instruction->GetPackedType();

  llvm::Value* result = nullptr;
  switch (instruction->GetReductionKind()) {
    case HVecReduce::kSum:
      // __ Addv(dst.S(), src.V4S()); // Int32
      // __ Addp(dst.D(), src.V2D()); // Int64
      result = __ CreateAddReduce(src);
      break;
    case HVecReduce::kMin:
      // __ Sminv(dst.S(), src.V4S());
      result = __ CreateIntMinReduce(src, /* IsSigned= */ true);
      break;
    case HVecReduce::kMax:
      // __ Smaxv(dst.S(), src.V4S());
      result = __ CreateIntMaxReduce(src, /* IsSigned= */ true);
      break;
  }
  DCHECK(result != nullptr);

  // The result value of this instruction has the same vector type as its input, with the result put
  // into the first element of the vector, but LLVM's reduction instructions produce a scalar, so we
  // have to manually put that scalar into a vector to get the proper value of the instruction.
  // TODO: Should we use undef here?
  llvm::Value* undef_vector = llvm::UndefValue::get(src->getType());
  result = __ CreateInsertElement(undef_vector, result, uint64_t{0});
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecCnv(HVecCnv* instruction) {
  llvm::Value* src = GetValue(instruction->InputAt(0));
  DCHECK(instruction->GetInputType() == DataType::Type::kInt32 &&
         instruction->GetResultType() == DataType::Type::kFloat32)
      << "Unsupported SIMD type: " << instruction->GetPackedType();
  size_t vector_length = instruction->GetVectorLength();
  llvm::Type* dst = GetVectorType(GetFloat32Type(), vector_length);
  llvm::Value* result = __ CreateSIToFP(src, dst);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecNeg(HVecNeg* instruction) {
  llvm::Value* src = GetValue(instruction->InputAt(0));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateNeg(src);
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateFNeg(src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecAbs(HVecAbs* instruction) {
  llvm::Value* src = GetValue(instruction->GetInput());
  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kInt8:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      llvm::Value* is_int_min_poison = __ getFalse();
      result = __ CreateIntrinsic(llvm::Intrinsic::abs, src->getType(), {src, is_int_min_poison});
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      result = __ CreateUnaryIntrinsic(llvm::Intrinsic::fabs, src);
      break;
    }
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecNot(HVecNot* instruction) {
  llvm::Value* src = GetValue(instruction->InputAt(0));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateNot(src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecAdd(HVecAdd* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateAdd(lhs, rhs);
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateFAdd(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecSaturationAdd(HVecSaturationAdd* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kUint16:
      result = __ CreateBinaryIntrinsic(llvm::Intrinsic::uadd_sat, lhs, rhs);
      break;
    case DataType::Type::kInt8:
    case DataType::Type::kInt16:
      result = __ CreateBinaryIntrinsic(llvm::Intrinsic::sadd_sat, lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecHalvingAdd(HVecHalvingAdd* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));
  size_t vector_length = instruction->GetVectorLength();
  llvm::Type* packed_type = GetLLVMType(instruction->GetPackedType());
  llvm::Type* vector_type = GetVectorType(packed_type, vector_length);

  llvm::Value* result = nullptr;
  llvm::Value* lhs_ext = nullptr;
  llvm::Value* rhs_ext = nullptr;
  llvm::Value* vector = nullptr;

  // LLVM has no explicit halving add instruction.
  // The addition might cause overflow which could affect the result of the ashr/lshr, therefore
  // before any operation an extension is made to the type of the vectors' elements, and then they
  // get truncated to the original types.
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
      lhs_ext = __ CreateZExt(lhs, GetVectorType(GetInt16Type(), vector_length));
      rhs_ext = __ CreateZExt(rhs, GetVectorType(GetInt16Type(), vector_length));
      vector = __ CreateVectorSplat(vector_length, GetConstantInt(GetInt16Type(), 1));
      result = __ CreateAdd(lhs_ext, rhs_ext);
      if (instruction->IsRounded()) {
        result = __ CreateAdd(result, vector);
      }
      result = __ CreateLShr(result, vector);
      result = __ CreateTrunc(result, vector_type);
      break;
    case DataType::Type::kInt8:
      lhs_ext = __ CreateSExt(lhs, GetVectorType(GetInt16Type(), vector_length));
      rhs_ext = __ CreateSExt(rhs, GetVectorType(GetInt16Type(), vector_length));
      vector = __ CreateVectorSplat(vector_length, GetConstantInt(GetInt16Type(), 1));
      result = __ CreateAdd(lhs_ext, rhs_ext);
      if (instruction->IsRounded()) {
        result = __ CreateAdd(result, vector);
      }
      result = __ CreateAShr(result, vector);
      result = __ CreateTrunc(result, vector_type);
      break;
    case DataType::Type::kUint16:
      lhs_ext = __ CreateZExt(lhs, GetVectorType(GetInt32Type(), vector_length));
      rhs_ext = __ CreateZExt(rhs, GetVectorType(GetInt32Type(), vector_length));
      vector = __ CreateVectorSplat(vector_length, GetConstantInt(GetInt32Type(), 1));
      result = __ CreateAdd(lhs_ext, rhs_ext);
      if (instruction->IsRounded()) {
        result = __ CreateAdd(result, vector);
      }
      result = __ CreateLShr(result, vector);
      result = __ CreateTrunc(result, vector_type);
      break;
    case DataType::Type::kInt16:
      lhs_ext = __ CreateSExt(lhs, GetVectorType(GetInt32Type(), vector_length));
      rhs_ext = __ CreateSExt(rhs, GetVectorType(GetInt32Type(), vector_length));
      vector = __ CreateVectorSplat(vector_length, GetConstantInt(GetInt32Type(), 1));
      result = __ CreateAdd(lhs_ext, rhs_ext);
      if (instruction->IsRounded()) {
        result = __ CreateAdd(result, vector);
      }
      result = __ CreateAShr(result, vector);
      result = __ CreateTrunc(result, vector_type);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecSub(HVecSub* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateSub(lhs, rhs);
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateFSub(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecSaturationSub(HVecSaturationSub* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kUint16:
      result = __ CreateBinaryIntrinsic(llvm::Intrinsic::usub_sat, lhs, rhs);
      break;
    case DataType::Type::kInt8:
    case DataType::Type::kInt16:
      result = __ CreateBinaryIntrinsic(llvm::Intrinsic::ssub_sat, lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecMul(HVecMul* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateMul(lhs, rhs);
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateFMul(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecDiv(HVecDiv* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateFDiv(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecMin(HVecMin* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kUint16:
      result = __ CreateBinaryIntrinsic(llvm::Intrinsic::umin, lhs, rhs);
      break;
    case DataType::Type::kInt8:
    case DataType::Type::kInt16:
    case DataType::Type::kInt64:
    case DataType::Type::kInt32:
      result = __ CreateBinaryIntrinsic(llvm::Intrinsic::smin, lhs, rhs);
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateMinimum(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecMax(HVecMax* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kUint16:
      result = __ CreateBinaryIntrinsic(llvm::Intrinsic::umax, lhs, rhs);
      break;
    case DataType::Type::kInt8:
    case DataType::Type::kInt16:
    case DataType::Type::kInt64:
    case DataType::Type::kInt32:
      result = __ CreateBinaryIntrinsic(llvm::Intrinsic::smax, lhs, rhs);
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateMaximum(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecAnd(HVecAnd* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateAnd(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecAndNot(HVecAndNot* instruction) {
  // TODO: Use BIC (vector, register).
  LOG(FATAL) << "Unsupported SIMD instruction " << instruction->GetId();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecOr(HVecOr* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateOr(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecXor(HVecXor* instruction) {
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* rhs = GetValue(instruction->InputAt(1));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      result = __ CreateXor(lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecShl(HVecShl* instruction) {
  size_t vector_length = instruction->GetVectorLength();
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* scalar_value = GetValue(instruction->InputAt(1), instruction->GetPackedType());
  DCHECK(!scalar_value->getType()->isVectorTy());
  llvm::Value* vector = __ CreateVectorSplat(vector_length, scalar_value);

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateShl(lhs, vector);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecShr(HVecShr* instruction) {
  size_t vector_length = instruction->GetVectorLength();
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* scalar_value = GetValue(instruction->InputAt(1), instruction->GetPackedType());
  DCHECK(!scalar_value->getType()->isVectorTy());
  llvm::Value* vector = __ CreateVectorSplat(vector_length, scalar_value);

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateAShr(lhs, vector);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecUShr(HVecUShr* instruction) {
  size_t vector_length = instruction->GetVectorLength();
  llvm::Value* lhs = GetValue(instruction->InputAt(0));
  llvm::Value* scalar_value = GetValue(instruction->InputAt(1), instruction->GetPackedType());
  DCHECK(!scalar_value->getType()->isVectorTy());
  llvm::Value* vector = __ CreateVectorSplat(vector_length, scalar_value);

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateLShr(lhs, vector);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecSetScalars(HVecSetScalars* instruction) {
  DCHECK_EQ(1u, instruction->InputCount());  // only one input currently implemented
  size_t vector_length = instruction->GetVectorLength();
  llvm::Type* packed_type = GetLLVMType(instruction->GetPackedType());
  llvm::Type* vector_type = GetVectorType(packed_type, vector_length);
  llvm::Value* scalar_value = GetValue(instruction->InputAt(0), instruction->GetPackedType());

  // Zero out all other elements first.
  // __ Movi(dst.V16B(), 0);
  llvm::Value* result = llvm::ConstantAggregateZero::get(vector_type);

  // Set required elements.
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      result = __ CreateInsertElement(result, scalar_value, uint64_t{0});
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  AddValue(instruction, result);
}

// Some early revisions of the Cortex-A53 have an erratum (835769) whereby it is possible for a
// 64-bit scalar multiply-accumulate instruction in AArch64 state to generate an incorrect result.
// However vector MultiplyAccumulate instruction is not affected.
void InstructionCodeGeneratorARM64LLVM::VisitVecMultiplyAccumulate(
    HVecMultiplyAccumulate* instruction) {
  llvm::Value* acc = GetValue(instruction->InputAt(0));
  llvm::Value* lhs = GetValue(instruction->InputAt(1));
  llvm::Value* rhs = GetValue(instruction->InputAt(2));

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
      if (instruction->GetOpKind() == HInstruction::kAdd) {
        result = __ CreateMul(lhs, rhs);
        result = __ CreateAdd(acc, result);
      } else {
        DCHECK(instruction->GetOpKind() == HInstruction::kSub);
        result = __ CreateMul(lhs, rhs);
        result = __ CreateSub(acc, result);
      }
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

inline llvm::Value* InstructionCodeGeneratorARM64LLVM::CreateSabal(llvm::Value* acc,
                                                                   llvm::Value* lhs,
                                                                   llvm::Value* rhs,
                                                                   size_t index,
                                                                   llvm::Type* temp_vector_type,
                                                                   llvm::Type* out_vector_type) {
  llvm::Value* is_int_min_poison = __ getFalse();
  llvm::ConstantInt* from_index = GetConstantInt(GetInt64Type(), index);

  // Extract a subvector (which has the size of the output vector and datatype of input vector) from
  // the given vector
  llvm::Value* temp_lhs = __ CreateExtractVector(temp_vector_type, lhs, from_index);
  llvm::Value* temp_rhs = __ CreateExtractVector(temp_vector_type, rhs, from_index);
  temp_lhs = __ CreateSExt(temp_lhs, out_vector_type);
  temp_rhs = __ CreateSExt(temp_rhs, out_vector_type);
  llvm::Value* result = __ CreateSub(temp_lhs, temp_rhs);
  result = __ CreateIntrinsic(llvm::Intrinsic::abs, result->getType(), {result, is_int_min_poison});
  result = __ CreateAdd(result, acc);
  return result;
}

void InstructionCodeGeneratorARM64LLVM::VisitVecSADAccumulate(HVecSADAccumulate* instruction) {
  llvm::Value* acc = GetValue(instruction->InputAt(0));
  llvm::Value* lhs = GetValue(instruction->InputAt(1));
  llvm::Value* rhs = GetValue(instruction->InputAt(2));

  llvm::Value* is_int_min_poison = __ getFalse();
  llvm::Type* out_vector_type =
      GetVectorType(GetLLVMType(instruction->GetPackedType()), instruction->GetVectorLength());
  llvm::Type* temp_vector_type =
      GetVectorType(GetLLVMType(instruction->InputAt(1)->AsVecOperation()->GetPackedType()),
                    instruction->GetVectorLength());
  llvm::Value* result = nullptr;
  switch (instruction->InputAt(1)->AsVecOperation()->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
      switch (instruction->GetPackedType()) {
        case DataType::Type::kInt16:
          // in:<16 x i8>  out:<8 x i16>
          result = CreateSabal(acc, lhs, rhs, /*index*/ 0, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 8, temp_vector_type, out_vector_type);
          break;
        case DataType::Type::kInt32:
          // in:<16 x i8>  out:<4 x i32>
          result = CreateSabal(acc, lhs, rhs, /*index*/ 0, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 4, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 8, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 12, temp_vector_type, out_vector_type);
          break;
        case DataType::Type::kInt64:
          // in:<16 x i8>  out:<2 x i64>
          result = CreateSabal(acc, lhs, rhs, /*index*/ 0, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 2, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 4, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 6, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 8, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 10, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 12, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 14, temp_vector_type, out_vector_type);
          break;
        default:
          LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
          UNREACHABLE();
      }
      break;
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      switch (instruction->GetPackedType()) {
        case DataType::Type::kInt32:
          // in:<8 x i16>  out:<4 x i32>
          result = CreateSabal(acc, lhs, rhs, /*index*/ 0, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 4, temp_vector_type, out_vector_type);
          break;
        case DataType::Type::kInt64:
          // in:<8 x i16>  out:<2 x i64>
          result = CreateSabal(acc, lhs, rhs, /*index*/ 0, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 2, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 4, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 6, temp_vector_type, out_vector_type);
          break;
        default:
          LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
          UNREACHABLE();
      }
      break;
    case DataType::Type::kInt32:
      switch (instruction->GetPackedType()) {
        case DataType::Type::kInt32:
          // in:<4 x i32>  out:<4 x i32>
          result = __ CreateSub(lhs, rhs);
          result = __ CreateIntrinsic(
              llvm::Intrinsic::abs, result->getType(), {result, is_int_min_poison});
          result = __ CreateAdd(acc, result);
          break;
        case DataType::Type::kInt64:
          // in:<4 x i32>  out:<2 x i64>
          result = CreateSabal(acc, lhs, rhs, /*index*/ 0, temp_vector_type, out_vector_type);
          result = CreateSabal(result, lhs, rhs, /*index*/ 2, temp_vector_type, out_vector_type);
          break;
        default:
          LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
          UNREACHABLE();
      }
      break;
    case DataType::Type::kInt64:
      switch (instruction->GetPackedType()) {
        case DataType::Type::kInt64:
          // in:<2 x i64>  out:<2 x i64>
          result = __ CreateSub(lhs, rhs);
          result = __ CreateIntrinsic(
              llvm::Intrinsic::abs, result->getType(), {result, is_int_min_poison});
          result = __ CreateAdd(acc, result);
          break;
        default:
          LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
          UNREACHABLE();
      }
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

inline llvm::Value* InstructionCodeGeneratorARM64LLVM::CreateDotProd(llvm::Value* acc,
                                                                     llvm::Value* lhs,
                                                                     llvm::Value* rhs,
                                                                     size_t index,
                                                                     bool is_zero_extending,
                                                                     llvm::Type* temp_vector_type,
                                                                     llvm::Type* out_vector_type) {
  llvm::ConstantInt* from_index = GetConstantInt(GetInt64Type(), index);

  // Extract a subvector (which has the size of the output vector and datatype of input vector) from
  // the given vector
  llvm::Value* temp_lhs = __ CreateExtractVector(temp_vector_type, lhs, from_index);
  llvm::Value* temp_rhs = __ CreateExtractVector(temp_vector_type, rhs, from_index);
  if (is_zero_extending) {
    temp_lhs = __ CreateZExt(temp_lhs, out_vector_type);
    temp_rhs = __ CreateZExt(temp_rhs, out_vector_type);
  } else {
    temp_lhs = __ CreateSExt(temp_lhs, out_vector_type);
    temp_rhs = __ CreateSExt(temp_rhs, out_vector_type);
  }

  llvm::Value* result = __ CreateMul(temp_lhs, temp_rhs);
  result = __ CreateAdd(acc, result);
  return result;
}

void InstructionCodeGeneratorARM64LLVM::VisitVecDotProd(HVecDotProd* instruction) {
  DCHECK_EQ(instruction->GetPackedType(), DataType::Type::kInt32);
  DCHECK_EQ(4u, instruction->GetVectorLength());
  llvm::Value* acc = GetValue(instruction->InputAt(0));
  llvm::Value* lhs = GetValue(instruction->InputAt(1));
  llvm::Value* rhs = GetValue(instruction->InputAt(2));

  llvm::Type* out_vector_type =
      GetVectorType(GetLLVMType(instruction->GetPackedType()), instruction->GetVectorLength());
  llvm::Type* temp_vector_type =
      GetVectorType(GetLLVMType(instruction->InputAt(1)->AsVecOperation()->GetPackedType()),
                    instruction->GetVectorLength());
  bool is_zero_extending = instruction->IsZeroExtending();
  size_t inputs_data_size =
      DataType::Size(instruction->InputAt(1)->AsVecOperation()->GetPackedType());

  llvm::Value* result = nullptr;
  switch (inputs_data_size) {
    case 1u:
      // in:<16 x i8>   out:<4 x i32>
      result =
          CreateDotProd(acc, lhs, rhs, 0, is_zero_extending, temp_vector_type, out_vector_type);
      result =
          CreateDotProd(result, lhs, rhs, 4, is_zero_extending, temp_vector_type, out_vector_type);
      result =
          CreateDotProd(result, lhs, rhs, 8, is_zero_extending, temp_vector_type, out_vector_type);
      result =
          CreateDotProd(result, lhs, rhs, 12, is_zero_extending, temp_vector_type, out_vector_type);
      break;
    case 2u:
      // in:<8 x i16>   out:<4 x i32>
      result =
          CreateDotProd(acc, lhs, rhs, 0, is_zero_extending, temp_vector_type, out_vector_type);
      result =
          CreateDotProd(result, lhs, rhs, 4, is_zero_extending, temp_vector_type, out_vector_type);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type size: " << inputs_data_size;
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::VecAddress(HVecMemoryOperation* instruction,
                                                           llvm::Type* packed_type,
                                                           size_t packed_type_size,
                                                           bool is_string_char_at) {
  DCHECK_IMPLIES(packed_type->isIntegerTy(),
                 8 * packed_type_size == packed_type->getIntegerBitWidth());
  llvm::Value* base = GetValue(instruction->InputAt(0));
  llvm::Value* index = GetValue(instruction->InputAt(1));

  if (instruction->InputAt(1)->IsIntermediateAddressIndex()) {
    DCHECK(!is_string_char_at);
    return CreateGEP(base, index);
  }

  uint32_t offset = is_string_char_at ? mirror::String::ValueOffset().Uint32Value()
                                      : mirror::Array::DataOffset(packed_type_size).Uint32Value();

  // HIntermediateAddress optimization is only applied for scalar ArrayGet and ArraySet.
  DCHECK(!instruction->InputAt(0)->IsIntermediateAddress());

  llvm::Value* array_base = CreateGEP(base, offset);
  return CreateGEP(packed_type, array_base, index);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecLoad(HVecLoad* instruction) {
  size_t vector_length = instruction->GetVectorLength();
  size_t packed_type_size = DataType::Size(instruction->GetPackedType());
  llvm::Type* packed_type = GetLLVMType(instruction->GetPackedType());
  llvm::Type* vector_type = GetVectorType(packed_type, vector_length);

  llvm::Value* result = nullptr;
  switch (instruction->GetPackedType()) {
    case DataType::Type::kInt16:  // (short) s.charAt(.) can yield HVecLoad/Int16/StringCharAt.
    case DataType::Type::kUint16:
      // Special handling of compressed/uncompressed string load.
      if (mirror::kUseStringCompression && instruction->IsStringCharAt()) {
        // vixl::aarch64::Label uncompressed_load, done;
        // Test compression bit.
        static_assert(static_cast<uint32_t>(mirror::StringCompressionFlag::kCompressed) == 0u,
                      "Expecting 0=compressed, 1=uncompressed");
        uint32_t count_offset = mirror::String::CountOffset().Uint32Value();

        llvm::BasicBlock* compressed_block = codegen_->CreateBasicBlock();
        llvm::BasicBlock* uncompressed_block = codegen_->CreateBasicBlock();
        llvm::BasicBlock* done_block = codegen_->CreateBasicBlock();

        // __ Ldr(length, HeapOperand(InputRegisterAt(instruction, 0), count_offset));
        llvm::Value* length =
            CreateLoadWithOffset(GetUint32Type(), GetValue(instruction->InputAt(0)), count_offset);
        // __ Tbnz(length.W(), 0, &uncompressed_load);
        llvm::Value* uncompressed_flag = __ CreateAnd(length, 1u);
        llvm::Value* is_uncompressed =
            __ CreateICmpNE(uncompressed_flag, GetConstantZero(uncompressed_flag->getType()));
        __ CreateCondBr(is_uncompressed, uncompressed_block, compressed_block);

        __ SetInsertPoint(compressed_block);
        // Zero extend 8 compressed bytes into 8 chars.
        // __ Ldr(DRegisterFrom(locations->Out()).V8B(),
        //        VecNEONAddress(instruction, &temps, 1, /*is_string_char_at*/ true, &scratch));
        llvm::Value* compressed_address =
            VecAddress(instruction, GetUint8Type(), 1, /* is_string_char_at= */ true);
        llvm::Type* byte_vector_type =
            llvm::VectorType::get(GetUint8Type(), llvm::ElementCount::getFixed(vector_length));
        llvm::Value* compressed_result = CreateLoad(byte_vector_type, compressed_address);
        // __ Uxtl(reg.V8H(), reg.V8B());
        compressed_result = __ CreateZExt(compressed_result, vector_type);
        // __ B(&done);
        __ CreateBr(done_block);

        // Load 8 direct uncompressed chars.
        // __ Bind(&uncompressed_load);
        __ SetInsertPoint(uncompressed_block);
        // __ Ldr(reg, VecNEONAddress(instruction, &temps, size, /*is_string_char_at*/ true,
        //        &scratch));
        llvm::Value* uncompressed_address =
            VecAddress(instruction, packed_type, packed_type_size, /* is_string_char_at= */ true);
        llvm::Value* uncompressed_result = CreateLoad(vector_type, uncompressed_address);
        __ CreateBr(done_block);

        // __ Bind(&done);
        __ SetInsertPoint(done_block);
        llvm::PHINode* result_phi = __ CreatePHI(vector_type, 2);
        result_phi->addIncoming(compressed_result, compressed_block);
        result_phi->addIncoming(uncompressed_result, uncompressed_block);
        result = result_phi;
        break;
      }
      FALLTHROUGH_INTENDED;
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kInt32:
    case DataType::Type::kFloat32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat64: {
      // __ Ldr(reg, VecNEONAddress(instruction, &temps, size, instruction->IsStringCharAt(),
      //        &scratch));
      llvm::Value* address =
          VecAddress(instruction, packed_type, packed_type_size, instruction->IsStringCharAt());
      result = CreateLoad(vector_type, address);
      break;
    }
    case DataType::Type::kBool: {
      llvm::Type* loaded_vector_type = GetVectorType(GetUint8Type(), vector_length);
      llvm::Value* address = VecAddress(
          instruction, GetUint8Type(), /* packed_type_size= */ 1, instruction->IsStringCharAt());
      result = CreateLoad(loaded_vector_type, address);
      result = __ CreateTrunc(result, vector_type);
      break;
    }
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
  DCHECK(result != nullptr);
  AddValue(instruction, result);
}

void InstructionCodeGeneratorARM64LLVM::VisitVecStore(HVecStore* instruction) {
  llvm::Type* packed_type = GetLLVMType(instruction->GetPackedType());
  size_t packed_type_size = DataType::Size(instruction->GetPackedType());
  llvm::Value* value = GetValue(instruction->InputAt(2));

  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool: {
      llvm::Type* stored_vector_type =
          GetVectorType(GetUint8Type(), instruction->GetVectorLength());
      value = __ CreateZExt(value, stored_vector_type);
      llvm::Value* address =
          VecAddress(instruction, GetUint8Type(), /* packed_type_size= */ 1, false);
      CreateStore(value, address);
      break;
    }
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kFloat32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat64: {
      llvm::Value* address = VecAddress(instruction, packed_type, packed_type_size, false);
      // __ Str(reg, VecNEONAddress(instruction, &temps, size, /*is_string_char_at*/ false,
      //        &scratch));
      CreateStore(value, address);
      break;
    }
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void InstructionCodeGeneratorARM64LLVM::VisitVecPredSetAll(HVecPredSetAll* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecPredWhile(HVecPredWhile* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecPredToBoolean(HVecPredToBoolean* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecPredNot(HVecPredNot* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecEqual(HVecEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecNotEqual(HVecNotEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecLessThan(HVecLessThan* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecLessThanOrEqual(HVecLessThanOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecGreaterThan(HVecGreaterThan* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecGreaterThanOrEqual(
    HVecGreaterThanOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecBelow(HVecBelow* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecBelowOrEqual(HVecBelowOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecAbove(HVecAbove* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorARM64LLVM::VisitVecAboveOrEqual(HVecAboveOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetLLVMType(DataType::Type type) const {
  return codegen_->GetLLVMType(type);
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetPointerType() const {
  return codegen_->GetPointerType();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetUncompressedGCPointerType() const {
  return codegen_->GetUncompressedGCPointerType();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetCompressedGCPointerType() const {
  return codegen_->GetCompressedGCPointerType();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetMethodPointerType() const {
  return codegen_->GetMethodPointerType();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetBooleanType() const {
  return codegen_->GetBooleanType();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetInt8Type() const {
  return codegen_->GetInt8Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetUint8Type() const {
  return codegen_->GetUint8Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetInt16Type() const {
  return codegen_->GetInt16Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetUint16Type() const {
  return codegen_->GetUint16Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetInt32Type() const {
  return codegen_->GetInt32Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetUint32Type() const {
  return codegen_->GetUint32Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetInt64Type() const {
  return codegen_->GetInt64Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetUint64Type() const {
  return codegen_->GetUint64Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetFloat32Type() const {
  return codegen_->GetFloat32Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetFloat64Type() const {
  return codegen_->GetFloat64Type();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetVoidType() const {
  return codegen_->GetVoidType();
}

llvm::Type* InstructionCodeGeneratorARM64LLVM::GetVectorType(llvm::Type* packed_type,
                                                             size_t length) const {
  return codegen_->GetVectorType(packed_type, length);
}

llvm::Function* InstructionCodeGeneratorARM64LLVM::GetFunction() const {
  return codegen_->GetFunction();
}

llvm::Constant* InstructionCodeGeneratorARM64LLVM::GetConstantZero(llvm::Type* type) const {
  return codegen_->GetConstantZero(type);
}

llvm::ConstantInt* InstructionCodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type,
                                                                     int32_t value) const {
  return codegen_->GetConstantInt(type, value);
}

llvm::ConstantInt* InstructionCodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type,
                                                                     int64_t value) const {
  return codegen_->GetConstantInt(type, value);
}

llvm::ConstantInt* InstructionCodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type,
                                                                     uint32_t value) const {
  return codegen_->GetConstantInt(type, value);
}

llvm::ConstantInt* InstructionCodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type,
                                                                     uint64_t value) const {
  return codegen_->GetConstantInt(type, value);
}

void InstructionCodeGeneratorARM64LLVM::AddValue(HInstruction* instruction,
                                                 llvm::Value* value) const {
  codegen_->AddValue(instruction, value);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::GetValue(HInstruction* instruction,
                                                         DataType::Type value_type) const {
  return codegen_->GetValue(instruction, value_type);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::GetThreadPointerValue(llvm::Type* type) const {
  return codegen_->GetThreadPointerValue(type);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::GetMarkingRegisterValue(llvm::Type* type) const {
  return codegen_->GetMarkingRegisterValue(type);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::GetImplicitSuspendRegisterValue(
    llvm::Type* type) const {
  return codegen_->GetImplicitSuspendRegisterValue(type);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::GetStackPointerValue() const {
  return codegen_->GetStackPointerValue();
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::CreateLoad(llvm::Type* type,
                                                           llvm::Value* address) const {
  return codegen_->CreateLoad(type, address);
}

void InstructionCodeGeneratorARM64LLVM::CreateStore(llvm::Value* value,
                                                    llvm::Value* address,
                                                    bool is_volatile) const {
  return codegen_->CreateStore(value, address, is_volatile);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::CreateGEP(llvm::Value* address,
                                                          llvm::Value* offset) const {
  return codegen_->CreateGEP(address, offset);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::CreateGEP(llvm::Value* address,
                                                          int64_t offset) const {
  return codegen_->CreateGEP(address, offset);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::CreateGEP(llvm::Type* type,
                                                          llvm::Value* address,
                                                          llvm::Value* offset) const {
  return codegen_->CreateGEP(type, address, offset);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::CreateGEP(llvm::Type* type,
                                                          llvm::Value* address,
                                                          int64_t offset) const {
  return codegen_->CreateGEP(type, address, offset);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::CreateLoadFromThreadPointer(llvm::Type* type,
                                                                            size_t offset) const {
  return codegen_->CreateLoadFromThreadPointer(type, offset);
}

void InstructionCodeGeneratorARM64LLVM::CreateStoreToThreadPointer(llvm::Value* value,
                                                                   size_t offset) const {
  codegen_->CreateStoreToThreadPointer(value, offset);
}

llvm::Value* InstructionCodeGeneratorARM64LLVM::CreateLoadWithOffset(llvm::Type* type,
                                                                     llvm::Value* address,
                                                                     int64_t offset) const {
  return codegen_->CreateLoadWithOffset(type, address, offset);
}

}  // namespace arm64_llvm
}  // namespace art HIDDEN
