/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "control_flow_simplifier.h"

#include "optimizing/nodes.h"
#include "reference_type_propagation.h"

namespace art HIDDEN {

static constexpr size_t kMaxInstructionsInBranch = 1u;

HControlFlowSimplifier::HControlFlowSimplifier(HGraph* graph,
                                               OptimizingCompilerStats* stats,
                                               const char* name)
    : HOptimization(graph, name, stats) {
}

// Returns true if `block` has only one predecessor, ends with a Goto
// or a Return and contains at most `kMaxInstructionsInBranch` other
// movable instruction with no side-effects.
static bool IsSimpleBlock(HBasicBlock* block) {
  if (block->GetPredecessors().size() != 1u) {
    return false;
  }
  DCHECK(block->GetPhis().IsEmpty());

  size_t num_instructions = 0u;
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* instruction = it.Current();
    if (instruction->IsControlFlow()) {
      return instruction->IsGoto() || instruction->IsReturn();
    } else if (instruction->CanBeMoved() &&
               !instruction->HasSideEffects() &&
               !instruction->CanThrow()) {
      if (instruction->IsSelect() && instruction->AsSelect()->GetCondition()->GetBlock() == block) {
        // Count one HCondition and HSelect in the same block as a single instruction.
        // This enables finding nested selects.
        continue;
      } else if (++num_instructions > kMaxInstructionsInBranch) {
        return false;  // bail as soon as we exceed number of allowed instructions
      }
    } else {
      return false;
    }
  }

  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

// Returns true if 'block1' and 'block2' are empty and merge into the
// same single successor.
static bool BlocksMergeTogether(HBasicBlock* block1, HBasicBlock* block2) {
  return block1->GetSingleSuccessor() == block2->GetSingleSuccessor();
}

// Search `block` for phis that have different inputs at `index1` and `index2`.
// If none is found, returns `{true, nullptr}`.
// If exactly one such `phi` is found, returns `{true, phi}`.
// Otherwise (if more than one such phi is found), returns `{false, nullptr}`.
static std::pair<bool, HPhi*> HasAtMostOnePhiWithDifferentInputs(HBasicBlock* block,
                                                                 size_t index1,
                                                                 size_t index2) {
  DCHECK_NE(index1, index2);

  HPhi* select_phi = nullptr;
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->AsPhi();
    auto&& inputs = phi->GetInputs();
    if (inputs[index1] == inputs[index2]) {
      continue;
    }
    if (select_phi == nullptr) {
      // First phi found.
      select_phi = phi;
    } else {
      // More than one phi found, return null.
      return {false, nullptr};
    }
  }
  return {true, select_phi};
}

bool HControlFlowSimplifier::TryGenerateSelectSimpleDiamondPattern(
    HBasicBlock* block, ScopedArenaSafeMap<HInstruction*, HSelect*>* cache) {
  DCHECK(block->GetLastInstruction()->IsIf());
  HIf* if_instruction = block->GetLastInstruction()->AsIf();
  HBasicBlock* true_block = if_instruction->IfTrueSuccessor();
  HBasicBlock* false_block = if_instruction->IfFalseSuccessor();
  DCHECK_NE(true_block, false_block);

  if (!IsSimpleBlock(true_block) ||
      !IsSimpleBlock(false_block) ||
      !BlocksMergeTogether(true_block, false_block)) {
    return false;
  }
  HBasicBlock* merge_block = true_block->GetSingleSuccessor();

  // If the branches are not empty, move instructions in front of the If.
  // TODO(dbrazdil): This puts an instruction between If and its condition.
  //                 Implement moving of conditions to first users if possible.
  while (!true_block->IsSingleGoto() && !true_block->IsSingleReturn()) {
    HInstruction* instr = true_block->GetFirstInstruction();
    DCHECK(!instr->CanThrow());
    instr->MoveBefore(if_instruction);
  }
  while (!false_block->IsSingleGoto() && !false_block->IsSingleReturn()) {
    HInstruction* instr = false_block->GetFirstInstruction();
    DCHECK(!instr->CanThrow());
    instr->MoveBefore(if_instruction);
  }
  DCHECK(true_block->IsSingleGoto() || true_block->IsSingleReturn());
  DCHECK(false_block->IsSingleGoto() || false_block->IsSingleReturn());

  // Find the resulting true/false values.
  size_t predecessor_index_true = merge_block->GetPredecessorIndexOf(true_block);
  size_t predecessor_index_false = merge_block->GetPredecessorIndexOf(false_block);
  DCHECK_NE(predecessor_index_true, predecessor_index_false);

  bool both_successors_return = true_block->IsSingleReturn() && false_block->IsSingleReturn();
  // TODO(solanes): Extend to support multiple phis? e.g.
  //   int a, b;
  //   if (bool) {
  //     a = 0; b = 1;
  //   } else {
  //     a = 1; b = 2;
  //   }
  //   // use a and b
  bool at_most_one_phi_with_different_inputs = false;
  HPhi* phi = nullptr;
  HInstruction* true_value = nullptr;
  HInstruction* false_value = nullptr;
  if (both_successors_return) {
    // Note: This can create a select with the same then-value and else-value.
    true_value = true_block->GetFirstInstruction()->InputAt(0);
    false_value = false_block->GetFirstInstruction()->InputAt(0);
  } else {
    std::tie(at_most_one_phi_with_different_inputs, phi) = HasAtMostOnePhiWithDifferentInputs(
        merge_block, predecessor_index_true, predecessor_index_false);
    if (!at_most_one_phi_with_different_inputs) {
      return false;
    }
    if (phi != nullptr) {
      true_value = phi->InputAt(predecessor_index_true);
      false_value = phi->InputAt(predecessor_index_false);
    }  // else we don't need to create a `HSelect` at all.
  }
  DCHECK(both_successors_return || at_most_one_phi_with_different_inputs);

  // Create the Select instruction and insert it in front of the If.
  HInstruction* condition = if_instruction->InputAt(0);
  HSelect* select = nullptr;
  if (both_successors_return || phi != nullptr) {
    select = new (graph_->GetAllocator()) HSelect(condition,
                                                  true_value,
                                                  false_value,
                                                  if_instruction->GetDexPc());
    block->InsertInstructionBefore(select, if_instruction);
    if (both_successors_return) {
      if (true_value->GetType() == DataType::Type::kReference) {
        DCHECK(false_value->GetType() == DataType::Type::kReference);
        ReferenceTypePropagation::FixUpSelectType(select, graph_->GetHandleCache());
      }
      false_block->GetFirstInstruction()->ReplaceInput(select, 0);
    } else {
      if (phi->GetType() == DataType::Type::kReference) {
        select->SetReferenceTypeInfoIfValid(phi->GetReferenceTypeInfo());
      }
      phi->ReplaceInput(select, predecessor_index_false);  // We'll remove the true branch below.
    }
  }

  // Remove the true branch which removes the corresponding Phi input if needed.
  // If left only with the false branch, the Phi is automatically removed.
  true_block->DisconnectAndDelete();

  // Merge remaining blocks which are now connected with Goto.
  DCHECK_EQ(block->GetSingleSuccessor(), false_block);
  block->MergeWith(false_block);
  if (!both_successors_return && merge_block->GetPredecessors().size() == 1u) {
    DCHECK_IMPLIES(phi != nullptr, phi->GetBlock() == nullptr);
    DCHECK(merge_block->GetPhis().IsEmpty());
    DCHECK_EQ(block->GetSingleSuccessor(), merge_block);
    block->MergeWith(merge_block);
  }

  MaybeRecordStat(stats_, select != nullptr ? MethodCompilationStat::kControlFlowSelectGenerated
                                            : MethodCompilationStat::kControlFlowDiamondRemoved);

  // Very simple way of finding common subexpressions in the generated HSelect statements
  // (since this runs after GVN). Lookup by condition, and reuse latest one if possible
  // (due to post order, latest select is most likely replacement). If needed, we could
  // improve this by e.g. using the operands in the map as well.
  if (select != nullptr) {
    auto it = cache->find(condition);
    if (it == cache->end()) {
      cache->Put(condition, select);
    } else {
      // Found cached value. See if latest can replace cached in the HIR.
      HSelect* cached_select = it->second;
      DCHECK_EQ(cached_select->GetCondition(), select->GetCondition());
      if (cached_select->GetTrueValue() == select->GetTrueValue() &&
          cached_select->GetFalseValue() == select->GetFalseValue() &&
          select->StrictlyDominates(cached_select)) {
        cached_select->ReplaceWith(select);
        cached_select->GetBlock()->RemoveInstruction(cached_select);
      }
      it->second = select;  // always cache latest
    }
  }

  // No need to update dominance information, as we are simplifying
  // a simple diamond shape, where the join block is merged with the
  // entry block. Any following blocks would have had the join block
  // as a dominator, and `MergeWith` handles changing that to the
  // entry block
  return true;
}

bool HControlFlowSimplifier::TryFlattenMerge(HBasicBlock* block,
                                             size_t reverse_post_order_index,
                                             ArenaBitVector* visited_blocks) {
  DCHECK(block->GetFirstInstruction()->IsGoto());
  DCHECK_EQ(block->GetFirstInstruction(), block->GetLastInstruction());
  HBasicBlock* successor = block->GetSingleSuccessor();
  DCHECK(!successor->IsExitBlock());  // `HGoto` does not flow to exit block.
  if (block->GetPredecessors().size() < 2u || successor->GetPredecessors().size() < 2u) {
    return false;
  }
  if (block->IsCatchBlock() || successor->IsCatchBlock()) {
    // Phi inputs do not correspond to catch block predecessors. Do not flatten.
    return false;
  }
  if (block->GetLoopInformation() != successor->GetLoopInformation()) {
    // The `block` is a pre-header, including the case when `successor` is an irreducible
    // loop entry that's not actually marked as loop header in the `HLoopInformation`.
    return false;
  }
  if (block->IsInLoop()) {
    // Do not merge if the `block` or the `successor` is a loop header, including irreducible
    // loop entries that are not actually marked as loop header in the `HLoopInformation`.
    // Even for irreducible loops, check for the recorded loop header first.
    HLoopInformation* loop_info = block->GetLoopInformation();
    if (block == loop_info->GetHeader() || successor == loop_info->GetHeader()) {
      return false;
    }
    if (UNLIKELY(loop_info->IsIrreducible())) {
      auto is_loop_header = [loop_info](HBasicBlock* b) {
        DCHECK_EQ(loop_info, b->GetLoopInformation());
        auto&& predecessors = b->GetPredecessors();
        return std::any_of(
            predecessors.begin(),
            predecessors.end(),
            [loop_info](HBasicBlock* p) { return p->GetLoopInformation() != loop_info; });
      };
      if (is_loop_header(block) || is_loop_header(successor)) {
        return false;
      }
    }
  }

  block->TakeGotoBlockSuccessorsOtherPredecessorsAndMergePhis();

  // Fix up domination information for unmerged blocks before calling `MergeWith()`.
  HBasicBlock* dominator = successor->GetDominator();
  if (block->GetDominator() == dominator) {
    dominator->RemoveDominatedBlock(successor);
  } else {
    block->GetDominator()->RemoveDominatedBlock(block);
    block->SetDominator(dominator);
    dominator->ReplaceDominatedBlock(successor, block);
  }
  successor->SetDominator(block);
  block->AddDominatedBlock(successor);

  // Move predecessors before `block` in reverse post order if needed.
  ScopedArenaAllocator allocator(graph_->GetArenaStack());
  ArenaBitVector predecessors_to_move(&allocator,
                                      graph_->GetBlocks().size(),
                                      /* expandable= */ false,
                                      kArenaAllocControlFlowSimplifier);
  ScopedArenaVector<HBasicBlock*> work_queue(allocator.Adapter(kArenaAllocLSE));
  auto mark_predecessors = [&](HBasicBlock* current) {
    for (HBasicBlock* predecessor : current->GetPredecessors()) {
      if (visited_blocks->IsBitSet(predecessor->GetBlockId()) &&
          !predecessors_to_move.IsBitSet(predecessor->GetBlockId())) {
        predecessors_to_move.SetBit(predecessor->GetBlockId());
        work_queue.push_back(predecessor);
      }
    }
  };
  mark_predecessors(block);
  if (!work_queue.empty()) {
    do {
      HBasicBlock* current = work_queue.back();
      work_queue.pop_back();
      mark_predecessors(current);
    } while (!work_queue.empty());
    // Move blocks marked in `predecessors_to_move` to the correct position in the reverse
    // post order while extracting `block` and other unmarked blocks to a temporary vector.
    ScopedArenaVector<HBasicBlock*> extracted(allocator.Adapter(kArenaAllocLSE));
    auto moved_end = graph_->reverse_post_order_.begin() + reverse_post_order_index;
    DCHECK_EQ(block, *moved_end);
    DCHECK(!predecessors_to_move.IsBitSet(block->GetBlockId()));
    extracted.push_back(block);
    auto move_it = std::next(moved_end);
    DCHECK(move_it != graph_->reverse_post_order_.end());
    while (*move_it != successor) {
      if (predecessors_to_move.IsBitSet((*move_it)->GetBlockId())) {
        *moved_end = *move_it;
        ++moved_end;
      } else {
        extracted.push_back(*move_it);
      }
      ++move_it;
      DCHECK(move_it != graph_->reverse_post_order_.end());
    }
    // Place extracted blocks in the freed range in reverse post order.
    DCHECK_EQ(static_cast<size_t>(std::distance(moved_end, move_it)), extracted.size());
    std::copy(extracted.begin(), extracted.end(), moved_end);
  }

  // Finish the merge using `MergeWith()`.
  block->MergeWith(successor);

  MaybeRecordStat(stats_, MethodCompilationStat::kControlFlowFlattenedMerge);
  return true;
}

bool HControlFlowSimplifier::Run() {
  bool did_simplify = false;

  ScopedArenaAllocator allocator(graph_->GetArenaStack());
  // Select cache with local allocator for `TryGenerateSelectSimpleDiamondPattern()`.
  ScopedArenaSafeMap<HInstruction*, HSelect*> select_cache(
      std::less<HInstruction*>(), allocator.Adapter(kArenaAllocControlFlowSimplifier));
  // Mark visited blocks by block id for reverse post order fixup in `TryFlattenMerge()`.
  ArenaBitVector visited_blocks(&allocator,
                                graph_->GetBlocks().size(),
                                /* expandable= */ false,
                                kArenaAllocControlFlowSimplifier);

  // Iterate in post order in the case that simplifying a block exposes simplification
  // opportunities for earier blocks. Do not process the entry block.
  // We may remove blocks from the reverse post order array, so make the iteration very explicit.
  HBasicBlock* const * reverse_post_order_data = graph_->GetReversePostOrder().data();
  size_t reverse_post_order_index = graph_->GetReversePostOrder().size();
  while (reverse_post_order_index != /* Do not process entry block with index 0. */ 1u) {
    --reverse_post_order_index;
    HBasicBlock* block = reverse_post_order_data[reverse_post_order_index];
    DCHECK(block != nullptr);
    DCHECK(block->GetFirstInstruction() != nullptr);
    HInstruction* last = block->GetLastInstruction();
    DCHECK(last != nullptr);
    bool rpo_may_have_changed = false;
    if (last->IsGoto()) {
      if (last == block->GetFirstInstruction() &&
          TryFlattenMerge(block, reverse_post_order_index, &visited_blocks)) {
        did_simplify = true;
        rpo_may_have_changed = true;
      }
    } else if (block->GetLastInstruction()->IsIf()) {
      if (TryGenerateSelectSimpleDiamondPattern(block, &select_cache)) {
        did_simplify = true;
      }
    }
    DCHECK_LT(block->GetBlockId(), graph_->GetBlocks().size());
    visited_blocks.SetBit(block->GetBlockId());
    // Blocks with higher indexes may have been removed but the `block` remains at the same index.
    // Removing from a `std::vector<>` does not change the data pointer.
    DCHECK_EQ(reverse_post_order_data, graph_->GetReversePostOrder().data());
    DCHECK_LT(reverse_post_order_index, graph_->GetReversePostOrder().size());
    if (rpo_may_have_changed) {
      DCHECK_GE(IndexOfElement(graph_->GetReversePostOrder(), block), reverse_post_order_index);
    } else {
      DCHECK_EQ(block, reverse_post_order_data[reverse_post_order_index]);
    }
  }
  DCHECK_EQ(reverse_post_order_index, 1u);
  DCHECK(reverse_post_order_data[0u]->IsEntryBlock());
  return did_simplify;
}

}  // namespace art
