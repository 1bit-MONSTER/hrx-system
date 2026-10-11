// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Generated source-to-Low rule emission.
//
// Emission consumes one rule selected during planning and its resolved Low
// descriptors, attributes, source origins, and result carriers. It interprets
// the retained emit program into target-Low operands, packets, result bindings,
// aliases, and elisions. Source-origin and result-carrier selection are
// complete before this interpreter runs; target materializers have their own
// contracts.

#ifndef LOOM_CODEGEN_LOW_LOWER_RULE_EMIT_H_
#define LOOM_CODEGEN_LOW_LOWER_RULE_EMIT_H_

#include "iree/base/api.h"
#include "loom/codegen/low/lower/rule_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_lower_selected_source_memory_t
    loom_low_lower_selected_source_memory_t;

// Emits target-Low packets for |source_op| using a previously selected rule.
// Multi-node rules provide the retained source graph with the root at index
// zero; root-only rules may provide NULL with a count of one.
iree_status_t loom_low_lower_rule_set_emit_rule(
    loom_low_lower_context_t* context,
    const loom_low_lower_rule_set_t* rule_set, const loom_op_t* source_op,
    const loom_low_lower_rule_t* rule,
    const loom_low_lower_resolved_emit_t* resolved_emits,
    const loom_low_lower_selected_source_memory_t* source_memory,
    const loom_op_t* const* source_nodes, uint8_t source_node_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_RULE_EMIT_H_
