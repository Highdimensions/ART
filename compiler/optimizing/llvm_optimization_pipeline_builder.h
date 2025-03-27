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

#ifndef ART_COMPILER_OPTIMIZING_LLVM_OPTIMIZATION_PIPELINE_BUILDER_H_
#define ART_COMPILER_OPTIMIZING_LLVM_OPTIMIZATION_PIPELINE_BUILDER_H_

#include "base/macros.h"
#include "code_generator_arm64_llvm.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wused-but-marked-unused"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/OptimizationLevel.h"
#pragma GCC diagnostic pop

namespace art HIDDEN {
namespace arm64_llvm {

class DebugPrintModulePass : public llvm::PassInfoMixin<DebugPrintModulePass> {
 public:
  llvm::PreservedAnalyses run(
      llvm::Module& module, [[maybe_unused]] llvm::ModuleAnalysisManager& module_analysis_manager) {
    module.print(llvm::dbgs(), nullptr);
    return llvm::PreservedAnalyses::all();
  }

  llvm::PreservedAnalyses run(
      llvm::Function& function,
      [[maybe_unused]] llvm::FunctionAnalysisManager& function_analysis_manager) {
    function.getParent()->print(llvm::dbgs(), nullptr);
    return llvm::PreservedAnalyses::all();
  }
};

llvm::MDNode* CreateLoopIDMetadata(llvm::LLVMContext& context, uint64_t id);

llvm::ModulePassManager BuildLLVMPassPipeline(
    CodeGeneratorARM64LLVM* codegen,
    llvm::TargetMachine* target_machine,
    llvm::OptimizationLevel optimization_level,
    bool duplicate_pipeline,
    llvm::LoopAnalysisManager& loop_analysis_manager,
    llvm::FunctionAnalysisManager& function_analysis_manager,
    llvm::CGSCCAnalysisManager& cgscc_analysis_manager,
    llvm::ModuleAnalysisManager& module_analysis_manager);

}  // namespace arm64_llvm
}  // namespace art HIDDEN

#endif  // ART_COMPILER_OPTIMIZING_LLVM_OPTIMIZATION_PIPELINE_BUILDER_H_
