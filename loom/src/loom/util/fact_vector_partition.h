// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Scope-local SSA materialization of regular static vector partitions.
//
// A partition records the authored packet values that form one logical vector.
// Every packet except the last has the same logical lane count, and the last
// packet may be shorter. Numeric value facts remain independent of this
// structural materialization.

#ifndef LOOM_UTIL_FACT_VECTOR_PARTITION_H_
#define LOOM_UTIL_FACT_VECTOR_PARTITION_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_value_fact_table_t loom_value_fact_table_t;

// Immutable authored packet tuple retained by the fact table.
typedef struct loom_value_fact_vector_partition_t {
  // SSA vector values in logical lane order.
  const loom_value_id_t* values;
  // Number of values in the partition.
  uint32_t count;
  // Logical lane count of every value except an optionally shorter last value.
  uint32_t regular_lane_count;
} loom_value_fact_vector_partition_t;

// Retains a copy of the regular vector partition for |value_id|. An empty
// partition clears the fact without allocating. Recomputing a producer replaces
// its partition; unchanged partitions reuse their storage.
iree_status_t loom_value_fact_table_define_vector_partition(
    loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_vector_partition_t partition);

// Returns the retained partition, or an empty record when none is available.
// The values borrow immutable fact-table storage until the populated scope is
// cleared. This is an indexed query and does not inspect source IR.
loom_value_fact_vector_partition_t loom_value_fact_table_query_vector_partition(
    const loom_value_fact_table_t* table, loom_value_id_t value_id);

// Shares the source partition with a value alias in the same SSA scope. An
// unavailable source clears the result partition.
iree_status_t loom_value_fact_table_forward_vector_partition(
    loom_value_fact_table_t* table, loom_value_id_t source_value_id,
    loom_value_id_t result_value_id);

// Withdraws a partition when its producer is invalidated, without allocating.
void loom_value_fact_table_clear_vector_partition(
    loom_value_fact_table_t* table, loom_value_id_t value_id);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_UTIL_FACT_VECTOR_PARTITION_H_
