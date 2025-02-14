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

#ifndef ART_COMPILER_OPTIMIZING_INTRINSICS_LLVM_ARM64_H_
#define ART_COMPILER_OPTIMIZING_INTRINSICS_LLVM_ARM64_H_

#include "base/macros.h"
#include "intrinsics.h"
#include "intrinsics_list.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#pragma GCC diagnostic ignored "-Wused-but-marked-unused"
#pragma GCC diagnostic ignored "-Wdeprecated-copy-with-dtor"
#pragma GCC diagnostic ignored "-Wframe-larger-than"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#pragma GCC diagnostic pop

namespace vixl {
namespace aarch64 {

class MacroAssembler;

}  // namespace aarch64
}  // namespace vixl

namespace art HIDDEN {

class ArenaAllocator;
class HInvokeStaticOrDirect;
class HInvokeVirtual;

namespace arm64_llvm {

class CodeGeneratorARM64LLVM;

class IntrinsicCodeGeneratorARM64LLVM final : public IntrinsicVisitor {
 public:
  explicit IntrinsicCodeGeneratorARM64LLVM(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  // Custom Dispatch method that checks whether there was an error while trying to emit the
  // intrinsic. Returns true if the intrinsic has been emitted, and false if it was unhandled.
  bool Dispatch(HInvoke* invoke) {
    ClearError();
    // Call actual Dispatch method.
    IntrinsicVisitor::Dispatch(invoke);
    bool result = !has_error_;
    ClearError();
    return result;
  }

  // Define visitor methods.

#define OPTIMIZING_INTRINSICS(                                             \
    Name, IsStatic, NeedsEnvironmentOrCache, SideEffects, Exceptions, ...) \
  void Visit##Name(HInvoke* invoke) override;
  ART_INTRINSICS_WITH_HINVOKE_LIST(OPTIMIZING_INTRINSICS)
#undef OPTIMIZING_INTRINSICS

  void SetError() { has_error_ = true; }
  void ClearError() { has_error_ = false; }

  llvm::Type* GetLLVMType(DataType::Type type) const;

  llvm::Type* GetPointerType() const;
  llvm::Type* GetUncompressedGCPointerType() const;
  llvm::Type* GetCompressedGCPointerType() const;
  llvm::Type* GetMethodPointerType() const;
  llvm::Type* GetBooleanType() const;
  llvm::Type* GetInt8Type() const;
  llvm::Type* GetUint8Type() const;
  llvm::Type* GetInt16Type() const;
  llvm::Type* GetUint16Type() const;
  llvm::Type* GetInt32Type() const;
  llvm::Type* GetUint32Type() const;
  llvm::Type* GetInt64Type() const;
  llvm::Type* GetUint64Type() const;
  llvm::Type* GetFloat32Type() const;
  llvm::Type* GetFloat64Type() const;
  llvm::Type* GetVoidType() const;
  llvm::Type* GetVectorType(llvm::Type* packed_type, size_t length) const;

  llvm::Function* GetFunction() const;
  // Returns the zero value for the given type. The type can be an integer, pointer or
  // floating-point type.
  llvm::Constant* GetConstantZero(llvm::Type* type) const;
  llvm::Constant* GetConstantInt(llvm::Type* type, int32_t value) const;
  llvm::Constant* GetConstantInt(llvm::Type* type, int64_t value) const;
  llvm::Constant* GetConstantInt(llvm::Type* type, uint32_t value) const;
  llvm::Constant* GetConstantInt(llvm::Type* type, uint64_t value) const;

  llvm::Constant* GetFP16AltNaNAsInt() const;
  llvm::Constant* GetFP16NaNAsInt() const;

  void AddValue(HInstruction* instruction, llvm::Value* value) const;
  llvm::Value* GetValue(HInstruction* instruction,
                        DataType::Type value_type = DataType::Type::kVoid) const;

  llvm::Value* CreateLoad(llvm::Type* type, llvm::Value* address) const;
  void CreateStore(llvm::Value* value, llvm::Value* address) const;
  llvm::Value* CreateGEP(llvm::Value* address, llvm::Value* offset) const;
  llvm::Value* CreateGEP(llvm::Value* address, int64_t offset) const;
  llvm::Value* CreateGEP(llvm::Type* type, llvm::Value* address, llvm::Value* offset) const;
  llvm::Value* CreateGEP(llvm::Type* type, llvm::Value* address, int64_t offset) const;

  llvm::Value* CreateLoadWithOffset(llvm::Type* type, llvm::Value* address, int64_t offset) const;

 private:
  ArenaAllocator* GetAllocator();

  void HandleValueOf(HInvoke* invoke,
                     const IntrinsicVisitor::ValueOfInfo& info,
                     DataType::Type type);

  CodeGeneratorARM64LLVM* const codegen_;
  bool has_error_ = false;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicCodeGeneratorARM64LLVM);
};

llvm::BasicBlock::iterator RewriteMemCpyI16(llvm::CallInst* call, CodeGeneratorARM64LLVM* codegen_);
llvm::BasicBlock::iterator RewriteMemCpyI32(llvm::CallInst* call, CodeGeneratorARM64LLVM* codegen_);
llvm::BasicBlock::iterator RewriteMemCpyI8ZextToI16(llvm::CallInst* call,
                                                    CodeGeneratorARM64LLVM* codegen_);
llvm::BasicBlock::iterator RewriteStringEquals(llvm::CallInst* call,
                                               CodeGeneratorARM64LLVM* codegen_);

}  // namespace arm64_llvm
}  // namespace art HIDDEN

#endif  // ART_COMPILER_OPTIMIZING_INTRINSICS_LLVM_ARM64_H_
