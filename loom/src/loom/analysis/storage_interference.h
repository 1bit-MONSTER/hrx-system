// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Target-independent interference analysis for function-local storage roots.

#ifndef LOOM_ANALYSIS_STORAGE_INTERFERENCE_H_
#define LOOM_ANALYSIS_STORAGE_INTERFERENCE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/local_value_domain.h"
#include "loom/ops/op_defs.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_storage_interference_t loom_storage_interference_t;

// Analyzes the storage roots and accesses in |function|.
//
// |fact_table| must contain the populated facts and retained CFG snapshots for
// |function|. |value_domain| must be acquired for the function body and remain
// active for the analysis lifetime. The returned analysis and all retained
// provenance, access, and control facts are allocated from |arena|.
//
// The analysis owns the function walk. Consumers query its indexed result and
// must not reconstruct aliases or access footprints from source IR.
iree_status_t loom_storage_interference_analyze_function(
    const loom_module_t* module, const loom_value_fact_table_t* fact_table,
    const loom_local_value_domain_t* value_domain, loom_func_like_t function,
    iree_arena_allocator_t* arena, loom_storage_interference_t** out_analysis);

// Returns true when |root_value_id| may have a memory footprint.
//
// Complete roots with no recorded accesses return false. Missing roots,
// incomplete provenance, and unknown memory effects conservatively return
// true. Storage layout consumers use this to omit provably unused allocations
// without repeating source traversal or demand analysis.
bool loom_storage_interference_root_may_be_accessed(
    const loom_storage_interference_t* analysis, loom_value_id_t root_value_id);

// Proves that two workgroup buffer.alloca roots never require their bytes at
// the same time. The proof accepts workgroup-uniform mutually exclusive
// control or a verified workgroup acq_rel barrier separating all synchronous
// and asynchronous access completions. Missing provenance, unknown memory
// effects, weaker barriers, and cyclic phase reversal fail the proof.
iree_status_t loom_storage_interference_prove_workgroup_nonoverlap(
    loom_storage_interference_t* analysis, loom_value_id_t lhs_root_value_id,
    loom_value_id_t rhs_root_value_id, bool* out_proven);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ANALYSIS_STORAGE_INTERFERENCE_H_
