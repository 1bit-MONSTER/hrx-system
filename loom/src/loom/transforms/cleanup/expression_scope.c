// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/cleanup/expression_scope.h"

#include <string.h>

#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ir/structural_hash.h"
#include "loom/ops/op_defs.h"
#include "loom/util/dominance.h"

//===----------------------------------------------------------------------===//
// Expression identity
//===----------------------------------------------------------------------===//

// Computes a content-aware hash for an op based on its kind, operands,
// result types, attributes, and instance flags. Uses loom_attribute_hash
// for each attribute so pointer-valued attribute kinds (I64_ARRAY,
// PREDICATE_LIST, DICT) are hashed by content rather than pointer value.
// Result types are included because they are not always derivable from
// (kind, operands, attributes) — cast and conversion ops can produce
// different result types from the same operands.
uint32_t loom_expression_hash(const loom_module_t* module,
                              const loom_op_t* op) {
  uint32_t hash = loom_structural_hash_initialize();
  hash = loom_structural_hash_mix_u16(hash, op->kind);
  hash = loom_structural_hash_mix_u16(hash, op->operand_count);
  const loom_value_id_t* operands = loom_op_operands((loom_op_t*)op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    hash = loom_structural_hash_mix_u32(hash, operands[i]);
  }
  const loom_op_vtable_t* vtable = loom_op_vtable(module, op);
  uint8_t operand_segment_count = loom_op_vtable_operand_segment_count(vtable);
  hash = loom_structural_hash_mix_u8(hash, operand_segment_count);
  if (operand_segment_count > 0) {
    const uint16_t* segment_counts = loom_op_const_operand_segment_counts(op);
    for (uint8_t i = 0; i < operand_segment_count; ++i) {
      hash = loom_structural_hash_mix_u16(hash, segment_counts[i]);
    }
  }
  const loom_value_id_t* results = loom_op_results((loom_op_t*)op);
  hash = loom_structural_hash_mix_u16(hash, op->result_count);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    if (results[i] != LOOM_VALUE_ID_INVALID) {
      loom_type_t type = loom_module_value_type(module, results[i]);
      hash = loom_structural_hash_mix_u32(hash, loom_type_hash(type));
    }
  }
  hash = loom_structural_hash_mix_u8(hash, op->attribute_count);
  if (op->attribute_count > 0) {
    const loom_attribute_t* attrs = loom_op_attrs((loom_op_t*)op);
    for (uint8_t i = 0; i < op->attribute_count; ++i) {
      uint32_t attribute_hash = loom_attribute_hash(&attrs[i]);
      hash = loom_structural_hash_mix_u32(hash, attribute_hash);
    }
  }
  hash = loom_structural_hash_mix_u8(hash, op->instance_flags);
  return loom_structural_hash_finalize(hash);
}

// Structural equality: two ops are CSE-equivalent if they have the
// same kind, operands (same value IDs in same order), result types
// (checked via the module's value table), attributes (structurally
// equal via loom_attribute_equal), instance flags, and no regions.
bool loom_expression_equal(const loom_module_t* module, const loom_op_t* a,
                           const loom_op_t* b) {
  if (a->kind != b->kind) {
    return false;
  }
  if (a->operand_count != b->operand_count) {
    return false;
  }
  if (a->result_count != b->result_count) {
    return false;
  }
  if (a->attribute_count != b->attribute_count) {
    return false;
  }
  if (a->instance_flags != b->instance_flags) {
    return false;
  }
  if (a->region_count > 0) {
    return false;
  }
  const loom_value_id_t* a_operands = loom_op_operands((loom_op_t*)a);
  const loom_value_id_t* b_operands = loom_op_operands((loom_op_t*)b);
  if (memcmp(a_operands, b_operands,
             (iree_host_size_t)a->operand_count * sizeof(loom_value_id_t)) !=
      0) {
    return false;
  }
  const loom_op_vtable_t* vtable = loom_op_vtable(module, a);
  uint8_t operand_segment_count = loom_op_vtable_operand_segment_count(vtable);
  if (operand_segment_count > 0 &&
      memcmp(loom_op_const_operand_segment_counts(a),
             loom_op_const_operand_segment_counts(b),
             (iree_host_size_t)operand_segment_count * sizeof(uint16_t)) != 0) {
    return false;
  }
  const loom_value_id_t* a_results = loom_op_results((loom_op_t*)a);
  const loom_value_id_t* b_results = loom_op_results((loom_op_t*)b);
  for (uint16_t i = 0; i < a->result_count; ++i) {
    if (a_results[i] != LOOM_VALUE_ID_INVALID &&
        b_results[i] != LOOM_VALUE_ID_INVALID) {
      loom_type_t a_type = loom_module_value_type(module, a_results[i]);
      loom_type_t b_type = loom_module_value_type(module, b_results[i]);
      if (!loom_type_equal(a_type, b_type)) {
        return false;
      }
    }
  }
  if (a->attribute_count > 0) {
    const loom_attribute_t* a_attrs = loom_op_attrs((loom_op_t*)a);
    const loom_attribute_t* b_attrs = loom_op_attrs((loom_op_t*)b);
    for (uint8_t i = 0; i < a->attribute_count; ++i) {
      if (!loom_attribute_equal(&a_attrs[i], &b_attrs[i])) {
        return false;
      }
    }
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Dominance scopes
//===----------------------------------------------------------------------===//

enum loom_expression_scope_flag_bits_e {
  // Incoming control flow can observe a different mutable state.
  LOOM_EXPRESSION_SCOPE_FLAG_STATE_BARRIER = 1u << 0,
  // The enclosing operation hides values from outside this region.
  LOOM_EXPRESSION_SCOPE_FLAG_ISOLATED = 1u << 1,
};
typedef uint8_t loom_expression_scope_flags_t;

struct loom_expression_scope_t {
  // Dominating block or enclosing block, including across isolation boundaries.
  loom_expression_scope_t* parent;
  // First index entry owned by this block; also its rollback checkpoint.
  uint32_t entry_start;
  // Earliest visible entry after the nearest isolation boundary.
  uint32_t visible_start;
  // Earliest visible entry after isolation or a mutable-state CFG boundary.
  uint32_t stateful_start;
  // Scope availability and traversal state.
  loom_expression_scope_flags_t flags;
};

// Entries double as the rollback log. Leaving a scope unlinks each insertion
// once and reuses its storage; there is no separate undo allocation.
typedef struct loom_expression_binding_t {
  // Available producer and caller metadata.
  loom_expression_entry_t entry;
  // Previous visible binding for this key, or UINT32_MAX for a newly added key.
  uint32_t previous;
  // Linear-probe distance from entry.hash to the slot restored on scope exit.
  // At most one slot per definition is occupied, bounding this by the u32
  // definition count even when the half-full index has more than 2^32 slots.
  uint32_t probe_distance;
} loom_expression_binding_t;

struct loom_expression_walk_t {
  // Module containing all visited operations.
  loom_module_t* module;
  // Shared dominator-order operation and scope traversal.
  loom_dominance_walk_t* dominance_walk;
  // Innermost active dominance scope, including completed dominating blocks.
  loom_expression_scope_t* active_scope;
  // Fixed inventory of client scopes, one for every block in the region tree.
  loom_expression_scope_t* scopes;
  // Number of scopes entered so far.
  iree_host_size_t scope_count;
  // Fixed-capacity entries shared by all dominance scopes in this walk.
  loom_expression_binding_t* bindings;
  // Latest binding index per structural key, or UINT32_MAX for an empty slot.
  uint32_t* bucket_heads;
  // Power-of-two slot count minus one; the table stays at most half full.
  iree_host_size_t bucket_mask;
  // Number of active bindings; scopes retain checkpoints into this array.
  uint32_t binding_count;
  // Definition frontier: counts value-producing ops, not result-less barriers.
  // Each counted op owns a distinct value-table entry, so the u32 value-ID
  // domain bounds this counter even when arbitrarily many writes intervene.
  uint32_t epoch;
};

static iree_host_size_t loom_expression_scope_slot(
    const loom_expression_cursor_t* cursor, uint32_t hash) {
  const loom_expression_walk_t* walk = cursor->walk;
  iree_host_size_t slot = hash & walk->bucket_mask;
  while (walk->bucket_heads[slot] != UINT32_MAX) {
    const loom_expression_entry_t* entry =
        &walk->bindings[walk->bucket_heads[slot]].entry;
    if (entry->hash == hash &&
        loom_expression_equal(cursor->module, entry->op, cursor->op)) {
      break;
    }
    slot = (slot + 1) & walk->bucket_mask;
  }
  return slot;
}

loom_expression_entry_t* loom_expression_scope_find(
    const loom_expression_cursor_t* cursor, uint32_t hash,
    loom_expression_lookup_flags_t flags) {
  loom_expression_walk_t* walk = cursor->walk;
  const loom_expression_scope_t* scope = cursor->scope;
  const uint32_t first =
      iree_any_bit_set(flags, LOOM_EXPRESSION_LOOKUP_FLAG_STATEFUL)
          ? scope->stateful_start
          : scope->visible_start;
  const uint32_t index =
      walk->bucket_heads[loom_expression_scope_slot(cursor, hash)];
  return index != UINT32_MAX && index >= first ? &walk->bindings[index].entry
                                               : NULL;
}

void loom_expression_scope_insert(const loom_expression_cursor_t* cursor,
                                  loom_expression_entry_t entry) {
  loom_expression_walk_t* walk = cursor->walk;
  const iree_host_size_t slot = loom_expression_scope_slot(cursor, entry.hash);
  const uint32_t previous = walk->bucket_heads[slot];
  if (previous != UINT32_MAX && previous >= cursor->scope->entry_start) {
    walk->bindings[previous].entry = entry;
    return;
  }
  const uint32_t index = walk->binding_count++;
  walk->bindings[index] = (loom_expression_binding_t){
      .entry = entry,
      .previous = previous,
      .probe_distance = (uint32_t)((slot - entry.hash) & walk->bucket_mask),
  };
  walk->bucket_heads[slot] = index;
}

// Establishes one rollback checkpoint when the shared dominance walk enters a
// block. Visibility cutoffs make isolation and mutable-state barriers constant
// time without changing structural-expression lookup policy.
static iree_status_t loom_expression_walk_enter_scope(
    void* user_data, loom_dominance_walk_scope_flags_t dominance_flags) {
  loom_expression_walk_t* walk = (loom_expression_walk_t*)user_data;
  loom_expression_scope_t* scope = &walk->scopes[walk->scope_count++];
  loom_expression_scope_flags_t flags = 0;
  if (iree_any_bit_set(dominance_flags,
                       LOOM_DOMINANCE_WALK_SCOPE_FLAG_STATE_BARRIER)) {
    flags |= LOOM_EXPRESSION_SCOPE_FLAG_STATE_BARRIER;
  }
  if (iree_any_bit_set(dominance_flags,
                       LOOM_DOMINANCE_WALK_SCOPE_FLAG_ISOLATED)) {
    flags |= LOOM_EXPRESSION_SCOPE_FLAG_ISOLATED;
  }
  loom_expression_scope_t* parent = walk->active_scope;
  *scope = (loom_expression_scope_t){
      .parent = parent,
      .entry_start = walk->binding_count,
      .visible_start =
          !parent ||
                  iree_any_bit_set(flags, LOOM_EXPRESSION_SCOPE_FLAG_ISOLATED)
              ? walk->binding_count
              : parent->visible_start,
      .stateful_start =
          !parent || iree_any_bit_set(
                         flags, LOOM_EXPRESSION_SCOPE_FLAG_ISOLATED |
                                    LOOM_EXPRESSION_SCOPE_FLAG_STATE_BARRIER)
              ? walk->binding_count
              : parent->stateful_start,
      .flags = flags,
  };
  walk->active_scope = scope;
  return iree_ok_status();
}

// Restores every binding introduced in the active scope. Strict LIFO removal
// preserves open-addressing probe chains without tombstones: keys displaced by
// a new key are removed before that key's slot is cleared.
static void loom_expression_walk_leave_scope(void* user_data) {
  loom_expression_walk_t* walk = (loom_expression_walk_t*)user_data;
  const uint32_t checkpoint = walk->active_scope->entry_start;
  while (walk->binding_count > checkpoint) {
    const loom_expression_binding_t* binding =
        &walk->bindings[--walk->binding_count];
    const iree_host_size_t slot =
        ((iree_host_size_t)binding->entry.hash + binding->probe_distance) &
        walk->bucket_mask;
    walk->bucket_heads[slot] = binding->previous;
  }
  walk->active_scope = walk->active_scope->parent;
}

static loom_op_t* loom_expression_first_region_op(const loom_region_t* region,
                                                  uint16_t block_start) {
  if (!region) {
    return NULL;
  }
  for (uint16_t i = block_start; i < region->block_count; ++i) {
    loom_op_t* op = loom_region_const_block(region, i)->first_op;
    if (op) {
      return op;
    }
  }
  return NULL;
}

static loom_op_t* loom_expression_first_nested_op(const loom_op_t* op,
                                                  uint8_t region_start) {
  for (uint8_t i = region_start; i < op->region_count; ++i) {
    loom_op_t* nested =
        loom_expression_first_region_op(loom_op_regions(op)[i], 0);
    if (nested) {
      return nested;
    }
  }
  return NULL;
}

// Counts the index's maximum insertions before any rewriting. Parent pointers
// provide the return path without recursive calls or an allocated stack. Each
// operation and block is visited once; finding the next sibling region scans
// only its owner's bounded region operands, never an enclosing subtree.
typedef struct loom_expression_inventory_t {
  // Maximum number of structural expression bindings.
  uint32_t definition_count;
  // Number of dominance scopes entered by the shared traversal.
  iree_host_size_t scope_count;
} loom_expression_inventory_t;

static loom_expression_inventory_t loom_expression_inventory(
    const loom_region_t* root) {
  loom_expression_inventory_t inventory = {
      .scope_count = root->block_count,
  };
  loom_op_t* op = loom_expression_first_region_op(root, 0);
  while (op) {
    inventory.definition_count += op->result_count != 0;
    for (uint8_t i = 0; i < op->region_count; ++i) {
      const loom_region_t* region = loom_op_regions(op)[i];
      inventory.scope_count += region ? region->block_count : 0;
    }
    loom_op_t* nested = loom_expression_first_nested_op(op, 0);
    if (nested) {
      op = nested;
      continue;
    }
    while (op) {
      if (op->next_op) {
        op = op->next_op;
        break;
      }
      const loom_region_t* region = op->parent_block->parent_region;
      loom_op_t* next = loom_expression_first_region_op(
          region, op->parent_block->region_index + 1);
      if (next) {
        op = next;
        break;
      }
      if (region == root) {
        return inventory;
      }
      const loom_op_t* parent = op->parent_op;
      for (uint8_t i = 0; i < parent->region_count; ++i) {
        if (loom_op_regions(parent)[i] == region) {
          next = loom_expression_first_nested_op(parent, i + 1);
          break;
        }
      }
      op = next ? next : (loom_op_t*)parent;
      if (next) {
        break;
      }
    }
  }
  return inventory;
}

iree_status_t loom_expression_walk_create(loom_module_t* module,
                                          loom_region_t* region,
                                          iree_arena_allocator_t* arena,
                                          loom_expression_walk_t** out_walk) {
  *out_walk = NULL;
  loom_expression_walk_t* walk = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, sizeof(*walk), (void**)&walk));
  *walk = (loom_expression_walk_t){.module = module};
  const loom_expression_inventory_t inventory =
      loom_expression_inventory(region);
  const uint32_t definition_count = inventory.definition_count;
  const iree_host_size_t bucket_count = iree_host_size_next_power_of_two(
      iree_max((iree_host_size_t)definition_count * 2, 1));
  walk->bucket_mask = bucket_count - 1;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, definition_count,
                                                 sizeof(*walk->bindings),
                                                 (void**)&walk->bindings));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, bucket_count,
                                                 sizeof(*walk->bucket_heads),
                                                 (void**)&walk->bucket_heads));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, inventory.scope_count,
                                                 sizeof(*walk->scopes),
                                                 (void**)&walk->scopes));
  memset(walk->bucket_heads, 0xFF, bucket_count * sizeof(*walk->bucket_heads));
  IREE_RETURN_IF_ERROR(loom_dominance_walk_create(
      module, region,
      (loom_dominance_walk_callbacks_t){
          .user_data = walk,
          .enter_scope = loom_expression_walk_enter_scope,
          .leave_scope = loom_expression_walk_leave_scope,
      },
      arena, &walk->dominance_walk));
  *out_walk = walk;
  return iree_ok_status();
}

iree_status_t loom_expression_walk_next(loom_expression_walk_t* walk,
                                        loom_expression_cursor_t* out_cursor) {
  *out_cursor = (loom_expression_cursor_t){0};
  loom_dominance_walk_cursor_t dominance_cursor;
  IREE_RETURN_IF_ERROR(
      loom_dominance_walk_next(walk->dominance_walk, &dominance_cursor));
  if (!dominance_cursor.op) {
    return iree_ok_status();
  }
  *out_cursor = (loom_expression_cursor_t){
      .walk = walk,
      .module = walk->module,
      .scope = walk->active_scope,
      .op = dominance_cursor.op,
      .traits = dominance_cursor.traits,
      .epoch = walk->epoch,
  };
  walk->epoch += dominance_cursor.op->result_count != 0;
  return iree_ok_status();
}

uint32_t loom_expression_observe_barriers(
    const loom_expression_cursor_t* cursor,
    loom_expression_barriers_t* barriers) {
  if (loom_traits_are_convergent(cursor->traits) ||
      loom_op_regions_have_convergent_effects(cursor->op)) {
    barriers->convergence = cursor->epoch;
  }
  if (loom_traits_may_write(cursor->traits) ||
      loom_op_regions_have_write_effects(cursor->op)) {
    barriers->memory = cursor->epoch;
  }
  return iree_any_bit_set(cursor->traits, LOOM_TRAIT_PURE)
             ? barriers->convergence
             : iree_max(barriers->convergence, barriers->memory);
}

//===----------------------------------------------------------------------===//
// Semantic eligibility and replacement
//===----------------------------------------------------------------------===//

static bool loom_expression_op_transfers_operand_ownership(
    const loom_module_t* module, const loom_op_t* op) {
  const loom_value_id_t* operands = loom_op_const_operands(op);
  const loom_use_index_t* indices = loom_op_operand_use_indices(op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    const loom_value_t* value = loom_module_value(module, operands[i]);
    const loom_use_t use = loom_value_uses(value)[indices[i]];
    if (iree_any_bit_set(loom_use_flags(use), LOOM_USE_FLAG_CONSUMES)) {
      return true;
    }
  }
  return false;
}

static bool loom_expression_result_has_sensitive_ownership(
    const loom_module_t* module, const loom_op_t* op) {
  const loom_value_id_t* results = loom_op_results((loom_op_t*)op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    const loom_value_id_t result = results[i];
    if (result == LOOM_VALUE_ID_INVALID) {
      continue;
    }
    const loom_value_t* value = loom_module_value(module, result);
    if (loom_value_ownership_use_count(value) != 0) {
      return true;
    }
  }
  return false;
}

bool loom_expression_can_share_result_ownership(const loom_module_t* module,
                                                const loom_op_t* op) {
  return !loom_expression_op_transfers_operand_ownership(module, op) &&
         !loom_expression_result_has_sensitive_ownership(module, op);
}

bool loom_expression_is_reusable(const loom_expression_cursor_t* cursor) {
  return cursor->op->result_count != 0 && cursor->op->region_count == 0 &&
         !loom_traits_has_side_effects(cursor->traits) &&
         !loom_traits_has_unique_identity(cursor->traits) &&
         loom_expression_can_share_result_ownership(cursor->module, cursor->op);
}

iree_status_t loom_expression_replace(loom_module_t* module, loom_op_t* op,
                                      loom_op_t* existing) {
  loom_value_id_t* results = loom_op_results(op);
  const loom_value_id_t* existing_results = loom_op_results(existing);
  iree_status_t status = iree_ok_status();
  for (uint16_t i = 0; i < op->result_count && iree_status_is_ok(status); ++i) {
    if (results[i] != LOOM_VALUE_ID_INVALID &&
        existing_results[i] != LOOM_VALUE_ID_INVALID) {
      status = loom_value_replace_all_uses_with(module, results[i],
                                                existing_results[i]);
    }
  }
  if (iree_status_is_ok(status)) {
    status = loom_op_erase(module, op);
  }
  return status;
}
