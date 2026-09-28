// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/array/binding.h"

#include <string.h>

#include "loom/ir/facts.h"
#include "loom/ops/encoding/storage.h"

static uint32_t loom_aie2p_array_binding_dimension_value(
    const loom_value_fact_table_t* facts, loom_type_t type,
    iree_host_size_t dimension) {
  if (!loom_type_dim_is_dynamic_at(type, dimension)) {
    const int64_t value = loom_type_dim_static_size_at(type, dimension);
    IREE_ASSERT(value >= 0 && value <= UINT32_MAX,
                "validated static binding dimension must fit u32");
    return (uint32_t)value;
  }

  const loom_value_id_t value_id = loom_type_dim_value_id_at(type, dimension);
  loom_value_facts_t element_facts = loom_value_facts_unknown();
  int64_t value = 0;
  const bool found =
      loom_value_facts_query_all_equal_element(
          &facts->context, loom_value_fact_table_lookup(facts, value_id),
          &element_facts) &&
      loom_value_facts_as_exact_i64(element_facts, &value);
  IREE_ASSERT(found && value >= 0 && value <= UINT32_MAX,
              "validated dynamic binding dimension must be an exact u32");
  (void)found;
  return (uint32_t)value;
}

static bool loom_aie2p_array_reject_binding_transfer(
    iree_string_view_t reason, iree_string_view_t* out_reason) {
  *out_reason = reason;
  return false;
}

static bool loom_aie2p_array_binding_stride_value(
    const loom_value_fact_address_layout_t* layout, uint8_t dimension,
    uint64_t* out_value, iree_string_view_t* out_reason) {
  int64_t value = 0;
  if (!loom_value_facts_as_exact_i64(layout->strides[dimension], &value) ||
      value < 0) {
    return loom_aie2p_array_reject_binding_transfer(
        IREE_SV("every source stride must resolve to an exact non-negative "
                "value"),
        out_reason);
  }
  *out_value = (uint64_t)value;
  return true;
}

static bool loom_aie2p_array_binding_address_dimension(
    uint64_t element_stride, uint32_t dimension, int32_t element_bit_width,
    uint8_t address_granularity_bits, uint8_t dimension_index,
    uint8_t dimension_count, const loom_xdna_dma_facts_t* dma_facts,
    loom_aie2p_array_binding_dma_dimension_t* out_dimension,
    iree_string_view_t* out_reason) {
  uint64_t step_bits = 0;
  uint64_t wrap_bits = 0;
  const bool has_outer_dimension = dimension_index + 1u < dimension_count;
  if (dimension_index == 0 && element_bit_width < address_granularity_bits) {
    uint64_t packed_dimension_bits = 0;
    if (element_stride != 1 ||
        !iree_checked_mul_u64(dimension, (uint64_t)element_bit_width,
                              &packed_dimension_bits) ||
        packed_dimension_bits % address_granularity_bits != 0) {
      return loom_aie2p_array_reject_binding_transfer(
          IREE_SV("the innermost transfer dimension must form packed native "
                  "address units"),
          out_reason);
    }
    step_bits = address_granularity_bits;
    if (has_outer_dimension) {
      wrap_bits = packed_dimension_bits;
    }
  } else {
    if (!iree_checked_mul_u64(element_stride, (uint64_t)element_bit_width,
                              &step_bits) ||
        step_bits % address_granularity_bits != 0) {
      return loom_aie2p_array_reject_binding_transfer(
          IREE_SV("every transfer stride must form whole native address "
                  "units"),
          out_reason);
    }
    if (has_outer_dimension) {
      wrap_bits = (uint64_t)dimension * address_granularity_bits;
    }
  }

  const uint64_t step_size = step_bits / address_granularity_bits;
  const uint64_t wrap = wrap_bits / address_granularity_bits;
  const uint64_t maximum_step_size = UINT64_C(1) << dma_facts->step_size_bits;
  const uint64_t maximum_wrap = UINT64_C(1) << dma_facts->wrap_bits;
  if (step_size == 0 || step_size > maximum_step_size ||
      (has_outer_dimension && (wrap == 0 || wrap > maximum_wrap))) {
    return loom_aie2p_array_reject_binding_transfer(
        IREE_SV("an address dimension exceeds the shim DMA step or wrap "
                "field"),
        out_reason);
  }
  *out_dimension = (loom_aie2p_array_binding_dma_dimension_t){
      .step_size = (uint32_t)step_size,
      .wrap = (uint32_t)wrap,
  };
  return true;
}

bool loom_aie2p_array_resolve_binding_transfer(
    const loom_module_t* module, const loom_value_fact_table_t* facts,
    const loom_xdna_array_family_t* family, loom_type_t source_type,
    loom_type_t record_type, uint64_t binding_view_byte_offset,
    bool partitioned, uint32_t partition_lane,
    uint32_t logical_record_byte_length, uint32_t logical_record_count,
    const loom_xdna_dma_facts_t* dma_facts,
    loom_aie2p_array_binding_plan_t* binding_plan,
    iree_string_view_t* out_reason) {
  *out_reason = iree_string_view_empty();
  const uint64_t logical_transfer_byte_length =
      (uint64_t)logical_record_byte_length * logical_record_count;

  uint64_t binding_byte_offset = binding_view_byte_offset;
  uint64_t binding_span_byte_length = logical_transfer_byte_length;
  uint64_t transfer_byte_length = logical_transfer_byte_length;
  uint64_t task_repeat_count = 1;
  uint8_t dma_dimension_count = 0;
  loom_aie2p_array_binding_dma_dimension_t
      dma_dimensions[LOOM_AIE2P_ARRAY_BINDING_DMA_DIMENSION_COUNT] = {0};

  loom_value_facts_t stride_storage[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  loom_value_fact_address_layout_t layout = {0};
  if (!loom_encoding_query_type_address_layout(
          &facts->context, module, source_type, stride_storage,
          IREE_ARRAYSIZE(stride_storage), &layout)) {
    return loom_aie2p_array_reject_binding_transfer(
        IREE_SV("the binding source must have one exact dense or strided "
                "address layout"),
        out_reason);
  }
  if (layout.kind == LOOM_VALUE_FACT_ADDRESS_LAYOUT_DENSE) {
    if (partitioned) {
      uint64_t partition_byte_offset = 0;
      if (!iree_checked_mul_u64(logical_transfer_byte_length, partition_lane,
                                &partition_byte_offset) ||
          !iree_checked_add_u64(binding_byte_offset, partition_byte_offset,
                                &binding_byte_offset)) {
        return loom_aie2p_array_reject_binding_transfer(
            IREE_SV("the selected partition offset must fit in 64 bits"),
            out_reason);
      }
    }
  } else if (layout.kind == LOOM_VALUE_FACT_ADDRESS_LAYOUT_STRIDED) {
    const uint8_t source_rank = loom_type_rank(source_type);
    const uint8_t record_rank = loom_type_rank(record_type);
    const uint8_t first_source_axis = partitioned ? 1 : 0;
    IREE_ASSERT(source_rank == layout.rank && layout.strides,
                "queried strided layout must cover its source rank");
    IREE_ASSERT(source_rank >= record_rank &&
                    first_source_axis <= source_rank - record_rank,
                "validated binding view must select a trailing record");
    const uint8_t record_start = source_rank - record_rank;
    const int32_t element_bit_width =
        loom_scalar_type_bitwidth(loom_type_element_type(source_type));
    IREE_ASSERT_GT(element_bit_width, 0);

    if (partitioned) {
      uint64_t lane_element_stride = 0;
      if (!loom_aie2p_array_binding_stride_value(
              &layout, 0, &lane_element_stride, out_reason)) {
        return false;
      }
      uint64_t binding_bit_offset = 0;
      if (!iree_checked_mul_u64(lane_element_stride, partition_lane,
                                &binding_bit_offset) ||
          !iree_checked_mul_u64(binding_bit_offset, (uint64_t)element_bit_width,
                                &binding_bit_offset) ||
          (binding_bit_offset & 7u) != 0) {
        return loom_aie2p_array_reject_binding_transfer(
            IREE_SV("the selected partition must begin at a whole-byte "
                    "representable offset"),
            out_reason);
      }
      if (!iree_checked_add_u64(binding_byte_offset, binding_bit_offset / 8u,
                                &binding_byte_offset)) {
        return loom_aie2p_array_reject_binding_transfer(
            IREE_SV("the selected partition offset must fit in 64 bits"),
            out_reason);
      }
    }

    uint8_t first_transfer_axis = first_source_axis;
    for (; first_transfer_axis < record_start; ++first_transfer_axis) {
      uint64_t element_stride = 0;
      if (!loom_aie2p_array_binding_stride_value(&layout, first_transfer_axis,
                                                 &element_stride, out_reason)) {
        return false;
      }
      if (element_stride != 0) {
        break;
      }
      const uint32_t dimension = loom_aie2p_array_binding_dimension_value(
          facts, source_type, first_transfer_axis);
      task_repeat_count *= dimension;
    }

    uint64_t transfer_element_count = 1;
    uint64_t maximum_element_offset = 0;
    bool transfer_is_dense = true;
    for (uint8_t axis = source_rank; axis-- > first_transfer_axis;) {
      const uint32_t dimension =
          loom_aie2p_array_binding_dimension_value(facts, source_type, axis);
      uint64_t element_stride = 0;
      if (!loom_aie2p_array_binding_stride_value(&layout, axis, &element_stride,
                                                 out_reason)) {
        return false;
      }
      IREE_ASSERT_NE(dimension, 0);
      if (element_stride == 0) {
        return loom_aie2p_array_reject_binding_transfer(
            IREE_SV("only leading source sequence axes may use zero stride "
                    "for task repetition"),
            out_reason);
      }
      transfer_is_dense &= element_stride == transfer_element_count;
      if (!iree_checked_mul_u64(transfer_element_count, dimension,
                                &transfer_element_count)) {
        return loom_aie2p_array_reject_binding_transfer(
            IREE_SV("the physical transfer shape must fit in 64 bits"),
            out_reason);
      }
      uint64_t axis_maximum_offset = 0;
      if (!iree_checked_mul_u64(element_stride, dimension - 1u,
                                &axis_maximum_offset) ||
          !iree_checked_add_u64(maximum_element_offset, axis_maximum_offset,
                                &maximum_element_offset)) {
        return loom_aie2p_array_reject_binding_transfer(
            IREE_SV("the source layout address span must fit in 64 bits"),
            out_reason);
      }
    }

    uint64_t transfer_bit_length = 0;
    uint64_t binding_span_bit_length = 0;
    if (!iree_checked_mul_u64(transfer_element_count,
                              (uint64_t)element_bit_width,
                              &transfer_bit_length) ||
        !iree_checked_add_u64(maximum_element_offset, 1u,
                              &maximum_element_offset) ||
        !iree_checked_mul_u64(maximum_element_offset,
                              (uint64_t)element_bit_width,
                              &binding_span_bit_length) ||
        (transfer_bit_length & 7u) != 0 ||
        (binding_span_bit_length & 7u) != 0) {
      return loom_aie2p_array_reject_binding_transfer(
          IREE_SV("the transfer shape must form whole-byte transfer and "
                  "address spans"),
          out_reason);
    }
    transfer_byte_length = transfer_bit_length / 8u;
    binding_span_byte_length = binding_span_bit_length / 8u;

    if (!transfer_is_dense) {
      const uint8_t transfer_rank = source_rank - first_transfer_axis;
      if (transfer_rank > dma_facts->address_dimension_count) {
        return loom_aie2p_array_reject_binding_transfer(
            IREE_SV("the strided transfer requires more address dimensions "
                    "than shim DMA provides"),
            out_reason);
      }
      dma_dimension_count = transfer_rank;
      for (uint8_t axis = source_rank; axis-- > first_transfer_axis;) {
        const uint8_t dimension_index = source_rank - axis - 1u;
        const uint32_t dimension =
            loom_aie2p_array_binding_dimension_value(facts, source_type, axis);
        uint64_t element_stride = 0;
        if (!loom_aie2p_array_binding_stride_value(
                &layout, axis, &element_stride, out_reason) ||
            !loom_aie2p_array_binding_address_dimension(
                element_stride, dimension, element_bit_width,
                family->address_generation_granularity_bits, dimension_index,
                transfer_rank, dma_facts, &dma_dimensions[dimension_index],
                out_reason)) {
          return false;
        }
      }
    }
  } else {
    IREE_ASSERT_UNREACHABLE("queried binding layout kind");
    return false;
  }

  const uint64_t scaled_transfer_length =
      transfer_byte_length / dma_facts->transfer_length_granularity;
  const uint64_t minimum_transfer_units =
      iree_max(1u, dma_facts->transfer_length_offset);
  const uint64_t maximum_transfer_units =
      (uint64_t)dma_facts->maximum_encoded_transfer_length +
      dma_facts->transfer_length_offset;
  if (transfer_byte_length == 0 || transfer_byte_length > UINT32_MAX ||
      transfer_byte_length % dma_facts->transfer_length_granularity != 0 ||
      scaled_transfer_length < minimum_transfer_units ||
      scaled_transfer_length > maximum_transfer_units) {
    return loom_aie2p_array_reject_binding_transfer(
        IREE_SV("the transfer length must fit the shim DMA length field in "
                "native units"),
        out_reason);
  }
  if (task_repeat_count > dma_facts->maximum_task_repeat_count) {
    return loom_aie2p_array_reject_binding_transfer(
        IREE_SV("the repeated task count exceeds the shim DMA queue field"),
        out_reason);
  }
  if (binding_byte_offset > INT64_MAX ||
      binding_byte_offset % dma_facts->address_alignment != 0 ||
      binding_span_byte_length > dma_facts->address_maximum ||
      binding_byte_offset >
          dma_facts->address_maximum - binding_span_byte_length) {
    return loom_aie2p_array_reject_binding_transfer(
        IREE_SV("the aligned binding offset and reachable span must fit the "
                "shim DMA address domain"),
        out_reason);
  }

  binding_plan->binding_byte_offset = binding_byte_offset;
  binding_plan->binding_span_byte_length = binding_span_byte_length;
  binding_plan->transfer_byte_length = (uint32_t)transfer_byte_length;
  binding_plan->task_repeat_count = (uint16_t)task_repeat_count;
  binding_plan->dma_dimension_count = dma_dimension_count;
  memcpy(binding_plan->dma_dimensions, dma_dimensions,
         sizeof(binding_plan->dma_dimensions));
  return true;
}
