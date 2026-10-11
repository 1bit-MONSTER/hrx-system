// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Source-graph emission placement legality.
//
// Generated rules describe exact source graphs whose related nodes are proved
// relocatable by the matcher. The matcher identifies the source node where
// every emitted operand is available. Exclusive graph edges and generated
// dead-sibling guards prove result availability: every graph node preceding
// the emission node has no use outside the graph. This analysis only needs to
// prove that a non-pure graph root crosses no ordering boundary. Read barriers
// are indexed once per immutable function and reused by every candidate.

#ifndef LOOM_CODEGEN_LOW_LOWER_SOURCE_GRAPH_PLACEMENT_H_
#define LOOM_CODEGEN_LOW_LOWER_SOURCE_GRAPH_PLACEMENT_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_lower_context_t loom_low_lower_context_t;
typedef struct loom_low_lower_rule_selection_t loom_low_lower_rule_selection_t;

// Returns whether |selection| can emit at its selected source node while
// preserving source execution and memory ordering. Singleton selections are
// legal without allocating placement state.
iree_status_t loom_low_lower_source_graph_placement_evaluate(
    loom_low_lower_context_t* context,
    const loom_low_lower_rule_selection_t* selection, bool* out_legal);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_SOURCE_GRAPH_PLACEMENT_H_
