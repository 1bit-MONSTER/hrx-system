// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Compiler-owned preparation of complete AIE2P XDNA products.

#ifndef LOOM_TOOLING_TARGET_AMD_XDNA_PRODUCT_PREPARE_H_
#define LOOM_TOOLING_TARGET_AMD_XDNA_PRODUCT_PREPARE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/xdna_product.h"
#include "loom/target/arch/amd/xdna/device/profile.h"
#include "loom/target/entry_selection.h"
#include "loom/target/function_version.h"
#include "loom/target/reporting/report.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_xdna_product_prepare_request_t {
  // Immutable module containing prepared AIE2P target-low IR.
  const loom_module_t* module;

  // Artifact roots selected through the shared compiler entry policy.
  loom_target_entry_list_t entries;

  // Concrete compiler function versions participating in emission.
  const loom_function_version_list_t* function_versions;

  // Low descriptor registry containing AIE2P core and array descriptors.
  const loom_low_descriptor_registry_t* low_descriptor_registry;

  // Explicit deployment profile, or NULL to use the array entry target facts.
  const loom_xdna_device_profile_t* device_profile;

  // Optional caller-owned structured compile report for this emission. The
  // report may retain target backing storage until report deinitialization.
  loom_target_compile_report_t* compile_report;

  // Diagnostic emitter receiving target diagnostics.
  iree_diagnostic_emitter_t diagnostic_emitter;

  // Invocation-local scratch arena.
  iree_arena_allocator_t* scratch_arena;

  // Host allocator owning resident compiler state.
  iree_allocator_t allocator;
} loom_xdna_product_prepare_request_t;

// Prepares one complete target-native XDNA product plan. Structured rejection
// returns OK with |out_prepared| false and an emitted diagnostic. The returned
// plan borrows storage from |request->scratch_arena|.
iree_status_t loom_xdna_product_prepare(
    const loom_xdna_product_prepare_request_t* request, bool* out_prepared,
    loom_aie2p_xdna_product_plan_t* out_plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_AMD_XDNA_PRODUCT_PREPARE_H_
