// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_VECTOR_PACKET_INSERT_LEGALIZATION_H_
#define LOOM_TRANSFORMS_VECTOR_PACKET_INSERT_LEGALIZATION_H_

#include "iree/base/api.h"
#include "loom/target/legalization.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_vector_packet_insert_materializer_t {
  // Opaque state passed to the materializer callbacks.
  void* user_data;
  // Returns the maximum number of IR operations emitted when dynamically
  // inserting into |result_type| at the given logical lane offset. Zero means
  // the component has no complete target representation.
  uint8_t (*operation_count)(void* user_data,
                             const loom_target_legalization_context_t* context,
                             loom_type_t result_type, uint32_t lane_offset);
  // Materializes a dynamic insertion into a scalar or vector component. The
  // result must equal |dest| when |position| is outside the component's
  // logical lane interval beginning at |lane_offset|.
  iree_status_t (*materialize_dynamic)(
      void* user_data, loom_target_legalization_context_t* context,
      loom_value_id_t value, loom_value_id_t dest, loom_value_id_t position,
      uint32_t lane_offset, loom_type_t result_type,
      loom_location_id_t location, loom_value_id_t* out_result);
} loom_vector_packet_insert_materializer_t;

// Packetizes a static rank-one scalar insertion whose result spans more than
// one target-native packet. Static insertion updates only the owning packet.
// Dynamic insertion builds one native candidate per packet and selects the
// candidate over that packet's lane interval before concatenating the result.
// Returns false through |out_rewritten| when the result already fits one
// packet, the operation is not a scalar insertion, or the expansion exceeds
// the static bound.
iree_status_t loom_vector_packet_legalize_insert(
    loom_target_legalization_context_t* context, loom_op_t* op,
    const loom_target_vector_packet_policy_t* policy,
    const loom_vector_packet_insert_materializer_t* insert_materializer,
    bool* out_rewritten);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_VECTOR_PACKET_INSERT_LEGALIZATION_H_
