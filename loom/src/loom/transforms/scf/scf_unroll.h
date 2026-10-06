// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// This pass consumes local scf.for unroll intent. Bare `unroll` requires a
// compile-time full trip count and clones the body inline. `unroll(%factor)`
// requires a compile-time factor and positive static step, then stripmines the
// loop so code growth is bounded by the requested factor even when bounds are
// dynamic or specialization-time facts are available.
//
// The pass is intentionally separate from scf-to-cfg so structured loop intent
// can survive until passes such as private-fragment promotion have used it, and
// then be removed before target lowering would otherwise materialize dynamic
// loop control.

#ifndef LOOM_TRANSFORMS_SCF_UNROLL_H_
#define LOOM_TRANSFORMS_SCF_UNROLL_H_

#include "iree/base/api.h"
#include "loom/pass/types.h"
#include "loom/rewrite/remap.h"
#include "loom/rewrite/rewriter.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

// Scalar domain retained independently of the source analysis and SSA identity.
// Emission binds a dynamic lower value from the current loop operand.
typedef struct loom_scf_unroll_full_plan_t {
  // Finite iteration count established by the original value analysis.
  uint32_t count;
  // Whether the current lower operand supplies the first induction value.
  bool dynamic_lower;
  // Exact first induction value for a static domain.
  int64_t lower;
  // Exact positive induction step.
  int64_t step;
} loom_scf_unroll_full_plan_t;

// Returns a full linear plan when the verified loop explicitly requests one.
// Partial, interleaved, pipelined or unresolved policies remain with their
// owning pass. This query consumes the original fact table before mutation.
bool loom_scf_unroll_plan_full(loom_pass_t* pass, loom_module_t* module,
                               loom_value_fact_table_t* facts, loom_op_t* op,
                               loom_scf_unroll_full_plan_t* out_plan);

// Materializes a selected full-linear plan using the ordinary unroller's body
// emitter. The caller owns |remap| and may supply selected operation projection
// entries in iteration/clone visitation order. The source loop is replaced;
// no value-fact table is consulted or updated during materialization.
iree_status_t loom_scf_unroll_emit_full(loom_pass_t* pass,
                                        loom_module_t* module,
                                        loom_rewriter_t* rewriter,
                                        loom_op_t* op,
                                        const loom_scf_unroll_full_plan_t* plan,
                                        loom_ir_remap_t* remap);

const loom_pass_info_t* loom_scf_unroll_pass_info(void);

iree_status_t loom_scf_unroll_create(loom_pass_t* pass,
                                     iree_string_view_t options_string);

iree_status_t loom_scf_unroll_run(loom_pass_t* pass, loom_module_t* module,
                                  loom_func_like_t function);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_SCF_UNROLL_H_
