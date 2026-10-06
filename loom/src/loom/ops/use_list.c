// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/use_list.h"

#include <string.h>

#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/op_defs.h"

// Initial overflow capacity when transitioning from inline to overflow.
// 8 covers the common case of values used 4-8 times without further
// reallocation. Values used more than 8 times get geometric growth.
#define LOOM_USE_INITIAL_OVERFLOW_CAPACITY 8

static iree_status_t loom_value_add_use(loom_module_t* module,
                                        loom_value_id_t value_id,
                                        loom_op_t* user_op,
                                        uint16_t operand_index,
                                        loom_use_flags_t flags) {
  loom_value_t* value = loom_module_value(module, value_id);
  if (value->use_count >= LOOM_VALUE_MAX_USE_COUNT) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED, "value %%%u has too many uses (%u max)",
        (unsigned)value_id, (unsigned)LOOM_VALUE_MAX_USE_COUNT);
  }
  loom_use_t use = loom_use_make(user_op, operand_index, flags);
  loom_use_index_t* operand_use_indices = loom_op_operand_use_indices(user_op);
  loom_use_index_t use_index = value->use_count;

  if (!loom_value_has_overflow_uses(value)) {
    if (value->use_count < LOOM_VALUE_INLINE_USE_COUNT) {
      // Common path: store inline.
      value->inline_uses[use_index] = use;
      operand_use_indices[operand_index] = use_index;
      ++value->use_count;
      return iree_ok_status();
    }
    // Transition from inline to overflow: allocate array, copy inline
    // uses, then add the new use.
    uint32_t capacity = LOOM_USE_INITIAL_OVERFLOW_CAPACITY;
    loom_use_t* overflow = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        &module->arena, capacity, sizeof(loom_use_t), (void**)&overflow));
    for (uint16_t i = 0; i < LOOM_VALUE_INLINE_USE_COUNT; ++i) {
      overflow[i] = value->inline_uses[i];
    }
    const uint32_t ownership_use_count =
        loom_value_ownership_use_count(value) + (flags != 0);
    overflow[use_index] = use;
    value->overflow_uses = overflow;
    value->overflow_capacity = capacity;
    value->overflow_ownership_use_count = ownership_use_count;
    value->flags |= LOOM_VALUE_FLAG_OVERFLOW_USES;
    operand_use_indices[operand_index] = use_index;
    ++value->use_count;
    return iree_ok_status();
  }

  // Already in overflow mode.
  if (value->use_count < value->overflow_capacity) {
    // Space available: append.
    value->overflow_uses[use_index] = use;
    value->overflow_ownership_use_count += flags != 0;
    operand_use_indices[operand_index] = use_index;
    ++value->use_count;
    return iree_ok_status();
  }

  // Overflow array is full: grow by 2x (floor to initial capacity as
  // a safety net against zero-capacity invariant violations).
  uint32_t new_capacity =
      iree_max(value->overflow_capacity, LOOM_USE_INITIAL_OVERFLOW_CAPACITY);
  if (new_capacity > LOOM_VALUE_MAX_USE_COUNT / 2) {
    new_capacity = LOOM_VALUE_MAX_USE_COUNT;
  } else {
    new_capacity *= 2;
  }
  loom_use_t* new_overflow = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      &module->arena, new_capacity, sizeof(loom_use_t), (void**)&new_overflow));
  memcpy(new_overflow, value->overflow_uses,
         (iree_host_size_t)value->use_count * sizeof(loom_use_t));
  new_overflow[use_index] = use;
  value->overflow_uses = new_overflow;
  value->overflow_capacity = new_capacity;
  value->overflow_ownership_use_count += flags != 0;
  operand_use_indices[operand_index] = use_index;
  ++value->use_count;
  return iree_ok_status();
}

static void loom_op_mark_operand_ownership(loom_module_t* module,
                                           const loom_op_t* op,
                                           uint16_t operand_index,
                                           loom_use_flags_t flags) {
  // Construction also accepts unverified parsed operations. Invalid references
  // remain available to the verifier, but cannot index the use table here.
  if (operand_index >= op->operand_count) {
    return;
  }
  const loom_value_id_t value_id = loom_op_const_operands(op)[operand_index];
  if (value_id == LOOM_VALUE_ID_INVALID) {
    return;
  }
  loom_value_t* value = loom_module_value(module, value_id);
  const loom_use_index_t use_index =
      loom_op_operand_use_indices(op)[operand_index];
  loom_use_t* use = &loom_value_uses_mutable(value)[use_index];
  const loom_use_flags_t old_flags = loom_use_flags(*use);
  *use = loom_use_with_flags(*use, old_flags | flags);
  if (loom_value_has_overflow_uses(value) && old_flags == 0) {
    ++value->overflow_ownership_use_count;
  }
}

// Ownership classification is produced once from the complete operation, not
// rediscovered by each user query.
static void loom_op_record_operand_ownership(loom_module_t* module,
                                             const loom_op_t* op) {
  if (op->operand_count == 0) {
    return;
  }
  const loom_tied_result_t* ties = loom_op_tied_results(op);
  for (uint16_t i = 0; i < op->tied_result_count; ++i) {
    if (ties[i].result_index < op->result_count) {
      loom_op_mark_operand_ownership(module, op, ties[i].operand_index,
                                     LOOM_USE_FLAG_CONSUMES);
    }
  }
  const loom_op_vtable_t* vtable = loom_op_vtable(module, op);
  if (!vtable) {
    return;
  }
  // Source fact transport does not constrain storage ownership. Physical
  // identity requires the same storage-relation contract consumed by placement.
  if (loom_traits_have_storage_relation(vtable->traits)) {
    if (loom_traits_are_fact_identity(vtable->traits)) {
      const uint16_t count = iree_min(op->operand_count, op->result_count);
      for (uint16_t i = 0; i < count; ++i) {
        loom_op_mark_operand_ownership(module, op, i, LOOM_USE_FLAG_IDENTITY);
      }
    } else if (loom_traits_are_value_alias(vtable->traits) &&
               op->result_count) {
      loom_op_mark_operand_ownership(module, op, 0, LOOM_USE_FLAG_IDENTITY);
    }
  }
  if (vtable->result_descriptors) {
    const uint16_t count =
        iree_min(op->result_count,
                 vtable->fixed_result_count +
                     iree_any_bit_set(vtable->vtable_flags,
                                      LOOM_OP_VTABLE_VARIADIC_RESULTS));
    for (uint16_t i = 0; i < count; ++i) {
      const loom_result_descriptor_t* descriptor =
          &vtable->result_descriptors[i];
      loom_use_flags_t flags = 0;
      switch (descriptor->ownership_effect) {
        case LOOM_RESULT_OWNERSHIP_TIED:
        case LOOM_RESULT_OWNERSHIP_MOVED:
          flags = LOOM_USE_FLAG_CONSUMES;
          break;
        case LOOM_RESULT_OWNERSHIP_ALIAS:
        case LOOM_RESULT_OWNERSHIP_BORROWED:
          flags = LOOM_USE_FLAG_IDENTITY;
          break;
        default:
          break;
      }
      if (flags && descriptor->ownership_source_operand_index !=
                       LOOM_RESULT_OWNERSHIP_SOURCE_FIELD_NONE) {
        const loom_value_slice_t source = loom_op_operand_field_span(
            vtable, op, descriptor->ownership_source_operand_index);
        for (uint16_t j = 0; j < source.count; ++j) {
          const uint16_t index =
              (uint16_t)(source.values - loom_op_const_operands(op)) + j;
          loom_op_mark_operand_ownership(module, op, index, flags);
        }
      }
    }
  }
  if (!vtable->operand_descriptors) {
    return;
  }
  const uint8_t field_count = loom_op_vtable_operand_descriptor_count(vtable);
  const uint16_t* segment_counts =
      loom_op_vtable_has_segmented_operands(vtable)
          ? loom_op_const_operand_segment_counts(op)
          : NULL;
  uint16_t start = 0;
  for (uint8_t field = 0; field < field_count; ++field) {
    const loom_operand_descriptor_t* descriptor =
        &vtable->operand_descriptors[field];
    const uint16_t remaining = op->operand_count - start;
    const uint16_t count =
        segment_counts ? iree_min(segment_counts[field], remaining)
        : iree_any_bit_set(descriptor->flags, LOOM_OPERAND_VARIADIC)
            ? remaining
            : iree_min(1, remaining);
    switch (descriptor->ownership_effect) {
      case LOOM_OPERAND_OWNERSHIP_CONSUME:
      case LOOM_OPERAND_OWNERSHIP_RELEASE:
      case LOOM_OPERAND_OWNERSHIP_DISCARD:
      case LOOM_OPERAND_OWNERSHIP_ESCAPE: {
        for (uint16_t i = 0; i < count; ++i) {
          loom_op_mark_operand_ownership(module, op, start + i,
                                         LOOM_USE_FLAG_CONSUMES);
        }
        break;
      }
      default:
        break;
    }
    start += count;
  }
}

void loom_op_refresh_operand_ownership(loom_module_t* module,
                                       const loom_op_t* op) {
  const loom_value_id_t* operands = loom_op_const_operands(op);
  const loom_use_index_t* indices = loom_op_operand_use_indices(op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    if (operands[i] == LOOM_VALUE_ID_INVALID) {
      continue;
    }
    loom_value_t* value = loom_module_value(module, operands[i]);
    loom_use_t* use = &loom_value_uses_mutable(value)[indices[i]];
    if (loom_value_has_overflow_uses(value)) {
      value->overflow_ownership_use_count -= loom_use_flags(*use) != 0;
    }
    *use = loom_use_with_flags(*use, 0);
  }
  loom_op_record_operand_ownership(module, op);
}

iree_status_t loom_op_record_operand_uses(loom_module_t* module,
                                          loom_op_t* op) {
  const loom_value_id_t* operands = loom_op_const_operands(op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    if (operands[i] != LOOM_VALUE_ID_INVALID) {
      IREE_RETURN_IF_ERROR(loom_value_add_use(module, operands[i], op, i, 0));
    }
  }
  loom_op_record_operand_ownership(module, op);
  return iree_ok_status();
}

iree_status_t loom_value_remove_use(loom_module_t* module,
                                    loom_value_id_t value_id,
                                    loom_op_t* user_op,
                                    uint16_t operand_index) {
  loom_value_t* value = loom_module_value(module, value_id);
  loom_use_index_t* operand_use_indices = loom_op_operand_use_indices(user_op);
  loom_use_index_t use_index = operand_use_indices[operand_index];
  loom_use_t* uses = loom_value_uses_mutable(value);
  if (use_index < value->use_count &&
      loom_use_user_op(uses[use_index]) == user_op &&
      loom_use_operand_index(uses[use_index]) == operand_index) {
    if (loom_value_has_overflow_uses(value)) {
      value->overflow_ownership_use_count -=
          loom_use_flags(uses[use_index]) != 0;
    }
    // Swap with last and decrement. Update the moved user's backpointer so
    // future removals stay O(1).
    loom_use_index_t last_index = value->use_count - 1;
    if (use_index != last_index) {
      loom_use_t moved_use = uses[last_index];
      uses[use_index] = moved_use;
      loom_op_t* moved_user_op = loom_use_user_op(moved_use);
      uint16_t moved_operand_index = loom_use_operand_index(moved_use);
      loom_op_operand_use_indices(moved_user_op)[moved_operand_index] =
          use_index;
    }
    operand_use_indices[operand_index] = LOOM_USE_INDEX_INVALID;
    --value->use_count;
    return iree_ok_status();
  }
  iree_string_view_t op_name = loom_op_name(module, user_op);
  return iree_make_status(IREE_STATUS_NOT_FOUND,
                          "no matching use of value %%%u by %.*s operand %u",
                          (unsigned)value_id, (int)op_name.size, op_name.data,
                          (unsigned)operand_index);
}

iree_status_t loom_op_set_operand(loom_module_t* module, loom_op_t* op,
                                  uint16_t operand_index,
                                  loom_value_id_t new_value_id) {
  loom_value_id_t* operands = loom_op_operands(op);
  loom_value_id_t old_value_id = operands[operand_index];
  if (old_value_id == new_value_id) {
    return iree_ok_status();
  }
  loom_use_flags_t flags = 0;
  if (old_value_id != LOOM_VALUE_ID_INVALID) {
    const loom_value_t* old_value = loom_module_value(module, old_value_id);
    flags = loom_use_flags(loom_value_uses(
        old_value)[loom_op_operand_use_indices(op)[operand_index]]);
    IREE_RETURN_IF_ERROR(
        loom_value_remove_use(module, old_value_id, op, operand_index));
  }
  operands[operand_index] = new_value_id;
  if (new_value_id != LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_value_add_use(module, new_value_id, op, operand_index, flags));
    if (old_value_id == LOOM_VALUE_ID_INVALID) {
      loom_op_record_operand_ownership(module, op);
    }
  }
  return iree_ok_status();
}

// Ensures a value's use list has capacity for at least |additional| more
// entries. Used by RAUW to pre-allocate before bulk transfer.
static iree_status_t loom_value_ensure_use_capacity(loom_module_t* module,
                                                    loom_value_t* value,
                                                    uint32_t additional) {
  if (additional > LOOM_VALUE_MAX_USE_COUNT - value->use_count) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "value use count exceeds maximum (%u)",
                            (unsigned)LOOM_VALUE_MAX_USE_COUNT);
  }
  uint32_t needed = value->use_count + additional;
  if (!loom_value_has_overflow_uses(value)) {
    if (needed <= LOOM_VALUE_INLINE_USE_COUNT) {
      return iree_ok_status();
    }
    // Transition to overflow with enough capacity.
    uint32_t capacity = LOOM_USE_INITIAL_OVERFLOW_CAPACITY;
    while (capacity < needed) {
      capacity *= 2;
    }
    loom_use_t* overflow = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        &module->arena, capacity, sizeof(loom_use_t), (void**)&overflow));
    for (uint32_t i = 0; i < value->use_count; ++i) {
      overflow[i] = value->inline_uses[i];
    }
    const uint32_t ownership_use_count = loom_value_ownership_use_count(value);
    value->overflow_uses = overflow;
    value->overflow_capacity = capacity;
    value->overflow_ownership_use_count = ownership_use_count;
    value->flags |= LOOM_VALUE_FLAG_OVERFLOW_USES;
    return iree_ok_status();
  }
  if (needed <= value->overflow_capacity) {
    return iree_ok_status();
  }
  // Grow overflow (floor to initial capacity for safety).
  uint32_t new_capacity =
      iree_max(value->overflow_capacity, LOOM_USE_INITIAL_OVERFLOW_CAPACITY);
  while (new_capacity < needed) {
    new_capacity *= 2;
  }
  loom_use_t* new_overflow = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      &module->arena, new_capacity, sizeof(loom_use_t), (void**)&new_overflow));
  memcpy(new_overflow, value->overflow_uses,
         (iree_host_size_t)value->use_count * sizeof(loom_use_t));
  value->overflow_uses = new_overflow;
  value->overflow_capacity = new_capacity;
  return iree_ok_status();
}

iree_status_t loom_value_replace_all_uses_with(loom_module_t* module,
                                               loom_value_id_t old_id,
                                               loom_value_id_t new_id) {
  if (old_id == new_id) {
    return iree_ok_status();
  }
  loom_value_t* old_value = loom_module_value(module, old_id);
  uint32_t old_use_count = old_value->use_count;

  loom_value_t* new_value = loom_module_value(module, new_id);
  IREE_RETURN_IF_ERROR(
      loom_value_ensure_use_capacity(module, new_value, old_use_count));

  if (loom_module_value_has_type_uses(module, old_id) ||
      loom_value_has_attribute_uses(old_value)) {
    loom_value_replacement_t replacement;
    loom_value_replacement_initialize(module, old_id, new_id, &replacement);
    iree_status_t status = loom_value_replacement_apply_types(&replacement);
    while (loom_value_has_attribute_uses(old_value) &&
           iree_status_is_ok(status)) {
      // Mutation invalidates the cursor. Successful replacement removes this
      // owner's old membership, so restarting selects an unprocessed owner.
      loom_type_use_iterator_t users;
      loom_attribute_users_begin(&module->type_uses, old_id, &users);
      const loom_attribute_user_t use = loom_attribute_users_next(&users);
      status = loom_value_replacement_apply_attribute(&replacement, use.op,
                                                      use.attribute_index);
      if (iree_status_is_ok(status)) {
        loom_trait_flags_t old_traits = use.op->traits;
        loom_op_refresh_effective_traits(module, use.op);
        loom_module_update_op_direct_summaries(module, use.op, old_traits,
                                               use.op->traits);
      }
    }
    loom_value_replacement_deinitialize(&replacement);
    IREE_RETURN_IF_ERROR(status);
  }
  if (old_use_count == 0) {
    return iree_ok_status();
  }

  // Patch every user op's operand slot.
  const loom_use_t* old_uses = loom_value_uses(old_value);
  for (uint32_t i = 0; i < old_use_count; ++i) {
    loom_op_t* user_op = loom_use_user_op(old_uses[i]);
    uint16_t operand_index = loom_use_operand_index(old_uses[i]);
    loom_op_operands(user_op)[operand_index] = new_id;
  }

  // Bulk-transfer use entries from old to new.
  // Append old's entries to new's list. The old_uses pointer (captured
  // above) is still valid: ensure_use_capacity only touches new_value,
  // and old_id != new_id is guarded at entry.
  loom_use_t* new_uses = loom_value_uses_mutable(new_value);
  uint32_t new_use_start = new_value->use_count;
  for (uint32_t i = 0; i < old_use_count; ++i) {
    uint32_t new_use_index = new_use_start + i;
    new_uses[new_use_index] = old_uses[i];
    loom_op_t* user_op = loom_use_user_op(old_uses[i]);
    uint16_t operand_index = loom_use_operand_index(old_uses[i]);
    loom_op_operand_use_indices(user_op)[operand_index] = new_use_index;
  }
  if (loom_value_has_overflow_uses(new_value)) {
    new_value->overflow_ownership_use_count +=
        loom_value_ownership_use_count(old_value);
  }
  new_value->use_count += old_use_count;

  // Clear old value's use list.
  old_value->use_count = 0;
  old_value->flags &= ~LOOM_VALUE_FLAG_OVERFLOW_USES;
  return iree_ok_status();
}

iree_status_t loom_value_replace_all_uses_except(loom_module_t* module,
                                                 loom_value_id_t old_id,
                                                 loom_value_id_t new_id,
                                                 const loom_op_t* except_op) {
  if (old_id == new_id) {
    return iree_ok_status();
  }
  loom_value_t* old_value = loom_module_value(module, old_id);
  if (old_value->use_count == 0) {
    return iree_ok_status();
  }

  // Count how many uses will be transferred vs kept.
  const loom_use_t* old_uses = loom_value_uses(old_value);
  uint32_t transfer_count = 0;
  for (uint32_t i = 0; i < old_value->use_count; ++i) {
    if (loom_use_user_op(old_uses[i]) != except_op) {
      ++transfer_count;
    }
  }
  if (transfer_count == 0) {
    return iree_ok_status();
  }

  // Ensure new has capacity.
  loom_value_t* new_value = loom_module_value(module, new_id);
  IREE_RETURN_IF_ERROR(
      loom_value_ensure_use_capacity(module, new_value, transfer_count));

  // Patch operand slots and transfer use entries.
  // Walk old's list backwards so swap-removal doesn't skip entries.
  loom_use_t* old_uses_mutable = loom_value_uses_mutable(old_value);
  loom_use_t* new_uses = loom_value_uses_mutable(new_value);
  for (uint32_t i = old_value->use_count; i-- > 0;) {
    if (loom_use_user_op(old_uses_mutable[i]) == except_op) {
      continue;
    }
    // Patch the operand slot.
    loom_op_t* user_op = loom_use_user_op(old_uses_mutable[i]);
    uint16_t operand_index = loom_use_operand_index(old_uses_mutable[i]);
    loom_op_operands(user_op)[operand_index] = new_id;
    // Add to new's list.
    if (loom_use_flags(old_uses_mutable[i])) {
      if (loom_value_has_overflow_uses(new_value)) {
        ++new_value->overflow_ownership_use_count;
      }
      if (loom_value_has_overflow_uses(old_value)) {
        --old_value->overflow_ownership_use_count;
      }
    }
    uint32_t new_use_index = new_value->use_count;
    new_uses[new_use_index] = old_uses_mutable[i];
    loom_op_operand_use_indices(user_op)[operand_index] = new_use_index;
    ++new_value->use_count;
    // Remove from old's list (swap with last).
    uint32_t last_index = old_value->use_count - 1;
    if (i != last_index) {
      loom_use_t moved_use = old_uses_mutable[last_index];
      old_uses_mutable[i] = moved_use;
      loom_op_t* moved_user_op = loom_use_user_op(moved_use);
      uint16_t moved_operand_index = loom_use_operand_index(moved_use);
      loom_op_operand_use_indices(moved_user_op)[moved_operand_index] = i;
    }
    --old_value->use_count;
  }

  // If old is now empty and was overflow, clear the flag.
  if (old_value->use_count == 0) {
    old_value->flags &= ~LOOM_VALUE_FLAG_OVERFLOW_USES;
  }
  return iree_ok_status();
}

iree_status_t loom_value_replace_uses_if(loom_module_t* module,
                                         loom_value_id_t old_id,
                                         loom_value_id_t new_id,
                                         loom_use_predicate_fn predicate,
                                         void* user_data) {
  if (old_id == new_id) {
    return iree_ok_status();
  }
  loom_value_t* old_value = loom_module_value(module, old_id);
  if (old_value->use_count == 0) {
    return iree_ok_status();
  }

  // Count how many uses will be transferred.
  const loom_use_t* old_uses = loom_value_uses(old_value);
  uint32_t transfer_count = 0;
  for (uint32_t i = 0; i < old_value->use_count; ++i) {
    if (predicate(loom_use_user_op(old_uses[i]), user_data)) {
      ++transfer_count;
    }
  }
  if (transfer_count == 0) {
    return iree_ok_status();
  }

  // Ensure new has capacity.
  loom_value_t* new_value = loom_module_value(module, new_id);
  IREE_RETURN_IF_ERROR(
      loom_value_ensure_use_capacity(module, new_value, transfer_count));

  // Patch and transfer (walk backwards for safe swap-removal).
  loom_use_t* old_uses_mutable = loom_value_uses_mutable(old_value);
  loom_use_t* new_uses = loom_value_uses_mutable(new_value);
  for (uint32_t i = old_value->use_count; i-- > 0;) {
    if (!predicate(loom_use_user_op(old_uses_mutable[i]), user_data)) {
      continue;
    }
    loom_op_t* user_op = loom_use_user_op(old_uses_mutable[i]);
    uint16_t operand_index = loom_use_operand_index(old_uses_mutable[i]);
    loom_op_operands(user_op)[operand_index] = new_id;
    if (loom_use_flags(old_uses_mutable[i])) {
      if (loom_value_has_overflow_uses(new_value)) {
        ++new_value->overflow_ownership_use_count;
      }
      if (loom_value_has_overflow_uses(old_value)) {
        --old_value->overflow_ownership_use_count;
      }
    }
    uint32_t new_use_index = new_value->use_count;
    new_uses[new_use_index] = old_uses_mutable[i];
    loom_op_operand_use_indices(user_op)[operand_index] = new_use_index;
    ++new_value->use_count;
    uint32_t last_index = old_value->use_count - 1;
    if (i != last_index) {
      loom_use_t moved_use = old_uses_mutable[last_index];
      old_uses_mutable[i] = moved_use;
      loom_op_t* moved_user_op = loom_use_user_op(moved_use);
      uint16_t moved_operand_index = loom_use_operand_index(moved_use);
      loom_op_operand_use_indices(moved_user_op)[moved_operand_index] = i;
    }
    --old_value->use_count;
  }
  if (old_value->use_count == 0) {
    old_value->flags &= ~LOOM_VALUE_FLAG_OVERFLOW_USES;
  }
  return iree_ok_status();
}

// Clears cached semantic summaries before rebuilding use/def state.
static void loom_region_reset_summaries(loom_region_t* region) {
  if (!region) {
    return;
  }
  region->read_effect_count = 0;
  region->write_effect_count = 0;
  region->convergent_effect_count = 0;
  region->observable_effect_count = 0;
  region->memory_access_count = 0;
  region->hint_source_count = 0;
  loom_block_t* block = NULL;
  loom_region_for_each_block(region, block) {
    block->parent_region = region;
    loom_op_t* op = NULL;
    loom_block_for_each_op(block, op) {
      op->flags &= ~LOOM_OP_FLAG_SUMMARIES_COUNTED;
      loom_region_t** regions = loom_op_regions(op);
      for (uint8_t i = 0; i < op->region_count; ++i) {
        loom_region_reset_summaries(regions[i]);
      }
    }
  }
}

// Walks all blocks in a region recursively, adding uses, setting def pointers,
// setting parent pointers, and recording direct semantic summaries for each
// op.
static iree_status_t loom_region_compute_uses(loom_module_t* module,
                                              loom_region_t* region,
                                              loom_op_t* parent_op) {
  if (!region) {
    return iree_ok_status();
  }
  loom_block_t* block = NULL;
  loom_region_for_each_block(region, block) {
    block->parent_region = region;
    // Set def pointers for block arguments.
    for (uint16_t a = 0; a < block->arg_count; ++a) {
      loom_value_id_t arg_id = loom_block_arg_id(block, a);
      if (arg_id != LOOM_VALUE_ID_INVALID) {
        loom_module_value(module, arg_id)->def =
            loom_value_def_make_block(block, a);
      }
    }
    loom_op_t* op = NULL;
    loom_block_for_each_op(block, op) {
      // Set parent pointers.
      op->parent_op = parent_op;
      op->parent_block = block;
      loom_module_record_op_summaries(module, op);
      IREE_RETURN_IF_ERROR(loom_module_refresh_op_attribute_uses(module, op));
      IREE_RETURN_IF_ERROR(loom_op_record_operand_uses(module, op));
      // Set def pointers on result values.
      loom_value_id_t* results = loom_op_results(op);
      for (uint16_t i = 0; i < op->result_count; ++i) {
        if (results[i] != LOOM_VALUE_ID_INVALID) {
          loom_module_value(module, results[i])->def =
              loom_value_def_make_op(op, i);
        }
      }
      // Refresh module-scope symbol links. Parsers and builders install
      // nested symbol links at their construction boundary.
      if (!parent_op) {
        const loom_op_vtable_t* vtable = loom_op_vtable(module, op);
        if (vtable &&
            iree_any_bit_set(vtable->traits, LOOM_TRAIT_SYMBOL_DEFINE)) {
          loom_module_link_symbol_defining_op(module, op, vtable);
        }
      }
      // Recurse into nested regions.
      for (uint8_t r = 0; r < op->region_count; ++r) {
        loom_region_t* nested = loom_op_regions(op)[r];
        IREE_RETURN_IF_ERROR(loom_region_compute_uses(module, nested, op));
      }
    }
  }
  return iree_ok_status();
}

iree_status_t loom_module_compute_uses(loom_module_t* module) {
  loom_module_reset_attribute_uses(module);
  loom_region_reset_summaries(module->body);
  module->poison_op_count = 0;
  // Clear all use data and live definition data on every value. Definition
  // pointers to erased operations retain immutable producer semantics.
  for (iree_host_size_t i = 0; i < module->values.count; ++i) {
    loom_value_t* value = loom_module_value(module, (loom_value_id_t)i);
    value->use_count = 0;
    value->flags &=
        ~(LOOM_VALUE_FLAG_OVERFLOW_USES | LOOM_VALUE_FLAG_ATTRIBUTE_USES);
    if (loom_value_is_block_arg(value)) {
      value->def = loom_value_def_make_none();
    } else {
      loom_op_t* defining_op = loom_value_def_op(value);
      if (!defining_op ||
          !iree_any_bit_set(defining_op->flags, LOOM_OP_FLAG_DEAD)) {
        value->def = loom_value_def_make_none();
      }
    }
    memset(value->inline_uses, 0,
           LOOM_VALUE_INLINE_USE_COUNT * sizeof(loom_use_t));
  }
  // Walk all ops and re-add uses, def pointers, and parent pointers.
  IREE_RETURN_IF_ERROR(loom_region_compute_uses(module, module->body, NULL));
  return loom_module_recompute_type_uses(module);
}
