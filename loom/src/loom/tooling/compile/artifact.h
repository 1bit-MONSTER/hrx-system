// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Loadable artifact production for live execution backends.
//
// Device providers project runtime devices into immutable target profiles.
// Artifact providers consume those profiles and prepared target-low IR to
// produce the executable representation loaded by the execution backend.

#ifndef LOOM_TOOLING_COMPILE_ARTIFACT_H_
#define LOOM_TOOLING_COMPILE_ARTIFACT_H_

#include "iree/base/api.h"
#include "iree/base/byte_sequence.h"
#include "loom/ir/module.h"
#include "loom/target/pipeline_options.h"
#include "loom/target/profile.h"
#include "loom/target/provider.h"
#include "loom/target/reporting/report.h"
#include "loom/target/types.h"
#include "loom/tooling/compile/options.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_artifact_provider_t loom_artifact_provider_t;

// Borrowed concrete artifact target selected for a runtime device.
typedef struct loom_artifact_target_t {
  // Immutable structured target profile selected for the device.
  const loom_target_profile_t* target_profile;
  // Family-owned target selector used for diagnostics and artifact metadata.
  iree_string_view_t target_key;
} loom_artifact_target_t;

// Returns the target-neutral bundle projected by |target|, or NULL.
static inline const loom_target_bundle_t* loom_artifact_target_bundle(
    const loom_artifact_target_t* target) {
  return target ? loom_target_profile_bundle(target->target_profile) : NULL;
}

// Loadable artifact bytes produced for an already-selected target.
typedef struct loom_artifact_t {
  // Provider-facing target key used to emit the artifact.
  iree_string_view_t target_key;
  // Durable target-neutral bundle resolved for the artifact.
  const loom_target_bundle_t* target_bundle;
  // Target-native artifact format.
  loom_target_artifact_format_t target_artifact_format;
  // Borrowed target-native artifact contents owned by |storage|.
  iree_byte_sequence_t* target_artifact_data;
  // Target-owned textual listing format, such as `amdgpu-assembly`.
  iree_string_view_t target_listing_format;
  // Borrowed textual listing contents owned by |storage|.
  iree_byte_sequence_t* target_listing_data;
  // Optional sidecar artifacts produced beside |executable_data|.
  const loom_target_emit_sidecar_artifact_t* sidecars;
  // Number of entries in |sidecars|.
  iree_host_size_t sidecar_count;
  // Borrowed primary executable contents owned by |storage|.
  iree_byte_sequence_t* executable_data;
  // Provider-owned storage released by |deinitialize_artifact|.
  void* storage;
} loom_artifact_t;

// Emits a loadable artifact from verified, prepared target-low IR. Providers
// check emission-specific constraints without repeating structural or Low
// verification. When |out_emitted| is true the artifact has a target bundle,
// non-empty target-native and executable contents, and a valid
// descriptor and contents for every sidecar. Returning OK with |out_emitted|
// false is reserved for product diagnostics emitted through |diagnostic_sink|;
// infrastructure failures return a non-OK status.
typedef iree_status_t (*loom_artifact_provider_emit_fn_t)(
    const loom_artifact_provider_t* provider, loom_module_t* module,
    const loom_artifact_target_t* target, const loom_compile_options_t* options,
    iree_allocator_t allocator, bool* out_emitted,
    loom_artifact_t* out_artifact);

typedef void (*loom_artifact_provider_deinitialize_artifact_fn_t)(
    const loom_artifact_provider_t* provider, loom_artifact_t* artifact,
    iree_allocator_t allocator);

// Linked loadable-artifact compiler for one target family.
struct loom_artifact_provider_t {
  // Stable provider name used in diagnostics and execution tooling.
  iree_string_view_t name;
  // Required target-family profile representation.
  const loom_target_profile_type_t* target_profile_type;
  // Artifact kind recorded in structured compile reports.
  loom_target_compile_artifact_kind_t artifact_kind;
  // Target-owned defaults used to prepare target-low IR before emission.
  loom_target_pipeline_options_t default_pipeline_options;
  // Emits a prepared target-low module to executable artifact bytes.
  loom_artifact_provider_emit_fn_t emit_artifact;
  // Releases storage owned by an artifact returned from |emit_artifact|.
  loom_artifact_provider_deinitialize_artifact_fn_t deinitialize_artifact;
};

// Selects a borrowed artifact target from |target_environment|.
//
// |target_specification| must use `family:selector` syntax and select the
// target family required by |provider|. The returned profile has process
// lifetime and |out_target->target_key| borrows the selector portion of
// |target_specification|. Selection performs no allocation and requires no
// teardown. On failure |out_target| remains empty.
iree_status_t loom_artifact_target_select(
    const loom_artifact_provider_t* provider,
    const loom_target_environment_t* target_environment,
    iree_string_view_t target_specification,
    loom_artifact_target_t* out_target);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_COMPILE_ARTIFACT_H_
