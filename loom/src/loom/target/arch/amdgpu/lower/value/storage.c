// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/value/storage.h"

#include "loom/ops/index/ops.h"
#include "loom/ops/vector/ops.h"
#include "loom/target/arch/amdgpu/lower/encoding/vector_conversion.h"
#include "loom/target/arch/amdgpu/lower/value/integer64.h"
#include "loom/target/arch/amdgpu/lower/value/vector_conversion.h"

void loom_amdgpu_mark_value_plan_storage_demands(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t plan) {
  switch (plan.id) {
    case LOOM_OP_INDEX_CAST: {
      const loom_amdgpu_index_cast_plan_t* index_cast_plan =
          (const loom_amdgpu_index_cast_plan_t*)plan.target_data;
      switch (index_cast_plan->kind) {
        case LOOM_AMDGPU_INDEX_CAST_KIND_PRESERVING_LOW_BITS:
        case LOOM_AMDGPU_INDEX_CAST_KIND_PRESERVING_LOW_BITS_TO_VGPR:
        case LOOM_AMDGPU_INDEX_CAST_KIND_ZERO_EXTENDING_LOW_32:
        case LOOM_AMDGPU_INDEX_CAST_KIND_SIGN_EXTENDING_LOW_32:
        case LOOM_AMDGPU_INDEX_CAST_KIND_NORMALIZING_NARROW:
        case LOOM_AMDGPU_INDEX_CAST_KIND_PREDICATE_TO_INTEGER:
        case LOOM_AMDGPU_INDEX_CAST_KIND_NARROWING_INTEGER:
        case LOOM_AMDGPU_INDEX_CAST_KIND_INTEGER_TO_PREDICATE:
          loom_low_lower_require_source_value_storage(context,
                                                      index_cast_plan->source);
          return;
        case LOOM_AMDGPU_INDEX_CAST_KIND_DIAGNOSTIC_REJECTED:
          return;
        case LOOM_AMDGPU_INDEX_CAST_KIND_NONE:
          break;
      }
      IREE_ASSERT_UNREACHABLE("unknown AMDGPU index cast plan kind");
      return;
    }
    case LOOM_OP_VECTOR_EXTRACT: {
      const loom_amdgpu_vector_extract_plan_t* extract_plan =
          (const loom_amdgpu_vector_extract_plan_t*)plan.target_data;
      loom_low_lower_require_source_value_storage(context,
                                                  extract_plan->source);
      if (iree_any_bit_set(extract_plan->flags,
                           LOOM_AMDGPU_VECTOR_EXTRACT_FLAG_DYNAMIC)) {
        loom_low_lower_require_source_value_storage(
            context, extract_plan->dynamic_index);
      }
      return;
    }
    case LOOM_OP_VECTOR_EXTF:
    case LOOM_OP_VECTOR_DECODE:
    case LOOM_OP_VECTOR_ENCODE:
    case LOOM_OP_VECTOR_FPTRUNC: {
      const loom_amdgpu_vector_16bit_float_conversion_plan_t* conversion_plan =
          (const loom_amdgpu_vector_16bit_float_conversion_plan_t*)
              plan.target_data;
      loom_low_lower_require_source_value_storage(
          context, conversion_plan->storage_source);
      if (conversion_plan->scale_source != LOOM_VALUE_ID_INVALID) {
        loom_low_lower_require_source_value_storage(
            context, conversion_plan->scale_source);
      }
      return;
    }
    default:
      loom_low_lower_require_source_operands_storage(context, source_op);
      return;
  }
}
