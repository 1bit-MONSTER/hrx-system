// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/module_state.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "iree/base/internal/math.h"
#include "loom/codegen/low/lower/rule_descriptor.h"
#include "loom/ir/intern_table.h"
#include "loom/ir/module.h"
#include "loom/ir/symbol_map.h"
#include "loom/ops/global/ops.h"

typedef struct loom_low_lower_module_target_state_record_t {
  // Target-owned static key identifying this module-scope state object.
  const void* key;
  // Byte length of state storage.
  iree_host_size_t data_length;
  // Zero-initialized state storage allocated from the module-state arena.
  void* data;
} loom_low_lower_module_target_state_record_t;

typedef struct loom_low_lower_read_only_data_record_t {
  // Module-local symbol published during execution; null while only planned.
  loom_symbol_ref_t symbol;
  // First source location that requested the payload.
  loom_location_id_t location;
  // Copied immutable payload bytes.
  iree_const_byte_span_t contents;
  // Content hash retained for the symbol name assigned during execution.
  uint32_t hash;
  // Log2 of the maximum power-of-two alignment requested by equal payloads.
  uint8_t minimum_alignment_log2;
} loom_low_lower_read_only_data_record_t;
static_assert(sizeof(loom_low_lower_read_only_data_record_t) <= 32,
              "immutable payload records must fit in 32 bytes");

struct loom_low_lower_module_state_t {
  // Arena used for module-scope target state records and payloads.
  iree_arena_allocator_t* arena;
  // Shared immutable table bindings, selected lazily by function lowerings.
  loom_low_lower_rule_descriptor_cache_t* rule_descriptor_cache;
  // Module-scope target state records keyed by target-owned static storage.
  loom_low_lower_module_target_state_record_t* target_state_records;
  // Number of populated target_state_records entries.
  iree_host_size_t target_state_record_count;
  // Number of allocated target_state_records entries.
  iree_host_size_t target_state_record_capacity;
  // Interned immutable payload records.
  loom_low_lower_read_only_data_record_t* read_only_data_records;
  // Number of populated read_only_data_records entries.
  iree_host_size_t read_only_data_record_count;
  // Number of allocated read_only_data_records entries.
  iree_host_size_t read_only_data_record_capacity;
  // Content index over read_only_data_records.
  loom_intern_table_t read_only_data_index;
  // Module symbol names indexed for constant-time reservations.
  loom_symbol_map_t symbol_names;
  // Module symbol prefix already captured in symbol_names.
  iree_host_size_t indexed_symbol_count;
  // True after all interned payloads have been materialized.
  bool finalized;
};

iree_status_t loom_low_lower_module_state_allocate(
    iree_arena_allocator_t* arena,
    loom_low_lower_module_state_t** out_module_state) {
  *out_module_state = NULL;
  loom_low_lower_module_state_t* module_state = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, sizeof(*module_state), (void**)&module_state));
  memset(module_state, 0, sizeof(*module_state));
  module_state->arena = arena;
  IREE_RETURN_IF_ERROR(loom_intern_table_initialize(
      arena, /*capacity=*/0, &module_state->read_only_data_index));
  *out_module_state = module_state;
  return iree_ok_status();
}

iree_status_t loom_low_lower_module_state_rule_descriptor_cache(
    loom_low_lower_module_state_t* module_state,
    loom_low_lower_rule_set_list_t rule_sets,
    const loom_low_descriptor_set_t* descriptor_set,
    loom_low_lower_rule_descriptor_cache_t** out_cache) {
  IREE_RETURN_IF_ERROR(loom_low_lower_rule_descriptor_cache_select(
      rule_sets, descriptor_set, module_state->arena,
      &module_state->rule_descriptor_cache));
  *out_cache = module_state->rule_descriptor_cache;
  return iree_ok_status();
}

iree_status_t loom_low_lower_module_state_get_or_allocate(
    loom_low_lower_module_state_t* module_state, const void* key,
    iree_host_size_t data_length, void** out_data) {
  IREE_ASSERT(key != NULL);
  IREE_ASSERT_GT(data_length, 0);
  *out_data = NULL;
  if (module_state == NULL) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "module-scope target lowering state is required");
  }
  for (iree_host_size_t i = 0; i < module_state->target_state_record_count;
       ++i) {
    loom_low_lower_module_target_state_record_t* record =
        &module_state->target_state_records[i];
    if (record->key != key) {
      continue;
    }
    IREE_ASSERT_EQ(record->data_length, data_length);
    *out_data = record->data;
    return iree_ok_status();
  }

  if (module_state->target_state_record_count ==
      module_state->target_state_record_capacity) {
    iree_host_size_t minimum_capacity = 0;
    if (!iree_host_size_checked_add(module_state->target_state_record_count, 1,
                                    &minimum_capacity)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE, "capacity overflow");
    }
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        module_state->arena, module_state->target_state_record_count,
        minimum_capacity, sizeof(*module_state->target_state_records),
        &module_state->target_state_record_capacity,
        (void**)&module_state->target_state_records));
  }

  void* data = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(module_state->arena, data_length, &data));
  memset(data, 0, data_length);
  const iree_host_size_t record_index =
      module_state->target_state_record_count++;
  loom_low_lower_module_target_state_record_t* record =
      &module_state->target_state_records[record_index];
  *record = (loom_low_lower_module_target_state_record_t){
      .key = key,
      .data_length = data_length,
      .data = data,
  };
  *out_data = data;
  return iree_ok_status();
}

static uint32_t loom_low_lower_read_only_data_hash(
    iree_const_byte_span_t contents) {
  uint32_t hash = (2166136261u ^ (uint32_t)contents.data_length) * 16777619u;
  for (iree_host_size_t i = 0; i < contents.data_length; ++i) {
    hash = (hash ^ contents.data[i]) * 16777619u;
  }
  return hash;
}

typedef struct loom_low_lower_read_only_data_query_t {
  // Current record-array base, which may move between interning calls.
  const loom_low_lower_read_only_data_record_t* records;
  // Borrowed bytes being compared with interned, arena-owned payloads.
  iree_const_byte_span_t contents;
} loom_low_lower_read_only_data_query_t;

static bool loom_low_lower_read_only_data_equal(const void* context,
                                                uint32_t record_index) {
  const loom_low_lower_read_only_data_query_t* query = context;
  const iree_const_byte_span_t existing = query->records[record_index].contents;
  return existing.data_length == query->contents.data_length &&
         memcmp(existing.data, query->contents.data,
                query->contents.data_length) == 0;
}

static iree_status_t loom_low_lower_module_state_refresh_symbol_names(
    loom_low_lower_module_state_t* module_state, const loom_module_t* module) {
  while (module_state->indexed_symbol_count < module->symbols.count) {
    const iree_host_size_t symbol_id = module_state->indexed_symbol_count;
    const loom_string_id_t name_id = module->symbols.entries[symbol_id].name_id;
    if (name_id != LOOM_STRING_ID_INVALID) {
      loom_symbol_id_t indexed_symbol_id = LOOM_SYMBOL_ID_INVALID;
      IREE_RETURN_IF_ERROR(loom_symbol_map_find_or_insert(
          &module_state->symbol_names, module_state->arena, name_id,
          (loom_symbol_id_t)symbol_id, &indexed_symbol_id));
    }
    ++module_state->indexed_symbol_count;
  }
  return iree_ok_status();
}

static iree_status_t loom_low_lower_module_state_reserve_read_only_data_symbol(
    loom_low_lower_module_state_t* module_state, loom_module_t* module,
    uint32_t hash, loom_symbol_ref_t* out_symbol) {
  *out_symbol = loom_symbol_ref_null();
  IREE_RETURN_IF_ERROR(
      loom_low_lower_module_state_refresh_symbol_names(module_state, module));
  char name_storage[64];
  for (uint32_t discriminator = 0; discriminator < LOOM_SYMBOL_ID_INVALID;
       ++discriminator) {
    const int name_length =
        discriminator == 0 ? snprintf(name_storage, sizeof(name_storage),
                                      "__low_rodata_%08" PRIx32, hash)
                           : snprintf(name_storage, sizeof(name_storage),
                                      "__low_rodata_%08" PRIx32 "$%" PRIu32,
                                      hash, discriminator);
    if (name_length < 0 ||
        (iree_host_size_t)name_length >= sizeof(name_storage)) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "read-only data symbol name overflow");
    }
    const iree_string_view_t name =
        iree_make_string_view(name_storage, (iree_host_size_t)name_length);
    const loom_string_id_t existing_name_id =
        loom_module_lookup_string(module, name);
    if (existing_name_id != LOOM_STRING_ID_INVALID &&
        loom_symbol_map_find(&module_state->symbol_names, existing_name_id) !=
            LOOM_SYMBOL_ID_INVALID) {
      continue;
    }

    loom_string_id_t name_id = LOOM_STRING_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_module_intern_string(module, name, &name_id));
    out_symbol->module_id = 0;
    IREE_RETURN_IF_ERROR(
        loom_module_add_symbol(module, name_id, &out_symbol->symbol_id));
    IREE_RETURN_IF_ERROR(loom_symbol_map_insert(&module_state->symbol_names,
                                                module_state->arena, name_id,
                                                out_symbol->symbol_id));
    ++module_state->indexed_symbol_count;
    return iree_ok_status();
  }
  return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                          "read-only data symbol namespace exhausted");
}

iree_status_t loom_low_lower_module_state_intern_read_only_data(
    loom_low_lower_module_state_t* module_state,
    iree_const_byte_span_t contents, uint64_t minimum_alignment,
    loom_location_id_t location, loom_low_lower_read_only_data_id_t* out_id) {
  IREE_ASSERT_FALSE(module_state->finalized);

  const uint32_t hash = loom_low_lower_read_only_data_hash(contents);
  const uint8_t minimum_alignment_log2 =
      (uint8_t)iree_math_count_trailing_zeros_u64(minimum_alignment);
  const loom_low_lower_read_only_data_query_t query = {
      .records = module_state->read_only_data_records,
      .contents = contents,
  };
  const loom_intern_probe_t probe =
      loom_intern_table_probe(&module_state->read_only_data_index, hash,
                              loom_low_lower_read_only_data_equal, &query);
  if (probe.index != UINT32_MAX) {
    loom_low_lower_read_only_data_record_t* record =
        &module_state->read_only_data_records[probe.index];
    record->minimum_alignment_log2 =
        iree_max(record->minimum_alignment_log2, minimum_alignment_log2);
    *out_id = probe.index;
    return iree_ok_status();
  }

  if (module_state->read_only_data_record_count + 1 > UINT16_MAX) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "too many interned read-only data payloads");
  }
  iree_host_size_t slot = probe.slot;
  IREE_RETURN_IF_ERROR(loom_intern_table_reserve_insert(
      module_state->arena, &module_state->read_only_data_index, hash,
      /*insertion_count=*/1, &slot));
  if (module_state->read_only_data_record_count ==
      module_state->read_only_data_record_capacity) {
    const iree_host_size_t minimum_capacity =
        module_state->read_only_data_record_count + 1;
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        module_state->arena, module_state->read_only_data_record_count,
        iree_max(minimum_capacity, 8u),
        sizeof(*module_state->read_only_data_records),
        &module_state->read_only_data_record_capacity,
        (void**)&module_state->read_only_data_records));
  }

  uint8_t* copied_data = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(
      module_state->arena, contents.data_length, (void**)&copied_data));
  memcpy(copied_data, contents.data, contents.data_length);
  const uint32_t record_index =
      (uint32_t)module_state->read_only_data_record_count++;
  module_state->read_only_data_records[record_index] =
      (loom_low_lower_read_only_data_record_t){
          .symbol = loom_symbol_ref_null(),
          .location = location,
          .contents =
              iree_make_const_byte_span(copied_data, contents.data_length),
          .hash = hash,
          .minimum_alignment_log2 = minimum_alignment_log2,
      };
  loom_intern_table_insert(&module_state->read_only_data_index, slot, hash,
                           record_index);
  *out_id = record_index;
  return iree_ok_status();
}

iree_status_t loom_low_lower_module_state_reference_read_only_data(
    loom_low_lower_module_state_t* module_state, loom_module_t* module,
    loom_low_lower_read_only_data_id_t id, loom_symbol_ref_t* out_symbol) {
  loom_low_lower_read_only_data_record_t* record =
      &module_state->read_only_data_records[id];
  if (!loom_symbol_ref_is_valid(record->symbol)) {
    IREE_RETURN_IF_ERROR(
        loom_low_lower_module_state_reserve_read_only_data_symbol(
            module_state, module, record->hash, &record->symbol));
  }
  *out_symbol = record->symbol;
  return iree_ok_status();
}

iree_status_t loom_low_lower_module_state_finalize(
    loom_low_lower_module_state_t* module_state, loom_module_t* module) {
  IREE_ASSERT(module_state != NULL);
  IREE_ASSERT(module != NULL);
  if (module_state->finalized) {
    return iree_ok_status();
  }
  loom_builder_t builder;
  loom_builder_initialize(module, &module->arena, loom_module_block(module),
                          &builder);
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; iree_status_is_ok(status) &&
                               i < module_state->read_only_data_record_count;
       ++i) {
    const loom_low_lower_read_only_data_record_t* record =
        &module_state->read_only_data_records[i];
    loom_symbol_ref_t symbol = loom_symbol_ref_null();
    status = loom_low_lower_module_state_reference_read_only_data(
        module_state, module, (loom_low_lower_read_only_data_id_t)i, &symbol);
    if (iree_status_is_ok(status)) {
      loom_op_t* definition = NULL;
      status = loom_global_rodata_def_build(
          &builder, LOOM_GLOBAL_RODATA_DEF_BUILD_FLAG_HAS_ALIGNMENT, symbol,
          (int64_t)(UINT64_C(1) << record->minimum_alignment_log2),
          loom_symbol_ref_array_empty(), record->contents, record->location,
          &definition);
    }
  }
  module_state->finalized = iree_status_is_ok(status);
  return status;
}

iree_status_t loom_low_lower_module_state_allocate_bytes(
    loom_low_lower_module_state_t* module_state, iree_host_size_t byte_length,
    void** out_ptr) {
  *out_ptr = NULL;
  if (module_state == NULL) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "module-scope target lowering state is required");
  }
  if (byte_length == 0) {
    return iree_ok_status();
  }
  return iree_arena_allocate(module_state->arena, byte_length, out_ptr);
}

iree_status_t loom_low_lower_module_state_allocate_array(
    loom_low_lower_module_state_t* module_state, iree_host_size_t count,
    iree_host_size_t element_size, void** out_ptr) {
  *out_ptr = NULL;
  if (count == 0) {
    return iree_ok_status();
  }
  if (module_state == NULL) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "module-scope target lowering state is required");
  }
  return iree_arena_allocate_array(module_state->arena, count, element_size,
                                   out_ptr);
}
