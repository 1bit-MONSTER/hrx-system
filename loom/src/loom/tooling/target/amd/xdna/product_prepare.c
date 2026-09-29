// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/amd/xdna/product_prepare.h"

#include "loom/analysis/symbol_facts.h"
#include "loom/codegen/low/diagnostics.h"
#include "loom/codegen/low/function_requirements.h"
#include "loom/codegen/low/target_binding.h"
#include "loom/error/diagnostic.h"
#include "loom/ir/module.h"
#include "loom/ops/low/ops.h"
#include "loom/ops/op_defs.h"
#include "loom/target/arch/amd/xdna/aie2p/array/binding_records.h"
#include "loom/target/arch/amd/xdna/aie2p/array/plan.h"
#include "loom/target/arch/amd/xdna/aie2p/array/program.h"
#include "loom/target/arch/amd/xdna/aie2p/array/resident.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/tile_link.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/xdna_product_prepare.h"
#include "loom/target/arch/amd/xdna/aie2p/facts.h"
#include "loom/target/arch/amd/xdna/device/profile.h"
#include "loom/target/arch/amd/xdna/error_catalog.h"
#include "loom/target/function_version.h"
#include "loom/target/reporting/low.h"
#include "loom/tooling/target/amd/xdna/array_report.h"
#include "loom/tooling/target/amd/xdna/leaf_compile.h"

static bool loom_aie2p_xdna_has_contract(const loom_module_t* module,
                                         const loom_op_t* function_op,
                                         iree_string_view_t contract) {
  const loom_func_like_t function =
      loom_func_like_const_cast(module, function_op);
  const loom_string_id_t contract_id = loom_func_like_repr_contract(function);
  return contract_id < module->strings.count &&
         iree_string_view_equal(
             loom_string_table_get(&module->strings, contract_id), contract);
}

static iree_status_t loom_aie2p_xdna_resolve_device_profile(
    const loom_xdna_product_prepare_request_t* request,
    const loom_xdna_device_profile_t** out_device_profile, bool* out_valid) {
  *out_device_profile = NULL;
  *out_valid = false;
  const loom_xdna_device_profile_t* device_profile = request->device_profile;
  for (iree_host_size_t i = 0; i < request->entries.count; ++i) {
    const loom_target_entry_t* entry = &request->entries.values[i];
    const loom_aie2p_target_facts_t* target_facts =
        loom_aie2p_target_facts_cast(entry->target_facts);
    const loom_xdna_device_profile_t* function_profile =
        target_facts != NULL ? target_facts->device_profile : NULL;
    if (function_profile == NULL) {
      continue;
    }
    if (device_profile != NULL && device_profile != function_profile) {
      const loom_diagnostic_param_t params[] = {
          loom_param_string(entry->func_name),
          loom_param_string(iree_make_cstring_view(function_profile->key)),
          loom_param_string(iree_make_cstring_view(device_profile->key)),
      };
      const loom_diagnostic_emission_t emission = {
          .op = entry->func.op,
          .error = LOOM_ERR_XDNA_035,
          .params = params,
          .param_count = IREE_ARRAYSIZE(params),
      };
      return iree_diagnostic_emit(request->diagnostic_emitter, &emission);
    }
    device_profile = function_profile;
  }
  if (device_profile == NULL) {
    const loom_target_entry_t* entry = &request->entries.values[0];
    const loom_diagnostic_param_t params[] = {
        loom_param_string(entry->func_name),
    };
    const loom_diagnostic_emission_t emission = {
        .op = entry->func.op,
        .error = LOOM_ERR_XDNA_034,
        .params = params,
        .param_count = IREE_ARRAYSIZE(params),
    };
    return iree_diagnostic_emit(request->diagnostic_emitter, &emission);
  }
  *out_device_profile = device_profile;
  *out_valid = true;
  return iree_ok_status();
}

static iree_string_view_t loom_aie2p_xdna_product_issue_quantity(
    loom_aie2p_xdna_product_issue_kind_t kind) {
  switch (kind) {
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_ENTRY_COUNT:
      return IREE_SV("entries");
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_ENTRY_NAME_BYTE_LENGTH:
      return IREE_SV("entry-name bytes");
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_BINDING_RECORD_COUNT:
      return IREE_SV("binding records");
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_RELOCATION_RECORD_COUNT:
      return IREE_SV("relocation records");
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_METADATA_BYTE_LENGTH:
      return IREE_SV("metadata bytes");
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_PROGRAM_HEADER_COUNT:
      return IREE_SV("program headers");
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_SECTION_HEADER_COUNT:
      return IREE_SV("section headers");
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_NATIVE_COMMAND_BYTE_LENGTH:
      return IREE_SV("native command bytes");
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_SYMBOL_STRING_BYTE_LENGTH:
      return IREE_SV("native symbol-name bytes");
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_NONE:
    case LOOM_AIE2P_XDNA_PRODUCT_ISSUE_PARTITION_COLUMN_COUNT:
      break;
  }
  IREE_ASSERT_UNREACHABLE("XDNA product issue kind");
  return IREE_SV("image records");
}

static iree_status_t loom_aie2p_xdna_emit_product_issue(
    const loom_xdna_product_prepare_request_t* request,
    const loom_xdna_device_profile_t* profile,
    const loom_aie2p_xdna_product_issue_t* issue) {
  const iree_host_size_t entry_ordinal =
      issue->entry_ordinal < request->entries.count
          ? issue->entry_ordinal
          : request->entries.count - 1u;
  const loom_target_entry_t* entry = &request->entries.values[entry_ordinal];
  if (issue->kind == LOOM_AIE2P_XDNA_PRODUCT_ISSUE_PARTITION_COLUMN_COUNT) {
    const loom_diagnostic_param_t params[] = {
        loom_param_string(entry->func_name),
        loom_param_u32((uint32_t)issue->actual),
        loom_param_string(iree_make_cstring_view(profile->key)),
        loom_param_u32((uint32_t)issue->minimum),
        loom_param_u32((uint32_t)issue->maximum),
    };
    const loom_diagnostic_emission_t emission = {
        .op = entry->func.op,
        .error = LOOM_ERR_XDNA_037,
        .params = params,
        .param_count = IREE_ARRAYSIZE(params),
    };
    return iree_diagnostic_emit(request->diagnostic_emitter, &emission);
  }
  const loom_diagnostic_param_t params[] = {
      loom_param_u64(issue->actual),
      loom_param_string(loom_aie2p_xdna_product_issue_quantity(issue->kind)),
      loom_param_string(entry->func_name),
      loom_param_u64(issue->maximum),
  };
  const loom_diagnostic_emission_t emission = {
      .op = entry->func.op,
      .error = LOOM_ERR_XDNA_036,
      .params = params,
      .param_count = IREE_ARRAYSIZE(params),
  };
  return iree_diagnostic_emit(request->diagnostic_emitter, &emission);
}

static iree_status_t loom_aie2p_xdna_collect_source_leaves(
    const loom_xdna_product_prepare_request_t* request,
    loom_aie2p_array_leaf_t** out_leaves, iree_host_size_t* out_leaf_count,
    bool* out_valid) {
  *out_leaves = NULL;
  *out_leaf_count = 0;
  *out_valid = false;
  loom_aie2p_array_leaf_t* leaves = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      request->scratch_arena, request->module->symbols.count, sizeof(*leaves),
      (void**)&leaves));
  loom_symbol_fact_table_t symbol_facts = {0};
  loom_symbol_fact_table_initialize(&symbol_facts, request->scratch_arena);
  loom_target_function_version_snapshot_t versions = {0};
  IREE_RETURN_IF_ERROR(loom_target_function_version_snapshot_build(
      request->module, request->function_versions, request->scratch_arena,
      &versions));
  iree_host_size_t leaf_index = 0;
  const loom_symbol_t* symbol = NULL;
  loom_module_for_each_symbol(request->module, symbol) {
    const loom_op_t* function_op = symbol->defining_op;
    if (function_op == NULL || !loom_low_func_def_isa(function_op) ||
        !loom_aie2p_xdna_has_contract(request->module, function_op,
                                      IREE_SV("amd.xdna.aie2p.core"))) {
      continue;
    }
    const loom_symbol_id_t symbol_id =
        (loom_symbol_id_t)(symbol - request->module->symbols.entries);
    const loom_target_function_version_t* version =
        loom_target_function_version_snapshot_at(&versions, symbol_id);
    loom_low_resolved_target_t target = {0};
    IREE_RETURN_IF_ERROR(loom_low_resolve_function_target(
        request->module, &symbol_facts, function_op,
        version != NULL ? version->function_target_facts : NULL,
        request->low_descriptor_registry, request->diagnostic_emitter,
        &target));
    if (target.descriptor_set == NULL) {
      return iree_ok_status();
    }
    // Array planning consumes declared interfaces and storage. Physical
    // lifetimes belong to the resident program after imports are bound and
    // protocol operations are inserted, not to this shared source body.
    loom_low_function_requirements_t requirements = {0};
    IREE_RETURN_IF_ERROR(loom_low_function_requirements_build(
        request->module, loom_low_func_def_body(function_op),
        request->scratch_arena, &requirements));
    leaves[leaf_index] = (loom_aie2p_array_leaf_t){
        .entry =
            {
                .module_id = 0,
                .symbol_id = symbol_id,
            },
        .function_op = function_op,
        .function_target_facts = target.target_facts,
        .memory_accesses = version != NULL ? version->memory_accesses : NULL,
        .requirements = requirements,
    };
    ++leaf_index;
  }
  *out_leaves = leaves;
  *out_leaf_count = leaf_index;
  *out_valid = true;
  return iree_ok_status();
}

static void loom_aie2p_xdna_plan_tile_link(
    const loom_aie2p_array_plan_t* plan, uint32_t worker_index,
    const loom_aie2p_leaf_realization_t* realization,
    loom_aie2p_tile_storage_placement_t* storage_placements,
    loom_aie2p_tile_link_layout_t* out_layout) {
  IREE_ASSERT_LE(realization->storage_domain_count, LOOM_STORAGE_SPACE_COUNT_);
  for (iree_host_size_t i = 0; i < realization->storage_domain_count; ++i) {
    const loom_aie2p_leaf_storage_domain_t* domain =
        &realization->storage_domains[i];
    const loom_aie2p_array_worker_storage_plan_t* placement = NULL;
    for (iree_host_size_t j = 0; j < plan->worker_storage_count; ++j) {
      if (plan->worker_storage[j].worker_index == worker_index &&
          plan->worker_storage[j].storage_space == domain->storage_space) {
        placement = &plan->worker_storage[j];
        break;
      }
    }
    const loom_aie2p_leaf_storage_requirement_t* requirement =
        loom_aie2p_leaf_storage_requirement(realization, domain->storage_space);
    IREE_ASSERT(placement != NULL);
    IREE_ASSERT(requirement != NULL);
    IREE_ASSERT_EQ(placement->byte_length, requirement->byte_length);
    storage_placements[i] = (loom_aie2p_tile_storage_placement_t){
        .storage_space = domain->storage_space,
        .load_address = placement->load_address,
    };
  }

  const loom_xdna_tile_coordinate_t coordinate =
      plan->worker_plans[worker_index].coordinate;
  const loom_xdna_tile_facts_t* tile =
      loom_xdna_array_tile_facts(plan->family, coordinate);
  *out_layout = (loom_aie2p_tile_link_layout_t){
      .program_address = tile->memory.program_base,
      .program_byte_capacity = tile->memory.program_capacity,
      .storage_placements = storage_placements,
      .storage_placement_count = realization->storage_domain_count,
  };
}

static iree_status_t loom_aie2p_xdna_compile_resident_tiles(
    const loom_xdna_product_prepare_request_t* request,
    loom_module_t* resident_module, const loom_aie2p_array_plan_t* plan,
    const loom_aie2p_array_resident_program_t* resident_program,
    loom_aie2p_xdna_tile_t** out_tiles, bool* out_compiled) {
  *out_compiled = false;
  *out_tiles = NULL;
  loom_aie2p_xdna_tile_t* tiles = NULL;
  loom_aie2p_leaf_contribution_t* contributions = NULL;
  loom_aie2p_linked_tile_t* linked_tiles = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      request->scratch_arena, resident_program->worker_count, sizeof(*tiles),
      (void**)&tiles));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      request->scratch_arena, resident_program->worker_count,
      sizeof(*contributions), (void**)&contributions));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      request->scratch_arena, resident_program->worker_count,
      sizeof(*linked_tiles), (void**)&linked_tiles));

  for (iree_host_size_t i = 0; i < resident_program->worker_count; ++i) {
    const loom_aie2p_array_resident_worker_t* resident =
        &resident_program->workers[i];
    loom_aie2p_leaf_contribution_t* contribution = &contributions[i];
    loom_target_compile_report_t worker_report;
    loom_target_compile_report_t* worker_report_ptr = NULL;
    if (request->compile_report != NULL) {
      loom_target_compile_report_initialize(&worker_report,
                                            request->compile_report->allocator);
      worker_report.requested_detail_flags =
          request->compile_report->requested_detail_flags;
      worker_report_ptr = &worker_report;
    }
    const loom_aie2p_leaf_compile_options_t worker_compile_options = {
        .function_target_facts = resident->function_target_facts,
        .memory_accesses = resident->memory_accesses,
        .descriptor_registry = request->low_descriptor_registry,
        .diagnostic_emitter = request->diagnostic_emitter,
        .compile_report = worker_report_ptr,
    };
    bool leaf_compiled = false;
    iree_status_t status = loom_aie2p_leaf_compile(
        resident_module, resident->function_op, &worker_compile_options,
        request->scratch_arena, &leaf_compiled, contribution);
    if (worker_report_ptr != NULL) {
      status = iree_status_join(
          status, loom_target_compile_report_record_entry_report(
                      request->compile_report, worker_report_ptr));
      loom_target_compile_report_deinitialize(worker_report_ptr);
    }
    IREE_RETURN_IF_ERROR(status);
    if (!leaf_compiled) {
      return iree_ok_status();
    }
    IREE_ASSERT_EQ(contribution->realization.resource_import_count, 0u);
    loom_aie2p_tile_storage_placement_t
        storage_placements[LOOM_STORAGE_SPACE_COUNT_];
    loom_aie2p_tile_link_layout_t link_layout = {0};
    loom_aie2p_xdna_plan_tile_link(plan, resident->worker_index,
                                   &contribution->realization,
                                   storage_placements, &link_layout);
    IREE_RETURN_IF_ERROR(loom_aie2p_tile_link(
        contribution, &link_layout, request->scratch_arena, &linked_tiles[i]));
    const loom_native_object_symbol_t* entry_symbol =
        &contribution->object
             .symbols[contribution->realization.entry_symbol_index];
    tiles[i] = (loom_aie2p_xdna_tile_t){
        .coordinate = plan->worker_plans[resident->worker_index].coordinate,
        .entry_name = entry_symbol->name,
        .entry_byte_length = entry_symbol->size,
        .entry_address = linked_tiles[i].entry_address,
        .sections = linked_tiles[i].assembly.sections,
        .section_count = linked_tiles[i].assembly.section_count,
        .entry_section_index = linked_tiles[i].entry_section_index,
    };
  }
  *out_tiles = tiles;
  *out_compiled = true;
  return iree_ok_status();
}

static iree_status_t loom_aie2p_xdna_compile_resident_entries(
    const loom_xdna_product_prepare_request_t* request,
    loom_module_t* resident_module, const loom_aie2p_array_plan_t* array_plans,
    iree_host_size_t entry_count, loom_aie2p_xdna_entry_t* product_entries,
    bool* out_compiled) {
  *out_compiled = false;
  for (iree_host_size_t i = 0; i < entry_count; ++i) {
    loom_aie2p_array_resident_program_t resident_program = {0};
    IREE_RETURN_IF_ERROR(loom_aie2p_array_materialize_resident_program(
        request->module, resident_module, &array_plans[i],
        request->scratch_arena, &resident_program));
    loom_aie2p_xdna_tile_t* tiles = NULL;
    bool tiles_compiled = false;
    IREE_RETURN_IF_ERROR(loom_aie2p_xdna_compile_resident_tiles(
        request, resident_module, &array_plans[i], &resident_program, &tiles,
        &tiles_compiled));
    if (!tiles_compiled) {
      return iree_ok_status();
    }
    IREE_ASSERT_EQ(resident_program.worker_count,
                   product_entries[i].array_program->tile_program_count);
    product_entries[i].tiles = tiles;
  }
  *out_compiled = true;
  return iree_ok_status();
}

typedef struct loom_aie2p_xdna_resident_storage_t {
  // Host allocator owning this storage record and the resident module.
  iree_allocator_t allocator;
  // Private block pool backing every resident-module arena allocation.
  iree_arena_block_pool_t block_pool;
  // Private module containing materialized resident worker functions.
  loom_module_t* module;
} loom_aie2p_xdna_resident_storage_t;

static void loom_aie2p_xdna_release_resident_storage(void* storage_ptr) {
  loom_aie2p_xdna_resident_storage_t* storage =
      (loom_aie2p_xdna_resident_storage_t*)storage_ptr;
  loom_module_free(storage->module);
  iree_arena_block_pool_deinitialize(&storage->block_pool);
  iree_allocator_free(storage->allocator, storage);
}

static iree_status_t loom_aie2p_xdna_allocate_resident_storage(
    const loom_xdna_product_prepare_request_t* request,
    loom_aie2p_xdna_resident_storage_t** out_storage) {
  *out_storage = NULL;
  const iree_allocator_t allocator = request->compile_report->allocator;
  loom_aie2p_xdna_resident_storage_t* storage = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(allocator, sizeof(*storage), (void**)&storage));
  *storage = (loom_aie2p_xdna_resident_storage_t){
      .allocator = allocator,
  };
  iree_arena_block_pool_initialize(32 * 1024, allocator, &storage->block_pool);
  iree_status_t status = loom_module_allocate(
      request->module->context, IREE_SV("aie2p.xdna.resident"),
      &storage->block_pool, /*hints=*/NULL, allocator, &storage->module);
  if (iree_status_is_ok(status)) {
    *out_storage = storage;
  } else {
    loom_aie2p_xdna_release_resident_storage(storage);
  }
  return status;
}

iree_status_t loom_xdna_product_prepare(
    const loom_xdna_product_prepare_request_t* request, bool* out_prepared,
    loom_aie2p_xdna_product_plan_t* out_plan) {
  IREE_ASSERT_ARGUMENT(request);
  IREE_ASSERT_ARGUMENT(request->module);
  IREE_ASSERT_ARGUMENT(request->entries.values);
  IREE_ASSERT(request->entries.count != 0);
  IREE_ASSERT_ARGUMENT(request->low_descriptor_registry);
  IREE_ASSERT_ARGUMENT(request->scratch_arena);
  IREE_ASSERT_ARGUMENT(out_prepared);
  IREE_ASSERT_ARGUMENT(out_plan);
  *out_prepared = false;
  *out_plan = (loom_aie2p_xdna_product_plan_t){0};

  const loom_xdna_device_profile_t* device_profile = NULL;
  bool device_profile_valid = false;
  IREE_RETURN_IF_ERROR(loom_aie2p_xdna_resolve_device_profile(
      request, &device_profile, &device_profile_valid));
  if (!device_profile_valid) {
    return iree_ok_status();
  }
  if (request->compile_report != NULL) {
    loom_target_compile_report_initialize_if_empty(request->compile_report,
                                                   request->allocator);
  }

  const iree_host_size_t entry_count = request->entries.count;
  loom_aie2p_array_leaf_t* source_leaves = NULL;
  iree_host_size_t source_leaf_count = 0;
  bool source_leaves_valid = false;
  IREE_RETURN_IF_ERROR(loom_aie2p_xdna_collect_source_leaves(
      request, &source_leaves, &source_leaf_count, &source_leaves_valid));
  if (!source_leaves_valid) {
    return iree_ok_status();
  }

  loom_aie2p_array_plan_t* array_plans = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(request->scratch_arena, entry_count,
                                sizeof(*array_plans), (void**)&array_plans));
  loom_aie2p_array_program_t* array_programs = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      request->scratch_arena, entry_count, sizeof(*array_programs),
      (void**)&array_programs));
  loom_aie2p_xdna_entry_t* product_entries = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      request->scratch_arena, entry_count, sizeof(*product_entries),
      (void**)&product_entries));
  for (iree_host_size_t i = 0; i < entry_count; ++i) {
    const loom_target_entry_t* source_entry = &request->entries.values[i];
    if (request->compile_report != NULL) {
      loom_target_compile_report_record_low_kernel_workload(
          request->compile_report, source_entry->func.op);
    }
    bool valid = false;
    IREE_RETURN_IF_ERROR(loom_aie2p_array_plan_build(
        request->module, source_entry->func.op, source_leaves,
        source_leaf_count, request->diagnostic_emitter, request->scratch_arena,
        &array_plans[i], &valid));
    if (!valid) {
      return iree_ok_status();
    }
    IREE_RETURN_IF_ERROR(loom_aie2p_array_program_build(
        &array_plans[i], request->scratch_arena, &array_programs[i]));
    product_entries[i] = (loom_aie2p_xdna_entry_t){
        .name = source_entry->func_name,
        .partition_column_count = array_plans[i].partition_column_count,
        .binding_count = array_plans[i].binding_slot_count,
        .array_program = &array_programs[i],
    };
  }

  const loom_aie2p_xdna_product_t product = {
      .device_profile = device_profile,
      .entries = product_entries,
      .entry_count = entry_count,
  };
  loom_aie2p_xdna_product_preparation_t* product_preparation = NULL;
  loom_aie2p_xdna_product_issue_t product_issue = {0};
  bool product_admitted = false;
  IREE_RETURN_IF_ERROR(loom_aie2p_xdna_product_preparation_begin(
      &product, request->scratch_arena, &product_admitted, &product_preparation,
      &product_issue));
  if (!product_admitted) {
    return loom_aie2p_xdna_emit_product_issue(request, device_profile,
                                              &product_issue);
  }

  loom_module_t* resident_module = NULL;
  iree_status_t status = iree_ok_status();
  if (request->compile_report != NULL) {
    loom_aie2p_xdna_resident_storage_t* resident_storage = NULL;
    status =
        loom_aie2p_xdna_allocate_resident_storage(request, &resident_storage);
    if (iree_status_is_ok(status)) {
      resident_module = resident_storage->module;
      loom_target_compile_report_take_storage(
          request->compile_report, resident_storage,
          loom_aie2p_xdna_release_resident_storage);
    }
  } else {
    status = loom_module_allocate(
        request->module->context, IREE_SV("aie2p.xdna.resident"),
        request->scratch_arena->block_pool, /*hints=*/NULL, request->allocator,
        &resident_module);
  }
  bool resident_entries_compiled = false;
  if (iree_status_is_ok(status)) {
    status = loom_aie2p_xdna_compile_resident_entries(
        request, resident_module, array_plans, entry_count, product_entries,
        &resident_entries_compiled);
  }
  if (request->compile_report == NULL) {
    loom_module_free(resident_module);
  }
  IREE_RETURN_IF_ERROR(status);
  if (!resident_entries_compiled) {
    return iree_ok_status();
  }

  for (iree_host_size_t i = 0; i < entry_count; ++i) {
    iree_xdna_elf_binding_record_t* binding_records = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        request->scratch_arena, array_plans[i].binding_slot_count,
        sizeof(*binding_records), (void**)&binding_records));
    loom_aie2p_array_binding_records_build(&array_plans[i], binding_records);
    product_entries[i].binding_records = binding_records;
    IREE_RETURN_IF_ERROR(loom_aie2p_array_report_record(
        request->module, product_entries[i].name, &array_plans[i],
        product_entries[i].tiles, request->compile_report,
        request->scratch_arena));
  }

  bool product_prepared = false;
  IREE_RETURN_IF_ERROR(loom_aie2p_xdna_product_preparation_finish(
      product_preparation, &product_prepared, out_plan, &product_issue));
  if (!product_prepared) {
    return loom_aie2p_xdna_emit_product_issue(request, device_profile,
                                              &product_issue);
  }
  *out_prepared = true;
  return iree_ok_status();
}
