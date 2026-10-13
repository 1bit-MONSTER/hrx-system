// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/loop_execution.h"

#include <string.h>

#include "loom/ir/ir.h"

typedef struct loom_low_loop_execution_binding_t {
  // Next sparse binding in insertion order.
  struct loom_low_loop_execution_binding_t* next;
  // Exact Low control operation emitted for a source loop header.
  const loom_op_t* control_op;
  // Producer-proven execution count for the represented loop.
  loom_low_loop_execution_facts_t facts;
} loom_low_loop_execution_binding_t;

static_assert(sizeof(loom_low_loop_execution_binding_t) <= 32,
              "Low loop execution bindings must remain compact");

struct loom_low_loop_execution_map_t {
  // Owner of map and binding storage.
  iree_arena_allocator_t* arena;
  // First sparse binding, or NULL for an empty map.
  loom_low_loop_execution_binding_t* head;
  // Last sparse binding, or NULL for an empty map.
  loom_low_loop_execution_binding_t* tail;
  // Number of retained bindings.
  uint32_t count;
};

static_assert(sizeof(loom_low_loop_execution_map_t) <= 32,
              "Low loop execution maps must remain compact");

iree_status_t loom_low_loop_execution_map_allocate(
    iree_arena_allocator_t* arena, loom_low_loop_execution_map_t** out_map) {
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, sizeof(**out_map), (void**)out_map));
  **out_map = (loom_low_loop_execution_map_t){.arena = arena};
  return iree_ok_status();
}

iree_status_t loom_low_loop_execution_map_insert(
    loom_low_loop_execution_map_t* map, const loom_op_t* control_op,
    loom_low_loop_execution_facts_t facts) {
  IREE_ASSERT(control_op != NULL);
  IREE_ASSERT(facts.count_kind == LOOM_LOW_LOOP_EXECUTION_COUNT_BOUNDED ||
              facts.count_kind == LOOM_LOW_LOOP_EXECUTION_COUNT_EXACT);
  IREE_ASSERT_LT(map->count, UINT32_MAX);
  loom_low_loop_execution_binding_t* binding = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(map->arena, sizeof(*binding), (void**)&binding));
  *binding = (loom_low_loop_execution_binding_t){
      .control_op = control_op,
      .facts = facts,
  };
  if (map->tail != NULL) {
    map->tail->next = binding;
  } else {
    map->head = binding;
  }
  map->tail = binding;
  ++map->count;
  return iree_ok_status();
}

iree_status_t loom_low_loop_execution_map_transfer(
    const loom_low_loop_execution_map_t* source,
    loom_low_loop_execution_map_t* target) {
  if (source == target) {
    return iree_ok_status();
  }
  for (const loom_low_loop_execution_binding_t* binding = source->head;
       binding != NULL; binding = binding->next) {
    IREE_RETURN_IF_ERROR(loom_low_loop_execution_map_insert(
        target, binding->control_op, binding->facts));
  }
  return iree_ok_status();
}

iree_status_t loom_low_loop_execution_index_build(
    const loom_low_loop_execution_map_t* map, const loom_region_t* body,
    iree_arena_allocator_t* arena, loom_low_loop_execution_index_t* out_index) {
  *out_index = (loom_low_loop_execution_index_t){0};
  if (map == NULL || map->count == 0 || body == NULL ||
      body->block_count == 0) {
    return iree_ok_status();
  }
  const loom_low_loop_execution_facts_t** facts_by_block = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, body->block_count,
                                                 sizeof(*facts_by_block),
                                                 (void**)&facts_by_block));
  memset(facts_by_block, 0,
         (iree_host_size_t)body->block_count * sizeof(*facts_by_block));
  for (const loom_low_loop_execution_binding_t* binding = map->head;
       binding != NULL; binding = binding->next) {
    const loom_op_t* control_op = binding->control_op;
    if (iree_any_bit_set(control_op->flags, LOOM_OP_FLAG_DEAD) ||
        control_op->parent_block == NULL ||
        control_op->parent_block->parent_region != body ||
        control_op->parent_block->last_op != control_op) {
      continue;
    }
    const uint16_t block_index = control_op->parent_block->region_index;
    IREE_ASSERT_LT(block_index, body->block_count);
    IREE_ASSERT(facts_by_block[block_index] == NULL);
    facts_by_block[block_index] = &binding->facts;
  }
  *out_index = (loom_low_loop_execution_index_t){
      .facts_by_block = facts_by_block,
      .block_count = body->block_count,
  };
  return iree_ok_status();
}

const loom_low_loop_execution_facts_t* loom_low_loop_execution_index_lookup(
    const loom_low_loop_execution_index_t* index, const loom_block_t* header) {
  if (index->facts_by_block == NULL || header == NULL ||
      header->region_index >= index->block_count || header->last_op == NULL) {
    return NULL;
  }
  const loom_low_loop_execution_facts_t* facts =
      index->facts_by_block[header->region_index];
  return facts;
}

struct loom_low_loop_execution_clone_t {
  // Map receiving cloned control bindings.
  loom_low_loop_execution_map_t* target;
  // Source bindings indexed by current source block order.
  const loom_low_loop_execution_binding_t* const* bindings_by_block;
  // Number of entries in |bindings_by_block|.
  iree_host_size_t block_count;
};

iree_status_t loom_low_loop_execution_clone_allocate(
    const loom_low_loop_execution_map_t* source,
    loom_low_loop_execution_map_t* target,
    iree_arena_allocator_t* scratch_arena,
    loom_low_loop_execution_clone_t** out_clone) {
  IREE_RETURN_IF_ERROR(iree_arena_allocate(scratch_arena, sizeof(**out_clone),
                                           (void**)out_clone));
  **out_clone = (loom_low_loop_execution_clone_t){.target = target};
  if (source == NULL || source->count == 0) {
    return iree_ok_status();
  }

  const loom_region_t* body = NULL;
  for (const loom_low_loop_execution_binding_t* binding = source->head;
       binding != NULL && body == NULL; binding = binding->next) {
    if (!iree_any_bit_set(binding->control_op->flags, LOOM_OP_FLAG_DEAD) &&
        binding->control_op->parent_block != NULL) {
      body = binding->control_op->parent_block->parent_region;
    }
  }
  if (body == NULL || body->block_count == 0) {
    return iree_ok_status();
  }

  const loom_low_loop_execution_binding_t** bindings_by_block = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, body->block_count, sizeof(*bindings_by_block),
      (void**)&bindings_by_block));
  memset(bindings_by_block, 0,
         (iree_host_size_t)body->block_count * sizeof(*bindings_by_block));
  for (const loom_low_loop_execution_binding_t* binding = source->head;
       binding != NULL; binding = binding->next) {
    const loom_op_t* control_op = binding->control_op;
    if (iree_any_bit_set(control_op->flags, LOOM_OP_FLAG_DEAD) ||
        control_op->parent_block == NULL ||
        control_op->parent_block->parent_region != body) {
      continue;
    }
    const uint16_t block_index = control_op->parent_block->region_index;
    IREE_ASSERT_LT(block_index, body->block_count);
    IREE_ASSERT(bindings_by_block[block_index] == NULL);
    bindings_by_block[block_index] = binding;
  }
  (*out_clone)->bindings_by_block = bindings_by_block;
  (*out_clone)->block_count = body->block_count;
  return iree_ok_status();
}

iree_status_t loom_low_loop_execution_clone_op(void* context,
                                               const loom_op_t* source_op,
                                               loom_op_t* target_op) {
  loom_low_loop_execution_clone_t* clone = context;
  if (clone->bindings_by_block == NULL || source_op->parent_block == NULL ||
      source_op->parent_block->region_index >= clone->block_count) {
    return iree_ok_status();
  }
  const loom_low_loop_execution_binding_t* binding =
      clone->bindings_by_block[source_op->parent_block->region_index];
  return binding != NULL && binding->control_op == source_op
             ? loom_low_loop_execution_map_insert(clone->target, target_op,
                                                  binding->facts)
             : iree_ok_status();
}
