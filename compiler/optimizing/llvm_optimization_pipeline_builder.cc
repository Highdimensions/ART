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

#pragma GCC diagnostic push
// A common optimization pass, llvm::InstCombinePass, is a very large object, which would exceed the
// default frame size restriction. Therefore we ignore this warning in this file.
#pragma GCC diagnostic ignored "-Wframe-larger-than"

#include "llvm_optimization_pipeline_builder.h"

#include "base/logging.h"
#include "code_generator_arm64_llvm_utils.h"
#include "intrinsics_arm64_llvm.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#pragma GCC diagnostic ignored "-Wdeprecated-copy-with-dtor"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#pragma GCC diagnostic pop

namespace art HIDDEN {
namespace arm64_llvm {

static constexpr std::string_view kLoopPatchpointIDKey = "art.patchpoint.id";

static bool IsArtMethod(llvm::Function* function) {
  return function->hasGC() && function->getGC() == kArtGCStrategyName;
}

static std::string ValueToString(llvm::Value* value) {
  std::string result;
  llvm::raw_string_ostream ss(result);
  ss << *value;
  return result;
}

class VerifierPass : public llvm::PassInfoMixin<VerifierPass> {
 public:
  VerifierPass(std::string name) : name_(std::move(name)) {}

  llvm::PreservedAnalyses run(
      llvm::Module& module, [[maybe_unused]] llvm::ModuleAnalysisManager& module_analysis_manager) {
    bool broken = llvm::verifyModule(module, &llvm::dbgs());
    if (broken) {
      module.print(llvm::dbgs(), nullptr);
      LOG(FATAL) << "Verify step '" << name_ << "' failed on module '"
                 << std::string_view(module.getName()) << "'";
    }
    return llvm::PreservedAnalyses::all();
  }

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    bool broken = llvm::verifyFunction(function, &llvm::dbgs());
    if (broken) {
      function.getParent()->print(llvm::dbgs(), nullptr);
      LOG(FATAL) << "Verify step '" << name_ << "' failed on function '"
                 << std::string_view(function.getName()) << "'";
    }
    return llvm::PreservedAnalyses::all();
  }

 private:
  std::string name_;
};

// Adds aliasing metadata to memory operations to help alias analysis in the optimization pipeline.
// With the IR that we generate, the optimizer's default behaviour has a hard time with alias
// analysis, so we need to help it by providing this metadata ourselves.
class InferMemoryScopesPass : public llvm::PassInfoMixin<InferMemoryScopesPass> {
 public:
  InferMemoryScopesPass(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    // TODO: Is this needed? Does adding metadata invalidate any analysis?
    bool changed = false;
    for (llvm::BasicBlock& block : function) {
      for (llvm::Instruction& instruction : block) {
        changed |= AddMetadata(&instruction);
      }
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }

    return llvm::PreservedAnalyses::none();
  }

  bool AddMetadata(llvm::Instruction* instruction) {
    if (llvm::LoadInst* load = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
      if (MaybeAddAliasScope(instruction, load->getPointerOperand())) {
        return true;
      }
    } else if (llvm::StoreInst* store = llvm::dyn_cast<llvm::StoreInst>(instruction)) {
      if (MaybeAddNoalias(instruction, store->getPointerOperand())) {
        return true;
      }
    } else if (llvm::CallBase* call = llvm::dyn_cast<llvm::CallBase>(instruction)) {
      llvm::Attribute statepoint_id = call->getFnAttr("statepoint-id");
      if (statepoint_id.isValid()) {
        llvm::StringRef id_string = statepoint_id.getValueAsString();
        uint64_t id = 0;
        bool error = id_string.getAsInteger(10, id);
        DCHECK(!error);
        switch (DecodePatchpointKindFromID(id)) {
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
            // NOTE: This should always have heap scope.
            if (MaybeAddAliasScope(instruction, call->getArgOperand(0))) {
              return true;
            }
            break;
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
            // NOTE: This should always have heap scope.
            if (MaybeAddNoalias(instruction, call->getArgOperand(1))) {
              return true;
            }
            break;
          case PatchpointKind::kNone:
          case PatchpointKind::kGCPointerAllocaMap:
          case PatchpointKind::kTryBoundaryStackReadClobber:
          case PatchpointKind::kImplicitSuspendCheck:
          case PatchpointKind::kReachabilityFence:
          case PatchpointKind::kCriticalNativeCall:
          case PatchpointKind::kLoadClassBootImageLinkTimePcRelative:
          case PatchpointKind::kLoadClassBootImageRelRo:
          case PatchpointKind::kLoadClassAppImageRelRo:
          case PatchpointKind::kLoadClassBootImageIntrinsic:
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
            break;
        }
      }
    }

    return false;
  }

  enum class MemoryScope {
    kNone,
    kHeap,
    kStack,
    kRuntime,
    kThreadObject,
  };

  static MemoryScope GetMemoryScope(llvm::Value* pointer) {
    DCHECK(pointer->getType()->isPointerTy());
    llvm::PointerType* pointer_type = llvm::cast<llvm::PointerType>(pointer->getType());
    // If it's a heap reference, the load must be from the heap.
    if (pointer_type->getAddressSpace() == kUncompressedGCAddressSpace ||
        pointer_type->getAddressSpace() == kCompressedGCPointerAddressSpace) {
      return MemoryScope::kHeap;
    }
    // If it's a method pointer, we treat the load as if it's from runtime memory.
    if (pointer_type->getAddressSpace() == kMethodPointerAddressSpace) {
      return MemoryScope::kRuntime;
    }

    // Strip GEPs from the pointer.
    while (llvm::GetElementPtrInst* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer)) {
      pointer = gep->getPointerOperand();
    }

    // If it's an alloca, then the load is from the stack.
    if (llvm::isa<llvm::AllocaInst>(pointer)) {
      return MemoryScope::kStack;
    }

    // If pointer is an 'inttoptr' of an '@llvm.read_register' call, then it must be either the
    // thread register, marking register or the implicit suspend check register, which all point
    // to runtime memory.
    if (llvm::IntToPtrInst* int_to_ptr = llvm::dyn_cast<llvm::IntToPtrInst>(pointer)) {
      if (llvm::IntrinsicInst* intrinsic =
              llvm::dyn_cast<llvm::IntrinsicInst>(int_to_ptr->getOperand(0));
          intrinsic != nullptr && intrinsic->getIntrinsicID() == llvm::Intrinsic::read_register) {
        llvm::Value* reg = intrinsic->getArgOperand(0);
        DCHECK(llvm::isa<llvm::MetadataAsValue>(reg));
        llvm::Metadata* reg_md = llvm::cast<llvm::MetadataAsValue>(reg)->getMetadata();
        DCHECK(llvm::isa<llvm::MDNode>(reg_md));
        llvm::MDNode* reg_md_node = llvm::cast<llvm::MDNode>(reg_md);
        DCHECK(llvm::isa<llvm::MDString>(reg_md_node->getOperand(0)));
        std::string_view reg_string =
            llvm::cast<llvm::MDString>(reg_md_node->getOperand(0))->getString();
        if (reg_string == "sp") {
          return MemoryScope::kStack;
        } else if (reg_string == fmt::format("x{}", tr.GetCode())) {
          return MemoryScope::kThreadObject;
        } else {
          return MemoryScope::kRuntime;
        }
      }
    }

    // If the pointer comes from a load instruction, then check if that load uses a pointer coming
    // from one of the runtime registers.
    if (llvm::LoadInst* load = llvm::dyn_cast<llvm::LoadInst>(pointer)) {
      llvm::Value* load_pointer = load->getPointerOperand();
      // Strip GEPs from the pointer.
      while (llvm::GetElementPtrInst* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(load_pointer)) {
        load_pointer = gep->getPointerOperand();
      }
      if (llvm::IntToPtrInst* int_to_ptr = llvm::dyn_cast<llvm::IntToPtrInst>(load_pointer)) {
        if (llvm::IntrinsicInst* intrinsic =
                llvm::dyn_cast<llvm::IntrinsicInst>(int_to_ptr->getOperand(0));
            intrinsic != nullptr && intrinsic->getIntrinsicID() == llvm::Intrinsic::read_register) {
          // TODO: We should check that it is indeed one of the three runtime reserved registers.
          return MemoryScope::kRuntime;
        }
      }
    }

    return MemoryScope::kNone;
  }

  bool MaybeAddAliasScope(llvm::Instruction* instruction, llvm::Value* pointer) {
    switch (GetMemoryScope(pointer)) {
      case MemoryScope::kNone:
        return false;
      case MemoryScope::kHeap:
        codegen_->AddHeapAliasScopeMetadata(instruction);
        return true;
      case MemoryScope::kStack:
        codegen_->AddStackAliasScopeMetadata(instruction);
        return true;
      case MemoryScope::kRuntime:
        codegen_->AddRuntimeAliasScopeMetadata(instruction);
        return true;
      case MemoryScope::kThreadObject:
        codegen_->AddThreadObjectAliasScopeMetadata(instruction);
        return true;
    }
  }

  bool MaybeAddNoalias(llvm::Instruction* instruction, llvm::Value* pointer) {
    switch (GetMemoryScope(pointer)) {
      case MemoryScope::kNone:
        return false;
      case MemoryScope::kHeap:
        codegen_->AddHeapNoaliasMetadata(instruction);
        return true;
      case MemoryScope::kStack:
        codegen_->AddStackNoaliasMetadata(instruction);
        return true;
      case MemoryScope::kRuntime:
        codegen_->AddRuntimeNoaliasMetadata(instruction);
        return true;
      case MemoryScope::kThreadObject:
        codegen_->AddThreadObjectNoaliasMetadata(instruction);
        return true;
    }
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

// Pass to add an @llvm.experimental.stackmap that records all allocas in it, which is needed to
// prevent LLVM from eliminating all allocas, even across try-catch boundaries. This pass is also
// important in preventing LLVM from optimizing simple recursive functions into an empty infinite
// loop.
// Example:
// void foo() {
//   foo();
// }
class RecordAllocasInStackMapPass : public llvm::PassInfoMixin<RecordAllocasInStackMapPass> {
 public:
  RecordAllocasInStackMapPass(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    llvm::IRBuilder<> builder(function.getContext());

    llvm::SmallVector<llvm::Value*> stack_map_args;
    stack_map_args.push_back(builder.getInt64(-1));
    stack_map_args.push_back(builder.getInt32(0));

    llvm::Instruction* insert_point = nullptr;
    for (llvm::Instruction& inst : function.getEntryBlock()) {
      if (llvm::AllocaInst* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
        stack_map_args.push_back(alloca);
      } else {
        insert_point = &inst;
        break;
      }
    }
    DCHECK(insert_point != nullptr);
    builder.SetInsertPoint(insert_point);
    llvm::CallInst* stack_map_call =
        builder.CreateIntrinsic(llvm::Intrinsic::experimental_stackmap, {}, stack_map_args);
    // Add memory(write) attribute to the stack map call, to indicate that the alloca positions are
    // recorded, and shouldn't be optimized out.
    stack_map_call->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        codegen_->GetLLVMContext(), llvm::MemoryEffects::writeOnly()));
    codegen_->AddRuntimeNoaliasMetadata(stack_map_call);

    return llvm::PreservedAnalyses::none();
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

// Marks all getelementptr instructions with the 'inbounds' attribute, which is guaranteed by the
// source language semantics (null checks, bounds checks, ...).
class MarkGEPsAsInBoundsPass : public llvm::PassInfoMixin<MarkGEPsAsInBoundsPass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    for (llvm::BasicBlock& basic_block : function) {
      for (llvm::Instruction& inst : basic_block) {
        if (llvm::GetElementPtrInst* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst)) {
          gep->setIsInBounds(true);
        }
      }
    }

    return llvm::PreservedAnalyses::none();
  }
};

// A simple peephole optimizer to fold redundant casts between compressed and uncompressed pointers.
class PeepholeOptimizationPass : public llvm::PassInfoMixin<PeepholeOptimizationPass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    bool changed = false;
    for (llvm::BasicBlock& basic_block : function) {
      changed |= RunOnBasicBlock(basic_block);
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }
    return llvm::PreservedAnalyses::none();
  }

  llvm::CallInst* GetGCPointerCast(llvm::Value* value) {
    if (llvm::CallInst* call = llvm::dyn_cast<llvm::CallInst>(value)) {
      if (call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->hasMetadata(kGCPointerCastMetadata)) {
        return call;
      }
    }
    return nullptr;
  }

  bool RunOnBasicBlock(llvm::BasicBlock& basic_block) {
    bool changed = false;

    llvm::BasicBlock::iterator bb_it = basic_block.begin();
    while (bb_it != basic_block.end()) {
      llvm::Instruction* inst = &*bb_it;
      // Optimize __cast_to_compressed and __cast_to_uncompressed
      if (llvm::CallInst* cast = GetGCPointerCast(inst)) {
        llvm::Value* arg = cast->getArgOperand(0);
        // Fold: __cast_to_[un]compressed(null) -> null
        if (llvm::isa<llvm::ConstantPointerNull>(arg)) {
          changed = true;
          llvm::PointerType* pointer_type = llvm::cast<llvm::PointerType>(inst->getType());
          inst->replaceAllUsesWith(llvm::ConstantPointerNull::get(pointer_type));
          bb_it = inst->eraseFromParent();
          continue;
        }
        // Fold: __cast_to_compressed(__cast_to_uncompressed(%p)) -> %p  and
        //       __cast_to_uncompressed(__cast_to_compressed(%p)) -> %p
        if (llvm::CallInst* arg_cast = GetGCPointerCast(arg)) {
          changed = true;
          inst->replaceAllUsesWith(arg_cast->getArgOperand(0));
          bb_it = inst->eraseFromParent();
          continue;
        }
      }

      ++bb_it;
    }

    return changed;
  }
};

// Disables loop unrolling for loops containing catch blocks, because ART requires catch blocks to
// be unique in the final native code.
class DisableCatchLoopUnrollingPass : public llvm::PassInfoMixin<DisableCatchLoopUnrollingPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Loop& loop,
                              [[maybe_unused]] llvm::LoopAnalysisManager& analysis_manager,
                              [[maybe_unused]] llvm::LoopStandardAnalysisResults& analysis_results,
                              [[maybe_unused]] llvm::LPMUpdater& updater) {
    bool changed = false;
    for (llvm::BasicBlock* basic_block : loop.blocks()) {
      if (basic_block->isEHPad()) {
        changed = true;
        llvm::MDNode* loop_id = loop.getLoopID();
        llvm::SmallVector<llvm::Metadata*> loop_metadata;
        // Placeholder, will be replaced by the new loop id metadata node.
        loop_metadata.push_back(nullptr);
        if (loop_id != nullptr) {
          loop_metadata.append(loop_id->op_begin(), loop_id->op_end());
        }
        llvm::Metadata* loop_unroll_disable_metadata =
            llvm::MDString::get(basic_block->getContext(), "llvm.loop.unroll.disable");
        loop_metadata.push_back(
            llvm::MDNode::get(basic_block->getContext(), loop_unroll_disable_metadata));

        llvm::MDTuple* new_loop_id = llvm::MDNode::get(basic_block->getContext(), loop_metadata);
        // Loop ID node's first operand must be itself.
        new_loop_id->replaceOperandWith(0, new_loop_id);
        loop.setLoopID(new_loop_id);
        break;
      }
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }
    return llvm::PreservedAnalyses::none();
  }
};

// Pass to remove the @llvm.experimental.stackmap call that records all alloca positions, and the
// alloca instructions that were eliminated everywhere else in the function.
class RemoveUnusedAllocasPass : public llvm::PassInfoMixin<RemoveUnusedAllocasPass> {
 public:
  static bool AreAllUsersStoresOrLifetime(llvm::AllocaInst* alloca) {
    for (llvm::User* user : alloca->users()) {
      if (llvm::isa<llvm::StoreInst>(user)) {
        continue;
      }
      if (llvm::isa<llvm::LifetimeIntrinsic>(user)) {
        continue;
      }

      return false;
    }
    return true;
  }

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    // Find and remove the stackmap intrinsic call that recorded the alloca values.
    for (llvm::BasicBlock& basic_block : function) {
      llvm::BasicBlock::iterator bb_it = basic_block.begin();
      while (bb_it != basic_block.end()) {
        llvm::Instruction& inst = *bb_it;
        if (llvm::IntrinsicInst* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&inst);
            intrinsic != nullptr &&
            intrinsic->getIntrinsicID() == llvm::Intrinsic::experimental_stackmap) {
          DCHECK_EQ(intrinsic->getNumUses(), 0u);
          llvm::Value* id = intrinsic->getArgOperand(0);
          DCHECK(llvm::isa<llvm::ConstantInt>(id));
          if (llvm::cast<llvm::ConstantInt>(id)->getSExtValue() == -1) {
            bb_it = intrinsic->eraseFromParent();
            continue;
          }
        }

        ++bb_it;
      }
    }

    // Remove all unused allocas.
    llvm::BasicBlock& entry_block = function.getEntryBlock();
    llvm::BasicBlock::iterator bb_it = entry_block.begin();
    while (bb_it != entry_block.end()) {
      llvm::Instruction& inst = *bb_it;
      llvm::AllocaInst* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst);
      if (alloca == nullptr) {
        break;
      }

      if (alloca->getNumUses() == 0) {
        bb_it = alloca->eraseFromParent();
        continue;
      }
      if (AreAllUsersStoresOrLifetime(alloca)) {
        auto it = alloca->use_begin();
        while (it != alloca->use_end()) {
          llvm::User* user = it->getUser();
          ++it;  // Do the iteration before eraseFromParent() is called below.
          DCHECK(llvm::isa<llvm::StoreInst>(user) || llvm::isa<llvm::LifetimeIntrinsic>(user));
          llvm::Instruction* user_inst = llvm::cast<llvm::Instruction>(user);
          user_inst->eraseFromParent();
        }
        DCHECK_EQ(alloca->getNumUses(), 0u);
        bb_it = alloca->eraseFromParent();
        continue;
      }

      ++bb_it;
    }

    return llvm::PreservedAnalyses::none();
  }
};

// Infers if the function can use implicit suspend checks based on if it uses wide vector values.
class InferCanUseImplicitSuspendChecksPass
    : public llvm::PassInfoMixin<InferCanUseImplicitSuspendChecksPass> {
 public:
  InferCanUseImplicitSuspendChecksPass(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    for (llvm::BasicBlock& basic_block : function) {
      for (llvm::Instruction& inst : basic_block) {
        // Implicit suspend checks only preserve the lower 64 bits of vector registers, so we
        // shouldn't use them in case there are SIMD operations in the function. These operations
        // either come from ART's vectorization, or LLVM's vectorization.
        if (GetVectorTypeSize(inst.getType()) > 8) {
          codegen_->SetCanUseImplicitSuspendCheck(false);
          return llvm::PreservedAnalyses::all();
        }
      }
    }

    codegen_->SetCanUseImplicitSuspendCheck(true);
    return llvm::PreservedAnalyses::all();
  }

  static size_t GetVectorTypeSize(llvm::Type* type) {
    if (!type->isVectorTy()) {
      return 0;
    }

    unsigned element_count = llvm::cast<llvm::VectorType>(type)->getElementCount().getFixedValue();
    return (element_count * type->getScalarSizeInBits()) / 8;
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

class ReplaceSuspendCheckPlaceholdersPass
    : public llvm::PassInfoMixin<ReplaceSuspendCheckPlaceholdersPass> {
 public:
  ReplaceSuspendCheckPlaceholdersPass(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    function_analysis_manager.registerPass([]() { return llvm::DominatorTreeAnalysis(); });
    llvm::DominatorTree& dominator_tree =
        function_analysis_manager.getResult<llvm::DominatorTreeAnalysis>(function);
    llvm::DomTreeUpdater dom_tree_updater(dominator_tree,
                                          llvm::DomTreeUpdater::UpdateStrategy::Lazy);
    bool changed = false;
    for (llvm::BasicBlock* basic_block = &function.getEntryBlock(); basic_block != nullptr;
         basic_block = basic_block->getNextNode()) {
      llvm::BasicBlock::iterator bb_it = basic_block->begin();
      while (bb_it != basic_block->end()) {
        llvm::CallBase* call = llvm::dyn_cast<llvm::CallBase>(&*bb_it);
        if (call == nullptr) {
          ++bb_it;
          continue;
        }

        llvm::Attribute statepoint_id = call->getFnAttr("statepoint-id");
        if (!statepoint_id.isValid()) {
          ++bb_it;
          continue;
        }

        llvm::StringRef id_string = statepoint_id.getValueAsString();
        uint64_t id = 0;
        bool error = id_string.getAsInteger(10, id);
        DCHECK(!error);
        if (DecodePatchpointKindFromID(id) != PatchpointKind::kImplicitSuspendCheck) {
          ++bb_it;
          continue;
        }

        changed = true;
        CHECK(call->getCalledFunction() != nullptr);
        CHECK(call->getCalledFunction()->getName() == "__suspend_check");
        codegen_->GetIRBuilder()->SetInsertPoint(call);
        uint32_t index = DecodePatchpointIndexFromID(id);
        HInstruction* instruction = codegen_->GetInstructionFromStackmapInfo(index);
        DCHECK(instruction->IsSuspendCheck());

        codegen_->SetCurrentBlock(instruction->GetBlock());
        codegen_->GetInstructionCodeGeneratorArm64()->GenerateSuspendCheck(
            instruction->AsSuspendCheck(), call, id, &dom_tree_updater);
        bb_it = call->eraseFromParent();
        basic_block = bb_it->getParent();
      }
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }
    return llvm::PreservedAnalyses::none();
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

// Inserts an entrypoint suspend check into the method, and adds a 2 instruction prologue for a
// stack overflow check, if they are required. For now the same condition, whether the function
// calls any other function, is used for both.
// TODO: Separate the conditions. In code_generator.cc the condition used for the suspend check and
// stack overflow check is `WillCall()` and `CanCall()` for each slow path. I'm not sure if this
// makes any difference, as the slow path has to call a function on the main path, but not on the
// slow path.
class PlaceEntrySuspendAndStackOverflowChecksPass
    : public llvm::PassInfoMixin<PlaceEntrySuspendAndStackOverflowChecksPass> {
 public:
  PlaceEntrySuspendAndStackOverflowChecksPass(CodeGeneratorARM64LLVM* codegen)
      : codegen_(codegen) {}

  // Returns true if function calls any other functions. This condition is used to determine whether
  // an entry suspend check and a stack overflow check is needed.
  static bool CanCall(llvm::Function& function) {
    for (llvm::BasicBlock& block : function) {
      for (llvm::Instruction& inst : block) {
        if ([[maybe_unused]] llvm::IntrinsicInst* intrinsic =
                llvm::dyn_cast<llvm::IntrinsicInst>(&inst)) {
          // Intrinsics don't count as function calls in our case. In llvm::PlaceSafepointsPass, it
          // checks for statepoint and patchpoint, but in our case function calls haven't been
          // rewritten at this point, so it is safe to ignore all intrinsic calls.
          // TODO: Check whether the above assumption holds in debug mode.
          continue;
        }

        llvm::CallBase* call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (call == nullptr) {
          continue;
        }

        // Skip calls that are followed by `unreachable`. These are typically calls to runtime
        // functions that throw an exception. If the exception could be caught in the function, the
        // call would be an invoke instruction, which isn't followed by any other instructions in
        // the block, so this check is valid for that case as well.
        if (call->getNextNode() != nullptr &&
            llvm::isa<llvm::UnreachableInst>(call->getNextNode())) {
          continue;
        }

        if (llvm::Function* called_function = call->getCalledFunction()) {
          if (called_function->hasMetadata(kPlaceholderFunctionMetadata)) {
            continue;
          }
        }

        llvm::Attribute statepoint_id = call->getFnAttr("statepoint-id");
        if (!statepoint_id.isValid()) {
          return true;
        }

        llvm::StringRef id_string = statepoint_id.getValueAsString();
        uint64_t id = 0;
        bool error = id_string.getAsInteger(10, id);
        DCHECK(!error);
        // Only kNone signals an actual call. All other patchpoint kinds translate to regular
        // instructions.
        // NOTE: We may be able to check for "gc-leaf-function" here as well, and skip this call if
        // it has the attribute. If that's valid, we could improve code generation that uses runtime
        // calls that don't require a stackmap entry, e.g. math function.
        if (DecodePatchpointKindFromID(id) == PatchpointKind::kNone) {
          return true;
        }
      }
    }
    return false;
  }

  static llvm::BasicBlock::iterator GetSuspendCheckInsertPoint(llvm::BasicBlock& entry_block) {
    auto it = entry_block.begin();
    // Suspend checks should be placed after all alloca instructions, otherwise the backend may not
    // be able to statically calculate the stack size of the function.
    while (llvm::isa<llvm::AllocaInst>(*it)) {
      ++it;
    }
    return it;
  }

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }
    if (!CanCall(function)) {
      return llvm::PreservedAnalyses::all();
    }

    // Add stack overflow check prologue to the function. x16 is used as a temporary register for
    // this, which should be fine, because its value is allowed to change between the point where
    // the call happens and where control reaches the beginning of this function.
    uint32_t temp_register_number = 16;
    uint32_t overflow_check_size = GetStackOverflowReservedBytes(InstructionSet::kArm64);
    std::array<uint32_t, kStackOverflowCheckPrologueSize> prologue_instructions = {
        // sub x16, sp, #0x2000
        CreateSub64Immediate(temp_register_number, kSpRegisterNumber, overflow_check_size),
        // ldr wzr, [x16]
        CreateLdr32(kWzrRegisterNumber, temp_register_number),
    };
    llvm::Constant* prologue_data =
        llvm::ConstantDataArray::get(codegen_->GetLLVMContext(), prologue_instructions);
    function.setPrologueData(prologue_data);

    // Generate suspend check.
    uint64_t id = codegen_->GetEntrySuspendCheckID();
    // The id may not have been set, meaning that the suspend check has been placed explicitly in
    // the IR during code generation.
    if (DecodePatchpointKindFromID(id) == PatchpointKind::kImplicitSuspendCheck) {
      codegen_->GetIRBuilder()->SetInsertPoint(
          GetSuspendCheckInsertPoint(function.getEntryBlock()));
      uint32_t index = DecodePatchpointIndexFromID(id);
      HInstruction* instruction = codegen_->GetInstructionFromStackmapInfo(index);
      DCHECK(instruction->IsSuspendCheck());

      function_analysis_manager.registerPass([]() { return llvm::DominatorTreeAnalysis(); });
      llvm::DominatorTree& dominator_tree =
          function_analysis_manager.getResult<llvm::DominatorTreeAnalysis>(function);
      llvm::DomTreeUpdater dom_tree_updater(dominator_tree,
                                            llvm::DomTreeUpdater::UpdateStrategy::Lazy);
      codegen_->GetInstructionCodeGeneratorArm64()->GenerateSuspendCheck(
          instruction->AsSuspendCheck(), nullptr, id, &dom_tree_updater, nullptr);
    }

    // Conservatively return ::none() here. TODO: Can we do better?
    return llvm::PreservedAnalyses::none();
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

// Inserts suspend checks at loop back-edges if they are required by ART. The decision of whether
// they are required or not is made by searching for a loop metadata node with the name
// `kLoopPatchpointIDKey`, which should have been emitted in `HandleGoto` if a suspend check is
// required. All other loops are ignored.
class PlaceLoopSuspendChecksPass : public llvm::PassInfoMixin<PlaceLoopSuspendChecksPass> {
 public:
  PlaceLoopSuspendChecksPass(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    function_analysis_manager.registerPass([]() { return llvm::LoopAnalysis(); });
    llvm::LoopInfo& loop_info = function_analysis_manager.getResult<llvm::LoopAnalysis>(function);
    loop_info.begin();

    llvm::SmallVector<llvm::Loop*> Worklist;
    Worklist.append(loop_info.getTopLevelLoops().begin(), loop_info.getTopLevelLoops().end());

    bool changed = false;
    while (!Worklist.empty()) {
      llvm::Loop* loop = Worklist.pop_back_val();
      Worklist.append(loop->getSubLoops().begin(), loop->getSubLoops().end());
      changed |= RunOnLoop(*loop, loop_info, function, function_analysis_manager);
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }
    // Conservatively return ::none() here. TODO: Can we do better?
    return llvm::PreservedAnalyses::none();
  }

  bool RunOnLoop(llvm::Loop& loop,
                 llvm::LoopInfo& loop_info,
                 llvm::Function& function,
                 llvm::FunctionAnalysisManager& function_analysis_manager) {
    llvm::MDNode* suspend_check_id_node = llvm::findOptionMDForLoop(&loop, kLoopPatchpointIDKey);
    if (suspend_check_id_node == nullptr) {
      // Originally ART didn't generate a suspend check here, so we don't have to generate one here
      // as well. This can happen with intrinsics, e.g. StringEquals, where a loop is generated, but
      // no suspend check is needed.
      return false;
    }

    DCHECK(llvm::isa<llvm::ConstantAsMetadata>(suspend_check_id_node->getOperand(1)));
    llvm::ConstantAsMetadata* id_node =
        llvm::cast<llvm::ConstantAsMetadata>(suspend_check_id_node->getOperand(1));
    DCHECK(llvm::isa<llvm::ConstantInt>(id_node->getValue()));
    uint64_t id = llvm::cast<llvm::ConstantInt>(id_node->getValue())->getZExtValue();

    llvm::DominatorTree& dominator_tree =
        function_analysis_manager.getResult<llvm::DominatorTreeAnalysis>(function);
    llvm::DomTreeUpdater dom_tree_updater(dominator_tree,
                                          llvm::DomTreeUpdater::UpdateStrategy::Lazy);

    llvm::BasicBlock* header_block = loop.getHeader();
    // Place the suspend check right before the branch.
    codegen_->GetIRBuilder()->SetInsertPoint(header_block->getFirstInsertionPt());
    uint32_t index = DecodePatchpointIndexFromID(id);
    HInstruction* instruction = codegen_->GetInstructionFromStackmapInfo(index);
    DCHECK(instruction->IsSuspendCheck());

    codegen_->GetInstructionCodeGeneratorArm64()->GenerateSuspendCheck(
        instruction->AsSuspendCheck(), nullptr, id, &dom_tree_updater, &loop_info);

    return true;
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

// Adds code to store the current method pointer to the bottom of the function's stack frame.
class FinalizeCurrentMethodStackPositionPass
    : public llvm::PassInfoMixin<FinalizeCurrentMethodStackPositionPass> {
 public:
  FinalizeCurrentMethodStackPositionPass(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  static llvm::BasicBlock::iterator GetStackStoreInsertPoint(llvm::BasicBlock& entry_block) {
    auto it = entry_block.begin();
    // We should place the stack store right after all of the allocas.
    while (llvm::isa<llvm::AllocaInst>(*it)) {
      ++it;
    }
    return it;
  }

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    // Only run on functions marked with `gc "art"`.
    if (!function.hasGC() || function.getGC() != kArtGCStrategyName) {
      return llvm::PreservedAnalyses::all();
    }

    bool needs_entry_store = false;
    for (llvm::BasicBlock& basic_block : function) {
      if (NeedsCurrentMethodOnStack(basic_block)) {
        needs_entry_store = true;
        break;
      }
    }

    // We haven't modified anything.
    if (!needs_entry_store) {
      return llvm::PreservedAnalyses::all();
    }

    llvm::IRBuilder<>& builder = *codegen_->GetIRBuilder();
    builder.SetInsertPoint(&function.getEntryBlock(),
                           GetStackStoreInsertPoint(function.getEntryBlock()));
    llvm::Value* current_method = codegen_->GetCurrentMethodPointerArgument();
    llvm::Value* sp_value = codegen_->GetStackPointerValue();
    builder.CreateStore(current_method, sp_value);

    return llvm::PreservedAnalyses::none();
  }

  // Returns whether an ART function call was encountered or not.
  bool NeedsCurrentMethodOnStack(llvm::BasicBlock& basic_block) {
    for (llvm::Instruction& inst : basic_block) {
      llvm::CallBase* call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }

      // If an ART calling convention is used, we need the current method pointer on the stack.
      switch (call->getCallingConv()) {
        case llvm::CallingConv::ARTInvokeDex:
        case llvm::CallingConv::ARTInvokeInterface:
        case llvm::CallingConv::ARTInvokePolymorphic:
        case llvm::CallingConv::ARTCriticalNative:
        case llvm::CallingConv::ARTInvokeRuntime:
        case llvm::CallingConv::ARTInvokeUnresolved:
        case llvm::CallingConv::ARTStringBuilderAppend:
        case llvm::CallingConv::ARTAnyReg:
        case llvm::CallingConv::ARTPreserveAll:
          return true;
        default:
          break;
      }

      // Calls without a "statepoint-id" attribute should be skipped, as they
      if (!call->hasFnAttr("statepoint-id")) {
        continue;
      }

      // If we're calling a placeholder function, we will need the current method pointer on the
      // stack in most cases. The few exceptions are e.g. compressed-uncompressed pointer casting,
      // which is handled by checking for the "statepoint-id" attribute.
      if (llvm::Function* called_function = call->getCalledFunction()) {
        if (called_function->hasMetadata(kPlaceholderFunctionMetadata)) {
          return true;
        }
      }
    }
    return false;
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

// Rewrites critical native calls with stack-passed arguments to a more explicit form, where we
// store the stack-passed values to the stack manually before the call. This is needed, because
// critical native calls grow the stack frame before the call in order to store stack-passed
// arguments there.
class RewriteCriticalNativeArgumentsPass
    : public llvm::PassInfoMixin<RewriteCriticalNativeArgumentsPass> {
 public:
  RewriteCriticalNativeArgumentsPass(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    bool changed = false;
    for (llvm::BasicBlock& basic_block : function) {
      llvm::BasicBlock::iterator bb_it = basic_block.begin();
      while (bb_it != basic_block.end()) {
        if (llvm::CallBase* call = llvm::dyn_cast<llvm::CallBase>(&*bb_it);
            call != nullptr && call->getCallingConv() == llvm::CallingConv::ARTCriticalNative) {
          auto [new_it, is_new_call] = RewriteCriticalNativeCall(call);
          if (is_new_call) {
            changed = true;
            bb_it = new_it;
            continue;
          }
        }

        ++bb_it;
      }
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }
    return llvm::PreservedAnalyses::none();
  }

  // Rounds n up to be 16-aligned.
  static int RoundUp16(int n) { return RoundUp(n, 16); }

  std::pair<llvm::BasicBlock::iterator, bool> RewriteCriticalNativeCall(llvm::CallBase* call) {
    constexpr uint32_t max_gpr_count = 8;  // x0-x7
    constexpr uint32_t max_fpr_count = 8;  // d0-d7

    llvm::SmallVector<llvm::Value*, 17> register_passed_arguments;
    llvm::SmallVector<llvm::Value*> stack_passed_arguments;
    uint32_t gprs_used = 0;
    uint32_t fprs_used = 0;

    for (llvm::Value* arg : call->args()) {
      // The method pointer argument will be passed in x15, so it shouldn't count towards the GPR
      // count.
      if (arg->getType()->isPointerTy() &&
          arg->getType()->getPointerAddressSpace() == kMethodPointerAddressSpace) {
        register_passed_arguments.push_back(arg);
        continue;
      }

      if (arg->getType()->isFloatingPointTy()) {
        if (fprs_used < max_fpr_count) {
          fprs_used += 1;
          register_passed_arguments.push_back(arg);
        } else {
          stack_passed_arguments.push_back(arg);
        }
      } else {
        DCHECK(arg->getType()->isIntegerTy() || arg->getType()->isPointerTy());
        if (gprs_used < max_gpr_count) {
          gprs_used += 1;
          register_passed_arguments.push_back(arg);
        } else {
          stack_passed_arguments.push_back(arg);
        }
      }
    }

    if (stack_passed_arguments.empty()) {
      // Nothing to do.
      return {{}, false};
    }

    llvm::IRBuilder<>& builder = *codegen_->GetIRBuilder();
    builder.SetInsertPoint(call);
    llvm::Value* sp_value = codegen_->GetStackPointerValue();
    const int stack_slot_size = static_cast<int>(kArm64PointerSize);
    const int stack_grow_size =
        RoundUp16(stack_slot_size * static_cast<int>(stack_passed_arguments.size()));

    int stack_offset = -stack_grow_size;
    llvm::Type* uncompressed_gc_ptr_type = builder.getPtrTy(kUncompressedGCAddressSpace);
    for (llvm::Value* argument : stack_passed_arguments) {
      llvm::Value* stack_slot = builder.CreatePtrAdd(sp_value, builder.getInt32(stack_offset));
      if (argument->getType() == uncompressed_gc_ptr_type) {
        argument = codegen_->CreateCastToCompressed(argument);
      }
      builder.CreateStore(argument, stack_slot);
      stack_offset += stack_slot_size;
    }

    llvm::Attribute statepoint_id_attr = call->getFnAttr("statepoint-id");
    CHECK(statepoint_id_attr.isValid());
    llvm::StringRef id_string = statepoint_id_attr.getValueAsString();
    uint64_t statepoint_id = 0;
    bool error = id_string.getAsInteger(10, statepoint_id);
    DCHECK(!error);
    DCHECK(DecodePatchpointKindFromID(statepoint_id) == PatchpointKind::kNone);
    uint32_t index = DecodePatchpointIndexFromID(statepoint_id);
    codegen_->SetStackmapInfoOffset(index, stack_grow_size);

    uint64_t new_id = EncodePatchpointID(PatchpointKind::kCriticalNativeCall, index);

    llvm::SmallVector<llvm::Value*, 18> new_arguments;
    new_arguments.append(register_passed_arguments);
    // Callee pointer, will be passed in lr.
    llvm::Value* callee = call->getCalledOperand();
    llvm::Type* critical_native_callee_type = builder.getPtrTy(kCriticalNativeFunctionAddressSpace);
    new_arguments.push_back(builder.CreateAddrSpaceCast(callee, critical_native_callee_type));

    llvm::SmallVector<llvm::OperandBundleDef, 1> deopt_bundle;
    std::optional<llvm::OperandBundleUse> deopt_bundle_use =
        call->getOperandBundle(llvm::LLVMContext::OB_deopt);
    if (deopt_bundle_use.has_value()) {
      std::vector<llvm::Value*> deopt_values(deopt_bundle_use->Inputs.begin(),
                                             deopt_bundle_use->Inputs.end());
      deopt_bundle.emplace_back("deopt", std::move(deopt_values));
    }

    llvm::FunctionType* callee_type = nullptr;
    {
      llvm::SmallVector<llvm::Type*, 18> argument_types;
      for (llvm::Value* arg : new_arguments) {
        argument_types.push_back(arg->getType());
      }
      callee_type = llvm::FunctionType::get(call->getType(), argument_types, /* isVarArg= */ false);
    }

    llvm::CallBase* new_call = nullptr;
    if (llvm::InvokeInst* invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
      new_call = builder.CreateInvoke(callee_type,
                                      callee,
                                      invoke->getNormalDest(),
                                      invoke->getUnwindDest(),
                                      new_arguments,
                                      deopt_bundle);
    } else {
      new_call = builder.CreateCall(callee_type, callee, new_arguments, deopt_bundle);
    }
    new_call->addFnAttr(
        llvm::Attribute::get(codegen_->GetLLVMContext(), "statepoint-id", std::to_string(new_id)));
    new_call->addFnAttr(llvm::Attribute::get(codegen_->GetLLVMContext(),
                                             "statepoint-num-patch-bytes",
                                             std::to_string(kCriticalNativePatchSize)));
    new_call->setCallingConv(llvm::CallingConv::ARTCriticalNative);

    // NOTE: Critical native calls don't have any extra argument or return value attributes, so we
    // don't need to add those to the new call.

    call->replaceAllUsesWith(new_call);
    llvm::BasicBlock::iterator new_it = call->eraseFromParent();

    return {new_it, true};
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

// This pass rewrites __cast_to_uncompressed, __cast_to_compressed, __cast_pointer_to_int, and
// __heap_reference_poisoning calls into the appropriate LLVM IR instructions, which should be
// done after RewriteStatepointsForGC has already run.
class RewriteTrivialPlaceholdersPass : public llvm::PassInfoMixin<RewriteTrivialPlaceholdersPass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    // Only run on functions marked with `gc "art"`.
    if (!function.hasGC() || function.getGC() != kArtGCStrategyName) {
      return llvm::PreservedAnalyses::all();
    }

    llvm::IRBuilder<> builder(function.getContext());
    bool changed = false;
    for (llvm::BasicBlock& basic_block : function) {
      llvm::BasicBlock::iterator bb_it = basic_block.begin();
      while (bb_it != basic_block.end()) {
        llvm::CallInst* call = llvm::dyn_cast<llvm::CallInst>(&*bb_it);
        if (call == nullptr || call->getCalledFunction() == nullptr) {
          ++bb_it;
          continue;
        }

        llvm::Function* called_function = call->getCalledFunction();
        llvm::Value* result_value = nullptr;
        builder.SetInsertPoint(call);
        if (called_function->hasMetadata(kGCPointerCastMetadata)) {
          DCHECK_EQ(call->arg_size(), 1u);
          result_value = builder.CreateAddrSpaceCast(call->getArgOperand(0), call->getType());
        } else if (called_function->hasMetadata(kGCPointerCastToIntMetadata)) {
          DCHECK_EQ(call->arg_size(), 1u);
          result_value = builder.CreatePtrToInt(call->getArgOperand(0), call->getType());
        } else if (called_function->hasMetadata(kHeapReferencePoisoningMetadata)) {
          DCHECK_EQ(call->arg_size(), 1u);
          llvm::Value* obj_int_value =
              builder.CreatePtrToInt(call->getArgOperand(0), builder.getInt32Ty());
          llvm::Value* poisoned_int_value = builder.CreateNeg(obj_int_value);
          result_value = builder.CreateIntToPtr(poisoned_int_value, call->getType());
        }

        if (result_value == nullptr) {
          ++bb_it;
          continue;
        }

        changed = true;
        call->replaceAllUsesWith(result_value);
        bb_it = call->eraseFromParent();
      }
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }

    return llvm::PreservedAnalyses::none();
  }
};

// ART doesn't handle relocations of derived pointers, so we need to eliminate them from the IR
// before code generation. the RewriteStatepointsForGC pass already eliminates some of these, but
// not all of them, so we need to do that manually. We do this elimination by calculating the
// offsets of the relocated derived pointers and applying those offsets to the base pointer after
// every relocation. Most of these will not be used in the code, so we rely on instcombine to run
// after this pass, which should eliminate these dead instructions.
class EliminateDerivedRelocationsPass
    : public llvm::PassInfoMixin<EliminateDerivedRelocationsPass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    // Only run on functions marked with `gc "art"`.
    if (!function.hasGC() || function.getGC() != kArtGCStrategyName) {
      return llvm::PreservedAnalyses::all();
    }

    llvm::SmallDenseSet<llvm::GCRelocateInst*> derived_relocations =
        CollectRelocatedDerivedPointers(function);
    if (derived_relocations.empty()) {
      return llvm::PreservedAnalyses::all();
    }

    llvm::IRBuilder<> builder(function.getContext());
    llvm::SmallDenseMap<llvm::Value*, llvm::Value*> derived_offset_map;
    for (llvm::GCRelocateInst* relocate : derived_relocations) {
      llvm::Value* offset = GetDerivedOffset(relocate, derived_offset_map);
      builder.SetInsertPoint(GetGEPInsertPoint(relocate));
      llvm::Value* relocated_base = GetRelocatedBasePointer(relocate);
      llvm::Value* new_derived_pointer =
          builder.CreatePtrAdd(relocated_base, offset, "", /* IsInBounds= */ true);
      relocate->replaceAllUsesWith(new_derived_pointer);
      relocate->eraseFromParent();
    }

    return llvm::PreservedAnalyses::none();
  }

  static llvm::Instruction* GetGEPInsertPoint(llvm::GCRelocateInst* relocate) {
    llvm::Instruction* result = relocate->getNextNode();
    while (llvm::isa<llvm::GCRelocateInst>(result)) {
      result = result->getNextNode();
    }
    return result;
  }

  static llvm::Value* GetRelocatedBasePointer(llvm::GCRelocateInst* derived_relocate) {
    DCHECK(derived_relocate->getBasePtrIndex() != derived_relocate->getDerivedPtrIndex());
    unsigned base_index = derived_relocate->getBasePtrIndex();
    llvm::Value* token = derived_relocate->getArgOperand(0);
    llvm::Value* relocated_base = nullptr;
    for (llvm::User* user : token->users()) {
      if (llvm::GCRelocateInst* relocate = llvm::dyn_cast<llvm::GCRelocateInst>(user)) {
        if (relocate->getDerivedPtrIndex() == base_index) {
          DCHECK_EQ(relocate->getBasePtrIndex(), base_index);
          relocated_base = relocate;
          break;
        }
      }
    }

    DCHECK(relocated_base != nullptr) << ValueToString(derived_relocate);
    return relocated_base;
  }

  static llvm::Value* RemoveRelocations(llvm::Value* relocate) {
    llvm::Value* result = relocate;
    while (llvm::GCRelocateInst* inner_relocate = llvm::dyn_cast<llvm::GCRelocateInst>(result)) {
      result = inner_relocate->getDerivedPtr();
    }
    return result;
  }

  static llvm::SmallDenseSet<llvm::GCRelocateInst*> CollectRelocatedDerivedPointers(
      llvm::Function& function) {
    llvm::SmallDenseSet<llvm::GCRelocateInst*> derived_relocations;

    for (llvm::BasicBlock& basic_block : function) {
      for (llvm::Instruction& inst : basic_block) {
        if (llvm::GCRelocateInst* relocate = llvm::dyn_cast<llvm::GCRelocateInst>(&inst)) {
          if (relocate->getBasePtrIndex() != relocate->getDerivedPtrIndex()) {
            DCHECK_EQ(relocate->getType()->getPointerAddressSpace(), kUncompressedGCAddressSpace)
                << ValueToString(relocate);
            derived_relocations.insert(relocate);
          }
        }
      }
    }

    return derived_relocations;
  }

  static llvm::Value* GetDerivedOffset(
      llvm::Value* derived_pointer,
      llvm::SmallDenseMap<llvm::Value*, llvm::Value*>& derived_offset_map) {
    derived_pointer = RemoveRelocations(derived_pointer);
    if (auto it = derived_offset_map.find(derived_pointer); it != derived_offset_map.end()) {
      return it->second;
    }

    llvm::IRBuilder<> builder(derived_pointer->getContext());
    if (llvm::GetElementPtrInst* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(derived_pointer)) {
      builder.SetInsertPoint(gep);

      DCHECK_EQ(gep->getNumIndices(), 1u);
      llvm::Value* index = gep->getOperand(1);
      DCHECK(index->getType()->isIntegerTy());
      if (index->getType() != builder.getInt64Ty()) {
        index = builder.CreateSExtOrTrunc(index, builder.getInt64Ty());
      }

      llvm::Value* incoming_offset = GetDerivedOffset(gep->getPointerOperand(), derived_offset_map);
      llvm::Value* index_offset = index;
      if (gep->getSourceElementType() != builder.getInt8Ty()) {
        llvm::TypeSize type_size =
            gep->getModule()->getDataLayout().getTypeAllocSize(gep->getSourceElementType());
        llvm::Value* type_size_value = builder.getInt64(type_size.getFixedValue());
        index_offset = builder.CreateMul(
            index, type_size_value, "", /* HasNUW= */ false, /* HasNSW= */ gep->isInBounds());
      }
      llvm::Value* offset = builder.CreateAdd(
          incoming_offset, index_offset, "", /* HasNUW= */ false, /* HasNSW= */ gep->isInBounds());
      derived_offset_map[derived_pointer] = offset;
      return offset;
    } else if (llvm::PHINode* phi = llvm::dyn_cast<llvm::PHINode>(derived_pointer)) {
      builder.SetInsertPoint(phi);
      unsigned num_incoming_values = phi->getNumIncomingValues();
      llvm::PHINode* offset_phi = builder.CreatePHI(builder.getInt64Ty(), num_incoming_values);
      // Set the offset to the new phi node to avoid creating this value multiple times.
      derived_offset_map[phi] = offset_phi;

      for (unsigned i = 0; i < num_incoming_values; ++i) {
        llvm::Value* incoming_offset =
            GetDerivedOffset(phi->getIncomingValue(i), derived_offset_map);
        offset_phi->addIncoming(incoming_offset, phi->getIncomingBlock(i));
      }
      return offset_phi;
    } else if (llvm::SelectInst* select = llvm::dyn_cast<llvm::SelectInst>(derived_pointer)) {
      builder.SetInsertPoint(select);
      llvm::Value* true_offset = GetDerivedOffset(select->getTrueValue(), derived_offset_map);
      llvm::Value* false_offset = GetDerivedOffset(select->getFalseValue(), derived_offset_map);
      llvm::Value* offset = builder.CreateSelect(select->getCondition(), true_offset, false_offset);
      derived_offset_map[select] = offset;
      return offset;
    } else if (llvm::AddrSpaceCastInst* addrspace_cast =
                   llvm::dyn_cast<llvm::AddrSpaceCastInst>(derived_pointer)) {
      return GetDerivedOffset(addrspace_cast->getPointerOperand(), derived_offset_map);
    }

    DCHECK(
        llvm::isa<llvm::CallBase>(derived_pointer) || llvm::isa<llvm::Argument>(derived_pointer) ||
        llvm::isa<llvm::LoadInst>(derived_pointer) || llvm::isa<llvm::Constant>(derived_pointer) ||
        llvm::isa<llvm::AtomicCmpXchgInst>(derived_pointer) ||
        llvm::isa<llvm::FreezeInst>(derived_pointer) ||
        llvm::isa<llvm::IntToPtrInst>(derived_pointer))
        << "Unknown derived pointer: " << ValueToString(derived_pointer);
    return builder.getInt64(0);
  }
};

// RewriteStatepointsForGC may generate unneeded phis at the top of unwind blocks, which need to be
// removed before code generation.
class EliminateUnwindRelocationPhisPass
    : public llvm::PassInfoMixin<EliminateUnwindRelocationPhisPass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    // Only run on functions marked with `gc "art"`.
    if (!function.hasGC() || function.getGC() != kArtGCStrategyName) {
      return llvm::PreservedAnalyses::all();
    }

    bool changed = false;
    for (llvm::BasicBlock& basic_block : function) {
      llvm::BasicBlock::iterator first_inst = basic_block.getFirstNonPHIIt();
      if (first_inst != basic_block.begin() && llvm::isa<llvm::ARTCatchSwitchInst>(first_inst)) {
        auto it = basic_block.begin();
        while (it != basic_block.end() && llvm::isa<llvm::PHINode>(&*it)) {
          if (it->getType()->getScalarType()->isPointerTy()) {
            unsigned address_space = it->getType()->getScalarType()->getPointerAddressSpace();
            if (address_space == kUncompressedGCAddressSpace ||
                address_space == kCompressedGCPointerAddressSpace) {
              // Replace these values with poison, as they should never be used.
              // TODO: Check whether instcombine can remove relocations using these values.
              it->replaceAllUsesWith(llvm::PoisonValue::get(it->getType()));
              it = it->eraseFromParent();
              changed = true;
            }
          } else {
            ++it;
          }
        }
      }
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }
    return llvm::PreservedAnalyses::none();
  }
};

// Checks that we have removed all derived pointer relocations.
class VerifyStatepointsPass : public llvm::PassInfoMixin<VerifyStatepointsPass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    // Only run on functions marked with `gc "art"`.
    if (!function.hasGC() || function.getGC() != kArtGCStrategyName) {
      return llvm::PreservedAnalyses::all();
    }

    for (llvm::BasicBlock& basic_block : function) {
      for (llvm::Instruction& inst : basic_block) {
        if (llvm::GCRelocateInst* relocate = llvm::dyn_cast<llvm::GCRelocateInst>(&inst)) {
          CHECK(relocate->getBasePtrIndex() == relocate->getDerivedPtrIndex())
              << "Base pointer and derived pointer differ in @llvm.experimental.gc.relocate() "
                 "call: "
              << ValueToString(relocate);
        }
      }
    }

    return llvm::PreservedAnalyses::all();
  }
};

// Pass used for creating @llvm.experimental.patchpoint.*() intrinsics for non-call instructions
// that need stack map entries in the final oat file. We achieve this by taking advantage of the
// llvm::RewriteStatepointsForGC pass, which collects all live GC pointers at a function call site,
// and rewriting the resulting @llvm.experimental.gc.statepoint() and subsequent relocations into a
// patchpoint, which records the locations of all live GC pointers at that instruction. For each
// patchpoint, the resulting object file needs to be patched with the appropriate instructions,
// which is done in CodeGeneratorARM64LLVM::ParseStackMap().
class RewritePatchpointsPass : public llvm::PassInfoMixin<RewritePatchpointsPass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    // Nothing to do for declarations.
    if (function.isDeclaration() || function.empty()) {
      return llvm::PreservedAnalyses::all();
    }

    // Only run on functions marked with `gc "art"`.
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    bool changed = false;
    for (llvm::BasicBlock& basic_block : function) {
      changed |= RunOnBasicBlock(basic_block);
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }

    // Conservatively return ::none() here. TODO: Can we do better?
    return llvm::PreservedAnalyses::none();
  }

  static uint32_t GetNumberOfPatchBytes(PatchpointKind patchpoint_kind) {
    switch (patchpoint_kind) {
      case PatchpointKind::kNone:
      case PatchpointKind::kGCPointerAllocaMap:
      case PatchpointKind::kTryBoundaryStackReadClobber:
      case PatchpointKind::kReachabilityFence:
      case PatchpointKind::kCriticalNativeCall:
        LOG(FATAL) << "Unreachable";
        UNREACHABLE();
      case PatchpointKind::kImplicitSuspendCheck:
        // ldr x21, [x21]
        return vixl::aarch64::kInstructionSize;
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
        // ld[a]r[b|h] (w|x|s|d)A, [xB, #offset]
        return vixl::aarch64::kInstructionSize;
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
        // st[l]r[b|h] (w|x|s|d)A, [xB, #offset]
        return vixl::aarch64::kInstructionSize;
      case PatchpointKind::kLoadClassBootImageLinkTimePcRelative:
      case PatchpointKind::kLoadClassBootImageRelRo:
      case PatchpointKind::kLoadClassAppImageRelRo:
      case PatchpointKind::kLoadClassBootImageIntrinsic:
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
        // This pass shouldn't encounter these patchpoints, because they don't need stack map
        // entries.
        LOG(FATAL) << "Unreachable";
        UNREACHABLE();
    }
  }

  static size_t GetNumberOfRegisterArguments(PatchpointKind patchpoint_kind,
                                             size_t number_of_arguments) {
    switch (patchpoint_kind) {
      case PatchpointKind::kLoadGcRoot:
      case PatchpointKind::kLoadBoolean:
      case PatchpointKind::kLoadInt8:
      case PatchpointKind::kLoadInt16:
      case PatchpointKind::kLoadInt32:
      case PatchpointKind::kLoadInt64:
      case PatchpointKind::kLoadFloat32:
      case PatchpointKind::kLoadFloat64:
        // Loads have an extra constant offset argument, which shouldn't be put into a register.
        DCHECK_EQ(number_of_arguments, 2u);
        return number_of_arguments - 1;
      case PatchpointKind::kStoreGcRoot:
      case PatchpointKind::kStoreBoolean:
      case PatchpointKind::kStoreInt8:
      case PatchpointKind::kStoreInt16:
      case PatchpointKind::kStoreInt32:
      case PatchpointKind::kStoreInt64:
      case PatchpointKind::kStoreFloat32:
      case PatchpointKind::kStoreFloat64:
        // Stores have an extra constant offset argument, which shouldn't be put into a register.
        DCHECK_EQ(number_of_arguments, 3u);
        return number_of_arguments - 1;
      case PatchpointKind::kNone:
      case PatchpointKind::kGCPointerAllocaMap:
      case PatchpointKind::kTryBoundaryStackReadClobber:
      case PatchpointKind::kImplicitSuspendCheck:
      case PatchpointKind::kReachabilityFence:
      case PatchpointKind::kCriticalNativeCall:
      case PatchpointKind::kLoadAcquireGcRoot:
      case PatchpointKind::kLoadAcquireBoolean:
      case PatchpointKind::kLoadAcquireInt8:
      case PatchpointKind::kLoadAcquireInt16:
      case PatchpointKind::kLoadAcquireInt32:
      case PatchpointKind::kLoadAcquireInt64:
      case PatchpointKind::kDiscardedLoad:
      case PatchpointKind::kStoreReleaseGcRoot:
      case PatchpointKind::kStoreReleaseBoolean:
      case PatchpointKind::kStoreReleaseInt8:
      case PatchpointKind::kStoreReleaseInt16:
      case PatchpointKind::kStoreReleaseInt32:
      case PatchpointKind::kStoreReleaseInt64:
      case PatchpointKind::kLoadClassBootImageLinkTimePcRelative:
      case PatchpointKind::kLoadClassBootImageRelRo:
      case PatchpointKind::kLoadClassAppImageRelRo:
      case PatchpointKind::kLoadClassBootImageIntrinsic:
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
        return number_of_arguments;
    }
  }

  static llvm::SmallVector<llvm::Value*> GetPatchpointArgs(llvm::GCStatepointInst* statepoint,
                                                           PatchpointKind kind) {
    llvm::LLVMContext& llvm_context = statepoint->getContext();
    llvm::Type* i32_type = llvm::Type::getInt32Ty(llvm_context);
    llvm::Type* method_pointer_type =
        llvm::PointerType::get(llvm_context, kMethodPointerAddressSpace);
    llvm::SmallVector<llvm::Value*> patchpoint_args;

    // ID
    llvm::Value* id = statepoint->getArgOperand(llvm::GCStatepointInst::IDPos);
    patchpoint_args.push_back(id);
    // number_of_bytes
    uint32_t number_of_bytes = GetNumberOfPatchBytes(kind);
    patchpoint_args.push_back(llvm::ConstantInt::get(i32_type, number_of_bytes));

    // function
    patchpoint_args.push_back(
        llvm::ConstantPointerNull::get(llvm::PointerType::get(llvm_context, 0)));
    // number_of_arguments
    size_t number_of_arguments = GetNumberOfRegisterArguments(kind, statepoint->actual_arg_size());
    patchpoint_args.push_back(llvm::ConstantInt::get(i32_type, number_of_arguments + 1));
    // arguments
    patchpoint_args.push_back(llvm::UndefValue::get(method_pointer_type));
    patchpoint_args.append(statepoint->actual_arg_begin(), statepoint->actual_arg_end());

    // Number of deopt values.
    size_t deopt_size = statepoint->deopt_end() - statepoint->deopt_begin();
    patchpoint_args.push_back(llvm::ConstantInt::get(i32_type, deopt_size));
    // Deopt values.
    patchpoint_args.append(statepoint->deopt_begin(), statepoint->deopt_end());
    // GC live values.
    patchpoint_args.append(statepoint->gc_live_begin(), statepoint->gc_live_end());

    return patchpoint_args;
  }

  static llvm::SmallVector<llvm::Value*> GetPatchpointArgs(llvm::CallBase* call,
                                                           uint64_t id,
                                                           PatchpointKind kind) {
    llvm::LLVMContext& llvm_context = call->getContext();
    llvm::Type* i32_type = llvm::Type::getInt32Ty(llvm_context);
    llvm::Type* i64_type = llvm::Type::getInt64Ty(llvm_context);
    llvm::Type* method_pointer_type =
        llvm::PointerType::get(llvm_context, kMethodPointerAddressSpace);
    llvm::SmallVector<llvm::Value*> patchpoint_args;

    // ID
    patchpoint_args.push_back(llvm::ConstantInt::get(i64_type, id));
    // number_of_bytes
    uint32_t number_of_bytes = GetNumberOfPatchBytes(kind);
    patchpoint_args.push_back(llvm::ConstantInt::get(i32_type, number_of_bytes));

    // function
    patchpoint_args.push_back(
        llvm::ConstantPointerNull::get(llvm::PointerType::get(llvm_context, 0)));
    // number_of_arguments
    size_t number_of_arguments = GetNumberOfRegisterArguments(kind, call->arg_size());
    patchpoint_args.push_back(llvm::ConstantInt::get(i32_type, number_of_arguments + 1));
    // arguments
    patchpoint_args.push_back(llvm::UndefValue::get(method_pointer_type));
    patchpoint_args.append(call->arg_begin(), call->arg_end());

    std::optional<llvm::OperandBundleUse> deopt_bundle =
        call->getOperandBundle(llvm::LLVMContext::OB_deopt);
    if (deopt_bundle) {
      // Number of deopt values.
      size_t deopt_size = deopt_bundle->Inputs.size();
      patchpoint_args.push_back(llvm::ConstantInt::get(i32_type, deopt_size));
      // Deopt values.
      patchpoint_args.append(deopt_bundle->Inputs.begin(), deopt_bundle->Inputs.end());
    } else {
      // Number of deopt values.
      patchpoint_args.push_back(llvm::ConstantInt::get(i32_type, 0));
    }

    return patchpoint_args;
  }

  static void ReplaceRelocations(llvm::Instruction* token, llvm::CallBase* patchpoint_call) {
    DCHECK(llvm::isa<llvm::GCStatepointInst>(token) || llvm::isa<llvm::LandingPadInst>(token));
    // Replace all relocated pointers with their original values and remove them.
    while (!token->materialized_uses().empty()) {
      llvm::User* statepoint_user = token->use_begin()->getUser();
      DCHECK(statepoint_user != nullptr);
      // gc.result calls are only present for non-void return types, otherwise only gc.relocate can
      // use the token, since the original function returns void.
      DCHECK_IMPLIES(patchpoint_call->getType()->isVoidTy(),
                     llvm::isa<llvm::GCRelocateInst>(statepoint_user));
      if (llvm::GCRelocateInst* relocate = llvm::dyn_cast<llvm::GCRelocateInst>(statepoint_user)) {
        llvm::Value* original_ptr = relocate->getDerivedPtr();
        DCHECK(relocate->getType() == original_ptr->getType());
        relocate->replaceAllUsesWith(original_ptr);
        DCHECK_EQ(relocate->getNumUses(), 0u);
        relocate->eraseFromParent();
      } else {
        DCHECK(llvm::isa<llvm::GCResultInst>(statepoint_user));
        llvm::GCResultInst* result = llvm::cast<llvm::GCResultInst>(statepoint_user);
        DCHECK(result->getType() == patchpoint_call->getType());
        result->replaceAllUsesWith(patchpoint_call);
        DCHECK_EQ(result->getNumUses(), 0u);
        result->eraseFromParent();
      }
    }
  }

  static void AddMemOperandAttributes(PatchpointKind kind, llvm::CallBase* patchpoint_call) {
    switch (kind) {
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
        patchpoint_call->addParamAttr(
            5, llvm::Attribute::get(patchpoint_call->getContext(), "memoperand"));
        break;
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
        patchpoint_call->addParamAttr(
            6, llvm::Attribute::get(patchpoint_call->getContext(), "memoperand"));
        break;
      case PatchpointKind::kNone:
      case PatchpointKind::kGCPointerAllocaMap:
      case PatchpointKind::kTryBoundaryStackReadClobber:
      case PatchpointKind::kImplicitSuspendCheck:
      case PatchpointKind::kReachabilityFence:
      case PatchpointKind::kCriticalNativeCall:
      case PatchpointKind::kLoadClassBootImageLinkTimePcRelative:
      case PatchpointKind::kLoadClassBootImageRelRo:
      case PatchpointKind::kLoadClassAppImageRelRo:
      case PatchpointKind::kLoadClassBootImageIntrinsic:
      case PatchpointKind::kLoadClassBssEntry:
      case PatchpointKind::kLoadClassBssEntryPublic:
      case PatchpointKind::kLoadClassBssEntryPackage:
      case PatchpointKind::kLoadStringBootImageRelRo:
      case PatchpointKind::kLoadStringBootImageLinkTimePcRelative:
      case PatchpointKind::kLoadStringBssEntry:
      case PatchpointKind::kLoadMethodTypeBssEntry:
      case PatchpointKind::kLoadMethodBootImageLinkTimePcRelative:
      case PatchpointKind::kLoadMethodBootImageRelRo:
      case PatchpointKind::kLoadMethodAppImageRelRo:
      case PatchpointKind::kLoadMethodBootImageJni:
      case PatchpointKind::kLoadMethodBssEntry:
      case PatchpointKind::kEntrypointThunkCallStatepoint:
      case PatchpointKind::kEntrypointThunkCallPatchpoint:
      case PatchpointKind::kCatchBlock:
        break;
    }
  }

  static llvm::CallBase* CreatePatchpointCall(PatchpointKind kind,
                                              llvm::Type* result_type,
                                              llvm::ArrayRef<llvm::Value*> args,
                                              llvm::Instruction* replaced_call) {
    llvm::IRBuilder<> builder(replaced_call);
    llvm::Function* patchpoint_callee = nullptr;
    llvm::Module* module = replaced_call->getModule();
    if (result_type->isVoidTy()) {
      patchpoint_callee = llvm::Intrinsic::getOrInsertDeclaration(
          module, llvm::Intrinsic::experimental_patchpoint_void);
    } else {
      patchpoint_callee = llvm::Intrinsic::getOrInsertDeclaration(
          module, llvm::Intrinsic::experimental_patchpoint, {result_type});
    }
    llvm::CallBase* patchpoint_call = nullptr;
    if (llvm::InvokeInst* invoke = llvm::dyn_cast<llvm::InvokeInst>(replaced_call)) {
      patchpoint_call = builder.CreateInvoke(
          patchpoint_callee, invoke->getNormalDest(), invoke->getUnwindDest(), args);
    } else {
      patchpoint_call = builder.CreateCall(patchpoint_callee, args);
    }
    patchpoint_call->setCallingConv(llvm::CallingConv::ARTAnyReg);
    AddMemOperandAttributes(kind, patchpoint_call);

    return patchpoint_call;
  }

  static bool RewriteStatepoint(llvm::GCStatepointInst* statepoint,
                                llvm::BasicBlock::iterator& bb_it) {
    llvm::Function* called_function = statepoint->getActualCalledFunction();
    if (called_function == nullptr) {
      return false;
    }
    PatchpointKind patchpoint_kind = DecodePatchpointKindFromID(statepoint->getID());
    if (patchpoint_kind == PatchpointKind::kNone ||
        patchpoint_kind == PatchpointKind::kCriticalNativeCall ||
        patchpoint_kind == PatchpointKind::kImplicitSuspendCheck ||
        patchpoint_kind == PatchpointKind::kEntrypointThunkCallStatepoint) {
      return false;
    }

    llvm::SmallVector<llvm::Value*> patchpoint_args =
        GetPatchpointArgs(statepoint, patchpoint_kind);

    llvm::CallBase* patchpoint_call = CreatePatchpointCall(
        patchpoint_kind, called_function->getReturnType(), patchpoint_args, statepoint);

    ReplaceRelocations(statepoint, patchpoint_call);
    // NOTE: We don't have to replace any relocations in the unwind block in case this was an
    // invoke instruction.

    DCHECK_EQ(statepoint->getNumUses(), 0u);
    statepoint->eraseFromParent();
    // Move the iterator to the new patchpoint intrinsic call.
    bb_it = llvm::BasicBlock::iterator(patchpoint_call);
    return true;
  }

  static bool RewriteCall(llvm::CallBase* call, llvm::BasicBlock::iterator& bb_it) {
    llvm::Function* called_function = call->getCalledFunction();
    if (called_function == nullptr || !called_function->hasMetadata(kRewriteToPatchpointMetadata)) {
      return false;
    }

    llvm::Attribute statepoint_id = call->getFnAttr("statepoint-id");
    if (!statepoint_id.isValid()) {
      return false;
    }
    llvm::StringRef id_string = statepoint_id.getValueAsString();
    uint64_t id = 0;
    bool error = id_string.getAsInteger(10, id);
    DCHECK(!error);

    PatchpointKind patchpoint_kind = DecodePatchpointKindFromID(id);
    llvm::SmallVector<llvm::Value*> patchpoint_args = GetPatchpointArgs(call, id, patchpoint_kind);

    llvm::CallBase* patchpoint_call = CreatePatchpointCall(
        patchpoint_kind, called_function->getReturnType(), patchpoint_args, call);

    // Inherit return attributes from the call. This can help code generation for
    // __load_* functions in some cases.
    for (llvm::Attribute::AttrKind attribute : {
             llvm::Attribute::ZExt,
             llvm::Attribute::SExt,
         }) {
      if (call->hasRetAttr(attribute)) {
        patchpoint_call->addRetAttr(attribute);
      }
    }

    if (!called_function->getReturnType()->isVoidTy()) {
      call->replaceAllUsesWith(patchpoint_call);
    }
    DCHECK_EQ(call->getNumUses(), 0u);
    call->eraseFromParent();
    // Move the iterator to the new patchpoint intrinsic call.
    bb_it = llvm::BasicBlock::iterator(patchpoint_call);
    return true;
  }

  bool RunOnBasicBlock(llvm::BasicBlock& basic_block) {
    bool changed = false;
    for (auto bb_it = basic_block.begin(); bb_it != basic_block.end(); ++bb_it) {
      llvm::Instruction& inst = *bb_it;
      if (llvm::GCStatepointInst* statepoint = llvm::dyn_cast<llvm::GCStatepointInst>(&inst)) {
        changed |= RewriteStatepoint(statepoint, bb_it);
      } else if (llvm::CallBase* call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        changed |= RewriteCall(call, bb_it);
      }
    }
    return changed;
  }
};

// Erases the functions in statepoints which are used for runtime patching only, e.g. implicit
// suspend checks. This removes any references to placeholder functions, which we eliminate before
// final code generation.
class ErasePatchedStatepointArgumentsPass
    : public llvm::PassInfoMixin<ErasePatchedStatepointArgumentsPass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    for (llvm::BasicBlock& basic_block : function) {
      for (llvm::Instruction& inst : basic_block) {
        llvm::GCStatepointInst* statepoint = llvm::dyn_cast<llvm::GCStatepointInst>(&inst);
        if (statepoint == nullptr) {
          continue;
        }
        llvm::Value* num_patch_bytes =
            statepoint->getArgOperand(llvm::GCStatepointInst::NumPatchBytesPos);
        DCHECK(llvm::isa<llvm::ConstantInt>(num_patch_bytes));
        if (llvm::cast<llvm::ConstantInt>(num_patch_bytes)->getZExtValue() != 0) {
          statepoint->setArgOperand(
              llvm::GCStatepointInst::CalledFunctionPos,
              llvm::ConstantPointerNull::get(llvm::PointerType::get(function.getContext(), 0)));
          statepoint->removeParamAttr(llvm::GCStatepointInst::CalledFunctionPos,
                                      llvm::Attribute::AttrKind::NonNull);
        }
      }
    }

    return llvm::PreservedAnalyses::all();
  }
};

// Function parameters have designated stack slots in the caller's frame, which we should use for
// storing these values to the stack. This pass replaces function parameter allocas
// with stack addresses using offsets from the entry stack pointer value.
class RemoveFunctionParameterStackSlotsPass
    : public llvm::PassInfoMixin<RemoveFunctionParameterStackSlotsPass> {
 public:
  RemoveFunctionParameterStackSlotsPass(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    llvm::SmallDenseMap<llvm::AllocaInst*, unsigned> allocas;
    RecordAllocasForRemoval(function.getEntryBlock(), allocas);
    if (allocas.empty()) {
      return llvm::PreservedAnalyses::all();
    }

    // Insert replacements after allocas
    auto it = function.getEntryBlock().begin();
    while (llvm::isa<llvm::AllocaInst>(*it)) {
      ++it;
    }
    llvm::IRBuilder<> builder(&*it);

    // Replace function parameter allocas with the stack address pointer
    for (auto [alloca, arg_index] : allocas) {
      DCHECK(alloca != nullptr);
      unsigned parameter_index = allocas.at(alloca);
      uint32_t stack_offset = codegen_->GetParameterStackOffset(parameter_index);
      llvm::Value* sp_on_entry =
          builder.CreateIntrinsic(llvm::Intrinsic::sponentry, {builder.getPtrTy()}, {});
      llvm::Value* parameter_stack_address =
          builder.CreateConstGEP1_64(builder.getInt8Ty(), sp_on_entry, stack_offset);

      // Remove possible lifetime intrinsics, which can be introduced with recursive inlining.
      for (llvm::User* user : alloca->users()) {
        llvm::LifetimeIntrinsic* lifetime_intrinsic = llvm::dyn_cast<llvm::LifetimeIntrinsic>(user);
        if (lifetime_intrinsic == nullptr) {
          continue;
        }
        lifetime_intrinsic->eraseFromParent();
      }
      alloca->replaceAllUsesWith(parameter_stack_address);
      alloca->eraseFromParent();
    }

    return llvm::PreservedAnalyses::none();
  }

  static void AddAlloca(llvm::SmallDenseMap<llvm::AllocaInst*, unsigned>& allocas,
                        llvm::AllocaInst* alloca,
                        unsigned index) {
    allocas[alloca] = index;
  }

  void RecordAllocasForRemoval(llvm::BasicBlock& basic_block,
                               llvm::SmallDenseMap<llvm::AllocaInst*, unsigned>& allocas) {
    for (llvm::Instruction& inst : basic_block) {
      llvm::AllocaInst* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst);
      if (alloca == nullptr) {
        continue;
      }
      llvm::MDNode* parameter_index_metadata = alloca->getMetadata(kParameterAllocaMetadata);
      if (parameter_index_metadata == nullptr) {
        continue;
      }
      DCHECK(llvm::isa<llvm::ConstantAsMetadata>(parameter_index_metadata->getOperand(0)));
      llvm::ConstantAsMetadata* parameter_index_node =
          llvm::cast<llvm::ConstantAsMetadata>(parameter_index_metadata->getOperand(0));
      DCHECK(llvm::isa<llvm::ConstantInt>(parameter_index_node->getValue()));
      unsigned parameter_index =
          llvm::cast<llvm::ConstantInt>(parameter_index_node->getValue())->getZExtValue();

      // Record the alloca for removal.
      AddAlloca(allocas, alloca, parameter_index);
      codegen_->SetParameterStackSlotUsed(parameter_index);
    }
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

// Pass used for collecting allocas that contain heap references, which needs to be put into stack
// map entries in try blocks.
class CollectGCPointerStackSlotsPass : public llvm::PassInfoMixin<CollectGCPointerStackSlotsPass> {
 public:
  struct StackSlotCollectionResult {
    llvm::BasicBlock::iterator insert_point;
    llvm::SmallVector<llvm::AllocaInst*> allocas;
  };

  static StackSlotCollectionResult GetStackmapInsertPoint(llvm::BasicBlock& entry_block) {
    StackSlotCollectionResult result{entry_block.begin(), {}};
    llvm::BasicBlock::iterator& it = result.insert_point;
    llvm::SmallVector<llvm::AllocaInst*>& allocas = result.allocas;
    // Place the stackmap after the allocas.
    while (llvm::AllocaInst* alloca = llvm::dyn_cast<llvm::AllocaInst>(&*it)) {
      if (alloca->hasMetadata(kGCPointerAllocaMetadata)) {
        allocas.push_back(alloca);
      }
      ++it;
    }
    return result;
  }

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    StackSlotCollectionResult result = GetStackmapInsertPoint(function.getEntryBlock());
    if (result.allocas.empty()) {
      return llvm::PreservedAnalyses::all();
    }

    llvm::IRBuilder<> builder(&function.getEntryBlock(), result.insert_point);
    // Create a stackmap intrinsic that records the positions of the alloca stack slots.
    llvm::SmallVector<llvm::Value*> arguments;
    arguments.reserve(2 + 2 * result.allocas.size());
    // ID
    uint64_t id = EncodePatchpointID(PatchpointKind::kGCPointerAllocaMap, 0);
    arguments.push_back(builder.getInt64(id));
    // num_shadow_bytes
    arguments.push_back(builder.getInt32(0));
    for (llvm::AllocaInst* alloca : result.allocas) {
      arguments.push_back(alloca);
      llvm::MDNode* instruction_id_metadata = alloca->getMetadata(kInstructionAllocaMetadata);
      DCHECK(instruction_id_metadata != nullptr);
      DCHECK(llvm::isa<llvm::ConstantAsMetadata>(instruction_id_metadata->getOperand(0)));
      llvm::ConstantAsMetadata* instruction_id_node =
          llvm::cast<llvm::ConstantAsMetadata>(instruction_id_metadata->getOperand(0));
      arguments.push_back(instruction_id_node->getValue());
    }
    builder.CreateIntrinsic(llvm::Intrinsic::experimental_stackmap, {}, arguments);

    // Conservatively return ::none() here. TODO: Can we do better?
    return llvm::PreservedAnalyses::none();
  }
};

// Pass to rewrite manually vectorized loop placeholder functions to the actual loop. This pass
// should be run after every other optimization pass, except for SimplifyCFG, otherwise we won't
// generate the desired machine code.
class RewriteInlineVectorLoopsPass : public llvm::PassInfoMixin<RewriteInlineVectorLoopsPass> {
 public:
  RewriteInlineVectorLoopsPass(CodeGeneratorARM64LLVM* codegen) : codegen_(codegen) {}

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    if (!IsArtMethod(&function)) {
      return llvm::PreservedAnalyses::all();
    }

    bool changed = false;
    for (llvm::BasicBlock* block = &function.getEntryBlock(); block != nullptr;
         block = block->getNextNode()) {
      llvm::BasicBlock::iterator it = block->begin();
      while (it != block->end()) {
        if (llvm::CallInst* call = llvm::dyn_cast<llvm::CallInst>(&*it)) {
          if (llvm::Function* called_function = call->getCalledFunction()) {
            if (called_function->hasMetadata(kMemCpyI16Metadata)) {
              it = RewriteMemCpyI16(call, codegen_);
              block = it->getParent();
              changed = true;
              continue;
            }

            if (called_function->hasMetadata(kMemCpyI32Metadata)) {
              it = RewriteMemCpyI32(call, codegen_);
              block = it->getParent();
              changed = true;
              continue;
            }

            if (called_function->hasMetadata(kMemCpyI8ZextToI16Metadata)) {
              it = RewriteMemCpyI8ZextToI16(call, codegen_);
              block = it->getParent();
              changed = true;
              continue;
            }

            if (called_function->hasMetadata(kStringEqualsMetadata)) {
              it = RewriteStringEquals(call, codegen_);
              block = it->getParent();
              changed = true;
              continue;
            }
          }
        }

        ++it;
      }
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }
    return llvm::PreservedAnalyses::none();
  }

 private:
  CodeGeneratorARM64LLVM* codegen_;
};

// This pass tries to optimize loads and stores using pointers loaded from registers, e.g. x19 or
// sp.
class OptimizeRegisterLoadStoresPass : public llvm::PassInfoMixin<OptimizeRegisterLoadStoresPass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    bool changed = false;
    for (llvm::BasicBlock& basic_block : function) {
      for (llvm::Instruction& inst : basic_block) {
        changed |= OptimizeInstruction(&inst);
      }
    }

    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }
    return llvm::PreservedAnalyses::none();
  }

  // Searches for the following pattern:
  //   %reg = call i64 @llvm.read_register.i64()
  //   %base = inttoptr i64 %reg to ptr
  //   %pointer = getelementptr i8, ptr %base, i64 %index
  // or:
  //   %base = call ptr @llvm.sponentry.p0()
  //   %pointer = getelementptr i8, ptr %base, i64 %index
  // If found, returns the duplicated value of %pointer, otherwise returns null.
  llvm::Value* GetOptimizedPointerOperand(llvm::Value* pointer, llvm::Instruction* insert_point) {
    llvm::Value* gep_offset = nullptr;
    llvm::GetElementPtrInst* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
    if (gep != nullptr && gep->getNumIndices() == 1) {
      gep_offset = gep->getOperand(1);
      pointer = gep->getPointerOperand();
    }

    if (llvm::IntToPtrInst* int_to_ptr = llvm::dyn_cast<llvm::IntToPtrInst>(pointer)) {
      llvm::IntrinsicInst* read_register =
          llvm::dyn_cast<llvm::IntrinsicInst>(int_to_ptr->getOperand(0));
      if (read_register == nullptr ||
          read_register->getIntrinsicID() != llvm::Intrinsic::read_register) {
        return nullptr;
      }

      llvm::IRBuilder<> builder(insert_point);
      llvm::Value* result = builder.CreateIntrinsic(
          llvm::Intrinsic::read_register, builder.getInt64Ty(), read_register->getArgOperand(0));
      result = builder.CreateIntToPtr(result, builder.getPtrTy());
      if (gep_offset != nullptr) {
        result = builder.CreateGEP(
            gep->getSourceElementType(), result, gep_offset, "", gep->isInBounds());
      }
      return result;
    } else if (llvm::IntrinsicInst* sponentry = llvm::dyn_cast<llvm::IntrinsicInst>(pointer);
               sponentry != nullptr && sponentry->getIntrinsicID() == llvm::Intrinsic::sponentry) {
      llvm::IRBuilder<> builder(insert_point);
      llvm::Value* result =
          builder.CreateIntrinsic(llvm::Intrinsic::sponentry, builder.getPtrTy(), {});
      if (gep_offset != nullptr) {
        result = builder.CreateGEP(
            gep->getSourceElementType(), result, gep_offset, "", gep->isInBounds());
      }
      return result;
    }

    return nullptr;
  }

  bool OptimizeInstruction(llvm::Instruction* inst) {
    if (llvm::LoadInst* load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
      if (llvm::Value* new_pointer = GetOptimizedPointerOperand(load->getPointerOperand(), inst)) {
        load->setOperand(load->getPointerOperandIndex(), new_pointer);
        return true;
      }
    } else if (llvm::StoreInst* store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
      if (llvm::Value* new_pointer = GetOptimizedPointerOperand(store->getPointerOperand(), inst)) {
        store->setOperand(store->getPointerOperandIndex(), new_pointer);
        return true;
      }
    }

    return false;
  }
};

// This pass reorders the values in the "gc-live" operand bundle in order to reduce the number of
// stack spills and restores of GC pointers. The values are reordered based on how soon they are
// used next after the relocation. Currently we only count the number of relocations of the pointer
// until its next use.
class OptimizeStatepointRegisterOrderPass
    : public llvm::PassInfoMixin<OptimizeStatepointRegisterOrderPass> {
 public:
  struct RelocationInfo {
    llvm::GCRelocateInst* relocation;
    uint32_t next_use_distance;
    uint32_t id;  // Tie-break used in comparison.

    bool operator<(const RelocationInfo& rhs) const {
      return std::tie(next_use_distance, id) < std::tie(rhs.next_use_distance, rhs.id);
    }
  };

  struct StatepointInfo {
    llvm::GCResultInst* result = nullptr;
    llvm::SmallDenseMap<llvm::Value*, RelocationInfo> relocation_infos{};
  };

  using StatepointInfoMap = llvm::SmallDenseMap<llvm::GCStatepointInst*, StatepointInfo>;

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    StatepointInfoMap statepoint_infos = CollectStatepointInfo(function);

    for (auto& [statepoint, statepoint_info] : statepoint_infos) {
      llvm::SmallVector<std::pair<llvm::Value*, RelocationInfo>> relocation_infos(
          statepoint_info.relocation_infos.begin(), statepoint_info.relocation_infos.end());
      std::sort(relocation_infos.begin(),
                relocation_infos.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });

      llvm::SmallDenseMap<llvm::Value*, llvm::Constant*> pointer_to_index_map;
      llvm::Type* i32_type = llvm::Type::getInt32Ty(function.getContext());
      for (size_t i = 0; i < relocation_infos.size(); ++i) {
        pointer_to_index_map[relocation_infos[i].first] = llvm::ConstantInt::get(i32_type, i);
      }

      // Update all relocation indices.
      for (llvm::User* user : statepoint->users()) {
        if (llvm::GCRelocateInst* relocate = llvm::dyn_cast<llvm::GCRelocateInst>(user)) {
          DCHECK_EQ(relocate->getBasePtrIndex(), relocate->getDerivedPtrIndex());
          llvm::Constant* new_index = pointer_to_index_map[relocate->getDerivedPtr()];
          relocate->setArgOperand(1, new_index);
          relocate->setArgOperand(2, new_index);
        }
      }

      std::vector<llvm::Value*> new_gc_live;
      new_gc_live.reserve(relocation_infos.size());
      for (auto& [ptr, relocation_info] : relocation_infos) {
        new_gc_live.push_back(ptr);
      }

      // Create the new statepoint instruction and replace the old one with it.
      llvm::OperandBundleDef new_bundle("gc-live", std::move(new_gc_live));
      llvm::CallBase* new_statepoint = llvm::CallBase::Create(
          statepoint, std::move(new_bundle), /* InsertPt= */ statepoint->getIterator());
      statepoint->replaceAllUsesWith(new_statepoint);
      statepoint->eraseFromParent();
    }

    return llvm::PreservedAnalyses::none();
  }

  static llvm::SmallDenseMap<llvm::Value*, llvm::GCRelocateInst*> GetPtrToRelocationMap(
      llvm::GCStatepointInst* statepoint) {
    llvm::SmallDenseMap<llvm::Value*, llvm::GCRelocateInst*> relocation_map;
    for (llvm::User* user : statepoint->users()) {
      if (llvm::GCRelocateInst* relocation = llvm::dyn_cast<llvm::GCRelocateInst>(user)) {
        relocation_map.insert({relocation->getDerivedPtr(), relocation});
      }
    }

    return relocation_map;
  }

  static llvm::GCResultInst* GetStatepointResult(llvm::GCStatepointInst* statepoint) {
    // Fast path for void return value, so we avoid iterating over the whole use list.
    if (statepoint->getActualReturnType()->isVoidTy()) {
      return nullptr;
    }

    for (llvm::User* user : statepoint->users()) {
      if (llvm::GCResultInst* result = llvm::dyn_cast<llvm::GCResultInst>(user)) {
        return result;
      }
    }
    return nullptr;
  }

  static StatepointInfoMap CollectStatepointInfo(llvm::Function& function) {
    StatepointInfoMap statepoint_infos;

    // This is set to uint32_max - 1, so that we can add 1 to it and still not overflow.
    constexpr uint32_t unknown_use_distance = std::numeric_limits<uint32_t>::max() - 1;

    // Collect all statepoints.
    for (llvm::BasicBlock& basic_block : function) {
      for (llvm::Instruction& inst : basic_block) {
        if (llvm::GCStatepointInst* statepoint = llvm::dyn_cast<llvm::GCStatepointInst>(&inst)) {
          uint32_t id = 0;
          auto [it, inserted] = statepoint_infos.try_emplace(statepoint);
          DCHECK(inserted);

          llvm::SmallDenseMap<llvm::Value*, llvm::GCRelocateInst*> relocation_map =
              GetPtrToRelocationMap(statepoint);
          it->second.result = GetStatepointResult(statepoint);

          for (llvm::Value* gc_ptr : statepoint->gc_live()) {
            if (auto relocation = relocation_map.find(gc_ptr); relocation != relocation_map.end()) {
              it->second.relocation_infos.insert({
                  gc_ptr,
                  RelocationInfo{
                      .relocation = relocation->second,
                      .next_use_distance = unknown_use_distance,
                      .id = id++,
                  },
              });
            }
          }
        }
      }
    }

    // We iterate through all statepoint infos, updating the next_use_distance field, until we've
    // reached a staedy state.
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto& [statepoint, statepoint_info] : statepoint_infos) {
        for (auto& [value, relocation_info] : statepoint_info.relocation_infos) {
          uint32_t next_use_distance = relocation_info.next_use_distance;

          llvm::SmallVector<llvm::Instruction*> worklist = {relocation_info.relocation};
          llvm::SmallDenseSet<llvm::Instruction*> seen_pointers;
          while (!worklist.empty()) {
            llvm::Instruction* current_pointer = worklist.pop_back_val();
            if (seen_pointers.contains(current_pointer)) {
              continue;
            }
            seen_pointers.insert(current_pointer);

            for (const llvm::Use& relocated_use : current_pointer->uses()) {
              if (llvm::PHINode* phi = llvm::dyn_cast<llvm::PHINode>(relocated_use.getUser())) {
                if (!seen_pointers.contains(phi)) {
                  worklist.push_back(phi);
                }
                continue;
              }

              if (IsNonRelocationUse(relocated_use)) {
                // The use needs the pointer value to be in a register, so we set next_use_distance
                // to 0.
                next_use_distance = 0;
                // We can't go any lower than 0, so we can break.
                break;
              }

              llvm::GCRelocateInst* relocate = GetRelocationUse(relocated_use);
              if (relocate == nullptr) {
                // The pointer is not relocated, so this must be a deopt use. In this case the value
                // doesn't need to be in a register, so we skip it.
                continue;
              }

              llvm::GCStatepointInst* use_statepoint =
                  llvm::cast<llvm::GCStatepointInst>(relocate->getArgOperand(0));
              auto statepoint_info_it = statepoint_infos.find(use_statepoint);
              if (statepoint_info_it != statepoint_infos.end()) {
                StatepointInfo& info = statepoint_info_it->second;
                auto relocation_info_it = info.relocation_infos.find(current_pointer);
                if (relocation_info_it != info.relocation_infos.end()) {
                  next_use_distance =
                      std::min(next_use_distance, relocation_info_it->second.next_use_distance + 1);
                }
              }
            }
          }

          if (next_use_distance != relocation_info.next_use_distance) {
            relocation_info.next_use_distance = next_use_distance;
            changed = true;
          }
        }
      }
    }

    return statepoint_infos;
  }

  static bool IsNonRelocationUse(const llvm::Use& use) {
    llvm::GCStatepointInst* statepoint = llvm::dyn_cast<llvm::GCStatepointInst>(use.getUser());
    if (statepoint == nullptr) {
      return true;
    }

    for (llvm::Use& arg : statepoint->args()) {
      if (arg.get() == use.get()) {
        return true;
      }
    }

    return false;
  }

  static llvm::GCRelocateInst* GetRelocationUse(const llvm::Use& use) {
    llvm::GCStatepointInst* statepoint = llvm::dyn_cast<llvm::GCStatepointInst>(use.getUser());
    DCHECK(statepoint != nullptr);

    for (llvm::User* user : statepoint->users()) {
      if (llvm::GCRelocateInst* relocate = llvm::dyn_cast<llvm::GCRelocateInst>(user)) {
        if (relocate->getDerivedPtr() == use.get()) {
          return relocate;
        }
      }
    }

    return nullptr;
  }
};

llvm::MDNode* CreateLoopIDMetadata(llvm::LLVMContext& context, uint64_t id) {
  llvm::Constant* id_value = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), id);
  llvm::Metadata* key = llvm::MDString::get(context, kLoopPatchpointIDKey);
  llvm::Metadata* id_loop_metadata =
      llvm::MDNode::get(context, {key, llvm::ConstantAsMetadata::get(id_value)});
  llvm::MDTuple* result = llvm::MDNode::get(context, {nullptr, id_loop_metadata});
  // Loop ID node's first operand must be itself.
  result->replaceOperandWith(0, result);
  return result;
}

static constexpr bool kRunVerifier = kIsDebugBuild;

llvm::ModulePassManager BuildLLVMPassPipeline(
    CodeGeneratorARM64LLVM* codegen,
    llvm::TargetMachine* target_machine,
    llvm::OptimizationLevel optimization_level,
    bool duplicate_pipeline,
    llvm::LoopAnalysisManager& loop_analysis_manager,
    llvm::FunctionAnalysisManager& function_analysis_manager,
    llvm::CGSCCAnalysisManager& cgscc_analysis_manager,
    llvm::ModuleAnalysisManager& module_analysis_manager) {
  // llvm::PipelineTuningOptions contains the following members:
  // bool LoopInterleaving;
  // bool LoopVectorization;
  // bool SLPVectorization;
  // bool LoopUnrolling;
  // bool ForgetAllSCEVInLoopUnroll;
  // unsigned LicmMssaOptCap;
  // unsigned LicmMssaNoAccForPromotionCap;
  // bool CallGraphProfile;
  // bool UnifiedLTO;
  // bool MergeFunctions;
  // int InlinerThreshold;
  // bool EagerlyInvalidateAnalyses;
  llvm::PipelineTuningOptions tuning_options;

  // llvm::PassBuilder is a very large object, which exceeds the stack frame size limit of 1736,
  // so we need to allocate it on the heap.
  // TODO: Allocate it using the ArenaAllocator.
  auto pass_builder = std::make_unique<llvm::PassBuilder>(target_machine, tuning_options);

  pass_builder->registerModuleAnalyses(module_analysis_manager);
  pass_builder->registerCGSCCAnalyses(cgscc_analysis_manager);
  pass_builder->registerFunctionAnalyses(function_analysis_manager);
  pass_builder->registerLoopAnalyses(loop_analysis_manager);
  pass_builder->crossRegisterProxies(loop_analysis_manager,
                                     function_analysis_manager,
                                     cgscc_analysis_manager,
                                     module_analysis_manager);
  // Add passes that should run in the beginning of the pipeline.
  pass_builder->registerPipelineStartEPCallback(
      [codegen](llvm::ModulePassManager& pass_manager,
                [[maybe_unused]] llvm::OptimizationLevel opt_level) {
        llvm::FunctionPassManager function_pass_manager;
        function_pass_manager.addPass(InferMemoryScopesPass(codegen));
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after InferMemoryScopesPass"));
        }
        function_pass_manager.addPass(RecordAllocasInStackMapPass(codegen));
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after RecordAllocasInStackMapPass"));
        }
        function_pass_manager.addPass(MarkGEPsAsInBoundsPass());
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after MarkGEPsAsInBoundsPass"));
        }

        pass_manager.addPass(
            llvm::createModuleToFunctionPassAdaptor(std::move(function_pass_manager)));
      });

  pass_builder->registerPeepholeEPCallback([](llvm::FunctionPassManager& function_pass_manager,
                                              [[maybe_unused]] llvm::OptimizationLevel opt_level) {
    function_pass_manager.addPass(PeepholeOptimizationPass());
    if (kRunVerifier) {
      function_pass_manager.addPass(VerifierPass("after PeepholeOptimizerPass"));
    }
  });

  pass_builder->registerLateLoopOptimizationsEPCallback(
      [](llvm::LoopPassManager& loop_pass_manager,
         [[maybe_unused]] llvm::OptimizationLevel opt_level) {
        loop_pass_manager.addPass(DisableCatchLoopUnrollingPass());
      });

  pass_builder->registerOptimizerLastEPCallback(
      [codegen](llvm::ModulePassManager& pass_manager,
                [[maybe_unused]] llvm::OptimizationLevel opt_level,
                [[maybe_unused]] llvm::ThinOrFullLTOPhase lto_phase) {
        // Place suspend checks at required positions after all optimization have completed,
        // but before RewriteStatepointsForGC is run.
        llvm::FunctionPassManager function_pass_manager;
        function_pass_manager.addPass(RemoveUnusedAllocasPass());
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after RemoveUnusedAllocasPass"));
        }
        function_pass_manager.addPass(InferCanUseImplicitSuspendChecksPass(codegen));
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after InferCanUseImplicitSuspendChecksPass"));
        }
        function_pass_manager.addPass(ReplaceSuspendCheckPlaceholdersPass(codegen));
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after ReplaceSuspendCheckPlaceholdersPass"));
        }
        function_pass_manager.addPass(PlaceEntrySuspendAndStackOverflowChecksPass(codegen));
        if (kRunVerifier) {
          function_pass_manager.addPass(
              VerifierPass("after PlaceEntrySuspendAndStackOverflowChecksPass"));
        }
        function_pass_manager.addPass(PlaceLoopSuspendChecksPass(codegen));
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after PlaceLoopSuspendChecksPass"));
        }
        function_pass_manager.addPass(llvm::SimplifyCFGPass());
        function_pass_manager.addPass(llvm::InstCombinePass());
        function_pass_manager.addPass(FinalizeCurrentMethodStackPositionPass(codegen));
        if (kRunVerifier) {
          function_pass_manager.addPass(
              VerifierPass("after FinalizeCurrentMethodStackPositionPass"));
        }

        pass_manager.addPass(
            llvm::createModuleToFunctionPassAdaptor(std::move(function_pass_manager)));
      });
  pass_builder->registerOptimizerLastEPCallback(
      [codegen](llvm::ModulePassManager& pass_manager,
                [[maybe_unused]] llvm::OptimizationLevel opt_level,
                [[maybe_unused]] llvm::ThinOrFullLTOPhase lto_phase) {
        pass_manager.addPass(
            llvm::createModuleToFunctionPassAdaptor(RewriteCriticalNativeArgumentsPass(codegen)));
        if (kRunVerifier) {
          pass_manager.addPass(VerifierPass("after RewriteCriticalNativeArgumentsPass"));
        }
        pass_manager.addPass(llvm::RewriteStatepointsForGC());

        llvm::FunctionPassManager function_pass_manager;
        function_pass_manager.addPass(RewriteTrivialPlaceholdersPass());
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after RewriteTrivialPlaceholdersPass"));
        }
        function_pass_manager.addPass(EliminateUnwindRelocationPhisPass());
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after EliminateUnwindRelocationPhisPass"));
        }
        function_pass_manager.addPass(EliminateDerivedRelocationsPass());
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after EliminateDerivedRelocationsPass"));
        }
        function_pass_manager.addPass(VerifyStatepointsPass());
        if (kRunVerifier) {
          function_pass_manager.addPass(VerifierPass("after VerifyStatepointsPass"));
        }
        // RewriteStatepointsForGC can leave unused relocated references in the IR, which forces
        // the backend to save them. We prevent this by eliminating the unused values with an extra
        // instcombine pass.
        // NOTE: Instcombine works better than DCE in this case, as it seems to
        // better understand the llvm.gc.statepoint intrinsic, allowing it to also eliminate values
        // that only appear in the "gc-live" section of a statepoint.
        function_pass_manager.addPass(llvm::InstCombinePass());
        // RewriteStatepointsForGC creates a landingpad for each invoke instruction, in order to
        // have separate relocations for each case. Since we don't use these relocations anyways, we
        // can merge these landingpads back into a single one, which we do using the simplify cfg
        // pass.
        function_pass_manager.addPass(llvm::SimplifyCFGPass());

        pass_manager.addPass(
            llvm::createModuleToFunctionPassAdaptor(std::move(function_pass_manager)));
      });

  auto finalize_ir_callback = [codegen](llvm::ModulePassManager& pass_manager,
                                        [[maybe_unused]] llvm::OptimizationLevel opt_level,
                                        [[maybe_unused]] llvm::ThinOrFullLTOPhase lto_phase) {
    // Add extra function passes in order to finalize the generated IR.
    llvm::FunctionPassManager function_pass_manager;
    // Rewrite some statepoint instructions to @llvm.experimental.patchpoint. This is needed
    // for generating stack maps for non-call instructions, e.g. implicit null check, loads from
    // the heap that would need read barriers, etc.
    function_pass_manager.addPass(RewritePatchpointsPass());
    if (kRunVerifier) {
      function_pass_manager.addPass(VerifierPass("after RewritePatchpointsPass"));
    }
    function_pass_manager.addPass(ErasePatchedStatepointArgumentsPass());
    if (kRunVerifier) {
      function_pass_manager.addPass(VerifierPass("after ErasePatchedStatepointArgumentsPass"));
    }
    // This pass should remove redundant phis that RewritePatchpointsPass may leave behind.
    function_pass_manager.addPass(llvm::InstCombinePass());
    function_pass_manager.addPass(RemoveFunctionParameterStackSlotsPass(codegen));
    if (kRunVerifier) {
      function_pass_manager.addPass(VerifierPass("after RemoveFunctionParameterStackSlotsPass"));
    }
    function_pass_manager.addPass(CollectGCPointerStackSlotsPass());
    if (kRunVerifier) {
      function_pass_manager.addPass(VerifierPass("after CollectGCPointerStackSlotsPass"));
    }
    function_pass_manager.addPass(RewriteInlineVectorLoopsPass(codegen));
    if (kRunVerifier) {
      function_pass_manager.addPass(VerifierPass("after RewriteInlineVectorLoopsPass"));
    }
    function_pass_manager.addPass(llvm::SimplifyCFGPass());
    function_pass_manager.addPass(OptimizeRegisterLoadStoresPass());
    if (kRunVerifier) {
      function_pass_manager.addPass(VerifierPass("after OptimizeRegisterLoadStoresPass"));
    }
    function_pass_manager.addPass(llvm::InstCombinePass());
    function_pass_manager.addPass(OptimizeStatepointRegisterOrderPass());
    if (kRunVerifier) {
      function_pass_manager.addPass(VerifierPass("after OptimizeStatepointRegisterOrderPass"));
    }

    pass_manager.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(function_pass_manager)));
  };

  if (duplicate_pipeline) {
    llvm::ModulePassManager pass_manager =
        pass_builder->buildPerModuleDefaultPipeline(optimization_level);

    // Add a second set of optimization passes, which are run after RewriteStatepointsForGC has
    // finished.
    auto second_pass_builder = std::make_unique<llvm::PassBuilder>(target_machine, tuning_options);
    second_pass_builder->registerOptimizerLastEPCallback(finalize_ir_callback);
    pass_manager.addPass(second_pass_builder->buildPerModuleDefaultPipeline(optimization_level));

    return pass_manager;
  } else {
    pass_builder->registerOptimizerLastEPCallback(finalize_ir_callback);

    return pass_builder->buildPerModuleDefaultPipeline(optimization_level);
  }
}

}  // namespace arm64_llvm
}  // namespace art HIDDEN

#pragma GCC diagnostic pop
