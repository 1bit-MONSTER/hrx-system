// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/spirv/artifact_provider.h"

#include "loom/target/arch/spirv/descriptors/low_registry.h"
#include "loom/target/arch/spirv/profile.h"
#include "loom/target/emit/spirv/module_compiler.h"
#include "loom/target/entry_selection.h"
#include "loom/target/function_contract.h"

typedef struct loom_spirv_compile_artifact_storage_t {
  // Target-owned artifact and sidecar storage.
  loom_target_emit_artifact_t target_artifact;
  // Durable target bundle resolved from the emitted entry.
  loom_target_bundle_storage_t target_bundle_storage;
} loom_spirv_compile_artifact_storage_t;

static void loom_spirv_compile_artifact_storage_free(
    loom_spirv_compile_artifact_storage_t* storage,
    iree_allocator_t allocator) {
  if (storage == NULL) {
    return;
  }
  loom_target_emit_artifact_release(&storage->target_artifact);
  iree_allocator_free(allocator, storage);
}

static bool loom_spirv_artifact_provider_bundle_is_compatible(
    void* user_data, const loom_target_entry_t* entry) {
  const loom_target_bundle_t* selected_bundle =
      (const loom_target_bundle_t*)user_data;
  const loom_target_bundle_t* bundle = loom_target_entry_bundle(entry);
  const loom_target_snapshot_t* snapshot = bundle->snapshot;
  const loom_target_export_plan_t* export_plan = bundle->export_plan;
  return snapshot != NULL && export_plan != NULL &&
         snapshot->codegen_format == LOOM_TARGET_CODEGEN_FORMAT_SPIRV &&
         snapshot->artifact_format ==
             LOOM_TARGET_ARTIFACT_FORMAT_SPIRV_BINARY &&
         export_plan->abi_kind == LOOM_TARGET_ABI_HAL_KERNEL &&
         (selected_bundle == NULL ||
          loom_target_function_contract_bundles_compatible(bundle,
                                                           selected_bundle));
}

static iree_status_t loom_spirv_artifact_provider_emit_entries(
    loom_module_t* module, const loom_target_entry_options_t* target_options,
    loom_target_entry_list_t entries, const loom_artifact_target_t* target,
    loom_target_entry_diagnostic_emitter_t* diagnostic_emitter,
    const loom_target_low_descriptor_registry_t* low_registry,
    const loom_compile_artifact_manifest_options_t* artifact_manifest,
    loom_target_compile_report_t* compile_report, iree_arena_allocator_t* arena,
    iree_allocator_t allocator, bool* out_emitted,
    loom_artifact_t* out_artifact) {
  *out_emitted = false;

  loom_spirv_compile_entry_t* compile_entries = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, entries.count,
                                                 sizeof(*compile_entries),
                                                 (void**)&compile_entries));
  for (uint16_t i = 0; i < entries.count; ++i) {
    compile_entries[i] = (loom_spirv_compile_entry_t){
        .function_op = entries.values[i].func.op,
        .target_facts = entries.values[i].target_facts,
    };
  }
  loom_spirv_compile_options_t compile_options = {0};
  compile_options.function_versions = target_options->function_versions;
  compile_options.entries = compile_entries;
  compile_options.entry_count = entries.count;

  loom_spirv_compile_artifact_storage_t* storage = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(allocator, sizeof(*storage), (void**)&storage));
  *storage = (loom_spirv_compile_artifact_storage_t){0};

  const loom_target_emit_request_t request = {
      .low_descriptor_registry = &low_registry->registry,
      .module = module,
      .function_versions = target_options->function_versions,
      .identifier = artifact_manifest->artifact_name,
      .artifact_manifest =
          {
              .mode = artifact_manifest->mode,
              .identifier = artifact_manifest->identifier,
          },
      .compile_report = compile_report,
      .diagnostic_emitter = loom_target_entry_emitter(diagnostic_emitter),
      .scratch_arena = arena,
      .allocator = allocator,
  };
  bool module_emitted = false;
  iree_status_t status = loom_spirv_compile_module_artifact(
      &request, &compile_options, &module_emitted, &storage->target_artifact);
  if (iree_status_is_ok(status) && module_emitted) {
    storage->target_bundle_storage = entries.values[0].target_facts->storage;
    loom_target_bundle_storage_rebind(&storage->target_bundle_storage);
  }
  if (iree_status_is_ok(status) && module_emitted) {
    *out_artifact = (loom_artifact_t){
        .target_bundle = &storage->target_bundle_storage.bundle,
        .target_artifact_format =
            storage->target_artifact.target_artifact_format,
        .target_artifact_data = storage->target_artifact.contents,
        .sidecars = storage->target_artifact.sidecars,
        .sidecar_count = storage->target_artifact.sidecar_count,
        .executable_data = storage->target_artifact.contents,
        .storage = storage,
    };
    *out_emitted = true;
  } else {
    loom_spirv_compile_artifact_storage_free(storage, allocator);
  }
  return status;
}

static iree_status_t loom_spirv_artifact_provider_emit_artifact(
    const loom_artifact_provider_t* provider, loom_module_t* module,
    const loom_artifact_target_t* target, const loom_compile_options_t* options,
    iree_allocator_t allocator, bool* out_emitted,
    loom_artifact_t* out_artifact) {
  IREE_ASSERT_ARGUMENT(provider);
  IREE_ASSERT_ARGUMENT(module);
  IREE_ASSERT_ARGUMENT(target);
  IREE_ASSERT_ARGUMENT(out_emitted);
  IREE_ASSERT_ARGUMENT(out_artifact);

  *out_emitted = false;
  *out_artifact = (loom_artifact_t){0};

  const loom_target_entry_options_t target_options = {
      .function_versions = options->function_versions,
      .diagnostic_sink = options->diagnostic_sink,
      .source_resolver = options->source_resolver,
      .max_errors = options->max_errors,
  };
  loom_target_entry_diagnostic_emitter_t diagnostic_emitter = {0};
  loom_target_entry_diagnostic_emitter_initialize(
      module, &target_options, LOOM_EMITTER_VERIFIER, &diagnostic_emitter);

  iree_arena_block_pool_t block_pool;
  iree_arena_block_pool_initialize(32 * 1024, allocator, &block_pool);
  iree_arena_allocator_t arena;
  iree_arena_initialize(&block_pool, &arena);

  loom_target_low_descriptor_registry_t low_registry = {0};
  loom_spirv_low_descriptor_registry_initialize(&low_registry);

  const loom_target_entry_predicate_t entry_predicate = {
      .fn = loom_spirv_artifact_provider_bundle_is_compatible,
      .user_data = (void*)loom_artifact_target_bundle(target),
  };
  loom_target_entry_list_t entries = {0};
  bool selected = false;
  iree_status_t status = loom_target_entry_select_all_entries(
      module, &target_options, entry_predicate, &diagnostic_emitter,
      IREE_SV("SPIR-V Vulkan HAL"), &arena, &selected, &entries);
  if (iree_status_is_ok(status) && selected &&
      diagnostic_emitter.error_count == 0) {
    status = loom_spirv_artifact_provider_emit_entries(
        module, &target_options, entries, target, &diagnostic_emitter,
        &low_registry, &options->artifact_manifest, options->report, &arena,
        allocator, out_emitted, out_artifact);
  }

  iree_arena_deinitialize(&arena);
  iree_arena_block_pool_deinitialize(&block_pool);
  return status;
}

static void loom_spirv_artifact_provider_deinitialize_artifact(
    const loom_artifact_provider_t* provider, loom_artifact_t* artifact,
    iree_allocator_t allocator) {
  (void)provider;
  if (artifact == NULL) {
    return;
  }
  if (artifact->storage != NULL) {
    loom_spirv_compile_artifact_storage_t* storage =
        (loom_spirv_compile_artifact_storage_t*)artifact->storage;
    loom_spirv_compile_artifact_storage_free(storage, allocator);
  }
  *artifact = (loom_artifact_t){0};
}

const loom_artifact_provider_t loom_spirv_vulkan_artifact_provider = {
    .name = IREE_SVL("spirv-vulkan-hal"),
    .target_profile_type = &loom_spirv_target_profile_type,
    .target_emitter = &loom_spirv_module_emitter,
    .emit_artifact = loom_spirv_artifact_provider_emit_artifact,
    .deinitialize_artifact = loom_spirv_artifact_provider_deinitialize_artifact,
};
