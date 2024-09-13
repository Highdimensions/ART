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

#include "instruction_simplifier_riscv64.h"

#include "instruction_simplifier.h"

namespace art HIDDEN {

namespace riscv64 {

class InstructionSimplifierRiscv64Visitor final : public HGraphVisitor {
 public:
  InstructionSimplifierRiscv64Visitor(HGraph* graph, OptimizingCompilerStats* stats)
      : HGraphVisitor(graph), stats_(stats) {}

 private:
  void RecordSimplification() {
    MaybeRecordStat(stats_, MethodCompilationStat::kInstructionSimplificationsArch);
  }

  void VisitBasicBlock(HBasicBlock* block) override {
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      if (instruction->IsInBlock()) {
        instruction->Accept(this);
      }
    }
  }

  // Replace Add which has Shl with distance of 1 or 2 or 3 with Riscv64ShiftAdd
  bool TryReplaceAddsWithShiftAdds(HShl* shl) {
    // There is no reason to replace Int32 Shl+Add with ShiftAdd because of
    // additional sign-extension required.
    if (shl->GetType() != DataType::Type::kInt64) {
      return false;
    }

    if (!shl->GetRight()->IsConstant()) {
      return false;
    }

    // The bytecode does not permit the shift distance to come from a wide variable
    DCHECK(shl->GetRight()->IsIntConstant());

    const int32_t distance = shl->GetRight()->AsIntConstant()->GetValue();
    if (distance < 1 || distance > 3) {
      return false;
    }

    bool replaced = false;

    for (const HUseListNode<HInstruction*>& use : shl->GetUses()) {
      HInstruction* user = use.GetUser();

      if (!user->IsAdd()) {
        continue;
      }
      HAdd* add = user->AsAdd();
      HInstruction* left = add->GetLeft();
      HInstruction* right = add->GetRight();
      DCHECK_EQ(add->GetType(), DataType::Type::kInt64)
          << "Replaceable Add must be the same 64 bit type as the input";

      // If the HAdd to replace has both inputs the same HShl<1|2|3>, then
      // don't perform the optimization. The processor will not be able to execute
      // these shifts parallel which is the purpose of the replace below.
      if (left == right) {
        continue;
      }

      HInstruction* add_other_input = left == shl ? right : left;
      HRiscv64ShiftAdd* shift_add = new (GetGraph()->GetAllocator())
          HRiscv64ShiftAdd(shl->GetLeft(), add_other_input, distance);

      add->GetBlock()->ReplaceAndRemoveInstructionWith(add, shift_add);
      replaced = true;
    }

    if (!shl->HasUses()) {
      shl->GetBlock()->RemoveInstruction(shl);
    }

    return replaced;
  }

  bool TryReplaceShrAndWithBitExtract(HBinaryOperation* op) {
    HInstruction* left = op->GetLeft();
    HInstruction* right = op->GetRight();
    DataType::Type result_type = op->GetType();

    HInstruction* op_value = (!left->IsShr()) ? left : right;
    if (!op_value->IsConstant()) {
      return false;
    } else if (!op_value->AsConstant()->IsOne()) {
      return false;
    }

    HShr* shr = (left->IsShr()) ? left->AsShr() : right->AsShr();

    HInstruction* shamt = shr->GetRight();
    HInstruction* shr_op = shr->GetLeft();

    HRiscv64BitExtract* bit_extract =
        new (GetGraph()->GetAllocator()) HRiscv64BitExtract(result_type, shr_op, shamt);
    op->GetBlock()->ReplaceAndRemoveInstructionWith(op, bit_extract);

    if (!shr->HasUses()) {
      shr->GetBlock()->RemoveInstruction(shr);
    }

    return true;
  }

  bool TryOptimizeBinaryOpWithConstWithBitManipulation(HBinaryOperation* op) {
    DCHECK(op->IsOr() || op->IsXor() || op->IsAnd());
    HInstruction* left = op->GetLeft();
    HInstruction* right = op->GetRight();
    DataType::Type result_type = op->GetType();

    HInstruction* op_value;
    uint64_t imm;
    if (left->IsConstant()) {
      imm = left->AsConstant()->GetValueAsUint64();
      op_value = right;
    } else {
      imm = right->AsConstant()->GetValueAsUint64();
      op_value = left;
    }

    int32_t power;
    if (op->IsAnd()) {
      if (!IsPowerOfTwo(~imm)) {
        return false;
      } else {
        power = WhichPowerOf2(~imm);
      }
    } else if (!IsPowerOfTwo(imm)) {
      return false;
    } else {
      power = WhichPowerOf2(imm);
    }

    HConstant* shamt = GetGraph()->GetConstant(DataType::Type::kInt32, power);

    if (op->IsOr()) {
      HRiscv64BitSet* bit_set =
          new (GetGraph()->GetAllocator()) HRiscv64BitSet(result_type, op_value, shamt);
      op->GetBlock()->ReplaceAndRemoveInstructionWith(op, bit_set);
    }

    if (op->IsXor()) {
      HRiscv64BitInvert* bit_invert =
          new (GetGraph()->GetAllocator()) HRiscv64BitInvert(result_type, op_value, shamt);
      op->GetBlock()->ReplaceAndRemoveInstructionWith(op, bit_invert);
    }

    if (op->IsAnd()) {
      HRiscv64BitClear* bit_clear =
          new (GetGraph()->GetAllocator()) HRiscv64BitClear(result_type, op_value, shamt);
      op->GetBlock()->ReplaceAndRemoveInstructionWith(op, bit_clear);
    }

    return true;
  }

  // Replace code looking like
  //    SHR tmp, a, b
  //    AND dst, 1, tmp
  // with
  //    Riscv64BitExtract dst, a, b
  // Replace code looking like
  //    SHL tmp, 1, b
  //    OR dst, a, tmp
  // with
  //    Riscv64BitSet dst, a, b
  // Replace code looking like
  //    SHL tmp, 1, b
  //    XOR dst, a, tmp
  // with
  //    Riscv64BitInvert dst, a, b
  // Replace code looking like
  //    SHL tmp, 1, b
  //    NOT tmp, tmp
  //    AND dst, a, tmp
  // with
  //    Riscv64BitClear dst, a, b
  bool TryOptimizeWithBitManipulation(HBinaryOperation* op) {
    DCHECK(op->IsAnd() || op->IsOr() || op->IsXor());
    if (!op->HasUses()) {
      return false;
    }

    DataType::Type result_type = op->GetType();
    if (result_type != DataType::Type::kInt64) {
      return false;
    }

    HInstruction* left = op->GetLeft();
    HInstruction* right = op->GetRight();

    if (left->IsShr() || right->IsShr()) {
      return TryReplaceShrAndWithBitExtract(op);
    }
    if ((left->IsConstant() || right->IsConstant())) {
      return TryOptimizeBinaryOpWithConstWithBitManipulation(op);
    }

    HShl* shl;
    HNot* hnot = nullptr;
    HInstruction* op_value;

    if (left->IsNot() || right->IsNot()) {
      hnot = (left->IsNot()) ? left->AsNot() : right->AsNot();
      if (!hnot->GetInput()->IsShl()) {
        return false;
      }
      shl = hnot->GetInput()->AsShl();
      op_value = (!left->IsNot()) ? left : right;
    } else {
      if (!left->IsShl() && !right->IsShl()) {
        return false;
      }
      if (left->IsShl()) {
        shl = left->AsShl();
        op_value = right;
      } else {
        shl = right->AsShl();
        op_value = left;
      }
    }

    HInstruction* shl_op = shl->GetLeft();
    if (!shl_op->IsConstant()) {
      return false;
    } else if (!shl_op->AsConstant()->IsOne()) {
      return false;
    }
    HInstruction* shamt = shl->GetRight();

    if (op->IsOr()) {
      HRiscv64BitSet* bit_set =
          new (GetGraph()->GetAllocator()) HRiscv64BitSet(result_type, op_value, shamt);
      op->GetBlock()->ReplaceAndRemoveInstructionWith(op, bit_set);
    }

    if (op->IsXor()) {
      HRiscv64BitInvert* bit_invert =
          new (GetGraph()->GetAllocator()) HRiscv64BitInvert(result_type, op_value, shamt);
      op->GetBlock()->ReplaceAndRemoveInstructionWith(op, bit_invert);
    }

    if (op->IsAnd()) {
      if (hnot == nullptr) {
        return false;
      }
      HRiscv64BitClear* bit_clear =
          new (GetGraph()->GetAllocator()) HRiscv64BitClear(result_type, op_value, shamt);
      op->GetBlock()->ReplaceAndRemoveInstructionWith(op, bit_clear);
      if (!hnot->HasUses()) {
        hnot->GetBlock()->RemoveInstruction(hnot);
      }
    }

    if (!shl->HasUses()) {
      shl->GetBlock()->RemoveInstruction(shl);
    }

    return true;
  }

  void VisitAnd(HAnd* inst) override {
    if (TryOptimizeWithBitManipulation(inst)) {
      RecordSimplification();
    } else if (TryMergeNegatedInput(inst)) {
      RecordSimplification();
    }
  }

  void VisitOr(HOr* inst) override {
    if (TryMergeNegatedInput(inst)) {
      RecordSimplification();
    } else if (TryOptimizeWithBitManipulation(inst)) {
      RecordSimplification();
    }
  }

  void VisitXor(HXor* inst) override {
    if (TryMergeNegatedInput(inst)) {
      RecordSimplification();
    } else if (TryOptimizeWithBitManipulation(inst)) {
      RecordSimplification();
    }
  }

  void VisitShl(HShl* inst) override {
    // Replace code looking like
    //    SHL tmp, a, 1 or 2 or 3
    //    ADD dst, tmp, b
    // with
    //    Riscv64ShiftAdd dst, a, b
    if (TryReplaceAddsWithShiftAdds(inst)) {
      RecordSimplification();
    }
  }

  void VisitSub(HSub* inst) override {
    if (TryMergeWithAnd(inst)) {
      RecordSimplification();
    }
  }

  OptimizingCompilerStats* stats_ = nullptr;
};

bool InstructionSimplifierRiscv64::Run() {
  auto visitor = InstructionSimplifierRiscv64Visitor(graph_, stats_);
  visitor.VisitReversePostOrder();
  return true;
}

}  // namespace riscv64
}  // namespace art
