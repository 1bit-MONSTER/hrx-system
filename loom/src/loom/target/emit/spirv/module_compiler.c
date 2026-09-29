// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/spirv/module_compiler.h"

#include "loom/analysis/symbol_facts.h"
#include "loom/codegen/low/diagnostics.h"
#include "loom/codegen/low/function.h"
#include "loom/codegen/low/target_binding.h"
#include "loom/error/error_catalog.h"
#include "loom/ir/module.h"
#include "loom/target/arch/spirv/descriptors/descriptors.h"
#include "loom/target/arch/spirv/module_contract.h"
#include "loom/target/emit/spirv/module_emitter.h"
#include "loom/target/emit/spirv/program.h"
#include "loom/target/function_version.h"

typedef enum loom_spirv_prepare_function_disposition_e {
  // Function belongs to another target and is not part of this program.
  LOOM_SPIRV_PREPARE_FUNCTION_SKIPPED = 0,
  // Function was appended to the prepared program.
  LOOM_SPIRV_PREPARE_FUNCTION_APPENDED = 1,
  // Function target resolution emitted a structured diagnostic.
  LOOM_SPIRV_PREPARE_FUNCTION_REJECTED = 2,
} loom_spirv_prepare_function_disposition_t;

typedef struct loom_spirv_program_build_t {
  // Module containing the prepared Low functions.
  loom_module_t* module;
  // Low representation registry used during target binding.
  const loom_low_descriptor_registry_t* descriptor_registry;
  // Structured diagnostic emitter for target-resolution failures.
  iree_diagnostic_emitter_t diagnostic_emitter;
  // Cached symbol facts shared by target resolution for every function.
  loom_symbol_fact_table_t symbol_facts;
  // Compiler function versions observed against this module symbol snapshot.
  loom_target_function_version_snapshot_t function_versions;
  // Mutable prepared function storage.
  loom_spirv_function_plan_t* functions;
  // Number of initialized entries in |functions|.
  iree_host_size_t function_count;
  // Capacity of |functions|.
  iree_host_size_t function_capacity;
  // Module contract established by the first prepared function.
  loom_spirv_module_contract_t contract;
  // Whether |contract| has been initialized.
  bool has_contract;
} loom_spirv_program_build_t;

void loom_spirv_compile_options_initialize(
    loom_spirv_compile_options_t* out_options) {
  IREE_ASSERT_ARGUMENT(out_options);
  *out_options = (loom_spirv_compile_options_t){0};
}

static iree_status_t loom_spirv_compile_options_validate(
    const loom_spirv_compile_options_t* options) {
  if (options == NULL || options->entry_count == 0) {
    return iree_ok_status();
  }
  if (options->entries == NULL) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "selected SPIR-V entries require a compile entry table");
  }
  for (iree_host_size_t i = 0; i < options->entry_count; ++i) {
    if (options->entries[i].function_op == NULL) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "selected SPIR-V entry table contains a null function");
    }
  }
  return iree_ok_status();
}

static iree_host_size_t loom_spirv_program_candidate_count(
    const loom_module_t* module, const loom_spirv_compile_options_t* options) {
  if (options != NULL && options->entry_count != 0) {
    return options->entry_count;
  }
  iree_host_size_t count = 0;
  const loom_symbol_t* symbol = NULL;
  loom_module_for_each_symbol(module, symbol) {
    if (loom_low_function_def_isa(symbol->defining_op)) {
      ++count;
    }
  }
  return count;
}

static iree_status_t loom_spirv_program_validate_target(
    const loom_low_resolved_target_t* target) {
  if (target->descriptor_set->stable_id !=
      SPIRV_LOGICAL_CORE_DESCRIPTOR_SET_ID) {
    const iree_string_view_t descriptor_set_key =
        loom_low_descriptor_set_string(target->descriptor_set,
                                       target->descriptor_set->key_string_ref);
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "verified SPIR-V Low function selected descriptor set '%.*s'; "
        "expected 'spirv.logical.core'",
        (int)descriptor_set_key.size, descriptor_set_key.data);
  }
  return iree_ok_status();
}

static iree_status_t loom_spirv_program_reject_missing_target(
    loom_spirv_program_build_t* build, loom_op_t* function_op) {
  const loom_diagnostic_param_t params[] = {
      loom_param_string(IREE_SV("SPIR-V")),
      loom_param_string(
          loom_low_diagnostic_function_name(build->module, function_op)),
  };
  const loom_diagnostic_emission_t emission = {
      .op = function_op,
      .error = LOOM_ERR_TARGET_009,
      .params = params,
      .param_count = IREE_ARRAYSIZE(params),
  };
  return iree_diagnostic_emit(build->diagnostic_emitter, &emission);
}

static iree_status_t loom_spirv_program_validate_contract(
    loom_spirv_program_build_t* build,
    const loom_low_resolved_target_t* target) {
  const loom_spirv_module_contract_t contract = loom_spirv_module_contract_make(
      target->target_name, loom_low_resolved_target_bundle(target),
      target->descriptor_set->stable_id, target->descriptor_set_key,
      target->feature_bits);
  if (!build->has_contract) {
    build->contract = contract;
    build->has_contract = true;
    return iree_ok_status();
  }
  if (!loom_spirv_module_contract_equal(&build->contract, &contract)) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "verified SPIR-V Low module mixes target contract '%.*s' with "
        "target contract '%.*s'",
        (int)build->contract.target_name.size, build->contract.target_name.data,
        (int)contract.target_name.size, contract.target_name.data);
  }
  return iree_ok_status();
}

static const loom_target_facts_t* loom_spirv_program_function_target_facts(
    const loom_spirv_program_build_t* build, loom_op_t* function_op,
    const loom_target_facts_t* selected_target_facts) {
  if (selected_target_facts != NULL) {
    return selected_target_facts;
  }
  const loom_symbol_ref_t function_ref = loom_low_function_callee(function_op);
  const loom_target_function_version_t* function_version =
      loom_target_function_version_snapshot_at(&build->function_versions,
                                               function_ref.symbol_id);
  return function_version != NULL ? function_version->function_target_facts
                                  : NULL;
}

static bool loom_spirv_program_needs_function_versions(
    const loom_spirv_compile_options_t* options) {
  if (options == NULL || options->entry_count == 0) {
    return true;
  }
  for (iree_host_size_t i = 0; i < options->entry_count; ++i) {
    if (options->entries[i].target_facts == NULL) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_spirv_program_prepare_function(
    loom_spirv_program_build_t* build, loom_op_t* function_op,
    const loom_target_facts_t* selected_target_facts,
    loom_spirv_prepare_function_disposition_t* out_disposition) {
  *out_disposition = LOOM_SPIRV_PREPARE_FUNCTION_SKIPPED;
  if (!loom_low_function_def_isa(function_op)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SPIR-V preparation requires a Low function "
                            "definition");
  }

  const loom_target_facts_t* function_target_facts =
      loom_spirv_program_function_target_facts(build, function_op,
                                               selected_target_facts);
  loom_low_resolved_target_t target = {0};
  IREE_RETURN_IF_ERROR(loom_low_resolve_function_target(
      build->module, &build->symbol_facts, function_op, function_target_facts,
      build->descriptor_registry, build->diagnostic_emitter, &target));
  if (target.descriptor_set == NULL) {
    *out_disposition = LOOM_SPIRV_PREPARE_FUNCTION_REJECTED;
    return iree_ok_status();
  }

  const loom_target_bundle_t* target_bundle =
      loom_low_resolved_target_bundle(&target);
  if (target_bundle != NULL && target_bundle->snapshot->codegen_format !=
                                   LOOM_TARGET_CODEGEN_FORMAT_SPIRV) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_spirv_program_validate_target(&target));
  if (target_bundle == NULL) {
    IREE_RETURN_IF_ERROR(
        loom_spirv_program_reject_missing_target(build, function_op));
    *out_disposition = LOOM_SPIRV_PREPARE_FUNCTION_REJECTED;
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_spirv_program_validate_contract(build, &target));

  IREE_ASSERT_LT(build->function_count, build->function_capacity);
  build->functions[build->function_count++] = (loom_spirv_function_plan_t){
      .function_op = function_op,
      .target_bundle = target_bundle,
      .descriptor_set = target.descriptor_set,
  };
  *out_disposition = LOOM_SPIRV_PREPARE_FUNCTION_APPENDED;
  return iree_ok_status();
}

static iree_status_t loom_spirv_program_plan_prepare(
    loom_module_t* module,
    const loom_low_descriptor_registry_t* descriptor_registry,
    iree_diagnostic_emitter_t diagnostic_emitter, iree_arena_allocator_t* arena,
    const loom_spirv_compile_options_t* options, bool* out_accepted,
    loom_spirv_program_plan_t* out_plan) {
  IREE_ASSERT_ARGUMENT(module);
  IREE_ASSERT_ARGUMENT(descriptor_registry);
  IREE_ASSERT_ARGUMENT(arena);
  IREE_ASSERT_ARGUMENT(out_accepted);
  IREE_ASSERT_ARGUMENT(out_plan);
  *out_accepted = false;
  *out_plan = (loom_spirv_program_plan_t){0};
  IREE_RETURN_IF_ERROR(loom_spirv_compile_options_validate(options));

  const iree_host_size_t function_capacity =
      loom_spirv_program_candidate_count(module, options);
  if (function_capacity == 0) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "SPIR-V preparation requires at least one Low function definition");
  }
  loom_spirv_function_plan_t* functions = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, function_capacity, sizeof(*functions), (void**)&functions));

  loom_spirv_program_build_t build = {
      .module = module,
      .descriptor_registry = descriptor_registry,
      .diagnostic_emitter = diagnostic_emitter,
      .functions = functions,
      .function_capacity = function_capacity,
  };
  loom_symbol_fact_table_initialize(&build.symbol_facts, arena);
  if (loom_spirv_program_needs_function_versions(options)) {
    IREE_RETURN_IF_ERROR(loom_target_function_version_snapshot_build(
        module, options != NULL ? options->function_versions : NULL, arena,
        &build.function_versions));
  }

  if (options != NULL && options->entry_count != 0) {
    for (iree_host_size_t i = 0; i < options->entry_count; ++i) {
      loom_spirv_prepare_function_disposition_t disposition =
          LOOM_SPIRV_PREPARE_FUNCTION_SKIPPED;
      IREE_RETURN_IF_ERROR(loom_spirv_program_prepare_function(
          &build, options->entries[i].function_op,
          options->entries[i].target_facts, &disposition));
      if (disposition == LOOM_SPIRV_PREPARE_FUNCTION_REJECTED) {
        return iree_ok_status();
      }
    }
  } else {
    loom_symbol_t* symbol = NULL;
    loom_module_for_each_symbol(module, symbol) {
      if (!loom_low_function_def_isa(symbol->defining_op)) {
        continue;
      }
      loom_spirv_prepare_function_disposition_t disposition =
          LOOM_SPIRV_PREPARE_FUNCTION_SKIPPED;
      IREE_RETURN_IF_ERROR(loom_spirv_program_prepare_function(
          &build, symbol->defining_op, /*selected_target_facts=*/NULL,
          &disposition));
      if (disposition == LOOM_SPIRV_PREPARE_FUNCTION_REJECTED) {
        return iree_ok_status();
      }
    }
  }

  if (build.function_count == 0) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "SPIR-V preparation found no compatible Low function definitions");
  }
  *out_plan = (loom_spirv_program_plan_t){
      .module = module,
      .functions = build.functions,
      .function_count = build.function_count,
  };
  *out_accepted = true;
  return iree_ok_status();
}

iree_status_t loom_spirv_compile_module_binary(
    loom_module_t* module,
    const loom_low_descriptor_registry_t* descriptor_registry,
    iree_diagnostic_emitter_t diagnostic_emitter, iree_arena_allocator_t* arena,
    const loom_spirv_compile_options_t* options, iree_allocator_t allocator,
    bool* out_emitted, loom_spirv_module_binary_t* out_module) {
  IREE_ASSERT_ARGUMENT(out_emitted);
  IREE_ASSERT_ARGUMENT(out_module);
  *out_emitted = false;
  *out_module = (loom_spirv_module_binary_t){0};

  loom_spirv_program_plan_t program = {0};
  bool accepted = false;
  IREE_RETURN_IF_ERROR(loom_spirv_program_plan_prepare(
      module, descriptor_registry, diagnostic_emitter, arena, options,
      &accepted, &program));
  if (!accepted) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      loom_spirv_program_emit_binary(&program, arena, out_module, allocator));
  *out_emitted = true;
  return iree_ok_status();
}
