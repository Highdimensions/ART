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

#include "code_flow_simplifier.h"

#include "optimizing/nodes.h"
#include "reference_type_propagation.h"

namespace art HIDDEN {

static constexpr size_t kMaxInstructionsInBranch = 1u;

HCodeFlowSimplifier::HCodeFlowSimplifier(HGraph* graph,
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

// Returns nullptr if `block` has either no phis or there is more than one phi. Otherwise returns
// that phi.
static HPhi* GetSinglePhi(HBasicBlock* block, size_t index1, size_t index2) {
  DCHECK_NE(index1, index2);

  HPhi* select_phi = nullptr;
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->AsPhi();
    if (select_phi == nullptr) {
      // First phi found.
      select_phi = phi;
    } else {
      // More than one phi found, return null.
      return nullptr;
    }
  }
  return select_phi;
}

bool HCodeFlowSimplifier::TryGenerateSelectSimpleDiamondPattern(
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
  HPhi* phi = GetSinglePhi(merge_block, predecessor_index_true, predecessor_index_false);

  HInstruction* true_value = nullptr;
  HInstruction* false_value = nullptr;
  if (both_successors_return) {
    true_value = true_block->GetFirstInstruction()->InputAt(0);
    false_value = false_block->GetFirstInstruction()->InputAt(0);
  } else if (phi != nullptr) {
    true_value = phi->InputAt(predecessor_index_true);
    false_value = phi->InputAt(predecessor_index_false);
  } else {
    return false;
  }
  DCHECK(both_successors_return || phi != nullptr);

  // Create the Select instruction and insert it in front of the If.
  HInstruction* condition = if_instruction->InputAt(0);
  HSelect* select = new (graph_->GetAllocator()) HSelect(condition,
                                                          true_value,
                                                          false_value,
                                                          if_instruction->GetDexPc());
  if (both_successors_return) {
    if (true_value->GetType() == DataType::Type::kReference) {
      DCHECK(false_value->GetType() == DataType::Type::kReference);
      ReferenceTypePropagation::FixUpSelectType(select, graph_->GetHandleCache());
    }
  } else if (phi->GetType() == DataType::Type::kReference) {
    select->SetReferenceTypeInfoIfValid(phi->GetReferenceTypeInfo());
  }
  block->InsertInstructionBefore(select, if_instruction);

  // Remove the true branch which removes the corresponding Phi
  // input if needed. If left only with the false branch, the Phi is
  // automatically removed.
  if (both_successors_return) {
    false_block->GetFirstInstruction()->ReplaceInput(select, 0);
  } else {
    phi->ReplaceInput(select, predecessor_index_false);
  }

  bool only_two_predecessors = (merge_block->GetPredecessors().size() == 2u);
  true_block->DisconnectAndDelete();

  // Merge remaining blocks which are now connected with Goto.
  DCHECK_EQ(block->GetSingleSuccessor(), false_block);
  block->MergeWith(false_block);
  if (!both_successors_return && only_two_predecessors) {
    DCHECK_EQ(only_two_predecessors, phi->GetBlock() == nullptr);
    DCHECK_EQ(block->GetSingleSuccessor(), merge_block);
    block->MergeWith(merge_block);
  }

  MaybeRecordStat(stats_, MethodCompilationStat::kSelectGenerated);

  // Very simple way of finding common subexpressions in the generated HSelect statements
  // (since this runs after GVN). Lookup by condition, and reuse latest one if possible
  // (due to post order, latest select is most likely replacement). If needed, we could
  // improve this by e.g. using the operands in the map as well.
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

  // No need to update dominance information, as we are simplifying
  // a simple diamond shape, where the join block is merged with the
  // entry block. Any following blocks would have had the join block
  // as a dominator, and `MergeWith` handles changing that to the
  // entry block
  return true;
}

HBasicBlock* HCodeFlowSimplifier::TryFixupDoubleDiamondPattern(HBasicBlock* block) {
  DCHECK(block->GetLastInstruction()->IsIf());
  HIf* if_instruction = block->GetLastInstruction()->AsIf();
  HBasicBlock* true_block = if_instruction->IfTrueSuccessor();
  HBasicBlock* false_block = if_instruction->IfFalseSuccessor();
  DCHECK_NE(true_block, false_block);

  // One branch must be a single goto, and the other one the inner if.
  if (true_block->IsSingleGoto() == false_block->IsSingleGoto()) {
    return nullptr;
  }

  HBasicBlock* single_goto = true_block->IsSingleGoto() ? true_block : false_block;
  HBasicBlock* inner_if_block = true_block->IsSingleGoto() ? false_block : true_block;

  // The innner if branch has to be a block with just a comparison and an if.
  if (!inner_if_block->EndsWithIf() ||
      inner_if_block->GetLastInstruction()->AsIf()->InputAt(0) !=
          inner_if_block->GetFirstInstruction() ||
      inner_if_block->GetLastInstruction()->GetPrevious() !=
          inner_if_block->GetFirstInstruction() ||
      !inner_if_block->GetFirstInstruction()->IsCondition()) {
    return nullptr;
  }

  HIf* inner_if_instruction = inner_if_block->GetLastInstruction()->AsIf();
  HBasicBlock* inner_if_true_block = inner_if_instruction->IfTrueSuccessor();
  HBasicBlock* inner_if_false_block = inner_if_instruction->IfFalseSuccessor();
  if (!inner_if_true_block->IsSingleGoto() || !inner_if_false_block->IsSingleGoto()) {
    return nullptr;
  }

  // One must merge into the outer condition and the other must not.
  if (BlocksMergeTogether(single_goto, inner_if_true_block) ==
      BlocksMergeTogether(single_goto, inner_if_false_block)) {
    return nullptr;
  }

  // First merge merges the outer if with one of the inner if branches. The block must be a Phi and
  // a Goto.
  HBasicBlock* first_merge = single_goto->GetSingleSuccessor();
  if (first_merge->GetNumberOfPredecessors() != 2 ||
      first_merge->GetPhis().CountSize() != 1 ||
      !first_merge->GetLastInstruction()->IsGoto() ||
      first_merge->GetFirstInstruction() != first_merge->GetLastInstruction()) {
    return nullptr;
  }

  HPhi* first_phi = first_merge->GetFirstPhi()->AsPhi();

  // Second merge is first_merge and the remainder branch merging. It must be phi + goto, or phi +
  // return. Depending on the first merge, we define the second merge.
  HBasicBlock* merges_into_second_merge =
    BlocksMergeTogether(single_goto, inner_if_true_block)
      ? inner_if_false_block
      : inner_if_true_block;
  if (!BlocksMergeTogether(first_merge, merges_into_second_merge)) {
    return nullptr;
  }

  HBasicBlock* second_merge = merges_into_second_merge->GetSingleSuccessor();
  if (second_merge->GetNumberOfPredecessors() != 2 ||
      second_merge->GetPhis().CountSize() != 1 ||
      !(second_merge->GetLastInstruction()->IsGoto() ||
        second_merge->GetLastInstruction()->IsReturn()) ||
      second_merge->GetFirstInstruction() != second_merge->GetLastInstruction()) {
    return nullptr;
  }

  size_t index = second_merge->GetPredecessorIndexOf(merges_into_second_merge);
  HPhi* second_phi = second_merge->GetFirstPhi()->AsPhi();

  // Merge the phis.
  first_phi->AddInput(second_phi->InputAt(index));
  merges_into_second_merge->ReplaceSuccessor(second_merge, first_merge);
  second_phi->ReplaceWith(first_phi);
  second_merge->RemovePhi(second_phi);

  // Sort out the new domination before merging the blocks
  DCHECK_EQ(second_merge->GetSinglePredecessor(), first_merge);
  second_merge->GetDominator()->RemoveDominatedBlock(second_merge);
  second_merge->SetDominator(first_merge);
  first_merge->AddDominatedBlock(second_merge);
  first_merge->MergeWith(second_merge);

  // No need to update dominance information. There's a chance that `merges_into_second_merge`
  // doesn't come before `first_merge` but we don't need to fix it since `merges_into_second_merge`
  // will disappear from the graph altogether when doing the follow-up
  // TryGenerateSelectSimpleDiamondPattern.

  return inner_if_block;
}

bool HCodeFlowSimplifier::TryMergeGotoBlock(HBasicBlock* block) {
  DCHECK(block->GetFirstInstruction()->IsGoto());
  DCHECK_EQ(block->GetFirstInstruction(), block->GetLastInstruction());
  HBasicBlock* successor = block->GetSingleSuccessor();
  if (block->GetLoopInformation() != successor->GetLoopInformation()) {
    // `block` is a pre-header, including the case when `successor` is an irreducible loop
    // entry that's not actually marked as loop header in the `HLoopInformation`.
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
  if (block->GetPredecessors().size() < 2u) {
    return false;
  }
  block->MergeGotoBlockWithSuccessor();
  return true;
}

bool HCodeFlowSimplifier::Run() {
  bool did_simplify = false;
  bool rebuild = false;

  // Select cache with local allocator.
  ScopedArenaAllocator allocator(graph_->GetArenaStack());
  ScopedArenaSafeMap<HInstruction*, HSelect*> select_cache(
      std::less<HInstruction*>(), allocator.Adapter(kArenaAllocCodeFlowSimplifier));

  // Iterate in post order in the case that simplifying a block exposes simplification
  // opportunities for earier blocks. Do not process the entry block.
  // We may remove blocks from the reverse post order array, so make the iteration very explict.
  HBasicBlock* const * reverse_post_order_data = graph_->GetReversePostOrder().data();
  size_t reverse_post_order_index = graph_->GetReversePostOrder().size();
  while (reverse_post_order_index != /* Do not process entry block with index 0. */ 1u) {
    --reverse_post_order_index;
    HBasicBlock* block = reverse_post_order_data[reverse_post_order_index];
    DCHECK(block != nullptr);
    DCHECK(block->GetFirstInstruction() != nullptr);
    DCHECK(block->GetLastInstruction() != nullptr);
    if (block->GetFirstInstruction()->IsGoto()) {
      if (TryMergeGotoBlock(block)) {
        did_simplify = true;
        rebuild = true;
      }
    } else if (block->GetLastInstruction()->IsIf()) {
      if (TryGenerateSelectSimpleDiamondPattern(block, &select_cache)) {
        did_simplify = true;
      }
    }
    // Blocks with higher indexes may have been removed but the `block` remains at the same index.
    // Removing from a `std::vector<>` does not change the data pointer.
    DCHECK_EQ(reverse_post_order_data, graph_->GetReversePostOrder().data());
    DCHECK_LT(reverse_post_order_index, graph_->GetReversePostOrder().size());
    DCHECK_EQ(block, reverse_post_order_data[reverse_post_order_index]);
  }
  DCHECK_EQ(reverse_post_order_index, 1u);
  DCHECK(reverse_post_order_data[0u]->IsEntryBlock());

  if (rebuild) {
    // TODO: We only need to update the reverse post order (RPO), all other information is correct.
    //
    // We could mark visited blocks in a `BitVector` indexed by the block id. Then, before merging
    // blocks in the `TryMergeGotoBlock()`, we could collect all marked predecessors of `successor`
    // as they need to be moved before the `block` in RPO, recording them in another `BitVector`.
    // If there are any, we would move them with a single pass over the RPO from `block` to
    // `successor` to the start of this RPO range while collecting other blocks (including the
    // `block`) in a temporary list (vector). Those collected blocks can then be re-inserted
    // between the kept blocks and `successor`, fixing the RPO.
    //
    // Alternatively, we could templatize `HGraph::ComputeDominanceInformation()` with
    // `template <bool kReversePostOrderOnly>`  to easily implement `RecomputeReversePostOrder()`
    // that keeps the dominance information untouched.
    graph_->RecomputeDominatorTree();
  }
  return did_simplify;
}

}  // namespace art
