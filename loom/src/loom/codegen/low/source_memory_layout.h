// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Target-independent physical layout queries for source memory accesses.

#ifndef LOOM_CODEGEN_LOW_SOURCE_MEMORY_LAYOUT_H_
#define LOOM_CODEGEN_LOW_SOURCE_MEMORY_LAYOUT_H_

#include <stdint.h>

#include "iree/base/api.h"
#include "loom/ops/vector/memory.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_LOW_SOURCE_MEMORY_ACCESS_BYTE_SHIFT_NONE UINT32_MAX

typedef enum loom_low_source_memory_address_layout_e {
  // The source layout is not proven to match a target-independent class.
  LOOM_LOW_SOURCE_MEMORY_ADDRESS_LAYOUT_UNPROVEN = 0,
  // Consecutive columns and rows use compact row-major element strides.
  LOOM_LOW_SOURCE_MEMORY_ADDRESS_LAYOUT_COMPACT_ROW_MAJOR = 1,
} loom_low_source_memory_address_layout_t;

typedef enum loom_low_source_memory_axis_byte_stride_kind_e {
  // No supported address layout describes the axis stride.
  LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_UNAVAILABLE = 0,
  // The complete byte stride is an exact compile-time value.
  LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_STATIC = 1,
  // Runtime SSA factors materialize the byte stride product.
  LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_DYNAMIC = 2,
  // Facts describe a runtime stride but no SSA value materializes it.
  LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_UNMATERIALIZED = 3,
  // The authored stride cannot be represented by the bounded product.
  LOOM_LOW_SOURCE_MEMORY_AXIS_BYTE_STRIDE_INVALID = 4,
} loom_low_source_memory_axis_byte_stride_kind_t;

typedef struct loom_low_source_memory_axis_byte_stride_t {
  // Availability and materialization class of this physical axis stride.
  loom_low_source_memory_axis_byte_stride_kind_t kind;
  // Static byte coefficient multiplied by every dynamic factor.
  int64_t static_byte_coefficient;
  // Runtime SSA values multiplied into the byte stride.
  loom_value_id_t dynamic_factors[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  // Number of populated dynamic factors.
  uint8_t dynamic_factor_count;
  // Facts for the complete byte stride product.
  loom_value_facts_t byte_facts;
  // Power-of-two shift for static_byte_coefficient, or BYTE_SHIFT_NONE.
  uint32_t static_byte_shift;
} loom_low_source_memory_axis_byte_stride_t;

// Returns the shift for a positive power-of-two byte stride. Otherwise returns
// false and writes BYTE_SHIFT_NONE.
bool loom_low_source_memory_query_byte_shift(int64_t byte_stride,
                                             uint32_t* out_shift);

// Describes one physical view-axis byte stride as a bounded materializable
// product. Dense layouts source runtime factors from suffix dimensions;
// explicit strided layouts use the fact table's retained per-axis bindings.
// Exact facts fold into the static coefficient. Fact-only non-exact strides
// are reported as UNMATERIALIZED instead of inventing an SSA source.
void loom_low_source_memory_query_axis_byte_stride(
    const loom_value_fact_table_t* fact_table,
    const loom_vector_memory_access_t* vector_access, uint8_t view_axis,
    loom_low_source_memory_axis_byte_stride_t* out_stride);

// Classifies a described source layout when its physical strides prove a
// target-independent address-layout contract.
loom_low_source_memory_address_layout_t
loom_low_source_memory_classify_address_layout(
    const loom_vector_memory_access_t* vector_access);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_SOURCE_MEMORY_LAYOUT_H_
