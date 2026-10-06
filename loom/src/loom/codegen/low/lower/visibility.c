// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/visibility.h"

#include "loom/codegen/low/lower/context.h"
#include "loom/codegen/low/lower/execution.h"
#include "loom/ops/atomic.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/callable_effects.h"
#include "loom/ops/kernel/ops.h"
#include "loom/ops/op_defs.h"

struct loom_low_lower_visibility_access_t {
  // Next observation in the shared walk, or NULL at the end.
  struct loom_low_lower_visibility_access_t* next;
  // Direct source-body block containing this operation.
  uint16_t block_index;
  // Bounded byte extent of a payload view, or zero for an acquisition point.
  uint64_t byte_extent;
};

static iree_status_t loom_low_lower_visibility_record(
    loom_low_lower_context_t* context,
    loom_low_lower_visibility_builder_t* builder, const loom_op_t* op,
    uint64_t byte_extent) {
  loom_low_lower_visibility_access_t* access = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(&context->planning_arena,
                                           sizeof(*access), (void**)&access));
  *access = (loom_low_lower_visibility_access_t){
      .block_index = loom_block_region_index(op->parent_block),
      .byte_extent = byte_extent,
  };
  if (builder->last_access) {
    builder->last_access->next = access;
  } else {
    builder->first_access = access;
  }
  builder->last_access = access;
  return iree_ok_status();
}

static iree_status_t loom_low_lower_visibility_acquire(
    loom_low_lower_context_t* context,
    loom_low_lower_visibility_builder_t* builder, const loom_op_t* op,
    uint8_t ordering, uint8_t scope) {
  if (ordering == LOOM_ATOMIC_ORDERING_SEQ_CST) {
    builder->requires_eager = true;
  }
  if ((ordering != LOOM_ATOMIC_ORDERING_ACQUIRE &&
       ordering != LOOM_ATOMIC_ORDERING_ACQ_REL) ||
      scope == LOOM_ATOMIC_SCOPE_THREAD) {
    return iree_ok_status();
  }
  builder->acquire_scope = iree_max(builder->acquire_scope, scope);
  return loom_low_lower_visibility_record(context, builder, op, 0);
}

iree_status_t loom_low_lower_visibility_observe(
    loom_low_lower_context_t* context,
    loom_low_lower_visibility_builder_t* builder, const loom_op_t* op) {
  if (!builder->model.invocation_count || builder->requires_eager) {
    return iree_ok_status();
  }
  if (op->parent_block->parent_region != builder->body) {
    builder->requires_eager = true;
    return iree_ok_status();
  }
  if (loom_buffer_fence_isa(op)) {
    return loom_low_lower_visibility_acquire(context, builder, op,
                                             loom_buffer_fence_ordering(op),
                                             loom_buffer_fence_scope(op));
  }
  if (loom_kernel_barrier_isa(op)) {
    if (loom_kernel_barrier_memory_space(op) ==
        LOOM_VALUE_FACT_MEMORY_SPACE_GLOBAL) {
      return loom_low_lower_visibility_acquire(context, builder, op,
                                               loom_kernel_barrier_ordering(op),
                                               loom_kernel_barrier_scope(op));
    }
    return iree_ok_status();
  }
  if (iree_any_bit_set(op->traits, LOOM_TRAIT_CALLABLE_BOUNDARY)) {
    const loom_call_like_t call =
        loom_call_like_cast(context->module, (loom_op_t*)op);
    if (loom_call_like_kind(call) == LOOM_CALL_LIKE_KIND_LOW_INVOKE) {
      const loom_symbol_ref_t callee = loom_call_like_callee(call);
      builder->requires_eager =
          callee.module_id != 0 ||
          loom_callable_effects_may_access_memory(loom_func_like_cast(
              context->module,
              context->module->symbols.entries[callee.symbol_id].defining_op));
      return iree_ok_status();
    }
  }
  const loom_memory_access_t access =
      loom_memory_access_cast(context->module, op);
  if (!loom_memory_access_isa(access)) {
    if (iree_any_bit_set(loom_op_effective_traits(context->module, op),
                         LOOM_TRAIT_UNKNOWN_EFFECTS | LOOM_TRAIT_READS_MEMORY |
                             LOOM_TRAIT_MEMORY_FENCE)) {
      builder->requires_eager = true;
    }
    return iree_ok_status();
  }
  const loom_memory_access_operation_kind_t kind =
      loom_memory_access_operation_kind(access);
  if (kind == LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_LOAD) {
    return loom_low_lower_visibility_acquire(
        context, builder, op,
        loom_attr_as_enum(loom_memory_access_atomic_ordering(access)),
        loom_attr_as_enum(loom_memory_access_atomic_scope(access)));
  }
  if (kind == LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_STORE) {
    builder->requires_eager |=
        loom_attr_as_enum(loom_memory_access_atomic_ordering(access)) ==
        LOOM_ATOMIC_ORDERING_SEQ_CST;
    return iree_ok_status();
  }
  if (loom_memory_access_operation_kind_is_atomic(kind)) {
    const uint8_t ordering =
        loom_attr_as_enum(loom_memory_access_atomic_ordering(access));
    builder->requires_eager |= ordering != LOOM_ATOMIC_ORDERING_RELAXED &&
                               ordering != LOOM_ATOMIC_ORDERING_RELEASE;
    return iree_ok_status();
  }
  if (kind != LOOM_MEMORY_ACCESS_OPERATION_LOAD) {
    return iree_ok_status();
  }

  const loom_value_id_t view = loom_memory_access_view(access);
  loom_value_fact_view_reference_t reference = {0};
  if (!loom_value_facts_query_view_reference(
          &context->lowering.fact_table->context,
          loom_value_fact_table_lookup(context->lowering.fact_table, view),
          &reference)) {
    builder->requires_eager = true;
    return iree_ok_status();
  }
  if (reference.memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_CONSTANT ||
      reference.memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_PRIVATE ||
      reference.memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP) {
    return iree_ok_status();
  }
  const loom_type_t type =
      loom_module_value_type(context->module, loom_op_const_results(op)[0]);
  if ((reference.memory_space != LOOM_VALUE_FACT_MEMORY_SPACE_GLOBAL &&
       reference.memory_space != LOOM_VALUE_FACT_MEMORY_SPACE_GENERIC) ||
      !loom_type_is_scalar(type) ||
      loom_scalar_type_bitwidth(loom_type_element_type(type)) != 32) {
    builder->requires_eager = true;
    return iree_ok_status();
  }
  int64_t byte_extent = 0;
  if (!loom_value_facts_as_exact_i64(reference.footprint_byte_length,
                                     &byte_extent) ||
      byte_extent <= 0 ||
      (uint64_t)byte_extent > builder->model.reuse_byte_limit) {
    // Without a bounded working set there is no retained reuse evidence.
    return iree_ok_status();
  }
  return loom_low_lower_visibility_record(context, builder, op, byte_extent);
}

iree_status_t loom_low_lower_visibility_select(
    loom_low_lower_context_t* context,
    const loom_low_lower_visibility_builder_t* builder,
    uint8_t* out_read_visibility_scope) {
  *out_read_visibility_scope = LOOM_ATOMIC_SCOPE_THREAD;
  if (!builder->model.invocation_count || builder->requires_eager ||
      builder->acquire_scope == LOOM_ATOMIC_SCOPE_THREAD) {
    return iree_ok_status();
  }
  const uint64_t* counts = NULL;
  IREE_RETURN_IF_ERROR(
      loom_low_lower_source_block_execution_counts(context, &counts));
  if (counts) {
    // Compare estimated read traffic with the complete bounded view envelope.
    // Counts are exact; locality is a cost estimate, never a legality proof.
    double acquisitions = 0;
    for (const loom_low_lower_visibility_access_t* access =
             builder->first_access;
         access; access = access->next) {
      if (!access->byte_extent) {
        acquisitions += (double)counts[access->block_index];
      }
    }
    if (acquisitions > 0) {
      for (const loom_low_lower_visibility_access_t* access =
               builder->first_access;
           access; access = access->next) {
        if (!access->byte_extent) {
          continue;
        }
        const double read_bytes = (double)counts[access->block_index] * 4.0 *
                                  builder->model.invocation_count;
        const double reuse_bytes =
            acquisitions * access->byte_extent * builder->model.reuse_count;
        if (read_bytes >= reuse_bytes) {
          return iree_ok_status();
        }
      }
    }
  }
  *out_read_visibility_scope = builder->acquire_scope;
  return iree_ok_status();
}
