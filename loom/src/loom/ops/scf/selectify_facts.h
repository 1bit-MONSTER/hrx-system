// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_OPS_SCF_SELECTIFY_FACTS_H_
#define LOOM_OPS_SCF_SELECTIFY_FACTS_H_

#include "loom/ir/ir.h"
#include "loom/rewrite/rewriter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns true when |op| is a removable branch-local fact identity.
bool loom_scf_if_op_is_strippable_fact_identity(const loom_module_t* module,
                                                const loom_op_t* op,
                                                loom_trait_flags_t traits);

// Proves that removing branch-local fact identities while speculating the
// bounded arm bodies preserves every fact retained on |op|'s results.
iree_status_t loom_scf_if_stripped_facts_preserve_results(
    loom_op_t* op, loom_rewriter_t* rewriter, const loom_block_t* then_block,
    const loom_op_t* then_yield, const loom_block_t* else_block,
    const loom_op_t* else_yield, bool* out_preserves_facts);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_SCF_SELECTIFY_FACTS_H_
