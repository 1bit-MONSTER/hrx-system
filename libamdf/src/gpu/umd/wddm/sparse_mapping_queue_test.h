// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_WDDM_SPARSE_MAPPING_QUEUE_TEST_H_
#define AMDF_SRC_GPU_UMD_WDDM_SPARSE_MAPPING_QUEUE_TEST_H_

#include <stdint.h>

// Test-only software-scheduled context. WKMI's binary C++ and CRT objects stay
// entirely inside the helper DLL; the regression uses only this private C ABI.
typedef struct amdf_sparse_mapping_test_queue_t
    amdf_sparse_mapping_test_queue_t;

// Native engine selection, consumed by context creation without rediscovery.
typedef struct amdf_sparse_mapping_test_scheduler_t {
  // Driver scheduler identifier of an available software-scheduled SDMA engine.
  uint32_t scheduler;
  // Node ordinal corresponding to that scheduler.
  uint32_t node_ordinal;
  // Native context private-data setting supplied by the adapter.
  uint32_t state_shadowing;
  // SDMA packet fields independent of the queue publication mechanism.
  uint64_t format_features;
} amdf_sparse_mapping_test_scheduler_t;

// Operations for the native mapping regression, not a production queue service.
// Results are NTSTATUS values. Failure stops the experiment; it never implies
// completion or authorizes freeing backing after uncertain native acceptance.
typedef struct amdf_sparse_mapping_test_queue_api_t {
  // Returns STATUS_NOT_SUPPORTED when there is no software-scheduled engine.
  int32_t (*select)(uint32_t adapter,
                    amdf_sparse_mapping_test_scheduler_t* out_scheduler);
  // Prepares all context, monitored-fence and submission storage before use.
  int32_t (*create)(uint32_t device, uint32_t physical_adapter_index,
                    const amdf_sparse_mapping_test_scheduler_t* scheduler,
                    amdf_sparse_mapping_test_queue_t** out_queue);
  // Submits and waits for exact monitored progress, excluding the reset value.
  int32_t (*execute)(amdf_sparse_mapping_test_queue_t* queue, uint64_t address,
                     uint32_t byte_length, uint64_t progress);
  // Releases a fully retired context and its fence.
  int32_t (*destroy)(amdf_sparse_mapping_test_queue_t* queue);
} amdf_sparse_mapping_test_queue_api_t;

#endif  // AMDF_SRC_GPU_UMD_WDDM_SPARSE_MAPPING_QUEUE_TEST_H_
