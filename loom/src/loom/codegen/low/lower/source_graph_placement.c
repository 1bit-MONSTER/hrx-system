// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/source_graph_placement.h"

#include "loom/analysis/motion.h"
#include "loom/codegen/low/lower/context.h"
#include "loom/codegen/low/lower/rule_match.h"

struct loom_low_lower_source_graph_placement_t {
  // Lazy source-order read barrier index for the immutable source function.
  loom_motion_read_barrier_table_t* read_barriers;
};

iree_status_t loom_low_lower_source_graph_placement_evaluate(
    loom_low_lower_context_t* context,
    const loom_low_lower_rule_selection_t* selection, bool* out_legal) {
  *out_legal = false;
  IREE_ASSERT(selection != NULL);
  IREE_ASSERT(selection->rule != NULL);
  IREE_ASSERT_GT(selection->source_node_count, 0);
  IREE_ASSERT_LT(selection->emission_source_node_index,
                 selection->source_node_count);
  const loom_op_t* emission_op =
      selection->source_nodes[selection->emission_source_node_index];
  const loom_op_t* root_op = selection->source_nodes[0];
  IREE_ASSERT_EQ(root_op->parent_block, emission_op->parent_block);
  if (selection->emission_source_node_index == 0) {
    *out_legal = true;
    return iree_ok_status();
  }
  if (loom_motion_op_can_relocate_effect_free(context->module, root_op)) {
    *out_legal = true;
    return iree_ok_status();
  }
  if (!loom_motion_op_is_ordinary_load(context->module, root_op) ||
      root_op->block_ordinal >= emission_op->block_ordinal) {
    return iree_ok_status();
  }
  if (context->source_graph_placement == NULL) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate(
        &context->analysis_arena, sizeof(*context->source_graph_placement),
        (void**)&context->source_graph_placement));
    *context->source_graph_placement =
        (loom_low_lower_source_graph_placement_t){0};
  }
  loom_low_lower_source_graph_placement_t* placement =
      context->source_graph_placement;
  if (placement->read_barriers == NULL) {
    IREE_RETURN_IF_ERROR(loom_motion_read_barrier_table_create(
        context->module, &context->analysis_arena, &placement->read_barriers));
  }
  bool can_cross = false;
  IREE_RETURN_IF_ERROR(loom_motion_read_barrier_table_can_cross(
      placement->read_barriers, root_op, emission_op, &can_cross));
  if (!can_cross) {
    return iree_ok_status();
  }

  *out_legal = true;
  return iree_ok_status();
}
