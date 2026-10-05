// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared component planning for shape-preserving vector packet legalization.

#ifndef LOOM_TRANSFORMS_VECTOR_COMPONENT_PACKET_LEGALIZATION_H_
#define LOOM_TRANSFORMS_VECTOR_COMPONENT_PACKET_LEGALIZATION_H_

#include "iree/base/api.h"
#include "loom/target/legalization.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_vector_component_packet_rewrite_callback_t {
  // Callback state borrowed for each completed source rewrite.
  void* user_data;
  // Optional observer for cold reporting and accounting.
  iree_status_t (*fn)(void* user_data, const loom_op_t* source_op,
                      uint64_t created_op_count, uint64_t erased_op_count);
} loom_vector_component_packet_rewrite_callback_t;

// Plans and rewrites every profitable decomposable vector component in
// |region|. The target contributes candidate lane counts through
// |context->vector_packet_policy|; the target contract remains the sole source
// of native legality. Components are emitted packet-major so direct SSA edges
// remain packet-local across mixed operations and fanout.
//
// |rewrite_callback| observes each authored operation only after its component
// rewrite has completed. Missing callback functions disable observation.
//
// Returns the number of authored operations replaced through
// |out_rewritten_op_count|. A zero count means that no component admitted a
// native candidate under the active legalization policy.
iree_status_t loom_vector_component_packet_legalize(
    loom_target_legalization_context_t* context, loom_region_t* region,
    loom_vector_component_packet_rewrite_callback_t rewrite_callback,
    uint32_t* out_rewritten_op_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_VECTOR_COMPONENT_PACKET_LEGALIZATION_H_
