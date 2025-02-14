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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_LLVM_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_LLVM_H_

#include "base/arena_containers.h"
#include "base/macros.h"
#include "class_root.h"
#include "code_generator.h"
#include "code_generator_arm64.h"
#include "code_generator_arm64_llvm_utils.h"
#include "dex/dex_file_types.h"
#include "dex/type_reference.h"
#include "driver/compiler_options.h"
#include "elf_file_parser.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/arm64/assembler_arm64.h"

// TODO(LLVM): Make LLVM compile with these warnings.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#pragma GCC diagnostic ignored "-Wused-but-marked-unused"
#pragma GCC diagnostic ignored "-Wdeprecated-copy-with-dtor"
#pragma GCC diagnostic ignored "-Wframe-larger-than"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Target/TargetMachine.h"
#pragma GCC diagnostic pop

namespace art HIDDEN {

namespace linker {
class Arm64RelativePatcherTest;
}  // namespace linker

namespace arm64_llvm {

class CodeGeneratorARM64LLVM;

// Use a local definition to prevent copying mistakes.
static constexpr size_t kArm64WordSize = static_cast<size_t>(kArm64PointerSize);

// Thread Register.
const vixl::aarch64::Register tr = vixl::aarch64::x19;
// Marking Register.
const vixl::aarch64::Register mr = vixl::aarch64::x20;
// Implicit suspend check register.
const vixl::aarch64::Register kImplicitSuspendCheckRegister = vixl::aarch64::x21;

// Callee-save registers AAPCS64, without x19 (Thread Register) (nor
// x20 (Marking Register) when emitting Baker read barriers).
const vixl::aarch64::CPURegList callee_saved_core_registers(vixl::aarch64::CPURegister::kRegister,
                                                            vixl::aarch64::kXRegSize,
                                                            (kReserveMarkingRegister
                                                                 ? vixl::aarch64::x21.GetCode()
                                                                 : vixl::aarch64::x20.GetCode()),
                                                            vixl::aarch64::x30.GetCode());
const vixl::aarch64::CPURegList callee_saved_fp_registers(vixl::aarch64::CPURegister::kVRegister,
                                                          vixl::aarch64::kDRegSize,
                                                          vixl::aarch64::d8.GetCode(),
                                                          vixl::aarch64::d15.GetCode());
#define UNIMPLEMENTED_INTRINSIC_LIST_ARM64LLVM(V) \
  V(MathSignumFloat)                              \
  V(MathSignumDouble)                             \
  V(MathCopySignFloat)                            \
  V(MathCopySignDouble)                           \
  V(IntegerRemainderUnsigned)                     \
  V(LongRemainderUnsigned)                        \
  V(StringStringIndexOf)                          \
  V(StringStringIndexOfAfter)                     \
  V(StringBufferAppend)                           \
  V(StringBufferLength)                           \
  V(StringBufferToString)                         \
  V(StringBuilderAppendObject)                    \
  V(StringBuilderAppendString)                    \
  V(StringBuilderAppendCharSequence)              \
  V(StringBuilderAppendCharArray)                 \
  V(StringBuilderAppendBoolean)                   \
  V(StringBuilderAppendChar)                      \
  V(StringBuilderAppendInt)                       \
  V(StringBuilderAppendLong)                      \
  V(StringBuilderAppendFloat)                     \
  V(StringBuilderAppendDouble)                    \
  V(StringBuilderLength)                          \
  V(StringBuilderToString)                        \
  V(SystemArrayCopyByte)                          \
  V(SystemArrayCopyInt)                           \
  V(UnsafeArrayBaseOffset)                        \
  /* 1.8 */                                       \
  V(MethodHandleInvoke)                           \
  /* OpenJDK 11*/                                 \
  V(JdkUnsafeArrayBaseOffset)

class SlowPathCodeARM64LLVM : public SlowPathCode {
 public:
  explicit SlowPathCodeARM64LLVM(HInstruction* instruction,
                                 llvm::BasicBlock* entry_block,
                                 llvm::BasicBlock* exit_block)
      : SlowPathCode(instruction), entry_block_(entry_block), exit_block_(exit_block) {}

  llvm::BasicBlock* GetEntryBlock() { return entry_block_; }
  llvm::BasicBlock* GetExitBlock() { return exit_block_; }

  // LLVM doesn't need to save any registers.
  void SaveLiveRegisters([[maybe_unused]] CodeGenerator* codegen,
                         [[maybe_unused]] LocationSummary* locations) final {}
  void RestoreLiveRegisters([[maybe_unused]] CodeGenerator* codegen,
                            [[maybe_unused]] LocationSummary* locations) final {}

 private:
  llvm::BasicBlock* entry_block_;
  llvm::BasicBlock* exit_block_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeARM64LLVM);
};

class InstructionCodeGeneratorARM64LLVM : public InstructionCodeGenerator {
 public:
  InstructionCodeGeneratorARM64LLVM(HGraph* graph,
                                    CodeGeneratorARM64LLVM* codegen,
                                    llvm::IRBuilder<>* ir_builder);

#define DECLARE_VISIT_INSTRUCTION(name, super) void Visit##name(H##name* instr) override;

  FOR_EACH_CONCRETE_INSTRUCTION_SCALAR_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_ARM64(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_SHARED(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_VECTOR_COMMON(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) override {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName() << " (id "
               << instruction->GetId() << ")";
  }

  llvm::IRBuilder<>* GetIRBuilder() const { return ir_builder_; }

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

  inline llvm::Value* CreateSabal(llvm::Value* acc,
                                  llvm::Value* lhs,
                                  llvm::Value* rhs,
                                  size_t index,
                                  llvm::Type* temp_vector_type,
                                  llvm::Type* out_vector_type);
  inline llvm::Value* CreateDotProd(llvm::Value* acc,
                                    llvm::Value* lhs,
                                    llvm::Value* rhs,
                                    size_t index,
                                    bool is_zero_extending,
                                    llvm::Type* temp_vector_type,
                                    llvm::Type* out_vector_type);

  llvm::Function* GetFunction() const;
  // Returns the zero value for the given type. The type can be an integer, pointer or
  // floating-point type.
  llvm::Constant* GetConstantZero(llvm::Type* type) const;
  llvm::ConstantInt* GetConstantInt(llvm::Type* type, int32_t value) const;
  llvm::ConstantInt* GetConstantInt(llvm::Type* type, int64_t value) const;
  llvm::ConstantInt* GetConstantInt(llvm::Type* type, uint32_t value) const;
  llvm::ConstantInt* GetConstantInt(llvm::Type* type, uint64_t value) const;

  void AddValue(HInstruction* instruction, llvm::Value* value) const;
  llvm::Value* GetValue(HInstruction* instruction,
                        DataType::Type value_type = DataType::Type::kVoid) const;

  // Gets the value out of the respective registers as the specified type. If `type` is nullptr,
  // then a default type is used.
  llvm::Value* GetThreadPointerValue(llvm::Type* type = nullptr) const;    // Default type: `ptr`.
  llvm::Value* GetMarkingRegisterValue(llvm::Type* type = nullptr) const;  // Default type: `i64`.
  llvm::Value* GetImplicitSuspendRegisterValue(
      llvm::Type* type = nullptr) const;  // Default type: `ptr`.
  llvm::Value* GetStackPointerValue() const;

  llvm::Value* CreateLoad(llvm::Type* type, llvm::Value* address) const;
  void CreateStore(llvm::Value* value, llvm::Value* address, bool is_volatile = false) const;
  llvm::Value* CreateGEP(llvm::Value* address, llvm::Value* offset) const;
  llvm::Value* CreateGEP(llvm::Value* address, int64_t offset) const;
  llvm::Value* CreateGEP(llvm::Type* type, llvm::Value* address, llvm::Value* offset) const;
  llvm::Value* CreateGEP(llvm::Type* type, llvm::Value* address, int64_t offset) const;

  llvm::Value* CreateLoadFromThreadPointer(llvm::Type* type, size_t offset) const;

  void CreateStoreToThreadPointer(llvm::Value* value, size_t offset) const;

  llvm::Value* CreateLoadWithOffset(llvm::Type* type, llvm::Value* address, int64_t offset) const;

  void GenerateSuspendCheck(HSuspendCheck* instruction,
                            llvm::CallBase* placeholder_call,
                            uint64_t id,
                            llvm::DomTreeUpdater* dom_tree_updater = nullptr,
                            llvm::LoopInfo* loop_info = nullptr);

 protected:
  void GenerateClassInitializationCheck(SlowPathCodeARM64LLVM* slow_path, llvm::Value* class_ptr);
  llvm::Value* GenerateBitstringTypeCheckCompare(HTypeCheckInstruction* check,
                                                 llvm::Value* class_ptr);
  // Some HIR instructions allow the use of narrower integer types in place of Int32, which would be
  // invalid when translating to LLVM. This functions casts the lhs and rhs operands to Int32 if
  // they don't match result_type.
  // Returns {lhs, rhs}.
  std::pair<llvm::Value*, llvm::Value*> NormalizeBinaryOperands(DataType::Type result_type,
                                                                HInstruction* lhs,
                                                                HInstruction* rhs);
  void HandleBinaryOp(HBinaryOperation* instr);

  void HandleFieldSet(HInstruction* instruction,
                      const FieldInfo& field_info,
                      bool value_can_be_null,
                      WriteBarrierKind write_barrier_kind);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleCondition(HCondition* instruction);

  // Generate a heap reference load using two different registers
  // `out` and `obj`:
  //
  //   out <- *(obj + offset)
  //
  // while honoring heap poisoning and/or read barriers (if any).
  llvm::Value* GenerateReferenceLoad(HInstruction* instruction,
                                     llvm::Value* obj,
                                     uint32_t offset,
                                     ReadBarrierOption read_barrier_option);

  void HandleShift(HBinaryOperation* instr);
  llvm::BranchInst* GenerateTestAndBranch(HInstruction* instruction,
                                          size_t condition_input_index,
                                          llvm::BasicBlock* true_target,
                                          llvm::BasicBlock* false_target);
  void HandleGoto(HInstruction* got, HBasicBlock* successor);

  llvm::Value* VecAddress(HVecMemoryOperation* instruction,
                          llvm::Type* packed_type,
                          size_t packed_type_size,
                          bool is_string_char_at);

  llvm::IRBuilder<>* const ir_builder_;
  CodeGeneratorARM64LLVM* const codegen_;
  unsigned next_parameter_index_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorARM64LLVM);
};

class CodeGeneratorARM64LLVM : public CodeGenerator {
 public:
  CodeGeneratorARM64LLVM(HGraph* graph,
                         const CompilerOptions& compiler_options,
                         OptimizingCompilerStats* stats = nullptr);
  virtual ~CodeGeneratorARM64LLVM() = default;

  llvm::OptimizationLevel GetOptimizationLevel() const;

  // Calculate stack offsets of parameters with respect to the caller's stack. This is used to
  // decide what kind of operation we should use when saving these parameter values to the stack for
  // catch blocks (ldr vs ldur and str vs stur).
  void CalculateParameterStackOffsets(llvm::ArrayRef<DataType::Type> parameter_types);

  uint32_t GetParameterStackOffset(size_t parameter_index, uint32_t frame_size) {
    DCHECK_LT(parameter_index, parameter_stack_offsets_.size());
    DCHECK_NE(parameter_stack_offsets_[parameter_index].offset, uint32_t(-1));
    return frame_size + parameter_stack_offsets_[parameter_index].offset;
  }

  void SetParameterStackSlotUsed(size_t parameter_index) {
    DCHECK_LT(parameter_index, parameter_stack_offsets_.size());
    parameter_stack_offsets_[parameter_index].is_used = true;
  }

  bool IsParameterStackSlotUsed(size_t parameter_index) const {
    DCHECK_LT(parameter_index, parameter_stack_offsets_.size());
    return parameter_stack_offsets_[parameter_index].is_used;
  }

  // Initialize LLVM metadata nodes used by @llvm.read_register and alias scopes.
  void InitializeMetadata();
  // Create __load_* and __store_* functions used for implicitly null checked memory operations.
  void InitializePlaceholderFunctions();

  void DoStackSavedValueLivenessAnalysis();

  bool IsParameterPassedOnStack(size_t parameter_index) const {
    DCHECK_LT(parameter_index, parameter_stack_offsets_.size());
    return parameter_stack_offsets_[parameter_index].is_passed_on_stack;
  }

  uint32_t GetParameterStackOffset(size_t parameter_index) const {
    DCHECK_LT(parameter_index, parameter_stack_offsets_.size());
    DCHECK(parameter_stack_offsets_[parameter_index].offset != ~uint32_t{0});
    return parameter_stack_offsets_[parameter_index].offset;
  }

  void GenerateFrameEntry() override;
  void GenerateFrameExit() override;

  llvm::Type* GetPhiType(HPhi* phi) const;
  void Bind(HBasicBlock* block) override;
  void SetCurrentBlock(HBasicBlock* block) { current_block_ = block; }

  llvm::BasicBlock* GetIRBasicBlock(HBasicBlock* block) {
    uint32_t block_id = block->GetBlockId();
    if (block_infos_[block_id].start_block == nullptr) {
      block_infos_[block_id].start_block = CreateBasicBlock();
      block_infos_[block_id].start_block->setName(fmt::format("B{}.start", block_id));
    }
    return block_infos_[block_id].start_block;
  }

  void SetLastIRBasicBlock(HBasicBlock* block, llvm::BasicBlock* llvm_block) {
    uint32_t block_id = block->GetBlockId();
    DCHECK(block_infos_[block_id].start_block != nullptr);
    DCHECK(block_infos_[block_id].end_block == nullptr);
    if (llvm_block == block_infos_[block_id].start_block) {
      llvm_block->setName(fmt::format("B{}.start.end", block_id));
    } else {
      llvm_block->setName(fmt::format("B{}.end", block_id));
    }
    block_infos_[block_id].end_block = llvm_block;
  }

  llvm::BasicBlock* GetLastIRBasicBlock(HBasicBlock* block) {
    uint32_t block_id = block->GetBlockId();
    DCHECK(block_infos_[block_id].end_block != nullptr);
    return block_infos_[block_id].end_block;
  }

  struct BasicBlockInfo {
    explicit BasicBlockInfo(ArenaAllocator* allocator)
        : start_block(nullptr),
          end_block(nullptr),
          instruction_index_map(allocator->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator)),
          live_stack_saved_values(allocator->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator)),
          partially_live_stack_saved_values(
              allocator->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator)),
          loaded_alloca_values(allocator->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator)),
          propagated_stack_saved_values(
              allocator->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator)),
          stack_saved_value_phis(allocator->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator)),
          instruction_value_map(allocator->Adapter(ArenaAllocKind::kArenaAllocCodeGenerator)) {}

    // First and last LLVM basic block for each HBasicBlock that will be compiled. It is necessary
    // to store both the first and last blocks for resolving phis.
    llvm::BasicBlock* start_block;
    llvm::BasicBlock* end_block;
    // Information containing the result of the try-catch liveness analysis.
    ArenaHashMap<HInstruction*, uint32_t> instruction_index_map;
    ArenaHashSet<HInstruction*> live_stack_saved_values;
    ArenaHashMap<HInstruction*, uint32_t> partially_live_stack_saved_values;
    ArenaHashSet<HInstruction*> loaded_alloca_values;
    ArenaHashMap<HInstruction*, HBasicBlock*> propagated_stack_saved_values;
    ArenaHashSet<HInstruction*> stack_saved_value_phis;
    // Value map local to the given basic block.
    ArenaHashMap<HInstruction*, llvm::Value*> instruction_value_map;
  };

  BasicBlockInfo& GetBasicBlockInfo(HBasicBlock* block) {
    DCHECK_LT(block->GetBlockId(), block_infos_.size());
    return block_infos_[block->GetBlockId()];
  }

  size_t GetWordSize() const override { return kArm64WordSize; }

  bool SupportsPredicatedSIMD() const override { return ShouldUseSVE(); }

  size_t GetSlowPathFPWidth() const override {
    return GetGraph()->HasSIMD() ? GetSIMDRegisterWidth() : vixl::aarch64::kDRegSizeInBytes;
  }

  size_t GetCalleePreservedFPWidth() const override { return vixl::aarch64::kDRegSizeInBytes; }

  size_t GetSIMDRegisterWidth() const override;

  uintptr_t GetAddressOf([[maybe_unused]] HBasicBlock* block) override {
    LOG(FATAL) << "Unreachable";
    return 0;
  }

  HGraphVisitor* GetLocationBuilder() override {
    if (arm64_codegen_ == nullptr) {
      ArenaAllocator* allocator = GetGraph()->GetAllocator();
      arm64_codegen_ = std::unique_ptr<arm64::CodeGeneratorARM64>(
          new (allocator) arm64::CodeGeneratorARM64(GetGraph(), GetCompilerOptions(), nullptr));
    }
    return arm64_codegen_->GetLocationBuilder();
  }
  InstructionCodeGeneratorARM64LLVM* GetInstructionCodeGeneratorArm64() {
    return &instruction_visitor_;
  }
  HGraphVisitor* GetInstructionVisitor() override { return GetInstructionCodeGeneratorArm64(); }

  Assembler* GetAssembler() override { return nullptr; }
  const Assembler& GetAssembler() const override {
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }

  ArrayRef<const uint8_t> GetCfiData() override { return ArrayRef<const uint8_t>(cfi_data_); }

  // Emit a write barrier if:
  // A) emit_null_check is false
  // B) emit_null_check is true, and value is not null.
  void MaybeMarkGCCard(llvm::Value* object, llvm::Value* value, bool emit_null_check);

  // Emit a write barrier unconditionally.
  void MarkGCCard(llvm::Value* object);

  // Crash if the card table is not valid. This check is only emitted for the CC GC. We assert
  // `(!clean || !self->is_gc_marking)`, since the card table should not be set to clean when the CC
  // GC is marking for eliminated write barriers.
  void CheckGCCardIsValid(llvm::Value* object);

  void GenerateMemoryBarrier(MemBarrierKind kind);

  // Register allocation.

  void SetupBlockedRegisters() const override;

  size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id) override;
  size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id) override;
  size_t SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) override;
  size_t RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) override;

  // The number of registers that can be allocated. The register allocator may
  // decide to reserve and not use a few of them.
  // We do not consider registers sp, xzr, wzr. They are either not allocatable
  // (xzr, wzr), or make for poor allocatable registers (sp alignment
  // requirements, etc.). This also facilitates our task as all other registers
  // can easily be mapped via to or from their type and index or code.
  static const int kNumberOfAllocatableRegisters = vixl::aarch64::kNumberOfRegisters - 1;
  static const int kNumberOfAllocatableFPRegisters = vixl::aarch64::kNumberOfVRegisters;
  static constexpr int kNumberOfAllocatableRegisterPairs = 0;

  void DumpCoreRegister(std::ostream& stream, int reg) const override;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const override;

  InstructionSet GetInstructionSet() const override { return InstructionSet::kArm64; }

  const Arm64InstructionSetFeatures& GetInstructionSetFeatures() const;

  void Initialize() override;

  // We want to use the STP and LDP instructions to spill and restore registers for slow paths.
  // These instructions can only encode offsets that are multiples of the register size accessed.
  uint32_t GetPreferredSlotsAlignment() const override { return vixl::aarch64::kXRegSizeInBytes; }

  void RunOptimizerPasses();
  void RemovePlaceholderFunctions();
  void AddClinitCheckPrologue();

  // The object file will probably be larger than what can fit into the stack
  // portion of a SmallVector, so set the buffer size to 0
  using ObjectMemoryBuffer = llvm::SmallVector<char, 0>;

  ObjectMemoryBuffer EmitObjectFile();

  struct EHFrameParseResult {
    uint32_t frame_size = 0;
    uint32_t core_spill_mask = 0;
    uint32_t fp_spill_mask = 0;
    int64_t cfa_offset = 0;
  };

  using RodataOffsetMap = llvm::SmallDenseMap<llvm::StringRef, uint32_t>;
  void ParseObjectFile(llvm::ArrayRef<char> object_file_buffer);
  EHFrameParseResult ParseEHFrame(ELFFileParser& object_file);
  uint64_t ParseFrameSize(ELFFileParser& object_file);
  RodataOffsetMap ParseCode(ELFFileParser& object_file);
  void ParseRelocations(ELFFileParser& object_file, const RodataOffsetMap& rodata_offsets);
  void ParseStackMap(ELFFileParser& object_file, uint32_t frame_size, int64_t cfa_offset);
  ArenaVector<uint32_t> ParseCatchBlockAddresses(ELFFileParser& object_file);
  void EmitEnvironment(HEnvironment* environment,
                       ArrayRef<const DexRegisterLocation> environment_locations,
                       bool needs_vreg_info = true,
                       bool is_for_catch_handler = false,
                       bool innermost_environment = true);
  void EmitVRegInfo(HEnvironment* environment,
                    ArrayRef<const DexRegisterLocation> environment_locations);
  void EmitVRegInfoOnlyCatchPhis(HEnvironment* environment,
                                 ArrayRef<const DexRegisterLocation> environment_locations);

  void Finalize() override;

  ArrayRef<const uint8_t> GetCode() const override {
    DCHECK(!code_.empty());
    return ArrayRef<const uint8_t>(code_);
  }

  // Code generation helpers.
  void MoveConstant(Location destination, int32_t value) override;
  void MoveLocation(Location dst, Location src, DataType::Type dst_type) override;
  void AddLocationAsTemp(Location location, LocationSummary* locations) override;

  llvm::Value* UnpoisonHeapReference(llvm::Value* value);
  llvm::Value* PoisonHeapReference(llvm::Value* value);
  llvm::Value* MaybeUnpoisonHeapReference(llvm::Value* value);
  llvm::Value* MaybePoisonHeapReference(llvm::Value* value);

  llvm::Value* Load(DataType::Type type, llvm::Value* address);
  void Store(DataType::Type type, bool is_value_signed, llvm::Value* value, llvm::Value* address);
  llvm::Value* LoadVolatile(HInstruction* instruction,
                            DataType::Type type,
                            llvm::Value* base,
                            bool needs_null_check);
  void StoreVolatile(HInstruction* instruction,
                     DataType::Type type,
                     bool is_value_signed,
                     llvm::Value* value,
                     llvm::Value* base,
                     bool needs_null_check);

  llvm::SmallVector<llvm::Type*> GetParameterTypes(llvm::ArrayRef<llvm::Value*> parameters) const;

  // Set the parameters and return type for an InvokeRuntime call.
  // NOTE: This is kind of a hack, but because InvokeRuntime is a virtual method
  // in the CodeGenerator base class, we have to work around it.
  void SetInvokeRuntimeParametersAndReturnType(
      llvm::ArrayRef<llvm::Value*> parameters,
      llvm::Type* return_type,
      llvm::CallingConv::ID cc = llvm::CallingConv::ARTInvokeRuntime,
      std::optional<uint64_t> statepoint_id = std::nullopt,
      llvm::SmallVector<llvm::OperandBundleDef, 1> deopt_bundle = {});

  struct InvokeParametersAndReturnType {
    llvm::SmallVector<llvm::Value*, 9> parameters;
    llvm::Type* return_type;
    llvm::CallingConv::ID cc;
    std::optional<uint64_t> statepoint_id;
    llvm::SmallVector<llvm::OperandBundleDef, 1> deopt_bundle;
  };

  InvokeParametersAndReturnType GetInvokeRuntimeParametersAndReturnType();
  llvm::CallBase* GetInvokeRuntimeResult(bool allow_void = false);

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(QuickEntrypointEnum entrypoint,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path = nullptr) override;

  // Generate code to invoke a runtime entry point, but do not record
  // PC-related information in a stack map.
  void InvokeRuntimeWithoutRecordingPcInfo(int32_t entry_point_offset,
                                           HInstruction* instruction,
                                           SlowPathCode* slow_path);

  ParallelMoveResolver* GetMoveResolver() override {
    LOG(FATAL) << "Unreachable";
    return nullptr;
  }

  bool NeedsTwoRegisters([[maybe_unused]] DataType::Type type) const override { return false; }

  // Check if the desired_string_load_kind is supported. If it is, return it,
  // otherwise return a fall-back kind that should be used instead.
  HLoadString::LoadKind GetSupportedLoadStringKind(
      HLoadString::LoadKind desired_string_load_kind) override;

  // Check if the desired_class_load_kind is supported. If it is, return it,
  // otherwise return a fall-back kind that should be used instead.
  HLoadClass::LoadKind GetSupportedLoadClassKind(
      HLoadClass::LoadKind desired_class_load_kind) override;

  // Check if the desired_dispatch_info is supported. If it is, return it,
  // otherwise return a fall-back info that should be used instead.
  HInvokeStaticOrDirect::DispatchInfo GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info, ArtMethod* method) override;

  [[deprecated]] void GenerateStaticOrDirectCall(
      [[maybe_unused]] HInvokeStaticOrDirect* invoke,
      [[maybe_unused]] Location temp,
      [[maybe_unused]] SlowPathCode* slow_path = nullptr) override {}
  [[deprecated]] void GenerateVirtualCall(
      [[maybe_unused]] HInvokeVirtual* invoke,
      [[maybe_unused]] Location temp,
      [[maybe_unused]] SlowPathCode* slow_path = nullptr) override {}

  [[nodiscard]] llvm::Value* LoadMethod(MethodLoadKind load_kind, HInvoke* invoke);
  [[nodiscard]] llvm::Value* GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke);
  [[nodiscard]] llvm::Value* GenerateStaticOrDirectRuntimeCall(HInvokeStaticOrDirect* invoke);
  [[nodiscard]] llvm::Value* GenerateVirtualCall(HInvokeVirtual* invoke);
  [[nodiscard]] llvm::Value* GenerateInvokePolymorphicCall(HInvokePolymorphic* invoke);

  llvm::Value* GenerateUnresolvedFieldAccess(HInstruction* field_access,
                                             DataType::Type field_type,
                                             uint32_t field_index);

  void MoveFromReturnRegister(Location trg, DataType::Type type) override;

  // Add a new boot image intrinsic patch for an instruction and return the loaded
  // value
  llvm::Value* NewBootImageIntrinsicPatch(uint32_t boot_image_reference);

  // Add a new LoadClass boot image relocation patch for an adrp and ldr pair and return the loaded
  // value.
  llvm::Value* NewClassBootImageRelRoPatch(uint32_t boot_image_offset);

  // Add a new LoadMethod boot image relocation patch for an adrp and ldr pair and return the loaded
  // value.
  llvm::Value* NewMethodBootImageRelRoPatch(uint32_t boot_image_offset);

  // Add a new LoadMethod app image relocation patch for an adrp and ldr pair and return the loaded
  // value.
  llvm::Value* NewAppImageMethodPatch(MethodReference target_method);

  // Add a new LoadMethod boot image relocation patch for an adrp and add pair and return the loaded
  // value.
  llvm::Value* NewMethodBootImageLinkTimePcRelativePatch(MethodReference target_method);

  // Add a new LoadString boot image relocation patch for an adrp and ldr pair and return the loaded
  // value.
  llvm::Value* NewStringBootImageRelRoPatch(uint32_t boot_image_offset);

  // Add a new LoadString boot image relocation patch for an adrp and add pair and return the loaded
  // value
  llvm::Value* NewStringBootImageLinkTimePcRelativePatch(const DexFile& dex_file,
                                                         const dex::StringIndex string_index);

  // Add a new .bss entry method patch info for an adrp and ldr pair and return the loaded value.
  llvm::Value* NewMethodBssEntryPatch(MethodReference target_method);

  // Add a new boot image type patch for an adrp and add pair and return the resulting value.
  llvm::Value* NewBootImageTypePatch(const DexFile& dex_file, dex::TypeIndex type_index);

  // Add a new boot image type patch for an adrp and ldr pair and return the resulting value.
  llvm::Value* NewAppImageTypePatch(const DexFile& dex_file, dex::TypeIndex type_index);

  // Add a new .bss entry type patch for an adrp and ldr pair and return the loaded value.
  llvm::Value* NewBssEntryTypePatch(HLoadClass* load_class);

  // Add a new .bss entry string patch for an adrp and ldr pair and return the loaded value.
  llvm::Value* NewStringBssEntryPatch(HLoadString* load_string);

  // Add a new .bss entry method type patch for an adrp and ldr pair and return the loaded value.
  llvm::Value* NewMethodTypeBssEntryPatch(HLoadMethodType* load_method_type);

  // Add a new boot image JNI entrypoint patch for an instruction and return the loaded value
  llvm::Value* NewMethodBootImageJniEntrypointPatch(MethodReference target_method);

  // Insert an entrypoint thunk function declaration into the module.
  llvm::FunctionCallee GetEntrypointThunkPlaceholderFunction(ThreadOffset64 entrypoint_offset,
                                                             llvm::FunctionType* function_type,
                                                             llvm::CallingConv::ID cc);

  // Insert a function into the module with the name `function_name` and type `type`.
  llvm::FunctionCallee GetPlaceholderFunction(std::string_view function_name,
                                              llvm::FunctionType* type);

  llvm::Value* LoadBootImageAddress(uint32_t boot_image_reference);
  llvm::Value* LoadIntrinsicDeclaringClass(HInvoke* invoke);
  llvm::Value* LoadClassRootForIntrinsic(ClassRoot class_root);

  void EmitLinkerPatches(ArenaVector<linker::LinkerPatch>* linker_patches) override;
  bool NeedsThunkCode(const linker::LinkerPatch& patch) const override;
  void EmitThunkCode(const linker::LinkerPatch& patch,
                     /*out*/ ArenaVector<uint8_t>* code,
                     /*out*/ std::string* debug_name) override;

  void EmitJitRootPatches(uint8_t* code, const uint8_t* roots_data) override;

  // Generate a GC root reference load:
  //
  //   <result> = *root_pointer
  //
  // while honoring read barriers based on read_barrier_option.
  llvm::Value* GenerateGcRootFieldLoad(HInstruction* instruction,
                                       llvm::Value* loaded_root,
                                       ReadBarrierOption read_barrier_option);
  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference field load when Baker's read barriers are used.
  // Overload suitable for Unsafe.getObject/-Volatile() intrinsic.
  llvm::Value* GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                                     llvm::Value* obj,
                                                     llvm::Value* field_address,
                                                     bool needs_null_check,
                                                     bool use_load_acquire);
  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference field load when Baker's read barriers are used.
  llvm::Value* GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                                     llvm::Value* obj,
                                                     uint32_t offset,
                                                     bool needs_null_check,
                                                     bool use_load_acquire);

  // If read barriers are enabled, generate a read barrier for a heap
  // reference using a slow path. If heap poisoning is enabled, also
  // unpoison the reference in `out`.
  llvm::Value* MaybeGenerateReadBarrierSlow(HInstruction* instruction,
                                            llvm::Value* ref,
                                            llvm::Value* obj,
                                            uint32_t offset,
                                            llvm::Value* index = nullptr);

  void IncreaseFrame(size_t adjustment) override;
  void DecreaseFrame(size_t adjustment) override;

  void GenerateNop() override;

  void GenerateImplicitNullCheck(HNullCheck* instruction) override;
  void GenerateExplicitNullCheck(HNullCheck* instruction) override;

  [[deprecated(
      "MaybeRecordImplicitNullCheck shouldn't be used with the LLVM code generator. Use "
      "ShouldRecordImplicitNullCheck")]] void
  MaybeRecordImplicitNullCheck([[maybe_unused]] HInstruction* instr) final {
    LOG(FATAL) << "`MaybeRecordImplicitNullCheck(HInstruction* instr)` called in LLVM code "
                  "generation. Use `ShouldRecordImplicitNullCheck(HInstruction* instr)` instead.";
  }

  bool ShouldRecordImplicitNullCheck(HInstruction* instr) {
    return instr->GetImplicitNullCheck() != nullptr;
  }

  void MaybeGenerateInlineCacheCheck(HInstruction* instruction, llvm::Value* klass);
  void MaybeIncrementHotness(bool is_frame_entry);
  void MaybeRecordTraceEvent(bool is_method_entry);

  bool CanUseImplicitSuspendCheck() const;
  void SetCanUseImplicitSuspendCheck(bool value);

  llvm::BasicBlock* CreateTryBoundaryCatchSwitch(HTryBoundary* try_boundary);
  void SetTryBoundaryCatchSwitch(HTryBoundary* try_boundary, llvm::BasicBlock* catchswitch_block);
  llvm::BasicBlock* GetTryBoundaryCatchSwitch(const HTryBoundary* try_boundary);

  llvm::LLVMContext& GetLLVMContext() { return llvm_context_; }

  llvm::Type* GetLLVMType(DataType::Type type) const;

  llvm::Type* GetPointerType() const { return llvm_types_.ptr; }
  llvm::Type* GetUncompressedGCPointerType() const { return llvm_types_.uncompressed_gc_ptr; }
  llvm::Type* GetCompressedGCPointerType() const { return llvm_types_.compressed_gc_ptr; }
  llvm::Type* GetMethodPointerType() const { return llvm_types_.method_ptr; }
  llvm::Type* GetBooleanType() const { return llvm_types_.boolean; }
  llvm::Type* GetInt8Type() const { return llvm_types_.i8; }
  llvm::Type* GetUint8Type() const { return llvm_types_.i8; }
  llvm::Type* GetInt16Type() const { return llvm_types_.i16; }
  llvm::Type* GetUint16Type() const { return llvm_types_.i16; }
  llvm::Type* GetInt32Type() const { return llvm_types_.i32; }
  llvm::Type* GetUint32Type() const { return llvm_types_.i32; }
  llvm::Type* GetInt64Type() const { return llvm_types_.i64; }
  llvm::Type* GetUint64Type() const { return llvm_types_.i64; }
  llvm::Type* GetFloat32Type() const { return llvm_types_.f32; }
  llvm::Type* GetFloat64Type() const { return llvm_types_.f64; }
  llvm::Type* GetVoidType() const { return llvm_types_.void_; }
  llvm::Type* GetVectorType(llvm::Type* packed_type, size_t length) const;

  llvm::Function* GetFunction() const { return function_; }
  // Returns the zero value for the given type. The type can be an integer, pointer or
  // floating-point type.
  llvm::Constant* GetConstantZero(llvm::Type* type);
  llvm::ConstantInt* GetConstantInt(llvm::Type* type, int32_t value);
  llvm::ConstantInt* GetConstantInt(llvm::Type* type, int64_t value);
  llvm::ConstantInt* GetConstantInt(llvm::Type* type, uint32_t value);
  llvm::ConstantInt* GetConstantInt(llvm::Type* type, uint64_t value);

  llvm::BasicBlock* CreateBasicBlock(std::string_view str = "");
  llvm::AllocaInst* CreateAlloca(llvm::Type* type);
  void AddValue(HInstruction* instruction, llvm::Value* value);
  llvm::AllocaInst* AddValueAlloca(HInstruction* instruction, llvm::Type* type);
  llvm::Value* GetValue(HInstruction* instruction,
                        DataType::Type value_type = DataType::Type::kVoid);
  llvm::Value* GetValueAllocaOrConstant(HInstruction* instruction);
  llvm::AllocaInst* MaybeGetValueAlloca(HInstruction* instruction);
  llvm::Type* GetValueType(HInstruction* instruction);

  void MapParameterValueToIndex(HParameterValue*, unsigned int);
  unsigned int GetIndexOfParameterValue(HParameterValue*);

  // Add a phi instruction that needs to be resolved at the end.
  void AddPhi(HPhi* phi, llvm::PHINode* llvm_phi);

  uint32_t AddStackMapInfo(HInstruction* instruction,
                           const DexFile* dex_file = nullptr,
                           uint32_t index_or_offset = 0);
  // NOTE: The result vector can only have 0 or 1 elements.
  llvm::SmallVector<llvm::OperandBundleDef, 1> GetDeoptBundle(HInstruction* instruction,
                                                              bool add_environment = true);
  void SetStatepointID(HInstruction* instruction, llvm::CallBase* call, uint64_t id);
  void SetEntrySuspendCheckID(uint64_t id) { entry_suspend_check_id_ = id; }
  uint64_t GetEntrySuspendCheckID() { return entry_suspend_check_id_; }

  // Gets the value out of the respective registers as the specified type. If `type` is nullptr,
  // then a default type is used.
  llvm::Value* GetThreadPointerValue(llvm::Type* type = nullptr);            // Default type: `ptr`.
  llvm::Value* GetMarkingRegisterValue(llvm::Type* type = nullptr);          // Default type: `i64`.
  llvm::Value* GetImplicitSuspendRegisterValue(llvm::Type* type = nullptr);  // Default type: `ptr`.
  llvm::Value* GetStackPointerValue();

  llvm::Instruction* CreateCastToUncompressed(llvm::Value* compressed_ptr);
  llvm::Instruction* CreateCastToCompressed(llvm::Value* uncompressed_ptr);
  llvm::Instruction* CreateCastToInt(llvm::Value* ptr);

  llvm::Value* CreateLoad(llvm::Type* type,
                          llvm::Value* address,
                          llvm::AtomicOrdering ordering = llvm::AtomicOrdering::Unordered);
  void CreateStore(llvm::Value* value,
                   llvm::Value* address,
                   llvm::AtomicOrdering ordering = llvm::AtomicOrdering::Unordered,
                   bool is_volatile = false);
  void CreateStore(llvm::Value* value, llvm::Value* address, bool is_volatile) {
    CreateStore(value, address, llvm::AtomicOrdering::Unordered, is_volatile);
  }
  llvm::Value* CreateGEP(llvm::Value* address, llvm::Value* offset);
  llvm::Value* CreateGEP(llvm::Value* address, int64_t offset);
  llvm::Value* CreateGEP(llvm::Type* type, llvm::Value* address, llvm::Value* offset);
  llvm::Value* CreateGEP(llvm::Type* type, llvm::Value* address, int64_t offset);

  struct CmpXchgResult {
    llvm::Value* loaded_value;
    llvm::Value* success;
  };
  CmpXchgResult CreateAtomicCmpXchg(llvm::Value* ptr,
                                    llvm::Value* cmp,
                                    llvm::Value* new_value,
                                    llvm::MaybeAlign align,
                                    llvm::AtomicOrdering success_ordering,
                                    llvm::AtomicOrdering failure_ordering,
                                    bool is_strong = true);
  llvm::Value* CreateAtomicRMW(llvm::AtomicRMWInst::BinOp op,
                               llvm::Value* ptr,
                               llvm::Value* val,
                               llvm::MaybeAlign align,
                               llvm::AtomicOrdering ordering);

  llvm::Value* CreateLoadFromThreadPointer(
      llvm::Type* type,
      size_t offset,
      llvm::AtomicOrdering ordering = llvm::AtomicOrdering::Unordered);
  void CreateStoreToThreadPointer(llvm::Value* value,
                                  size_t offset,
                                  llvm::AtomicOrdering ordering = llvm::AtomicOrdering::Unordered);

  llvm::Value* CreateLoadWithOffset(
      llvm::Type* type,
      llvm::Value* address,
      int64_t offset,
      llvm::AtomicOrdering ordering = llvm::AtomicOrdering::Unordered);
  void CreateStoreWithOffset(llvm::Value* value,
                             llvm::Value* address,
                             int64_t offset,
                             llvm::AtomicOrdering ordering = llvm::AtomicOrdering::Unordered);

  llvm::Instruction* CreateLoadWithImplicitNullCheck(HNullCheck* instruction,
                                                     llvm::Type* type,
                                                     llvm::Value* address,
                                                     int64_t offset = 0);
  llvm::Instruction* CreateDiscardedLoadWithImplicitNullCheck(HNullCheck* instruction,
                                                              llvm::Value* address,
                                                              int64_t offset = 0);
  llvm::Value* CreateLoadAcquireWithImplicitNullCheck(HNullCheck* instruction,
                                                      llvm::Type* type,
                                                      llvm::Value* address);

  llvm::Value* CreateLoadMaybeWithImplicitNullCheck(HInstruction* instruction,
                                                    llvm::Type* type,
                                                    llvm::Value* address,
                                                    int64_t offset = 0);

  void CreateStoreWithImplicitNullCheck(HNullCheck* instruction,
                                        DataType::Type value_type,
                                        llvm::Value* value,
                                        llvm::Value* address,
                                        int64_t offset = 0);
  void CreateStoreReleaseWithImplicitNullCheck(HNullCheck* instruction,
                                               DataType::Type value_type,
                                               llvm::Value* value,
                                               llvm::Value* address);

  void CreateStoreMaybeWithImplicitNullCheck(HInstruction* instruction,
                                             DataType::Type value_type,
                                             bool is_value_signed,
                                             llvm::Value* value,
                                             llvm::Value* address,
                                             int64_t offset = 0);

  llvm::CallBase* CreateCallOrInvoke(
      HInstruction* instruction,
      llvm::FunctionCallee callee,
      llvm::ArrayRef<llvm::Value*> arguments,
      llvm::BasicBlock* normal_successor = nullptr,
      bool add_environment = true,
      llvm::ArrayRef<llvm::OperandBundleDef> deopt_bundle = std::nullopt);

  llvm::CallBase* CreateCallWithCC(
      HInstruction* instruction,
      llvm::CallingConv::ID cc,
      llvm::FunctionCallee callee,
      llvm::ArrayRef<llvm::Value*> args = std::nullopt,
      bool add_deopt_bundle = true,
      llvm::ArrayRef<llvm::OperandBundleDef> deopt_bundle = std::nullopt);
  llvm::CallBase* CreateInvokeDexCall(HInstruction* instruction,
                                      llvm::FunctionType* function_type,
                                      llvm::Value* callee,
                                      llvm::ArrayRef<llvm::Value*> args = std::nullopt);
  llvm::CallBase* CreateInvokeInterfaceCall(HInstruction* instruction,
                                            llvm::FunctionType* function_type,
                                            llvm::Value* callee,
                                            llvm::ArrayRef<llvm::Value*> args = std::nullopt);
  llvm::CallBase* CreateCriticalNativeCall(HInstruction* instruction,
                                           llvm::FunctionType* function_type,
                                           llvm::Value* callee,
                                           llvm::ArrayRef<llvm::Value*> args = std::nullopt);

  llvm::Function* GetPlaceholderFunction(PatchpointKind kind) const;
  llvm::Function* GetSuspendCheckPlaceholderFunction() const {
    return placeholder_functions_.suspend_check;
  }
  llvm::Function* GetMemCpyI16PlaceholderFunction() const {
    return placeholder_functions_.memcpy_i16;
  }
  llvm::Function* GetMemCpyI32PlaceholderFunction() const {
    return placeholder_functions_.memcpy_i32;
  }
  llvm::Function* GetMemCpyI8ZextToI16PlaceholderFunction() const {
    return placeholder_functions_.memcpy_i8_zext_to_i16;
  }
  llvm::Function* GetStringEqualsPlaceholderFunction() const {
    return placeholder_functions_.string_equals;
  }

  llvm::CallInst* CreatePatchpoint(llvm::Type* type,
                                   uint64_t id,
                                   uint32_t number_of_bytes,
                                   llvm::ArrayRef<llvm::Value*> arguments = {},
                                   llvm::ArrayRef<llvm::Value*> recorded_values = {},
                                   std::optional<llvm::ModRefInfo> memory_effects = std::nullopt);

  llvm::CallInst* CreateStackMap(uint64_t id,
                                 llvm::ArrayRef<llvm::Value*> recorded_values = {},
                                 std::optional<llvm::ModRefInfo> memory_effects = std::nullopt);

  llvm::BranchInst* CreateBranchIfTrue(llvm::Value* condition, llvm::BasicBlock* true_block);
  llvm::BranchInst* CreateBranchIfFalse(llvm::Value* condition, llvm::BasicBlock* false_block);

  llvm::Value* GetUndefCurrentMethodPointer();
  llvm::Value* GetCurrentMethodPointerArgument();

  void AddStackAliasScopeMetadata(llvm::Instruction* instruction);
  void AddHeapAliasScopeMetadata(llvm::Instruction* instruction);
  void AddRuntimeAliasScopeMetadata(llvm::Instruction* instruction);
  void AddThreadObjectAliasScopeMetadata(llvm::Instruction* instruction);
  void AddStackNoaliasMetadata(llvm::Instruction* instruction);
  void AddHeapNoaliasMetadata(llvm::Instruction* instruction);
  void AddRuntimeNoaliasMetadata(llvm::Instruction* instruction);
  void AddThreadObjectNoaliasMetadata(llvm::Instruction* instruction);
  void AddCallNoaliasMetadata(llvm::Instruction* instruction);

  llvm::Module* GetModule() const { return module_.get(); }
  void DumpModule() const;

  llvm::IRBuilder<>* GetIRBuilder() { return &ir_builder_; }

  void SetStackmapInfoOffset(uint32_t index, uint32_t new_offset) {
    DCHECK_LT(index, stack_map_infos_.size());
    stack_map_infos_[index].index_or_offset = new_offset;
  }

  HInstruction* GetInstructionFromStackmapInfo(uint32_t index) {
    DCHECK_LT(index, stack_map_infos_.size());
    return stack_map_infos_[index].instruction;
  }

  bool CPUHasCRC() const;
  bool CPUHasFP16() const;

 private:
  // The PcRelativePatchInfo is used for PC-relative addressing of methods/strings/types,
  // whether through .data.bimg.rel.ro, .bss, or directly in the boot image.
  struct PcRelativePatchInfo : PatchInfo<uint64_t> {
    PcRelativePatchInfo(const DexFile* dex_file, uint32_t off_or_idx)
        : PatchInfo<uint64_t>(dex_file, off_or_idx) {}

    uint64_t pc_insn_label = 0;
  };

  struct BakerReadBarrierPatchInfo {
    explicit BakerReadBarrierPatchInfo(uint32_t data) : label(), custom_data(data) {}

    vixl::aarch64::Label label;
    uint32_t custom_data;
  };

  void NewPcRelativePatch(const DexFile* dex_file,
                          uint32_t offset_or_index,
                          uint64_t insn_offset,
                          std::optional<uint64_t> adrp_offset,
                          ArenaDeque<PcRelativePatchInfo>* patches);

  template <linker::LinkerPatch (*Factory)(size_t, const DexFile*, uint32_t, uint32_t)>
  static void EmitPcRelativeLinkerPatches(const ArenaDeque<PcRelativePatchInfo>& infos,
                                          ArenaVector<linker::LinkerPatch>* linker_patches);

  // Returns whether SVE features are supported and should be used.
  bool ShouldUseSVE() const;

  bool CPUHasFeature(std::string_view feature) const;

  llvm::Value* HeapReferencePoisoning(llvm::Value* value);

  // TODO refactor
  void AddAttributesToFunction(llvm::ArrayRef<DataType::Type> rt_pt);
  void AddAttributesToCall(HInstruction* instruction, llvm::CallBase* call);

  // LLVMContext is not thread-safe, so each thread (i.e. CodeGenerator) must
  // have its own instance.
  llvm::LLVMContext llvm_context_;
  LLVMTypes llvm_types_;
  std::unique_ptr<llvm::TargetMachine> target_machine_;
  std::unique_ptr<llvm::Module> module_;
  llvm::Function* function_ = nullptr;
  llvm::IRBuilder<> ir_builder_;

  struct StackOffsetInfo {
    uint32_t offset;
    bool is_passed_on_stack;
    bool is_used;
  };
  ArenaVector<StackOffsetInfo> parameter_stack_offsets_;

  struct Metadata {
    // Metadata used in llvm.read_register and llvm.write_register.
    llvm::Value* thread_register = nullptr;
    llvm::Value* marking_register = nullptr;
    llvm::Value* implicit_suspend_check_register = nullptr;
    llvm::Value* stack_pointer_register = nullptr;

    // Memory scope metadata used for loads and stores.
    llvm::MDNode* stack_scope = nullptr;
    llvm::MDNode* heap_scope = nullptr;
    // Memory that belongs to the runtime, e.g. the card table.
    llvm::MDNode* runtime_scope = nullptr;
    // Memory region pointed to by the thread register. It is beneficial to treat this as a separate
    // memory scope in some cases.
    llvm::MDNode* thread_object_scope = nullptr;

    llvm::MDNode* stack_noalias = nullptr;
    llvm::MDNode* heap_noalias = nullptr;
    llvm::MDNode* runtime_noalias = nullptr;
    llvm::MDNode* thread_object_noalias = nullptr;
    llvm::MDNode* call_noalias = nullptr;
  };
  Metadata metadata_;

  // Placeholder functions of patchpoints.
  struct PlaceholderFunctions {
    llvm::Function* cast_to_uncompressed = nullptr;
    llvm::Function* cast_to_compressed = nullptr;
    llvm::Function* cast_pointer_to_int = nullptr;
    llvm::Function* heap_reference_poisoning = nullptr;
    llvm::Function* implicit_suspend_check = nullptr;
    llvm::Function* suspend_check = nullptr;
    llvm::Function* load_gc_root = nullptr;
    llvm::Function* load_i1 = nullptr;
    llvm::Function* load_i8 = nullptr;
    llvm::Function* load_i16 = nullptr;
    llvm::Function* load_i32 = nullptr;
    llvm::Function* load_i64 = nullptr;
    llvm::Function* load_f32 = nullptr;
    llvm::Function* load_f64 = nullptr;
    llvm::Function* load_acquire_gc_root = nullptr;
    llvm::Function* load_acquire_i1 = nullptr;
    llvm::Function* load_acquire_i8 = nullptr;
    llvm::Function* load_acquire_i16 = nullptr;
    llvm::Function* load_acquire_i32 = nullptr;
    llvm::Function* load_acquire_i64 = nullptr;
    llvm::Function* discarded_load = nullptr;
    llvm::Function* store_gc_root = nullptr;
    llvm::Function* store_i1 = nullptr;
    llvm::Function* store_i8 = nullptr;
    llvm::Function* store_i16 = nullptr;
    llvm::Function* store_i32 = nullptr;
    llvm::Function* store_i64 = nullptr;
    llvm::Function* store_f32 = nullptr;
    llvm::Function* store_f64 = nullptr;
    llvm::Function* store_release_gc_root = nullptr;
    llvm::Function* store_release_i1 = nullptr;
    llvm::Function* store_release_i8 = nullptr;
    llvm::Function* store_release_i16 = nullptr;
    llvm::Function* store_release_i32 = nullptr;
    llvm::Function* store_release_i64 = nullptr;
    llvm::Function* memcpy_i16 = nullptr;
    llvm::Function* memcpy_i32 = nullptr;
    llvm::Function* memcpy_i8_zext_to_i16 = nullptr;
    llvm::Function* string_equals = nullptr;
  };
  PlaceholderFunctions placeholder_functions_;

  // Indexed by block id.
  ArenaVector<BasicBlockInfo> block_infos_;
  ArenaHashSet<HInstruction*> all_stack_saved_values_;

  // Parameters used by InvokeRuntime. The maximum number of arguments with the usual
  // ARTInvokeRuntime calling convention is 9: registers x0-7, and the current method pointer on the
  // stack.
  llvm::SmallVector<llvm::Value*, 9> invoke_runtime_parameters_;
  llvm::Type* invoke_runtime_return_type_ = nullptr;
  llvm::CallingConv::ID invoke_runtime_cc_ = llvm::CallingConv::ARTInvokeRuntime;
  std::optional<uint64_t> invoke_runtime_statepoint_id_ = std::nullopt;
  llvm::SmallVector<llvm::OperandBundleDef, 1> invoke_runtime_deopt_bundle_ = {};
  // Result of InvokeRuntime.
  llvm::CallBase* invoke_runtime_result_ = nullptr;

  std::optional<bool> can_use_implicit_suspend_checks_ = std::nullopt;

  // LLVM values associated with each instruction.
  // We use allocas for all non-constant values, and rely on the mem2reg pass to convert this back
  // to the usual SSA form. This is needed for try-catch code generation, because:
  //   * If a catch block uses a value that was created before entering the try block, that value
  //     needs to be saved to the stack before the try block, and loaded in the catch block.
  //   * The HIR graph doesn't provide these used values directly, so for simplicity we assume every
  //     value may be used in a catch block, and therefore needs to be saved to an alloca.
  //   * FIXME: Can we determine these values statically, to avoid unnecessary work for LLVM?
  ArenaHashMap<HInstruction*, llvm::Value*> instruction_value_map_;
  ArenaHashMap<HInstruction*, llvm::AllocaInst*> instruction_alloca_map_;
  HBasicBlock* current_block_ = nullptr;
  llvm::BasicBlock* alloca_block_ = nullptr;
  // Phi instructions to be resolved at the end of IR generation.
  ArenaDeque<std::pair<HPhi*, llvm::PHINode*>> phis_;
  struct StackSavedValuePhiInfo {
    HInstruction* value;
    llvm::PHINode* phi;
    HBasicBlock* block;
  };
  ArenaDeque<StackSavedValuePhiInfo> stack_saved_value_phis_;

  ArenaHashMap<const HTryBoundary*, llvm::BasicBlock*> try_boundary_catchswitch_map_;
  ArenaVector<llvm::Constant*> catch_block_addresses_;

  // Stack map id used for the entry suspend check instruction.
  uint64_t entry_suspend_check_id_ = static_cast<uint64_t>(PatchpointKind::kNone);

  // Since the 64 bit wide parameters occupy 2 virtual in-registers
  // we cannot use the information from the graph itself
  // have to map these HParameterValues to their exact index
  ArenaHashMap<HParameterValue*, unsigned int> parametervalue_index_map_;

  struct StackMapInfo {
    HInstruction* instruction;
    const DexFile* dex_file;
    uint32_t index_or_offset;
    bool is_used;
  };

  ArenaDeque<StackMapInfo> stack_map_infos_;

  std::unique_ptr<arm64::CodeGeneratorARM64> arm64_codegen_;

  InstructionCodeGeneratorARM64LLVM instruction_visitor_;

  // The generated machine code.
  ArenaVector<uint8_t> code_;

  // Call frame information buffer.
  ArenaVector<uint8_t> cfi_data_;

  // PC-relative method patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<PcRelativePatchInfo> boot_image_method_patches_;
  // PC-relative method patch info for kLoadMethodAppImageRelRo.
  ArenaDeque<PcRelativePatchInfo> app_image_method_patches_;
  // PC-relative method patch info for kBssEntry.
  ArenaDeque<PcRelativePatchInfo> method_bss_entry_patches_;
  // PC-relative type patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<PcRelativePatchInfo> boot_image_type_patches_;
  // PC-relative type patch info for kLoadClassAppImageRelRo.
  ArenaDeque<PcRelativePatchInfo> app_image_type_patches_;
  // PC-relative type patch info for kBssEntry.
  ArenaDeque<PcRelativePatchInfo> type_bss_entry_patches_;
  // PC-relative public type patch info for kBssEntryPublic.
  ArenaDeque<PcRelativePatchInfo> public_type_bss_entry_patches_;
  // PC-relative package type patch info for kBssEntryPackage.
  ArenaDeque<PcRelativePatchInfo> package_type_bss_entry_patches_;
  // PC-relative String patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<PcRelativePatchInfo> boot_image_string_patches_;
  // PC-relative String patch info for kBssEntry.
  ArenaDeque<PcRelativePatchInfo> string_bss_entry_patches_;
  // PC-relative method patch info for kLoadMethodTypeBssEntry.
  ArenaDeque<PcRelativePatchInfo> method_type_bss_entry_patches_;
  // PC-relative method patch info for kBootImageLinkTimePcRelative+kCallCriticalNative.
  ArenaDeque<PcRelativePatchInfo> boot_image_jni_entrypoint_patches_;
  // PC-relative patch info for IntrinsicObjects for the boot image,
  // and for method/type/string patches for kBootImageRelRo otherwise.
  ArenaDeque<PcRelativePatchInfo> boot_image_other_patches_;
  // Patch info for calls to entrypoint dispatch thunks. Used for slow paths.
  ArenaDeque<PatchInfo<uint64_t>> call_entrypoint_patches_;
  // Baker read barrier patch info.
  ArenaDeque<BakerReadBarrierPatchInfo> baker_read_barrier_patches_;

  friend class linker::Arm64RelativePatcherTest;
  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorARM64LLVM);
};

}  // namespace arm64_llvm
}  // namespace art HIDDEN

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_LLVM_H_
