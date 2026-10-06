// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Applied source loop schedules retained by compiler function versions.

#ifndef LOOM_TARGET_LOOP_PIPELINE_H_
#define LOOM_TARGET_LOOP_PIPELINE_H_

#include "iree/base/api.h"

typedef enum loom_source_loop_pipeline_partition_e {
  // The complete source operation belongs to one stage.
  LOOM_SOURCE_LOOP_PIPELINE_PARTITION_NONE = 0,
  // A conditional's read closure and ordered remainder belong to two stages.
  LOOM_SOURCE_LOOP_PIPELINE_PARTITION_GUARDED = 1,
} loom_source_loop_pipeline_partition_t;

// One source operation copy's position in the applied read-ahead schedule.
typedef struct loom_source_loop_pipeline_operation_t {
  // Position of the operation in the original source loop body.
  uint32_t source_position;
  // Source mnemonic borrowed from the compilation context's dialect tables.
  iree_string_view_t op_name;
  // Original iterations ahead of the ordered consumer; zero for consumers.
  uint32_t iteration_lookahead;
  // Interior partition used to place this operation copy.
  loom_source_loop_pipeline_partition_t partition;
} loom_source_loop_pipeline_operation_t;

// An immutable applied policy, independent of subsequent IR replacement.
// The version owner's arena owns this record and its operation array. No
// source operation or value handles survive the scheduling transform here.
typedef struct loom_source_loop_pipeline_t {
  // Next applied policy in the same function version, or NULL.
  struct loom_source_loop_pipeline_t* next;
  // Stable ordinal among policies applied to this function version.
  iree_host_size_t loop_ordinal;
  // Requested iteration depth; one records a serial policy.
  uint32_t depth;
  // Number of SSA values retained in each queued iteration record.
  uint32_t values_per_record;
  // Number of ordinary source reads in the producer stage.
  uint32_t read_count;
  // Applied copies in source body order. Reconstructed arithmetic has both
  // producer and consumer entries.
  const loom_source_loop_pipeline_operation_t* operations;
  // Number of entries in operations; zero for a depth-one policy.
  uint32_t operation_count;
} loom_source_loop_pipeline_t;

// Applied policies in stable production order. Empty versions allocate nothing.
typedef struct loom_source_loop_pipeline_list_t {
  // First applied policy, or NULL.
  loom_source_loop_pipeline_t* head;
  // Last applied policy, or NULL.
  loom_source_loop_pipeline_t* tail;
} loom_source_loop_pipeline_list_t;

#endif  // LOOM_TARGET_LOOP_PIPELINE_H_
