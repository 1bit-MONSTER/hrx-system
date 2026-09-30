// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/vm/module_compiler.h"

#include "loom/target/arch/vm/ops/ops.h"
#include "loom/target/arch/vm/program_build.h"
#include "loom/target/arch/vm/provider.h"
#include "loom/target/emit/vm/module_binary.h"

static iree_status_t loom_vm_module_emit(
    const loom_target_emit_request_t* request, bool* out_emitted,
    loom_target_emit_artifact_t* out_artifact) {
  *out_emitted = false;
  *out_artifact = (loom_target_emit_artifact_t){0};

  const iree_arena_checkpoint_t checkpoint =
      iree_arena_checkpoint_save(request->scratch_arena);
  loom_vm_program_plan_t plan = {0};
  bool accepted = false;
  iree_status_t status = loom_vm_program_plan_build(
      request->module, request->function_versions,
      request->low_descriptor_registry, request->diagnostic_emitter,
      request->scratch_arena, request->allocator, &accepted, &plan);
  if (iree_status_is_ok(status) && accepted) {
    status = loom_vm_program_emit_binary(&plan, request->allocator,
                                         &out_artifact->contents);
  }
  if (iree_status_is_ok(status) && accepted) {
    out_artifact->target_artifact_format =
        LOOM_TARGET_ARTIFACT_FORMAT_VM_BINARY;
    *out_emitted = true;
  }
  loom_vm_program_plan_deinitialize(&plan);
  iree_arena_checkpoint_restore(&checkpoint);
  return status;
}

const loom_target_emitter_t loom_vm_module_emitter = {
    .name = IREE_SVL("vm"),
    .public_artifact_format = IREE_SVL("vm"),
    .default_identifier = IREE_SVL("module.vm"),
    .target_artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_VM_BINARY,
    .emit = loom_vm_module_emit,
};

const loom_target_provider_t loom_vm_module_provider = {
    .emitter_list =
        {
            .values =
                (const loom_target_emitter_t* const[]){&loom_vm_module_emitter},
            .count = 1,
        },
    .canonical_module_emitter = &loom_vm_module_emitter,
    .canonical_module_fact_type = &loom_vm_target_fact_type,
};

static const loom_target_provider_t* const kVmCompilerProviders[] = {
    &loom_vm_target_provider,
    &loom_vm_module_provider,
};

const loom_target_provider_set_t loom_vm_compiler_provider_set = {
    .providers = kVmCompilerProviders,
    .provider_count = IREE_ARRAYSIZE(kVmCompilerProviders),
};
