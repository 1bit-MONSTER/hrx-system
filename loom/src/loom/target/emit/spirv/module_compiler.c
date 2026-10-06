// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/spirv/module_compiler.h"

#include <stdint.h>

#include "loom/analysis/symbol_facts.h"
#include "loom/codegen/low/diagnostics.h"
#include "loom/codegen/low/function.h"
#include "loom/codegen/low/target_binding.h"
#include "loom/error/error_catalog.h"
#include "loom/ir/module.h"
#include "loom/ops/op_defs.h"
#include "loom/target/arch/spirv/descriptors/descriptors.h"
#include "loom/target/arch/spirv/module_contract.h"
#include "loom/target/emit/spirv/module_emitter.h"
#include "loom/target/emit/spirv/program.h"
#include "loom/target/function_version.h"
#include "loom/target/reporting/artifact_manifest_collect.h"

typedef enum loom_spirv_program_function_disposition_e {
  // Function belongs to another target and is not part of this program.
  LOOM_SPIRV_PROGRAM_FUNCTION_SKIPPED = 0,
  // Function was appended to the program plan.
  LOOM_SPIRV_PROGRAM_FUNCTION_APPENDED = 1,
  // Function target resolution emitted a structured diagnostic.
  LOOM_SPIRV_PROGRAM_FUNCTION_REJECTED = 2,
} loom_spirv_program_function_disposition_t;

typedef struct loom_spirv_program_build_t {
  // Module containing the target-low functions.
  loom_module_t* module;
  // Low representation registry used during target binding.
  const loom_low_descriptor_registry_t* descriptor_registry;
  // Structured diagnostic emitter for target-resolution failures.
  iree_diagnostic_emitter_t diagnostic_emitter;
  // Cached symbol facts shared by target resolution for every function.
  loom_symbol_fact_table_t symbol_facts;
  // Compiler function versions observed against this module symbol snapshot.
  loom_target_function_version_snapshot_t function_versions;
  // Mutable planned function storage.
  loom_spirv_function_plan_t* functions;
  // Number of initialized entries in |functions|.
  iree_host_size_t function_count;
  // Capacity of |functions|.
  iree_host_size_t function_capacity;
  // Module contract established by the first planned function.
  loom_spirv_module_contract_t contract;
  // Whether |contract| has been initialized.
  bool has_contract;
} loom_spirv_program_build_t;

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

static iree_status_t loom_spirv_program_build_function(
    loom_spirv_program_build_t* build, loom_op_t* function_op,
    const loom_target_facts_t* selected_target_facts,
    loom_spirv_program_function_disposition_t* out_disposition) {
  *out_disposition = LOOM_SPIRV_PROGRAM_FUNCTION_SKIPPED;
  if (!loom_low_function_def_isa(function_op)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SPIR-V program planning requires a Low function "
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
    *out_disposition = LOOM_SPIRV_PROGRAM_FUNCTION_REJECTED;
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
    *out_disposition = LOOM_SPIRV_PROGRAM_FUNCTION_REJECTED;
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_spirv_program_validate_contract(build, &target));

  IREE_ASSERT_LT(build->function_count, build->function_capacity);
  build->functions[build->function_count++] = (loom_spirv_function_plan_t){
      .function_op = function_op,
      .target_facts = target.target_facts,
      .descriptor_set = target.descriptor_set,
  };
  *out_disposition = LOOM_SPIRV_PROGRAM_FUNCTION_APPENDED;
  return iree_ok_status();
}

static iree_status_t loom_spirv_program_plan_build(
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

  const iree_host_size_t function_capacity =
      loom_spirv_program_candidate_count(module, options);
  if (function_capacity == 0) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "SPIR-V program planning requires at least one Low function "
        "definition");
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
  if (options == NULL || options->entry_count == 0) {
    IREE_RETURN_IF_ERROR(loom_target_function_version_snapshot_build(
        module, options != NULL ? options->function_versions : NULL, arena,
        &build.function_versions));
  }

  if (options != NULL && options->entry_count != 0) {
    for (iree_host_size_t i = 0; i < options->entry_count; ++i) {
      loom_spirv_program_function_disposition_t disposition =
          LOOM_SPIRV_PROGRAM_FUNCTION_SKIPPED;
      IREE_RETURN_IF_ERROR(loom_spirv_program_build_function(
          &build, options->entries[i].function_op,
          options->entries[i].target_facts, &disposition));
      if (disposition == LOOM_SPIRV_PROGRAM_FUNCTION_REJECTED) {
        return iree_ok_status();
      }
    }
  } else {
    loom_symbol_t* symbol = NULL;
    loom_module_for_each_symbol(module, symbol) {
      if (!loom_low_function_def_isa(symbol->defining_op)) {
        continue;
      }
      loom_spirv_program_function_disposition_t disposition =
          LOOM_SPIRV_PROGRAM_FUNCTION_SKIPPED;
      IREE_RETURN_IF_ERROR(loom_spirv_program_build_function(
          &build, symbol->defining_op, /*selected_target_facts=*/NULL,
          &disposition));
      if (disposition == LOOM_SPIRV_PROGRAM_FUNCTION_REJECTED) {
        return iree_ok_status();
      }
    }
  }

  if (build.function_count == 0) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "SPIR-V program planning found no compatible Low function "
        "definitions");
  }
  *out_plan = (loom_spirv_program_plan_t){
      .module = module,
      .functions = build.functions,
      .function_count = build.function_count,
  };
  *out_accepted = true;
  return iree_ok_status();
}

static iree_status_t loom_spirv_program_compile(
    loom_module_t* module,
    const loom_low_descriptor_registry_t* descriptor_registry,
    iree_diagnostic_emitter_t diagnostic_emitter, iree_arena_allocator_t* arena,
    const loom_spirv_compile_options_t* options, iree_allocator_t allocator,
    bool* out_emitted, loom_spirv_program_plan_t* out_program,
    loom_spirv_module_binary_t* out_module) {
  *out_emitted = false;
  *out_program = (loom_spirv_program_plan_t){0};
  *out_module = (loom_spirv_module_binary_t){0};

  bool accepted = false;
  IREE_RETURN_IF_ERROR(loom_spirv_program_plan_build(
      module, descriptor_registry, diagnostic_emitter, arena, options,
      &accepted, out_program));
  if (!accepted) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_spirv_program_emit_binary(out_program, arena,
                                                      out_module, allocator));
  *out_emitted = true;
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
  return loom_spirv_program_compile(
      module, descriptor_registry, diagnostic_emitter, arena, options,
      allocator, out_emitted, &program, out_module);
}

typedef struct loom_spirv_module_artifact_storage_t {
  // Host allocator owning this storage.
  iree_allocator_t allocator;
  // Artifact manifest sidecar descriptor.
  loom_target_emit_sidecar_artifact_t artifact_manifest;
} loom_spirv_module_artifact_storage_t;

static void loom_spirv_module_artifact_storage_release(void* storage) {
  loom_spirv_module_artifact_storage_t* artifact_storage =
      (loom_spirv_module_artifact_storage_t*)storage;
  iree_allocator_free(artifact_storage->allocator, artifact_storage);
}

static iree_status_t loom_spirv_program_collect_manifest_entries(
    const loom_spirv_program_plan_t* program, iree_arena_allocator_t* arena,
    loom_target_entry_list_t* out_entries) {
  *out_entries = (loom_target_entry_list_t){0};
  if (program->function_count > UINT16_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "SPIR-V artifact manifest has too many functions");
  }

  loom_target_entry_t* entries = NULL;
  if (program->function_count != 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        arena, program->function_count, sizeof(*entries), (void**)&entries));
  }
  for (iree_host_size_t i = 0; i < program->function_count; ++i) {
    const loom_spirv_function_plan_t* function_plan = &program->functions[i];
    const loom_func_like_t function =
        loom_func_like_cast(program->module, function_plan->function_op);
    const loom_symbol_ref_t function_ref =
        loom_low_function_callee(function_plan->function_op);
    entries[i] = (loom_target_entry_t){
        .func = function,
        .func_name =
            loom_low_diagnostic_symbol_name(program->module, function_ref),
        .func_ref = function_ref,
        .target_facts = function_plan->target_facts,
    };
  }
  *out_entries = (loom_target_entry_list_t){
      .values = entries,
      .count = (uint16_t)program->function_count,
  };
  return iree_ok_status();
}

static iree_status_t loom_spirv_module_attach_artifact_manifest(
    const loom_target_emit_request_t* request,
    const loom_spirv_program_plan_t* program,
    loom_target_emit_artifact_t* artifact) {
  if (request->artifact_manifest.mode ==
      LOOM_TARGET_ARTIFACT_MANIFEST_MODE_NONE) {
    return iree_ok_status();
  }

  loom_target_entry_list_t entries = {0};
  IREE_RETURN_IF_ERROR(loom_spirv_program_collect_manifest_entries(
      program, request->scratch_arena, &entries));
  loom_target_artifact_manifest_collect_options_t manifest_options;
  loom_target_artifact_manifest_collect_options_initialize(&manifest_options);
  manifest_options.mode = request->artifact_manifest.mode;
  manifest_options.artifact_name = request->identifier;
  manifest_options.artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_SPIRV_BINARY;
  manifest_options.flags =
      LOOM_TARGET_ARTIFACT_MANIFEST_COLLECT_FLAG_ARTIFACT_BYTE_LENGTH;
  manifest_options.artifact_byte_length =
      iree_byte_sequence_length(artifact->contents);

  loom_target_artifact_manifest_json_t manifest_json = {0};
  iree_status_t status =
      loom_target_artifact_manifest_collect_json_from_entries(
          program->module, entries, &manifest_options, request->scratch_arena,
          request->allocator, &manifest_json);
  IREE_ASSERT(!iree_status_is_ok(status) ||
              manifest_json.contents.data != NULL);

  iree_byte_sequence_t* manifest_contents = NULL;
  if (iree_status_is_ok(status)) {
    iree_byte_span_t contents =
        iree_make_byte_span((uint8_t*)manifest_json.contents.data,
                            manifest_json.contents.data_length);
    status = iree_byte_sequence_create_from_span_move(
        &contents, request->allocator, &manifest_contents);
    if (iree_status_is_ok(status)) {
      manifest_json.contents = iree_const_byte_span_empty();
    }
  }

  loom_spirv_module_artifact_storage_t* storage = NULL;
  if (iree_status_is_ok(status)) {
    status = iree_allocator_malloc(request->allocator, sizeof(*storage),
                                   (void**)&storage);
  }
  if (iree_status_is_ok(status)) {
    *storage = (loom_spirv_module_artifact_storage_t){
        .allocator = request->allocator,
        .artifact_manifest =
            {
                .kind =
                    LOOM_TARGET_EMIT_SIDECAR_ARTIFACT_KIND_ARTIFACT_MANIFEST,
                .identifier = request->artifact_manifest.identifier,
                .contents = manifest_contents,
            },
    };
    artifact->sidecars = &storage->artifact_manifest;
    artifact->sidecar_count = 1;
    artifact->storage = storage;
    artifact->release_storage = loom_spirv_module_artifact_storage_release;
    manifest_contents = NULL;
    storage = NULL;
  }

  iree_byte_sequence_release(manifest_contents);
  loom_target_artifact_manifest_json_release(&manifest_json,
                                             request->allocator);
  iree_allocator_free(request->allocator, storage);
  return status;
}

iree_status_t loom_spirv_compile_module_artifact(
    const loom_target_emit_request_t* request,
    const loom_spirv_compile_options_t* options, bool* out_emitted,
    loom_target_emit_artifact_t* out_artifact) {
  *out_emitted = false;
  *out_artifact = (loom_target_emit_artifact_t){0};

  loom_spirv_program_plan_t program = {0};
  loom_spirv_module_binary_t binary = {0};
  bool module_emitted = false;
  iree_status_t status = loom_spirv_program_compile(
      request->module, request->low_descriptor_registry,
      request->diagnostic_emitter, request->scratch_arena, options,
      request->allocator, &module_emitted, &program, &binary);
  loom_target_emit_artifact_t artifact = {0};
  if (iree_status_is_ok(status) && module_emitted) {
    iree_byte_span_t contents =
        iree_make_byte_span(binary.words, binary.word_count * sizeof(uint32_t));
    status = iree_byte_sequence_create_from_span_move(
        &contents, request->allocator, &artifact.contents);
    if (iree_status_is_ok(status)) {
      binary.words = NULL;
      binary.word_count = 0;
      artifact.target_artifact_format =
          LOOM_TARGET_ARTIFACT_FORMAT_SPIRV_BINARY;
    }
  }
  if (iree_status_is_ok(status) && module_emitted) {
    status = loom_spirv_module_attach_artifact_manifest(request, &program,
                                                        &artifact);
  }
  if (iree_status_is_ok(status) && module_emitted &&
      request->compile_report != NULL) {
    loom_target_compile_report_initialize_if_empty(request->compile_report,
                                                   request->allocator);
    const loom_target_bundle_t* target_bundle =
        loom_spirv_function_plan_target_bundle(&program.functions[0]);
    request->compile_report->artifact_kind =
        target_bundle->export_plan->abi_kind == LOOM_TARGET_ABI_HAL_KERNEL
            ? LOOM_TARGET_COMPILE_ARTIFACT_KIND_HAL_EXECUTABLE
            : LOOM_TARGET_COMPILE_ARTIFACT_KIND_TARGET_ARTIFACT;
    request->compile_report->target_family_name =
        program.functions[0].target_facts->fact_type->name;
    loom_target_compile_report_record_target_bundle(request->compile_report,
                                                    target_bundle);
  }
  if (iree_status_is_ok(status) && module_emitted) {
    *out_artifact = artifact;
    artifact = (loom_target_emit_artifact_t){0};
    *out_emitted = true;
  }

  loom_target_emit_artifact_release(&artifact);
  loom_spirv_module_binary_deinitialize(&binary, request->allocator);
  return status;
}

static iree_status_t loom_spirv_module_emit(
    const loom_target_emit_request_t* request, bool* out_emitted,
    loom_target_emit_artifact_t* out_artifact) {
  const loom_spirv_compile_options_t options = {
      .function_versions = request->function_versions,
  };
  return loom_spirv_compile_module_artifact(request, &options, out_emitted,
                                            out_artifact);
}

const loom_target_emitter_t loom_spirv_module_emitter = {
    .name = IREE_SVL("spirv"),
    .public_artifact_format = IREE_SVL("spirv"),
    .default_identifier = IREE_SVL("module.spv"),
    .target_artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_SPIRV_BINARY,
    .default_pipeline_options =
        {
            .control_flow_lowering =
                LOOM_TARGET_CONTROL_FLOW_LOWERING_STRUCTURED_LOW,
        },
    .emit = loom_spirv_module_emit,
};
