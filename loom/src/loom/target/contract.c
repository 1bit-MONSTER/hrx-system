// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/contract.h"

#include <stddef.h>
#include <stdint.h>

loom_type_t loom_target_contract_query_projected_value_type(
    loom_target_contract_vector_lane_projection_t projection,
    const loom_module_t* module, loom_value_id_t value_id) {
  const loom_type_t authored_type = loom_module_value_type(module, value_id);
  uint64_t lane_count = 0;
  if (!loom_type_is_vector(authored_type) ||
      !loom_type_static_element_count(authored_type, &lane_count) ||
      lane_count != projection.source_lane_count) {
    return authored_type;
  }
  return loom_type_shaped_1d(
      LOOM_TYPE_VECTOR, loom_type_element_type(authored_type),
      loom_dim_pack_static(projection.projected_lane_count),
      loom_type_rank(authored_type) == 1 ? authored_type.encoding_id : 0);
}

iree_status_t loom_target_contract_query_get_or_allocate_target_state(
    const loom_target_contract_query_environment_t* environment,
    const void* key, iree_host_size_t data_length, void** out_data) {
  IREE_ASSERT_ARGUMENT(environment);
  IREE_ASSERT_ARGUMENT(key);
  IREE_ASSERT_GT(data_length, 0);
  IREE_ASSERT_ARGUMENT(out_data);
  *out_data = NULL;
  if (environment->target_state_allocator.fn == NULL) {
    return iree_ok_status();
  }
  return environment->target_state_allocator.fn(
      environment->target_state_allocator.user_data, key, data_length,
      out_data);
}
