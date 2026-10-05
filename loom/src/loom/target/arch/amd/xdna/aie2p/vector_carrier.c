// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/vector_carrier.h"

#include "loom/ir/scalar_type.h"

loom_aie2p_vector_carrier_t loom_aie2p_vector_carrier_for_type(
    loom_type_t type) {
  uint64_t element_count = 0;
  if (!loom_type_is_vector(type) || !loom_type_is_all_static(type) ||
      !loom_type_static_element_count(type, &element_count) ||
      element_count == 0) {
    return (loom_aie2p_vector_carrier_t){0};
  }

  const loom_scalar_type_t element_type = loom_type_element_type(type);
  const bool is_rank_one = loom_type_rank(type) == 1;
  const int32_t element_bit_count = loom_scalar_type_bitwidth(element_type);
  if (is_rank_one && element_count == 32 &&
      element_type == LOOM_SCALAR_TYPE_F32) {
    return (loom_aie2p_vector_carrier_t){
        .kind = LOOM_AIE2P_VECTOR_CARRIER_ACCUMULATOR,
        .unit_count = 2,
    };
  }

  // Rank-one accumulator values above 1024 bits retain the containing
  // four-unit physical view. AIE2P has no allocatable three-unit accumulator
  // view; units beyond the source vector's logical extent are unobservable.
  const bool has_accumulator_element_type =
      element_type == LOOM_SCALAR_TYPE_I32 ||
      element_type == LOOM_SCALAR_TYPE_I64 ||
      element_type == LOOM_SCALAR_TYPE_F32;
  if (is_rank_one && has_accumulator_element_type && element_bit_count > 0 &&
      element_count > 1024 / (uint32_t)element_bit_count &&
      element_count <= 2048 / (uint32_t)element_bit_count) {
    return (loom_aie2p_vector_carrier_t){
        .kind = LOOM_AIE2P_VECTOR_CARRIER_ACCUMULATOR,
        .unit_count = 4,
    };
  }

  if (element_type == LOOM_SCALAR_TYPE_I1) {
    if (element_count <= 128) {
      return (loom_aie2p_vector_carrier_t){
          .kind = LOOM_AIE2P_VECTOR_CARRIER_PREDICATE,
          .unit_count = (uint32_t)((element_count + 63u) / 64u),
      };
    }
    // Predicate payloads have no ordinary vector-register representation.
    return (loom_aie2p_vector_carrier_t){0};
  }

  if (element_bit_count > 0 &&
      element_count > 512 / (uint32_t)element_bit_count &&
      element_count <= 1024 / (uint32_t)element_bit_count) {
    return (loom_aie2p_vector_carrier_t){
        .kind = LOOM_AIE2P_VECTOR_CARRIER_ORDINARY,
        .unit_count = 4,
    };
  }
  if (element_bit_count > 0 &&
      element_count <= 512 / (uint32_t)element_bit_count) {
    return (loom_aie2p_vector_carrier_t){
        .kind = LOOM_AIE2P_VECTOR_CARRIER_ORDINARY,
        .unit_count = 2,
    };
  }
  return (loom_aie2p_vector_carrier_t){0};
}
