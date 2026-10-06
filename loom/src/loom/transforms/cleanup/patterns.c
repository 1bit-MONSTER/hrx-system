// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/cleanup/patterns.h"

iree_status_t loom_cleanup_pattern_registry_storage_initialize(
    const loom_cleanup_pattern_provider_set_t* provider_set,
    iree_allocator_t allocator,
    loom_cleanup_pattern_registry_storage_t* out_storage) {
  IREE_ASSERT_ARGUMENT(provider_set);
  IREE_ASSERT_ARGUMENT(out_storage);
  *out_storage = (loom_cleanup_pattern_registry_storage_t){0};

  iree_status_t status = loom_rewrite_pattern_registry_storage_initialize(
      provider_set->region_initialization, allocator,
      &out_storage->region_initialization_storage);
  if (iree_status_is_ok(status)) {
    status = loom_rewrite_pattern_registry_storage_initialize(
        provider_set->universal_pre_fold, allocator,
        &out_storage->universal_pre_fold_storage);
  }
  if (iree_status_is_ok(status)) {
    status = loom_rewrite_pattern_registry_storage_initialize(
        provider_set->universal_post_type, allocator,
        &out_storage->universal_post_type_storage);
  }
  if (iree_status_is_ok(status)) {
    status = loom_rewrite_pattern_registry_storage_initialize(
        provider_set->source_combine, allocator,
        &out_storage->source_combine_storage);
  }
  if (iree_status_is_ok(status)) {
    out_storage->registry = (loom_cleanup_pattern_registry_t){
        .region_initialization = loom_rewrite_pattern_registry_storage_registry(
            &out_storage->region_initialization_storage),
        .universal_pre_fold = loom_rewrite_pattern_registry_storage_registry(
            &out_storage->universal_pre_fold_storage),
        .universal_post_type = loom_rewrite_pattern_registry_storage_registry(
            &out_storage->universal_post_type_storage),
        .source_combine = loom_rewrite_pattern_registry_storage_registry(
            &out_storage->source_combine_storage),
        .special_value_policy = provider_set->special_value_policy,
        .fact_refinement_policy = provider_set->fact_refinement_policy,
    };
  } else {
    loom_cleanup_pattern_registry_storage_deinitialize(out_storage);
  }
  return status;
}

void loom_cleanup_pattern_registry_storage_deinitialize(
    loom_cleanup_pattern_registry_storage_t* storage) {
  if (storage == NULL) {
    return;
  }
  loom_rewrite_pattern_registry_storage_deinitialize(
      &storage->source_combine_storage);
  loom_rewrite_pattern_registry_storage_deinitialize(
      &storage->universal_post_type_storage);
  loom_rewrite_pattern_registry_storage_deinitialize(
      &storage->universal_pre_fold_storage);
  loom_rewrite_pattern_registry_storage_deinitialize(
      &storage->region_initialization_storage);
  *storage = (loom_cleanup_pattern_registry_storage_t){0};
}

const loom_cleanup_pattern_registry_t*
loom_cleanup_pattern_registry_storage_registry(
    const loom_cleanup_pattern_registry_storage_t* storage) {
  return &storage->registry;
}
