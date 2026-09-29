// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/amd/xdna/artifact_provider.h"

#include "iree/io/vec_stream.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/amd/xdna/aie2p/descriptors/low_registry.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/xdna_product.h"
#include "loom/target/arch/amd/xdna/aie2p/facts.h"
#include "loom/target/arch/amd/xdna/aie2p/profile.h"
#include "loom/target/arch/amd/xdna/aie2p/records/target_records.h"
#include "loom/target/entry_selection.h"
#include "loom/tooling/target/amd/xdna/product_prepare.h"

static bool loom_xdna_artifact_entry_compatible(
    void* user_data, const loom_target_entry_t* entry) {
  (void)user_data;
  if (!loom_low_func_def_isa(entry->func.op)) {
    return false;
  }
  const loom_aie2p_target_facts_t* target_facts =
      loom_aie2p_target_facts_cast(entry->target_facts);
  const loom_target_bundle_t* bundle = loom_target_entry_bundle(entry);
  return target_facts != NULL &&
         target_facts->base.selector == LOOM_AIE2P_TARGET_KIND_ARRAY &&
         bundle != NULL && bundle->snapshot != NULL &&
         bundle->export_plan != NULL &&
         bundle->snapshot->codegen_format ==
             LOOM_TARGET_CODEGEN_FORMAT_LOW_NATIVE &&
         bundle->snapshot->artifact_format == LOOM_TARGET_ARTIFACT_FORMAT_ELF &&
         bundle->export_plan->abi_kind == LOOM_TARGET_ABI_ARRAY_PROGRAM;
}

static iree_status_t loom_xdna_artifact_emit_contents(
    loom_module_t* module,
    const loom_function_version_list_t* function_versions,
    const loom_low_descriptor_registry_t* low_descriptor_registry,
    const loom_xdna_device_profile_t* device_profile,
    loom_target_compile_report_t* compile_report,
    iree_diagnostic_emitter_t diagnostic_emitter,
    iree_arena_allocator_t* scratch_arena, iree_allocator_t allocator,
    bool* out_emitted, iree_byte_sequence_t** out_contents) {
  *out_emitted = false;
  *out_contents = NULL;

  const loom_target_entry_predicate_t entry_predicate = {
      .fn = loom_xdna_artifact_entry_compatible,
  };
  loom_target_entry_list_t entries = {0};
  bool entries_selected = false;
  IREE_RETURN_IF_ERROR(loom_target_entry_select_all_entries(
      module, function_versions, entry_predicate, diagnostic_emitter,
      IREE_SV("XDNA AIE2P array"), scratch_arena, &entries_selected, &entries));
  if (!entries_selected) {
    return iree_ok_status();
  }

  const loom_xdna_product_prepare_request_t prepare_request = {
      .module = module,
      .entries = entries,
      .function_versions = function_versions,
      .low_descriptor_registry = low_descriptor_registry,
      .device_profile = device_profile,
      .compile_report = compile_report,
      .diagnostic_emitter = diagnostic_emitter,
      .scratch_arena = scratch_arena,
      .allocator = allocator,
  };
  loom_aie2p_xdna_product_plan_t product_plan = {0};
  bool product_prepared = false;
  IREE_RETURN_IF_ERROR(loom_xdna_product_prepare(
      &prepare_request, &product_prepared, &product_plan));
  if (!product_prepared) {
    return iree_ok_status();
  }

  iree_io_stream_t* stream = NULL;
  iree_status_t status = iree_io_vec_stream_create(
      IREE_IO_STREAM_MODE_READABLE | IREE_IO_STREAM_MODE_WRITABLE |
          IREE_IO_STREAM_MODE_SEEKABLE,
      4096, allocator, &stream);
  if (iree_status_is_ok(status)) {
    status = loom_aie2p_xdna_product_write_plan(&product_plan, stream,
                                                scratch_arena);
  }
  if (iree_status_is_ok(status)) {
    IREE_ASSERT_GT(iree_io_stream_length(stream), 0);
    status = iree_io_vec_stream_move_contents(stream, out_contents);
  }
  if (iree_status_is_ok(status)) {
    *out_emitted = true;
  }
  iree_io_stream_release(stream);
  return status;
}

static iree_status_t loom_xdna_artifact_provider_emit_artifact(
    const loom_artifact_provider_t* provider, loom_module_t* module,
    const loom_artifact_target_t* target, const loom_compile_options_t* options,
    iree_allocator_t allocator, bool* out_emitted,
    loom_artifact_t* out_artifact) {
  (void)provider;
  IREE_ASSERT_ARGUMENT(module);
  IREE_ASSERT_ARGUMENT(target);
  IREE_ASSERT_ARGUMENT(options);
  IREE_ASSERT_ARGUMENT(out_emitted);
  IREE_ASSERT_ARGUMENT(out_artifact);
  *out_emitted = false;
  *out_artifact = (loom_artifact_t){0};

  const loom_aie2p_target_profile_t* profile =
      loom_aie2p_target_profile_cast(target->target_profile);
  if (target->target_profile != NULL && profile == NULL) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "XDNA artifact emission requires an AIE2P target profile");
  }
  if (options->artifact_flags != LOOM_COMPILE_ARTIFACT_FLAG_NONE) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "XDNA artifact emission has no debug artifacts");
  }
  if (options->artifact_manifest.mode !=
      LOOM_TARGET_ARTIFACT_MANIFEST_MODE_NONE) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "XDNA artifact emission has no sidecar manifest");
  }

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
  loom_target_low_descriptor_registry_t low_descriptor_registry = {0};
  loom_aie2p_low_descriptor_registry_initialize(&low_descriptor_registry);

  iree_byte_sequence_t* contents = NULL;
  bool artifact_emitted = false;
  iree_status_t status = loom_xdna_artifact_emit_contents(
      module, options->function_versions, &low_descriptor_registry.registry,
      profile != NULL ? profile->device_profile : NULL, options->report,
      loom_target_entry_emitter(&diagnostic_emitter), &arena, allocator,
      &artifact_emitted, &contents);
  if (iree_status_is_ok(status) && artifact_emitted) {
    *out_artifact = (loom_artifact_t){
        .target_key = target->target_key,
        .target_bundle = &loom_aie2p_array_target_bundle,
        .target_artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_ELF,
        .target_artifact_data = contents,
        .executable_data = contents,
        .storage = contents,
    };
    *out_emitted = true;
    contents = NULL;
  }
  iree_byte_sequence_release(contents);
  iree_arena_deinitialize(&arena);
  iree_arena_block_pool_deinitialize(&block_pool);
  return status;
}

static void loom_xdna_artifact_provider_deinitialize_artifact(
    const loom_artifact_provider_t* provider, loom_artifact_t* artifact,
    iree_allocator_t allocator) {
  (void)provider;
  (void)allocator;
  if (artifact == NULL) {
    return;
  }
  iree_byte_sequence_release((iree_byte_sequence_t*)artifact->storage);
  *artifact = (loom_artifact_t){0};
}

static iree_status_t loom_xdna_artifact_emitter_emit(
    const loom_target_emit_request_t* request, bool* out_emitted,
    loom_target_emit_artifact_t* out_artifact) {
  *out_emitted = false;
  *out_artifact = (loom_target_emit_artifact_t){0};
  if (request->artifact_manifest.mode !=
      LOOM_TARGET_ARTIFACT_MANIFEST_MODE_NONE) {
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "XDNA entry metadata is embedded in the canonical ELF; sidecar "
        "artifact manifests are not supported");
  }
  IREE_RETURN_IF_ERROR(loom_xdna_artifact_emit_contents(
      request->module, request->function_versions,
      request->low_descriptor_registry, /*device_profile=*/NULL,
      request->compile_report, request->diagnostic_emitter,
      request->scratch_arena, request->allocator, out_emitted,
      &out_artifact->contents));
  if (*out_emitted) {
    out_artifact->target_artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_ELF;
  }
  return iree_ok_status();
}

static const loom_target_emitter_t loom_xdna_artifact_emitter = {
    .name = IREE_SVL("xdna"),
    .public_artifact_format = IREE_SVL("xdna"),
    .default_identifier = IREE_SVL("module.xdna"),
    .target_artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_ELF,
    .emit = loom_xdna_artifact_emitter_emit,
};

static const loom_target_emitter_t* const kXdnaArtifactEmitters[] = {
    &loom_xdna_artifact_emitter,
};

const loom_target_provider_t loom_xdna_artifact_emitter_provider = {
    .emitter_list =
        {
            .values = kXdnaArtifactEmitters,
            .count = IREE_ARRAYSIZE(kXdnaArtifactEmitters),
        },
};

const loom_artifact_provider_t loom_xdna_artifact_provider = {
    .name = IREE_SVL("xdna"),
    .public_artifact_format = IREE_SVL(LOOM_XDNA_ARTIFACT_FORMAT),
    .flags = LOOM_ARTIFACT_PROVIDER_FLAG_CANONICAL,
    .target_profile_type = &loom_aie2p_target_profile_type,
    .artifact_kind = LOOM_TARGET_COMPILE_ARTIFACT_KIND_HAL_EXECUTABLE,
    .emit_artifact = loom_xdna_artifact_provider_emit_artifact,
    .deinitialize_artifact = loom_xdna_artifact_provider_deinitialize_artifact,
};
