// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/text/printer/name_plan.h"

#include <inttypes.h>
#include <string.h>

#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/op_defs.h"

// Stack buffer size for formatting generated value-name suffixes.
#define LOOM_PRINT_NAME_SUFFIX_BUFFER_SIZE 32

struct loom_print_name_resolution_t {
  // Zero preserves the explicit name or bare numeric ID. One appends $ID;
  // larger selectors append $(suffix - 1)$ID to the explicit name, if any.
  uint32_t suffix;
};

static_assert(sizeof(loom_print_name_resolution_t) == 4,
              "name resolutions must remain compact");

typedef struct loom_print_name_index_entry_t {
  // Parser scope containing the explicit name.
  const void* scope;
  // Interned name ID plus one; zero marks an empty table entry.
  uint32_t name_key;
  // True when multiple printable values have this name in the same scope.
  bool duplicated;
} loom_print_name_index_entry_t;

static const loom_op_vtable_t* loom_print_name_defining_op_vtable(
    const loom_module_t* module, const loom_op_t* op) {
  if (!module || !module->context || !op) {
    return NULL;
  }
  return loom_context_resolve_op(module->context, op->kind);
}

static bool loom_print_name_has_local_signature_results(
    const loom_op_vtable_t* vtable) {
  return vtable &&
         (iree_any_bit_set(vtable->traits, LOOM_TRAIT_SYMBOL_DEFINE) ||
          loom_op_vtable_has_signature_only_results(vtable));
}

// Returns the parser scope that receives |value_id|'s printed definition.
static const void* loom_print_name_parse_scope(const loom_module_t* module,
                                               loom_value_id_t value_id) {
  if (!module || value_id >= module->values.count) {
    return NULL;
  }
  const loom_value_t* value = loom_module_value(module, value_id);
  if (loom_value_is_block_arg(value)) {
    loom_block_t* block = loom_value_def_block(value);
    return block ? (const void*)block->parent_region : NULL;
  }

  loom_op_t* def_op = loom_value_def_op(value);
  if (def_op) {
    const loom_op_vtable_t* vtable =
        loom_print_name_defining_op_vtable(module, def_op);
    if (loom_print_name_has_local_signature_results(vtable)) {
      return (const void*)def_op;
    }
    loom_block_t* block = def_op->parent_block;
    return block ? (const void*)block->parent_region : NULL;
  }

  const loom_use_t* uses = loom_value_uses(value);
  if (!uses || value->use_count == 0) {
    return NULL;
  }
  loom_op_t* user_op = loom_use_user_op(uses[0]);
  const loom_op_vtable_t* user_vtable =
      loom_print_name_defining_op_vtable(module, user_op);
  if (loom_print_name_has_local_signature_results(user_vtable)) {
    return (const void*)user_op;
  }
  loom_block_t* block = user_op ? user_op->parent_block : NULL;
  return block ? (const void*)block->parent_region : NULL;
}

static bool loom_print_name_value_is_printable(const loom_module_t* module,
                                               loom_value_id_t value_id) {
  if (!module || value_id >= module->values.count) {
    return false;
  }
  const loom_value_t* value = loom_module_value(module, value_id);
  if (loom_value_is_block_arg(value)) {
    return loom_value_def_block(value) != NULL;
  }
  loom_op_t* def_op = loom_value_def_op(value);
  if (def_op) {
    return !iree_any_bit_set(def_op->flags, LOOM_OP_FLAG_DEAD);
  }
  return value->use_count > 0;
}

static bool loom_print_name_value_has_name(const loom_module_t* module,
                                           loom_value_id_t value_id,
                                           loom_string_id_t* out_name_id) {
  *out_name_id = LOOM_STRING_ID_INVALID;
  if (!module || value_id >= module->values.count) {
    return false;
  }
  loom_string_id_t name_id = loom_module_value(module, value_id)->name_id;
  if (name_id == LOOM_STRING_ID_INVALID || name_id >= module->strings.count) {
    return false;
  }
  *out_name_id = name_id;
  return true;
}

typedef struct loom_print_name_binding_t {
  // Explicit spelling introduced in the current lexical scope.
  loom_string_id_t name_id;
  // Previous binding restored on scope exit.
  loom_value_id_t previous_value;
} loom_print_name_binding_t;

typedef struct loom_print_name_capture_state_t {
  // IR and retained operand/type/attribute references being printed.
  const loom_module_t* module;
  // Resolutions marked when a captured reference needs disambiguation.
  loom_print_name_resolution_t* resolutions;
  // Current value bound to each interned explicit spelling.
  loom_value_id_t* active_values;
  // Scope rollback records, bounded by printable values with explicit names.
  loom_print_name_binding_t* bindings;
  // Number of active rollback records.
  iree_host_size_t binding_count;
} loom_print_name_capture_state_t;

static void loom_print_name_bind(loom_print_name_capture_state_t* state,
                                 loom_value_id_t value_id) {
  loom_string_id_t name_id;
  if (!loom_print_name_value_has_name(state->module, value_id, &name_id) ||
      !loom_print_name_value_is_printable(state->module, value_id)) {
    return;
  }
  state->bindings[state->binding_count++] = (loom_print_name_binding_t){
      .name_id = name_id,
      .previous_value = state->active_values[name_id],
  };
  state->active_values[name_id] = value_id;
}

static void loom_print_name_restore(loom_print_name_capture_state_t* state,
                                    iree_host_size_t watermark) {
  while (state->binding_count > watermark) {
    const loom_print_name_binding_t binding =
        state->bindings[--state->binding_count];
    state->active_values[binding.name_id] = binding.previous_value;
  }
}

static void loom_print_name_check_capture(
    loom_print_name_capture_state_t* state, loom_value_id_t value_id) {
  loom_string_id_t name_id;
  if (!loom_print_name_value_has_name(state->module, value_id, &name_id)) {
    return;
  }
  const loom_value_id_t binding = state->active_values[name_id];
  if (binding != LOOM_VALUE_ID_INVALID && binding != value_id) {
    state->resolutions[value_id].suffix = 1;
  }
}

static void loom_print_name_check_type_captures(
    loom_print_name_capture_state_t* state, loom_value_id_t value_id) {
  loom_type_use_iterator_t dependencies;
  loom_module_value_type_dependencies(state->module, value_id, &dependencies);
  for (loom_value_id_t referenced_id =
           loom_type_dependencies_next(&dependencies);
       referenced_id != LOOM_VALUE_ID_INVALID;
       referenced_id = loom_type_dependencies_next(&dependencies)) {
    loom_print_name_check_capture(state, referenced_id);
  }
}

// Reserve each scope's direct definitions before checking its references. This
// makes canonical names independent of CFG layout while preserving harmless
// shadowing. Every scope is visited once; reference checks never walk
// ancestors.
static void loom_print_name_check_region_captures(
    loom_print_name_capture_state_t* state, const loom_region_t* region) {
  if (!region) {
    return;
  }
  const iree_host_size_t region_watermark = state->binding_count;
  for (uint16_t b = 0; b < region->block_count; ++b) {
    const loom_block_t* block = loom_region_const_block(region, b);
    for (uint16_t i = 0; i < block->arg_count; ++i) {
      loom_print_name_bind(state, loom_block_arg_id(block, i));
    }
    for (const loom_op_t* op = block->first_op; op; op = op->next_op) {
      const loom_op_vtable_t* vtable = loom_op_vtable(state->module, op);
      if (loom_print_name_has_local_signature_results(vtable)) {
        continue;
      }
      for (uint16_t i = 0; i < op->result_count; ++i) {
        loom_print_name_bind(state, loom_op_const_results(op)[i]);
      }
    }
  }
  for (uint16_t b = 0; b < region->block_count; ++b) {
    const loom_block_t* block = loom_region_const_block(region, b);
    for (uint16_t i = 0; i < block->arg_count; ++i) {
      loom_print_name_check_type_captures(state, loom_block_arg_id(block, i));
    }
    for (const loom_op_t* op = block->first_op; op; op = op->next_op) {
      const iree_host_size_t signature_watermark = state->binding_count;
      const loom_value_id_t* operands = loom_op_const_operands(op);
      const loom_value_id_t* results = loom_op_const_results(op);
      const loom_op_vtable_t* vtable = loom_op_vtable(state->module, op);
      const bool symbol =
          iree_any_bit_set(op->traits, LOOM_TRAIT_SYMBOL_DEFINE);
      const bool local_signature_results =
          loom_print_name_has_local_signature_results(vtable);
      if (local_signature_results) {
        uint16_t argument_count = 0;
        const loom_value_id_t* arguments = NULL;
        if (symbol && vtable && vtable->func_like) {
          arguments = loom_func_like_arg_ids(
              (loom_func_like_t){.op = (loom_op_t*)op,
                                 .vtable = vtable->func_like},
              &argument_count);
        } else if (symbol && vtable && loom_op_vtable_owns_operands(vtable)) {
          arguments = operands;
          argument_count = op->operand_count;
        }
        for (uint16_t i = 0; i < argument_count; ++i) {
          loom_print_name_bind(state, arguments[i]);
        }
        for (uint16_t i = 0; i < op->result_count; ++i) {
          loom_print_name_bind(state, results[i]);
        }
        for (uint16_t i = 0; i < argument_count; ++i) {
          loom_print_name_check_type_captures(state, arguments[i]);
        }
      }
      for (uint16_t i = 0; i < op->operand_count; ++i) {
        loom_print_name_check_capture(state, operands[i]);
        loom_print_name_check_type_captures(state, operands[i]);
      }
      for (uint16_t i = 0; i < op->result_count; ++i) {
        loom_print_name_check_type_captures(state, results[i]);
      }
      for (uint8_t i = 0; i < op->attribute_count; ++i) {
        loom_type_use_iterator_t dependencies;
        loom_attribute_dependencies_begin(&state->module->type_uses, op, i,
                                          &dependencies);
        for (loom_value_id_t referenced_id =
                 loom_type_dependencies_next(&dependencies);
             referenced_id != LOOM_VALUE_ID_INVALID;
             referenced_id = loom_type_dependencies_next(&dependencies)) {
          loom_print_name_check_capture(state, referenced_id);
        }
      }
      loom_print_name_restore(state, signature_watermark);
      for (uint8_t i = 0; i < op->region_count; ++i) {
        loom_print_name_check_region_captures(state, loom_op_regions(op)[i]);
      }
    }
  }
  loom_print_name_restore(state, region_watermark);
}

static uint64_t loom_print_name_hash(const void* scope,
                                     loom_string_id_t name_id) {
  uint64_t value = (uint64_t)(uintptr_t)scope;
  value ^= (uint64_t)name_id + UINT64_C(0x9e3779b97f4a7c15) + (value << 6) +
           (value >> 2);
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

static loom_print_name_index_entry_t* loom_print_name_index_find(
    loom_print_name_index_entry_t* entries, iree_host_size_t capacity,
    const void* scope, loom_string_id_t name_id) {
  if (!entries || capacity == 0) {
    return NULL;
  }
  const uint32_t name_key = name_id + 1;
  const iree_host_size_t mask = capacity - 1;
  iree_host_size_t slot =
      (iree_host_size_t)loom_print_name_hash(scope, name_id) & mask;
  while (entries[slot].name_key != 0) {
    if (entries[slot].name_key == name_key && entries[slot].scope == scope) {
      return &entries[slot];
    }
    slot = (slot + 1) & mask;
  }
  return NULL;
}

static void loom_print_name_index_insert(loom_print_name_index_entry_t* entries,
                                         iree_host_size_t capacity,
                                         const void* scope,
                                         loom_string_id_t name_id) {
  const uint32_t name_key = name_id + 1;
  const iree_host_size_t mask = capacity - 1;
  iree_host_size_t slot =
      (iree_host_size_t)loom_print_name_hash(scope, name_id) & mask;
  while (entries[slot].name_key != 0) {
    if (entries[slot].name_key == name_key && entries[slot].scope == scope) {
      entries[slot].duplicated = true;
      return;
    }
    slot = (slot + 1) & mask;
  }
  entries[slot].scope = scope;
  entries[slot].name_key = name_key;
}

static bool loom_print_name_is_explicit(const loom_module_t* module,
                                        const uint8_t* explicit_names,
                                        iree_string_view_t name) {
  loom_string_id_t name_id = loom_module_lookup_string(module, name);
  return name_id != LOOM_STRING_ID_INVALID &&
         (explicit_names[name_id / 8] & (1u << (name_id % 8))) != 0;
}

static iree_string_view_t loom_print_name_format_suffix(
    loom_value_id_t value_id, uint32_t suffix, char* buffer,
    iree_host_size_t buffer_capacity) {
  int length = 0;
  if (suffix == 0) {
    length = iree_snprintf(buffer, buffer_capacity, "%" PRIu32, value_id);
  } else if (suffix == 1) {
    length = iree_snprintf(buffer, buffer_capacity, "$%" PRIu32, value_id);
  } else {
    length = iree_snprintf(buffer, buffer_capacity, "$%" PRIu32 "$%" PRIu32,
                           suffix - 1, value_id);
  }
  IREE_ASSERT(length > 0 && (iree_host_size_t)length < buffer_capacity);
  return iree_make_string_view(buffer, (iree_host_size_t)length);
}

// The final $ID component makes candidate families disjoint across values,
// including values whose explicit names already contain dollar markers. Each
// failed lookup therefore consumes a distinct explicit spelling, bounding total
// probes by values plus explicit names. At most the other values can obstruct
// one value, so the selected suffix fits in the module's uint32_t ID space.
static uint32_t loom_print_name_resolve_suffix(
    const loom_module_t* module, const uint8_t* explicit_names,
    loom_value_id_t value_id, iree_string_view_t base_name, uint32_t suffix,
    char* buffer, iree_host_size_t buffer_capacity) {
  if (base_name.size) {
    memcpy(buffer, base_name.data, base_name.size);
  }
  for (;;) {
    iree_string_view_t tail =
        loom_print_name_format_suffix(value_id, suffix, buffer + base_name.size,
                                      buffer_capacity - base_name.size);
    iree_string_view_t candidate =
        iree_make_string_view(buffer, base_name.size + tail.size);
    if (!loom_print_name_is_explicit(module, explicit_names, candidate)) {
      break;
    }
    ++suffix;
  }
  return suffix;
}

iree_status_t loom_print_name_plan_initialize(
    const loom_module_t* module, loom_print_name_plan_t* out_plan) {
  memset(out_plan, 0, sizeof(*out_plan));
  iree_arena_initialize(module->arena.block_pool, &out_plan->arena);
  if (module->values.count == 0) {
    return iree_ok_status();
  }

  iree_host_size_t named_value_count = 0;
  iree_host_size_t indexed_name_count = 0;
  iree_host_size_t maximum_name_length = 0;
  for (iree_host_size_t i = 0; i < module->values.count; ++i) {
    loom_string_id_t name_id = LOOM_STRING_ID_INVALID;
    if (!loom_print_name_value_has_name(module, (loom_value_id_t)i, &name_id)) {
      continue;
    }
    ++named_value_count;
    if (loom_string_table_get(&module->strings, name_id).size >
        maximum_name_length) {
      maximum_name_length =
          loom_string_table_get(&module->strings, name_id).size;
    }
    if (loom_print_name_value_is_printable(module, (loom_value_id_t)i)) {
      ++indexed_name_count;
    }
  }
  if (named_value_count == 0) {
    return iree_ok_status();
  }

  iree_status_t status = iree_arena_allocate_array(
      &out_plan->arena, module->values.count, sizeof(*out_plan->resolutions),
      (void**)&out_plan->resolutions);
  if (!iree_status_is_ok(status)) {
    loom_print_name_plan_deinitialize(out_plan);
    return status;
  }
  memset(out_plan->resolutions, 0,
         module->values.count * sizeof(*out_plan->resolutions));

  if (indexed_name_count == 0) {
    return iree_ok_status();
  }

  iree_host_size_t index_capacity =
      iree_host_size_next_power_of_two((indexed_name_count * 4 + 2) / 3);
  if (index_capacity == 0) {
    loom_print_name_plan_deinitialize(out_plan);
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "SSA name index capacity exceeds storage limit");
  }
  if (index_capacity < 16) {
    index_capacity = 16;
  }

  iree_host_size_t index_offset = 0;
  iree_host_size_t explicit_names_offset = 0;
  const iree_host_size_t explicit_names_size =
      iree_host_size_ceil_div(module->strings.count, 8);
  iree_host_size_t candidate_buffer_offset = 0;
  iree_host_size_t temporary_size = 0;
  iree_arena_checkpoint_t temporary_checkpoint =
      iree_arena_checkpoint_save(&out_plan->arena);
  status = IREE_STRUCT_LAYOUT(
      0, &temporary_size,
      IREE_STRUCT_FIELD_ALIGNED(index_capacity, loom_print_name_index_entry_t,
                                iree_alignof(loom_print_name_index_entry_t),
                                &index_offset),
      IREE_STRUCT_FIELD(explicit_names_size, uint8_t, &explicit_names_offset),
      IREE_STRUCT_FIELD(maximum_name_length, char, &candidate_buffer_offset),
      IREE_STRUCT_FIELD(LOOM_PRINT_NAME_SUFFIX_BUFFER_SIZE, char, NULL));
  void* temporary_storage = NULL;
  if (iree_status_is_ok(status)) {
    status = iree_arena_allocate(&out_plan->arena, temporary_size,
                                 &temporary_storage);
  }
  if (!iree_status_is_ok(status)) {
    iree_arena_checkpoint_restore(&temporary_checkpoint);
    loom_print_name_plan_deinitialize(out_plan);
    return status;
  }

  loom_print_name_index_entry_t* index_entries =
      (loom_print_name_index_entry_t*)((uint8_t*)temporary_storage +
                                       index_offset);
  memset(index_entries, 0, index_capacity * sizeof(*index_entries));
  uint8_t* explicit_names = (uint8_t*)temporary_storage + explicit_names_offset;
  memset(explicit_names, 0, explicit_names_size);
  char* candidate_buffer = (char*)temporary_storage + candidate_buffer_offset;
  const iree_host_size_t candidate_buffer_capacity =
      temporary_size - candidate_buffer_offset;

  bool shared_spelling = false;
  for (iree_host_size_t i = 0; i < module->values.count; ++i) {
    loom_string_id_t name_id = LOOM_STRING_ID_INVALID;
    if (loom_print_name_value_is_printable(module, (loom_value_id_t)i) &&
        loom_print_name_value_has_name(module, (loom_value_id_t)i, &name_id)) {
      // Reserve explicit spellings across scopes so generated names cannot
      // shadow an explicit name in an enclosing or nested parser scope.
      shared_spelling |=
          (explicit_names[name_id / 8] & (1u << (name_id % 8))) != 0;
      explicit_names[name_id / 8] |= 1u << (name_id % 8);
      loom_print_name_index_insert(
          index_entries, index_capacity,
          loom_print_name_parse_scope(module, (loom_value_id_t)i), name_id);
    }
  }

  if (shared_spelling) {
    loom_print_name_capture_state_t state = {
        .module = module,
        .resolutions = out_plan->resolutions,
    };
    status = iree_arena_allocate_array(&out_plan->arena, module->strings.count,
                                       sizeof(*state.active_values),
                                       (void**)&state.active_values);
    if (iree_status_is_ok(status)) {
      status = iree_arena_allocate_array(&out_plan->arena, indexed_name_count,
                                         sizeof(*state.bindings),
                                         (void**)&state.bindings);
    }
    if (!iree_status_is_ok(status)) {
      iree_arena_checkpoint_restore(&temporary_checkpoint);
      loom_print_name_plan_deinitialize(out_plan);
      return status;
    }
    memset(state.active_values, 0xFF,
           module->strings.count * sizeof(*state.active_values));
    loom_print_name_check_region_captures(&state, module->body);
  }

  for (iree_host_size_t i = 0; i < module->values.count; ++i) {
    const loom_value_id_t value_id = (loom_value_id_t)i;
    const void* scope = loom_print_name_parse_scope(module, value_id);
    loom_string_id_t name_id = LOOM_STRING_ID_INVALID;
    if (!loom_print_name_value_has_name(module, value_id, &name_id)) {
      out_plan->resolutions[i].suffix = loom_print_name_resolve_suffix(
          module, explicit_names, value_id, iree_string_view_empty(), 0,
          candidate_buffer, candidate_buffer_capacity);
      continue;
    }

    loom_print_name_index_entry_t* entry = loom_print_name_index_find(
        index_entries, index_capacity, scope, name_id);
    const bool duplicated =
        entry && (entry->duplicated ||
                  !loom_print_name_value_is_printable(module, value_id));
    if (!duplicated && out_plan->resolutions[i].suffix == 0) {
      continue;
    }
    out_plan->resolutions[i].suffix = loom_print_name_resolve_suffix(
        module, explicit_names, value_id,
        loom_string_table_get(&module->strings, name_id), 1, candidate_buffer,
        candidate_buffer_capacity);
  }

  iree_arena_checkpoint_restore(&temporary_checkpoint);
  return iree_ok_status();
}

void loom_print_name_plan_deinitialize(loom_print_name_plan_t* plan) {
  iree_arena_deinitialize(&plan->arena);
  memset(plan, 0, sizeof(*plan));
}

static iree_status_t loom_print_name_write_resolution(
    loom_output_stream_t* stream, const loom_module_t* module,
    loom_value_id_t value_id, loom_print_name_resolution_t resolution) {
  IREE_RETURN_IF_ERROR(loom_output_stream_write_char(stream, '%'));
  loom_string_id_t name_id = LOOM_STRING_ID_INVALID;
  if (loom_print_name_value_has_name(module, value_id, &name_id)) {
    IREE_RETURN_IF_ERROR(loom_output_stream_write(
        stream, loom_string_table_get(&module->strings, name_id)));
    if (resolution.suffix == 0) {
      return iree_ok_status();
    }
  }
  char buffer[LOOM_PRINT_NAME_SUFFIX_BUFFER_SIZE];
  return loom_output_stream_write(
      stream, loom_print_name_format_suffix(value_id, resolution.suffix, buffer,
                                            sizeof(buffer)));
}

iree_status_t loom_print_name_plan_write_value_ref(loom_print_name_plan_t* plan,
                                                   loom_output_stream_t* stream,
                                                   const loom_module_t* module,
                                                   loom_value_id_t value_id) {
  if (!module || value_id >= module->values.count) {
    return loom_output_stream_write_cstring(stream, "%?");
  }
  if (!plan->arena.block_pool) {
    IREE_RETURN_IF_ERROR(loom_print_name_plan_initialize(module, plan));
  }
  if (!plan->resolutions) {
    return loom_output_stream_write_format(stream, "%%%" PRIu32, value_id);
  }
  return loom_print_name_write_resolution(stream, module, value_id,
                                          plan->resolutions[value_id]);
}
