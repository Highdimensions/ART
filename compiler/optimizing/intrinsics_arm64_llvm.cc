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

#include "intrinsics_arm64_llvm.h"

#include "art_method.h"
#include "base/bit_utils.h"
#include "code_generator_arm64_llvm.h"
#include "data_type-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "heap_poisoning.h"
#include "intrinsic_objects.h"
#include "intrinsics.h"
#include "intrinsics_utils.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "lock_word.h"
#include "mirror/reference.h"
#include "mirror/string-inl.h"
#include "mirror/var_handle.h"
#include "well_known_classes.h"

// TODO(LLVM): Make LLVM compile with these warnings.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#pragma GCC diagnostic ignored "-Wused-but-marked-unused"
#pragma GCC diagnostic ignored "-Wdeprecated-copy-with-dtor"
#pragma GCC diagnostic ignored "-Wframe-larger-than"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#pragma GCC diagnostic pop

#define TODO()                                               \
  do {                                                       \
    LOG(FATAL) << "TODO: Implement " << __PRETTY_FUNCTION__; \
  } while (false)

#define UNUSED(x) ((void)(x))

namespace art HIDDEN {

namespace arm64_llvm {

ArenaAllocator* IntrinsicCodeGeneratorARM64LLVM::GetAllocator() {
  return codegen_->GetGraph()->GetAllocator();
}

#define __ codegen_->GetIRBuilder()->

// Default slow-path for fallback (calling the managed code to handle the intrinsic) in an
// intrinsified call. This will copy the arguments into the positions for a regular call.
//
// Note: The actual parameters are required to be in the locations given by the invoke's location
//       summary. If an intrinsic modifies those locations before a slowpath call, they must be
//       restored!
//
// Note: If an invoke wasn't sharpened, we will put down an invoke-virtual here. That's potentially
//       sub-optimal (compared to a direct pointer call), but this is a slow-path.

class IntrinsicSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  explicit IntrinsicSlowPathARM64LLVM(HInvoke* invoke,
                                      llvm::BasicBlock* entry_block,
                                      llvm::BasicBlock* exit_block,
                                      llvm::PHINode* result_phi = nullptr)
      : SlowPathCodeARM64LLVM(invoke, entry_block, exit_block),
        result_phi_(result_phi),
        invoke_(invoke) {}

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen_ = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen_->SetCurrentBlock(instruction_->GetBlock());

    __ SetInsertPoint(GetEntryBlock());

    [[maybe_unused]] llvm::Value* result = nullptr;
    if (invoke_->IsInvokeStaticOrDirect()) {
      HInvokeStaticOrDirect* invoke_static_or_direct = invoke_->AsInvokeStaticOrDirect();
      DCHECK_NE(invoke_static_or_direct->GetMethodLoadKind(), MethodLoadKind::kRecursive);
      DCHECK_NE(invoke_static_or_direct->GetCodePtrLocation(),
                CodePtrLocation::kCallCriticalNative);
      result = codegen_->GenerateStaticOrDirectCall(invoke_static_or_direct);
    } else if (invoke_->IsInvokeVirtual()) {
      result = codegen_->GenerateVirtualCall(invoke_->AsInvokeVirtual());
    } else {
      DCHECK(invoke_->IsInvokePolymorphic());
      result = codegen_->GenerateInvokePolymorphicCall(invoke_->AsInvokePolymorphic());
    }

    if (result_phi_) {
      result_phi_->addIncoming(result, __ GetInsertBlock());
    }

    __ CreateBr(GetExitBlock());
  }

  const char* GetDescription() const override { return "IntrinsicSlowPath"; }

 protected:
  llvm::PHINode* result_phi_;

 private:
  HInvoke* invoke_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicSlowPathARM64LLVM);
};

// The MethodHandle.invokeExact intrinsic sets up arguments to match the target method call. If we
// need to go to the slow path, we call art_quick_invoke_polymorphic_with_hidden_receiver, which
// expects the MethodHandle object in w0 (in place of the actual ArtMethod).
class InvokePolymorphicSlowPathARM64LLVM : public SlowPathCodeARM64LLVM {
 public:
  InvokePolymorphicSlowPathARM64LLVM(HInvoke* invoke,
                                     llvm::BasicBlock* entry_block,
                                     llvm::BasicBlock* exit_block,
                                     llvm::PHINode* result_phi,
                                     llvm::Value* method_handle)
      : SlowPathCodeARM64LLVM(invoke, entry_block, exit_block),
        result_phi_(result_phi),
        method_handle_(method_handle),
        invoke_(invoke) {
    DCHECK(invoke->IsInvokePolymorphic());
  }

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    CodeGeneratorARM64LLVM* codegen_ = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
    codegen_->SetCurrentBlock(instruction_->GetBlock());

    __ SetInsertPoint(GetEntryBlock());

    llvm::SmallVector<llvm::Value*> args;
    uint32_t number_of_args = invoke_->GetNumberOfArguments();
    args.reserve(number_of_args - 1 + 2);
    args.push_back(codegen_->GetUndefCurrentMethodPointer());
    // Passing `MethodHandle` object as hidden argument.
    args.push_back(method_handle_);
    for (uint32_t i = 1; i < number_of_args; ++i) {
      args.push_back(codegen_->GetValue(invoke_->InputAt(i)));
    }
    llvm::Type* result_type =
        result_phi_ != nullptr ? result_phi_->getType() : codegen_->GetVoidType();
    codegen_->SetInvokeRuntimeParametersAndReturnType(
        args, result_type, llvm::CallingConv::ARTInvokeRuntimeHiddenReceiver);
    codegen_->InvokeRuntime(QuickEntrypointEnum::kQuickInvokePolymorphicWithHiddenReceiver,
                            invoke_);
    if (result_phi_) {
      result_phi_->addIncoming(codegen_->GetInvokeRuntimeResult(), __ GetInsertBlock());
    }

    __ CreateBr(GetExitBlock());
  }

  const char* GetDescription() const override { return "InvokePolymorphicSlowPathARM64LLVM"; }

 private:
  llvm::PHINode* result_phi_;
  llvm::Value* const method_handle_;
  HInvoke* invoke_;

  DISALLOW_COPY_AND_ASSIGN(InvokePolymorphicSlowPathARM64LLVM);
};

void IntrinsicCodeGeneratorARM64LLVM::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  llvm::Value* input = GetValue(invoke->InputAt(0));
  llvm::Value* res = __ CreateBitCast(input, GetInt64Type());
  AddValue(invoke, res);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  llvm::Value* input = GetValue(invoke->InputAt(0));
  llvm::Value* res = __ CreateBitCast(input, GetFloat64Type());
  AddValue(invoke, res);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  llvm::Value* input = GetValue(invoke->InputAt(0));
  llvm::Value* res = __ CreateBitCast(input, GetInt32Type());
  AddValue(invoke, res);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  llvm::Value* input = GetValue(invoke->InputAt(0));
  llvm::Value* res = __ CreateBitCast(input, GetFloat32Type());
  AddValue(invoke, res);
}

static void GenByteSwap(HInvoke* invoke, CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0), invoke->GetType());
  llvm::CallInst* result = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, input);
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitIntegerReverseBytes(HInvoke* invoke) {
  GenByteSwap(invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitLongReverseBytes(HInvoke* invoke) {
  GenByteSwap(invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitShortReverseBytes(HInvoke* invoke) {
  GenByteSwap(invoke, codegen_);
}

static void GenLeadingZeros(HInvoke* invoke,
                            DataType::Type type,
                            CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0), type);
  llvm::Value* is_poison = __ getFalse();
  llvm::Value* result =
      __ CreateIntrinsic(llvm::Intrinsic::ctlz, input->getType(), {input, is_poison});
  if (type == DataType::Type::kInt64) {
    result = __ CreateTrunc(result, codegen_->GetInt32Type());
  }
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  GenLeadingZeros(invoke, DataType::Type::kInt32, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  GenLeadingZeros(invoke, DataType::Type::kInt64, codegen_);
}

static void GenTrailingZeros(HInvoke* invoke,
                             DataType::Type type,
                             CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0), type);
  llvm::Value* is_poison = __ getFalse();
  llvm::Value* result =
      __ CreateIntrinsic(llvm::Intrinsic::cttz, input->getType(), {input, is_poison});
  if (type == DataType::Type::kInt64) {
    result = __ CreateTrunc(result, codegen_->GetInt32Type());
  }
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  GenTrailingZeros(invoke, DataType::Type::kInt32, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  GenTrailingZeros(invoke, DataType::Type::kInt64, codegen_);
}

static void GenBitReverse(HInvoke* invoke, CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0), invoke->GetType());
  llvm::CallInst* result = __ CreateUnaryIntrinsic(llvm::Intrinsic::bitreverse, input);
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitIntegerReverse(HInvoke* invoke) {
  GenBitReverse(invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitLongReverse(HInvoke* invoke) {
  GenBitReverse(invoke, codegen_);
}

static void GenBitCount(HInvoke* invoke, DataType::Type type, CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0), type);
  llvm::Value* result = __ CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, input);

  // NOTE: @llvm.ctpop() returns the same type as its argument, so we need to truncate i64 to i32
  // for LongBitCount.
  if (type == DataType::Type::kInt64) {
    result = __ CreateTrunc(result, codegen_->GetInt32Type());
  }
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitLongBitCount(HInvoke* invoke) {
  GenBitCount(invoke, DataType::Type::kInt64, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitIntegerBitCount(HInvoke* invoke) {
  GenBitCount(invoke, DataType::Type::kInt32, codegen_);
}

static void GenHighestOneBit(HInvoke* invoke,
                             DataType::Type type,
                             CodeGeneratorARM64LLVM* codegen_) {
  DCHECK(type == DataType::Type::kInt32 || type == DataType::Type::kInt64);
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0), type);
  llvm::Value* zero = codegen_->GetConstantZero(input->getType());
  llvm::Value* is_input_zero = __ CreateICmpEQ(input, zero);
  // We can treat ctlz of 0 as poison, since we check for that case explicitly.
  llvm::Value* is_zero_poison = __ getTrue();
  llvm::CallInst* clz =
      __ CreateIntrinsic(llvm::Intrinsic::ctlz, input->getType(), {input, is_zero_poison});

  llvm::Value* highest_possible_bit = type == DataType::Type::kInt64
                                          ? __ getInt64(uint64_t{1} << 63)
                                          : __ getInt32(uint64_t{1} << 31);

  llvm::Value* non_zero_result = __ CreateLShr(highest_possible_bit, clz);
  llvm::Value* result = __ CreateSelect(is_input_zero, zero, non_zero_result);

  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitIntegerHighestOneBit(HInvoke* invoke) {
  GenHighestOneBit(invoke, DataType::Type::kInt32, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitLongHighestOneBit(HInvoke* invoke) {
  GenHighestOneBit(invoke, DataType::Type::kInt64, codegen_);
}

static void GenLowestOneBit(HInvoke* invoke,
                            DataType::Type type,
                            CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0), type);
  llvm::Value* neg = __ CreateNeg(input);
  llvm::Value* result = __ CreateAnd(input, neg);
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitIntegerLowestOneBit(HInvoke* invoke) {
  GenLowestOneBit(invoke, DataType::Type::kInt32, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitLongLowestOneBit(HInvoke* invoke) {
  GenLowestOneBit(invoke, DataType::Type::kInt64, codegen_);
}

static void GenMathFunc(llvm::Intrinsic::ID intrinsic_id,
                        HInvoke* invoke,
                        CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* result = __ CreateUnaryIntrinsic(intrinsic_id, input);
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathSqrt(HInvoke* invoke) {
  GenMathFunc(llvm::Intrinsic::sqrt, invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathCeil(HInvoke* invoke) {
  GenMathFunc(llvm::Intrinsic::ceil, invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathFloor(HInvoke* invoke) {
  GenMathFunc(llvm::Intrinsic::floor, invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathRint(HInvoke* invoke) {
  GenMathFunc(llvm::Intrinsic::rint, invoke, codegen_);
}

void GenMathRound(HInvoke* invoke, bool is_double, CodeGeneratorARM64LLVM* codegen_) {
  // Math.round() semantics:
  // 1. Returns the closest int to the argument, with ties rounding to positive infinity.
  // 2. If the argument is NaN, the result is 0.
  // 3. If the argument is negative infinity or any value less than or equal to the value of
  //    Integer/Long.MIN_VALUE, the result is equal to the value of Integer/Long.MIN_VALUE.
  // 4. If the argument is positive infinity or any value greater than or equal to the value of
  //    Integer/Long.MAX_VALUE, the result is equal to the value of Integer/Long.MAX_VALUE.
  llvm::Type* result_type = is_double ? codegen_->GetInt64Type() : codegen_->GetInt32Type();

  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0));

  // The check for a tie is enough to determine whether incrementation is needed,
  // since `input - rounded_input == 0.5` only if a tie was
  // rounded down, which would mean it's negative.
  // TODO: Revisit this decision to not use branches. Unconditionally executing the round and lround
  // puts extra strain on the FPU, which could slow this intrinsic down, especially if the input
  // consists of mostly positive numbers.
  llvm::Value* rounded_float_input = __ CreateUnaryIntrinsic(llvm::Intrinsic::round, input);
  llvm::Value* round_diff = __ CreateFSub(input, rounded_float_input);
  llvm::Value* half_float_value = llvm::ConstantFP::get(input->getType(), 0.5);
  llvm::Value* do_increment = __ CreateFCmpOEQ(round_diff, half_float_value);

  // NOTE: lround's implementation-defined behavior satisfies points 2, 3, and 4 above.
  llvm::Value* rounded_input =
      __ CreateIntrinsic(llvm::Intrinsic::lround, {result_type, input->getType()}, input);
  llvm::Value* incremented_rounded_input =
      __ CreateAdd(rounded_input, codegen_->GetConstantInt(result_type, 1));
  llvm::Value* result = __ CreateSelect(do_increment, incremented_rounded_input, rounded_input);
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathRoundDouble(HInvoke* invoke) {
  GenMathRound(invoke, true, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathRoundFloat(HInvoke* invoke) {
  GenMathRound(invoke, false, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMemoryPeekByte(HInvoke* invoke) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* ptr = __ CreateIntToPtr(input, codegen_->GetPointerType());
  llvm::Value* loaded_byte = CreateLoad(__ getInt8Ty(), ptr);
  codegen_->AddValue(invoke, loaded_byte);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMemoryPeekIntNative(HInvoke* invoke) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* ptr = __ CreateIntToPtr(input, codegen_->GetPointerType());
  llvm::Value* result = CreateLoad(__ getInt32Ty(), ptr);
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMemoryPeekLongNative(HInvoke* invoke) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* ptr = __ CreateIntToPtr(input, codegen_->GetPointerType());
  llvm::Value* result = CreateLoad(__ getInt64Ty(), ptr);
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMemoryPeekShortNative(HInvoke* invoke) {
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* ptr = __ CreateIntToPtr(input, codegen_->GetPointerType());
  llvm::Value* loaded_halfword = CreateLoad(__ getInt16Ty(), ptr);
  codegen_->AddValue(invoke, loaded_halfword);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMemoryPokeByte(HInvoke* invoke) {
  llvm::Value* location = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* value = codegen_->GetValue(invoke->InputAt(1));
  DCHECK(location->getType()->isIntegerTy());
  DCHECK(value->getType()->isIntegerTy());
  llvm::Value* location_ptr = __ CreateIntToPtr(location, codegen_->GetPointerType());
  llvm::Value* byte_value = __ CreateTrunc(value, __ getInt8Ty());
  CreateStore(byte_value, location_ptr);
}

static void GenStore(HInvoke* invoke, CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* location = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* value = codegen_->GetValue(invoke->InputAt(1));
  DCHECK(location->getType()->isIntegerTy());
  DCHECK(value->getType()->isIntegerTy());
  llvm::Value* location_ptr = __ CreateIntToPtr(location, codegen_->GetPointerType());
  codegen_->CreateStore(value, location_ptr);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMemoryPokeIntNative(HInvoke* invoke) {
  GenStore(invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMemoryPokeLongNative(HInvoke* invoke) {
  GenStore(invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMemoryPokeShortNative(HInvoke* invoke) {
  llvm::Value* location = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* value = codegen_->GetValue(invoke->InputAt(1));
  DCHECK(location->getType()->isIntegerTy());
  DCHECK(value->getType()->isIntegerTy());
  llvm::Value* location_ptr = __ CreateIntToPtr(location, codegen_->GetPointerType());
  llvm::Value* halfword_value = __ CreateTrunc(value, __ getInt16Ty());
  CreateStore(halfword_value, location_ptr);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitThreadCurrentThread([[maybe_unused]] HInvoke* invoke) {
  llvm::Value* result =
      codegen_->CreateLoadFromThreadPointer(codegen_->GetUncompressedGCPointerType(),
                                            Thread::PeerOffset<kArm64PointerSize>().Int32Value());
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGet(HInvoke* invoke) { VisitJdkUnsafeGet(invoke); }
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetVolatile(HInvoke* invoke) {
  VisitJdkUnsafeGetVolatile(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetLong(HInvoke* invoke) {
  VisitJdkUnsafeGetLong(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  VisitJdkUnsafeGetLongVolatile(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetObject(HInvoke* invoke) {
  VisitJdkUnsafeGetReference(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  VisitJdkUnsafeGetReferenceVolatile(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetByte(HInvoke* invoke) {
  VisitJdkUnsafeGetByte(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetAbsolute(HInvoke* invoke) {
  VisitJdkUnsafeGetAbsolute(invoke);
}

static void GenUnsafeGet(HInvoke* invoke,
                         DataType::Type type,
                         llvm::AtomicOrdering ordering,
                         CodeGeneratorARM64LLVM* codegen_) {
  DCHECK((type == DataType::Type::kInt8) || (type == DataType::Type::kInt32) ||
         (type == DataType::Type::kInt64) || (type == DataType::Type::kReference));

  llvm::Value* base = codegen_->GetValue(invoke->InputAt(1));
  llvm::Value* offset = codegen_->GetValue(invoke->InputAt(2));
  llvm::Value* address = codegen_->CreateGEP(base, offset);

  llvm::Type* llvm_type = codegen_->GetLLVMType(type);
  // NOTE: In the arm64 code generator implementation only the atomic version can have an implicit
  // null check, so we mimic that behaviour here as well.
  llvm::Value* result =
      ordering != llvm::AtomicOrdering::Unordered && codegen_->ShouldRecordImplicitNullCheck(invoke)
          ? codegen_->CreateLoadAcquireWithImplicitNullCheck(
                invoke->GetImplicitNullCheck(), llvm_type, address)
          : codegen_->CreateLoad(llvm_type, address, ordering);

  if (type == DataType::Type::kReference) {
    result = codegen_->MaybeUnpoisonHeapReference(result);
  }
  codegen_->AddValue(invoke, result);
}

static void GenUnsafeGetAbsolute(HInvoke* invoke,
                                 DataType::Type type,
                                 llvm::AtomicOrdering ordering,
                                 CodeGeneratorARM64LLVM* codegen_) {
  DCHECK((type == DataType::Type::kInt8) || (type == DataType::Type::kInt32) ||
         (type == DataType::Type::kInt64));

  llvm::Value* address = codegen_->GetValue(invoke->InputAt(1), DataType::Type::kInt64);
  llvm::Value* address_ptr = __ CreateIntToPtr(address, codegen_->GetPointerType());
  llvm::Type* llvm_type = codegen_->GetLLVMType(type);
  // NOTE: In the arm64 code generator implementation only the atomic version can have an implicit
  // null check, so we mimic that behaviour here as well.
  llvm::Value* result =
      ordering != llvm::AtomicOrdering::Unordered && codegen_->ShouldRecordImplicitNullCheck(invoke)
          ? codegen_->CreateLoadAcquireWithImplicitNullCheck(
                invoke->GetImplicitNullCheck(), llvm_type, address_ptr)
          : codegen_->CreateLoad(llvm_type, address_ptr, ordering);
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke, DataType::Type::kInt32, llvm::AtomicOrdering::Unordered, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(
      invoke, DataType::Type::kInt32, llvm::AtomicOrdering::SequentiallyConsistent, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetAcquire(HInvoke* invoke) {
  GenUnsafeGet(invoke, DataType::Type::kInt32, llvm::AtomicOrdering::Acquire, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke, DataType::Type::kInt64, llvm::AtomicOrdering::Unordered, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(
      invoke, DataType::Type::kInt64, llvm::AtomicOrdering::SequentiallyConsistent, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetLongAcquire(HInvoke* invoke) {
  GenUnsafeGet(invoke, DataType::Type::kInt64, llvm::AtomicOrdering::Acquire, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetReference(HInvoke* invoke) {
  GenUnsafeGet(invoke, DataType::Type::kReference, llvm::AtomicOrdering::Unordered, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetReferenceVolatile(HInvoke* invoke) {
  GenUnsafeGet(
      invoke, DataType::Type::kReference, llvm::AtomicOrdering::SequentiallyConsistent, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetReferenceAcquire(HInvoke* invoke) {
  GenUnsafeGet(invoke, DataType::Type::kReference, llvm::AtomicOrdering::Acquire, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetByte(HInvoke* invoke) {
  GenUnsafeGet(invoke, DataType::Type::kInt8, llvm::AtomicOrdering::Unordered, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetAbsolute(HInvoke* invoke) {
  GenUnsafeGetAbsolute(invoke, DataType::Type::kInt32, llvm::AtomicOrdering::Unordered, codegen_);
}

static void GenUnsafePut(HInvoke* invoke,
                         DataType::Type type,
                         llvm::AtomicOrdering ordering,
                         CodeGeneratorARM64LLVM* codegen_) {
  static constexpr int kOffsetIndex = 2;
  static constexpr int kValueIndex = 3;
  llvm::Value* base = codegen_->GetValue(invoke->InputAt(1));               // Object pointer.
  llvm::Value* offset = codegen_->GetValue(invoke->InputAt(kOffsetIndex));  // Long offset.
  llvm::Value* value = codegen_->GetValue(invoke->InputAt(kValueIndex));
  llvm::Value* source = value;

  if (kPoisonHeapReferences && type == DataType::Type::kReference &&
      !IsZeroBitPattern(invoke->InputAt(kValueIndex))) {
    source = codegen_->PoisonHeapReference(source);
  }
  llvm::Value* exact_address = codegen_->CreateGEP(base, offset);
  llvm::Type* llvm_type = codegen_->GetLLVMType(type);
  if (source->getType() != llvm_type) {
    bool is_value_signed = !DataType::IsUnsignedType(invoke->InputAt(kValueIndex)->GetType());
    source = __ CreateIntCast(source, llvm_type, is_value_signed);
  }
  codegen_->CreateStore(source, exact_address, ordering);

  if (type == DataType::Type::kReference && !IsZeroBitPattern(invoke->InputAt(kValueIndex))) {
    bool value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen_->MaybeMarkGCCard(base, value, value_can_be_null);
  }
}

static void GenUnsafePutAbsolute(HInvoke* invoke,
                                 [[maybe_unused]] DataType::Type type,
                                 llvm::AtomicOrdering ordering,
                                 CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* address = codegen_->GetValue(invoke->InputAt(1), DataType::Type::kInt64);
  llvm::Value* address_ptr = __ CreateIntToPtr(address, codegen_->GetPointerType());
  llvm::Value* value = codegen_->GetValue(invoke->InputAt(2));
  codegen_->CreateStore(value, address_ptr, ordering);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePut(HInvoke* invoke) { VisitJdkUnsafePut(invoke); }
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutOrderedInt(HInvoke* invoke) {
  VisitJdkUnsafePutOrderedInt(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutVolatile(HInvoke* invoke) {
  VisitJdkUnsafePutVolatile(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutObject(HInvoke* invoke) {
  VisitJdkUnsafePutReference(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutOrderedObject(HInvoke* invoke) {
  VisitJdkUnsafePutOrderedObject(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  VisitJdkUnsafePutReferenceVolatile(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutLong(HInvoke* invoke) {
  VisitJdkUnsafePutLong(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  VisitJdkUnsafePutLongOrdered(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  VisitJdkUnsafePutLongVolatile(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutByte(HInvoke* invoke) {
  VisitJdkUnsafePutByte(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafePutAbsolute(HInvoke* invoke) {
  VisitJdkUnsafePutAbsolute(invoke);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kInt32, llvm::AtomicOrdering::Unordered, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutOrderedInt(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kInt32, llvm::AtomicOrdering::Release, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(
      invoke, DataType::Type::kInt32, llvm::AtomicOrdering::SequentiallyConsistent, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutRelease(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kInt32, llvm::AtomicOrdering::Release, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutReference(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kReference, llvm::AtomicOrdering::Unordered, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutOrderedObject(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kReference, llvm::AtomicOrdering::Release, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutReferenceVolatile(HInvoke* invoke) {
  GenUnsafePut(
      invoke, DataType::Type::kReference, llvm::AtomicOrdering::SequentiallyConsistent, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutReferenceRelease(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kReference, llvm::AtomicOrdering::Release, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kInt64, llvm::AtomicOrdering::Unordered, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kInt64, llvm::AtomicOrdering::Release, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(
      invoke, DataType::Type::kInt64, llvm::AtomicOrdering::SequentiallyConsistent, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutLongRelease(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kInt64, llvm::AtomicOrdering::Release, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutByte(HInvoke* invoke) {
  GenUnsafePut(invoke, DataType::Type::kInt8, llvm::AtomicOrdering::Unordered, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafePutAbsolute(HInvoke* invoke) {
  GenUnsafePutAbsolute(invoke, DataType::Type::kInt32, llvm::AtomicOrdering::Unordered, codegen_);
}

static void GenUnsafeCas(HInvoke* invoke, DataType::Type type, CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* base = codegen_->GetValue(invoke->InputAt(1));       // Object pointer.
  llvm::Value* offset = codegen_->GetValue(invoke->InputAt(2));     // Long offset.
  llvm::Value* expected = codegen_->GetValue(invoke->InputAt(3));   // Expected.
  llvm::Value* new_value = codegen_->GetValue(invoke->InputAt(4));  // New value.

  if (type == DataType::Type::kReference) {
    // Mark card for object assuming new value is stored.
    bool new_value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen_->MaybeMarkGCCard(base, new_value, new_value_can_be_null);
  }

  llvm::Value* exact_address = codegen_->CreateGEP(base, offset);
  llvm::MaybeAlign align;
  if (type == DataType::Type::kInt32 || type == DataType::Type::kReference) {
    align = llvm::MaybeAlign(alignof(int32_t));
  } else if (type == DataType::Type::kInt64) {
    align = llvm::MaybeAlign(alignof(int64_t));
  }

  // NOTE: The ARM64 codegen manually creates a loop here, which LLVM will also do for this
  // instruction, if the target CPU doesn't support CAS instructions.
  // NOTE: Compare and swap operations in Unsafe have volatile read and write semantics, which
  // corresponds to sequentially consistent atomic ordering.
  auto [loaded_value, success] =
      codegen_->CreateAtomicCmpXchg(exact_address,
                                    expected,
                                    new_value,
                                    align,
                                    llvm::AtomicOrdering::SequentiallyConsistent,
                                    llvm::AtomicOrdering::SequentiallyConsistent);
  codegen_->AddValue(invoke, success);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeCASInt(HInvoke* invoke) {
  VisitJdkUnsafeCASInt(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeCASLong(HInvoke* invoke) {
  VisitJdkUnsafeCASLong(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeCASObject(HInvoke* invoke) {
  VisitJdkUnsafeCASObject(invoke);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeCASInt(HInvoke* invoke) {
  // `jdk.internal.misc.Unsafe.compareAndSwapInt` has compare-and-set semantics (see javadoc).
  VisitJdkUnsafeCompareAndSetInt(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeCASLong(HInvoke* invoke) {
  // `jdk.internal.misc.Unsafe.compareAndSwapLong` has compare-and-set semantics (see javadoc).
  VisitJdkUnsafeCompareAndSetLong(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeCASObject(HInvoke* invoke) {
  // `jdk.internal.misc.Unsafe.compareAndSwapObject` has compare-and-set semantics (see javadoc).
  VisitJdkUnsafeCompareAndSetReference(invoke);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeCompareAndSetInt(HInvoke* invoke) {
  GenUnsafeCas(invoke, DataType::Type::kInt32, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeCompareAndSetLong(HInvoke* invoke) {
  GenUnsafeCas(invoke, DataType::Type::kInt64, codegen_);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeCompareAndSetReference(HInvoke* invoke) {
  // The only supported read barrier implementation is the Baker-style read barriers.
  DCHECK_IMPLIES(codegen_->EmitReadBarrier(), kUseBakerReadBarrier);

  GenUnsafeCas(invoke, DataType::Type::kReference, codegen_);
}

enum class GetAndUpdateOp {
  kSet,
  kAdd,
  kAddWithByteSwap,
  kAnd,
  kOr,
  kXor,
};

static llvm::Value* GenerateGetAndUpdate(CodeGeneratorARM64LLVM* codegen_,
                                         GetAndUpdateOp get_and_update_op,
                                         llvm::AtomicOrdering order,
                                         llvm::Value* ptr,
                                         llvm::Value* arg) {
  if (get_and_update_op == GetAndUpdateOp::kAddWithByteSwap) {
    DCHECK(order != llvm::AtomicOrdering::AcquireRelease);
    bool use_load_acquire = order == llvm::AtomicOrdering::Acquire ||
                            order == llvm::AtomicOrdering::SequentiallyConsistent;
    bool use_store_release = order == llvm::AtomicOrdering::Release ||
                             order == llvm::AtomicOrdering::SequentiallyConsistent;
    llvm::Intrinsic::ID load_intrinsic =
        use_load_acquire ? llvm::Intrinsic::aarch64_ldaxr : llvm::Intrinsic::aarch64_ldxr;
    llvm::Intrinsic::ID store_intrinsic =
        use_store_release ? llvm::Intrinsic::aarch64_stlxr : llvm::Intrinsic::aarch64_stxr;

    llvm::Type* arg_type = arg->getType();
    llvm::Type* arg_int_type = arg_type;
    bool is_fp = arg_type->isFloatingPointTy();
    if (is_fp) {
      arg_int_type = arg_int_type == codegen_->GetFloat32Type() ? codegen_->GetInt32Type()
                                                                : codegen_->GetInt64Type();
    }
    llvm::Attribute element_type_attr = llvm::Attribute::get(
        codegen_->GetLLVMContext(), llvm::Attribute::ElementType, arg_int_type);

    llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock();
    __ CreateBr(loop_block);
    __ SetInsertPoint(loop_block);

    llvm::CallInst* loaded_value_call = __ CreateIntrinsic(load_intrinsic, {ptr->getType()}, {ptr});
    loaded_value_call->addParamAttr(0, element_type_attr);
    llvm::Value* loaded_value = loaded_value_call;
    llvm::Type* intrinsic_value_type = loaded_value->getType();
    if (intrinsic_value_type != arg_int_type) {
      loaded_value = __ CreateTrunc(loaded_value, arg_int_type);
    }
    loaded_value = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, loaded_value);
    if (is_fp) {
      loaded_value = __ CreateBitCast(loaded_value, arg_type);
    }

    llvm::Value* stored_value =
        is_fp ? __ CreateFAdd(loaded_value, arg) : __ CreateAdd(loaded_value, arg);

    if (is_fp) {
      stored_value = __ CreateBitCast(stored_value, arg_int_type);
    }
    stored_value = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, stored_value);
    if (stored_value->getType() != intrinsic_value_type) {
      stored_value = __ CreateZExt(stored_value, intrinsic_value_type);
    }
    llvm::CallInst* store_call =
        __ CreateIntrinsic(store_intrinsic, {ptr->getType()}, {stored_value, ptr});
    store_call->addParamAttr(1, element_type_attr);
    llvm::Value* failed =
        __ CreateICmpNE(store_call, codegen_->GetConstantZero(store_call->getType()));
    llvm::BasicBlock* exit_block = codegen_->CreateBasicBlock();
    __ CreateCondBr(failed, loop_block, exit_block);
    __ SetInsertPoint(exit_block);

    return loaded_value;
  }

  llvm::MaybeAlign align = codegen_->GetModule()->getDataLayout().getABITypeAlign(arg->getType());
  llvm::AtomicRMWInst::BinOp op;
  switch (get_and_update_op) {
    case GetAndUpdateOp::kSet:
      op = llvm::AtomicRMWInst::Xchg;
      break;
    case GetAndUpdateOp::kAdd:
      if (arg->getType()->isFloatingPointTy()) {
        op = llvm::AtomicRMWInst::FAdd;
      } else {
        op = llvm::AtomicRMWInst::Add;
      }
      break;
    case GetAndUpdateOp::kAddWithByteSwap:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
    case GetAndUpdateOp::kAnd:
      op = llvm::AtomicRMWInst::And;
      break;
    case GetAndUpdateOp::kOr:
      op = llvm::AtomicRMWInst::Or;
      break;
    case GetAndUpdateOp::kXor:
      op = llvm::AtomicRMWInst::Xor;
      break;
  }
  bool is_boolean = arg->getType() == codegen_->GetBooleanType();
  if (is_boolean) {
    arg = __ CreateZExt(arg, codegen_->GetUint8Type());
  }
  llvm::Value* result = codegen_->CreateAtomicRMW(op, ptr, arg, align, order);
  if (is_boolean) {
    result = __ CreateTrunc(result, codegen_->GetBooleanType());
  }
  return result;
}

static void GenUnsafeGetAndUpdate(HInvoke* invoke,
                                  DataType::Type type,
                                  CodeGeneratorARM64LLVM* codegen_,
                                  GetAndUpdateOp get_and_update_op) {
  llvm::Value* base = codegen_->GetValue(invoke->InputAt(1));    // Object pointer.
  llvm::Value* offset = codegen_->GetValue(invoke->InputAt(2));  // Long offset.
  llvm::Value* arg = codegen_->GetValue(invoke->InputAt(3));     // New value or addend.

  if (type == DataType::Type::kReference) {
    DCHECK(get_and_update_op == GetAndUpdateOp::kSet);
    // Mark card for object as a new value shall be stored.
    bool new_value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen_->MaybeMarkGCCard(base, /*value=*/arg, new_value_can_be_null);
  }

  DCHECK(get_and_update_op == GetAndUpdateOp::kSet || get_and_update_op == GetAndUpdateOp::kAdd);

  llvm::AtomicRMWInst::BinOp operation = (get_and_update_op == GetAndUpdateOp::kSet)
                                             ? llvm::AtomicRMWInst::BinOp::Xchg
                                             : llvm::AtomicRMWInst::BinOp::Add;

  llvm::Value* exact_address = codegen_->CreateGEP(base, offset);
  llvm::MaybeAlign align;
  if (type == DataType::Type::kInt32) {
    align = llvm::MaybeAlign(alignof(int32_t));
  } else if (type == DataType::Type::kInt64) {
    align = llvm::MaybeAlign(alignof(int64_t));
  }

  bool is_boolean = arg->getType() == codegen_->GetBooleanType();
  if (is_boolean) {
    arg = __ CreateZExt(arg, codegen_->GetUint8Type());
  }
  llvm::Value* result = codegen_->CreateAtomicRMW(
      operation,
      exact_address,
      arg,
      align,
      llvm::AtomicOrdering::AcquireRelease);  // TODO revisit memory ordering
  if (is_boolean) {
    result = __ CreateTrunc(result, codegen_->GetBooleanType());
  }
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetAndAddInt(HInvoke* invoke) {
  VisitJdkUnsafeGetAndAddInt(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetAndAddLong(HInvoke* invoke) {
  VisitJdkUnsafeGetAndAddLong(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetAndSetInt(HInvoke* invoke) {
  VisitJdkUnsafeGetAndSetInt(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetAndSetLong(HInvoke* invoke) {
  VisitJdkUnsafeGetAndSetLong(invoke);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitUnsafeGetAndSetObject(HInvoke* invoke) {
  VisitJdkUnsafeGetAndSetReference(invoke);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetAndAddInt(HInvoke* invoke) {
  GenUnsafeGetAndUpdate(invoke, DataType::Type::kInt32, codegen_, GetAndUpdateOp::kAdd);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetAndAddLong(HInvoke* invoke) {
  GenUnsafeGetAndUpdate(invoke, DataType::Type::kInt64, codegen_, GetAndUpdateOp::kAdd);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetAndSetInt(HInvoke* invoke) {
  GenUnsafeGetAndUpdate(invoke, DataType::Type::kInt32, codegen_, GetAndUpdateOp::kSet);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetAndSetLong(HInvoke* invoke) {
  GenUnsafeGetAndUpdate(invoke, DataType::Type::kInt64, codegen_, GetAndUpdateOp::kSet);
}
void IntrinsicCodeGeneratorARM64LLVM::VisitJdkUnsafeGetAndSetReference(HInvoke* invoke) {
  GenUnsafeGetAndUpdate(invoke, DataType::Type::kReference, codegen_, GetAndUpdateOp::kSet);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitStringCompareTo(HInvoke* invoke) {
  llvm::Value* str = GetValue(invoke->InputAt(0));
  llvm::Value* arg = GetValue(invoke->InputAt(1));

  llvm::Type* result_type = GetLLVMType(invoke->GetType());

  const bool can_slow_path = invoke->InputAt(1)->CanBeNull();

  llvm::BasicBlock* current_block = __ GetInsertBlock();
  llvm::BasicBlock* end_block = codegen_->CreateBasicBlock("end_block");
  __ SetInsertPoint(end_block);
  const int phi_incoming_count =
      3 + (can_slow_path ? 1 : 0) + (mirror::kUseStringCompression ? 2 : 0);
  llvm::PHINode* result_phi = __ CreatePHI(result_type, phi_incoming_count);
  __ SetInsertPoint(current_block);

  // Get offsets of count and value fields within a string object.
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // Take slow path and throw if input can be and is null.
  SlowPathCodeARM64LLVM* slow_path = nullptr;
  if (can_slow_path) {
    llvm::BasicBlock* entry_block = codegen_->CreateBasicBlock("entry_block");

    slow_path = new (codegen_->GetScopedAllocator())
        IntrinsicSlowPathARM64LLVM(invoke, entry_block, end_block, result_phi);
    codegen_->AddSlowPath(slow_path);
    llvm::Value* is_zero = __ CreateICmpEQ(arg, GetConstantZero(arg->getType()));
    llvm::BranchInst* br = codegen_->CreateBranchIfTrue(is_zero, slow_path->GetEntryBlock());
    ExpectFalseBranch(br);
  }

  // Reference equality check, return 0 if same reference.
  // __ Subs(out, str, arg);
  // __ B(&end, eq);
  llvm::Value* are_equal_reference = __ CreateICmpEQ(str, arg);
  {
    llvm::BasicBlock* continue_block = codegen_->CreateBasicBlock();
    __ CreateCondBr(are_equal_reference, end_block, continue_block);
    result_phi->addIncoming(GetConstantZero(result_phi->getType()), __ GetInsertBlock());
    __ SetInsertPoint(continue_block);
  }

  llvm::Value* str_length = nullptr;
  llvm::Value* arg_length = nullptr;
  llvm::Value* str_uncompressed = nullptr;
  llvm::Value* arg_uncompressed = nullptr;

  if (mirror::kUseStringCompression) {
    // Load `count` fields of this and argument strings.
    // __ Ldr(temp3, HeapOperand(str, count_offset));
    llvm::Value* str_count = codegen_->CreateLoadWithOffset(GetInt32Type(), str, count_offset);
    // __ Ldr(temp2, HeapOperand(arg, count_offset));
    llvm::Value* arg_count = codegen_->CreateLoadWithOffset(GetInt32Type(), arg, count_offset);
    // Clean out compression flag from lengths.
    str_uncompressed = __ CreateAnd(str_count, 1);
    str_uncompressed = __ CreateTrunc(str_uncompressed, GetBooleanType());
    arg_uncompressed = __ CreateAnd(arg_count, 1);
    arg_uncompressed = __ CreateTrunc(arg_uncompressed, GetBooleanType());
    // __ Lsr(temp0, temp3, 1u);
    str_length = __ CreateLShr(str_count, 1);
    // __ Lsr(temp1, temp2, 1u);
    arg_length = __ CreateLShr(arg_count, 1);
  } else {
    // Load lengths of this and argument strings.
    //__ Ldr(temp0, HeapOperand(str, count_offset));
    str_length = codegen_->CreateLoadWithOffset(GetInt32Type(), str, count_offset);
    //__ Ldr(temp1, HeapOperand(arg, count_offset));
    arg_length = codegen_->CreateLoadWithOffset(GetInt32Type(), arg, count_offset);
  }

  // out = length diff.
  // __ Subs(out, temp0, temp1);
  // NOTE: This value should be returned if there's no valid index `k`, where
  // `str.charAt(k) != arg.charAt(k)`, i.e. if one of the strings is the prefix of the other one.
  llvm::Value* length_diff = __ CreateSub(str_length, arg_length);

  // temp0 = min(len(str), len(arg)).
  // __ Csel(temp0, temp1, temp0, ge);
  llvm::Value* min_length =
      __ CreateIntrinsic(llvm::Intrinsic::umin, {str_length->getType()}, {str_length, arg_length});

  {
    // Shorter string is empty?
    //__ Cbz(temp0, &end);
    llvm::Value* min_length_is_zero =
        __ CreateICmpEQ(min_length, GetConstantZero(min_length->getType()));
    llvm::BasicBlock* continue_block = codegen_->CreateBasicBlock();
    llvm::BranchInst* br = __ CreateCondBr(min_length_is_zero, end_block, continue_block);
    ExpectFalseBranch(br);

    result_phi->addIncoming(length_diff, __ GetInsertBlock());
    __ SetInsertPoint(continue_block);
  }

  llvm::Value* bytes_to_compare = min_length;
  llvm::BasicBlock* different_compression_block = nullptr;
  if (mirror::kUseStringCompression) {
    // Check if both strings are using the same compression style to use this comparison loop.
    // __ Eor(temp2, temp2, Operand(temp3));
    llvm::Value* same_compression_style = __ CreateICmpEQ(str_uncompressed, arg_uncompressed);

    // __ Tbnz(temp2, 0, &different_compression);
    different_compression_block = codegen_->CreateBasicBlock("different_compression_block");
    llvm::BranchInst* br =
        codegen_->CreateBranchIfFalse(same_compression_style, different_compression_block);
    ExpectTrueBranch(br);

    // For string compression, calculate the number of bytes to compare (not chars).
    // This could in theory exceed INT32_MAX, so treat temp0 as unsigned.
    // __ Lsl(temp0, temp0, temp3);
    llvm::Value* byte_count_shift = __ CreateZExt(str_uncompressed, min_length->getType());
    bytes_to_compare = __ CreateShl(min_length, byte_count_shift);
  }

  // Assertions that must hold in order to compare strings 8 bytes at a time.
  DCHECK_ALIGNED(value_offset, 8);
  static_assert(IsAligned<8>(kObjectAlignment), "String of odd length is not zero padded");

  const size_t char_size = DataType::Size(DataType::Type::kUint16);
  DCHECK_EQ(char_size, 2u);
  llvm::Type* compare_type = GetUint64Type();

  llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock("loop_block");
  llvm::BasicBlock* find_char_diff_block = codegen_->CreateBasicBlock("find_char_diff_block");
  __ CreateBr(loop_block);

  llvm::BasicBlock* loops_parent_block = __ GetInsertBlock();

  __ SetInsertPoint(loop_block);
  llvm::PHINode* loop_offset_phi = __ CreatePHI(GetInt64Type(), 2);
  loop_offset_phi->addIncoming(GetConstantInt(loop_offset_phi->getType(), value_offset),
                               loops_parent_block);
  llvm::PHINode* remaining_bytes_phi = __ CreatePHI(GetInt32Type(), 2);
  remaining_bytes_phi->addIncoming(bytes_to_compare, loops_parent_block);

  // __ Ldr(temp4, MemOperand(str.X(), temp1.X()));
  llvm::Value* str_address = __ CreatePtrAdd(str, loop_offset_phi);
  llvm::Value* str_value_loop = CreateLoad(compare_type, str_address);
  // __ Ldr(temp2, MemOperand(arg.X(), temp1.X()));
  llvm::Value* arg_address = __ CreatePtrAdd(arg, loop_offset_phi);
  llvm::Value* arg_value_loop = CreateLoad(compare_type, arg_address);
  // __ Cmp(temp4, temp2);
  llvm::Value* values_are_not_equal = __ CreateICmpNE(str_value_loop, arg_value_loop);

  // __ B(ne, &find_char_diff);
  codegen_->CreateBranchIfTrue(values_are_not_equal, find_char_diff_block);

  // __ Add(temp1, temp1, char_size * 4);
  llvm::Value* next_offset =
      __ CreateAdd(loop_offset_phi, GetConstantInt(loop_offset_phi->getType(), char_size * 4));

  // With string compression, we have compared 8 bytes, otherwise 4 chars.
  // __ Subs(temp0, temp0, (mirror::kUseStringCompression) ? 8 : 4);
  llvm::Value* compared_bytes =
      GetConstantInt(remaining_bytes_phi->getType(), mirror::kUseStringCompression ? 8 : 4);
  llvm::Value* next_remaining_bytes = __ CreateSub(remaining_bytes_phi,
                                                   compared_bytes,
                                                   "",
                                                   /* HasNUW= */ true,
                                                   /* HasNSW= */ true);
  // __ B(&loop, hi);
  llvm::Value* continue_loop =
      __ CreateICmpSGT(next_remaining_bytes, GetConstantZero(next_remaining_bytes->getType()));
  __ CreateCondBr(continue_loop, loop_block, end_block);
  loop_offset_phi->addIncoming(next_offset, __ GetInsertBlock());
  remaining_bytes_phi->addIncoming(next_remaining_bytes, __ GetInsertBlock());
  result_phi->addIncoming(length_diff, __ GetInsertBlock());

  // Find the single character difference.
  __ SetInsertPoint(find_char_diff_block);
  // Get the bit position of the first character that differs.
  // __ Eor(temp1, temp2, temp4);
  llvm::Value* value_diff_xor = __ CreateXor(arg_value_loop, str_value_loop);
  // __ Rbit(temp1, temp1);
  // __ Clz(temp1, temp1);
  llvm::Value* is_poison = __ getFalse();
  llvm::Value* diff_bit_position = __ CreateIntrinsic(
      llvm::Intrinsic::cttz, value_diff_xor->getType(), {value_diff_xor, is_poison});

  // If the number of chars remaining <= the index where the difference occurs (0-3), then
  // the difference occurs outside the remaining string data, so just return length diff (out).
  // Unlike ARM, we're doing the comparison in one go here, without the subtraction at the
  // find_char_diff_2nd_cmp path, so it doesn't matter whether the comparison is signed or
  // unsigned when string compression is disabled.
  // When it's enabled, the comparison must be unsigned.
  // __ Cmp(temp0, Operand(temp1.W(), LSR, (mirror::kUseStringCompression) ? 3 : 4));
  // NOTE: Division by either 8 or 16.
  llvm::Value* diff_index = __ CreateZExtOrTrunc(diff_bit_position, remaining_bytes_phi->getType());
  diff_index = __ CreateLShr(diff_index, mirror::kUseStringCompression ? 3 : 4);
  llvm::Value* is_diff_outside = __ CreateICmpSLE(remaining_bytes_phi, diff_index);
  {
    // __ B(ls, &end);
    llvm::BasicBlock* continue_block = codegen_->CreateBasicBlock();
    __ CreateCondBr(is_diff_outside, end_block, continue_block);
    result_phi->addIncoming(length_diff, __ GetInsertBlock());
    __ SetInsertPoint(continue_block);
  }

  // Extract the characters and calculate the difference.
  llvm::Value* shift = nullptr;
  if (mirror::kUseStringCompression) {
    // __ Bic(temp1, temp1, 0x7);
    // __ Bic(temp1, temp1, Operand(temp3.X(), LSL, 3u));
    llvm::Value* compressed_mask = GetConstantInt(diff_bit_position->getType(), 0x7);
    compressed_mask = __ CreateNot(compressed_mask);
    llvm::Value* uncompressed_mask = GetConstantInt(diff_bit_position->getType(), 0xf);
    uncompressed_mask = __ CreateNot(uncompressed_mask);
    llvm::Value* mask = __ CreateSelect(str_uncompressed, uncompressed_mask, compressed_mask);
    shift = __ CreateAnd(diff_bit_position, mask);
  } else {
    // __ Bic(temp1, temp1, 0xf);
    llvm::Value* mask = GetConstantInt(diff_bit_position->getType(), 0xf);
    mask = __ CreateNot(mask);
    shift = __ CreateAnd(diff_bit_position, mask);
  }
  DCHECK(shift->getType() == str_value_loop->getType());

  // __ Lsr(temp2, temp2, temp1);
  llvm::Value* str_diff_char = __ CreateLShr(str_value_loop, shift);
  // __ Lsr(temp4, temp4, temp1);
  llvm::Value* arg_diff_char = __ CreateLShr(arg_value_loop, shift);

  if (mirror::kUseStringCompression) {
    // Prioritize the case of compressed strings and calculate such result first.
    // __ Uxtb(temp1, temp4);
    // __ Sub(out, temp1.W(), Operand(temp2.W(), UXTB));
    llvm::Value* str_diff_byte_value = __ CreateTrunc(str_diff_char, GetUint8Type());
    str_diff_byte_value = __ CreateZExt(str_diff_byte_value, result_phi->getType());
    llvm::Value* arg_diff_byte_value = __ CreateTrunc(arg_diff_char, GetUint8Type());
    arg_diff_byte_value = __ CreateZExt(arg_diff_byte_value, result_phi->getType());
    llvm::Value* compressed_result = __ CreateSub(str_diff_byte_value, arg_diff_byte_value);

    // __ Tbz(temp3, 0u, &end);  // If actually compressed, we're done.
    llvm::BasicBlock* continue_block = codegen_->CreateBasicBlock();
    __ CreateCondBr(str_uncompressed, continue_block, end_block);
    result_phi->addIncoming(compressed_result, __ GetInsertBlock());
    __ SetInsertPoint(continue_block);
  }

  // __ Uxth(temp4, temp4);
  // __ Sub(out, temp4.W(), Operand(temp2.W(), UXTH));
  str_diff_char = __ CreateTrunc(str_diff_char, GetUint16Type());
  str_diff_char = __ CreateZExt(str_diff_char, result_phi->getType());
  arg_diff_char = __ CreateTrunc(arg_diff_char, GetUint16Type());
  arg_diff_char = __ CreateZExt(arg_diff_char, result_phi->getType());
  llvm::Value* uncompressed_result = __ CreateSub(str_diff_char, arg_diff_char);

  __ CreateBr(end_block);
  result_phi->addIncoming(uncompressed_result, __ GetInsertBlock());

  if (mirror::kUseStringCompression) {
    // __ Bind(&different_compression);
    __ SetInsertPoint(different_compression_block);

    // Comparison for different compression style.
    const size_t c_char_size = DataType::Size(DataType::Type::kInt8);
    DCHECK_EQ(c_char_size, 1u);

    // `temp1` will hold the compressed data pointer, `temp2` the uncompressed data pointer.
    // Note that flags have been set by the `str` compression flag extraction to `temp3`
    // before branching to the `different_compression` label.
    // __ Csel(temp1, str, arg, eq);   // Pointer to the compressed string.
    llvm::Value* compressed_str = __ CreateSelect(str_uncompressed, arg, str);
    // __ Csel(temp2, str, arg, ne);   // Pointer to the uncompressed string.
    llvm::Value* uncompressed_str = __ CreateSelect(str_uncompressed, str, arg);

    // Adjust temp1 and temp2 from string pointers to data pointers.
    // __ Add(temp1, temp1, Operand(value_offset));
    llvm::Value* compressed_ptr_start = CreateGEP(compressed_str, value_offset);
    // __ Add(temp2, temp2, Operand(value_offset));
    llvm::Value* uncompressed_ptr_start = CreateGEP(uncompressed_str, value_offset);

    llvm::BasicBlock* different_compression_loop_block =
        codegen_->CreateBasicBlock("different_compression_loop");

    __ CreateBr(different_compression_loop_block);
    llvm::BasicBlock* before_loop_block = __ GetInsertBlock();

    __ SetInsertPoint(different_compression_loop_block);
    llvm::PHINode* compressed_ptr_phi = __ CreatePHI(compressed_ptr_start->getType(), 2);
    compressed_ptr_phi->addIncoming(compressed_ptr_start, before_loop_block);
    llvm::PHINode* uncompressed_ptr_phi = __ CreatePHI(uncompressed_ptr_start->getType(), 2);
    uncompressed_ptr_phi->addIncoming(uncompressed_ptr_start, before_loop_block);
    llvm::PHINode* remaining_length_phi = __ CreatePHI(min_length->getType(), 2);
    remaining_length_phi->addIncoming(min_length, before_loop_block);

    // __ Ldrb(temp4, MemOperand(temp1.X(), c_char_size, PostIndex));
    llvm::Value* next_compressed_ptr = CreateGEP(compressed_ptr_phi, c_char_size);
    llvm::Value* compressed_char = CreateLoad(GetUint8Type(), compressed_ptr_phi);
    compressed_char = __ CreateZExt(compressed_char, result_phi->getType());
    // __ Ldrh(temp3, MemOperand(temp2.X(), char_size, PostIndex));
    llvm::Value* next_uncompressed_ptr = CreateGEP(uncompressed_ptr_phi, char_size);
    llvm::Value* uncompressed_char = CreateLoad(GetUint16Type(), uncompressed_ptr_phi);
    uncompressed_char = __ CreateZExt(uncompressed_char, result_phi->getType());

    // __ Subs(temp4, temp4, Operand(temp3));
    llvm::Value* char_diff = __ CreateSub(compressed_char, uncompressed_char, "");
    llvm::Value* is_different = __ CreateICmpNE(char_diff, GetConstantZero(char_diff->getType()));

    //  __ B(&different_compression_diff, ne);
    llvm::BasicBlock* different_compression_diff_block =
        codegen_->CreateBasicBlock("different_compression_diff");
    codegen_->CreateBranchIfTrue(is_different, different_compression_diff_block);

    // __ Subs(temp0, temp0, 2);
    llvm::Value* next_remaining_length =
        __ CreateSub(remaining_length_phi,
                     GetConstantInt(remaining_length_phi->getType(), 1),
                     "",
                     /*HasNUW= */ true,
                     /*HaNSW= */ true);
    llvm::Value* has_any_remaining_chars =
        __ CreateICmpNE(next_remaining_length, GetConstantZero(next_remaining_length->getType()));
    // __ B(&different_compression_loop, hi);
    // __ B(&end);
    __ CreateCondBr(has_any_remaining_chars, different_compression_loop_block, end_block);
    result_phi->addIncoming(length_diff, __ GetInsertBlock());
    compressed_ptr_phi->addIncoming(next_compressed_ptr, __ GetInsertBlock());
    uncompressed_ptr_phi->addIncoming(next_uncompressed_ptr, __ GetInsertBlock());
    remaining_length_phi->addIncoming(next_remaining_length, __ GetInsertBlock());

    // Calculate the difference.
    // __ Bind(&different_compression_diff);
    __ SetInsertPoint(different_compression_diff_block);
    static_assert(static_cast<uint32_t>(mirror::StringCompressionFlag::kCompressed) == 0u,
                  "Expecting 0=compressed, 1=uncompressed");
    // __ Tst(temp0, Operand(1));
    // __ Cneg(out, temp4, ne);
    llvm::Value* negated_char_diff = __ CreateNeg(char_diff);
    llvm::Value* result_char_diff = __ CreateSelect(str_uncompressed, negated_char_diff, char_diff);
    __ CreateBr(end_block);
    result_phi->addIncoming(result_char_diff, __ GetInsertBlock());
  }

  __ SetInsertPoint(end_block);
  AddValue(invoke, result_phi);
}

static const char* GetConstString(HInstruction* candidate, uint32_t* utf16_length) {
  if (candidate->IsLoadString()) {
    HLoadString* load_string = candidate->AsLoadString();
    const DexFile& dex_file = load_string->GetDexFile();
    return dex_file.GetStringDataAndUtf16Length(load_string->GetStringIndex(), utf16_length);
  }
  return nullptr;
}

void IntrinsicCodeGeneratorARM64LLVM::VisitStringEquals(HInvoke* invoke) {
  llvm::Value* str = GetValue(invoke->InputAt(0));
  llvm::Value* arg = GetValue(invoke->InputAt(1));

  llvm::BasicBlock* return_true_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* return_false_block = codegen_->CreateBasicBlock();

  // Get offsets of count, value, and class fields within a string object.
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  const int32_t class_offset = mirror::Object::ClassOffset().Int32Value();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  StringEqualsOptimizations optimizations(invoke);
  if (!optimizations.GetArgumentNotNull()) {
    // Check if input is null, return false if it is.
    // __ Cbz(arg, &return_false);
    llvm::Value* is_null = __ CreateICmpEQ(arg, GetConstantZero(arg->getType()));
    codegen_->CreateBranchIfTrue(is_null, return_false_block);
  }

  // Reference equality check, return true if same reference.
  // __ Cmp(str, arg);
  llvm::Value* is_same_reference = __ CreateICmpEQ(str, arg);
  // __ B(&return_true, eq);
  codegen_->CreateBranchIfTrue(is_same_reference, return_true_block);

  if (!optimizations.GetArgumentIsString()) {
    // Instanceof check for the argument by comparing class fields.
    // All string objects must have the same type since String cannot be subclassed.
    // Receiver must be a string object, so its class field is equal to all strings' class fields.
    // If the argument is a string object, its class field must be equal to receiver's class field.
    //
    // As the String class is expected to be non-movable, we can read the class
    // field from String.equals' arguments without read barriers.
    AssertNonMovableStringClass();
    // /* HeapReference<Class> */ temp = str->klass_
    // __ Ldr(temp, MemOperand(str.X(), class_offset));
    llvm::Value* str_class =
        CreateLoadWithOffset(GetUncompressedGCPointerType(), str, class_offset);
    // /* HeapReference<Class> */ temp1 = arg->klass_
    // __ Ldr(temp1, MemOperand(arg.X(), class_offset));
    llvm::Value* arg_class =
        CreateLoadWithOffset(GetUncompressedGCPointerType(), arg, class_offset);
    // Also, because we use the previously loaded class references only in the
    // following comparison, we don't need to unpoison them.
    // __ Cmp(temp, temp1);
    llvm::Value* is_arg_string = __ CreateICmpEQ(str_class, arg_class);
    // __ B(&return_false_block, ne);
    codegen_->CreateBranchIfFalse(is_arg_string, return_false_block);
  }

  // Check if one of the inputs is a const string. Do not special-case both strings
  // being const, such cases should be handled by constant folding if needed.
  uint32_t const_string_length = 0u;
  const char* const_string = GetConstString(invoke->InputAt(0), &const_string_length);
  if (const_string == nullptr) {
    const_string = GetConstString(invoke->InputAt(1), &const_string_length);
    if (const_string != nullptr) {
      std::swap(str, arg);  // Make sure the const string is in `str`.
    }
  }
  bool is_compressed = mirror::kUseStringCompression && const_string != nullptr &&
                       mirror::String::DexFileStringAllASCII(const_string, const_string_length);

  llvm::Value* string_count = nullptr;
  if (const_string != nullptr) {
    // Load `count` field of the argument string and check if it matches the const string.
    // Also compares the compression style, if differs return false.
    // __ Ldr(temp, MemOperand(arg.X(), count_offset));
    llvm::Value* arg_count = CreateLoadWithOffset(GetInt32Type(), arg, count_offset);
    // __ Cmp(temp, Operand(mirror::String::GetFlaggedCount(const_string_length, is_compressed)));
    int32_t flagged_count = mirror::String::GetFlaggedCount(const_string_length, is_compressed);
    llvm::Value* is_equal_count =
        __ CreateICmpEQ(arg_count, GetConstantInt(GetInt32Type(), flagged_count));
    // __ B(&return_false_block, ne);
    codegen_->CreateBranchIfFalse(is_equal_count, return_false_block);
    string_count = arg_count;
  } else {
    // Load `count` fields of this and argument strings.
    // __ Ldr(temp, MemOperand(str.X(), count_offset));
    llvm::Value* str_count = CreateLoadWithOffset(GetInt32Type(), str, count_offset);
    // __ Ldr(temp1, MemOperand(arg.X(), count_offset));
    llvm::Value* arg_count = CreateLoadWithOffset(GetInt32Type(), arg, count_offset);
    // Check if `count` fields are equal, return false if they're not.
    // Also compares the compression style, if differs return false.
    // __ Cmp(temp, temp1);
    llvm::Value* is_equal_count = __ CreateICmpEQ(str_count, arg_count);
    // __ B(&return_false_block, ne);
    codegen_->CreateBranchIfFalse(is_equal_count, return_false_block);
    string_count = str_count;
  }

  // Assertions that must hold in order to compare strings 8 bytes at a time.
  // Ok to do this because strings are zero-padded to kObjectAlignment.
  DCHECK_ALIGNED(value_offset, 8);
  static_assert(IsAligned<8>(kObjectAlignment), "String of odd length is not zero padded");

  // NOTE: ART unrolls the loop if the constant string is smaller than a certain size. We delegate
  // this decision to LLVM.

  // Return true if both strings are empty. Even with string compression `count == 0` means empty.
  static_assert(static_cast<uint32_t>(mirror::StringCompressionFlag::kCompressed) == 0u,
                "Expecting 0=compressed, 1=uncompressed");
  llvm::Value* is_zero_count =
      __ CreateICmpEQ(string_count, GetConstantZero(string_count->getType()));
  // __ Cbz(temp, &return_true);
  codegen_->CreateBranchIfTrue(is_zero_count, return_true_block);

  llvm::Value* number_of_bytes_to_compare = string_count;
  if (mirror::kUseStringCompression) {
    // For string compression, calculate the number of bytes to compare (not chars).
    // This could in theory exceed INT32_MAX, so treat temp as unsigned.
    // __ And(temp1, temp, Operand(1));  // Extract compression flag.
    llvm::Value* compression_flag = __ CreateAnd(string_count, 1);
    // __ Lsr(temp, temp, 1u);           // Extract length.
    llvm::Value* length = __ CreateLShr(string_count, 1);
    // __ Lsl(temp, temp, temp1);        // Calculate number of bytes to compare.
    number_of_bytes_to_compare = __ CreateShl(length, compression_flag);
  }

  llvm::BasicBlock* end_block = codegen_->CreateBasicBlock();

  llvm::BasicBlock* placeholder_function_block = __ GetInsertBlock();
  llvm::Function* placeholder_function = codegen_->GetStringEqualsPlaceholderFunction();
  llvm::Value* lhs_start_addr = CreateGEP(str, value_offset);
  llvm::Value* rhs_start_addr = CreateGEP(arg, value_offset);
  llvm::Value* result = __ CreateCall(placeholder_function,
                                      {lhs_start_addr, rhs_start_addr, number_of_bytes_to_compare});
  __ CreateBr(end_block);

  // Return true and exit the function.
  // If loop does not result in returning false, we return true.
  // __ Bind(&return_true);
  __ SetInsertPoint(return_true_block);
  // __ Mov(out, 1);
  // __ B(&end);
  __ CreateBr(end_block);

  // Return false and exit the function.
  // __ Bind(&return_false);
  __ SetInsertPoint(return_false_block);
  // __ Mov(out, 0);
  __ CreateBr(end_block);

  __ SetInsertPoint(end_block);
  llvm::Type* boolean_type = GetBooleanType();
  llvm::PHINode* result_phi = __ CreatePHI(boolean_type, 3);
  result_phi->addIncoming(llvm::ConstantInt::getTrue(boolean_type), return_true_block);
  result_phi->addIncoming(llvm::ConstantInt::getFalse(boolean_type), return_false_block);
  result_phi->addIncoming(result, placeholder_function_block);

  // __ Bind(&end);
  AddValue(invoke, result_phi);
}

static llvm::MDTuple* GetVectorizedLoopID(llvm::LLVMContext& context) {
  // NOTE: LLVM puts these two attributes on vectorized loops:
  //   !{!"llvm.loop.isvectorized", i32 1}
  //   !{!"llvm.loop.unroll.runtime.disable"}

  llvm::Constant* one_value = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 1);
  // !{!"llvm.loop.isvectorized", i32 1}
  llvm::MDTuple* is_vectorized_md =
      llvm::MDTuple::get(context,
                         {llvm::MDString::get(context, "llvm.loop.isvectorized"),
                          llvm::ConstantAsMetadata::get(one_value)});

  // !{!"llvm.loop.unroll.runtime.disable"}
  llvm::MDTuple* disable_runtime_unrolling_md =
      llvm::MDTuple::get(context, llvm::MDString::get(context, "llvm.loop.unroll.runtime.disable"));

  // We need to disable LSR for these loops, otherwise we would get different and sub-optimal code
  // generation.
  // !{!"llvm.loop.lsr.disable"}
  llvm::MDTuple* disable_lsr_md =
      llvm::MDTuple::get(context, llvm::MDString::get(context, "llvm.loop.lsr.disable"));

  llvm::MDTuple* loop_id = llvm::MDTuple::get(
      context, {nullptr, is_vectorized_md, disable_runtime_unrolling_md, disable_lsr_md});
  loop_id->replaceOperandWith(0, loop_id);
  return loop_id;
}

static void GenerateStringEqualsLoop(llvm::Value* lhs,
                                     llvm::Value* rhs,
                                     llvm::Value* length,
                                     llvm::BasicBlock* done_block,
                                     llvm::PHINode* result_phi,
                                     CodeGeneratorARM64LLVM* codegen_) {
  constexpr int step_size = mirror::kUseStringCompression ? 8 : 4;
  llvm::Type* i64_type = codegen_->GetUint64Type();

  llvm::BasicBlock* loop_header_block = __ GetInsertBlock();
  llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock();
  __ CreateBr(loop_block);
  __ SetInsertPoint(loop_block);
  llvm::PHINode* iv = __ CreatePHI(length->getType(), 2);
  llvm::PHINode* lhs_iv = __ CreatePHI(lhs->getType(), 2);
  llvm::PHINode* rhs_iv = __ CreatePHI(rhs->getType(), 2);
  iv->addIncoming(length, loop_header_block);
  lhs_iv->addIncoming(lhs, loop_header_block);
  rhs_iv->addIncoming(rhs, loop_header_block);

  llvm::Value* lhs_load = codegen_->CreateLoad(i64_type, lhs_iv);
  llvm::Value* rhs_load = codegen_->CreateLoad(i64_type, rhs_iv);

  llvm::Value* lhs_iv_next = __ CreateConstGEP1_64(i64_type, lhs_iv, 1);
  llvm::Value* rhs_iv_next = __ CreateConstGEP1_64(i64_type, rhs_iv, 1);

  llvm::Value* loaded_chars_equal = __ CreateICmpEQ(lhs_load, rhs_load);
  llvm::BasicBlock* loop_continue_block = codegen_->CreateBasicBlock();
  __ CreateCondBr(loaded_chars_equal, loop_continue_block, done_block);
  result_phi->addIncoming(__ getFalse(), __ GetInsertBlock());

  __ SetInsertPoint(loop_continue_block);
  llvm::Value* iv_next = __ CreateNSWAdd(iv, codegen_->GetConstantInt(iv->getType(), -step_size));
  llvm::Value* continue_loop =
      __ CreateICmpSGT(iv_next, codegen_->GetConstantZero(iv_next->getType()));
  llvm::Instruction* loop_br = __ CreateCondBr(continue_loop, loop_block, done_block);
  result_phi->addIncoming(__ getTrue(), __ GetInsertBlock());
  iv->addIncoming(iv_next, __ GetInsertBlock());
  lhs_iv->addIncoming(lhs_iv_next, __ GetInsertBlock());
  rhs_iv->addIncoming(rhs_iv_next, __ GetInsertBlock());

  llvm::MDNode* loop_id = GetVectorizedLoopID(codegen_->GetLLVMContext());
  loop_br->setMetadata(llvm::LLVMContext::MD_loop, loop_id);
}

// TODO: Can it be worth it to rewrite this loop to be vectorized? We're already comparing 8 bytes
// at a time, so there may not be too much room for improvement.
llvm::BasicBlock::iterator RewriteStringEquals(llvm::CallInst* call,
                                               CodeGeneratorARM64LLVM* codegen_) {
  DCHECK(call->getCalledFunction()->hasMetadata(kStringEqualsMetadata));

  llvm::BasicBlock* old_block = call->getParent();
  llvm::BasicBlock* new_block = llvm::splitBlockBefore(old_block, call, nullptr, nullptr, nullptr);
  DCHECK(llvm::isa<llvm::BranchInst>(new_block->getTerminator()))
      << "Expected an unconditional branch at the end of the new block.";
  DCHECK(llvm::cast<llvm::BranchInst>(new_block->getTerminator())->isUnconditional())
      << "Expected an unconditional branch at the end of the new block.";
  DCHECK(llvm::cast<llvm::BranchInst>(new_block->getTerminator())->getSuccessor(0) == old_block)
      << "Expected an unconditional branch at the end of the new block.";

  __ SetInsertPoint(old_block->getFirstInsertionPt());
  llvm::PHINode* result_phi = __ CreatePHI(codegen_->GetBooleanType(), 2);

  new_block->getTerminator()->eraseFromParent();
  __ SetInsertPoint(new_block);
  GenerateStringEqualsLoop(call->getArgOperand(0),
                           call->getArgOperand(1),
                           call->getArgOperand(2),
                           old_block,
                           result_phi,
                           codegen_);

  call->replaceAllUsesWith(result_phi);
  return call->eraseFromParent();
}

static void GenerateVisitStringIndexOf(HInvoke* invoke,
                                       CodeGeneratorARM64LLVM* codegen_,
                                       bool start_at_zero) {
  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* done_block = codegen_->CreateBasicBlock();

  llvm::Type* result_type = codegen_->GetLLVMType(invoke->GetType());
  llvm::BasicBlock* current_block = __ GetInsertBlock();
  __ SetInsertPoint(done_block);
  llvm::PHINode* result_phi = __ CreatePHI(result_type, 2);
  __ SetInsertPoint(current_block);

  SlowPathCodeARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
      IntrinsicSlowPathARM64LLVM(invoke, slow_path_entry_block, done_block, result_phi);
  codegen_->AddSlowPath(slow_path);

  // Check for code points > 0xFFFF.
  // NOTE: The ART compiler does some checks here to see if the char is statically known, and
  // decides to emit a slow path based on the result. We omit those checks here and rely on LLVM to
  // do the constant-folding for us.
  llvm::Value* char_value = codegen_->GetValue(invoke->InputAt(1), DataType::Type::kInt32);
  // __ Tst(char_reg, 0xFFFF0000);
  llvm::Value* do_slow_path =
      __ CreateICmpSGT(char_value, codegen_->GetConstantInt(char_value->getType(), 0xFFFF));
  // __ B(ne, slow_path->GetEntryLabel());
  llvm::Instruction* br = codegen_->CreateBranchIfTrue(do_slow_path, slow_path_entry_block);
  ExpectFalseBranch(br);

  llvm::Value* start_from = codegen_->GetConstantZero(codegen_->GetInt32Type());
  if (!start_at_zero) {
    start_from = codegen_->GetValue(invoke->InputAt(2), DataType::Type::kInt32);
  }

  llvm::SmallVector<llvm::Value*> runtime_arguments;
  runtime_arguments.push_back(codegen_->GetUndefCurrentMethodPointer());
  runtime_arguments.push_back(codegen_->GetValue(invoke->InputAt(0)));
  runtime_arguments.push_back(char_value);
  runtime_arguments.push_back(start_from);

  codegen_->SetInvokeRuntimeParametersAndReturnType(runtime_arguments, result_type);
  codegen_->InvokeRuntime(kQuickIndexOf, invoke, slow_path);
  CheckEntrypointTypes<kQuickIndexOf, int32_t, void*, uint32_t, uint32_t>();
  llvm::Value* runtime_result = codegen_->GetInvokeRuntimeResult();

  // __ Bind(slow_path->GetExitLabel());
  __ CreateBr(done_block);
  result_phi->addIncoming(runtime_result, __ GetInsertBlock());
  __ SetInsertPoint(done_block);

  codegen_->AddValue(invoke, result_phi);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitStringIndexOf(HInvoke* invoke) {
  GenerateVisitStringIndexOf(invoke, codegen_, /* start_at_zero= */ true);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitStringIndexOfAfter(HInvoke* invoke) {
  GenerateVisitStringIndexOf(invoke, codegen_, /* start_at_zero= */ false);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitStringNewStringFromBytes(HInvoke* invoke) {
  llvm::BasicBlock* entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* exit_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* not_slow_path_block = codegen_->CreateBasicBlock();

  llvm::Type* result_type = codegen_->GetLLVMType(invoke->GetType());

  llvm::BasicBlock* current_block = __ GetInsertBlock();
  __ SetInsertPoint(exit_block);
  llvm::PHINode* phi_result = __ CreatePHI(result_type, 2);
  __ SetInsertPoint(current_block);

  SlowPathCodeARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
      IntrinsicSlowPathARM64LLVM(invoke, entry_block, exit_block, phi_result);
  codegen_->AddSlowPath(slow_path);

  llvm::Value* byte_array = GetValue(invoke->InputAt(0));
  llvm::Value* is_null = __ CreateICmpEQ(byte_array, GetConstantZero(byte_array->getType()));
  __ CreateCondBr(is_null, slow_path->GetEntryBlock(), not_slow_path_block);

  __ SetInsertPoint(not_slow_path_block);

  llvm::SmallVector<llvm::Value*> runtime_arguments;
  runtime_arguments.push_back(codegen_->GetUndefCurrentMethodPointer());
  runtime_arguments.push_back(byte_array);
  runtime_arguments.push_back(GetValue(invoke->InputAt(1)));
  runtime_arguments.push_back(GetValue(invoke->InputAt(2)));
  runtime_arguments.push_back(GetValue(invoke->InputAt(3)));

  codegen_->SetInvokeRuntimeParametersAndReturnType(runtime_arguments, result_type);

  codegen_->InvokeRuntime(kQuickAllocStringFromBytes, invoke, slow_path);
  CheckEntrypointTypes<kQuickAllocStringFromBytes, void*, void*, int32_t, int32_t, int32_t>();

  llvm::CallBase* runtime_result = codegen_->GetInvokeRuntimeResult();
  runtime_result->addRetAttr(llvm::Attribute::NonNull);

  __ CreateBr(slow_path->GetExitBlock());
  phi_result->addIncoming(runtime_result, __ GetInsertBlock());
  __ SetInsertPoint(slow_path->GetExitBlock());

  AddValue(invoke, phi_result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitStringNewStringFromChars(HInvoke* invoke) {
  // No need to emit code checking whether `locations->InAt(2)` is a null
  // pointer, as callers of the native method
  //
  //   java.lang.StringFactory.newStringFromChars(int offset, int charCount, char[] data)
  //
  // all include a null check on `data` before calling that method.
  llvm::SmallVector<llvm::Value*> runtime_arguments;
  runtime_arguments.push_back(codegen_->GetUndefCurrentMethodPointer());
  runtime_arguments.push_back(GetValue(invoke->InputAt(0)));
  runtime_arguments.push_back(GetValue(invoke->InputAt(1)));
  runtime_arguments.push_back(GetValue(invoke->InputAt(2)));

  llvm::Type* result_type = codegen_->GetLLVMType(invoke->GetType());

  codegen_->SetInvokeRuntimeParametersAndReturnType(runtime_arguments, result_type);
  codegen_->InvokeRuntime(kQuickAllocStringFromChars, invoke);
  CheckEntrypointTypes<kQuickAllocStringFromChars, void*, int32_t, int32_t, void*>();

  llvm::CallBase* runtime_result = codegen_->GetInvokeRuntimeResult();
  runtime_result->addRetAttr(llvm::Attribute::NonNull);

  AddValue(invoke, runtime_result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitStringNewStringFromString(HInvoke* invoke) {
  llvm::BasicBlock* entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* exit_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* not_slow_path_block = codegen_->CreateBasicBlock();

  llvm::Type* result_type = codegen_->GetLLVMType(invoke->GetType());

  llvm::BasicBlock* current_block = __ GetInsertBlock();
  __ SetInsertPoint(exit_block);
  llvm::PHINode* phi_result = __ CreatePHI(result_type, 2);
  __ SetInsertPoint(current_block);

  SlowPathCodeARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
      IntrinsicSlowPathARM64LLVM(invoke, entry_block, exit_block, phi_result);
  codegen_->AddSlowPath(slow_path);

  llvm::Value* string_to_copy = GetValue(invoke->InputAt(0));
  llvm::Value* is_null =
      __ CreateICmpEQ(string_to_copy, GetConstantZero(string_to_copy->getType()));
  __ CreateCondBr(is_null, slow_path->GetEntryBlock(), not_slow_path_block);

  __ SetInsertPoint(not_slow_path_block);

  llvm::SmallVector<llvm::Value*> runtime_arguments;
  runtime_arguments.push_back(codegen_->GetUndefCurrentMethodPointer());
  runtime_arguments.push_back(string_to_copy);

  codegen_->SetInvokeRuntimeParametersAndReturnType(runtime_arguments, result_type);

  codegen_->InvokeRuntime(kQuickAllocStringFromString, invoke, slow_path);
  CheckEntrypointTypes<kQuickAllocStringFromString, void*, void*>();

  llvm::CallBase* runtime_result = codegen_->GetInvokeRuntimeResult();
  runtime_result->addRetAttr(llvm::Attribute::NonNull);

  __ CreateBr(slow_path->GetExitBlock());
  __ SetInsertPoint(slow_path->GetExitBlock());
  phi_result->addIncoming(runtime_result, not_slow_path_block);

  AddValue(invoke, phi_result);
}

static void GenFPToFPCall(HInvoke* invoke,
                          CodeGeneratorARM64LLVM* codegen,
                          QuickEntrypointEnum entry) {
  uint32_t number_of_arguments = invoke->GetNumberOfArguments();
  llvm::SmallVector<llvm::Value*> arguments;
  arguments.push_back(codegen->GetUndefCurrentMethodPointer());
  for (uint32_t i = 0; i < number_of_arguments; ++i) {
    DCHECK(DataType::IsFloatingPointType(invoke->InputAt(i)->GetType()));
    arguments.push_back(codegen->GetValue(invoke->InputAt(i)));
  }
  DCHECK(DataType::IsFloatingPointType(invoke->GetType()));
  llvm::Type* result_type = codegen->GetLLVMType(invoke->GetType());
  codegen->SetInvokeRuntimeParametersAndReturnType(arguments, result_type);
  codegen->InvokeRuntime(entry, invoke);
  llvm::Value* result = codegen->GetInvokeRuntimeResult();
  codegen->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathCos(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickCos);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathSin(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickSin);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathAcos(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAcos);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathAsin(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAsin);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathAtan(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAtan);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathCbrt(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickCbrt);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathCosh(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickCosh);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathExp(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickExp);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathExpm1(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickExpm1);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathLog(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickLog);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathLog10(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickLog10);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathSinh(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickSinh);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathTan(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickTan);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathTanh(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickTanh);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathAtan2(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAtan2);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathPow(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickPow);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathHypot(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickHypot);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathNextAfter(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickNextAfter);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitStringGetCharsNoCheck(HInvoke* invoke) {
  // Check assumption that sizeof(Char) is 2 (used in scaling below).
  const size_t char_size = DataType::Size(DataType::Type::kUint16);
  DCHECK_EQ(char_size, 2u);

  // Location of data in char array buffer.
  const uint32_t data_offset = mirror::Array::DataOffset(char_size).Uint32Value();

  // Location of char array data in string.
  const uint32_t value_offset = mirror::String::ValueOffset().Uint32Value();

  // void getCharsNoCheck(int srcBegin, int srcEnd, char[] dst, int dstBegin);
  llvm::Value* srcObj = GetValue(invoke->InputAt(0));
  llvm::Value* srcBegin = GetValue(invoke->InputAt(1), DataType::Type::kInt32);
  llvm::Value* srcEnd = GetValue(invoke->InputAt(2), DataType::Type::kInt32);
  llvm::Value* dstObj = GetValue(invoke->InputAt(3));
  llvm::Value* dstBegin = GetValue(invoke->InputAt(4), DataType::Type::kInt32);

  DCHECK(srcObj->getType() == GetUncompressedGCPointerType());
  DCHECK(srcBegin->getType() == GetInt32Type());
  DCHECK(srcEnd->getType() == GetInt32Type());
  DCHECK(dstObj->getType() == GetUncompressedGCPointerType());
  DCHECK(dstBegin->getType() == GetInt32Type());

  llvm::BasicBlock* done = codegen_->CreateBasicBlock();

  // __ Sub(num_chr, srcEnd, srcBegin);
  // NOTE: We add nuw and nsw attributes, since srcBegin and srcEnd are non-negative signed
  // integers, and srcEnd >= srcBegin
  llvm::Value* num_chr = __ CreateSub(srcEnd, srcBegin, "", /* HasNUW= */ true, /* HasNSW= */ true);
  // Early out for valid zero-length retrievals.
  // __ Cbz(num_chr, &done);
  llvm::Value* num_chr_is_zero = __ CreateICmpEQ(num_chr, GetConstantZero(GetInt32Type()));
  codegen_->CreateBranchIfTrue(num_chr_is_zero, done);

  // dst address start to copy to.
  // __ Add(dst_ptr, dstObj, Operand(data_offset));
  llvm::Value* dst_ptr = CreateGEP(dstObj, data_offset);
  // __ Add(dst_ptr, dst_ptr, Operand(dstBegin, LSL, 1));
  dst_ptr = CreateGEP(GetUint16Type(), dst_ptr, dstBegin);

  // src address to copy from.
  // __ Add(src_ptr, srcObj, Operand(value_offset));
  llvm::Value* src_ptr = CreateGEP(srcObj, value_offset);

  llvm::BasicBlock* compressed_string_preloop = nullptr;
  if (mirror::kUseStringCompression) {
    // Location of count in string.
    const uint32_t count_offset = mirror::String::CountOffset().Uint32Value();
    // String's length.
    // __ Ldr(tmp2, MemOperand(srcObj, count_offset));
    llvm::Value* string_length = CreateLoadWithOffset(GetInt32Type(), srcObj, count_offset);
    // __ Tbz(tmp2, 0, &compressed_string_preloop);
    // Clean out compression flag from lengths.
    llvm::Value* last_bit = __ CreateAnd(string_length, 1);
    llvm::Value* is_compressed = __ CreateICmpEQ(last_bit, GetConstantZero(last_bit->getType()));
    compressed_string_preloop = codegen_->CreateBasicBlock();
    codegen_->CreateBranchIfTrue(is_compressed, compressed_string_preloop);
  }

  // Uncompressed loop.
  {
    llvm::Type* value_type = GetUint16Type();
    llvm::Value* src_ptr_uncompressed = CreateGEP(value_type, src_ptr, srcBegin);

    // Create a placeholder function call for the inline copying, which will be replaced by a
    // vectorized loop after optimization.
    llvm::Function* placeholder_function = codegen_->GetMemCpyI16PlaceholderFunction();
    __ CreateCall(placeholder_function, {dst_ptr, src_ptr_uncompressed, num_chr});
    __ CreateBr(done);
  }

  if (mirror::kUseStringCompression) {
    __ SetInsertPoint(compressed_string_preloop);

    llvm::Type* loaded_type = GetUint8Type();
    llvm::Value* src_ptr_compressed = CreateGEP(loaded_type, src_ptr, srcBegin);

    // Create a placeholder function call for the inline copying, which will be replaced by a
    // vectorized loop after optimization.
    llvm::Function* placeholder_function = codegen_->GetMemCpyI8ZextToI16PlaceholderFunction();
    __ CreateCall(placeholder_function, {dst_ptr, src_ptr_compressed, num_chr});
    __ CreateBr(done);
  }

  __ SetInsertPoint(done);
}

static void CheckSystemArrayCopyPosition(CodeGeneratorARM64LLVM* codegen_,
                                         llvm::Value* array,
                                         llvm::Value* pos,
                                         llvm::Value* length,
                                         SlowPathCodeARM64LLVM* slow_path,
                                         bool length_is_array_length,
                                         bool position_sign_checked) {
  // Check that pos >= 0.
  if (!position_sign_checked) {
    // __ Tbnz(pos_reg, pos_reg.GetSizeInBits() - 1, slow_path->GetEntryLabel());
    llvm::Value* is_negative = __ CreateICmpSLT(pos, codegen_->GetConstantZero(pos->getType()));
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_negative, slow_path->GetEntryBlock());
    ExpectFalseBranch(br);
  }

  const int32_t length_offset = mirror::Array::LengthOffset().Int32Value();
  llvm::Value* array_length = nullptr;
  if (length_is_array_length) {
    array_length = length;
  } else {
    // __ Ldr(temp, MemOperand(array, length_offset));
    array_length = codegen_->CreateLoadWithOffset(codegen_->GetInt32Type(), array, length_offset);
  }

  // Calculate length(array) - pos.
  // Both operands are known to be non-negative `int32_t`, so the difference cannot underflow as
  // `int32_t`.
  // __ Sub(temp, temp, pos_reg);
  llvm::Value* array_length_minus_pos = __ CreateNSWSub(array_length, pos);

  // Check that (length(array) - pos) >= length.
  // __ Cmp(temp, OperandFrom(length, DataType::Type::kInt32));
  llvm::Value* invalid_pos = __ CreateICmpSLT(array_length_minus_pos, length);
  // __ B(slow_path->GetEntryLabel(), lt);
  llvm::Instruction* br = codegen_->CreateBranchIfTrue(invalid_pos, slow_path->GetEntryBlock());
  ExpectFalseBranch(br);
}

static llvm::Value* GenArrayAddress(CodeGeneratorARM64LLVM* codegen_,
                                    llvm::Value* base,
                                    llvm::Value* pos,
                                    DataType::Type type,
                                    int32_t data_offset) {
  if (data_offset != 0) {
    // __ Add(dest, base, data_offset);
    base = codegen_->CreateGEP(base, data_offset);
  }
  // __ Add(dest, base, Operand(XRegisterFrom(pos), LSL, DataType::SizeShift(type)));
  return codegen_->CreateGEP(codegen_->GetLLVMType(type), base, pos);
}

// This value is greater than ARRAYCOPY_SHORT_CHAR_ARRAY_THRESHOLD in libcore,
// so if we choose to jump to the slow path we will end up in the native implementation.
static constexpr int32_t kSystemArrayCopyCharThreshold = 192;

void IntrinsicCodeGeneratorARM64LLVM::VisitSystemArrayCopyChar(HInvoke* invoke) {
  llvm::Value* src = GetValue(invoke->InputAt(0));
  llvm::Value* src_pos = GetValue(invoke->InputAt(1), DataType::Type::kInt32);
  llvm::Value* dest = GetValue(invoke->InputAt(2));
  llvm::Value* dest_pos = GetValue(invoke->InputAt(3), DataType::Type::kInt32);
  llvm::Value* length = GetValue(invoke->InputAt(4), DataType::Type::kInt32);

  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* done_block = codegen_->CreateBasicBlock();

  SlowPathCodeARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
      IntrinsicSlowPathARM64LLVM(invoke, slow_path_entry_block, done_block);
  codegen_->AddSlowPath(slow_path);

  // If source and destination are the same, take the slow path. Overlapping copy regions must be
  // copied in reverse and we can't know in all cases if it's needed.
  {
    // __ Cmp(src, dst);
    llvm::Value* src_dest_equals = __ CreateICmpEQ(src, dest);
    // __ B(slow_path->GetEntryLabel(), eq);
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(src_dest_equals, slow_path_entry_block);
    ExpectFalseBranch(br);
  }

  // Bail out if the source is null.
  {
    // __ Cbz(src, slow_path->GetEntryLabel());
    llvm::Value* is_src_null = __ CreateICmpEQ(src, GetConstantZero(src->getType()));
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_src_null, slow_path_entry_block);
    ExpectFalseBranch(br);
  }

  // Bail out if the destination is null.
  {
    // __ Cbz(dst, slow_path->GetEntryLabel());
    llvm::Value* is_dest_null = __ CreateICmpEQ(dest, GetConstantZero(dest->getType()));
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_dest_null, slow_path_entry_block);
    ExpectFalseBranch(br);
  }

  // Merge the following two comparisons into one:
  //   If the length is negative, bail out (delegate to libcore's native implementation).
  //   If the length > kSystemArrayCopyCharThreshold then (currently) prefer libcore's
  //   native implementation.
  // NOTE: We don't set a threshold for native fallback. The inline code we generate seems to be
  // faster than the native implementation.
  if constexpr (false) {
    // __ Cmp(WRegisterFrom(length), kSystemArrayCopyCharThreshold);
    llvm::Value* native_threshold =
        GetConstantInt(length->getType(), kSystemArrayCopyCharThreshold);
    llvm::Value* use_native = __ CreateICmpSGT(length, native_threshold);
    // __ B(slow_path->GetEntryLabel(), hi);
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(use_native, slow_path_entry_block);
    ExpectFalseBranch(br);
  } else {
    llvm::Value* use_native = __ CreateICmpSLT(length, GetConstantZero(length->getType()));
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(use_native, slow_path_entry_block);
    ExpectFalseBranch(br);
  }

  CheckSystemArrayCopyPosition(codegen_,
                               src,
                               src_pos,
                               length,
                               slow_path,
                               /*length_is_array_length=*/false,
                               /*position_sign_checked=*/false);

  CheckSystemArrayCopyPosition(codegen_,
                               dest,
                               dest_pos,
                               length,
                               slow_path,
                               /*length_is_array_length=*/false,
                               /*position_sign_checked=*/false);

  const DataType::Type type = DataType::Type::kUint16;
  const int32_t char_size = DataType::Size(DataType::Type::kUint16);
  const uint32_t data_offset = mirror::Array::DataOffset(char_size).Uint32Value();

  llvm::Value* src_start_addr = GenArrayAddress(codegen_, src, src_pos, type, data_offset);
  llvm::Value* dest_start_addr = GenArrayAddress(codegen_, dest, dest_pos, type, data_offset);

  // Create a placeholder function call for the inline copying, which will be replaced by a
  // vectorized loop after optimization.
  llvm::Function* placeholder_function = codegen_->GetMemCpyI16PlaceholderFunction();
  __ CreateCall(placeholder_function, {dest_start_addr, src_start_addr, length});
  __ CreateBr(done_block);
  __ SetInsertPoint(done_block);
}

template <typename CastFn>
static void GenerateVectorizedCopyLoop(llvm::Value* dest,
                                       llvm::Value* src,
                                       llvm::Value* length,
                                       llvm::Type* loaded_type,
                                       CastFn cast_fn,
                                       llvm::BasicBlock* done_block,
                                       CodeGeneratorARM64LLVM* codegen_) {
  // We use 16 byte wide chunks for vectorization.
  constexpr size_t vector_byte_width = 16;
  size_t type_size = codegen_->GetModule()->getDataLayout().getTypeAllocSize(loaded_type);
  DCHECK(type_size >= 1 && type_size < vector_byte_width);
  size_t vector_width = vector_byte_width / type_size;
  llvm::Type* loaded_vector_type = codegen_->GetVectorType(loaded_type, vector_width);
  llvm::MDNode* loop_id = GetVectorizedLoopID(codegen_->GetLLVMContext());

  llvm::BasicBlock* vector_preheader_block = __ GetInsertBlock();
  llvm::BasicBlock* vector_loop_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* vector_end_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* scalar_preheader_block = vector_end_block;
  llvm::BasicBlock* scalar_loop_block = codegen_->CreateBasicBlock();

  llvm::Value* vector_width_value =
      __ getIntN(length->getType()->getIntegerBitWidth(), vector_width);
  llvm::Value* negative_vector_width_value =
      __ getIntN(length->getType()->getIntegerBitWidth(), -static_cast<int>(vector_width));
  llvm::Value* zero_value = codegen_->GetConstantZero(length->getType());

  llvm::Value* vector_iv_start_value = __ CreateNSWAdd(length, negative_vector_width_value);
  llvm::Value* skip_vector_loop = __ CreateICmpSLT(vector_iv_start_value, zero_value);
  __ CreateCondBr(skip_vector_loop, vector_end_block, vector_loop_block);

  // Vector loop:
  //   ldr  q0, [x0], #size
  //   subs w2, #width
  //   str  q0, [x1], #size
  //   b.ge loop
  __ SetInsertPoint(vector_loop_block);
  llvm::PHINode* vector_iv = __ CreatePHI(length->getType(), 2);
  llvm::PHINode* vector_src_iv = __ CreatePHI(src->getType(), 2);
  llvm::PHINode* vector_dest_iv = __ CreatePHI(dest->getType(), 2);
  vector_iv->addIncoming(vector_iv_start_value, vector_preheader_block);
  vector_src_iv->addIncoming(src, vector_preheader_block);
  vector_dest_iv->addIncoming(dest, vector_preheader_block);

  llvm::Value* loaded_vector_value = codegen_->CreateLoad(loaded_vector_type, vector_src_iv);
  llvm::Value* loaded_vector_value_cast = cast_fn(*codegen_->GetIRBuilder(), loaded_vector_value);
  codegen_->CreateStore(loaded_vector_value_cast, vector_dest_iv);

  // NOTE: This needs to be an 'add', otherwise LLVM doesn't generate a 'subs' instruction.
  llvm::Value* vector_iv_next = __ CreateNSWAdd(vector_iv, negative_vector_width_value);
  llvm::Value* vector_src_iv_next = __ CreateConstGEP1_64(loaded_vector_type, vector_src_iv, 1);
  llvm::Value* vector_dest_iv_next =
      __ CreateConstGEP1_64(loaded_vector_value_cast->getType(), vector_dest_iv, 1);
  llvm::Value* vector_is_done = __ CreateICmpSLT(vector_iv_next, zero_value);
  llvm::BranchInst* vector_br =
      __ CreateCondBr(vector_is_done, vector_end_block, vector_loop_block);
  vector_br->setMetadata(llvm::LLVMContext::MD_loop, loop_id);

  vector_iv->addIncoming(vector_iv_next, vector_loop_block);
  vector_src_iv->addIncoming(vector_src_iv_next, vector_loop_block);
  vector_dest_iv->addIncoming(vector_dest_iv_next, vector_loop_block);

  __ SetInsertPoint(vector_end_block);
  llvm::PHINode* reduced_scalar_length = __ CreatePHI(length->getType(), 2);
  llvm::PHINode* scalar_src_start = __ CreatePHI(src->getType(), 2);
  llvm::PHINode* scalar_dest_start = __ CreatePHI(dest->getType(), 2);
  reduced_scalar_length->addIncoming(vector_iv_start_value, vector_preheader_block);
  reduced_scalar_length->addIncoming(vector_iv_next, vector_loop_block);
  scalar_src_start->addIncoming(src, vector_preheader_block);
  scalar_src_start->addIncoming(vector_src_iv_next, vector_loop_block);
  scalar_dest_start->addIncoming(dest, vector_preheader_block);
  scalar_dest_start->addIncoming(vector_dest_iv_next, vector_loop_block);

  llvm::Value* scalar_length = __ CreateAdd(reduced_scalar_length, vector_width_value);
  llvm::Value* skip_scalar_loop = __ CreateICmpEQ(scalar_length, zero_value);
  __ CreateCondBr(skip_scalar_loop, done_block, scalar_loop_block);

  __ SetInsertPoint(scalar_loop_block);
  llvm::PHINode* scalar_iv = __ CreatePHI(length->getType(), 2);
  llvm::PHINode* scalar_src_iv = __ CreatePHI(src->getType(), 2);
  llvm::PHINode* scalar_dest_iv = __ CreatePHI(dest->getType(), 2);
  scalar_iv->addIncoming(scalar_length, scalar_preheader_block);
  scalar_src_iv->addIncoming(scalar_src_start, scalar_preheader_block);
  scalar_dest_iv->addIncoming(scalar_dest_start, scalar_preheader_block);

  llvm::Value* loaded_scalar_value = codegen_->CreateLoad(loaded_type, scalar_src_iv);
  llvm::Value* loaded_scalar_value_cast = cast_fn(*codegen_->GetIRBuilder(), loaded_scalar_value);
  codegen_->CreateStore(loaded_scalar_value_cast, scalar_dest_iv);

  // NOTE: This needs to be an 'add', otherwise LLVM doesn't generate a 'subs' instruction.
  llvm::Value* scalar_iv_next =
      __ CreateNSWAdd(scalar_iv, codegen_->GetConstantInt(length->getType(), -1));
  llvm::Value* scalar_src_iv_next = __ CreateConstGEP1_64(loaded_type, scalar_src_iv, 1);
  llvm::Value* scalar_dest_iv_next =
      __ CreateConstGEP1_64(loaded_scalar_value_cast->getType(), scalar_dest_iv, 1);
  llvm::Value* scalar_is_done = __ CreateICmpEQ(scalar_iv_next, zero_value);
  llvm::BranchInst* scalar_br = __ CreateCondBr(scalar_is_done, done_block, scalar_loop_block);
  scalar_br->setMetadata(llvm::LLVMContext::MD_loop, loop_id);

  scalar_iv->addIncoming(scalar_iv_next, scalar_loop_block);
  scalar_src_iv->addIncoming(scalar_src_iv_next, scalar_loop_block);
  scalar_dest_iv->addIncoming(scalar_dest_iv_next, scalar_loop_block);

  __ SetInsertPoint(done_block);
}

static void GenerateVectorizedCopyLoop(llvm::Value* dest,
                                       llvm::Value* src,
                                       llvm::Value* length,
                                       llvm::Type* loaded_type,
                                       llvm::BasicBlock* done_block,
                                       CodeGeneratorARM64LLVM* codegen_) {
  // If the length is constant, use the @llvm.memcpy.inline intrinsic.
  // NOTE: It would be more correct to use @llvm.memcpy.element.unordered.atomic, but currently that
  // doesn't lower to an inline loop ever. We should patch this in LLVM!
  if (llvm::isa<llvm::ConstantInt>(length)) {
    size_t type_size = codegen_->GetModule()->getDataLayout().getTypeAllocSize(loaded_type);
    size_t byte_length = llvm::cast<llvm::ConstantInt>(length)->getZExtValue() * type_size;
    // For now we set a limit of 256 bytes.
    // TODO: Revise this limit. The only real concern is probably code size.
    if (byte_length <= 256) {
      __ CreateMemCpyInline(
          dest, llvm::Align(type_size), src, llvm::Align(type_size), __ getInt64(byte_length));
      __ CreateBr(done_block);
      return;
    }
  }

  GenerateVectorizedCopyLoop(
      dest,
      src,
      length,
      loaded_type,
      [](llvm::IRBuilder<>&, llvm::Value* loaded_value) { return loaded_value; },
      done_block,
      codegen_);
}

llvm::BasicBlock::iterator RewriteMemCpyI16(llvm::CallInst* call,
                                            CodeGeneratorARM64LLVM* codegen_) {
  DCHECK(call->getCalledFunction()->hasMetadata(kMemCpyI16Metadata));

  llvm::BasicBlock* old_block = call->getParent();
  llvm::BasicBlock* new_block = llvm::splitBlockBefore(old_block, call, nullptr, nullptr, nullptr);
  DCHECK(llvm::isa<llvm::BranchInst>(new_block->getTerminator()))
      << "Expected an unconditional branch at the end of the new block.";
  DCHECK(llvm::cast<llvm::BranchInst>(new_block->getTerminator())->isUnconditional())
      << "Expected an unconditional branch at the end of the new block.";
  DCHECK(llvm::cast<llvm::BranchInst>(new_block->getTerminator())->getSuccessor(0) == old_block)
      << "Expected an unconditional branch at the end of the new block.";

  new_block->getTerminator()->eraseFromParent();
  __ SetInsertPoint(new_block);
  GenerateVectorizedCopyLoop(call->getArgOperand(0),
                             call->getArgOperand(1),
                             call->getArgOperand(2),
                             codegen_->GetUint16Type(),
                             old_block,
                             codegen_);

  return call->eraseFromParent();
}

llvm::BasicBlock::iterator RewriteMemCpyI32(llvm::CallInst* call,
                                            CodeGeneratorARM64LLVM* codegen_) {
  DCHECK(call->getCalledFunction()->hasMetadata(kMemCpyI32Metadata));

  llvm::BasicBlock* old_block = call->getParent();
  llvm::BasicBlock* new_block = llvm::splitBlockBefore(old_block, call, nullptr, nullptr, nullptr);
  DCHECK(llvm::isa<llvm::BranchInst>(new_block->getTerminator()))
      << "Expected an unconditional branch at the end of the new block.";
  DCHECK(llvm::cast<llvm::BranchInst>(new_block->getTerminator())->isUnconditional())
      << "Expected an unconditional branch at the end of the new block.";
  DCHECK(llvm::cast<llvm::BranchInst>(new_block->getTerminator())->getSuccessor(0) == old_block)
      << "Expected an unconditional branch at the end of the new block.";

  new_block->getTerminator()->eraseFromParent();
  __ SetInsertPoint(new_block);
  GenerateVectorizedCopyLoop(
      call->getArgOperand(0),
      call->getArgOperand(1),
      call->getArgOperand(2),
      codegen_->GetUint32Type(),  // NOTE: We're actually copying 32-bit GC pointers.
      old_block,
      codegen_);

  return call->eraseFromParent();
}

llvm::BasicBlock::iterator RewriteMemCpyI8ZextToI16(llvm::CallInst* call,
                                                    CodeGeneratorARM64LLVM* codegen_) {
  DCHECK(call->getCalledFunction()->hasMetadata(kMemCpyI8ZextToI16Metadata));

  llvm::BasicBlock* old_block = call->getParent();
  llvm::BasicBlock* new_block = llvm::splitBlockBefore(old_block, call, nullptr, nullptr, nullptr);
  DCHECK(llvm::isa<llvm::BranchInst>(new_block->getTerminator()))
      << "Expected an unconditional branch at the end of the new block.";
  DCHECK(llvm::cast<llvm::BranchInst>(new_block->getTerminator())->isUnconditional())
      << "Expected an unconditional branch at the end of the new block.";
  DCHECK(llvm::cast<llvm::BranchInst>(new_block->getTerminator())->getSuccessor(0) == old_block)
      << "Expected an unconditional branch at the end of the new block.";

  new_block->getTerminator()->eraseFromParent();
  __ SetInsertPoint(new_block);
  auto cast_fn = [](llvm::IRBuilder<>& builder, llvm::Value* loaded_value) {
    llvm::Type* dest_type = nullptr;
    if (llvm::VectorType* vector_type = llvm::dyn_cast<llvm::VectorType>(loaded_value->getType())) {
      dest_type = llvm::VectorType::get(builder.getInt16Ty(), vector_type->getElementCount());
    } else {
      dest_type = builder.getInt16Ty();
    }
    return builder.CreateZExt(loaded_value, dest_type);
  };
  GenerateVectorizedCopyLoop(call->getArgOperand(0),
                             call->getArgOperand(1),
                             call->getArgOperand(2),
                             codegen_->GetUint8Type(),
                             cast_fn,
                             old_block,
                             codegen_);

  return call->eraseFromParent();
}

// We choose to use the native implementation for longer copy lengths.
static constexpr int32_t kSystemArrayCopyThreshold = 128;

static bool CheckSystemArrayCopy(HInvoke* invoke, CodeGeneratorARM64LLVM* codegen_) {
  // The only read barrier implementation supporting the SystemArrayCopy intrinsic is the
  // Baker-style read barriers.
  if (codegen_->EmitNonBakerReadBarrier()) {
    return false;
  }

  // Check to see if we have known failures that will cause us to have to bail out
  // to the runtime, and just generate the runtime call directly.
  // NOTE: These checks are simplified in LLVM, because the optimizer should be able to infer the
  // results of most of these checks.
  HIntConstant* src_pos = invoke->InputAt(1)->AsIntConstantOrNull();
  HIntConstant* dest_pos = invoke->InputAt(3)->AsIntConstantOrNull();

  SystemArrayCopyOptimizations optimizations(invoke);

  if (optimizations.GetDestinationIsSource()) {
    if (src_pos != nullptr && dest_pos != nullptr && src_pos->GetValue() < dest_pos->GetValue()) {
      // We only support backward copying if source and destination are the same.
      return false;
    }
  }

  if (optimizations.GetDestinationIsPrimitiveArray() || optimizations.GetSourceIsPrimitiveArray()) {
    // We currently don't intrinsify primitive copying.
    return false;
  }

  return true;
}

void IntrinsicCodeGeneratorARM64LLVM::VisitSystemArrayCopy(HInvoke* invoke) {
  if (!CheckSystemArrayCopy(invoke, codegen_)) {
    SetError();
    return;
  }

  uint32_t class_offset = mirror::Object::ClassOffset().Uint32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Uint32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Uint32Value();
  uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Uint32Value();
  // uint32_t monitor_offset = mirror::Object::MonitorOffset().Uint32Value();

  llvm::Value* src = GetValue(invoke->InputAt(0));
  llvm::Value* src_pos = GetValue(invoke->InputAt(1), DataType::Type::kInt32);
  llvm::Value* dest = GetValue(invoke->InputAt(2));
  llvm::Value* dest_pos = GetValue(invoke->InputAt(3), DataType::Type::kInt32);
  llvm::Value* length = GetValue(invoke->InputAt(4), DataType::Type::kInt32);

  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* done_block = codegen_->CreateBasicBlock();

  SlowPathCodeARM64LLVM* intrinsic_slow_path = new (codegen_->GetScopedAllocator())
      IntrinsicSlowPathARM64LLVM(invoke, slow_path_entry_block, done_block);
  codegen_->AddSlowPath(intrinsic_slow_path);

  SystemArrayCopyOptimizations optimizations(invoke);

  // If source and destination are the same, we always go to slow path.
  // FIXME: ART only goes to the slow path if dest_pos > src_pos, i.e. when we need to copy back to
  // front. We don't do this, because in case the length is constant, we use @llvm.memcpy.inline.*,
  // which requires the source and destination buffers not to overlap. Instead, we could check
  // overlapping manually here, so that we can use the fast when copying within the same array.
  {
    llvm::Value* src_dest_equals = nullptr;
    if (optimizations.GetDestinationIsSource()) {
      src_dest_equals = __ getTrue();
    } else {
      src_dest_equals = __ CreateICmpEQ(src, dest);
    }

    llvm::Instruction* br = codegen_->CreateBranchIfTrue(src_dest_equals, slow_path_entry_block);
    ExpectFalseBranch(br);
  }

  if (!optimizations.GetSourceIsNotNull()) {
    // Bail out if the source is null.
    // __ Cbz(src, intrinsic_slow_path->GetEntryLabel());
    llvm::Value* is_src_null = __ CreateICmpEQ(src, GetConstantZero(src->getType()));
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_src_null, slow_path_entry_block);
    ExpectFalseBranch(br);
  }

  if (!optimizations.GetDestinationIsNotNull() && !optimizations.GetDestinationIsSource()) {
    // Bail out if the destination is null.
    // __ Cbz(dest, intrinsic_slow_path->GetEntryLabel());
    llvm::Value* is_dest_null = __ CreateICmpEQ(dest, GetConstantZero(dest->getType()));
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_dest_null, slow_path_entry_block);
    ExpectFalseBranch(br);
  }

  // Merge the following two comparisons into one:
  //   If the length is negative, bail out (delegate to libcore's native implementation).
  //   If the length >= 128 then (currently) prefer native implementation.
  if constexpr (false) {
    // __ Cmp(WRegisterFrom(length), kSystemArrayCopyThreshold);
    llvm::Value* native_threshold = GetConstantInt(length->getType(), kSystemArrayCopyThreshold);
    llvm::Value* use_native = __ CreateICmpUGT(length, native_threshold);
    // __ B(intrinsic_slow_path->GetEntryLabel(), hs);
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(use_native, slow_path_entry_block);
    ExpectFalseBranch(br);
  } else {
    llvm::Value* use_native = __ CreateICmpSLT(length, GetConstantZero(length->getType()));
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(use_native, slow_path_entry_block);
    ExpectFalseBranch(br);
  }

  // Validity checks: source.
  CheckSystemArrayCopyPosition(codegen_,
                               src,
                               src_pos,
                               length,
                               intrinsic_slow_path,
                               optimizations.GetCountIsSourceLength(),
                               /* position_sign_checked= */ false);

  // Validity checks: dest.
  bool dest_position_sign_checked = optimizations.GetSourcePositionIsDestinationPosition();
  CheckSystemArrayCopyPosition(codegen_,
                               dest,
                               dest_pos,
                               length,
                               intrinsic_slow_path,
                               optimizations.GetCountIsDestinationLength(),
                               dest_position_sign_checked);

  auto check_non_primitive_array_class = [&](llvm::Value* klass) {
    // No read barrier is needed for reading a chain of constant references for comparing
    // with null, or for reading a constant primitive value, see `ReadBarrierOption`.
    // /* HeapReference<Class> */ temp = klass->component_type_
    // __ Ldr(temp, HeapOperand(klass, component_offset));
    llvm::Value* component =
        CreateLoadWithOffset(GetUncompressedGCPointerType(), klass, component_offset);
    component = codegen_->MaybeUnpoisonHeapReference(component);
    // Check that the component type is not null.
    {
      // __ Cbz(temp, intrinsic_slow_path->GetEntryLabel());
      llvm::Value* component_is_null =
          __ CreateICmpEQ(component, GetConstantZero(component->getType()));
      llvm::Instruction* br =
          codegen_->CreateBranchIfTrue(component_is_null, slow_path_entry_block);
      ExpectFalseBranch(br);
    }
    // Check that the component type is not a primitive.
    // /* uint16_t */ temp = static_cast<uint16>(klass->primitive_type_);
    {
      // __ Ldrh(temp, HeapOperand(temp, primitive_offset));
      llvm::Value* primitive = CreateLoadWithOffset(GetUint16Type(), component, primitive_offset);
      // __ Cbnz(temp, intrinsic_slow_path->GetEntryLabel());
      llvm::Value* is_primitive =
          __ CreateICmpNE(primitive, GetConstantInt(primitive->getType(), Primitive::kPrimNot));
      llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_primitive, slow_path_entry_block);
      ExpectFalseBranch(br);
    }
  };

  if (!optimizations.GetDoesNotNeedTypeCheck()) {
    // Check whether all elements of the source array are assignable to the component
    // type of the destination array. We do two checks: the classes are the same,
    // or the destination is Object[]. If none of these checks succeed, we go to the
    // slow path.

    llvm::Value* dest_class = nullptr;
    llvm::Value* src_class = nullptr;
    if (codegen_->EmitBakerReadBarrier()) {
      // /* HeapReference<Class> */ temp1 = dest->klass_
      dest_class = codegen_->GenerateFieldLoadWithBakerReadBarrier(invoke,
                                                                   dest,
                                                                   class_offset,
                                                                   /* needs_null_check= */ false,
                                                                   /* use_load_acquire= */ false);
      // /* HeapReference<Class> */ temp2 = src->klass_
      src_class = codegen_->GenerateFieldLoadWithBakerReadBarrier(invoke,
                                                                  src,
                                                                  class_offset,
                                                                  /* needs_null_check= */ false,
                                                                  /* use_load_acquire= */ false);
    } else {
      // /* HeapReference<Class> */ temp1 = dest->klass_
      // __ Ldr(temp1, MemOperand(dest, class_offset));
      dest_class = CreateLoadWithOffset(GetUncompressedGCPointerType(), dest, class_offset);
      dest_class = codegen_->MaybeUnpoisonHeapReference(dest_class);
      // /* HeapReference<Class> */ temp2 = src->klass_
      // __ Ldr(temp2, MemOperand(src, class_offset));
      src_class = CreateLoadWithOffset(GetUncompressedGCPointerType(), src, class_offset);
      src_class = codegen_->MaybeUnpoisonHeapReference(src_class);
    }

    // __ Cmp(temp1, temp2);
    if (optimizations.GetDestinationIsTypedObjectArray()) {
      DCHECK(optimizations.GetDestinationIsNonPrimitiveArray());
      llvm::BasicBlock* do_copy_block = codegen_->CreateBasicBlock();
      // For class match, we can skip the source type check regardless of the optimization flag.
      llvm::Value* is_matching_class = __ CreateICmpEQ(dest_class, src_class);
      // __ B(&do_copy, eq);
      codegen_->CreateBranchIfTrue(is_matching_class, do_copy_block);
      // No read barrier is needed for reading a chain of constant references
      // for comparing with null, see `ReadBarrierOption`.
      // /* HeapReference<Class> */ temp1 = temp1->component_type_
      // __ Ldr(temp1, HeapOperand(temp1, component_offset));
      llvm::Value* dest_component =
          CreateLoadWithOffset(GetUncompressedGCPointerType(), dest_class, component_offset);
      dest_component = codegen_->MaybeUnpoisonHeapReference(dest_component);
      // /* HeapReference<Class> */ temp1 = temp1->super_class_
      // __ Ldr(temp1, HeapOperand(temp1, super_offset));
      llvm::Value* dest_component_super_class =
          CreateLoadWithOffset(GetUncompressedGCPointerType(), dest_component, super_offset);
      // No need to unpoison the result, we're comparing against null.
      {
        // __ Cbnz(temp1, intrinsic_slow_path->GetEntryLabel());
        llvm::Value* dest_component_has_super = __ CreateICmpNE(
            dest_component_super_class, GetConstantZero(dest_component_super_class->getType()));
        llvm::Instruction* br =
            codegen_->CreateBranchIfTrue(dest_component_has_super, slow_path_entry_block);
        ExpectFalseBranch(br);
      }
      // Bail out if the source is not a non primitive array.
      if (!optimizations.GetSourceIsNonPrimitiveArray()) {
        check_non_primitive_array_class(src_class);
      }
      // __ Bind(&do_copy);
      __ CreateBr(do_copy_block);
      __ SetInsertPoint(do_copy_block);
    } else {
      DCHECK(!optimizations.GetDestinationIsTypedObjectArray());
      // For class match, we can skip the array type check completely if at least one of source
      // and destination is known to be a non primitive array, otherwise one check is enough.
      {
        llvm::Value* are_different_classes = __ CreateICmpNE(dest_class, src_class);
        // __ B(intrinsic_slow_path->GetEntryLabel(), ne);
        llvm::Instruction* br =
            codegen_->CreateBranchIfTrue(are_different_classes, slow_path_entry_block);
        ExpectFalseBranch(br);
      }
      if (!optimizations.GetDestinationIsNonPrimitiveArray() &&
          !optimizations.GetSourceIsNonPrimitiveArray()) {
        check_non_primitive_array_class(src_class);
      }
    }
  } else if (!optimizations.GetSourceIsNonPrimitiveArray()) {
    DCHECK(optimizations.GetDestinationIsNonPrimitiveArray());
    // Bail out if the source is not a non primitive array.
    // No read barrier is needed for reading a chain of constant references for comparing
    // with null, or for reading a constant primitive value, see `ReadBarrierOption`.
    // /* HeapReference<Class> */ temp2 = src->klass_
    // __ Ldr(temp2, MemOperand(src, class_offset));
    llvm::Value* src_class =
        CreateLoadWithOffset(GetUncompressedGCPointerType(), src, class_offset);
    src_class = codegen_->MaybeUnpoisonHeapReference(src_class);
    check_non_primitive_array_class(src_class);
  }

  // Don't enter the copy loop if the length is zero.
  // __ Cbz(WRegisterFrom(length), &skip_copy_and_write_barrier);
  llvm::Value* is_length_zero = __ CreateICmpEQ(length, GetConstantZero(length->getType()));
  codegen_->CreateBranchIfTrue(is_length_zero, done_block);

  bool emit_rb = codegen_->EmitBakerReadBarrier();

  const DataType::Type type = DataType::Type::kReference;
  const int32_t element_size = DataType::Size(type);
  const uint32_t data_offset = mirror::Array::DataOffset(element_size).Uint32Value();

  // SlowPathCodeARM64LLVM* read_barrier_slow_path = nullptr;
  if (emit_rb) {
    TODO();
  }

  // Compute base source address, base destination address, and end
  // source address for System.arraycopy* intrinsics in `src_base`,
  // `dst_base` and `src_end` respectively.
  llvm::Value* src_start_addr = GenArrayAddress(codegen_, src, src_pos, type, data_offset);
  llvm::Value* dest_start_addr = GenArrayAddress(codegen_, dest, dest_pos, type, data_offset);

  if (emit_rb) {
    TODO();
  }

  // Create a placeholder function call for the inline copying, which will be replaced by a
  // vectorized loop after optimization.
  DCHECK_EQ(element_size, 4);
  llvm::Function* placeholder_function = codegen_->GetMemCpyI32PlaceholderFunction();
  __ CreateCall(placeholder_function, {dest_start_addr, src_start_addr, length});
  __ CreateBr(done_block);
  __ SetInsertPoint(done_block);
}

static void GenIsInfinite(HInvoke* invoke, CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* value = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* test = codegen_->GetConstantInt(codegen_->GetInt32Type(), llvm::FPClassTest::fcInf);
  llvm::Value* result =
      __ CreateIntrinsic(llvm::Intrinsic::is_fpclass, {value->getType()}, {value, test});
  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFloatIsInfinite(HInvoke* invoke) {
  GenIsInfinite(invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitDoubleIsInfinite(HInvoke* invoke) {
  GenIsInfinite(invoke, codegen_);
}

// Copied from intrinsics.cc
static bool CanReferenceBootImageObjects(HInvoke* invoke, const CompilerOptions& compiler_options) {
  // Piggyback on the method load kind to determine whether we can use PC-relative addressing
  // for AOT. This should cover both the testing config (non-PIC boot image) and codegens that
  // reject PC-relative load kinds and fall back to the runtime call.
  if (compiler_options.IsAotCompiler() &&
      !invoke->AsInvokeStaticOrDirect()->HasPcRelativeMethodLoadKind()) {
    return false;
  }
  if (!compiler_options.IsBootImage() &&
      Runtime::Current()->GetHeap()->GetBootImageSpaces().empty()) {
    return false;  // Running without boot image, cannot use required boot image objects.
  }
  return true;
}

#define VISIT_INTRINSIC(name, low, high, type, start_index)                              \
  void IntrinsicCodeGeneratorARM64LLVM::Visit##name##ValueOf(HInvoke* invoke) {          \
    const CompilerOptions& compiler_options = codegen_->GetCompilerOptions();            \
    if (!CanReferenceBootImageObjects(invoke, compiler_options)) {                       \
      SetError();                                                                        \
      return;                                                                            \
    }                                                                                    \
    IntrinsicVisitor::ValueOfInfo info =                                                 \
        IntrinsicVisitor::ComputeValueOfInfo(invoke,                                     \
                                             compiler_options,                           \
                                             WellKnownClasses::java_lang_##name##_value, \
                                             low,                                        \
                                             high - low + 1,                             \
                                             start_index);                               \
    HandleValueOf(invoke, info, type);                                                   \
  }
BOXED_TYPES(VISIT_INTRINSIC)
#undef VISIT_INTRINSIC

void IntrinsicCodeGeneratorARM64LLVM::HandleValueOf(HInvoke* invoke,
                                                    const IntrinsicVisitor::ValueOfInfo& info,
                                                    DataType::Type type) {
  llvm::Value* in = GetValue(invoke->InputAt(0));
  bool is_signed = !DataType::IsUnsignedType(invoke->InputAt(0)->GetType());
  llvm::Value* in_int32 = __ CreateIntCast(in, GetInt32Type(), is_signed);
  // Check bounds of our cache.
  // __ Add(out.W(), in.W(), -info.low);
  llvm::Value* index = __ CreateSub(in_int32, GetConstantInt(in_int32->getType(), info.low));
  // __ Cmp(out.W(), info.length);
  llvm::Value* needs_allocation =
      __ CreateICmpUGE(index, GetConstantInt(GetUint32Type(), info.length));
  llvm::BasicBlock* allocate_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* load_cached_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* done_block = codegen_->CreateBasicBlock();
  // __ B(&allocate, hs);
  __ CreateCondBr(needs_allocation, allocate_block, load_cached_block);

  __ SetInsertPoint(done_block);
  llvm::PHINode* result_phi = __ CreatePHI(GetUncompressedGCPointerType(), 2);

  // If the value is within the bounds, load the object directly from the array.
  __ SetInsertPoint(load_cached_block);
  llvm::Value* cached_value = nullptr;
  if (invoke->InputAt(0)->IsIntConstant()) {
    // NOTE: Control flow can only reach this point if the value is cached, otherwise this will just
    //       be dead code, so we can safely set cached_value to undef in case it's not cached.
    int32_t value = invoke->InputAt(0)->AsIntConstant()->GetValue();
    if (static_cast<uint32_t>(value - info.low) < info.length) {
      DCHECK_NE(info.value_boot_image_reference, ValueOfInfo::kInvalidReference);
      cached_value = codegen_->LoadBootImageAddress(info.value_boot_image_reference);
    } else {
      cached_value = llvm::UndefValue::get(GetUncompressedGCPointerType());
    }
  } else {
    llvm::Value* cached_array_base =
        codegen_->LoadBootImageAddress(info.array_data_boot_image_reference);
    llvm::Value* cached_value_address =
        CreateGEP(GetUncompressedGCPointerType(), cached_array_base, index);
    cached_value = codegen_->Load(DataType::Type::kReference, cached_value_address);
    cached_value = codegen_->MaybeUnpoisonHeapReference(cached_value);
  }
  // __ B(&done);
  __ CreateBr(done_block);
  result_phi->addIncoming(cached_value, __ GetInsertBlock());

  // __ Bind(&allocate);
  __ SetInsertPoint(allocate_block);
  // Otherwise allocate and initialize a new object.
  llvm::Value* intrinsic_class = codegen_->LoadIntrinsicDeclaringClass(invoke);
  codegen_->SetInvokeRuntimeParametersAndReturnType(
      {codegen_->GetUndefCurrentMethodPointer(), intrinsic_class}, GetUncompressedGCPointerType());
  codegen_->InvokeRuntime(kQuickAllocObjectInitialized, invoke);
  CheckEntrypointTypes<kQuickAllocObjectWithChecks, void*, mirror::Class*>();
  llvm::CallBase* allocated_value = codegen_->GetInvokeRuntimeResult();
  allocated_value->addRetAttr(llvm::Attribute::NonNull);
  llvm::Value* allocated_value_box_storage = CreateGEP(allocated_value, info.value_offset);
  bool is_value_signed = !DataType::IsUnsignedType(invoke->InputAt(0)->GetType());
  codegen_->Store(type, is_value_signed, in, allocated_value_box_storage);
  // Class pointer and `value` final field stores require a barrier before publication.
  codegen_->GenerateMemoryBarrier(MemBarrierKind::kStoreStore);
  __ CreateBr(done_block);
  result_phi->addIncoming(allocated_value, __ GetInsertBlock());

  // __ Bind(&done);
  __ SetInsertPoint(done_block);
  AddValue(invoke, result_phi);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitReferenceGetReferent(HInvoke* invoke) {
  if (!CanReferenceBootImageObjects(invoke, codegen_->GetCompilerOptions())) {
    SetError();
    return;
  }

  llvm::Value* obj = GetValue(invoke->InputAt(0));

  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* exit_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* current_block = __ GetInsertBlock();
  __ SetInsertPoint(exit_block);
  llvm::PHINode* result_phi = __ CreatePHI(GetLLVMType(invoke->GetType()), 2);
  __ SetInsertPoint(current_block);
  SlowPathCodeARM64LLVM* slow_path = new (GetAllocator())
      IntrinsicSlowPathARM64LLVM(invoke, slow_path_entry_block, exit_block, result_phi);
  codegen_->AddSlowPath(slow_path);

  if (codegen_->EmitReadBarrier()) {
    TODO();
  }

  {
    // Load the java.lang.ref.Reference class.
    llvm::Value* reference_class = codegen_->LoadIntrinsicDeclaringClass(invoke);

    // Check static fields java.lang.ref.Reference.{disableIntrinsic,slowPathEnabled} together.
    MemberOffset disable_intrinsic_offset = IntrinsicVisitor::GetReferenceDisableIntrinsicOffset();
    DCHECK_ALIGNED(disable_intrinsic_offset.Uint32Value(), 2u);
    DCHECK_EQ(disable_intrinsic_offset.Uint32Value() + 1u,
              IntrinsicVisitor::GetReferenceSlowPathEnabledOffset().Uint32Value());
    // __ Ldrh(temp, HeapOperand(temp, disable_intrinsic_offset.Uint32Value()));
    llvm::Value* disable_intrinsic_flag = CreateLoadWithOffset(
        GetUint16Type(), reference_class, disable_intrinsic_offset.Uint32Value());
    // __ Cbnz(temp, slow_path->GetEntryLabel());
    llvm::Value* disable_intrinsic =
        __ CreateICmpNE(disable_intrinsic_flag, GetConstantZero(disable_intrinsic_flag->getType()));
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(disable_intrinsic, slow_path_entry_block);
    ExpectFalseBranch(br);
  }

  // Load the value from the field.
  uint32_t referent_offset = mirror::Reference::ReferentOffset().Uint32Value();
  llvm::Value* result = nullptr;
  if (codegen_->EmitBakerReadBarrier()) {
    result = codegen_->GenerateFieldLoadWithBakerReadBarrier(invoke,
                                                             obj,
                                                             referent_offset,
                                                             /*needs_null_check=*/true,
                                                             /*use_load_acquire=*/true);
  } else {
    // MemOperand field = HeapOperand(WRegisterFrom(obj), referent_offset);
    llvm::Value* field = CreateGEP(obj, referent_offset);
    result = codegen_->LoadVolatile(
        invoke, DataType::Type::kReference, field, /* needs_null_check= */ true);
    result = codegen_->MaybeGenerateReadBarrierSlow(invoke, result, obj, referent_offset);
  }
  // __ Bind(slow_path->GetExitLabel());
  __ CreateBr(exit_block);
  result_phi->addIncoming(result, __ GetInsertBlock());
  __ SetInsertPoint(exit_block);
  AddValue(invoke, result_phi);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitReferenceRefersTo(HInvoke* invoke) {
  if (codegen_->EmitNonBakerReadBarrier()) {
    SetError();
    return;
  }

  llvm::Value* obj = GetValue(invoke->InputAt(0));
  llvm::Value* other = GetValue(invoke->InputAt(1));

  uint32_t referent_offset = mirror::Reference::ReferentOffset().Uint32Value();
  // uint32_t monitor_offset = mirror::Object::MonitorOffset().Uint32Value();

  llvm::Value* field = CreateGEP(obj, referent_offset);
  llvm::Value* referent =
      codegen_->LoadVolatile(invoke, DataType::Type::kReference, field, /*needs_null_check=*/true);
  referent = codegen_->MaybeUnpoisonHeapReference(referent);

  // __ Cmp(tmp, other);

  if (codegen_->EmitReadBarrier()) {
    TODO();
  }

  // Convert ZF into the Boolean result.
  // __ Cset(out, eq);
  llvm::Value* is_equal = __ CreateICmpEQ(referent, other);
  AddValue(invoke, is_equal);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitThreadInterrupted(HInvoke* invoke) {
  const uint32_t offset = Thread::InterruptedOffset<kArm64PointerSize>().Int32Value();

  llvm::Value* load =
      codegen_->CreateLoadFromThreadPointer(GetInt32Type(), offset, llvm::AtomicOrdering::Acquire);

  llvm::BasicBlock* done = codegen_->CreateBasicBlock();
  llvm::BasicBlock* extra_store = codegen_->CreateBasicBlock();

  llvm::Value* is_zero = __ CreateICmpEQ(load, __ getInt32(0));

  __ CreateCondBr(is_zero, done, extra_store);

  __ SetInsertPoint(extra_store);

  codegen_->CreateStoreToThreadPointer(
      GetConstantZero(GetInt32Type()), offset, llvm::AtomicOrdering::Release);

  __ CreateBr(done);

  __ SetInsertPoint(done);
  AddValue(invoke, __ CreateTrunc(load, GetBooleanType()));
}

void IntrinsicCodeGeneratorARM64LLVM::VisitReachabilityFence(HInvoke* invoke) {
  llvm::Value* heap_reference = GetValue(invoke->InputAt(0));
  uint64_t id =
      EncodePatchpointID(PatchpointKind::kReachabilityFence, codegen_->AddStackMapInfo(invoke));
  std::array<llvm::Value*, 5> patchpoint_args = {
      /* ID= */ __ getInt64(id),
      /* number_of_bytes= */ __ getInt32(0),
      /* target= */ GetConstantZero(GetPointerType()),
      /* number_of_arguments= */ __ getInt32(0),
      heap_reference,
  };
  llvm::CallInst* call =
      __ CreateIntrinsic(llvm::Intrinsic::experimental_patchpoint_void, {}, patchpoint_args);
  call->addFnAttr(llvm::Attribute::getWithMemoryEffects(codegen_->GetLLVMContext(),
                                                        llvm::MemoryEffects::none()));
}

// Lower the invoke of CRC32.update(int crc, int b).
void IntrinsicCodeGeneratorARM64LLVM::VisitCRC32Update(HInvoke* invoke) {
  if (!codegen_->CPUHasCRC()) {
    SetError();
    return;
  }

  llvm::Value* crc = GetValue(invoke->InputAt(0), DataType::Type::kInt32);
  llvm::Value* val = GetValue(invoke->InputAt(1), DataType::Type::kInt32);

  crc = __ CreateNot(crc);
  llvm::Value* res =
      __ CreateIntrinsic(llvm::Intrinsic::AARCH64Intrinsics::aarch64_crc32b, {}, {crc, val});
  res = __ CreateNot(res);

  AddValue(invoke, res);
}

// Generate code using CRC32 instructions which calculates
// a CRC32 value of a byte.
//
// Parameters:
//   crc    - a register holding an initial CRC value
//   ptr    - a register holding a memory address of bytes
//   length - a register holding a number of bytes to process
static llvm::Value* GenerateCodeForCalculationCRC32ValueOfBytes(CodeGeneratorARM64LLVM* codegen_,
                                                                llvm::Value* crc,
                                                                llvm::Value* ptr,
                                                                llvm::Value* length) {
  // The algorithm of CRC32 of bytes is:
  //   crc = ~crc
  //   process a few first bytes to make the array 8-byte aligned
  //   while array has 8 bytes do:
  //     crc = crc32_of_8bytes(crc, 8_bytes(array))
  //   if array has 4 bytes:
  //     crc = crc32_of_4bytes(crc, 4_bytes(array))
  //   if array has 2 bytes:
  //     crc = crc32_of_2bytes(crc, 2_bytes(array))
  //   if array has a byte:
  //     crc = crc32_of_byte(crc, 1_byte(array))
  //   crc = ~crc
  llvm::BasicBlock* start = __ GetInsertBlock();
  llvm::BasicBlock* loop = codegen_->CreateBasicBlock();
  llvm::BasicBlock* done = codegen_->CreateBasicBlock();

  llvm::BasicBlock* proc_4b = codegen_->CreateBasicBlock();
  llvm::BasicBlock* proc_2b = codegen_->CreateBasicBlock();
  llvm::BasicBlock* proc_1b = codegen_->CreateBasicBlock();

  llvm::BasicBlock* aligned2 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* aligned4 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* aligned8 = codegen_->CreateBasicBlock();

  llvm::BasicBlock* start_cont1 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* start_cont2 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* aligned2_cont1 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* aligned2_cont2 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* aligned4_cont1 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* aligned4_cont2 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* proc_1b_cont1 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* proc_2b_cont1 = codegen_->CreateBasicBlock();
  llvm::BasicBlock* proc_4b_cont1 = codegen_->CreateBasicBlock();

  llvm::Type* ptr_type = ptr->getType();
  llvm::Type* length_type = length->getType();
  llvm::Type* out_type = codegen_->GetInt32Type();

  // __ Mvn(out, crc);
  llvm::Value* out_start = __ CreateNot(crc);

  // __ Tbz(ptr, 0, &aligned2);
  llvm::Value* mask_start = __ CreateShl(__ getInt32(1), __ getInt32(0));
  llvm::Value* ptr_int_start = codegen_->CreateLoad(codegen_->GetInt32Type(), ptr);
  llvm::Value* bit_start = __ CreateAnd(ptr_int_start, mask_start);
  llvm::Value* is_zero_start = __ CreateICmpEQ(bit_start, __ getInt32(0));
  __ CreateCondBr(is_zero_start, aligned2, start_cont1);

  __ SetInsertPoint(start_cont1);
  // __ Subs(len, len, 1);
  llvm::Value* length_start_cont1 = __ CreateSub(length, __ getInt32(1));
  llvm::Value* less_than_one = __ CreateICmpSLT(length_start_cont1, __ getInt32(0));
  __ CreateCondBr(less_than_one, done, start_cont2);

  __ SetInsertPoint(start_cont2);
  // __ Ldrb(array_elem, MemOperand(ptr, 1, PostIndex));
  llvm::Value* array_elem_start_cont2 = codegen_->CreateLoad(codegen_->GetInt8Type(), ptr);
  llvm::Value* ptr_start_cont2 = __ CreatePtrAdd(ptr, __ getInt32(1));
  // __ Crc32b(out, out, array_elem);
  array_elem_start_cont2 = __ CreateZExt(array_elem_start_cont2, codegen_->GetInt32Type());
  llvm::Value* out_start_cont2 = __ CreateIntrinsic(
      llvm::Intrinsic::AARCH64Intrinsics::aarch64_crc32b, {}, {out_start, array_elem_start_cont2});
  __ CreateBr(aligned2);

  // __ Bind(&aligned2);
  __ SetInsertPoint(aligned2);
  llvm::PHINode* ptr_phi_aligned2 = __ CreatePHI(ptr_type, 2);
  ptr_phi_aligned2->addIncoming(ptr, start);
  ptr_phi_aligned2->addIncoming(ptr_start_cont2, start_cont2);

  llvm::PHINode* length_phi_aligned2 = __ CreatePHI(length_type, 2);
  length_phi_aligned2->addIncoming(length, start);
  length_phi_aligned2->addIncoming(length_start_cont1, start_cont2);

  llvm::PHINode* out_phi_aligned2 = __ CreatePHI(out_type, 2);
  out_phi_aligned2->addIncoming(out_start, start);
  out_phi_aligned2->addIncoming(out_start_cont2, start_cont2);

  // __ Tbz(ptr, 1, &aligned4);
  llvm::Value* mask_aligned2 = __ CreateShl(__ getInt32(1), __ getInt32(1));
  llvm::Value* ptr_int_aligned2 = codegen_->CreateLoad(codegen_->GetInt32Type(), ptr_phi_aligned2);
  llvm::Value* bit_aligned2 = __ CreateAnd(ptr_int_aligned2, mask_aligned2);
  llvm::Value* is_zero_aligned2 = __ CreateICmpEQ(bit_aligned2, __ getInt32(0));
  __ CreateCondBr(is_zero_aligned2, aligned4, aligned2_cont1);

  __ SetInsertPoint(aligned2_cont1);
  // __ Subs(len, len, 2);
  llvm::Value* length_aligned2_cont1 = __ CreateSub(length_phi_aligned2, __ getInt32(2));
  llvm::Value* less_than_two = __ CreateICmpSLT(length_aligned2_cont1, __ getInt32(0));
  __ CreateCondBr(less_than_two, proc_1b, aligned2_cont2);

  __ SetInsertPoint(aligned2_cont2);
  // __ Ldrh(array_elem, MemOperand(ptr, 2, PostIndex));
  llvm::Value* array_elem_aligned2_cont2 =
      codegen_->CreateLoad(codegen_->GetInt16Type(), ptr_phi_aligned2);
  llvm::Value* ptr_aligned2_cont2 = __ CreatePtrAdd(ptr_phi_aligned2, __ getInt32(2));
  // __ Crc32h(out, out, array_elem);
  array_elem_aligned2_cont2 = __ CreateZExt(array_elem_aligned2_cont2, codegen_->GetInt32Type());
  llvm::Value* out_aligned2_cont2 =
      __ CreateIntrinsic(llvm::Intrinsic::AARCH64Intrinsics::aarch64_crc32h,
                         {},
                         {out_phi_aligned2, array_elem_aligned2_cont2});
  __ CreateBr(aligned4);

  // __ Bind(&aligned4);
  __ SetInsertPoint(aligned4);
  llvm::PHINode* ptr_phi_aligned4 = __ CreatePHI(ptr_type, 2);
  ptr_phi_aligned4->addIncoming(ptr_phi_aligned2, aligned2);
  ptr_phi_aligned4->addIncoming(ptr_aligned2_cont2, aligned2_cont2);

  llvm::PHINode* length_phi_aligned4 = __ CreatePHI(length_type, 2);
  length_phi_aligned4->addIncoming(length_phi_aligned2, aligned2);
  length_phi_aligned4->addIncoming(length_aligned2_cont1, aligned2_cont2);

  llvm::PHINode* out_phi_aligned4 = __ CreatePHI(out_type, 2);
  out_phi_aligned4->addIncoming(out_phi_aligned2, aligned2);
  out_phi_aligned4->addIncoming(out_aligned2_cont2, aligned2_cont2);

  // __ Tbz(ptr, 2, &aligned8);
  llvm::Value* mask_aligned4 = __ CreateShl(__ getInt32(1), __ getInt32(2));
  llvm::Value* ptr_int_aligned4 = codegen_->CreateLoad(codegen_->GetInt32Type(), ptr_phi_aligned4);
  llvm::Value* bit_aligned4 = __ CreateAnd(ptr_int_aligned4, mask_aligned4);
  llvm::Value* is_zero_aligned4 = __ CreateICmpEQ(bit_aligned4, __ getInt32(0));
  __ CreateCondBr(is_zero_aligned4, aligned8, aligned4_cont1);

  __ SetInsertPoint(aligned4_cont1);
  // __ Subs(len, len, 4);
  llvm::Value* length_aligned4_cont1 = __ CreateSub(length_phi_aligned4, __ getInt32(4));
  llvm::Value* less_than_four = __ CreateICmpSLT(length_aligned4_cont1, __ getInt32(0));
  __ CreateCondBr(less_than_four, proc_2b, aligned4_cont2);

  __ SetInsertPoint(aligned4_cont2);
  // __ Ldr(array_elem, MemOperand(ptr, 4, PostIndex));
  llvm::Value* array_elem_aligned4_cont2 =
      codegen_->CreateLoad(codegen_->GetInt32Type(), ptr_phi_aligned4);
  llvm::Value* ptr_aligned4_cont2 = __ CreatePtrAdd(ptr_phi_aligned4, __ getInt32(4));
  // __ Crc32w(out, out, array_elem);
  llvm::Value* out_aligned4_cont2 =
      __ CreateIntrinsic(llvm::Intrinsic::AARCH64Intrinsics::aarch64_crc32w,
                         {},
                         {out_phi_aligned4, array_elem_aligned4_cont2});
  __ CreateBr(aligned8);

  // __ Bind(&aligned8);
  __ SetInsertPoint(aligned8);
  llvm::PHINode* ptr_phi_aligned8 = __ CreatePHI(ptr_type, 2);
  ptr_phi_aligned8->addIncoming(ptr_phi_aligned4, aligned4);
  ptr_phi_aligned8->addIncoming(ptr_aligned4_cont2, aligned4_cont2);

  llvm::PHINode* length_phi_aligned8 = __ CreatePHI(length_type, 2);
  length_phi_aligned8->addIncoming(length_phi_aligned4, aligned4);
  length_phi_aligned8->addIncoming(length_aligned4_cont1, aligned4_cont2);

  llvm::PHINode* out_phi_aligned8 = __ CreatePHI(out_type, 2);
  out_phi_aligned8->addIncoming(out_phi_aligned4, aligned4);
  out_phi_aligned8->addIncoming(out_aligned4_cont2, aligned4_cont2);

  // __ Subs(len, len, 8);
  llvm::Value* length_aligned8 = __ CreateSub(length_phi_aligned8, __ getInt32(8));
  // If len < 8 go to process data by 4 bytes, 2 bytes and a byte.
  // __ B(&process_4bytes, lo);
  llvm::Value* less_than_eight = __ CreateICmpSLT(length_aligned8, __ getInt32(0));
  __ CreateCondBr(less_than_eight, proc_4b, loop);

  // The main loop processing data by 8 bytes.
  // __ Bind(&loop);
  __ SetInsertPoint(loop);
  llvm::PHINode* ptr_phi_loop = __ CreatePHI(ptr_type, 2);
  ptr_phi_loop->addIncoming(ptr_phi_aligned8, aligned8);

  llvm::PHINode* length_phi_loop = __ CreatePHI(length_type, 2);
  length_phi_loop->addIncoming(length_aligned8, aligned8);

  llvm::PHINode* out_phi_loop = __ CreatePHI(out_type, 2);
  out_phi_loop->addIncoming(out_phi_aligned8, aligned8);

  // __ Ldr(array_elem.X(), MemOperand(ptr, 8, PostIndex));
  llvm::Value* array_elem_loop = codegen_->CreateLoad(codegen_->GetInt64Type(), ptr_phi_loop);
  llvm::Value* ptr_loop = __ CreatePtrAdd(ptr_phi_loop, __ getInt32(8));
  ptr_phi_loop->addIncoming(ptr_loop, loop);

  //  __ Crc32x(out, out, array_elem.X());
  llvm::Value* out_loop = __ CreateIntrinsic(
      llvm::Intrinsic::AARCH64Intrinsics::aarch64_crc32x, {}, {out_phi_loop, array_elem_loop});
  out_phi_loop->addIncoming(out_loop, loop);

  // __ Subs(len, len, 8);
  llvm::Value* length_loop = __ CreateSub(length_phi_loop, __ getInt32(8));
  length_phi_loop->addIncoming(length_loop, loop);

  llvm::Value* greater_eq_than_eight = __ CreateICmpSGE(length_phi_loop, __ getInt32(8));
  __ CreateCondBr(greater_eq_than_eight, loop, proc_4b);

  // Process the data which is less than 8 bytes.
  // The code generated below works with values of len
  // which come in the range [-8, 0].
  // The first three bits are used to detect whether 4 bytes or 2 bytes or
  // a byte can be processed.
  // The checking order is from bit 2 to bit 0:
  //  bit 2 is set: at least 4 bytes available
  //  bit 1 is set: at least 2 bytes available
  //  bit 0 is set: at least a byte available

  // __ Bind(&process_4bytes);
  __ SetInsertPoint(proc_4b);
  llvm::PHINode* ptr_phi_proc_4b = __ CreatePHI(ptr_type, 2);
  ptr_phi_proc_4b->addIncoming(ptr_phi_aligned8, aligned8);
  ptr_phi_proc_4b->addIncoming(ptr_loop, loop);

  llvm::PHINode* length_phi_proc_4b = __ CreatePHI(length_type, 2);
  length_phi_proc_4b->addIncoming(length_phi_aligned8, aligned8);
  length_phi_proc_4b->addIncoming(length_loop, loop);

  llvm::PHINode* out_phi_proc_4b = __ CreatePHI(out_type, 2);
  out_phi_proc_4b->addIncoming(out_phi_aligned8, aligned8);
  out_phi_proc_4b->addIncoming(out_loop, loop);

  // Goto process_2bytes if less than four bytes available
  // __ Tbz(len, 2, &process_2bytes);
  llvm::Value* mask_proc_4b = __ CreateShl(__ getInt32(1), __ getInt32(2));
  llvm::Value* bit_proc_4b = __ CreateAnd(length_phi_proc_4b, mask_proc_4b);
  llvm::Value* is_zero_proc_4b = __ CreateICmpEQ(bit_proc_4b, __ getInt32(0));
  __ CreateCondBr(is_zero_proc_4b, proc_2b, proc_4b_cont1);

  __ SetInsertPoint(proc_4b_cont1);
  // __ Ldr(array_elem, MemOperand(ptr, 4, PostIndex));
  llvm::Value* array_elem_proc_4b_cont1 =
      codegen_->CreateLoad(codegen_->GetInt32Type(), ptr_phi_proc_4b);
  llvm::Value* ptr_proc_4b = __ CreatePtrAdd(ptr_phi_proc_4b, __ getInt32(4));
  // __ Crc32w(out, out, array_elem);
  llvm::Value* out_proc_4b_cont1 =
      __ CreateIntrinsic(llvm::Intrinsic::AARCH64Intrinsics::aarch64_crc32w,
                         {},
                         {out_phi_proc_4b, array_elem_proc_4b_cont1});
  __ CreateBr(proc_2b);

  // __ Bind(&process_2bytes);
  __ SetInsertPoint(proc_2b);
  llvm::PHINode* ptr_phi_proc_2b = __ CreatePHI(ptr_type, 3);
  ptr_phi_proc_2b->addIncoming(ptr_phi_aligned4, aligned4_cont1);
  ptr_phi_proc_2b->addIncoming(ptr_phi_proc_4b, proc_4b);
  ptr_phi_proc_2b->addIncoming(ptr_proc_4b, proc_4b_cont1);

  llvm::PHINode* length_phi_proc_2b = __ CreatePHI(length_type, 3);
  length_phi_proc_2b->addIncoming(length_phi_proc_4b, proc_4b);
  length_phi_proc_2b->addIncoming(length_phi_proc_4b, proc_4b_cont1);
  length_phi_proc_2b->addIncoming(length_aligned4_cont1, aligned4_cont1);

  llvm::PHINode* out_phi_proc_2b = __ CreatePHI(out_type, 3);
  out_phi_proc_2b->addIncoming(out_phi_proc_4b, proc_4b);
  out_phi_proc_2b->addIncoming(out_proc_4b_cont1, proc_4b_cont1);
  out_phi_proc_2b->addIncoming(out_phi_aligned4, aligned4_cont1);

  // Goto process_1bytes if less than two bytes available
  // __ Tbz(len, 1, &process_1byte);
  llvm::Value* mask_proc_2b = __ CreateShl(__ getInt32(1), __ getInt32(1));
  llvm::Value* bit_proc_2b = __ CreateAnd(length_phi_proc_2b, mask_proc_2b);
  llvm::Value* is_zero_proc_2b = __ CreateICmpEQ(bit_proc_2b, __ getInt32(0));
  __ CreateCondBr(is_zero_proc_2b, proc_1b, proc_2b_cont1);

  __ SetInsertPoint(proc_2b_cont1);
  // __ Ldrh(array_elem, MemOperand(ptr, 2, PostIndex));
  llvm::Value* array_elem_proc_2b_cont1 =
      codegen_->CreateLoad(codegen_->GetInt16Type(), ptr_phi_proc_2b);
  llvm::Value* ptr_proc_2b = __ CreatePtrAdd(ptr_phi_proc_2b, __ getInt32(2));
  // __ Crc32h(out, out, array_elem);
  array_elem_proc_2b_cont1 = __ CreateZExt(array_elem_proc_2b_cont1, codegen_->GetInt32Type());
  llvm::Value* out_proc_2b_cont1 =
      __ CreateIntrinsic(llvm::Intrinsic::AARCH64Intrinsics::aarch64_crc32h,
                         {},
                         {out_phi_proc_2b, array_elem_proc_2b_cont1});
  __ CreateBr(proc_1b);

  // __ Bind(&process_1byte);
  __ SetInsertPoint(proc_1b);

  llvm::PHINode* ptr_phi_proc_1b = __ CreatePHI(ptr_type, 3);
  ptr_phi_proc_1b->addIncoming(ptr_phi_aligned2, aligned2_cont1);
  ptr_phi_proc_1b->addIncoming(ptr_phi_proc_2b, proc_2b);
  ptr_phi_proc_1b->addIncoming(ptr_proc_2b, proc_2b_cont1);

  llvm::PHINode* length_phi_proc_1b = __ CreatePHI(length_type, 3);
  length_phi_proc_1b->addIncoming(length_phi_proc_2b, proc_2b);
  length_phi_proc_1b->addIncoming(length_phi_proc_2b, proc_2b_cont1);
  length_phi_proc_1b->addIncoming(length_aligned2_cont1, aligned2_cont1);

  llvm::PHINode* out_phi_proc_1b = __ CreatePHI(out_type, 3);
  out_phi_proc_1b->addIncoming(out_phi_proc_2b, proc_2b);
  out_phi_proc_1b->addIncoming(out_proc_2b_cont1, proc_2b_cont1);
  out_phi_proc_1b->addIncoming(out_phi_aligned2, aligned2_cont1);

  // Goto done if no bytes available
  // __ Tbz(len, 0, &done);
  llvm::Value* mask_proc_1b = __ CreateShl(__ getInt32(1), __ getInt32(0));
  llvm::Value* bit_proc_1b = __ CreateAnd(length_phi_proc_1b, mask_proc_1b);
  llvm::Value* is_zero_proc_1b = __ CreateICmpEQ(bit_proc_1b, __ getInt32(0));
  __ CreateCondBr(is_zero_proc_1b, done, proc_1b_cont1);

  __ SetInsertPoint(proc_1b_cont1);
  // __ Ldrb(array_elem, MemOperand(ptr));
  llvm::Value* array_elem_proc_1b_cont1 =
      codegen_->CreateLoad(codegen_->GetInt8Type(), ptr_phi_proc_1b);
  // __ Crc32b(out, out, array_elem);
  array_elem_proc_1b_cont1 = __ CreateZExt(array_elem_proc_1b_cont1, codegen_->GetInt32Type());
  llvm::Value* out_proc_1b_cont1 =
      __ CreateIntrinsic(llvm::Intrinsic::AARCH64Intrinsics::aarch64_crc32b,
                         {},
                         {out_phi_proc_1b, array_elem_proc_1b_cont1});
  __ CreateBr(done);

  __ SetInsertPoint(done);
  llvm::PHINode* out_phi = __ CreatePHI(out_type, 3);
  out_phi->addIncoming(out_start, start_cont1);
  out_phi->addIncoming(out_proc_1b_cont1, proc_1b_cont1);
  out_phi->addIncoming(out_phi_proc_1b, proc_1b);

  // __ Mvn(out, out);
  llvm::Value* result = __ CreateNot(out_phi);

  return result;
}

// The threshold for sizes of arrays to use the library provided implementation
// of CRC32.updateBytes instead of the intrinsic.
static constexpr int32_t kCRC32UpdateBytesThreshold = 64 * 1024;

// Lower the invoke of CRC32.updateBytes(int crc, byte[] b, int off, int len)
//
// Note: The intrinsic is not used if len exceeds a threshold.
void IntrinsicCodeGeneratorARM64LLVM::VisitCRC32UpdateBytes(HInvoke* invoke) {
  if (!codegen_->CPUHasCRC()) {
    SetError();
    return;
  }

  llvm::BasicBlock* entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* exit_block = codegen_->CreateBasicBlock();

  llvm::BasicBlock* current_block = __ GetInsertBlock();
  __ SetInsertPoint(exit_block);
  llvm::PHINode* phi_result = __ CreatePHI(GetInt32Type(), 2);

  __ SetInsertPoint(current_block);

  llvm::BasicBlock* not_slow_path_block = codegen_->CreateBasicBlock();

  SlowPathCodeARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
      IntrinsicSlowPathARM64LLVM(invoke, entry_block, exit_block, phi_result);

  codegen_->AddSlowPath(slow_path);

  llvm::Value* length = GetValue(invoke->InputAt(3), DataType::Type::kInt32);
  llvm::Value* threshold = __ getInt32(kCRC32UpdateBytesThreshold);
  llvm::Value* above_threshold = __ CreateICmpSGT(length, threshold);
  __ CreateCondBr(above_threshold, slow_path->GetEntryBlock(), not_slow_path_block);

  __ SetInsertPoint(not_slow_path_block);

  llvm::Value* array = GetValue(invoke->InputAt(1));

  const uint32_t array_data_offset = mirror::Array::DataOffset(Primitive::kPrimByte).Uint32Value();

  llvm::Value* ptr = nullptr;
  if (invoke->InputAt(2)->IsConstant()) {
    int32_t offset_value = invoke->InputAt(2)->AsIntConstant()->GetValue();
    llvm::Value* total_offset = __ getInt32(array_data_offset + offset_value);
    ptr = CreateGEP(array, total_offset);
  } else {
    ptr = CreateGEP(array, __ getInt32(array_data_offset));
    llvm::Value* offset = GetValue(invoke->InputAt(2), DataType::Type::kInt32);
    ptr = __ CreatePtrAdd(ptr, offset);
  }

  llvm::Value* crc = GetValue(invoke->InputAt(0), DataType::Type::kInt32);

  llvm::Value* fast_result =
      GenerateCodeForCalculationCRC32ValueOfBytes(codegen_, crc, ptr, length);

  llvm::BasicBlock* fast_block = __ GetInsertBlock();
  __ CreateBr(slow_path->GetExitBlock());

  __ SetInsertPoint(slow_path->GetExitBlock());
  phi_result->addIncoming(fast_result, fast_block);

  AddValue(invoke, phi_result);
}

// Lower the invoke of CRC32.updateByteBuffer(int crc, long addr, int off, int len)
//
// There is no need to generate code checking if addr is 0.
// The method updateByteBuffer is a private method of java.util.zip.CRC32.
// This guarantees no calls outside of the CRC32 class.
// An address of DirectBuffer is always passed to the call of updateByteBuffer.
// It might be an implementation of an empty DirectBuffer which can use a zero
// address but it must have the length to be zero. The current generated code
// correctly works with the zero length.
void IntrinsicCodeGeneratorARM64LLVM::VisitCRC32UpdateByteBuffer(HInvoke* invoke) {
  if (!codegen_->CPUHasCRC()) {
    SetError();
    return;
  }

  llvm::Value* crc = GetValue(invoke->InputAt(0), DataType::Type::kInt32);
  llvm::Value* addr = GetValue(invoke->InputAt(1), DataType::Type::kInt64);
  llvm::Value* offset = GetValue(invoke->InputAt(2), DataType::Type::kInt32);
  llvm::Value* length = GetValue(invoke->InputAt(3), DataType::Type::kInt32);

  llvm::Value* addr_ptr = __ CreateIntToPtr(addr, codegen_->GetPointerType());
  llvm::Value* ptr = __ CreatePtrAdd(addr_ptr, offset);

  llvm::Value* result = GenerateCodeForCalculationCRC32ValueOfBytes(codegen_, crc, ptr, length);
  AddValue(invoke, result);
}

static llvm::Value* CreateHalfFromInteger(HInvoke* invoke,
                                          const int input_position,
                                          CodeGeneratorARM64LLVM* codegen_) {
  DCHECK(DataType::IsIntegralType(invoke->GetType()));
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(input_position), DataType::Type::kInt16);

  return __ CreateBitCast(input, __ getHalfTy());
}

static void GenerateFP16Compare(HInvoke* invoke,
                                llvm::CmpInst::Predicate predicate,
                                CodeGeneratorARM64LLVM* codegen_) {
  llvm::Type* result_type = codegen_->GetLLVMType(invoke->GetType());
  llvm::Value* half0 = CreateHalfFromInteger(invoke, /* input_position = */ 0, codegen_);
  llvm::Value* half1 = CreateHalfFromInteger(invoke, /* input_position = */ 1, codegen_);

  llvm::Value* cmp_result = __ CreateFCmp(predicate, half0, half1);
  llvm::Value* result = __ CreateZExt(cmp_result, result_type);
  result = __ CreateBitCast(result, result_type);

  codegen_->AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16ToFloat(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  llvm::Value* input = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* half = __ CreateTrunc(input, codegen_->GetInt16Type());
  half = __ CreateBitCast(half, __ getHalfTy());
  half = __ CreateFPExt(half, GetFloat32Type());
  AddValue(invoke, half);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16ToHalf(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  llvm::Value* input = GetValue(invoke->InputAt(0));
  llvm::Value* result = __ CreateBitCast(input, GetFloat32Type());
  result = __ CreateFPTrunc(result, __ getHalfTy());
  result = __ CreateBitCast(result, GetInt16Type());
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16Floor(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  llvm::Value* half = CreateHalfFromInteger(invoke, /* input_position = */ 0, codegen_);
  llvm::Value* res = __ CreateUnaryIntrinsic(llvm::Intrinsic::floor, half);
  res = __ CreateBitCast(res, GetInt16Type());
  AddValue(invoke, res);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16Ceil(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  llvm::Value* half = CreateHalfFromInteger(invoke, /* input_position = */ 0, codegen_);
  llvm::Value* res = __ CreateUnaryIntrinsic(llvm::Intrinsic::ceil, half);
  res = __ CreateBitCast(res, GetInt16Type());
  AddValue(invoke, res);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16Rint(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  llvm::Value* half = CreateHalfFromInteger(invoke, /* input_position = */ 0, codegen_);
  llvm::Value* res = __ CreateUnaryIntrinsic(llvm::Intrinsic::rint, half);
  res = __ CreateBitCast(res, GetInt16Type());
  AddValue(invoke, res);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16Greater(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  GenerateFP16Compare(invoke, llvm::CmpInst::Predicate::FCMP_OGT, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16GreaterEquals(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  GenerateFP16Compare(invoke, llvm::CmpInst::Predicate::FCMP_OGE, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16Less(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  GenerateFP16Compare(invoke, llvm::CmpInst::Predicate::FCMP_OLT, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16LessEquals(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  GenerateFP16Compare(invoke, llvm::CmpInst::Predicate::FCMP_OLE, codegen_);
}

static llvm::Value* CompareAndConditionalSelect(llvm::Value* lhs,
                                                llvm::Value* rhs,
                                                llvm::CmpInst::Predicate predicate,
                                                llvm::Type* result_type,
                                                CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* not_equal_result = nullptr;
  llvm::Value* predicate_result = nullptr;

  if (lhs->getType()->isIntegerTy()) {
    not_equal_result = __ CreateICmpNE(lhs, rhs);
    predicate_result = __ CreateICmp(predicate, lhs, rhs);
  } else if (lhs->getType()->isHalfTy()) {
    not_equal_result = __ CreateFCmpONE(lhs, rhs);
    predicate_result = __ CreateFCmp(predicate, lhs, rhs);
  }

  DCHECK(not_equal_result != nullptr && predicate_result != nullptr);

  llvm::Value* result = __ CreateZExt(not_equal_result, result_type);
  llvm::Value* neg_result = __ CreateNeg(result);
  llvm::Value* comparison_result = __ CreateSelect(predicate_result, result, neg_result);
  comparison_result = __ CreateBitCast(comparison_result, result_type);
  return comparison_result;
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16Compare(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }

  //  0 if: left == right
  //  1 if: left  > right
  // -1 if: left  < right
  llvm::Type* result_type = codegen_->GetLLVMType(invoke->GetType());

  llvm::Value* input0 = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* int0 = __ CreateTrunc(input0, codegen_->GetInt16Type());
  llvm::Value* half0 = __ CreateBitCast(int0, __ getHalfTy());

  llvm::Value* input1 = codegen_->GetValue(invoke->InputAt(1));
  llvm::Value* int1 = __ CreateTrunc(input1, codegen_->GetInt16Type());
  llvm::Value* half1 = __ CreateBitCast(int1, __ getHalfTy());

  llvm::BasicBlock* zero_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* equal_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* nan_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* normal_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* end_block = codegen_->CreateBasicBlock();

  llvm::Value* not_equal_or_nan = __ CreateFCmpUNE(half0, half1);
  codegen_->CreateBranchIfFalse(not_equal_or_nan, equal_block);

  // Not equal OR either of the inputs is NaN.
  // NaN is equal to itself and greater than any other number.
  llvm::Value* is_half0_nan = __ CreateFCmpUNE(half0, half0);
  llvm::Value* is_half1_nan = __ CreateFCmpUNE(half1, half1);
  llvm::Value* either_is_nan = __ CreateOr(is_half0_nan, is_half1_nan);
  __ CreateCondBr(either_is_nan, nan_block, normal_block);
  __ SetInsertPoint(nan_block);
  llvm::Value* nan_result = CompareAndConditionalSelect(
      is_half0_nan, is_half1_nan, llvm::CmpInst::Predicate::ICMP_UGT, result_type, codegen_);
  __ CreateBr(end_block);

  // Equal OR one of the inputs is +0 and the other is -0. Reverse operand order because -0 > +0
  // when compared.
  __ SetInsertPoint(equal_block);
  llvm::Value* operand_is_zero = __ CreateFCmpOEQ(half0, llvm::ConstantFP::getZero(__ getHalfTy()));
  __ CreateCondBr(operand_is_zero, zero_block, normal_block);
  __ SetInsertPoint(zero_block);
  llvm::Value* zero_result = CompareAndConditionalSelect(
      int0, int1, llvm::CmpInst::Predicate::ICMP_ULT, result_type, codegen_);
  __ CreateBr(end_block);

  // Normal values
  __ SetInsertPoint(normal_block);
  llvm::Value* normal_result = CompareAndConditionalSelect(
      half0, half1, llvm::CmpInst::Predicate::FCMP_OGT, result_type, codegen_);
  __ CreateBr(end_block);

  __ SetInsertPoint(end_block);
  llvm::PHINode* result_phi = __ CreatePHI(result_type, 3);
  result_phi->addIncoming(nan_result, nan_block);
  result_phi->addIncoming(zero_result, zero_block);
  result_phi->addIncoming(normal_result, normal_block);
  AddValue(invoke, result_phi);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16Min(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  llvm::Value* half0 = CreateHalfFromInteger(invoke, /* input_position = */ 0, codegen_);
  llvm::Value* half1 = CreateHalfFromInteger(invoke, /* input_position = */ 1, codegen_);
  llvm::Value* min = __ CreateMinimum(half0, half1);
  min = __ CreateBitCast(min, GetInt16Type());
  llvm::Value* is_alt_nan = __ CreateICmpEQ(min, GetFP16AltNaNAsInt());
  min = __ CreateSelect(is_alt_nan, GetFP16NaNAsInt(), min);
  AddValue(invoke, min);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitFP16Max(HInvoke* invoke) {
  if (!codegen_->CPUHasFP16()) {
    SetError();
    return;
  }
  llvm::Value* half0 = CreateHalfFromInteger(invoke, /* input_position = */ 0, codegen_);
  llvm::Value* half1 = CreateHalfFromInteger(invoke, /* input_position = */ 1, codegen_);
  llvm::Value* max = __ CreateMaximum(half0, half1);
  max = __ CreateBitCast(max, GetInt16Type());
  llvm::Value* is_alt_nan = __ CreateICmpEQ(max, GetFP16AltNaNAsInt());
  max = __ CreateSelect(is_alt_nan, GetFP16NaNAsInt(), max);
  AddValue(invoke, max);
}

static void GenerateDivideUnsigned(HInvoke* invoke, CodeGeneratorARM64LLVM* codegen_) {
  DataType::Type type = invoke->GetType();
  DCHECK(type == DataType::Type::kInt32 || type == DataType::Type::kInt64);

  llvm::Type* llvm_type = codegen_->GetLLVMType(type);
  llvm::Value* dividend = codegen_->GetValue(invoke->InputAt(0), type);
  llvm::Value* divisor = codegen_->GetValue(invoke->InputAt(1), type);

  llvm::BasicBlock* current_block = __ GetInsertBlock();
  llvm::BasicBlock* entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* exit_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* not_slow_path_block = codegen_->CreateBasicBlock();

  __ SetInsertPoint(exit_block);
  llvm::PHINode* phi_result = __ CreatePHI(llvm_type, 2);
  __ SetInsertPoint(current_block);

  SlowPathCodeARM64LLVM* slow_path = new (codegen_->GetScopedAllocator())
      IntrinsicSlowPathARM64LLVM(invoke, entry_block, exit_block, phi_result);
  codegen_->AddSlowPath(slow_path);
  llvm::Value* is_zero = __ CreateICmpEQ(divisor, codegen_->GetConstantZero(llvm_type));
  __ CreateCondBr(is_zero, slow_path->GetEntryBlock(), not_slow_path_block);

  __ SetInsertPoint(not_slow_path_block);

  llvm::Value* result = __ CreateUDiv(dividend, divisor);
  __ CreateBr(slow_path->GetExitBlock());
  __ SetInsertPoint(slow_path->GetExitBlock());

  phi_result->addIncoming(result, not_slow_path_block);
  codegen_->AddValue(invoke, phi_result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitIntegerDivideUnsigned(HInvoke* invoke) {
  GenerateDivideUnsigned(invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitLongDivideUnsigned(HInvoke* invoke) {
  GenerateDivideUnsigned(invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathMultiplyHigh(HInvoke* invoke) {
  DataType::Type type = invoke->GetType();
  DCHECK(type == DataType::Type::kInt64);
  llvm::Value* x = codegen_->GetValue(invoke->InputAt(0), type);
  llvm::Value* y = codegen_->GetValue(invoke->InputAt(1), type);

  llvm::Value* x_sext = __ CreateSExt(x, __ getIntNTy(128));
  llvm::Value* y_sext = __ CreateSExt(y, __ getIntNTy(128));
  llvm::Value* mul = __ CreateNSWMul(x_sext, y_sext);
  llvm::Value* mul_shift = __ CreateLShr(mul, 64);

  codegen_->AddValue(invoke, __ CreateTrunc(mul_shift, GetInt64Type()));
}

static void GenerateMathFma(HInvoke* invoke, CodeGeneratorARM64LLVM* codegen_) {
  DataType::Type type = invoke->GetType();
  llvm::Value* a = codegen_->GetValue(invoke->InputAt(0), type);
  llvm::Value* b = codegen_->GetValue(invoke->InputAt(1), type);
  llvm::Value* c = codegen_->GetValue(invoke->InputAt(2), type);

  codegen_->AddValue(
      invoke,
      __ CreateIntrinsic(llvm::Intrinsic::fmuladd, {codegen_->GetLLVMType(type)}, {a, b, c}));
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathFmaDouble(HInvoke* invoke) {
  GenerateMathFma(invoke, codegen_);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMathFmaFloat(HInvoke* invoke) {
  GenerateMathFma(invoke, codegen_);
}

class VarHandleSlowPathARM64LLVM : public IntrinsicSlowPathARM64LLVM {
 public:
  VarHandleSlowPathARM64LLVM(HInvoke* invoke,
                             llvm::BasicBlock* entry_block,
                             llvm::BasicBlock* exit_block,
                             llvm::PHINode* result_phi,
                             llvm::AtomicOrdering order)
      : IntrinsicSlowPathARM64LLVM(invoke, entry_block, exit_block, result_phi),
        order_(order),
        return_success_(false),
        strong_(false),
        get_and_update_op_(GetAndUpdateOp::kAdd) {}

  void SetByteArrayViewCheckBlock(llvm::BasicBlock* block) {
    DCHECK(byte_array_view_check_block_ == nullptr);
    byte_array_view_check_block_ = block;
  }
  llvm::BasicBlock* GetByteArrayViewCheckBlock() const {
    DCHECK(byte_array_view_check_block_ != nullptr);
    return byte_array_view_check_block_;
  }
  bool HasByteArrayViewCheckBlock() const { return byte_array_view_check_block_ != nullptr; }

  void SetNativeByteOrderBlock(llvm::BasicBlock* block, llvm::PHINode* offset_phi) {
    DCHECK(native_byte_order_block_ == nullptr);
    native_byte_order_block_ = block;
    native_byte_order_offset_phi_ = offset_phi;
  }
  llvm::BasicBlock* GetNativeByteOrderBlock() const {
    DCHECK(native_byte_order_block_ != nullptr);
    return native_byte_order_block_;
  }
  llvm::PHINode* GetNativeByteOrderOffsetPhi() const {
    DCHECK(native_byte_order_offset_phi_ != nullptr);
    return native_byte_order_offset_phi_;
  }

  llvm::PHINode* GetResultPhi() const { return result_phi_; }

  void SetCompareAndSetOrExchangeArgs(bool return_success, bool strong) {
    if (return_success) {
      DCHECK(GetAccessModeTemplate() == mirror::VarHandle::AccessModeTemplate::kCompareAndSet);
    } else {
      DCHECK(GetAccessModeTemplate() == mirror::VarHandle::AccessModeTemplate::kCompareAndExchange);
    }
    return_success_ = return_success;
    strong_ = strong;
  }

  void SetGetAndUpdateOp(GetAndUpdateOp get_and_update_op) {
    DCHECK(GetAccessModeTemplate() == mirror::VarHandle::AccessModeTemplate::kGetAndUpdate);
    get_and_update_op_ = get_and_update_op;
  }

  void EmitNativeCode(CodeGenerator* codegen_base) override {
    if (HasByteArrayViewCheckBlock()) {
      EmitByteArrayViewCode(codegen_base);
    }
    IntrinsicSlowPathARM64LLVM::EmitNativeCode(codegen_base);
  }

 private:
  HInvoke* GetInvoke() const { return GetInstruction()->AsInvoke(); }

  mirror::VarHandle::AccessModeTemplate GetAccessModeTemplate() const {
    return mirror::VarHandle::GetAccessModeTemplateByIntrinsic(GetInvoke()->GetIntrinsic());
  }

  void EmitByteArrayViewCode(CodeGenerator* codegen_in);

  llvm::BasicBlock* byte_array_view_check_block_ = nullptr;
  llvm::BasicBlock* native_byte_order_block_ = nullptr;
  llvm::PHINode* native_byte_order_offset_phi_ = nullptr;
  // Shared parameter for all VarHandle intrinsics.
  llvm::AtomicOrdering order_;
  // Extra arguments for GenerateVarHandleCompareAndSetOrExchange().
  bool return_success_;
  bool strong_;
  // Extra argument for GenerateVarHandleGetAndUpdate().
  GetAndUpdateOp get_and_update_op_;
};

// Generate subtype check without read barriers.
static void GenerateSubTypeObjectCheckNoReadBarrier(CodeGeneratorARM64LLVM* codegen_,
                                                    SlowPathCodeARM64LLVM* slow_path,
                                                    llvm::Value* object,
                                                    llvm::Value* type,
                                                    bool object_can_be_null = true) {
  const MemberOffset class_offset = mirror::Object::ClassOffset();
  const MemberOffset super_class_offset = mirror::Class::SuperClassOffset();

  llvm::BasicBlock* success_block = codegen_->CreateBasicBlock();
  if (object_can_be_null) {
    // __ Cbz(object, &success);
    llvm::Value* is_object_null =
        __ CreateICmpEQ(object, codegen_->GetConstantZero(object->getType()));
    codegen_->CreateBranchIfTrue(is_object_null, success_block);
  }

  // __ Ldr(temp, HeapOperand(object, class_offset.Int32Value()));
  llvm::Value* object_class = codegen_->CreateLoadWithOffset(
      codegen_->GetUncompressedGCPointerType(), object, class_offset.Int32Value());
  object_class = codegen_->MaybeUnpoisonHeapReference(object_class);

  llvm::BasicBlock* loop_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* before_loop_block = __ GetInsertBlock();
  // __ Bind(&loop);
  __ CreateBr(loop_block);
  __ SetInsertPoint(loop_block);
  llvm::PHINode* class_phi = __ CreatePHI(object_class->getType(), 2);
  class_phi->addIncoming(object_class, before_loop_block);

  // __ Cmp(type, temp);
  llvm::Value* is_class_equal = __ CreateICmpEQ(type, class_phi);
  // __ B(&success, eq);
  codegen_->CreateBranchIfTrue(is_class_equal, success_block);
  // __ Ldr(temp, HeapOperand(temp, super_class_offset.Int32Value()));
  llvm::Value* super_class = codegen_->CreateLoadWithOffset(
      class_phi->getType(), class_phi, super_class_offset.Int32Value());
  super_class = codegen_->MaybeUnpoisonHeapReference(super_class);
  // __ Cbz(temp, slow_path->GetEntryLabel());
  llvm::Value* is_super_class_null =
      __ CreateICmpEQ(super_class, codegen_->GetConstantZero(super_class->getType()));
  llvm::Instruction* br =
      codegen_->CreateBranchIfTrue(is_super_class_null, slow_path->GetEntryBlock());
  ExpectFalseBranch(br);
  // __ B(&loop);
  __ CreateBr(loop_block);
  class_phi->addIncoming(super_class, __ GetInsertBlock());

  // __ Bind(&success);
  __ SetInsertPoint(success_block);
}

// Check access mode and the primitive type from VarHandle.varType.
// Check reference arguments against the VarHandle.varType; for references this is a subclass
// check without read barrier, so it can have false negatives which we handle in the slow path.
static void GenerateVarHandleAccessModeAndVarTypeChecks(HInvoke* invoke,
                                                        CodeGeneratorARM64LLVM* codegen_,
                                                        SlowPathCodeARM64LLVM* slow_path,
                                                        DataType::Type type) {
  mirror::VarHandle::AccessMode access_mode =
      mirror::VarHandle::GetAccessModeByIntrinsic(invoke->GetIntrinsic());
  Primitive::Type primitive_type = DataTypeToPrimitive(type);

  llvm::Value* varhandle = codegen_->GetValue(invoke->InputAt(0));

  const MemberOffset var_type_offset = mirror::VarHandle::VarTypeOffset();
  const MemberOffset access_mode_bit_mask_offset = mirror::VarHandle::AccessModesBitMaskOffset();
  const MemberOffset primitive_type_offset = mirror::Class::PrimitiveTypeOffset();

  // Check that the operation is permitted and the primitive type of varhandle.varType.
  // We do not need a read barrier when loading a reference only for loading constant
  // primitive field through the reference. Use LDP to load the fields together.
  DCHECK_EQ(var_type_offset.Int32Value() + 4, access_mode_bit_mask_offset.Int32Value());
  // __ Ldp(var_type_no_rb, temp2, HeapOperand(varhandle, var_type_offset.Int32Value()));
  llvm::Value* var_type = codegen_->CreateLoadWithOffset(
      codegen_->GetUncompressedGCPointerType(), varhandle, var_type_offset.Int32Value());
  var_type = codegen_->MaybeUnpoisonHeapReference(var_type);
  llvm::Value* access_mode_bit_mask = codegen_->CreateLoadWithOffset(
      codegen_->GetUint32Type(), varhandle, access_mode_bit_mask_offset.Int32Value());

  {
    // __ Tbz(temp2, static_cast<uint32_t>(access_mode), slow_path->GetEntryLabel());
    llvm::Value* access_mode_bit =
        __ CreateAnd(access_mode_bit_mask, 1u << static_cast<uint32_t>(access_mode));
    llvm::Value* is_zero_access_mode =
        __ CreateICmpEQ(access_mode_bit, codegen_->GetConstantZero(access_mode_bit->getType()));
    llvm::Instruction* br =
        codegen_->CreateBranchIfTrue(is_zero_access_mode, slow_path->GetEntryBlock());
    ExpectFalseBranch(br);
  }

  // __ Ldrh(temp2, HeapOperand(var_type_no_rb, primitive_type_offset.Int32Value()));
  llvm::Value* primitive_type_value = codegen_->CreateLoadWithOffset(
      codegen_->GetUint16Type(), var_type, primitive_type_offset.Int32Value());
  // __ Cmp(temp2, static_cast<uint16_t>(primitive_type));
  llvm::Value* is_different_primitive_type =
      __ CreateICmpNE(primitive_type_value,
                      codegen_->GetConstantInt(primitive_type_value->getType(), primitive_type));
  // __ B(slow_path->GetEntryLabel(), ne);
  llvm::Instruction* br =
      codegen_->CreateBranchIfTrue(is_different_primitive_type, slow_path->GetEntryBlock());
  ExpectFalseBranch(br);

  if (type == DataType::Type::kReference) {
    // Check reference arguments against the varType.
    // False negatives due to varType being an interface or array type
    // or due to the missing read barrier are handled by the slow path.
    size_t expected_coordinates_count = GetExpectedVarHandleCoordinatesCount(invoke);
    uint32_t arguments_start = /* VarHandle object */ 1u + expected_coordinates_count;
    uint32_t number_of_arguments = invoke->GetNumberOfArguments();
    for (size_t arg_index = arguments_start; arg_index != number_of_arguments; ++arg_index) {
      HInstruction* arg = invoke->InputAt(arg_index);
      DCHECK_EQ(arg->GetType(), DataType::Type::kReference);
      if (!arg->IsNullConstant()) {
        llvm::Value* arg_value = codegen_->GetValue(invoke->InputAt(arg_index));
        GenerateSubTypeObjectCheckNoReadBarrier(codegen_, slow_path, arg_value, var_type);
      }
    }
  }
}

static void GenerateVarHandleStaticFieldCheck(HInvoke* invoke,
                                              CodeGeneratorARM64LLVM* codegen_,
                                              SlowPathCodeARM64LLVM* slow_path) {
  llvm::Value* varhandle = codegen_->GetValue(invoke->InputAt(0));

  const MemberOffset coordinate_type0_offset = mirror::VarHandle::CoordinateType0Offset();

  // Check that the VarHandle references a static field by checking that coordinateType0 == null.
  // Do not emit read barrier (or unpoison the reference) for comparing to null.
  // __ Ldr(temp, HeapOperand(varhandle, coordinate_type0_offset.Int32Value()));
  llvm::Value* coordinate_type0 = codegen_->CreateLoadWithOffset(
      codegen_->GetUncompressedGCPointerType(), varhandle, coordinate_type0_offset.Int32Value());
  // __ Cbnz(temp, slow_path->GetEntryLabel());
  llvm::Value* is_coordinate_type_null =
      __ CreateICmpEQ(coordinate_type0, codegen_->GetConstantZero(coordinate_type0->getType()));
  llvm::Instruction* br =
      codegen_->CreateBranchIfFalse(is_coordinate_type_null, slow_path->GetEntryBlock());
  ExpectTrueBranch(br);
}

static void GenerateVarHandleInstanceFieldChecks(HInvoke* invoke,
                                                 CodeGeneratorARM64LLVM* codegen_,
                                                 SlowPathCodeARM64LLVM* slow_path) {
  VarHandleOptimizations optimizations(invoke);
  llvm::Value* varhandle = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* object = codegen_->GetValue(invoke->InputAt(1));

  const MemberOffset coordinate_type0_offset = mirror::VarHandle::CoordinateType0Offset();
  const MemberOffset coordinate_type1_offset = mirror::VarHandle::CoordinateType1Offset();

  // Null-check the object.
  if (!optimizations.GetSkipObjectNullCheck()) {
    // __ Cbz(object, slow_path->GetEntryLabel());
    llvm::Value* is_object_null =
        __ CreateICmpEQ(object, codegen_->GetConstantZero(object->getType()));
    llvm::Instruction* br =
        codegen_->CreateBranchIfTrue(is_object_null, slow_path->GetEntryBlock());
    ExpectFalseBranch(br);
  }

  if (!optimizations.GetUseKnownImageVarHandle()) {
    // Check that the VarHandle references an instance field by checking that
    // coordinateType1 == null. coordinateType0 should not be null, but this is handled by the
    // type compatibility check with the source object's type, which will fail for null.
    // __ Ldp(temp, temp2, HeapOperand(varhandle, coordinate_type0_offset.Int32Value()));
    llvm::Value* coordinate_type0 = codegen_->CreateLoadWithOffset(
        codegen_->GetUncompressedGCPointerType(), varhandle, coordinate_type0_offset.Int32Value());
    coordinate_type0 = codegen_->MaybeUnpoisonHeapReference(coordinate_type0);
    llvm::Value* coordinate_type1 = codegen_->CreateLoadWithOffset(
        codegen_->GetUncompressedGCPointerType(), varhandle, coordinate_type1_offset.Int32Value());
    // No need for read barrier or unpoisoning of coordinateType1 for comparison with null.
    {
      // __ Cbnz(temp2, slow_path->GetEntryLabel());
      llvm::Value* is_coordinate_type1_null =
          __ CreateICmpEQ(coordinate_type1, codegen_->GetConstantZero(coordinate_type1->getType()));
      llvm::Instruction* br =
          codegen_->CreateBranchIfFalse(is_coordinate_type1_null, slow_path->GetEntryBlock());
      ExpectTrueBranch(br);
    }

    // Check that the object has the correct type.
    // We deliberately avoid the read barrier, letting the slow path handle the false negatives.
    GenerateSubTypeObjectCheckNoReadBarrier(
        codegen_, slow_path, object, coordinate_type0, /* object_can_be_null= */ false);
  }
}

static void GenerateVarHandleArrayChecks(HInvoke* invoke,
                                         CodeGeneratorARM64LLVM* codegen_,
                                         VarHandleSlowPathARM64LLVM* slow_path) {
  VarHandleOptimizations optimizations(invoke);
  llvm::Value* varhandle = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* object = codegen_->GetValue(invoke->InputAt(1));
  llvm::Value* index = codegen_->GetValue(invoke->InputAt(2));
  DataType::Type value_type =
      GetVarHandleExpectedValueType(invoke, /* expected_coordinates_count= */ 2u);
  Primitive::Type primitive_type = DataTypeToPrimitive(value_type);

  const MemberOffset coordinate_type0_offset = mirror::VarHandle::CoordinateType0Offset();
  const MemberOffset coordinate_type1_offset = mirror::VarHandle::CoordinateType1Offset();
  const MemberOffset component_type_offset = mirror::Class::ComponentTypeOffset();
  const MemberOffset primitive_type_offset = mirror::Class::PrimitiveTypeOffset();
  const MemberOffset class_offset = mirror::Object::ClassOffset();
  const MemberOffset array_length_offset = mirror::Array::LengthOffset();

  // Null-check the object.
  if (!optimizations.GetSkipObjectNullCheck()) {
    // __ Cbz(object, slow_path->GetEntryLabel());
    llvm::Value* is_object_null =
        __ CreateICmpEQ(object, codegen_->GetConstantZero(object->getType()));
    llvm::Instruction* br =
        codegen_->CreateBranchIfTrue(is_object_null, slow_path->GetEntryBlock());
    ExpectFalseBranch(br);
  }

  // Check that the VarHandle references an array, byte array view or ByteBuffer by checking
  // that coordinateType1 != null. If that's true, coordinateType1 shall be int.class and
  // coordinateType0 shall not be null but we do not explicitly verify that.
  // __ Ldp(temp, temp2, HeapOperand(varhandle, coordinate_type0_offset.Int32Value()));
  llvm::Value* coordinate_type0 = codegen_->CreateLoadWithOffset(
      codegen_->GetUncompressedGCPointerType(), varhandle, coordinate_type0_offset.Int32Value());
  coordinate_type0 = codegen_->MaybeUnpoisonHeapReference(coordinate_type0);
  llvm::Value* coordinate_type1 = codegen_->CreateLoadWithOffset(
      codegen_->GetUncompressedGCPointerType(), varhandle, coordinate_type1_offset.Int32Value());
  // No need for read barrier or unpoisoning of coordinateType1 for comparison with null.
  {
    // __ Cbz(temp2, slow_path->GetEntryLabel());
    llvm::Value* is_coordinate_type1_null =
        __ CreateICmpEQ(coordinate_type1, codegen_->GetConstantZero(coordinate_type1->getType()));
    llvm::Instruction* br =
        codegen_->CreateBranchIfTrue(is_coordinate_type1_null, slow_path->GetEntryBlock());
    ExpectFalseBranch(br);
  }

  // Check object class against componentType0.
  //
  // This is an exact check and we defer other cases to the runtime. This includes
  // conversion to array of superclass references, which is valid but subsequently
  // requires all update operations to check that the value can indeed be stored.
  // We do not want to perform such extra checks in the intrinsified code.
  //
  // We do this check without read barrier, so there can be false negatives which we
  // defer to the slow path. There shall be no false negatives for array classes in the
  // boot image (including Object[] and primitive arrays) because they are non-movable.
  // __ Ldr(temp2, HeapOperand(object, class_offset.Int32Value()));
  llvm::Value* object_class = codegen_->CreateLoadWithOffset(
      codegen_->GetUncompressedGCPointerType(), object, class_offset.Int32Value());
  object_class = codegen_->MaybeUnpoisonHeapReference(object_class);
  // __ Cmp(temp, temp2);
  llvm::Value* is_different_type = __ CreateICmpNE(coordinate_type0, object_class);
  {
    // __ B(slow_path->GetEntryLabel(), ne);
    llvm::Instruction* br =
        codegen_->CreateBranchIfTrue(is_different_type, slow_path->GetEntryBlock());
    ExpectFalseBranch(br);
  }

  // Check that the coordinateType0 is an array type. We do not need a read barrier
  // for loading constant reference fields (or chains of them) for comparison with null,
  // nor for finally loading a constant primitive field (primitive type) below.
  // __ Ldr(temp2, HeapOperand(temp, component_type_offset.Int32Value()));
  llvm::Value* coordinate_type0_component_type =
      codegen_->CreateLoadWithOffset(codegen_->GetUncompressedGCPointerType(),
                                     coordinate_type0,
                                     component_type_offset.Int32Value());
  coordinate_type0_component_type =
      codegen_->MaybeUnpoisonHeapReference(coordinate_type0_component_type);
  {
    // __ Cbz(temp2, slow_path->GetEntryLabel());
    llvm::Value* is_coordinate_type0_component_type_null =
        __ CreateICmpEQ(coordinate_type0_component_type,
                        codegen_->GetConstantZero(coordinate_type0_component_type->getType()));
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_coordinate_type0_component_type_null,
                                                         slow_path->GetEntryBlock());
    ExpectFalseBranch(br);
  }

  // Check that the array component type matches the primitive type.
  // __ Ldrh(temp2, HeapOperand(temp2, primitive_type_offset.Int32Value()));
  llvm::Value* coordinate_type0_primitive_type =
      codegen_->CreateLoadWithOffset(codegen_->GetUint16Type(),
                                     coordinate_type0_component_type,
                                     primitive_type_offset.Int32Value());
  llvm::BasicBlock* slow_path_block = slow_path->GetEntryBlock();
  if (primitive_type != Primitive::kPrimNot) {
    // With the exception of `kPrimNot` (handled above), `kPrimByte` and `kPrimBoolean`,
    // we shall check for a byte array view in the slow path.
    // The check requires the ByteArrayViewVarHandle.class to be in the boot image,
    // so we cannot emit that if we're JITting without boot image.
    bool boot_image_available = codegen_->GetCompilerOptions().IsBootImage() ||
                                !Runtime::Current()->GetHeap()->GetBootImageSpaces().empty();
    bool can_be_view = (DataType::Size(value_type) != 1u) && boot_image_available;
    if (can_be_view) {
      slow_path_block = codegen_->CreateBasicBlock();
      slow_path->SetByteArrayViewCheckBlock(slow_path_block);
    }
  }

  // __ Cmp(temp2, static_cast<uint16_t>(primitive_type));
  llvm::Value* is_different_primitive_type = __ CreateICmpNE(
      coordinate_type0_primitive_type,
      codegen_->GetConstantInt(coordinate_type0_primitive_type->getType(), primitive_type));
  {
    // __ B(slow_path_label, ne);
    llvm::Instruction* br =
        codegen_->CreateBranchIfTrue(is_different_primitive_type, slow_path_block);
    ExpectFalseBranch(br);
  }

  // Check for array index out of bounds.
  // __ Ldr(temp, HeapOperand(object, array_length_offset.Int32Value()));
  llvm::Value* array_length = codegen_->CreateLoadWithOffset(
      codegen_->GetInt32Type(), object, array_length_offset.Int32Value());
  // __ Cmp(index, temp);
  llvm::Value* is_out_of_bounds = __ CreateICmpUGE(index, array_length);
  // __ B(slow_path->GetEntryLabel(), hs);
  llvm::Instruction* br =
      codegen_->CreateBranchIfTrue(is_out_of_bounds, slow_path->GetEntryBlock());
  ExpectFalseBranch(br);
}

static void GenerateVarHandleCoordinateChecks(HInvoke* invoke,
                                              CodeGeneratorARM64LLVM* codegen_,
                                              VarHandleSlowPathARM64LLVM* slow_path) {
  size_t expected_coordinates_count = GetExpectedVarHandleCoordinatesCount(invoke);
  if (expected_coordinates_count == 0u) {
    GenerateVarHandleStaticFieldCheck(invoke, codegen_, slow_path);
  } else if (expected_coordinates_count == 1u) {
    GenerateVarHandleInstanceFieldChecks(invoke, codegen_, slow_path);
  } else {
    DCHECK_EQ(expected_coordinates_count, 2u);
    GenerateVarHandleArrayChecks(invoke, codegen_, slow_path);
  }
}

static VarHandleSlowPathARM64LLVM* GenerateVarHandleChecks(HInvoke* invoke,
                                                           CodeGeneratorARM64LLVM* codegen_,
                                                           llvm::AtomicOrdering order,
                                                           DataType::Type type) {
  size_t expected_coordinates_count = GetExpectedVarHandleCoordinatesCount(invoke);
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetUseKnownImageVarHandle()) {
    DCHECK_NE(expected_coordinates_count, 2u);
    if (expected_coordinates_count == 0u || optimizations.GetSkipObjectNullCheck()) {
      return nullptr;
    }
  }

  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* slow_path_exit_block = codegen_->CreateBasicBlock();
  llvm::PHINode* result_phi = nullptr;
  if (invoke->GetType() != DataType::Type::kVoid) {
    llvm::BasicBlock* current_block = __ GetInsertBlock();
    __ SetInsertPoint(slow_path_exit_block);
    result_phi = __ CreatePHI(codegen_->GetLLVMType(invoke->GetType()), 2);
    __ SetInsertPoint(current_block);
  }
  VarHandleSlowPathARM64LLVM* slow_path =
      new (codegen_->GetScopedAllocator()) VarHandleSlowPathARM64LLVM(
          invoke, slow_path_entry_block, slow_path_exit_block, result_phi, order);
  codegen_->AddSlowPath(slow_path);

  if (!optimizations.GetUseKnownImageVarHandle()) {
    GenerateVarHandleAccessModeAndVarTypeChecks(invoke, codegen_, slow_path, type);
  }
  GenerateVarHandleCoordinateChecks(invoke, codegen_, slow_path);

  return slow_path;
}

struct VarHandleTarget {
  llvm::Value* object;
  llvm::Value* offset;
};

static VarHandleTarget GetVarHandleTarget(HInvoke* invoke, CodeGeneratorARM64LLVM* codegen_) {
  size_t expected_coordinates_count = GetExpectedVarHandleCoordinatesCount(invoke);

  VarHandleTarget target{.object = nullptr, .offset = nullptr};
  if (expected_coordinates_count != 0) {
    target.object = codegen_->GetValue(invoke->InputAt(1));
  }
  return target;
}

static void GenerateVarHandleTarget(HInvoke* invoke,
                                    VarHandleTarget& target,
                                    CodeGeneratorARM64LLVM* codegen_) {
  llvm::Value* varhandle = codegen_->GetValue(invoke->InputAt(0));
  size_t expected_coordinates_count = GetExpectedVarHandleCoordinatesCount(invoke);

  if (expected_coordinates_count <= 1u) {
    if (VarHandleOptimizations(invoke).GetUseKnownImageVarHandle()) {
      // Do we need to do this at all? This looks like JIT code generation.
      ScopedObjectAccess soa(Thread::Current());
      ArtField* target_field = GetImageVarHandleField(invoke);
      if (expected_coordinates_count == 0u) {
        ObjPtr<mirror::Class> declaring_class = target_field->GetDeclaringClass();
        if (Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(declaring_class)) {
          uint32_t boot_image_offset = CodeGenerator::GetBootImageOffset(declaring_class);
          target.object = codegen_->NewClassBootImageRelRoPatch(boot_image_offset);
        } else {
          target.object = codegen_->NewBootImageTypePatch(declaring_class->GetDexFile(),
                                                          declaring_class->GetDexTypeIndex());
        }
      }
      uint32_t field_offset = target_field->GetOffset().Uint32Value();
      target.offset = codegen_->GetConstantInt(codegen_->GetUint32Type(), field_offset);
    } else {
      const MemberOffset art_field_offset = mirror::FieldVarHandle::ArtFieldOffset();
      const MemberOffset offset_offset = ArtField::OffsetOffset();

      // Load the ArtField*, the offset and, if needed, declaring class.
      // __ Ldr(field.X(), HeapOperand(varhandle, art_field_offset.Int32Value()));
      llvm::Value* field = codegen_->CreateLoadWithOffset(
          codegen_->GetPointerType(), varhandle, art_field_offset.Int32Value());
      // __ Ldr(target.offset, MemOperand(field.X(), offset_offset.Int32Value()));
      target.offset = codegen_->CreateLoadWithOffset(
          codegen_->GetInt32Type(), field, offset_offset.Int32Value());
      if (expected_coordinates_count == 0u) {
        target.object =
            codegen_->CreateLoadWithOffset(codegen_->GetUncompressedGCPointerType(),
                                           field,
                                           ArtField::DeclaringClassOffset().Int32Value());
        codegen_->GenerateGcRootFieldLoad(
            invoke, target.object, codegen_->GetCompilerReadBarrierOption());
      }
    }
  } else {
    DCHECK_EQ(expected_coordinates_count, 2u);
    DataType::Type value_type =
        GetVarHandleExpectedValueType(invoke, /* expected_coordinates_count= */ 2u);
    size_t size = DataType::Size(value_type);
    MemberOffset data_offset = mirror::Array::DataOffset(DataType::Size(value_type));

    llvm::Value* index = codegen_->GetValue(invoke->InputAt(2), DataType::Type::kInt32);
    llvm::Value* shifted_index = index;
    if (size != 1) {
      // __ Lsl(shifted_index, index, size_shift);
      shifted_index = __ CreateMul(index, __ getInt32(size));
    }
    // __ Add(target.offset, shifted_index, data_offset.Int32Value());
    target.offset = __ CreateAdd(shifted_index, __ getInt32(data_offset.Int32Value()));
  }
}

static llvm::Value* GenerateVarHandleGet(
    HInvoke* invoke,
    CodeGeneratorARM64LLVM* codegen_,
    llvm::AtomicOrdering order,
    bool byte_swap = false,
    std::optional<VarHandleTarget> byte_array_target = std::nullopt) {
  DataType::Type type = invoke->GetType();
  DCHECK_NE(type, DataType::Type::kVoid);

  VarHandleSlowPathARM64LLVM* slow_path = nullptr;
  VarHandleTarget target =
      byte_array_target.has_value() ? *byte_array_target : GetVarHandleTarget(invoke, codegen_);
  if (!byte_swap) {
    slow_path = GenerateVarHandleChecks(invoke, codegen_, order, type);
    GenerateVarHandleTarget(invoke, target, codegen_);
    if (slow_path != nullptr) {
      // __ Bind(slow_path->GetNativeByteOrderLabel());
      llvm::BasicBlock* native_byte_order_block = codegen_->CreateBasicBlock();
      __ CreateBr(native_byte_order_block);
      llvm::BasicBlock* before_native_byte_order_block = __ GetInsertBlock();
      __ SetInsertPoint(native_byte_order_block);
      llvm::PHINode* offset_phi = __ CreatePHI(target.offset->getType(), 2);
      offset_phi->addIncoming(target.offset, before_native_byte_order_block);
      target.offset = offset_phi;
      slow_path->SetNativeByteOrderBlock(native_byte_order_block, offset_phi);
    }
  }
  DCHECK(target.object != nullptr);
  DCHECK(target.offset != nullptr);

  DCHECK(!codegen_->EmitBakerReadBarrier());
  // Load the value from the target location.
  llvm::Value* address = __ CreatePtrAdd(target.object, target.offset);
  llvm::Value* result = codegen_->CreateLoad(codegen_->GetLLVMType(type), address, order);

  if (type == DataType::Type::kReference) {
    DCHECK(!byte_swap);
    result =
        codegen_->MaybeGenerateReadBarrierSlow(invoke, result, target.object, 0u, target.offset);
  } else if (byte_swap) {
    bool is_fp = DataType::IsFloatingPointType(type);
    llvm::Type* result_type = result->getType();
    llvm::Type* result_int_type =
        type == DataType::Type::kFloat32 ? codegen_->GetInt32Type() : codegen_->GetInt64Type();
    if (is_fp) {
      result = __ CreateBitCast(result, result_int_type);
    }
    result = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, result);
    if (is_fp) {
      result = __ CreateBitCast(result, result_type);
    }
  }

  if (slow_path != nullptr) {
    DCHECK(!byte_swap);
    // __ Bind(slow_path->GetExitLabel());
    __ CreateBr(slow_path->GetExitBlock());
    if (llvm::PHINode* result_phi = slow_path->GetResultPhi()) {
      result_phi->addIncoming(result, __ GetInsertBlock());
      result = result_phi;
    }
    __ SetInsertPoint(slow_path->GetExitBlock());
  }

  return result;
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGet(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  // NOTE: llvm::AtomicOrdering::Unordered models Java's non-volatile shared access. Ultimately it
  // will create a simple `ldr` instruction.
  llvm::Value* result = GenerateVarHandleGet(invoke, codegen_, llvm::AtomicOrdering::Unordered);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetOpaque(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGet(invoke, codegen_, llvm::AtomicOrdering::Monotonic);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAcquire(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGet(invoke, codegen_, llvm::AtomicOrdering::Acquire);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetVolatile(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result =
      GenerateVarHandleGet(invoke, codegen_, llvm::AtomicOrdering::SequentiallyConsistent);
  AddValue(invoke, result);
}

static void GenerateVarHandleSet(HInvoke* invoke,
                                 CodeGeneratorARM64LLVM* codegen_,
                                 llvm::AtomicOrdering order,
                                 bool byte_swap = false,
                                 std::optional<VarHandleTarget> byte_array_target = std::nullopt) {
  uint32_t value_index = invoke->GetNumberOfArguments() - 1;
  DataType::Type value_type = GetDataTypeFromShorty(invoke, value_index);

  llvm::Value* value = codegen_->GetValue(invoke->InputAt(value_index), value_type);

  VarHandleSlowPathARM64LLVM* slow_path = nullptr;
  VarHandleTarget target =
      byte_array_target.has_value() ? *byte_array_target : GetVarHandleTarget(invoke, codegen_);
  if (!byte_swap) {
    slow_path = GenerateVarHandleChecks(invoke, codegen_, order, value_type);
    GenerateVarHandleTarget(invoke, target, codegen_);
    if (slow_path != nullptr) {
      // __ Bind(slow_path->GetNativeByteOrderLabel());
      llvm::BasicBlock* native_byte_order_block = codegen_->CreateBasicBlock();
      __ CreateBr(native_byte_order_block);
      llvm::BasicBlock* before_native_byte_order_block = __ GetInsertBlock();
      __ SetInsertPoint(native_byte_order_block);
      llvm::PHINode* offset_phi = __ CreatePHI(target.offset->getType(), 2);
      offset_phi->addIncoming(target.offset, before_native_byte_order_block);
      target.offset = offset_phi;
      slow_path->SetNativeByteOrderBlock(native_byte_order_block, offset_phi);
    }
  }
  DCHECK(target.object != nullptr);
  DCHECK(target.offset != nullptr);

  // Store the value to the target location.
  llvm::Value* source = value;
  if (kPoisonHeapReferences && value_type == DataType::Type::kReference) {
    source = codegen_->PoisonHeapReference(source);
  }
  if (byte_swap) {
    bool is_fp = DataType::IsFloatingPointType(value_type);
    llvm::Type* source_type = source->getType();
    llvm::Type* source_int_type = value_type == DataType::Type::kFloat32 ? codegen_->GetInt32Type()
                                                                         : codegen_->GetInt64Type();
    if (is_fp) {
      source = __ CreateBitCast(source, source_int_type);
    }
    source = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, source);
    if (is_fp) {
      source = __ CreateBitCast(source, source_type);
    }
  }
  llvm::Value* address = __ CreatePtrAdd(target.object, target.offset);
  codegen_->CreateStore(source, address, order);

  if (CodeGenerator::StoreNeedsWriteBarrier(value_type, invoke->InputAt(value_index))) {
    codegen_->MaybeMarkGCCard(target.object, value, /* emit_null_check= */ true);
  }

  if (slow_path != nullptr) {
    DCHECK(!byte_swap);
    // __ Bind(slow_path->GetExitLabel());
    __ CreateBr(slow_path->GetExitBlock());
    __ SetInsertPoint(slow_path->GetExitBlock());
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleSet(HInvoke* invoke) {
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  GenerateVarHandleSet(invoke, codegen_, llvm::AtomicOrdering::Unordered);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleSetOpaque(HInvoke* invoke) {
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  GenerateVarHandleSet(invoke, codegen_, llvm::AtomicOrdering::Monotonic);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleSetRelease(HInvoke* invoke) {
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  GenerateVarHandleSet(invoke, codegen_, llvm::AtomicOrdering::Release);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleSetVolatile(HInvoke* invoke) {
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  GenerateVarHandleSet(invoke, codegen_, llvm::AtomicOrdering::SequentiallyConsistent);
}

static llvm::Value* GenerateVarHandleCompareAndSetOrExchange(
    HInvoke* invoke,
    CodeGeneratorARM64LLVM* codegen_,
    llvm::AtomicOrdering order,
    bool return_success,
    bool strong,
    bool byte_swap = false,
    std::optional<VarHandleTarget> byte_array_target = std::nullopt) {
  DCHECK(return_success || strong);

  uint32_t expected_index = invoke->GetNumberOfArguments() - 2;
  uint32_t new_value_index = invoke->GetNumberOfArguments() - 1;
  DataType::Type value_type = GetDataTypeFromShorty(invoke, new_value_index);
  DCHECK_EQ(value_type, GetDataTypeFromShorty(invoke, expected_index));

  llvm::Value* expected = codegen_->GetValue(invoke->InputAt(expected_index), value_type);
  llvm::Value* new_value = codegen_->GetValue(invoke->InputAt(new_value_index), value_type);
  llvm::Type* llvm_value_type = expected->getType();

  VarHandleSlowPathARM64LLVM* slow_path = nullptr;
  VarHandleTarget target =
      byte_array_target.has_value() ? *byte_array_target : GetVarHandleTarget(invoke, codegen_);
  if (!byte_swap) {
    slow_path = GenerateVarHandleChecks(invoke, codegen_, order, value_type);
    GenerateVarHandleTarget(invoke, target, codegen_);
    if (slow_path != nullptr) {
      slow_path->SetCompareAndSetOrExchangeArgs(return_success, strong);
      // __ Bind(slow_path->GetNativeByteOrderLabel());
      llvm::BasicBlock* native_byte_order_block = codegen_->CreateBasicBlock();
      __ CreateBr(native_byte_order_block);
      llvm::BasicBlock* before_native_byte_order_block = __ GetInsertBlock();
      __ SetInsertPoint(native_byte_order_block);
      llvm::PHINode* offset_phi = __ CreatePHI(target.offset->getType(), 2);
      offset_phi->addIncoming(target.offset, before_native_byte_order_block);
      target.offset = offset_phi;
      slow_path->SetNativeByteOrderBlock(native_byte_order_block, offset_phi);
    }
  }
  DCHECK(target.object != nullptr);
  DCHECK(target.offset != nullptr);

  // This needs to be before the temp registers, as MarkGCCard also uses VIXL temps.
  if (CodeGenerator::StoreNeedsWriteBarrier(value_type, invoke->InputAt(new_value_index))) {
    // Mark card for object assuming new value is stored.
    bool new_value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen_->MaybeMarkGCCard(target.object, new_value, new_value_can_be_null);
  }

  // __ Add(tmp_ptr, target.object.X(), target.offset.X());
  llvm::Value* address = __ CreatePtrAdd(target.object, target.offset);

  // Move floating point values to scratch registers.
  // Note that float/double CAS uses bitwise comparison, rather than the operator==.
  llvm::Value* expected_int_value = expected;
  llvm::Value* new_value_int_value = new_value;
  bool is_fp = DataType::IsFloatingPointType(value_type);
  bool is_bool = value_type == DataType::Type::kBool;
  if (is_fp) {
    llvm::Type* int_type = value_type == DataType::Type::kFloat32 ? codegen_->GetInt32Type()
                                                                  : codegen_->GetInt64Type();
    expected_int_value = __ CreateBitCast(expected, int_type);
    new_value_int_value = __ CreateBitCast(new_value, int_type);
  } else if (is_bool) {
    expected_int_value = __ CreateZExt(expected, codegen_->GetUint8Type());
    new_value_int_value = __ CreateZExt(new_value, codegen_->GetUint8Type());
  }

  if (byte_swap) {
    expected_int_value = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, expected_int_value);
    new_value_int_value = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, new_value_int_value);
  }

  DCHECK(!codegen_->EmitReadBarrier());

  llvm::Align align = codegen_->GetModule()->getDataLayout().getABITypeAlign(llvm_value_type);
  DCHECK(order == llvm::AtomicOrdering::Acquire || order == llvm::AtomicOrdering::Release ||
         order == llvm::AtomicOrdering::SequentiallyConsistent ||
         order == llvm::AtomicOrdering::Monotonic);
  llvm::AtomicOrdering success_order = order;
  llvm::AtomicOrdering failure_order =
      order == llvm::AtomicOrdering::Release ? llvm::AtomicOrdering::Monotonic : order;
  auto [loaded_value, success] = codegen_->CreateAtomicCmpXchg(address,
                                                               expected_int_value,
                                                               new_value_int_value,
                                                               align,
                                                               success_order,
                                                               failure_order,
                                                               strong);

  llvm::Value* result = nullptr;
  if (return_success) {
    result = success;
  } else {
    DCHECK(strong);
    // If it is a strong compare-exchange, then the returned values is the old value.
    result = loaded_value;
    if (byte_swap) {
      result = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, result);
    }

    if (is_fp) {
      result = __ CreateBitCast(result, llvm_value_type);
    } else if (is_bool) {
      result = __ CreateTrunc(result, llvm_value_type);
    }
  }

  if (slow_path != nullptr) {
    DCHECK(!byte_swap);
    // __ Bind(slow_path->GetExitLabel());
    __ CreateBr(slow_path->GetExitBlock());
    if (llvm::PHINode* result_phi = slow_path->GetResultPhi()) {
      result_phi->addIncoming(result, __ GetInsertBlock());
      result = result_phi;
    }
    __ SetInsertPoint(slow_path->GetExitBlock());
  }

  return result;
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleCompareAndExchange(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result =
      GenerateVarHandleCompareAndSetOrExchange(invoke,
                                               codegen_,
                                               llvm::AtomicOrdering::SequentiallyConsistent,
                                               /* return_success= */ false,
                                               /* strong= */ true);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleCompareAndExchangeAcquire(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleCompareAndSetOrExchange(invoke,
                                                                 codegen_,
                                                                 llvm::AtomicOrdering::Acquire,
                                                                 /* return_success= */ false,
                                                                 /* strong= */ true);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleCompareAndExchangeRelease(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleCompareAndSetOrExchange(invoke,
                                                                 codegen_,
                                                                 llvm::AtomicOrdering::Release,
                                                                 /* return_success= */ false,
                                                                 /* strong= */ true);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleCompareAndSet(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result =
      GenerateVarHandleCompareAndSetOrExchange(invoke,
                                               codegen_,
                                               llvm::AtomicOrdering::SequentiallyConsistent,
                                               /* return_success= */ true,
                                               /* strong= */ true);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleWeakCompareAndSet(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result =
      GenerateVarHandleCompareAndSetOrExchange(invoke,
                                               codegen_,
                                               llvm::AtomicOrdering::SequentiallyConsistent,
                                               /* return_success= */ true,
                                               /* strong= */ false);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleWeakCompareAndSetAcquire(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleCompareAndSetOrExchange(invoke,
                                                                 codegen_,
                                                                 llvm::AtomicOrdering::Acquire,
                                                                 /* return_success= */ true,
                                                                 /* strong= */ false);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleWeakCompareAndSetPlain(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleCompareAndSetOrExchange(invoke,
                                                                 codegen_,
                                                                 llvm::AtomicOrdering::Monotonic,
                                                                 /* return_success= */ true,
                                                                 /* strong= */ false);
  AddValue(invoke, result);
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleWeakCompareAndSetRelease(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleCompareAndSetOrExchange(invoke,
                                                                 codegen_,
                                                                 llvm::AtomicOrdering::Release,
                                                                 /* return_success= */ true,
                                                                 /* strong= */ false);
  AddValue(invoke, result);
}

static llvm::Value* GenerateVarHandleGetAndUpdate(
    HInvoke* invoke,
    CodeGeneratorARM64LLVM* codegen_,
    GetAndUpdateOp get_and_update_op,
    llvm::AtomicOrdering order,
    bool byte_swap = false,
    std::optional<VarHandleTarget> byte_array_target = std::nullopt) {
  uint32_t arg_index = invoke->GetNumberOfArguments() - 1;
  DataType::Type value_type = GetDataTypeFromShorty(invoke, arg_index);

  llvm::Value* arg = codegen_->GetValue(invoke->InputAt(arg_index), value_type);
  llvm::Type* llvm_value_type = arg->getType();

  VarHandleSlowPathARM64LLVM* slow_path = nullptr;
  VarHandleTarget target =
      byte_array_target.has_value() ? *byte_array_target : GetVarHandleTarget(invoke, codegen_);
  if (!byte_swap) {
    slow_path = GenerateVarHandleChecks(invoke, codegen_, order, value_type);
    GenerateVarHandleTarget(invoke, target, codegen_);
    if (slow_path != nullptr) {
      slow_path->SetGetAndUpdateOp(get_and_update_op);
      // __ Bind(slow_path->GetNativeByteOrderLabel());
      llvm::BasicBlock* native_byte_order_block = codegen_->CreateBasicBlock();
      __ CreateBr(native_byte_order_block);
      llvm::BasicBlock* before_native_byte_order_block = __ GetInsertBlock();
      __ SetInsertPoint(native_byte_order_block);
      llvm::PHINode* offset_phi = __ CreatePHI(target.offset->getType(), 2);
      offset_phi->addIncoming(target.offset, before_native_byte_order_block);
      target.offset = offset_phi;
      slow_path->SetNativeByteOrderBlock(native_byte_order_block, offset_phi);
    }
  }
  DCHECK(target.object != nullptr);
  DCHECK(target.offset != nullptr);

  // This needs to be before the temp registers, as MarkGCCard also uses VIXL temps.
  if (CodeGenerator::StoreNeedsWriteBarrier(value_type, invoke->InputAt(arg_index))) {
    DCHECK(get_and_update_op == GetAndUpdateOp::kSet);
    // Mark card for object, the new value shall be stored.
    bool new_value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen_->MaybeMarkGCCard(target.object, arg, new_value_can_be_null);
  }

  // __ Add(tmp_ptr, target.object.X(), target.offset.X());
  llvm::Value* address = __ CreatePtrAdd(target.object, target.offset);

  if (byte_swap) {
    DCHECK_NE(value_type, DataType::Type::kReference);
    DCHECK_NE(DataType::Size(value_type), 1u);
    if (get_and_update_op == GetAndUpdateOp::kAdd) {
      // We need to do the byte swapping in the CAS loop for GetAndAdd.
      get_and_update_op = GetAndUpdateOp::kAddWithByteSwap;
    } else {
      // For other operations, avoid byte swap inside the CAS loop by providing an adjusted `arg`.
      // For GetAndSet use a scratch register; FP argument is already in a scratch register.
      // For bitwise operations GenerateGetAndUpdate() needs both scratch registers;
      // we have allocated a normal temporary to handle that.
      bool is_fp = DataType::IsFloatingPointType(value_type);
      llvm::Type* arg_int_type = value_type == DataType::Type::kFloat32 ? codegen_->GetInt32Type()
                                                                        : codegen_->GetInt64Type();
      if (is_fp) {
        arg = __ CreateBitCast(arg, arg_int_type);
      }
      arg = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, arg);
      if (is_fp) {
        arg = __ CreateBitCast(arg, llvm_value_type);
      }
    }
  }

  llvm::Value* result = GenerateGetAndUpdate(codegen_, get_and_update_op, order, address, arg);

  DCHECK(!codegen_->EmitReadBarrier());
  if (byte_swap && get_and_update_op != GetAndUpdateOp::kAddWithByteSwap) {
    bool is_fp = DataType::IsFloatingPointType(value_type);
    llvm::Type* result_int_type = value_type == DataType::Type::kFloat32 ? codegen_->GetInt32Type()
                                                                         : codegen_->GetInt64Type();
    if (is_fp) {
      result = __ CreateBitCast(result, result_int_type);
    }
    result = __ CreateUnaryIntrinsic(llvm::Intrinsic::bswap, result);
    if (is_fp) {
      result = __ CreateBitCast(result, llvm_value_type);
    }
  }

  if (slow_path != nullptr) {
    DCHECK(!byte_swap);
    // __ Bind(slow_path->GetExitLabel());
    __ CreateBr(slow_path->GetExitBlock());
    if (llvm::PHINode* result_phi = slow_path->GetResultPhi()) {
      result_phi->addIncoming(result, __ GetInsertBlock());
      result = result_phi;
    }
    __ SetInsertPoint(slow_path->GetExitBlock());
  }

  return result;
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndSet(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kSet, llvm::AtomicOrdering::SequentiallyConsistent);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndSetAcquire(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kSet, llvm::AtomicOrdering::Acquire);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndSetRelease(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kSet, llvm::AtomicOrdering::Release);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndAdd(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kAdd, llvm::AtomicOrdering::SequentiallyConsistent);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndAddAcquire(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kAdd, llvm::AtomicOrdering::Acquire);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndAddRelease(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kAdd, llvm::AtomicOrdering::Release);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndBitwiseAnd(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kAnd, llvm::AtomicOrdering::SequentiallyConsistent);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndBitwiseAndAcquire(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kAnd, llvm::AtomicOrdering::Acquire);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndBitwiseAndRelease(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kAnd, llvm::AtomicOrdering::Release);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndBitwiseOr(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kOr, llvm::AtomicOrdering::SequentiallyConsistent);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndBitwiseOrAcquire(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kOr, llvm::AtomicOrdering::Acquire);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndBitwiseOrRelease(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kOr, llvm::AtomicOrdering::Release);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndBitwiseXor(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kXor, llvm::AtomicOrdering::SequentiallyConsistent);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndBitwiseXorAcquire(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kXor, llvm::AtomicOrdering::Acquire);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitVarHandleGetAndBitwiseXorRelease(HInvoke* invoke) {
  DCHECK(!codegen_->EmitNonBakerReadBarrier());
  VarHandleOptimizations optimizations(invoke);
  if (optimizations.GetDoNotIntrinsify()) {
    SetError();
    return;
  }

  llvm::Value* result = GenerateVarHandleGetAndUpdate(
      invoke, codegen_, GetAndUpdateOp::kXor, llvm::AtomicOrdering::Release);
  if (invoke->GetType() != DataType::Type::kVoid) {
    AddValue(invoke, result);
  }
}

void VarHandleSlowPathARM64LLVM::EmitByteArrayViewCode(CodeGenerator* codegen_base) {
  CodeGeneratorARM64LLVM* codegen_ = down_cast<CodeGeneratorARM64LLVM*>(codegen_base);
  codegen_->SetCurrentBlock(instruction_->GetBlock());

  HInvoke* invoke = GetInvoke();
  mirror::VarHandle::AccessModeTemplate access_mode_template = GetAccessModeTemplate();
  DataType::Type value_type =
      GetVarHandleExpectedValueType(invoke, /* expected_coordinates_count= */ 2u);
  DCHECK_NE(value_type, DataType::Type::kReference);
  size_t size = DataType::Size(value_type);
  DCHECK_GT(size, 1u);
  llvm::Value* varhandle = codegen_->GetValue(invoke->InputAt(0));
  llvm::Value* object = codegen_->GetValue(invoke->InputAt(1));
  llvm::Value* index = codegen_->GetValue(invoke->InputAt(2), DataType::Type::kInt32);

  MemberOffset class_offset = mirror::Object::ClassOffset();
  MemberOffset array_length_offset = mirror::Array::LengthOffset();
  MemberOffset data_offset = mirror::Array::DataOffset(Primitive::kPrimByte);
  MemberOffset native_byte_order_offset = mirror::ByteArrayViewVarHandle::NativeByteOrderOffset();

  // __ Bind(GetByteArrayViewCheckLabel());
  __ SetInsertPoint(GetByteArrayViewCheckBlock());

  VarHandleTarget target = GetVarHandleTarget(invoke, codegen_);
  // The main path checked that the coordinateType0 is an array class that matches
  // the class of the actual coordinate argument but it does not match the value type.
  // Check if the `varhandle` references a ByteArrayViewVarHandle instance.
  // __ Ldr(temp, HeapOperand(varhandle, class_offset.Int32Value()));
  llvm::Value* varhandle_class = codegen_->CreateLoadWithOffset(
      codegen_->GetUncompressedGCPointerType(), varhandle, class_offset.Int32Value());
  varhandle_class = codegen_->MaybeUnpoisonHeapReference(varhandle_class);
  llvm::Value* array_view_class =
      codegen_->LoadClassRootForIntrinsic(ClassRoot::kJavaLangInvokeByteArrayViewVarHandle);
  // __ Cmp(temp, temp2);
  llvm::Value* are_different_classes = __ CreateICmpNE(varhandle_class, array_view_class);
  {
    // __ B(GetEntryLabel(), ne);
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(are_different_classes, GetEntryBlock());
    ExpectFalseBranch(br);
  }

  // Check for array index out of bounds.
  // __ Ldr(temp, HeapOperand(object, array_length_offset.Int32Value()));
  llvm::Value* array_length = codegen_->CreateLoadWithOffset(
      codegen_->GetInt32Type(), object, array_length_offset.Int32Value());
  // __ Subs(temp, temp, index);
  // __ Ccmp(temp, size, NoFlag, hs);  // If SUBS yields LO (C=false), keep the C flag clear.
  llvm::Value* is_out_of_bounds = __ CreateICmpUGE(index, array_length);
  {
    // __ B(GetEntryLabel(), lo);
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_out_of_bounds, GetEntryBlock());
    ExpectFalseBranch(br);
  }

  // Construct the target.
  // __ Add(target.offset, index, data_offset.Int32Value());
  target.offset =
      __ CreateAdd(index, codegen_->GetConstantInt(index->getType(), data_offset.Int32Value()));

  // Alignment check. For unaligned access, go to the runtime.
  DCHECK(IsPowerOfTwo(size));
  // __ Tst(target.offset, size - 1u);
  llvm::Value* align_rem =
      __ CreateURem(target.offset, codegen_->GetConstantInt(target.offset->getType(), size));
  llvm::Value* is_unaligned =
      __ CreateICmpNE(align_rem, codegen_->GetConstantZero(align_rem->getType()));
  {
    // __ B(GetEntryLabel(), ne);
    llvm::Instruction* br = codegen_->CreateBranchIfTrue(is_unaligned, GetEntryBlock());
    ExpectFalseBranch(br);
  }

  // Byte order check. For native byte order return to the main path.
  // __ Ldr(temp, HeapOperand(varhandle, native_byte_order_offset.Int32Value()));
  llvm::Value* native_byte_order_flag = codegen_->CreateLoadWithOffset(
      codegen_->GetUint32Type(), varhandle, native_byte_order_offset.Int32Value());
  llvm::Value* is_native_byte_order = __ CreateICmpNE(
      native_byte_order_flag, codegen_->GetConstantZero(native_byte_order_flag->getType()));
  {
    // __ Cbnz(temp, GetNativeByteOrderLabel());
    llvm::BasicBlock* continue_block = codegen_->CreateBasicBlock();
    __ CreateCondBr(is_native_byte_order, GetNativeByteOrderBlock(), continue_block);
    GetNativeByteOrderOffsetPhi()->addIncoming(target.offset, __ GetInsertBlock());
    __ SetInsertPoint(continue_block);
  }

  llvm::Value* result = nullptr;
  switch (access_mode_template) {
    case mirror::VarHandle::AccessModeTemplate::kGet:
      result = GenerateVarHandleGet(invoke, codegen_, order_, /* byte_swap= */ true, target);
      break;
    case mirror::VarHandle::AccessModeTemplate::kSet:
      GenerateVarHandleSet(invoke, codegen_, order_, /* byte_swap= */ true, target);
      break;
    case mirror::VarHandle::AccessModeTemplate::kCompareAndSet:
    case mirror::VarHandle::AccessModeTemplate::kCompareAndExchange:
      result = GenerateVarHandleCompareAndSetOrExchange(
          invoke, codegen_, order_, return_success_, strong_, /* byte_swap= */ true, target);
      break;
    case mirror::VarHandle::AccessModeTemplate::kGetAndUpdate:
      result = GenerateVarHandleGetAndUpdate(
          invoke, codegen_, get_and_update_op_, order_, /* byte_swap= */ true, target);
      break;
  }
  // __ B(GetExitLabel());
  __ CreateBr(GetExitBlock());
  if (result != nullptr) {
    if (llvm::PHINode* result_phi = GetResultPhi()) {
      result_phi->addIncoming(result, __ GetInsertBlock());
    }
  }
}

void IntrinsicCodeGeneratorARM64LLVM::VisitMethodHandleInvokeExact(HInvoke* invoke) {
  llvm::Value* method_handle = GetValue(invoke->InputAt(0));

  llvm::BasicBlock* slow_path_entry_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* done_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* current_block = __ GetInsertBlock();
  llvm::Type* result_type = GetLLVMType(invoke->GetType());
  __ SetInsertPoint(done_block);
  llvm::PHINode* result_phi = result_type->isVoidTy() ? nullptr : __ CreatePHI(result_type, 3);
  __ SetInsertPoint(current_block);
  SlowPathCodeARM64LLVM* slow_path =
      new (codegen_->GetScopedAllocator()) InvokePolymorphicSlowPathARM64LLVM(
          invoke, slow_path_entry_block, done_block, result_phi, method_handle);
  codegen_->AddSlowPath(slow_path);

  llvm::Value* call_site_type = GetValue(invoke->InputAt(invoke->GetNumberOfArguments()));

  // Call site should match with MethodHandle's type.
  // __ Ldr(temp, HeapOperand(method_handle.W(), mirror::MethodHandle::MethodTypeOffset()));
  // codegen_->GetAssembler()->MaybeUnpoisonHeapReference(temp);
  llvm::Value* method_handle_type =
      CreateLoadWithOffset(GetUncompressedGCPointerType(),
                           method_handle,
                           mirror::MethodHandle::MethodTypeOffset().Int32Value());
  method_handle_type = codegen_->MaybeUnpoisonHeapReference(method_handle_type);
  // __ Cmp(call_site_type, temp);
  llvm::Value* is_equal_type = __ CreateICmpEQ(call_site_type, method_handle_type);
  // __ B(ne, slow_path->GetEntryLabel());
  llvm::Instruction* equal_type_br =
      codegen_->CreateBranchIfFalse(is_equal_type, slow_path->GetEntryBlock());
  ExpectTrueBranch(equal_type_br);

  // __ Ldr(method, HeapOperand(method_handle.W(), mirror::MethodHandle::ArtFieldOrMethodOffset()));
  llvm::Value* method =
      CreateLoadWithOffset(GetMethodPointerType(),
                           method_handle,
                           mirror::MethodHandle::ArtFieldOrMethodOffset().Int32Value());

  llvm::BasicBlock* execute_target_method_block = codegen_->CreateBasicBlock();
  llvm::BasicBlock* begin_block = __ GetInsertBlock();
  __ SetInsertPoint(execute_target_method_block);
  llvm::PHINode* execute_target_method_phi = __ CreatePHI(method->getType(), 6);
  __ SetInsertPoint(begin_block);

  llvm::BasicBlock* method_dispatch_block = codegen_->CreateBasicBlock();

  // __ Ldr(method_handle_kind, HeapOperand(method_handle.W(),
  //                                        mirror::MethodHandle::HandleKindOffset()));
  llvm::Value* method_handle_kind = CreateLoadWithOffset(
      GetInt32Type(), method_handle, mirror::MethodHandle::HandleKindOffset().Int32Value());

  // __ Cmp(method_handle_kind, Operand(mirror::MethodHandle::Kind::kFirstAccessorKind));
  llvm::Value* first_accessor_value =
      GetConstantInt(method_handle_kind->getType(), mirror::MethodHandle::Kind::kFirstAccessorKind);
  llvm::Value* is_accessor = __ CreateICmpSGE(method_handle_kind, first_accessor_value);
  // __ B(lt, &method_dispatch);
  codegen_->CreateBranchIfFalse(is_accessor, method_dispatch_block);
  // __ Ldr(method, HeapOperand(method_handle.W(), mirror::MethodHandleImpl::TargetOffset()));
  llvm::Value* accessor_method = CreateLoadWithOffset(
      GetMethodPointerType(), method_handle, mirror::MethodHandleImpl::TargetOffset().SizeValue());
  // __ B(&execute_target_method);
  execute_target_method_phi->addIncoming(accessor_method, __ GetInsertBlock());
  __ CreateBr(execute_target_method_block);

  // __ Bind(&method_dispatch);
  __ SetInsertPoint(method_dispatch_block);
  // __ Cmp(method_handle_kind, Operand(mirror::MethodHandle::Kind::kInvokeStatic));
  llvm::Value* is_invoke_static =
      __ CreateICmpEQ(method_handle_kind,
                      GetConstantInt(GetInt32Type(), mirror::MethodHandle::Kind::kInvokeStatic));
  // __ B(eq, &execute_target_method);
  execute_target_method_phi->addIncoming(method, __ GetInsertBlock());
  codegen_->CreateBranchIfTrue(is_invoke_static, execute_target_method_block);

  if (invoke->AsInvokePolymorphic()->CanTargetInstanceMethod()) {
    llvm::Value* receiver = GetValue(invoke->InputAt(1));

    // Receiver shouldn't be null for all the following cases.
    // __ Cbz(receiver, slow_path->GetEntryLabel());
    llvm::Value* is_receiver_null = __ CreateICmpEQ(receiver, GetConstantZero(receiver->getType()));
    llvm::Instruction* receiver_null_br =
        codegen_->CreateBranchIfTrue(is_receiver_null, slow_path->GetEntryBlock());
    ExpectFalseBranch(receiver_null_br);

    // __ Cmp(method_handle_kind, Operand(mirror::MethodHandle::Kind::kInvokeDirect));
    llvm::Value* is_invoke_direct =
        __ CreateICmpEQ(method_handle_kind,
                        GetConstantInt(GetInt32Type(), mirror::MethodHandle::Kind::kInvokeDirect));
    // No dispatch is needed for invoke-direct.
    // __ B(eq, &execute_target_method);
    execute_target_method_phi->addIncoming(method, __ GetInsertBlock());
    codegen_->CreateBranchIfTrue(is_invoke_direct, execute_target_method_block);

    llvm::BasicBlock* non_virtual_dispatch_block = codegen_->CreateBasicBlock();
    // __ Cmp(method_handle_kind, Operand(mirror::MethodHandle::Kind::kInvokeVirtual));
    llvm::Value* is_invoke_virtual =
        __ CreateICmpEQ(method_handle_kind,
                        GetConstantInt(GetInt32Type(), mirror::MethodHandle::Kind::kInvokeVirtual));
    // __ B(ne, &non_virtual_dispatch);
    codegen_->CreateBranchIfFalse(is_invoke_virtual, non_virtual_dispatch_block);

    // Skip virtual dispatch if `method` is private.
    // __ Ldr(temp, MemOperand(method, ArtMethod::AccessFlagsOffset().Int32Value()));
    llvm::Value* method_access_flags =
        CreateLoadWithOffset(GetUint32Type(), method, ArtMethod::AccessFlagsOffset().Int32Value());
    // __ And(temp, temp, Operand(kAccPrivate));
    llvm::Value* is_private_flag = __ CreateAnd(method_access_flags, kAccPrivate);
    // __ Cbnz(temp, &execute_target_method);
    llvm::Value* is_private =
        __ CreateICmpNE(is_private_flag, GetConstantZero(is_private_flag->getType()));
    execute_target_method_phi->addIncoming(method, __ GetInsertBlock());
    codegen_->CreateBranchIfTrue(is_private, execute_target_method_block);

    // If method is defined in the receiver's class, execute it as it is.
    // __ Ldr(temp, MemOperand(method, ArtMethod::DeclaringClassOffset().Int32Value()));
    llvm::Value* method_declaring_class = CreateLoadWithOffset(
        GetUncompressedGCPointerType(), method, ArtMethod::DeclaringClassOffset().Int32Value());
    // __ Ldr(receiver_class, HeapOperand(receiver.W(),
    //                                    mirror::Object::ClassOffset().Int32Value()));
    // codegen_->GetAssembler()->MaybeUnpoisonHeapReference(receiver_class.W());
    llvm::Value* receiver_class = CreateLoadWithOffset(
        GetUncompressedGCPointerType(), receiver, mirror::Object::ClassOffset().Int32Value());
    receiver_class = codegen_->MaybeUnpoisonHeapReference(receiver_class);
    // `receiver_class` is read w/o read barriers: false negatives go through virtual dispatch.
    // __ Cmp(temp, receiver_class);
    llvm::Value* is_same_class = __ CreateICmpEQ(method_declaring_class, receiver_class);
    // __ B(eq, &execute_target_method);
    execute_target_method_phi->addIncoming(method, __ GetInsertBlock());
    codegen_->CreateBranchIfTrue(is_same_class, execute_target_method_block);

    // MethodIndex is uint16_t.
    // __ Ldrh(temp, MemOperand(method, ArtMethod::MethodIndexOffset().Int32Value()));
    llvm::Value* method_index =
        CreateLoadWithOffset(GetUint16Type(), method, ArtMethod::MethodIndexOffset().Int32Value());

    constexpr uint32_t vtable_offset =
        mirror::Class::EmbeddedVTableOffset(art::PointerSize::k64).Int32Value();
    // __ Add(receiver_class.X(), receiver_class.X(), vtable_offset);
    llvm::Value* receiver_vtable = CreateGEP(receiver_class, vtable_offset);
    // __ Ldr(method, MemOperand(receiver_class.X(), temp, Extend::UXTW, 3u));
    llvm::Value* method_index_u64 = __ CreateZExt(method_index, GetUint64Type());
    llvm::Value* receiver_method_address =
        CreateGEP(GetMethodPointerType(), receiver_vtable, method_index_u64);
    llvm::Value* receiver_method = CreateLoad(GetMethodPointerType(), receiver_method_address);
    // __ B(&execute_target_method);
    execute_target_method_phi->addIncoming(receiver_method, __ GetInsertBlock());
    __ CreateBr(execute_target_method_block);

    // __ Bind(&non_virtual_dispatch);
    __ SetInsertPoint(non_virtual_dispatch_block);
    // __ Cmp(method_handle_kind, Operand(mirror::MethodHandle::Kind::kInvokeInterface));
    llvm::Value* is_invoke_interface = __ CreateICmpEQ(
        method_handle_kind,
        GetConstantInt(GetInt32Type(), mirror::MethodHandle::Kind::kInvokeInterface));
    // __ B(ne, slow_path->GetEntryLabel());
    llvm::Instruction* interface_br =
        codegen_->CreateBranchIfFalse(is_invoke_interface, slow_path->GetEntryBlock());
    ExpectTrueBranch(interface_br);

    // Skip virtual dispatch if `method` is private.
    // Re-using method_handle_kind to store access flags.
    // __ Ldr(access_flags, MemOperand(method, ArtMethod::AccessFlagsOffset().Int32Value()));
    llvm::Value* interface_method_access_flags =
        CreateLoadWithOffset(GetUint32Type(), method, ArtMethod::AccessFlagsOffset().Int32Value());
    // __ And(temp, access_flags, Operand(kAccPrivate));
    llvm::Value* is_interface_private_flag =
        __ CreateAnd(interface_method_access_flags, kAccPrivate);
    // __ Cbnz(temp, &execute_target_method);
    llvm::Value* is_interface_private = __ CreateICmpNE(
        is_interface_private_flag, GetConstantZero(is_interface_private_flag->getType()));
    execute_target_method_phi->addIncoming(method, __ GetInsertBlock());
    codegen_->CreateBranchIfTrue(is_interface_private, execute_target_method_block);

    // Set the hidden argument.
    // __ Mov(ip1, method);

    llvm::BasicBlock* get_imt_index_from_method_index_block = codegen_->CreateBasicBlock();
    llvm::BasicBlock* do_imt_dispatch_block = codegen_->CreateBasicBlock();

    llvm::BasicBlock* interface_current_block = __ GetInsertBlock();
    __ SetInsertPoint(do_imt_dispatch_block);
    llvm::PHINode* imt_index_phi = __ CreatePHI(GetUint16Type(), 2);
    __ SetInsertPoint(interface_current_block);

    // Get IMT index.
    // Not doing default conflict check as IMT index is set for all method which have
    // kAccAbstract bit.
    // __ And(temp, access_flags, Operand(kAccAbstract));
    llvm::Value* is_interface_abstract_flag =
        __ CreateAnd(interface_method_access_flags, kAccAbstract);
    // __ Cbz(temp, &get_imt_index_from_method_index);
    llvm::Value* is_interface_abstract = __ CreateICmpNE(
        is_interface_abstract_flag, GetConstantZero(is_interface_abstract_flag->getType()));
    codegen_->CreateBranchIfFalse(is_interface_abstract, get_imt_index_from_method_index_block);

    // imt_index is uint16_t
    // __ Ldrh(temp, MemOperand(method, ArtMethod::ImtIndexOffset().Int32Value()));
    llvm::Value* imt_index =
        CreateLoadWithOffset(GetUint16Type(), method, ArtMethod::ImtIndexOffset().Int32Value());
    // __ B(&do_imt_dispatch);
    __ CreateBr(do_imt_dispatch_block);
    imt_index_phi->addIncoming(imt_index, __ GetInsertBlock());

    // Default method, do method->GetMethodIndex() & (ImTable::kSizeTruncToPowerOfTwo - 1);
    // __ Bind(&get_imt_index_from_method_index);
    __ SetInsertPoint(get_imt_index_from_method_index_block);
    // __ Ldr(temp, MemOperand(method, ArtMethod::MethodIndexOffset().Int32Value()));
    llvm::Value* interface_method_index =
        CreateLoadWithOffset(GetUint16Type(), method, ArtMethod::MethodIndexOffset().Int32Value());
    // __ And(temp, temp, Operand(ImTable::kSizeTruncToPowerOfTwo - 1));
    llvm::Value* imt_index_from_method_index =
        __ CreateAnd(interface_method_index, ImTable::kSizeTruncToPowerOfTwo - 1);
    __ CreateBr(do_imt_dispatch_block);
    imt_index_phi->addIncoming(imt_index_from_method_index, __ GetInsertBlock());

    // __ Bind(&do_imt_dispatch);
    __ SetInsertPoint(do_imt_dispatch_block);
    // Re-using `method` to store receiver class and ImTableEntry.
    // __ Ldr(method.W(), HeapOperand(receiver.W(), mirror::Object::ClassOffset()));
    receiver_class = CreateLoadWithOffset(
        GetUncompressedGCPointerType(), receiver, mirror::Object::ClassOffset().Int32Value());
    // codegen_->GetAssembler()->MaybeUnPoisonHeapReference(method.W());
    receiver_class = codegen_->MaybeUnpoisonHeapReference(receiver_class);

    // __ Ldr(method, MemOperand(method,
    //                           mirror::Class::ImtPtrOffset(PointerSize::k64).Int32Value()));
    llvm::Value* imt_table_entry =
        CreateLoadWithOffset(GetPointerType(),
                             receiver_class,
                             mirror::Class::ImtPtrOffset(PointerSize::k64).Int32Value());
    // __ Ldr(method, MemOperand(method, temp, Extend::UXTW, 3u));
    llvm::Value* imt_index_phi_u64 = __ CreateZExt(imt_index_phi, GetUint64Type());
    llvm::Value* imt_method_address =
        CreateGEP(GetMethodPointerType(), imt_table_entry, imt_index_phi_u64);
    llvm::Value* imt_method = CreateLoad(GetMethodPointerType(), imt_method_address);

    llvm::SmallVector<llvm::Value*> args;
    uint32_t number_of_args = invoke->GetNumberOfArguments();
    args.reserve(number_of_args - 1 + 3);
    // We need to pass the callee method twice. These will be placed in x0 and x17/ip1.
    args.push_back(imt_method);
    args.push_back(imt_method);
    args.push_back(codegen_->GetUndefCurrentMethodPointer());
    for (uint32_t i = 1; i < number_of_args; ++i) {
      args.push_back(GetValue(invoke->InputAt(i)));
    }
    // Passing MethodHandle object as the last parameter: accessors implementations rely on it.
    args.push_back(method_handle);
    llvm::SmallVector<llvm::Type*> arg_types = codegen_->GetParameterTypes(args);
    llvm::FunctionType* func_type = llvm::FunctionType::get(result_type, arg_types, false);

    Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64PointerSize);
    llvm::Value* func = CreateLoadWithOffset(GetPointerType(), imt_method, entry_point.SizeValue());
    llvm::CallBase* call = codegen_->CreateInvokeInterfaceCall(invoke, func_type, func, args);
    uint64_t id = EncodePatchpointID(PatchpointKind::kNone, codegen_->AddStackMapInfo(invoke));
    codegen_->SetStatepointID(invoke, call, id);

    __ CreateBr(done_block);
    if (result_phi) {
      result_phi->addIncoming(call, __ GetInsertBlock());
    }
  } else {
    // Not invoke-static and the first argument is not a reference type.
    // __ B(slow_path->GetEntryLabel());
    __ CreateBr(slow_path->GetEntryBlock());
  }

  // __ Bind(&execute_target_method);
  __ SetInsertPoint(execute_target_method_block);

  llvm::SmallVector<llvm::Value*> args;
  uint32_t number_of_args = invoke->GetNumberOfArguments();
  args.reserve(number_of_args + 2);
  // We need to pass the callee method twice. These will be placed in x0 and x17/ip1.
  args.push_back(execute_target_method_phi);
  args.push_back(codegen_->GetUndefCurrentMethodPointer());
  for (uint32_t i = 1; i < number_of_args; ++i) {
    args.push_back(GetValue(invoke->InputAt(i)));
  }
  // Passing MethodHandle object as the last parameter: accessors implementations rely on it.
  args.push_back(method_handle);
  llvm::SmallVector<llvm::Type*> arg_types = codegen_->GetParameterTypes(args);
  llvm::FunctionType* func_type = llvm::FunctionType::get(result_type, arg_types, false);

  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64PointerSize);
  // __ Ldr(lr, MemOperand(method, entry_point.SizeValue()));
  llvm::Value* func =
      CreateLoadWithOffset(GetPointerType(), execute_target_method_phi, entry_point.SizeValue());
  // __ Blr(lr);
  // codegen_->RecordPcInfo(invoke, invoke->GetDexPc(), slow_path);
  llvm::CallBase* call = codegen_->CreateInvokeDexCall(invoke, func_type, func, args);
  uint64_t id = EncodePatchpointID(PatchpointKind::kNone, codegen_->AddStackMapInfo(invoke));
  codegen_->SetStatepointID(invoke, call, id);
  // __ Bind(slow_path->GetExitLabel());
  __ CreateBr(done_block);
  if (result_phi) {
    result_phi->addIncoming(call, __ GetInsertBlock());
  }

  __ SetInsertPoint(done_block);

  if (result_phi) {
    AddValue(invoke, result_phi);
  }
}

#undef __

#define MARK_UNIMPLEMENTED(Name)                                                        \
  void IntrinsicCodeGeneratorARM64LLVM::Visit##Name([[maybe_unused]] HInvoke* invoke) { \
    SetError();                                                                         \
  }
UNIMPLEMENTED_INTRINSIC_LIST_ARM64LLVM(MARK_UNIMPLEMENTED);
#undef MARK_UNIMPLEMENTED

// Originally defined in intrinsics.h, but that also includes
// a LocationsBuilder class, which we don't need.
#undef UNREACHABLE_INTRINSIC
#define UNREACHABLE_INTRINSIC(Arch, Name)                             \
  void IntrinsicCodeGenerator##Arch::Visit##Name(HInvoke* invoke) {   \
    LOG(FATAL) << "Unreachable: intrinsic " << invoke->GetIntrinsic() \
               << " should have been converted to HIR";               \
  }
UNREACHABLE_INTRINSICS(ARM64LLVM)
#undef UNREACHABLE_INTRINSIC

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetLLVMType(DataType::Type type) const {
  return codegen_->GetLLVMType(type);
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetPointerType() const {
  return codegen_->GetPointerType();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetUncompressedGCPointerType() const {
  return codegen_->GetUncompressedGCPointerType();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetCompressedGCPointerType() const {
  return codegen_->GetCompressedGCPointerType();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetMethodPointerType() const {
  return codegen_->GetMethodPointerType();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetBooleanType() const {
  return codegen_->GetBooleanType();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetInt8Type() const { return codegen_->GetInt8Type(); }

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetUint8Type() const {
  return codegen_->GetUint8Type();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetInt16Type() const {
  return codegen_->GetInt16Type();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetUint16Type() const {
  return codegen_->GetUint16Type();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetInt32Type() const {
  return codegen_->GetInt32Type();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetUint32Type() const {
  return codegen_->GetUint32Type();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetInt64Type() const {
  return codegen_->GetInt64Type();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetUint64Type() const {
  return codegen_->GetUint64Type();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetFloat32Type() const {
  return codegen_->GetFloat32Type();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetFloat64Type() const {
  return codegen_->GetFloat64Type();
}

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetVoidType() const { return codegen_->GetVoidType(); }

llvm::Type* IntrinsicCodeGeneratorARM64LLVM::GetVectorType(llvm::Type* packed_type,
                                                           size_t length) const {
  return codegen_->GetVectorType(packed_type, length);
}

llvm::Function* IntrinsicCodeGeneratorARM64LLVM::GetFunction() const {
  return codegen_->GetFunction();
}

llvm::Constant* IntrinsicCodeGeneratorARM64LLVM::GetConstantZero(llvm::Type* type) const {
  return codegen_->GetConstantZero(type);
}

llvm::Constant* IntrinsicCodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type,
                                                                int32_t value) const {
  return codegen_->GetConstantInt(type, value);
}

llvm::Constant* IntrinsicCodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type,
                                                                int64_t value) const {
  return codegen_->GetConstantInt(type, value);
}

llvm::Constant* IntrinsicCodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type,
                                                                uint32_t value) const {
  return codegen_->GetConstantInt(type, value);
}

llvm::Constant* IntrinsicCodeGeneratorARM64LLVM::GetConstantInt(llvm::Type* type,
                                                                uint64_t value) const {
  return codegen_->GetConstantInt(type, value);
}

llvm::Constant* IntrinsicCodeGeneratorARM64LLVM::GetFP16AltNaNAsInt() const {
  llvm::Constant* kFP16AltNaN = codegen_->GetConstantInt(codegen_->GetInt16Type(), 0x7e98);
  return kFP16AltNaN;
}

llvm::Constant* IntrinsicCodeGeneratorARM64LLVM::GetFP16NaNAsInt() const {
  llvm::Constant* kFP16NaN = codegen_->GetConstantInt(codegen_->GetInt16Type(), 0x7e00);
  return kFP16NaN;
}

void IntrinsicCodeGeneratorARM64LLVM::AddValue(HInstruction* instruction,
                                               llvm::Value* value) const {
  codegen_->AddValue(instruction, value);
}

llvm::Value* IntrinsicCodeGeneratorARM64LLVM::GetValue(HInstruction* instruction,
                                                       DataType::Type value_type) const {
  return codegen_->GetValue(instruction, value_type);
}

llvm::Value* IntrinsicCodeGeneratorARM64LLVM::CreateLoad(llvm::Type* type,
                                                         llvm::Value* address) const {
  return codegen_->CreateLoad(type, address);
}

void IntrinsicCodeGeneratorARM64LLVM::CreateStore(llvm::Value* value, llvm::Value* address) const {
  codegen_->CreateStore(value, address);
}

llvm::Value* IntrinsicCodeGeneratorARM64LLVM::CreateGEP(llvm::Value* address,
                                                        llvm::Value* offset) const {
  return codegen_->CreateGEP(address, offset);
}

llvm::Value* IntrinsicCodeGeneratorARM64LLVM::CreateGEP(llvm::Value* address,
                                                        int64_t offset) const {
  return codegen_->CreateGEP(address, offset);
}

llvm::Value* IntrinsicCodeGeneratorARM64LLVM::CreateGEP(llvm::Type* type,
                                                        llvm::Value* address,
                                                        llvm::Value* offset) const {
  return codegen_->CreateGEP(type, address, offset);
}

llvm::Value* IntrinsicCodeGeneratorARM64LLVM::CreateGEP(llvm::Type* type,
                                                        llvm::Value* address,
                                                        int64_t offset) const {
  return codegen_->CreateGEP(type, address, offset);
}

llvm::Value* IntrinsicCodeGeneratorARM64LLVM::CreateLoadWithOffset(llvm::Type* type,
                                                                   llvm::Value* address,
                                                                   int64_t offset) const {
  return codegen_->CreateLoadWithOffset(type, address, offset);
}

}  // namespace arm64_llvm
}  // namespace art HIDDEN
