// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/control_plan.h"

#include <string.h>

#include "loom/codegen/low/lower/context.h"
#include "loom/codegen/low/lower/realization.h"
#include "loom/ir/module.h"
#include "loom/ops/cfg/ops.h"

typedef struct loom_low_lower_control_source_t {
  // Target branch expansion selected at this source block's terminator.
  loom_low_lower_plan_t branch;
  // Effective successor identities; NULL uses the authored destinations.
  loom_low_lower_block_ref_t* successors;
  // Per-operand conversion recipes; absent when selected types suffice.
  const void** operands;
} loom_low_lower_control_source_t;

typedef struct loom_low_lower_control_block_t {
  // Existing planned block before which to insert, or zero for the tail.
  loom_low_lower_block_ref_t before;
  // Authored block supplying the complete signature, or zero for no arguments.
  // Synthetic signature references are resolved once when this row is created.
  loom_low_lower_block_ref_t signature;
} loom_low_lower_control_block_t;

struct loom_low_lower_control_plan_t {
  // Source-block-indexed branch recipes and edge rewrites.
  loom_low_lower_control_source_t* sources;
  // Synthetic block recipes in creation order.
  loom_low_lower_control_block_t* blocks;
  // Number of populated synthetic block recipes.
  iree_host_size_t block_count;
  // Allocated synthetic block capacity.
  iree_host_size_t block_capacity;
};

loom_low_lower_block_ref_t loom_low_lower_control_source_block(
    const loom_block_t* source_block) {
  return (loom_low_lower_block_ref_t)source_block->region_index + 1;
}

iree_host_size_t loom_low_lower_control_block_count(
    const loom_low_lower_context_t* context) {
  const loom_low_lower_control_plan_t* plan = context->lowering->control_plan;
  return loom_func_like_body(context->source_function)->block_count +
         (plan ? plan->block_count : 0);
}

static loom_low_lower_block_ref_t loom_low_lower_control_signature(
    const loom_low_lower_context_t* context, loom_low_lower_block_ref_t block) {
  const uint16_t source_count =
      loom_func_like_body(context->source_function)->block_count;
  return block > source_count
             ? context->lowering->control_plan->blocks[block - source_count - 1]
                   .signature
             : block;
}

uint16_t loom_low_lower_control_argument_count(
    const loom_low_lower_context_t* context, loom_low_lower_block_ref_t block) {
  const loom_low_lower_block_ref_t signature =
      loom_low_lower_control_signature(context, block);
  if (signature == 0) {
    return 0;
  }
  const uint16_t index = (uint16_t)(signature - 1);
  const loom_block_t* source =
      loom_region_block(loom_func_like_body(context->source_function), index);
  const uint16_t authored_count =
      index == 0 ? context->lowering->boundary.argument_count
                 : source->arg_count;
  return authored_count +
         loom_low_lower_realization_block_argument_count(context, source);
}

loom_type_t loom_low_lower_control_argument_type(
    const loom_low_lower_context_t* context, loom_low_lower_block_ref_t block,
    uint16_t argument_index) {
  const uint16_t index =
      (uint16_t)(loom_low_lower_control_signature(context, block) - 1);
  const loom_block_t* source =
      loom_region_block(loom_func_like_body(context->source_function), index);
  const uint16_t authored_count =
      index == 0 ? context->lowering->boundary.argument_count
                 : source->arg_count;
  if (argument_index >= authored_count) {
    return loom_low_lower_realization_block_argument_type(
        context, source, argument_index - authored_count);
  }
  return index == 0 ? context->lowering->boundary.argument_types[argument_index]
                    : loom_low_lower_structural_block_argument_type(
                          context, index, argument_index);
}

iree_status_t loom_low_lower_control_add_block(
    loom_low_lower_context_t* context, loom_low_lower_block_ref_t before,
    loom_low_lower_block_ref_t signature,
    loom_low_lower_block_ref_t* out_block) {
  loom_low_lower_control_plan_t* plan = context->lowering->control_plan;
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      context->function_arena, plan->block_count, plan->block_count + 1,
      sizeof(*plan->blocks), &plan->block_capacity, (void**)&plan->blocks));
  plan->blocks[plan->block_count++] = (loom_low_lower_control_block_t){
      .before = before,
      .signature = loom_low_lower_control_signature(context, signature),
  };
  *out_block =
      (loom_low_lower_block_ref_t)loom_low_lower_control_block_count(context);
  return iree_ok_status();
}

loom_low_lower_block_ref_t loom_low_lower_control_successor(
    const loom_low_lower_context_t* context, const loom_op_t* source_terminator,
    uint8_t successor_index) {
  const loom_low_lower_control_plan_t* plan = context->lowering->control_plan;
  if (plan) {
    const loom_low_lower_control_source_t* source =
        &plan->sources[source_terminator->parent_block->region_index];
    if (source->successors) {
      return source->successors[successor_index];
    }
  }
  return loom_low_lower_control_source_block(
      loom_op_const_successors(source_terminator)[successor_index]);
}

iree_status_t loom_low_lower_control_interpose_successor(
    loom_low_lower_context_t* context, const loom_op_t* source_terminator,
    uint8_t successor_index, loom_low_lower_block_ref_t block,
    loom_low_lower_block_ref_t* out_previous) {
  loom_low_lower_control_source_t* source =
      &context->lowering->control_plan
           ->sources[source_terminator->parent_block->region_index];
  if (source->successors == NULL) {
    IREE_RETURN_IF_ERROR(loom_low_lower_allocate_function_array(
        context, source_terminator->successor_count,
        sizeof(*source->successors), (void**)&source->successors));
    for (uint8_t i = 0; i < source_terminator->successor_count; ++i) {
      source->successors[i] = loom_low_lower_control_source_block(
          loom_op_const_successors(source_terminator)[i]);
    }
  }
  *out_previous = source->successors[successor_index];
  IREE_ASSERT_EQ(loom_low_lower_control_argument_count(context, *out_previous),
                 loom_low_lower_control_argument_count(context, block));
  source->successors[successor_index] = block;
  return iree_ok_status();
}

void loom_low_lower_set_branch_plan(loom_low_lower_context_t* context,
                                    const loom_op_t* source_terminator,
                                    loom_low_lower_plan_t plan) {
  loom_low_lower_plan_t* destination =
      &context->lowering->control_plan
           ->sources[source_terminator->parent_block->region_index]
           .branch;
  IREE_ASSERT(loom_low_lower_plan_is_empty(*destination));
  *destination = plan;
}

bool loom_low_lower_lookup_branch_plan(loom_low_lower_context_t* context,
                                       const loom_op_t* source_terminator,
                                       loom_low_lower_plan_t* out_plan) {
  const loom_low_lower_control_plan_t* plan = context->lowering->control_plan;
  *out_plan =
      plan ? plan->sources[source_terminator->parent_block->region_index].branch
           : loom_low_lower_plan_empty();
  return !loom_low_lower_plan_is_empty(*out_plan);
}

static iree_status_t loom_low_lower_control_plan_initialize(
    loom_low_lower_context_t* context, uint16_t source_count) {
  loom_low_lower_control_plan_t* plan = NULL;
  IREE_RETURN_IF_ERROR(
      loom_low_lower_allocate_plan_data(context, sizeof(*plan), (void**)&plan));
  *plan = (loom_low_lower_control_plan_t){0};
  IREE_RETURN_IF_ERROR(loom_low_lower_allocate_function_array(
      context, source_count, sizeof(*plan->sources), (void**)&plan->sources));
  for (uint16_t i = 0; i < source_count; ++i) {
    plan->sources[i] = (loom_low_lower_control_source_t){
        .branch = loom_low_lower_plan_empty(),
    };
  }
  context->lowering->control_plan = plan;
  return iree_ok_status();
}

static iree_status_t loom_low_lower_control_plan_operands(
    loom_low_lower_context_t* context, const loom_op_t* terminator) {
  const bool is_branch = loom_cfg_br_isa(terminator);
  if (!is_branch &&
      !loom_low_lower_source_op_is_callable_exit(context, terminator)) {
    return iree_ok_status();
  }
  const loom_low_lower_block_ref_t destination =
      is_branch ? loom_low_lower_control_successor(context, terminator, 0) : 0;
  const loom_value_id_t* operands = loom_op_const_operands(terminator);
  iree_status_t status = iree_ok_status();
  for (uint16_t i = 0;
       i < terminator->operand_count && iree_status_is_ok(status) &&
       !loom_low_lower_context_diagnostic_limit_reached(context);
       ++i) {
    const loom_type_t required_type =
        is_branch
            ? loom_low_lower_control_argument_type(context, destination, i)
            : context->lowering->boundary.result_types[i];
    if (loom_type_equal(loom_low_lower_value_binding_type(context, operands[i]),
                        required_type)) {
      continue;
    }
    const void* recipe = NULL;
    status = context->policy->control_operand.prepare(
        context->policy->control_operand.user_data, context, terminator,
        operands[i], required_type, &recipe);
    if (!iree_status_is_ok(status) ||
        loom_low_lower_context_diagnostic_limit_reached(context) ||
        recipe == NULL) {
      continue;
    }
    if (context->lowering->control_plan == NULL) {
      status = loom_low_lower_control_plan_initialize(
          context, loom_func_like_body(context->source_function)->block_count);
    }
    if (iree_status_is_ok(status)) {
      loom_low_lower_control_source_t* source =
          &context->lowering->control_plan
               ->sources[terminator->parent_block->region_index];
      if (source->operands == NULL) {
        status = loom_low_lower_allocate_function_array(
            context, terminator->operand_count, sizeof(*source->operands),
            (void**)&source->operands);
        if (iree_status_is_ok(status)) {
          memset(source->operands, 0,
                 terminator->operand_count * sizeof(*source->operands));
        }
      }
      if (iree_status_is_ok(status)) {
        source->operands[i] = recipe;
      }
    }
  }
  return status;
}

iree_status_t loom_low_lower_control_plan_build(
    loom_low_lower_context_t* context) {
  if (context->policy->prepare_branch.fn == NULL &&
      context->policy->control_operand.prepare == NULL) {
    return iree_ok_status();
  }
  loom_region_t* body = loom_func_like_body(context->source_function);
  iree_arena_allocator_t scratch_arena;
  iree_arena_initialize(context->module->arena.block_pool, &scratch_arena);
  iree_status_t status = iree_ok_status();
  for (uint16_t i = 0;
       i < body->block_count && iree_status_is_ok(status) &&
       !loom_low_lower_context_diagnostic_limit_reached(context);
       ++i) {
    const loom_op_t* terminator =
        loom_block_const_last_op(loom_region_block(body, i));
    if (terminator == NULL) {
      continue;
    }
    if (terminator->successor_count != 0 &&
        context->policy->prepare_branch.fn != NULL) {
      if (context->lowering->control_plan == NULL) {
        status =
            loom_low_lower_control_plan_initialize(context, body->block_count);
      }
      if (iree_status_is_ok(status)) {
        status = context->policy->prepare_branch.fn(
            context->policy->prepare_branch.user_data, context, terminator,
            &scratch_arena);
      }
    }
    if (iree_status_is_ok(status) &&
        !loom_low_lower_context_diagnostic_limit_reached(context) &&
        context->policy->control_operand.prepare != NULL) {
      status = loom_low_lower_control_plan_operands(context, terminator);
    }
    iree_arena_reset(&scratch_arena);
    if (iree_status_is_ok(status) &&
        loom_low_lower_control_block_count(context) > UINT16_MAX) {
      status = loom_low_lower_emit_branch_constraint(
          context, terminator, IREE_SV("block_count_u16"));
    }
  }
  iree_arena_deinitialize(&scratch_arena);
  return status;
}

iree_status_t loom_low_lower_control_materialize_operand(
    loom_low_lower_context_t* context, const loom_op_t* source_terminator,
    uint16_t operand_index, loom_type_t required_type,
    loom_value_id_t* inout_low_value) {
  if (loom_type_equal(loom_module_value_type(context->module, *inout_low_value),
                      required_type)) {
    return iree_ok_status();
  }
  const loom_low_lower_control_plan_t* plan = context->lowering->control_plan;
  const void* const* operands =
      plan ? plan->sources[source_terminator->parent_block->region_index]
                 .operands
           : NULL;
  IREE_RETURN_IF_ERROR(context->policy->control_operand.emit(
      context->policy->control_operand.user_data, context, source_terminator,
      loom_op_const_operands(source_terminator)[operand_index],
      *inout_low_value, required_type,
      operands ? operands[operand_index] : NULL, inout_low_value));
  IREE_ASSERT(
      loom_type_equal(loom_module_value_type(context->module, *inout_low_value),
                      required_type));
  return iree_ok_status();
}

loom_block_t* loom_low_lower_control_block(
    const loom_low_lower_context_t* context, loom_low_lower_block_ref_t block) {
  return block ? context->block_map[block - 1] : NULL;
}

iree_status_t loom_low_lower_control_create_blocks(
    loom_low_lower_context_t* context) {
  const loom_low_lower_control_plan_t* plan = context->lowering->control_plan;
  if (plan == NULL) {
    return iree_ok_status();
  }
  const uint16_t source_count =
      loom_func_like_body(context->source_function)->block_count;
  loom_region_t* body = loom_func_like_body(
      loom_func_like_cast(context->module, context->low_func_op));
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < plan->block_count && iree_status_is_ok(status); ++i) {
    const loom_low_lower_control_block_t* recipe = &plan->blocks[i];
    const loom_block_t* before =
        loom_low_lower_control_block(context, recipe->before);
    loom_block_t* block = NULL;
    status = loom_region_insert_block(
        context->module, body,
        before ? before->region_index : body->block_count, &block);
    if (!iree_status_is_ok(status)) {
      break;
    }
    context->block_map[source_count + i] = block;
    const loom_block_t* signature =
        loom_low_lower_control_block(context, recipe->signature);
    const uint16_t argument_count = signature ? signature->arg_count : 0;
    for (uint16_t j = 0; j < argument_count && iree_status_is_ok(status); ++j) {
      const loom_value_id_t original = loom_block_arg_id(signature, j);
      loom_value_id_t argument = LOOM_VALUE_ID_INVALID;
      status = loom_builder_define_block_arg(
          &context->builder, block,
          loom_module_value_type(context->module, original), &argument);
      if (iree_status_is_ok(status)) {
        status =
            loom_module_copy_value_name(context->module, original, argument);
      }
    }
  }
  return status;
}
